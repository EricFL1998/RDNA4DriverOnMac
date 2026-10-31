#include "RDNA4DriverUserClient.hpp"
#include "RDNA4Driver.hpp"
#include <IOKit/IOLib.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(RDNA4DriverUserClient, IOUserClient)

namespace {
// The largest PM4 stream a single submission may carry. The ring is 1 MiB and
// the available space is re-derived per submission, so this is a sanity bound
// rather than an allocator limit.
constexpr uint32_t kMaxSubmitDwords = 256u * 1024u;
class ClientLockGuard {
    IOLock* lock;
public:
    explicit ClientLockGuard(IOLock* value) : lock(value) {
        if (lock) IOLockLock(lock);
    }
    ~ClientLockGuard() { if (lock) IOLockUnlock(lock); }
};
} // namespace

bool RDNA4DriverUserClient::start(IOService* provider) {
    if (!super::start(provider)) return false;

    driver = OSDynamicCast(RDNA4Driver, provider);
    if (!driver) {
        IOLog("RDNA4UserClient: provider is not RDNA4Driver — refusing\n");
        super::stop(provider);
        return false;
    }

    clientLock = IOLockAlloc();
    if (!clientLock) {
        driver = nullptr;
        super::stop(provider);
        return false;
    }

    IOLog("RDNA4UserClient: started (user window GPU 0x%llx, %llu MiB)\n",
          static_cast<unsigned long long>(driver->userHeapGpuAddr()),
          static_cast<unsigned long long>(driver->userHeapSize() >> 20));
    return true;
}

void RDNA4DriverUserClient::stop(IOService* provider) {
    {
        ClientLockGuard guard(clientLock);
        releaseAllocations();
        driver = nullptr;
    }
    super::stop(provider);
}

void RDNA4DriverUserClient::free() {
    if (clientLock) {
        IOLockFree(clientLock);
        clientLock = nullptr;
    }
    super::free();
}

void RDNA4DriverUserClient::releaseAllocations() {
    closing = true;
    if (!driver || allocationCount == 0) return;

    // The process can die immediately after submission. Its ranges must not
    // be handed to the next process while either queue can still access them.
    const bool computeDone = !lastComputeFence ||
        driver->waitForComputeFence(lastComputeFence, 5000);
    const bool graphicsDone = !lastGraphicsFence ||
        driver->waitForGraphicsFence(lastGraphicsFence, 5000);
    if (!computeDone || !graphicsDone) {
        IOLog("RDNA4UserClient: retaining %u allocations: GPU completion unconfirmed\n",
              allocationCount);
        return;
    }

    const uint32_t count = allocationCount;
    while (allocationCount) driver->freeVRAMBuffer(allocations[--allocationCount]);
    lastComputeFence = lastGraphicsFence = 0;
    IOLog("RDNA4UserClient: reclaimed %u allocations on close\n", count);
}

