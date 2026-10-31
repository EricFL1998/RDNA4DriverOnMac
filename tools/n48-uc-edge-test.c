// Edge-case and concurrency tests for the RDNA4Driver user client.
//
//   boundary: zero-size alloc, oversized alloc, unaligned submit, oversized
//             submit, bogus free, double free, alloc-after-free reuse
//   concurrent: two processes submitting at the same time must each get their
//             own fence and their own data (no cross-talk through the ring)
//   stress: 100 alloc/write/submit/wait/free cycles in the 32 MiB window
//
// clang -arch x86_64 -o n48_uc_edge tools/n48-uc-edge-test.c \
//       -framework IOKit -framework CoreFoundation

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <IOKit/IOKitLib.h>

enum {
    kSelGetDeviceInfo = 0,
    kSelAllocateBuffer = 1,
    kSelFreeBuffer = 2,
    kSelSubmitCommands = 3,
    kSelWaitFence = 4,
    kMemTypeWindow = 0,
};

static int failures;

static void report(const char* what, int ok, const char* detail) {
    printf("  %s %-46s %s\n", ok ? "ok  " : "FAIL", what, detail ? detail : "");
    if (!ok) failures++;
}

static io_connect_t openDriver(void) {
    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("RDNA4Driver"));
    if (!service) return 0;
    io_connect_t conn = 0;
    if (IOServiceOpen(service, mach_task_self(), 0, &conn) != KERN_SUCCESS) conn = 0;
    IOObjectRelease(service);
    return conn;
}

static kern_return_t getInfo(io_connect_t conn, uint64_t* windowGpu, uint64_t* windowSize) {
    uint64_t info[4] = {0};
    uint32_t count = 4;
    kern_return_t kr = IOConnectCallMethod(conn, kSelGetDeviceInfo, NULL, 0, NULL, 0,
                                           info, &count, NULL, NULL);
    if (kr == KERN_SUCCESS) {
        *windowGpu = info[2];
        *windowSize = info[3];
    }
    return kr;
}

static kern_return_t allocate(io_connect_t conn, uint64_t size, uint64_t* addr) {
    uint64_t out = 0;
    uint32_t count = 1;
    kern_return_t kr = IOConnectCallMethod(conn, kSelAllocateBuffer, &size, 1, NULL, 0,
                                           &out, &count, NULL, NULL);
    if (kr == KERN_SUCCESS) *addr = out;
    return kr;
}

static kern_return_t release(io_connect_t conn, uint64_t addr) {
    return IOConnectCallMethod(conn, kSelFreeBuffer, &addr, 1, NULL, 0, NULL, NULL, NULL, NULL);
}

// WRITE_DATA that stores `value` at `gpuAddr`.
static void buildWriteData(uint32_t* pm4, uint64_t gpuAddr, uint32_t value) {
    pm4[0] = (3u << 30) | (3u << 16) | (0x37u << 8);   // PACKET3_WRITE_DATA, count 3
    pm4[1] = (1u << 30) | (5u << 8) | (1u << 20);      // ME, memory dst, WR_CONFIRM
    pm4[2] = (uint32_t)(gpuAddr & 0xFFFFFFFCu);
    pm4[3] = (uint32_t)(gpuAddr >> 32);
    pm4[4] = value;
}

static kern_return_t submit(io_connect_t conn, const void* data, size_t bytes, uint64_t* fence) {
    uint64_t out = 0;
    uint32_t count = 1;
    kern_return_t kr = IOConnectCallMethod(conn, kSelSubmitCommands, NULL, 0, data, bytes,
                                           &out, &count, NULL, NULL);
    if (kr == KERN_SUCCESS && fence) *fence = out;
    return kr;
}

static kern_return_t waitFence(io_connect_t conn, uint64_t fence, uint64_t timeoutMS) {
    uint64_t in[2] = { fence, timeoutMS };
    return IOConnectCallMethod(conn, kSelWaitFence, in, 2, NULL, 0, NULL, NULL, NULL, NULL);
}

// One write/submit/wait/verify round trip on an already-allocated buffer.
static int gpuRoundTrip(io_connect_t conn, volatile uint32_t* buf,
                        uint64_t gpuAddr, uint32_t value) {
    buf[0] = 0;
    uint32_t pm4[5];
    buildWriteData(pm4, gpuAddr, value);
    uint64_t fence = 0;
    if (submit(conn, pm4, sizeof pm4, &fence) != KERN_SUCCESS) return -1;
    if (waitFence(conn, fence, 5000) != KERN_SUCCESS) return -2;
    return buf[0] == value ? 0 : -3;
}

