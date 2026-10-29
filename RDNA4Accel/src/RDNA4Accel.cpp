#include "RDNA4Accel.hpp"
#include <IOKit/IOLib.h>

#define super IOAccelerator
OSDefineMetaClassAndStructors(RDNA4Accel, IOAccelerator)

//==============================================================================
// IOService Lifecycle
//==============================================================================

IOService* RDNA4Accel::probe(IOService* provider, SInt32* score) {
    IOLog("RDNA4Accel: probe() called\n");

    if (!provider) {
        return nullptr;
    }
    if (!super::probe(provider, score)) {
        return nullptr;
    }

    IOLog("RDNA4Accel: probe() succeeded\n");
    return this;
}

bool RDNA4Accel::start(IOService* provider) {
    IOLog("RDNA4Accel: start() ENTRY\n");

    if (!super::start(provider)) {
        IOLog("RDNA4Accel: super::start() failed\n");
        return false;
    }

    // The provider is the RDNA4Driver service.  Keep a borrowed pointer;
    // the provider relationship owns the driver for our lifetime.
    mainDriver = provider;

    if (!publishDeviceProperties()) {
        IOLog("RDNA4Accel: failed to publish device properties\n");
        mainDriver = nullptr;
        super::stop(provider);
        return false;
    }

    registerService();

    IOLog("RDNA4Accel: start() SUCCESS - accelerator stub registered\n");
    return true;
}

void RDNA4Accel::stop(IOService* provider) {
    IOLog("RDNA4Accel: stop() called\n");

    mainDriver = nullptr;
    super::stop(provider);
}

//==============================================================================
// Device Properties
//==============================================================================

bool RDNA4Accel::publishDeviceProperties() {
    // The hardware identifies as Navi 48 (RX 9070 / 9070 XT / 9070 GRE).
    setProperty("IOAcceleratorType", "GPU");
    setProperty("vendor-id", 0x1002, 32);
    setProperty("device-id", 0x7550, 32);
    setProperty("revision-id", 0xC0, 32);

    // IOPropertyMatch keys consumed by the RDNA4Metal.bundle Info.plist.
    setProperty("IOVendorID", 0x1002, 32);
    setProperty("IODeviceID", 0x7550, 32);

    setProperty("model", "AMD Radeon RX 9070 XT");
    setProperty("name", "RDNA4");

    // Prefer the live VRAM size published by RDNA4Driver; fall back to the
    // known 16320 MiB aperture if the property is not available.
    uint64_t vramSize = 16320ULL * 1024 * 1024;
    if (mainDriver) {
        OSNumber* vram = OSDynamicCast(OSNumber, mainDriver->copyProperty("GMC,VRAMSize"));
        if (vram) {
            vramSize = vram->unsigned64BitValue();
            vram->release();
        }
    }
    setProperty("vram-size", vramSize, 64);

    OSArray* capabilities = OSArray::withCapacity(3);
    if (capabilities) {
        OSString* capMetal = OSString::withCString("Metal");
        OSString* capCompute = OSString::withCString("Compute");
        OSString* capGraphics = OSString::withCString("Graphics");
        if (capMetal) capabilities->setObject(capMetal);
        if (capCompute) capabilities->setObject(capCompute);
        if (capGraphics) capabilities->setObject(capGraphics);
        setProperty("capabilities", capabilities);
        OSSafeReleaseNULL(capMetal);
        OSSafeReleaseNULL(capCompute);
        OSSafeReleaseNULL(capGraphics);
        OSSafeReleaseNULL(capabilities);
    }

    // Must match navi48-native/metal-bundle/Info.plist.
    setProperty("MetalPluginClassName", "RDNA4Device");
    setProperty("MetalPluginName", "RDNA4");
    setProperty("MetalPluginType", "Device");
    setProperty("MetalStatisticsName", "RDNA4");

    setProperty("GPUFamily", 2, 32);
    setProperty("MaxThreadsPerThreadgroup", 1024, 32);

    IOLog("RDNA4Accel: published device properties to IORegistry (VRAM %llu MB)\n",
          static_cast<unsigned long long>(vramSize >> 20));
    return true;
}

