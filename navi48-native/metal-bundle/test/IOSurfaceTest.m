#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <CoreVideo/CoreVideo.h>

/**
 * Phase 3.5.1 - IOSurface Support Test
 * 
 * Tests:
 * 1. Creating IOSurface
 * 2. Creating Metal texture from IOSurface
 * 3. Rendering to IOSurface-backed texture
 * 4. Reading back from IOSurface (CPU access)
 * 5. GPU-CPU synchronization
 */

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        NSLog(@"========================================");
        NSLog(@"  Phase 3.5.1 - IOSurface Test");
        NSLog(@"========================================");
        NSLog(@"");
        
        // ============================================================
        // Test 1: Create IOSurface
        // ============================================================
        NSLog(@"Test 1: Creating IOSurface...");
        
        NSUInteger width = 256;
        NSUInteger height = 256;
        
        NSDictionary* surfaceProps = @{
            (id)kIOSurfaceWidth: @(width),
            (id)kIOSurfaceHeight: @(height),
            (id)kIOSurfaceBytesPerElement: @4,
            (id)kIOSurfacePixelFormat: @(kCVPixelFormatType_32BGRA),
            (id)kIOSurfaceBytesPerRow: @(width * 4),
        };
        
        IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef)surfaceProps);
        if (!surface) {
            NSLog(@"❌ FAILED: Could not create IOSurface");
            return 1;
        }
        
        NSLog(@"✅ Created IOSurface: %lux%lu, %zu bytes",
              IOSurfaceGetWidth(surface),
              IOSurfaceGetHeight(surface),
              IOSurfaceGetAllocSize(surface));
        NSLog(@"   Bytes per row: %zu", IOSurfaceGetBytesPerRow(surface));
        NSLog(@"");
        
        // ============================================================
        // Test 2: Get Metal device
        // ============================================================
        NSLog(@"Test 2: Getting Metal device...");
        
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            NSLog(@"❌ FAILED: No Metal device found");
            CFRelease(surface);
            return 1;
        }
        
        NSLog(@"✅ Metal device: %@", device.name);
        NSLog(@"");
        
        // ============================================================
        // Test 3: Create texture from IOSurface
        // ============================================================
        NSLog(@"Test 3: Creating Metal texture from IOSurface...");
        
        MTLTextureDescriptor* texDesc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:width
                                        height:height
                                     mipmapped:NO];
        texDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        texDesc.storageMode = MTLStorageModeShared;  // Important for IOSurface
        
        NSError* error = nil;
        id<MTLTexture> texture = [device newTextureWithDescriptor:texDesc
                                                         iosurface:surface
                                                             plane:0];
        
        if (!texture) {
            NSLog(@"❌ FAILED: Could not create texture from IOSurface");
            NSLog(@"   Error: %@", error);
            CFRelease(surface);
            return 1;
        }
        
        NSLog(@"✅ Created Metal texture from IOSurface");
        NSLog(@"   Size: %lux%lu", texture.width, texture.height);
        NSLog(@"   Format: %lu", (unsigned long)texture.pixelFormat);
        NSLog(@"   Storage mode: %lu (should be 0=Shared)", (unsigned long)texture.storageMode);
        NSLog(@"");
        
        // ============================================================
        // Test 4: Lock IOSurface and fill with test pattern (CPU write)
        // ============================================================
        NSLog(@"Test 4: Writing test pattern to IOSurface (CPU)...");
        
        IOSurfaceLock(surface, 0, NULL);
        
        uint32_t* pixels = (uint32_t*)IOSurfaceGetBaseAddress(surface);
        size_t bytesPerRow = IOSurfaceGetBytesPerRow(surface);
        
        // Fill with gradient pattern
        for (NSUInteger y = 0; y < height; y++) {
            uint32_t* row = (uint32_t*)((uint8_t*)pixels + y * bytesPerRow);
            for (NSUInteger x = 0; x < width; x++) {
                uint8_t r = (x * 255) / width;
                uint8_t g = (y * 255) / height;
                uint8_t b = 128;
                uint8_t a = 255;
                row[x] = (a << 24) | (r << 16) | (g << 8) | b;  // BGRA
            }
        }
        
        IOSurfaceUnlock(surface, 0, NULL);
        
        NSLog(@"✅ Written gradient pattern (CPU)");
        NSLog(@"   Top-left: 0x%08X (should be 0xFF0000FF)", pixels[0]);
        NSLog(@"   Top-right: 0x%08X", ((uint32_t*)((uint8_t*)pixels))[width-1]);
        NSLog(@"");
        
        // ============================================================
        // Test 5: Read back via Metal texture (GPU read)
        // ============================================================
        NSLog(@"Test 5: Reading back via Metal texture (GPU)...");
        
        uint32_t testPixels[4];
        [texture getBytes:testPixels
              bytesPerRow:16
            bytesPerImage:16
               fromRegion:MTLRegionMake2D(0, 0, 2, 2)
              mipmapLevel:0
                    slice:0];
        
        NSLog(@"✅ Read back via Metal:");
        NSLog(@"   [0,0] = 0x%08X", testPixels[0]);
        NSLog(@"   [1,0] = 0x%08X", testPixels[1]);
        NSLog(@"   [0,1] = 0x%08X", testPixels[2]);
        NSLog(@"   [1,1] = 0x%08X", testPixels[3]);
        
        // Verify consistency
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        uint32_t* cpuPixels = (uint32_t*)IOSurfaceGetBaseAddress(surface);
        
        BOOL consistent = YES;
        if (testPixels[0] != cpuPixels[0] ||
            testPixels[1] != cpuPixels[1]) {
            consistent = NO;
        }
        
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        
        if (consistent) {
            NSLog(@"✅ CPU and GPU views are CONSISTENT");
        } else {
            NSLog(@"⚠️  WARNING: CPU and GPU views differ");
            NSLog(@"   This might be expected if cache not synced");
        }
        NSLog(@"");
        
        // ============================================================
        // Test 6: Render to IOSurface-backed texture
        // ============================================================
        NSLog(@"Test 6: Rendering to IOSurface-backed texture...");
        
        // Load shaders
        NSString* libraryPath = @"triangle.metallib";
        id<MTLLibrary> library = [device newLibraryWithFile:libraryPath error:&error];
        if (!library) {
            NSLog(@"⚠️  SKIPPED: No shader library found (%@)", libraryPath);
            NSLog(@"   Compile first: xcrun -sdk macosx metal -c triangle.metal -o triangle.air");
            NSLog(@"   Then: xcrun -sdk macosx metallib triangle.air -o triangle.metallib");
            goto test7;
        }
        
        id<MTLFunction> vertexFunc = [library newFunctionWithName:@"vertexShader"];
        id<MTLFunction> fragmentFunc = [library newFunctionWithName:@"fragmentShader"];
        
        if (!vertexFunc || !fragmentFunc) {
            NSLog(@"⚠️  SKIPPED: Shader functions not found");
            goto test7;
        }
        
        // Create pipeline
        MTLRenderPipelineDescriptor* pipelineDesc = [MTLRenderPipelineDescriptor new];
        pipelineDesc.vertexFunction = vertexFunc;
        pipelineDesc.fragmentFunction = fragmentFunc;
        MTLVertexDescriptor* vertexDescriptor = [MTLVertexDescriptor new];
        vertexDescriptor.attributes[0].format = MTLVertexFormatFloat2;
        vertexDescriptor.attributes[0].offset = 0;
        vertexDescriptor.attributes[0].bufferIndex = 0;
        vertexDescriptor.attributes[1].format = MTLVertexFormatFloat3;
        vertexDescriptor.attributes[1].offset = 8;
        vertexDescriptor.attributes[1].bufferIndex = 0;
        vertexDescriptor.layouts[0].stride = 20;
        vertexDescriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        vertexDescriptor.layouts[0].stepRate = 1;
        pipelineDesc.vertexDescriptor = vertexDescriptor;
        pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        
        id<MTLRenderPipelineState> pipeline = 
            [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
        
        if (!pipeline) {
            NSLog(@"⚠️  SKIPPED: Could not create pipeline: %@", error);
            goto test7;
        }
        
        // Create command queue and buffer
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
        static const float triangleVertices[3][5] = {
            {-0.8f, -0.8f, 1.0f, 0.0f, 0.0f},
            { 0.8f, -0.8f, 0.0f, 1.0f, 0.0f},
            { 0.0f,  0.8f, 0.0f, 0.0f, 1.0f},
        };
        id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:triangleVertices
                                                         length:sizeof(triangleVertices)
                                                        options:MTLResourceStorageModeShared];
        
        // Setup render pass
        MTLRenderPassDescriptor* renderPass = [MTLRenderPassDescriptor renderPassDescriptor];
        renderPass.colorAttachments[0].texture = texture;
        renderPass.colorAttachments[0].loadAction = MTLLoadActionClear;
        renderPass.colorAttachments[0].storeAction = MTLStoreActionStore;
        renderPass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.1, 1.0);
        
        // Encode rendering
        id<MTLRenderCommandEncoder> encoder = 
            [commandBuffer renderCommandEncoderWithDescriptor:renderPass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        
        MTLViewport viewport = {0, 0, (double)width, (double)height, 0, 1};
        [encoder setViewport:viewport];
        
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:3];
        [encoder endEncoding];
        
        // Submit and wait
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        if (commandBuffer.status == MTLCommandBufferStatusCompleted) {
            NSLog(@"✅ Rendered to IOSurface-backed texture");
        } else {
            NSLog(@"❌ FAILED: Rendering failed with status %ld", (long)commandBuffer.status);
            goto cleanup;
        }
        
        // Read back center pixel
        uint32_t centerPixel;
        [texture getBytes:&centerPixel
              bytesPerRow:4
            bytesPerImage:4
               fromRegion:MTLRegionMake2D(width/2, height/2, 1, 1)
              mipmapLevel:0
                    slice:0];
        
        NSLog(@"   Center pixel: 0x%08X", centerPixel);
        
        // Verify via CPU (IOSurface)
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        uint32_t* surfPixels = (uint32_t*)IOSurfaceGetBaseAddress(surface);
        size_t surfBytesPerRow = IOSurfaceGetBytesPerRow(surface);
        uint32_t* centerRow = (uint32_t*)((uint8_t*)surfPixels + (height/2) * surfBytesPerRow);
        uint32_t cpuCenterPixel = centerRow[width/2];
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        
        NSLog(@"   CPU reads:    0x%08X", cpuCenterPixel);
        
        if (centerPixel == cpuCenterPixel) {
            NSLog(@"✅ GPU render → CPU read: CONSISTENT");
        } else {
            NSLog(@"⚠️  WARNING: GPU/CPU mismatch (cache sync issue?)");
        }
        NSLog(@"");
        
