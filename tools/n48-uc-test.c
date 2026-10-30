// User-space bring-up test for the RDNA4Driver user client.
//
//   1. open RDNA4Driver, GetDeviceInfo -> user window base/size
//   2. IOConnectMapMemory64(type 0) -> map the window
//   3. AllocateBuffer -> GPU address inside the window
//   4. write a pattern through the mapping, read it back (mapping works?)
//   5. submit one WRITE_DATA PM4 that stores a magic into the buffer,
//      wait for the driver's fence, read the buffer back (GPU works?)
//
// gcc -o n48_uc_test n48_uc_test.c -framework IOKit -framework CoreFoundation

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <IOKit/IOKitLib.h>
#include <mach/error.h>

enum {
    kSelGetDeviceInfo = 0,
    kSelAllocateBuffer = 1,
    kSelFreeBuffer = 2,
    kSelSubmitCommands = 3,
    kSelWaitFence = 4,
    kMemTypeWindow = 0,
};

#define RET_OK 0

static int check(const char* what, kern_return_t kr) {
    if (kr == KERN_SUCCESS) {
        printf("  ok   %s\n", what);
        return RET_OK;
    }
    printf("  FAIL %s (kr=0x%x)\n", what, kr);
    return 1;
}

int main(void) {
    int failures = 0;
    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("RDNA4Driver"));
    if (!service) {
        printf("FAIL: RDNA4Driver service not found\n");
        return 1;
    }

    io_connect_t conn = 0;
    if (check("IOServiceOpen", IOServiceOpen(service, mach_task_self(), 0, &conn))) {
        IOObjectRelease(service);
        return 1;
    }
    IOObjectRelease(service);

    uint64_t info[4] = {0};
    uint32_t infoCount = 4;
    if (check("GetDeviceInfo",
              IOConnectCallMethod(conn, kSelGetDeviceInfo, NULL, 0, NULL, 0,
                                  info, &infoCount, NULL, NULL))) return 1;
    printf("  vendor 0x%llx device 0x%llx windowGPU 0x%llx size %llu MiB\n",
           info[0], info[1], info[2], info[3] >> 20);
    const uint64_t windowGpu = info[2];
    const uint64_t windowSize = info[3];
    if (windowGpu == 0 || windowSize == 0) {
        printf("FAIL: empty user window\n");
        return 1;
    }

    mach_vm_address_t mapped = 0;
    mach_vm_size_t mappedSize = 0;
    if (check("IOConnectMapMemory64(window)",
              IOConnectMapMemory64(conn, kMemTypeWindow, mach_task_self(),
                                   &mapped, &mappedSize, kIOMapAnywhere))) return 1;
    printf("  mapped %llu MiB at %p\n",
           (unsigned long long)(mappedSize >> 20), (void*)mapped);

    uint64_t allocIn = 4096;
    uint64_t gpuAddr = 0;
    uint32_t outCount = 1;
    if (check("AllocateBuffer(4096)",
              IOConnectCallMethod(conn, kSelAllocateBuffer, &allocIn, 1, NULL, 0,
                                  &gpuAddr, &outCount, NULL, NULL))) return 1;
    printf("  buffer GPU 0x%llx (offset 0x%llx in window)\n",
           gpuAddr, gpuAddr - windowGpu);
    if (gpuAddr < windowGpu || gpuAddr - windowGpu >= windowSize) {
        printf("FAIL: address is outside the mapped window\n");
        return 1;
    }

    volatile uint32_t* buf = (volatile uint32_t*)((uint8_t*)mapped + (gpuAddr - windowGpu));

    // 4. CPU-side mapping round trip.
    for (int i = 0; i < 64; i++) buf[i] = 0xC0DE0000u + i;
    int mismatches = 0;
    for (int i = 0; i < 64; i++) if (buf[i] != 0xC0DE0000u + i) mismatches++;
    printf("  %s mapping write/read (%d/%d mismatched)\n",
           mismatches == 0 ? "ok  " : "FAIL", mismatches, 64);
    if (mismatches) failures++;

    // 5. GPU-side: WRITE_DATA 0x1234ABCD into buf[0].
    for (int i = 0; i < 64; i++) buf[i] = 0;
    uint32_t pm4[5];
    pm4[0] = (3u << 30) | (3u << 16) | (0x37u << 8);   // PACKET3_WRITE_DATA, count 3
    pm4[1] = (1u << 30) | (5u << 8) | (1u << 20);      // ENGINE_SEL=ME, DST_SEL=memory, WR_CONFIRM
    pm4[2] = (uint32_t)(gpuAddr & 0xFFFFFFFCu);
    pm4[3] = (uint32_t)(gpuAddr >> 32);
    pm4[4] = 0x1234ABCDu;

    uint64_t fence = 0;
    uint32_t fenceCount = 1;
    if (check("SubmitCommands(WRITE_DATA)",
              IOConnectCallMethod(conn, kSelSubmitCommands, NULL, 0,
                                  pm4, sizeof(pm4),
                                  &fence, &fenceCount, NULL, NULL))) {
        failures++;
    } else {
        printf("  driver fence value %llu\n", fence);
        uint64_t waitIn[2] = { fence, 5000 };
        if (check("WaitFence(5000 ms)",
                  IOConnectCallMethod(conn, kSelWaitFence, waitIn, 2, NULL, 0,
                                      NULL, NULL, NULL, NULL))) {
            failures++;
        } else if (buf[0] != 0x1234ABCDu) {
            printf("  FAIL GPU WRITE_DATA: buf[0]=0x%08x (want 0x1234ABCD)\n", buf[0]);
            failures++;
        } else {
            printf("  ok   GPU WRITE_DATA landed: buf[0]=0x%08x\n", buf[0]);
        }
    }

    uint64_t freeIn = gpuAddr;
    check("FreeBuffer", IOConnectCallMethod(conn, kSelFreeBuffer, &freeIn, 1, NULL, 0,
                                            NULL, NULL, NULL, NULL));

    IOServiceClose(conn);
    printf("\n%s\n", failures ? "RESULT: FAILURES" : "RESULT: ALL PASSED");
    return failures ? 1 : 0;
}
