#include "RDNA4UserClient.h"
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <string.h>

enum {
    kSelectorGetDeviceInfo = 0,
    kSelectorAllocateBuffer = 1,
    kSelectorFreeBuffer = 2,
    kSelectorSubmitCommands = 3,
    kSelectorWaitFence = 4,
    kSelectorSubmitGraphics = 5,
    kSelectorWaitGraphicsFence = 6,
    kSelectorGetNggRing = 8,
    kMemoryTypeUserWindow = 0,
};

static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static RDNA4GpuLink gLink;
static int gLinkValid = 0;

static int rdna4_open_service(RDNA4GpuLink* link) {
    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("RDNA4Driver"));
    if (!service) return 0;

    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &link->connection);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) return 0;

    uint64_t outputs[4] = {0, 0, 0, 0};
    uint32_t count = 4;
    kr = IOConnectCallMethod(link->connection, kSelectorGetDeviceInfo,
                             NULL, 0, NULL, 0, outputs, &count, NULL, NULL);
    if (kr != KERN_SUCCESS || count < 4 || outputs[3] == 0) {
        IOServiceClose(link->connection);
        link->connection = 0;
        return 0;
    }
    link->windowGpu = outputs[2];
    link->windowSize = outputs[3];

    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    kr = IOConnectMapMemory64(link->connection, kMemoryTypeUserWindow,
                              mach_task_self(), &address, &size, kIOMapAnywhere);
    // The window stays allocatable even when the map fails; contents then has
    // no CPU view and RDNA4GpuPointerForAddress() reports NULL.
    link->windowCpu = (kr == KERN_SUCCESS) ? (void*)address : NULL;
    return 1;
}

RDNA4GpuLink* RDNA4GpuLinkAcquire(void) {
    pthread_mutex_lock(&gLock);
    if (!gLinkValid) {
        memset(&gLink, 0, sizeof(gLink));
        gLinkValid = rdna4_open_service(&gLink);
    }
    RDNA4GpuLink* link = gLinkValid ? &gLink : NULL;
    pthread_mutex_unlock(&gLock);
    return link;
}

void RDNA4GpuLinkRelease(void) {
    pthread_mutex_lock(&gLock);
    if (gLinkValid) {
        if (gLink.windowCpu) {
            IOConnectUnmapMemory64(gLink.connection, kMemoryTypeUserWindow,
                                   mach_task_self(),
                                   (mach_vm_address_t)gLink.windowCpu);
            gLink.windowCpu = NULL;
        }
        if (gLink.connection) IOServiceClose(gLink.connection);
        memset(&gLink, 0, sizeof(gLink));
        gLinkValid = 0;
    }
    pthread_mutex_unlock(&gLock);
}

void* RDNA4GpuPointerForAddress(RDNA4GpuLink* link, uint64_t gpuAddress) {
    if (!link || !link->windowCpu) return NULL;
    if (gpuAddress < link->windowGpu) return NULL;
    const uint64_t offset = gpuAddress - link->windowGpu;
    if (offset >= link->windowSize) return NULL;
    return (uint8_t*)link->windowCpu + offset;
}

uint64_t RDNA4GpuAllocate(RDNA4GpuLink* link, uint64_t size) {
    if (!link || !link->connection || size == 0) return 0;
    uint64_t input = size;
    uint64_t output = 0;
    uint32_t count = 1;
    kern_return_t kr = IOConnectCallMethod(link->connection, kSelectorAllocateBuffer,
                                          &input, 1, NULL, 0, &output, &count, NULL, NULL);
    if (kr != KERN_SUCCESS || count != 1 || output == 0) {
        NSLog(@"RDNA4GpuAllocate: %llu bytes failed (IOReturn=0x%08x, outputs=%u, "
               "window=%llu bytes at GPU 0x%llx)",
              (unsigned long long)size, kr, count,
              (unsigned long long)link->windowSize, (unsigned long long)link->windowGpu);
        return 0;
    }
    return output;
}

uint64_t RDNA4GpuNggRing(RDNA4GpuLink* link, uint32_t* size) {
    if (!link || !link->connection) return 0;
    uint64_t outputs[2] = { 0, 0 };
    uint32_t count = 2;
    kern_return_t kr = IOConnectCallMethod(link->connection, kSelectorGetNggRing,
                                           NULL, 0, NULL, 0, outputs, &count, NULL, NULL);
    if (kr != KERN_SUCCESS || count < 2) return 0;
    if (size) *size = (uint32_t)outputs[1];
    return outputs[0];
}

void RDNA4GpuFree(RDNA4GpuLink* link, uint64_t gpuAddress) {    if (!link || !link->connection || gpuAddress == 0) return;
    uint64_t input = gpuAddress;
    IOConnectCallMethod(link->connection, kSelectorFreeBuffer,
                        &input, 1, NULL, 0, NULL, NULL, NULL, NULL);
}

int RDNA4GpuSubmit(RDNA4GpuLink* link, const void* pm4, size_t bytes, uint64_t* fence) {
    if (!link || !link->connection || !pm4 || bytes == 0) return -1;
    uint64_t output = 0;
    uint32_t count = 1;
    kern_return_t kr = IOConnectCallMethod(link->connection, kSelectorSubmitCommands,
                                           NULL, 0, pm4, bytes,
                                           &output, &count, NULL, NULL);
    if (kr != KERN_SUCCESS) return -1;
    if (fence) *fence = output;
    return 0;
}

int RDNA4GpuWait(RDNA4GpuLink* link, uint64_t fence, uint64_t timeoutMs) {
    if (!link || !link->connection) return -1;
    uint64_t inputs[2] = { fence, timeoutMs ? timeoutMs : 5000 };
    kern_return_t kr = IOConnectCallMethod(link->connection, kSelectorWaitFence,
                                           inputs, 2, NULL, 0, NULL, NULL, NULL, NULL);
    return kr == KERN_SUCCESS ? 0 : -1;
}

int RDNA4GpuSubmitGraphics(RDNA4GpuLink* link, const void* pm4, size_t bytes, uint64_t* fence) {
    if (!link || !link->connection || !pm4 || bytes == 0) return -1;
    uint64_t output = 0;
    uint32_t count = 1;
    kern_return_t kr = IOConnectCallMethod(link->connection, kSelectorSubmitGraphics,
                                           NULL, 0, pm4, bytes,
                                           &output, &count, NULL, NULL);
    if (kr != KERN_SUCCESS) return -1;
    if (fence) *fence = output;
    return 0;
}

int RDNA4GpuWaitGraphics(RDNA4GpuLink* link, uint64_t fence, uint64_t timeoutMs) {
    if (!link || !link->connection) return -1;
    uint64_t inputs[2] = { fence, timeoutMs ? timeoutMs : 5000 };
    kern_return_t kr = IOConnectCallMethod(link->connection, kSelectorWaitGraphicsFence,
                                           inputs, 2, NULL, 0, NULL, NULL, NULL, NULL);
    return kr == KERN_SUCCESS ? 0 : -1;
}