static void testBoundaries(io_connect_t conn, volatile uint8_t* mapped,
                           uint64_t windowGpu, uint64_t windowSize) {
    printf("\n[boundary]\n");
    uint64_t addr = 0;
    const char* detail;
    char buf[128];

    kern_return_t kr = allocate(conn, 0, &addr);
    report("zero-size alloc rejected", kr != KERN_SUCCESS, "allocator returned an address");

    kr = allocate(conn, windowSize + 4096, &addr);
    report("oversized alloc rejected", kr != KERN_SUCCESS, "window over-allocation accepted");

    kr = allocate(conn, 4096, &addr);
    snprintf(buf, sizeof buf, "kr=0x%x addr=0x%llx", kr, (unsigned long long)addr);
    if (kr != KERN_SUCCESS) { report("alloc 4096", 0, buf); return; }
    const uint64_t first = addr;
    volatile uint32_t* p = (volatile uint32_t*)(mapped + (first - windowGpu));
    p[0] = 0xFEEDFACEu;

    release(conn, first);
    uint64_t second = 0;
    kr = allocate(conn, 4096, &second);
    snprintf(buf, sizeof buf, "first=0x%llx second=0x%llx",
             (unsigned long long)first, (unsigned long long)second);
    report("alloc-after-free reuses the window", kr == KERN_SUCCESS && second == first, buf);

    const uint32_t tooBigDwords = 256u * 1024u + 1u;
    void* big = calloc(tooBigDwords, 4);
    kr = submit(conn, big, (size_t)tooBigDwords * 4, NULL);
    report("oversized submit rejected (1 MiB + 4 B)", kr != KERN_SUCCESS, "no size gate");
    free(big);

    kr = submit(conn, "123456", 6, NULL);
    report("unaligned submit rejected (6 B)", kr != KERN_SUCCESS, "no alignment gate");

    uint32_t valid[5];
    buildWriteData(valid, first, 0x11112222u);
    uint64_t fence = 0;
    kr = submit(conn, valid, sizeof valid, &fence);
    report("ring still accepts a valid packet", kr == KERN_SUCCESS, "submit path wedged");
    if (kr == KERN_SUCCESS) {
        (void)waitFence(conn, fence, 5000);
    }

    const uint64_t bogus = windowGpu + 0x30;
    kr = release(conn, bogus);
    report("free of an unallocated address is logged, not fatal",
           kr == KERN_SUCCESS || kr == kIOReturnBadArgument, "driver still alive below");

    kr = release(conn, second);
    kr = release(conn, second);
    report("double free is survivable", kr == KERN_SUCCESS || kr == kIOReturnBadArgument,
           "driver still alive below");

    uint64_t third = 0;
    kr = allocate(conn, 4096, &third);
    if (kr == KERN_SUCCESS) {
        volatile uint32_t* q = (volatile uint32_t*)(mapped + (third - windowGpu));
        const int rc = gpuRoundTrip(conn, q, third, 0xBEEF0001u);
        snprintf(buf, sizeof buf, "rc=%d value=0x%08x", rc, q[0]);
        report("GPU still works after the error cases", rc == 0, buf);
        release(conn, third);
    } else {
        report("GPU still works after the error cases", 0, "realloc failed");
    }

    kr = release(conn, windowGpu - 4096);
    report("free below the window rejected", kr == kIOReturnBadArgument, "no kernel fault");
    (void)detail;
}

static int concurrencyChild(int index, int iterations) {
    io_connect_t conn = openDriver();
    if (!conn) return 10;
    uint64_t windowGpu = 0, windowSize = 0;
    if (getInfo(conn, &windowGpu, &windowSize) != KERN_SUCCESS) return 11;

    mach_vm_address_t mapped = 0;
    mach_vm_size_t mappedSize = 0;
    if (IOConnectMapMemory64(conn, kMemTypeWindow, mach_task_self(), &mapped, &mappedSize,
                             kIOMapAnywhere) != KERN_SUCCESS) return 12;

    uint64_t addr = 0;
    if (allocate(conn, 4096, &addr) != KERN_SUCCESS) return 13;
    volatile uint32_t* buf = (volatile uint32_t*)((uint8_t*)mapped + (addr - windowGpu));

    for (int i = 0; i < iterations; i++) {
        const uint32_t want = 0xC0DE0000u | ((uint32_t)index << 12) | (uint32_t)i;
        const int rc = gpuRoundTrip(conn, buf, addr, want);
        if (rc != 0) {
            printf("  child %d iteration %d failed rc=%d got=0x%08x want=0x%08x\n",
                   index, i, rc, buf[0], want);
            return 20 + rc;
        }
    }
    release(conn, addr);
    IOServiceClose(conn);
    return 0;
}

