// Exercise the production UserClient. Only IOKit and the hardware provider
// are substituted: the connection ownership and close/death paths are real.
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <map>
#include <stdio.h>

class RDNA4Driver : public IOService {
    std::map<uint64_t, uint64_t> blocks;
public:
    bool complete = true;
    bool freedBeforeFence = false;
    bool pending = false;
    unsigned frees = 0;
    uint64_t userHeapGpuAddr() const { return 0x100000000ull; }
    uint64_t userHeapSize() const { return 32u * 1024u * 1024u; }
    uint64_t allocateUserVRAMBuffer(size_t size) {
        size = (size + 4095u) & ~size_t(4095u);
        uint64_t next = userHeapGpuAddr();
        for (const auto& block : blocks) {
            if (size <= block.first - next) break;
            next = block.first + block.second;
        }
        if (size > userHeapGpuAddr() + userHeapSize() - next) return 0;
        blocks[next] = size;
        return next;
    }
    void freeVRAMBuffer(uint64_t address) {
        if (pending) freedBeforeFence = true;
        if (blocks.erase(address)) ++frees;
    }
    size_t liveCount() const { return blocks.size(); }
    IOMemoryDescriptor* copyUserHeapDescriptor() { return nullptr; }
    uint64_t allocFenceValue() { return 1; }
    uint64_t computeFenceGpuAddr() { return 0x200000000ull; }
    bool submitComputeCommands(const uint32_t*, size_t) { pending = true; return true; }
    bool submitUserComputeCommands(const uint32_t*, size_t, uint64_t* fence) {
        pending = true; *fence = 1; return true;
    }
    bool waitForComputeFence(uint64_t, uint64_t) { pending = !complete; return complete; }
    bool graphicsQueueReady() { return true; }
    uint64_t graphicsFenceGpuAddr() { return 0x200001000ull; }
    uint64_t nggRingBase() const { return 0x200002000ull; }
    uint32_t nggRingBytes() const { return 0xA80000u; }
    bool submitGraphicsCommands(const uint32_t*, size_t, uint64_t* fence) {
        pending = true; *fence = 1; return true;
    }
    bool waitForGraphicsFence(uint64_t, uint64_t) { pending = !complete; return complete; }
};

// Supply the hardware provider above while compiling the actual UserClient.
#define RDNA4Driver_hpp
#include "../src/core/RDNA4DriverUserClient.cpp"
#undef super

class Client : public RDNA4DriverUserClient {
public:
    ~Client() override { free(); }
    IOReturn call(uint32_t selector, uint64_t input, uint64_t* output = nullptr) {
        IOExternalMethodArguments arguments;
        arguments.scalarInput = &input;
        arguments.scalarInputCount = 1;
        arguments.scalarOutput = output;
        arguments.scalarOutputCount = output ? 1 : 0;
        return externalMethod(selector, &arguments, nullptr, nullptr, nullptr);
    }
    IOReturn submit(uint32_t selector) {
        const uint32_t nop = 0xffff1000u;
        uint64_t fence = 0;
        IOExternalMethodArguments arguments;
        arguments.structureInput = &nop;
        arguments.structureInputSize = sizeof(nop);
        arguments.scalarOutput = &fence;
        arguments.scalarOutputCount = 1;
        return externalMethod(selector, &arguments, nullptr, nullptr, nullptr);
    }
};

static int failures = 0;
static void check(bool ok, const char* description) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", description); ++failures; }
}

static void testLifecycle(bool death) {
    RDNA4Driver driver;
    for (int i = 0; i < 8; ++i) {
        Client client;
        check(client.start(&driver), "start client");
        uint64_t address = 0;
        check(client.call(1, 11075583, &address) == kIOReturnSuccess,
              "repeated ring allocation fits after previous connection ended");
        if (death) client.clientDied(); else client.clientClose();
        client.stop(&driver);
        check(driver.liveCount() == 0, "closing/dying reclaims outstanding allocations");
        check(client.call(1, 4096, &address) != kIOReturnSuccess,
              "closed client cannot acquire new allocations");
    }
}

static void testOwnership() {
    RDNA4Driver driver;
    Client owner, other;
    owner.start(&driver); other.start(&driver);
    uint64_t address = 0;
    check(owner.call(1, 4096, &address) == kIOReturnSuccess, "owner allocates");
    check(other.call(2, address) == kIOReturnBadArgument, "foreign free rejected");
    other.clientClose(); other.stop(&driver);
    check(driver.liveCount() == 1, "other client's close preserves owner's allocation");
    check(owner.call(2, address) == kIOReturnSuccess, "owner frees its buffer");
    owner.clientClose(); owner.stop(&driver);
    check(driver.frees == 1, "explicit free is not repeated at close/stop");
}

static void testPendingWork(uint32_t selector, bool completes) {
    RDNA4Driver driver;
    driver.complete = completes;
    Client client;
    client.start(&driver);
    uint64_t address = 0;
    check(client.call(1, 4096, &address) == kIOReturnSuccess, "allocate before submission");
    check(client.submit(selector) == kIOReturnSuccess, "submit before close");
    client.clientClose(); client.stop(&driver);
    check(!driver.freedBeforeFence, "close never recycles memory ahead of outstanding GPU work");
    check(driver.liveCount() == (completes ? 0u : 1u),
          "close reclaims completed work and retains unconfirmed work");
}

int main() {
    testLifecycle(false);
    testLifecycle(true);
    testOwnership();
    for (uint32_t selector : {3u, 5u}) {
        testPendingWork(selector, true);
        testPendingWork(selector, false);
    }
    printf("UserClient lifecycle: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
