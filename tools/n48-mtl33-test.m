// End-to-end test of the Metal bundle without Metal's system loader:
//   dlopen the bundle -> instantiate RDNA4Device -> newBufferWithLength
//   -> write through buffer.contents -> GPU writes into the same VRAM via a
//   raw PM4 WRITE_DATA -> read back through contents.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <dlfcn.h>

@interface RDNA4BufferProbe : NSObject
@end
@implementation RDNA4BufferProbe
@end
static uint64_t bufferGpuAddress(id buf) {
    SEL s = NSSelectorFromString(@"gpuAddress");
    if (![buf respondsToSelector:s]) return 0;
    typedef uint64_t (*Fn)(id, SEL);
    return ((Fn)[buf methodForSelector:s])(buf, s);
}

int main(void) {
    @autoreleasepool {
        const char* path = "/Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal";
        void* h = dlopen(path, RTLD_NOW);
        if (!h) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
        Class devClass = NSClassFromString(@"RDNA4Device");
        if (!devClass) { printf("FAIL RDNA4Device class missing\n"); return 1; }
        id<MTLDevice> dev = [[devClass alloc] init];
        if (!dev) { printf("FAIL device init\n"); return 1; }
        printf("ok   RDNA4Device instantiated: %s\n", [[dev name] UTF8String]);

        id<MTLBuffer> buf = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        if (!buf) { printf("FAIL newBufferWithLength\n"); return 1; }
        const uint64_t gpu = bufferGpuAddress(buf);
        printf("ok   buffer: %lu bytes, contents=%p, GPU 0x%llx\n",
               (unsigned long)[buf length], [buf contents], (unsigned long long)gpu);
        if (![buf contents] || gpu == 0) { printf("FAIL buffer has no mapping or address\n"); return 1; }

        volatile uint32_t* host = (volatile uint32_t*)[buf contents];
        for (int i = 0; i < 8; i++) host[i] = 0xA5A50000u + i;
        int bad = 0;
        for (int i = 0; i < 8; i++) if (host[i] != 0xA5A50000u + i) bad++;
        printf("%s host write/read via contents (%d bad)\n", bad ? "FAIL" : "ok  ", bad);
        if (bad) return 1;

        io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                      IOServiceMatching("RDNA4Driver"));
        if (!svc) { printf("FAIL RDNA4Driver service\n"); return 1; }
        io_connect_t conn = 0;
        if (IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
            printf("FAIL IOServiceOpen\n"); return 1;
        }
        IOObjectRelease(svc);

        for (int i = 0; i < 8; i++) host[i] = 0;
        uint32_t pm4[5];
        pm4[0] = (3u << 30) | (3u << 16) | (0x37u << 8);
        pm4[1] = (1u << 30) | (5u << 8) | (1u << 20);
        pm4[2] = (uint32_t)(gpu & 0xFFFFFFFCu);
        pm4[3] = (uint32_t)(gpu >> 32);
        pm4[4] = 0x33AA33AAu;

        uint64_t fence = 0; uint32_t fc = 1;
        kern_return_t kr = IOConnectCallMethod(conn, 3, NULL, 0, pm4, sizeof(pm4),
                                               &fence, &fc, NULL, NULL);
        if (kr != KERN_SUCCESS) { printf("FAIL submit kr=0x%x\n", kr); return 1; }
        uint64_t win[2] = { fence, 5000 };
        kr = IOConnectCallMethod(conn, 4, win, 2, NULL, 0, NULL, NULL, NULL, NULL);
        if (kr != KERN_SUCCESS) { printf("FAIL waitFence kr=0x%x\n", kr); return 1; }

        if (host[0] == 0x33AA33AAu) {
            printf("ok   GPU wrote through the bundle's buffer: contents[0]=0x%08x\n", host[0]);
        } else {
            printf("FAIL GPU WRITE_DATA: contents[0]=0x%08x\n", host[0]);
            IOServiceClose(conn);
            return 1;
        }
        // Command buffer path: queue -> buffer -> commit -> wait.
        id<MTLCommandQueue> q = [dev newCommandQueue];
        if (!q) { printf("FAIL newCommandQueue\n"); IOServiceClose(conn); return 1; }
        id<MTLCommandBuffer> cb = [q commandBuffer];
        if (!cb) { printf("FAIL commandBuffer\n"); IOServiceClose(conn); return 1; }
        [cb commit];
        [cb waitUntilCompleted];
        const MTLCommandBufferStatus st = [cb status];
        printf("%s command buffer commit/complete (status=%lu)\n",
               st == MTLCommandBufferStatusCompleted ? "ok  " : "FAIL", (unsigned long)st);
        if (st != MTLCommandBufferStatusCompleted) { IOServiceClose(conn); return 1; }

        IOServiceClose(conn);
        printf("\nRESULT: 3.3 BUFFER + COMMAND PATH PASSED\n");
    }
    return 0;
}