static void testConcurrency(int iterations) {
    printf("\n[concurrent]\n");
    pid_t kids[2];
    for (int k = 0; k < 2; k++) {
        pid_t pid = fork();
        if (pid == 0) _exit(concurrencyChild(k + 1, iterations));
        kids[k] = pid;
    }
    for (int k = 0; k < 2; k++) {
        int status = 0;
        waitpid(kids[k], &status, 0);
        char what[64], buf[64];
        snprintf(what, sizeof what, "process %d: %d interleaved submits", k + 1, iterations);
        snprintf(buf, sizeof buf, "exit=%d", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        report(what, WIFEXITED(status) && WEXITSTATUS(status) == 0, buf);
    }
}

static void testStress(io_connect_t conn, volatile uint8_t* mapped,
                       uint64_t windowGpu, uint64_t windowSize, int cycles) {
    printf("\n[stress] %d alloc/write/submit/wait/free cycles in a %llu MiB window\n",
           cycles, (unsigned long long)(windowSize >> 20));
    int allocFailures = 0, gpuFailures = 0;
    uint64_t maxOffset = 0;
    for (int i = 0; i < cycles; i++) {
        uint64_t addr = 0;
        if (allocate(conn, 4096, &addr) != KERN_SUCCESS) { allocFailures++; continue; }
        if (addr - windowGpu > maxOffset) maxOffset = addr - windowGpu;
        volatile uint32_t* buf = (volatile uint32_t*)(mapped + (addr - windowGpu));
        if (gpuRoundTrip(conn, buf, addr, 0x5A5A0000u + (uint32_t)i) != 0) gpuFailures++;
        release(conn, addr);
    }
    char buf[160];
    snprintf(buf, sizeof buf, "%d alloc failures, highest offset 0x%llx",
             allocFailures, (unsigned long long)maxOffset);
    report("window does not run out", allocFailures == 0, buf);
    snprintf(buf, sizeof buf, "%d rounds failed", gpuFailures);
    report("every round produced its own GPU write", gpuFailures == 0, buf);
}

int main(int argc, char** argv) {
    const int cycles = argc > 1 ? atoi(argv[1]) : 100;
    printf("RDNA4 user client edge/concurrency test\n");

    io_connect_t conn = openDriver();
    if (!conn) { printf("FAIL: RDNA4Driver service not openable\n"); return 1; }

    uint64_t windowGpu = 0, windowSize = 0;
    if (getInfo(conn, &windowGpu, &windowSize) != KERN_SUCCESS || windowSize == 0) {
        printf("FAIL: GetDeviceInfo gave no window\n");
        return 1;
    }
    printf("window GPU 0x%llx size %llu MiB\n",
           (unsigned long long)windowGpu, (unsigned long long)(windowSize >> 20));

    mach_vm_address_t mapped = 0;
    mach_vm_size_t mappedSize = 0;
    if (IOConnectMapMemory64(conn, kMemTypeWindow, mach_task_self(), &mapped, &mappedSize,
                             kIOMapAnywhere) != KERN_SUCCESS) {
        printf("FAIL: cannot map the user window\n");
        return 1;
    }

    testBoundaries(conn, (volatile uint8_t*)mapped, windowGpu, windowSize);
    IOServiceClose(conn);
    testConcurrency(10);

    conn = openDriver();
    if (!conn) { printf("FAIL: reopen\n"); return 1; }
    // The mapping belongs to the connection that created it; a new connection
    // needs its own, otherwise the stress loop writes into a dead mapping.
    mach_vm_address_t mapped2 = 0;
    mach_vm_size_t mapped2Size = 0;
    if (IOConnectMapMemory64(conn, kMemTypeWindow, mach_task_self(), &mapped2, &mapped2Size,
                             kIOMapAnywhere) != KERN_SUCCESS) {
        printf("FAIL: cannot remap the user window\n");
        return 1;
    }
    printf("remapped window at %p (%llu MiB)\n", (void*)mapped2,
           (unsigned long long)(mapped2Size >> 20));
    testStress(conn, (volatile uint8_t*)mapped2, windowGpu, windowSize, cycles);
    IOServiceClose(conn);

    printf("\nRESULT: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
