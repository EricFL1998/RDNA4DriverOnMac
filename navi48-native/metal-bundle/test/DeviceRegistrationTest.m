#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>

// Normal discovery must use Metal.framework, without manually loading the driver.
@interface NSObject (RDNA4InitializationTest)
- (instancetype)initWithAcceleratorPort:(io_service_t)service;
- (void)initLimits;
- (void)initFeatureQueries;
- (void)initWorkarounds;
@end

static int fail(NSString *message) {
    fprintf(stderr, "FAIL: %s\n", message.UTF8String);
    return 1;
}

static int checkBundle(NSString *path) {
    NSError *error = nil;
    NSBundle *bundle = [NSBundle bundleWithPath:path];
    if (![bundle loadAndReturnError:&error]) {
        return fail([NSString stringWithFormat:@"bundle load: %@", error]);
    }
    Class cls = [bundle classNamed:@"RDNA4Device"];
    Class base = NSClassFromString(@"_MTLDevice");
    if (!cls || !base || ![cls isSubclassOfClass:base]) {
        return fail(@"Metal's loader requires RDNA4Device to inherit _MTLDevice");
    }
    Protocol *spi = NSProtocolFromString(@"MTLDeviceSPI");
    if (!spi || ![cls conformsToProtocol:spi]) {
        return fail(@"Metal's MTLAddDevice requires MTLDeviceSPI conformance");
    }
    if (![cls instancesRespondToSelector:@selector(initWithAcceleratorPort:)]) {
        return fail(@"missing initWithAcceleratorPort:");
    }
    if ([[cls alloc] initWithAcceleratorPort:IO_OBJECT_NULL]) {
        return fail(@"a null accelerator port must not create a device");
    }
    io_service_t root = IORegistryGetRootEntry(kIOMainPortDefault);
    id invalid = [[cls alloc] initWithAcceleratorPort:root];
    IOObjectRelease(root);
    if (invalid) return fail(@"an unrelated registry entry must not create a device");
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceMatching("RDNA4Accel"));
    if (service) {
        uint64_t expectedID = 0;
        IORegistryEntryGetRegistryEntryID(service, &expectedID);
        id<MTLDevice> device = [[cls alloc] initWithAcceleratorPort:service];
        IOObjectRelease(service);
        if (!device || device.registryID != expectedID) {
            return fail(@"initializer must preserve the matched service's registryID");
        }
        // The native loader runs these before admitting a device. Exercise
        // the framework implementation while the candidate is still staged.
        [(id)device initLimits];
        [(id)device initFeatureQueries];
        [(id)device initWorkarounds];
        puts("PASS: live accelerator initialization and native Metal feature initialization");
    }
    puts("PASS: native device class and invalid-port rejection");
    return 0;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        @try {
            if (argc == 3 && strcmp(argv[1], "--bundle") == 0) {
                return checkBundle([NSString stringWithUTF8String:argv[2]]);
            }
            BOOL resources = argc == 2 && strcmp(argv[1], "--resources") == 0;
            if (argc != 1 && !resources) {
                return fail(@"usage: DeviceRegistrationTest [--resources | --bundle /absolute/path]");
            }
            NSMutableSet<NSNumber *> *expected = [NSMutableSet set];
            io_iterator_t it = IO_OBJECT_NULL;
            kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                IOServiceMatching("RDNA4Accel"), &it);
            if (kr != KERN_SUCCESS) return fail(@"cannot enumerate RDNA4Accel services");
            io_service_t service;
            while ((service = IOIteratorNext(it))) {
                uint64_t registryID = 0;
                kr = IORegistryEntryGetRegistryEntryID(service, &registryID);
                IOObjectRelease(service);
                if (kr == KERN_SUCCESS) [expected addObject:@(registryID)];
            }
            IOObjectRelease(it);
            if (expected.count == 0) return fail(@"no RDNA4Accel hardware; another GPU is not a pass");

            NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
            NSMutableSet<NSNumber *> *found = [NSMutableSet set];
            for (id<MTLDevice> device in devices) {
                printf("Metal: %s registryID=0x%llx\n", device.name.UTF8String,
                       (unsigned long long)device.registryID);
                if (![expected containsObject:@(device.registryID)]) continue;
                if ([found containsObject:@(device.registryID)]) return fail(@"duplicate RDNA4 device");
                [found addObject:@(device.registryID)];
                io_service_t live = IOServiceGetMatchingService(kIOMainPortDefault,
                    IORegistryEntryIDMatching(device.registryID));
                BOOL matches = live && IOObjectConformsTo(live, "RDNA4Accel");
                if (live) IOObjectRelease(live);
                if (!matches) return fail(@"Metal registryID does not resolve to RDNA4Accel");
                if (resources) {
                    id<MTLCommandQueue> queue = [device newCommandQueue];
                    id<MTLBuffer> buffer = [device newBufferWithLength:4096
                        options:MTLResourceStorageModeShared];
                    if (!queue || queue.device != device || ![queue commandBuffer]) {
                        return fail(@"command queue/buffer creation failed");
                    }
                    if (!buffer || buffer.device != device || buffer.length != 4096 || !buffer.contents) {
                        return fail(@"GPU buffer allocation/mapping failed");
                    }
                    memset(buffer.contents, 0x5a, buffer.length);
                    if (((volatile unsigned char *)buffer.contents)[4095] != 0x5a) {
                        return fail(@"mapped buffer readback failed");
                    }
                    puts("PASS: queue, command buffer and mapped 4096-byte allocation (no GPU submission)");
                }
            }
            if (![found isEqualToSet:expected]) {
                return fail([NSString stringWithFormat:@"RDNA4 missing from MTLCopyAllDevices: expected %@, found %@",
                             expected, found]);
            }
            id<MTLDevice> defaultDevice = MTLCreateSystemDefaultDevice();
            if (!defaultDevice) return fail(@"MTLCreateSystemDefaultDevice returned nil");
            printf("Default: %s registryID=0x%llx\n", defaultDevice.name.UTF8String,
                   (unsigned long long)defaultDevice.registryID);
            if (devices.count == 1 && ![found containsObject:@(defaultDevice.registryID)]) {
                return fail(@"the only Metal device is RDNA4 but the default does not match");
            }
            puts("PASS: RDNA4 discovered by the system Metal API with its live registryID");
            return 0;
        } @catch (NSException *exception) {
            return fail([NSString stringWithFormat:@"%@: %@", exception.name, exception.reason]);
        }
    }
}
