#include <IOKit/IOLib.h>
#include "../src/hardware/VramLayout.hpp"
#include "../src/hardware/PM4BuilderKernel.hpp"
#include <stdio.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <vector>

// The hardware/memory table fixture is deliberately tiny. The included
// definitions below are taken directly from the production source by the runner.
#define LOG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
class RDNA4Driver {
public:
    struct { uint64_t vramStart = 0x8000000000ull; } gmcContext;
    void* computeRing = this;
    void* vram = this;
    uint64_t vramSize = 256ull * 1024ull * 1024ull;
    uint64_t computeHeapNext = VramLayout::kComputeHeap;
    uint64_t userHeapNext = 0;
    static const uint32_t kMaxVramBlocks = 64;
    struct VramBlock { uint64_t gpuAddr = 0, size = 0; };
    VramBlock vramLiveBlocks[kMaxVramBlocks], vramFreeBlocks[kMaxVramBlocks];
    uint32_t vramLiveBlockCount = 0, vramFreeBlockCount = 0;
    IOLock* vramLock = IOLockAlloc();
    IOLock* submitLock = IOLockAlloc();
    ~RDNA4Driver() { IOLockFree(vramLock); IOLockFree(submitLock); }
    uint64_t allocateVRAMBuffer(size_t);
    uint64_t userHeapGpuAddr() const;
    uint64_t userHeapSize() const;
    uint64_t allocateUserVRAMBuffer(size_t);
    void freeVRAMBuffer(uint64_t);
    bool vramFreeInsert(uint64_t, uint64_t);
    uint64_t vramFreeTake(uint64_t, bool);
    void vramBlocksReset();
    std::atomic<uint64_t> nextFence { 0 };
    std::mutex reservationMutex;
    std::condition_variable reservationChanged;
    bool releaseFirst = false, firstReserved = false, secondReserved = false;
    std::vector<uint64_t> submittedFences;
    uint64_t allocFenceValue() {
        uint64_t fence = ++nextFence;
        std::unique_lock<std::mutex> lock(reservationMutex);
        if (fence == 1) {
            firstReserved = true;
            reservationChanged.notify_all();
            reservationChanged.wait(lock, [&] { return releaseFirst; });
        } else {
            secondReserved = true;
            reservationChanged.notify_all();
        }
        return fence;
    }
    uint64_t computeFenceGpuAddr() { return 0x800c100000ull; }
    bool submitUserComputeCommands(const uint32_t*, size_t, uint64_t*);
    bool submitComputeCommandsLocked(const uint32_t* stream, size_t dwords) {
        // RELEASE_MEM's final three words are value low, value high, interrupt.
        submittedFences.push_back(uint64_t(stream[dwords - 3]) |
                                  (uint64_t(stream[dwords - 2]) << 32));
        return true;
    }
};
#include "vram-allocation-methods.inc"

static unsigned failures;
static void check(bool condition, const char* message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

static void testFullTable(bool kernel) {
    RDNA4Driver driver;
    for (unsigned i = 0; i < 64; ++i) {
        check((kernel ? driver.allocateVRAMBuffer(4096) :
                        driver.allocateUserVRAMBuffer(4096)) != 0, "fill live table");
    }
    const uint64_t before = kernel ? driver.computeHeapNext : driver.userHeapNext;
    check((kernel ? driver.allocateVRAMBuffer(4096) :
                    driver.allocateUserVRAMBuffer(4096)) == 0,
          "full tracking table rejects allocation");
    check((kernel ? driver.computeHeapNext : driver.userHeapNext) == before,
          "rejected allocation consumes no heap space");
}

static void testFullTableWithFreeTail() {
    RDNA4Driver driver;
    const uint64_t first = driver.allocateUserVRAMBuffer(8192);
    for (unsigned i = 1; i < 64; ++i) driver.allocateUserVRAMBuffer(4096);
    driver.freeVRAMBuffer(first);
    check(driver.allocateUserVRAMBuffer(4096) == first, "released block can be split");
    check(driver.allocateUserVRAMBuffer(4096) == 0, "full table also rejects free-list allocation");
    driver.freeVRAMBuffer(first);
    check(driver.allocateUserVRAMBuffer(8192) == first, "failed allocation preserves reusable tail");
}

static void testRingReuse() {
    RDNA4Driver driver;
    for (unsigned i = 0; i < 100; ++i) {
        const uint64_t vs = driver.allocateUserVRAMBuffer(212);
        const uint64_t ps = driver.allocateUserVRAMBuffer(104);
        const uint64_t ring = driver.allocateUserVRAMBuffer(11075583);
        const uint64_t target = driver.allocateUserVRAMBuffer(800 * 600 * 4);
        check(vs && ps && ring && target, "shader/ring/target fit after reuse");
        driver.freeVRAMBuffer(ring);
        driver.freeVRAMBuffer(vs);
        driver.freeVRAMBuffer(target);
        driver.freeVRAMBuffer(ps);
    }
    check(driver.vramLiveBlockCount == 0, "all repeated allocations remain releasable");
    check(driver.allocateUserVRAMBuffer(0) == 0, "zero size rejected");
    check(driver.allocateUserVRAMBuffer(SIZE_MAX) == 0, "alignment overflow rejected");
}

#ifdef TEST_FENCED_SUBMIT
static void testFenceSubmissionOrder() {
    RDNA4Driver driver;
    const uint32_t nop = 0xffff1000u;
    uint64_t firstFence = 0, secondFence = 0;
    bool firstOK = false, secondOK = false;
    std::thread first([&] {
        firstOK = driver.submitUserComputeCommands(&nop, 1, &firstFence);
    });
    {
        std::unique_lock<std::mutex> lock(driver.reservationMutex);
        driver.reservationChanged.wait(lock, [&] { return driver.firstReserved; });
    }
    std::thread second([&] {
        secondOK = driver.submitUserComputeCommands(&nop, 1, &secondFence);
    });
    {
        std::unique_lock<std::mutex> lock(driver.reservationMutex);
        const bool overtook = driver.reservationChanged.wait_for(
            lock, std::chrono::milliseconds(100), [&] { return driver.secondReserved; });
        check(!overtook, "second client cannot reserve a fence ahead of first submission");
        driver.releaseFirst = true;
        driver.reservationChanged.notify_all();
    }
    first.join(); second.join();
    check(firstOK && secondOK && firstFence == 1 && secondFence == 2,
          "each compute submission returns its own fence");
    check(driver.submittedFences == std::vector<uint64_t>({1, 2}),
          "fence numbering follows actual queue order");
}
#endif

int main() {
    testFullTable(false);
    testFullTable(true);
    testFullTableWithFreeTail();
    testRingReuse();
#ifdef TEST_FENCED_SUBMIT
    testFenceSubmissionOrder();
#endif
    printf("VRAM allocator: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}
