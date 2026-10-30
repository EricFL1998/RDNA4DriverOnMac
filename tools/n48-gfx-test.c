// Graphics (GFX) queue test: proves the MES-mapped GFX ring fetches and
// executes PM4, without involving any shader.
//
//   1. open RDNA4Driver, GetGraphicsInfo -> is the GFX queue mapped?
//   2. map the user window, allocate a buffer in it
//   3. submit a WRITE_DATA through selector 5 (graphics queue)
//   4. wait on the graphics fence (selector 6)
//   5. read the buffer back: the value must be the one the GPU wrote
//
// clang -arch x86_64 -o n48_gfx tools/n48-gfx-test.c \
//       -framework IOKit -framework CoreFoundation

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <IOKit/IOKitLib.h>

enum {
    kSelGetDeviceInfo = 0,
    kSelAllocateBuffer = 1,
    kSelFreeBuffer = 2,
    kSelSubmitCommands = 3,
    kSelWaitFence = 4,
    kSelSubmitGraphics = 5,
    kSelWaitGraphicsFence = 6,
    kSelGetGraphicsInfo = 7,
    kMemTypeWindow = 0,
};

static int failures;

static void check(const char* what, int ok, const char* detail) {
    printf("  %s %-42s %s\n", ok ? "ok  " : "FAIL", what, detail ? detail : "");
    if (!ok) failures++;
}

int main(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("RDNA4Driver"));
    if (!svc) { printf("FAIL: RDNA4Driver not found\n"); return 1; }

    io_connect_t conn = 0;
    if (IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
        printf("FAIL: IOServiceOpen\n");
        IOObjectRelease(svc);
        return 1;
    }
    IOObjectRelease(svc);

    uint64_t info[4] = {0, 0, 0, 0};
    uint32_t infoCount = 4;
    kern_return_t kr = IOConnectCallMethod(conn, kSelGetGraphicsInfo, NULL, 0, NULL, 0,
                                           info, &infoCount, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        printf("FAIL: GetGraphicsInfo kr=0x%x\n", kr);
        return 1;
    }
    char buf[128];
    snprintf(buf, sizeof buf, "fenceAddr=0x%llx", (unsigned long long)info[1]);
    check("GFX queue mapped by the MES", info[0] == 1, buf);
    if (info[0] != 1) {
        printf("\nRESULT: GFX queue is not up\n");
        return 1;
    }

    uint64_t dev[4] = {0, 0, 0, 0};
    infoCount = 4;
    if (IOConnectCallMethod(conn, kSelGetDeviceInfo, NULL, 0, NULL, 0,
                            dev, &infoCount, NULL, NULL) != KERN_SUCCESS) {
        printf("FAIL: GetDeviceInfo\n");
        return 1;
    }
    const uint64_t windowGpu = dev[2];

    mach_vm_address_t mapped = 0;
    mach_vm_size_t mappedSize = 0;
    if (IOConnectMapMemory64(conn, kMemTypeWindow, mach_task_self(), &mapped, &mappedSize,
                             kIOMapAnywhere) != KERN_SUCCESS) {
        printf("FAIL: cannot map the user window\n");
        return 1;
    }

    uint64_t size = 4096, gpuAddr = 0;
    uint32_t outCount = 1;
    if (IOConnectCallMethod(conn, kSelAllocateBuffer, &size, 1, NULL, 0,
                            &gpuAddr, &outCount, NULL, NULL) != KERN_SUCCESS) {
        printf("FAIL: AllocateBuffer\n");
        return 1;
    }
    volatile uint32_t* words =
        (volatile uint32_t*)((uint8_t*)mapped + (gpuAddr - windowGpu));
    words[0] = 0;
    words[1] = 0;

    uint32_t pm4[10];
    pm4[0] = (3u << 30) | (3u << 16) | (0x37u << 8);   // WRITE_DATA, 4 payload
    pm4[1] = (1u << 30) | (5u << 8) | (1u << 20);      // ME, memory, WR_CONFIRM
    pm4[2] = (uint32_t)(gpuAddr & 0xFFFFFFFCu);
    pm4[3] = (uint32_t)(gpuAddr >> 32);
    pm4[4] = 0x6F8F0001u;
    pm4[5] = (3u << 30) | (3u << 16) | (0x37u << 8);   // second word
    pm4[6] = (1u << 30) | (5u << 8) | (1u << 20);
    pm4[7] = (uint32_t)((gpuAddr + 4) & 0xFFFFFFFCu);
    pm4[8] = (uint32_t)((gpuAddr + 4) >> 32);
    pm4[9] = 0x6F8F0002u;

    uint64_t fence = 0;
    uint32_t fenceCount = 1;
    kr = IOConnectCallMethod(conn, kSelSubmitGraphics, NULL, 0, pm4, sizeof(pm4),
                             &fence, &fenceCount, NULL, NULL);
    snprintf(buf, sizeof buf, "kr=0x%x fence=%llu", kr, (unsigned long long)fence);
    check("submit WRITE_DATA on the GFX queue", kr == KERN_SUCCESS, buf);
    if (kr != KERN_SUCCESS) {
        printf("\nRESULT: GFX submission refused\n");
        return 1;
    }

    uint64_t wait[2] = { fence, 5000 };
    kr = IOConnectCallMethod(conn, kSelWaitGraphicsFence, wait, 2, NULL, 0,
                             NULL, NULL, NULL, NULL);
    snprintf(buf, sizeof buf, "kr=0x%x", kr);
    check("graphics fence reached", kr == KERN_SUCCESS, buf);

    snprintf(buf, sizeof buf, "[0]=0x%08x [1]=0x%08x", words[0], words[1]);
    check("GPU wrote both dwords through the GFX ring",
          words[0] == 0x6F8F0001u && words[1] == 0x6F8F0002u, buf);

    // A second submission must get a new fence: the counter is driver-owned.
    uint64_t fence2 = 0;
    fenceCount = 1;
    kr = IOConnectCallMethod(conn, kSelSubmitGraphics, NULL, 0, pm4, sizeof(pm4),
                             &fence2, &fenceCount, NULL, NULL);
    snprintf(buf, sizeof buf, "%llu then %llu", (unsigned long long)fence,
             (unsigned long long)fence2);
    check("second submission takes a new fence", kr == KERN_SUCCESS && fence2 > fence, buf);

    uint64_t freeAddr = gpuAddr;
    IOConnectCallMethod(conn, kSelFreeBuffer, &freeAddr, 1, NULL, 0, NULL, NULL, NULL, NULL);
    IOServiceClose(conn);

    printf("\nRESULT: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