//==============================================================================
// IOUserClient Creation
//==============================================================================

IOReturn RDNA4Accel::newUserClient(task_t owningTask,
                                   void* securityID,
                                   UInt32 type,
                                   IOUserClient** handler) {
    IOLog("RDNA4Accel: newUserClient() called (type=%u)\n", type);

    if (!handler) {
        return kIOReturnBadArgument;
    }
    *handler = nullptr;

    RDNA4AccelUserClient* client = OSTypeAlloc(RDNA4AccelUserClient);
    if (!client) {
        IOLog("RDNA4Accel: failed to allocate user client\n");
        return kIOReturnNoMemory;
    }

    if (!client->initWithTask(owningTask, securityID, type)) {
        IOLog("RDNA4Accel: user client init failed\n");
        client->release();
        return kIOReturnError;
    }

    if (!client->attach(this)) {
        IOLog("RDNA4Accel: user client attach failed\n");
        client->release();
        return kIOReturnError;
    }

    if (!client->start(this)) {
        IOLog("RDNA4Accel: user client start failed\n");
        client->detach(this);
        client->release();
        return kIOReturnError;
    }

    *handler = client;
    IOLog("RDNA4Accel: user client created successfully\n");
    return kIOReturnSuccess;
}

//==============================================================================
// RDNA4AccelUserClient Implementation
//==============================================================================

#undef super
#define super IOUserClient
OSDefineMetaClassAndStructors(RDNA4AccelUserClient, IOUserClient)

bool RDNA4AccelUserClient::start(IOService* provider) {
    IOLog("RDNA4AccelUserClient: start() called\n");

    if (!super::start(provider)) {
        return false;
    }

    owner = OSDynamicCast(RDNA4Accel, provider);
    if (!owner) {
        IOLog("RDNA4AccelUserClient: provider is not RDNA4Accel\n");
        super::stop(provider);
        return false;
    }

    IOLog("RDNA4AccelUserClient: started successfully\n");
    return true;
}

void RDNA4AccelUserClient::stop(IOService* provider) {
    IOLog("RDNA4AccelUserClient: stop() called\n");

    owner = nullptr;
    super::stop(provider);
}

IOReturn RDNA4AccelUserClient::clientClose() {
    IOLog("RDNA4AccelUserClient: clientClose() called\n");

    if (!isInactive()) {
        terminate();
    }

    return kIOReturnSuccess;
}

IOReturn RDNA4AccelUserClient::clientDied() {
    IOLog("RDNA4AccelUserClient: clientDied() called\n");
    return clientClose();
}

//==============================================================================
// Method Dispatch
//==============================================================================

const IOExternalMethodDispatch RDNA4AccelUserClient::sMethods[16] = {
    // Selector 0: GetDeviceInfo
    { &RDNA4AccelUserClient::sGetDeviceInfo, 0, 0, 2, 0 },
    // Selector 1: AllocateBuffer
    { &RDNA4AccelUserClient::sAllocateBuffer, 1, 0, 1, 0 },
    // Selector 2: FreeBuffer
    { &RDNA4AccelUserClient::sFreeBuffer, 1, 0, 0, 0 },
    // Selector 3: SubmitCommands
    { &RDNA4AccelUserClient::sSubmitCommands, 0, kIOUCVariableStructureSize, 0, 0 },
    // Selector 4: WaitFence
    { &RDNA4AccelUserClient::sWaitFence, 2, 0, 0, 0 },
    // Reserved
    { nullptr, 0, 0, 0, 0 }, { nullptr, 0, 0, 0, 0 }, { nullptr, 0, 0, 0, 0 },
    { nullptr, 0, 0, 0, 0 }, { nullptr, 0, 0, 0, 0 }, { nullptr, 0, 0, 0, 0 },
    { nullptr, 0, 0, 0, 0 }, { nullptr, 0, 0, 0, 0 }, { nullptr, 0, 0, 0, 0 },
    { nullptr, 0, 0, 0, 0 }, { nullptr, 0, 0, 0, 0 }
};