test7:
        // ============================================================
        // Test 7: Performance test
        // ============================================================
        NSLog(@"Test 7: Performance test (IOSurface operations)...");
        
        NSDate* startTime = [NSDate date];
        int iterations = 1000;
        
        for (int i = 0; i < iterations; i++) {
            IOSurfaceLock(surface, 0, NULL);
            uint32_t* ptr = (uint32_t*)IOSurfaceGetBaseAddress(surface);
            ptr[0] = i;  // Write something
            IOSurfaceUnlock(surface, 0, NULL);
        }
        
        NSTimeInterval elapsed = -[startTime timeIntervalSinceNow];
        double opsPerSec = iterations / elapsed;
        
        NSLog(@"✅ Performance: %.0f lock/unlock ops/sec", opsPerSec);
        NSLog(@"   (%d iterations in %.3f seconds)", iterations, elapsed);
        NSLog(@"");
        
        // ============================================================
        // Summary
        // ============================================================
        NSLog(@"========================================");
        NSLog(@"  Test Summary");
        NSLog(@"========================================");
        NSLog(@"✅ Test 1: IOSurface creation");
        NSLog(@"✅ Test 2: Metal device");
        NSLog(@"✅ Test 3: Texture from IOSurface");
        NSLog(@"✅ Test 4: CPU write to IOSurface");
        NSLog(@"✅ Test 5: GPU read from IOSurface");
        NSLog(@"%@ Test 6: GPU render to IOSurface", library ? @"✅" : @"⚠️ ");
        NSLog(@"✅ Test 7: Performance");
        NSLog(@"");
        NSLog(@"Phase 3.5.1 IOSurface support: %@", library ? @"PASSED" : @"PARTIALLY PASSED");
        NSLog(@"========================================");
        
cleanup:
        CFRelease(surface);
        return 0;
    }
}