IOReturn RDNA4DriverUserClient::clientClose() {
    {
        ClientLockGuard guard(clientLock);
        releaseAllocations();
    }
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

IOReturn RDNA4DriverUserClient::clientDied() {
    return super::clientDied();
}

IOReturn RDNA4DriverUserClient::clientMemoryForType(UInt32 type,
                                                    IOOptionBits* options,
                                                    IOMemoryDescriptor** memory) {
    ClientLockGuard guard(clientLock);
    if (!memory) return kIOReturnBadArgument;
    *memory = nullptr;
    if (options) *options = 0;

    if (type != 0) return kIOReturnUnsupported;
    if (!driver || closing) return kIOReturnNotAttached;

    // A fresh descriptor per mapping: IOUserClient owns what we hand back and
    // releases it when the client goes away.
    IOMemoryDescriptor* window = driver->copyUserHeapDescriptor();
    if (!window) {
        IOLog("RDNA4UserClient: user window is not available\n");
        return kIOReturnNotReady;
    }
    *memory = window;
    return kIOReturnSuccess;
}

const IOExternalMethodDispatch RDNA4DriverUserClient::sMethods[9] = {
    { &RDNA4DriverUserClient::sGetDeviceInfo,    0, 0, 4, 0 },
    { &RDNA4DriverUserClient::sAllocateBuffer,   1, 0, 1, 0 },
    { &RDNA4DriverUserClient::sFreeBuffer,       1, 0, 0, 0 },
    { &RDNA4DriverUserClient::sSubmitCommands,   0, kIOUCVariableStructureSize, 1, 0 },
    { &RDNA4DriverUserClient::sWaitFence,        2, 0, 0, 0 },
    { &RDNA4DriverUserClient::sSubmitGraphics,   0, kIOUCVariableStructureSize, 1, 0 },
    { &RDNA4DriverUserClient::sWaitGraphicsFence, 2, 0, 0, 0 },
    { &RDNA4DriverUserClient::sGetGraphicsInfo,  0, 0, 4, 0 },
    { &RDNA4DriverUserClient::sGetNggRing,       0, 0, 2, 0 },
};

IOReturn RDNA4DriverUserClient::externalMethod(uint32_t selector,
                                               IOExternalMethodArguments* arguments,
                                               IOExternalMethodDispatch* dispatch,
                                               OSObject* target, void* reference) {
    (void)dispatch;
    (void)target;
    (void)reference;

    ClientLockGuard guard(clientLock);
    if (!driver || closing) return kIOReturnNotAttached;

    if (!arguments || selector >= (sizeof(sMethods) / sizeof(sMethods[0])) ||
        !sMethods[selector].function) {
        return kIOReturnUnsupported;
    }

    return super::externalMethod(selector, arguments,
                                 const_cast<IOExternalMethodDispatch*>(&sMethods[selector]),
                                 this, nullptr);
}

IOReturn RDNA4DriverUserClient::sGetDeviceInfo(OSObject* target, void* reference,
                                               IOExternalMethodArguments* arguments) {
    (void)reference;
    RDNA4DriverUserClient* self = OSDynamicCast(RDNA4DriverUserClient, target);
    if (!self || !self->driver) return kIOReturnNotAttached;
    if (!arguments || arguments->scalarOutputCount < 4 || !arguments->scalarOutput) {
        return kIOReturnBadArgument;
    }

    arguments->scalarOutput[0] = 0x1002;
    arguments->scalarOutput[1] = 0x7550;  // Navi 48
    arguments->scalarOutput[2] = self->driver->userHeapGpuAddr();
    arguments->scalarOutput[3] = self->driver->userHeapSize();
    return kIOReturnSuccess;
}

IOReturn RDNA4DriverUserClient::sAllocateBuffer(OSObject* target, void* reference,
                                                IOExternalMethodArguments* arguments) {
    (void)reference;
    RDNA4DriverUserClient* self = OSDynamicCast(RDNA4DriverUserClient, target);
    if (!self || !self->driver) return kIOReturnNotAttached;
    if (!arguments || arguments->scalarInputCount < 1 || !arguments->scalarInput ||
        arguments->scalarOutputCount < 1 || !arguments->scalarOutput) {
        return kIOReturnBadArgument;
    }

    const uint64_t size = arguments->scalarInput[0];
    if (size == 0 || size > self->driver->userHeapSize()) {
        return kIOReturnBadArgument;
    }
    if (self->allocationCount == kMaxAllocations) return kIOReturnNoMemory;

    const uint64_t addr = self->driver->allocateUserVRAMBuffer(
        static_cast<size_t>(size));
    if (!addr) return kIOReturnNoMemory;

    self->allocations[self->allocationCount++] = addr;
    arguments->scalarOutput[0] = addr;
    return kIOReturnSuccess;
}

IOReturn RDNA4DriverUserClient::sFreeBuffer(OSObject* target, void* reference,
                                            IOExternalMethodArguments* arguments) {
    (void)reference;
    RDNA4DriverUserClient* self = OSDynamicCast(RDNA4DriverUserClient, target);
    if (!self || !self->driver) return kIOReturnNotAttached;
    if (!arguments || arguments->scalarInputCount < 1 || !arguments->scalarInput) {
        return kIOReturnBadArgument;
    }

    // Only the user window is releasable from here: the live-block table also
    // tracks kernel allocations (ring, MQD, MES contexts), and letting a caller
    // free one of those by address would pull memory out from under the driver.
    const uint64_t addr = arguments->scalarInput[0];
    const uint64_t windowGpu = self->driver->userHeapGpuAddr();
    const uint64_t windowSize = self->driver->userHeapSize();
    if (windowSize == 0 || addr < windowGpu || addr - windowGpu >= windowSize) {
        return kIOReturnBadArgument;
    }

    for (uint32_t i = 0; i < self->allocationCount; ++i) {
        if (self->allocations[i] != addr) continue;
        self->driver->freeVRAMBuffer(addr);
        self->allocations[i] = self->allocations[--self->allocationCount];
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}

IOReturn RDNA4DriverUserClient::sSubmitCommands(OSObject* target, void* reference,
                                                IOExternalMethodArguments* arguments) {
    (void)reference;
    RDNA4DriverUserClient* self = OSDynamicCast(RDNA4DriverUserClient, target);
    if (!self || !self->driver) return kIOReturnNotAttached;
    if (!arguments || !arguments->structureInput || arguments->structureInputSize == 0 ||
        !arguments->scalarOutput || arguments->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    const size_t bytes = arguments->structureInputSize;
    if ((bytes & 3u) || (bytes / 4) > kMaxSubmitDwords) {
        return kIOReturnBadArgument;
    }
    const auto* pm4 = static_cast<const uint32_t*>(arguments->structureInput);
    const size_t dwords = bytes / 4;

    uint64_t fence = 0;
    if (!self->driver->submitUserComputeCommands(pm4, dwords, &fence)) {
        return kIOReturnNoResources;
    }

    self->lastComputeFence = fence;
    arguments->scalarOutput[0] = fence;
    return kIOReturnSuccess;
}

IOReturn RDNA4DriverUserClient::sWaitFence(OSObject* target, void* reference,
                                           IOExternalMethodArguments* arguments) {
    (void)reference;
    RDNA4DriverUserClient* self = OSDynamicCast(RDNA4DriverUserClient, target);
    if (!self || !self->driver) return kIOReturnNotAttached;
    if (!arguments || arguments->scalarInputCount < 2 || !arguments->scalarInput) {
        return kIOReturnBadArgument;
    }

    const uint64_t fence = arguments->scalarInput[0];
    uint64_t timeoutMS = arguments->scalarInput[1];
    // Callers that pass a nanosecond timeout (the Metal bundle used to) would
    // otherwise mean "wait ~57 days"; clamp to a sane ceiling.
    if (timeoutMS == 0 || timeoutMS > 60000) timeoutMS = 60000;

    if (!self->driver->waitForComputeFence(fence, timeoutMS)) return kIOReturnTimeout;
    if (fence >= self->lastComputeFence) self->lastComputeFence = 0;
    return kIOReturnSuccess;
}

IOReturn RDNA4DriverUserClient::sSubmitGraphics(OSObject* target, void* reference,
                                                IOExternalMethodArguments* arguments) {
    (void)reference;
    RDNA4DriverUserClient* self = OSDynamicCast(RDNA4DriverUserClient, target);
    if (!self || !self->driver) return kIOReturnNotAttached;
    if (!arguments || !arguments->structureInput || arguments->structureInputSize == 0 ||
        !arguments->scalarOutput || arguments->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }
    if (!self->driver->graphicsQueueReady()) return kIOReturnNotReady;

    const size_t bytes = arguments->structureInputSize;
    if ((bytes & 3u) || (bytes / 4) > kMaxSubmitDwords) return kIOReturnBadArgument;

    const auto* pm4 = static_cast<const uint32_t*>(arguments->structureInput);
    const size_t dwords = bytes / 4;

    uint64_t fence = 0;
    if (!self->driver->submitGraphicsCommands(pm4, dwords, &fence)) {
        return kIOReturnNoResources;
    }

    self->lastGraphicsFence = fence;
    arguments->scalarOutput[0] = fence;
    return kIOReturnSuccess;
}

IOReturn RDNA4DriverUserClient::sWaitGraphicsFence(OSObject* target, void* reference,
                                                   IOExternalMethodArguments* arguments) {
    (void)reference;
    RDNA4DriverUserClient* self = OSDynamicCast(RDNA4DriverUserClient, target);
    if (!self || !self->driver) return kIOReturnNotAttached;
    if (!arguments || arguments->scalarInputCount < 2 || !arguments->scalarInput) {
        return kIOReturnBadArgument;
    }
    uint64_t timeoutMS = arguments->scalarInput[1];
    if (timeoutMS == 0 || timeoutMS > 60000) timeoutMS = 60000;
    const uint64_t fence = arguments->scalarInput[0];
    if (!self->driver->waitForGraphicsFence(fence, timeoutMS)) return kIOReturnTimeout;
    if (fence >= self->lastGraphicsFence) self->lastGraphicsFence = 0;
    return kIOReturnSuccess;
}

IOReturn RDNA4DriverUserClient::sGetGraphicsInfo(OSObject* target, void* reference,
                                                 IOExternalMethodArguments* arguments) {
    (void)reference;
    RDNA4DriverUserClient* self = OSDynamicCast(RDNA4DriverUserClient, target);
    if (!self || !self->driver) return kIOReturnNotAttached;
    if (!arguments || arguments->scalarOutputCount < 4 || !arguments->scalarOutput) {
        return kIOReturnBadArgument;
    }
    arguments->scalarOutput[0] = self->driver->graphicsQueueReady() ? 1 : 0;
    arguments->scalarOutput[1] = self->driver->graphicsFenceGpuAddr();
    arguments->scalarOutput[2] = 0;
    arguments->scalarOutput[3] = 0;
    return kIOReturnSuccess;
}

IOReturn RDNA4DriverUserClient::sGetNggRing(OSObject* target, void* reference,
                                            IOExternalMethodArguments* arguments) {
    (void)reference;
    RDNA4DriverUserClient* self = OSDynamicCast(RDNA4DriverUserClient, target);
    if (!self || !self->driver) return kIOReturnNotAttached;
    if (!arguments || arguments->scalarOutputCount < 2 || !arguments->scalarOutput) {
        return kIOReturnBadArgument;
    }
    arguments->scalarOutput[0] = self->driver->nggRingBase();
    arguments->scalarOutput[1] = self->driver->nggRingBytes();
    return kIOReturnSuccess;
}
