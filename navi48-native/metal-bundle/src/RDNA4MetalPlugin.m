#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import "RDNA4Device.h"

/**
 * Optional entry points for direct driver diagnostics. Metal.framework does
 * not call these functions: it loads RDNA4Device and invokes
 * -initWithAcceleratorPort: through its native device discovery path.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a new RDNA4 Metal device
 * 
 * Finds a live RDNA4Accel service; returns nil when no RDNA4 hardware exists.
 * 
 * @return New RDNA4Device instance, or nil if creation fails
 */
id<MTLDevice> RDNA4CreateDevice(void) {
    // Create device instance
    RDNA4Device* device = [[RDNA4Device alloc] init];
    
    if (device) {
        NSLog(@"RDNA4CreateDevice: ✅ Created device: %@", device.name);
        return device;
    } else {
        NSLog(@"RDNA4CreateDevice: ❌ Failed to create device");
        return nil;
    }
}

/**
 * Get all available RDNA4 devices
 * 
 * Alternative entry point that returns an array of all devices.
 */
NSArray<id<MTLDevice>>* RDNA4GetAllDevices(void) {
    NSLog(@"RDNA4GetAllDevices: Called");
    
    id<MTLDevice> device = RDNA4CreateDevice();
    if (device) {
        return @[device];
    }
    
    return @[];
}

/**
 * Bundle load function
 * 
 * Loading alone does not prove that Metal accepted the device class.
 */
__attribute__((constructor))
static void RDNA4MetalBundleLoad(void) {
    NSLog(@"RDNA4Metal 1.0.2 loaded (native _MTLDevice discovery)");
}

/**
 * Bundle unload function
 */
__attribute__((destructor))
static void RDNA4MetalBundleUnload(void) {
    NSLog(@"RDNA4Metal Bundle Unloaded");
}

#ifdef __cplusplus
}
#endif
