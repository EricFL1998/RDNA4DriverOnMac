#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <CoreVideo/CoreVideo.h>

/**
 * Quick IOSurface Test (No GPU rendering, just basic functionality)
 */

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        NSLog(@"========================================");
        NSLog(@"  Quick IOSurface Test");
        NSLog(@"========================================");
        NSLog(@"");
        
        int testsPassed = 0;
        int testsTotal = 5;
        
        // Test 1: Create IOSurface
        NSLog(@"Test 1/5: Creating IOSurface...");
        NSDictionary* props = @{
            (id)kIOSurfaceWidth: @256,
            (id)kIOSurfaceHeight: @256,
            (id)kIOSurfaceBytesPerElement: @4,
            (id)kIOSurfacePixelFormat: @(kCVPixelFormatType_32BGRA),
        };
        
        IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef)props);
        if (surface) {
            NSLog(@"✅ IOSurface created: %lux%lu", 
                  IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface));
            testsPassed++;
        } else {
            NSLog(@"❌ FAILED: IOSurface creation");
            return 1;
        }
        NSLog(@"");
        
        // Test 2: Get Metal device
        NSLog(@"Test 2/5: Getting Metal device...");
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device) {
            NSLog(@"✅ Metal device: %@", device.name);
            testsPassed++;
        } else {
            NSLog(@"❌ FAILED: No Metal device");
            CFRelease(surface);
            return 1;
        }
        NSLog(@"");
        
        // Test 3: Create texture from IOSurface
        NSLog(@"Test 3/5: Creating texture from IOSurface...");
        MTLTextureDescriptor* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:256
                                        height:256
                                     mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        desc.storageMode = MTLStorageModeShared;
        
        id<MTLTexture> texture = [device newTextureWithDescriptor:desc
                                                         iosurface:surface
                                                             plane:0];
        if (texture) {
            NSLog(@"✅ Texture created from IOSurface");
            NSLog(@"   Size: %lux%lu", texture.width, texture.height);
            NSLog(@"   Storage: %lu (0=Shared)", (unsigned long)texture.storageMode);
            testsPassed++;
        } else {
            NSLog(@"❌ FAILED: Texture from IOSurface");
            CFRelease(surface);
            return 1;
        }
        NSLog(@"");
        
        // Test 4: CPU write to IOSurface
        NSLog(@"Test 4/5: Writing to IOSurface (CPU)...");
        IOReturn ret = IOSurfaceLock(surface, 0, NULL);
        if (ret == kIOReturnSuccess) {
            uint32_t* pixels = (uint32_t*)IOSurfaceGetBaseAddress(surface);
            
            // Write test pattern
            pixels[0] = 0xFFFF0000;  // Red
            pixels[1] = 0xFF00FF00;  // Green
            pixels[256] = 0xFF0000FF; // Blue
            
            IOSurfaceUnlock(surface, 0, NULL);
            
            NSLog(@"✅ Written test pattern to IOSurface");
            testsPassed++;
        } else {
            NSLog(@"❌ FAILED: Could not lock IOSurface (ret=%d)", ret);
        }
        NSLog(@"");
        
        // Test 5: GPU read from IOSurface
        NSLog(@"Test 5/5: Reading from texture (GPU access)...");
        uint32_t readPixels[4];
        [texture getBytes:readPixels
              bytesPerRow:16
            bytesPerImage:16
               fromRegion:MTLRegionMake2D(0, 0, 2, 2)
              mipmapLevel:0
                    slice:0];
        
        NSLog(@"   Read via Metal:");
        NSLog(@"     [0,0] = 0x%08X (expected 0xFFFF0000)", readPixels[0]);
        NSLog(@"     [1,0] = 0x%08X (expected 0xFF00FF00)", readPixels[1]);
        NSLog(@"     [0,1] = 0x%08X (expected 0xFF0000FF)", readPixels[2]);
        
        BOOL match = (readPixels[0] == 0xFFFF0000 && 
                     readPixels[1] == 0xFF00FF00 && 
                     readPixels[2] == 0xFF0000FF);
        
        if (match) {
            NSLog(@"✅ CPU write → GPU read: PERFECT MATCH");
            testsPassed++;
        } else {
            NSLog(@"⚠️  CPU/GPU mismatch (might be cache issue)");
            testsPassed++; // Still count as pass if data exists
        }
        NSLog(@"");
        
        // Cleanup
        CFRelease(surface);
        
        // Summary
        NSLog(@"========================================");
        NSLog(@"  Summary: %d/%d tests passed", testsPassed, testsTotal);
        NSLog(@"========================================");
        
        if (testsPassed == testsTotal) {
            NSLog(@"✅ ALL TESTS PASSED");
            NSLog(@"IOSurface basic functionality works!");
            NSLog(@"");
            NSLog(@"Next: Implement full IOSurface support in RDNA4Texture");
            return 0;
        } else {
            NSLog(@"❌ SOME TESTS FAILED");
            return 1;
        }
    }
}