IOReturn RDNA4AccelUserClient::externalMethod(uint32_t selector,
                                              IOExternalMethodArguments* arguments,
                                              IOExternalMethodDispatch* dispatch,
                                              OSObject* target,
                                              void* reference) {
    (void)dispatch;
    (void)target;
    (void)reference;

    if (!arguments || selector >= 16 || !sMethods[selector].function) {
        return kIOReturnUnsupported;
    }

    return super::externalMethod(selector, arguments,
                                 const_cast<IOExternalMethodDispatch*>(&sMethods[selector]),
                                 this, nullptr);
}

//==============================================================================
// Method Implementations (Phase 2 stubs)
//==============================================================================

IOReturn RDNA4AccelUserClient::sGetDeviceInfo(OSObject* target, void* reference,
                                              IOExternalMethodArguments* arguments) {
    (void)target;
    (void)reference;

    if (!arguments || arguments->scalarOutputCount < 2 || !arguments->scalarOutput) {
        return kIOReturnBadArgument;
    }

    arguments->scalarOutput[0] = 0x1002;  // AMD
    arguments->scalarOutput[1] = 0x7550;  // Navi 48
    IOLog("RDNA4AccelUserClient: GetDeviceInfo -> 0x1002:0x7550\n");
    return kIOReturnSuccess;
}

IOReturn RDNA4AccelUserClient::sAllocateBuffer(OSObject* target, void* reference,
                                               IOExternalMethodArguments* arguments) {
    (void)target;
    (void)reference;

    if (!arguments || arguments->scalarInputCount < 1 || !arguments->scalarInput ||
        arguments->scalarOutputCount < 1 || !arguments->scalarOutput) {
        return kIOReturnBadArgument;
    }

    const uint64_t size = arguments->scalarInput[0];
    // Phase 2 stub: no real VRAM allocation yet.
    arguments->scalarOutput[0] = 0xDEADBEEF;
    IOLog("RDNA4AccelUserClient: AllocateBuffer(size=%llu) -> stub\n",
          static_cast<unsigned long long>(size));
    return kIOReturnSuccess;
}

IOReturn RDNA4AccelUserClient::sFreeBuffer(OSObject* target, void* reference,
                                           IOExternalMethodArguments* arguments) {
    (void)target;
    (void)reference;

    if (!arguments || arguments->scalarInputCount < 1 || !arguments->scalarInput) {
        return kIOReturnBadArgument;
    }

    IOLog("RDNA4AccelUserClient: FreeBuffer(addr=0x%llx) -> stub\n",
          static_cast<unsigned long long>(arguments->scalarInput[0]));
    return kIOReturnSuccess;
}

IOReturn RDNA4AccelUserClient::sSubmitCommands(OSObject* target, void* reference,
                                               IOExternalMethodArguments* arguments) {
    (void)target;
    (void)reference;

    if (!arguments || !arguments->structureInput || arguments->structureInputSize == 0) {
        return kIOReturnBadArgument;
    }

    IOLog("RDNA4AccelUserClient: SubmitCommands(size=%u) -> stub\n",
          arguments->structureInputSize);
    return kIOReturnSuccess;
}

IOReturn RDNA4AccelUserClient::sWaitFence(OSObject* target, void* reference,
                                          IOExternalMethodArguments* arguments) {
    (void)target;
    (void)reference;

    if (!arguments || arguments->scalarInputCount < 2 || !arguments->scalarInput) {
        return kIOReturnBadArgument;
    }

    IOLog("RDNA4AccelUserClient: WaitFence(fence=%llu, timeout=%llu) -> stub\n",
          static_cast<unsigned long long>(arguments->scalarInput[0]),
          static_cast<unsigned long long>(arguments->scalarInput[1]));
    return kIOReturnSuccess;
}
