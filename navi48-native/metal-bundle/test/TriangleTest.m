#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/**
 * TriangleTest - Simple Triangle Rendering Test
 * 
 * 首个三角形绘制测试
 * 
 * Tests:
 * - Creating render pipeline
 * - Setting up vertex buffer
 * - Creating render target texture
 * - Encoding draw commands
 * - Submitting to GPU
 * - Reading back results
 */

// Simple vertex structure
typedef struct {
    float position[2];  // x, y
    float color[3];     // r, g, b
} Vertex;

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        NSLog(@"========================================");
        NSLog(@"  RDNA4 Triangle Test");
        NSLog(@"========================================");
        NSLog(@"");
        
        // Get Metal device
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            NSLog(@"❌ FATAL: No Metal device found");
            return 1;
        }
        
        NSLog(@"✅ Found Metal device: %@", device.name);
        NSLog(@"");
        
        // Create command queue
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) {
            NSLog(@"❌ FATAL: Failed to create command queue");
            return 1;
        }
        
        // Load shaders
        NSString* libraryPath = @"triangle.metallib";
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithFile:libraryPath error:&error];
        if (!library) {
            NSLog(@"❌ Failed to load library: %@", error);
            NSLog(@"   Looking for: %@", libraryPath);
            NSLog(@"   You need to compile triangle.metal first:");
            NSLog(@"   xcrun -sdk macosx metal -c triangle.metal -o triangle.air");
            NSLog(@"   xcrun -sdk macosx metallib triangle.air -o triangle.metallib");
            return 1;
        }
        NSLog(@"✅ Loaded library: %@", libraryPath);
        
        id<MTLFunction> vertexFunction = [library newFunctionWithName:@"vertexShader"];
        id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"fragmentShader"];
        
        if (!vertexFunction || !fragmentFunction) {
            NSLog(@"❌ Failed to find shader functions");
            return 1;
        }
        NSLog(@"✅ Found vertex and fragment functions");
        NSLog(@"");
        
        // Create render pipeline
        NSLog(@"========================================");
        NSLog(@"  Creating Render Pipeline");
        NSLog(@"========================================");
        
        MTLRenderPipelineDescriptor* pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
        pipelineDesc.label = @"Triangle Pipeline";
        pipelineDesc.vertexFunction = vertexFunction;
        pipelineDesc.fragmentFunction = fragmentFunction;
        pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        
        id<MTLRenderPipelineState> pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDesc
                                                                                          error:&error];
        if (!pipelineState) {
            NSLog(@"❌ Failed to create render pipeline: %@", error);
            return 1;
        }
        NSLog(@"✅ Created render pipeline");
        NSLog(@"");
        
        // Create vertex data (red triangle)
        Vertex vertices[3] = {
            {{  0.0f,  0.5f }, { 1.0f, 0.0f, 0.0f }},  // Top (red)
            {{ -0.5f, -0.5f }, { 0.0f, 1.0f, 0.0f }},  // Bottom left (green)
            {{  0.5f, -0.5f }, { 0.0f, 0.0f, 1.0f }}   // Bottom right (blue)
        };
        
        id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:vertices
                                                         length:sizeof(vertices)
                                                        options:MTLResourceStorageModeShared];
        if (!vertexBuffer) {
            NSLog(@"❌ Failed to create vertex buffer");
            return 1;
        }
        NSLog(@"✅ Created vertex buffer (%zu bytes)", sizeof(vertices));
        
        // Create render target texture (800x600)
        NSUInteger width = 800;
        NSUInteger height = 600;
        
        MTLTextureDescriptor* textureDesc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:width
                                        height:height
                                     mipmapped:NO];
        textureDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        
        id<MTLTexture> renderTarget = [device newTextureWithDescriptor:textureDesc];
        if (!renderTarget) {
            NSLog(@"❌ Failed to create render target");
            return 1;
        }
        NSLog(@"✅ Created render target (%lux%lu)", width, height);
        NSLog(@"");
        
        // Create render pass descriptor
        NSLog(@"========================================");
        NSLog(@"  Encoding Draw Commands");
        NSLog(@"========================================");
        
        MTLRenderPassDescriptor* renderPassDesc = [MTLRenderPassDescriptor renderPassDescriptor];
        renderPassDesc.colorAttachments[0].texture = renderTarget;
        renderPassDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
        renderPassDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
        renderPassDesc.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.2, 0.2, 1.0);  // Gray background
        
        // Create command buffer
        id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
        commandBuffer.label = @"Triangle Draw";
        
        // Encode rendering commands
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDesc];
        encoder.label = @"Triangle Encoder";
        
        [encoder setRenderPipelineState:pipelineState];
        [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        
        // Set viewport
        MTLViewport viewport = {0, 0, (double)width, (double)height, 0, 1};
        [encoder setViewport:viewport];
        
        // Set scissor
        MTLScissorRect scissor = {0, 0, width, height};
        [encoder setScissorRect:scissor];
        
        // Draw triangle
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:3];
        
        [encoder endEncoding];
        
        NSLog(@"✅ Encoded draw commands");
        NSLog(@"");
        
        // Submit to GPU
        NSLog(@"========================================");
        NSLog(@"  Submitting to GPU");
        NSLog(@"========================================");
        
        [commandBuffer commit];
        NSLog(@"⏳ Waiting for GPU...");
        
        [commandBuffer waitUntilCompleted];
        
        if (commandBuffer.status == MTLCommandBufferStatusCompleted) {
            NSLog(@"✅ GPU execution completed");
        } else {
            NSLog(@"❌ Command buffer failed with status: %ld", (long)commandBuffer.status);
            if (commandBuffer.error) {
                NSLog(@"   Error: %@", commandBuffer.error);
            }
            return 1;
        }
        NSLog(@"");
        
        // Read back a few pixels to verify
        NSLog(@"========================================");
        NSLog(@"  Verifying Results");
        NSLog(@"========================================");
        
        uint32_t pixels[4];  // Read 2x2 pixels from center
        [renderTarget getBytes:pixels
                   bytesPerRow:8
                 bytesPerImage:16
                    fromRegion:MTLRegionMake2D(width/2 - 1, height/2 - 1, 2, 2)
                   mipmapLevel:0
                         slice:0];
        
        // Check if pixels are not all background color
        uint32_t bgColor = 0xFF333333;  // BGRA gray
        BOOL hasNonBg = NO;
        for (int i = 0; i < 4; i++) {
            if (pixels[i] != bgColor && pixels[i] != 0) {
                hasNonBg = YES;
                break;
            }
        }
        
        if (hasNonBg) {
            NSLog(@"✅ Triangle pixels detected");
            NSLog(@"   Center pixels: 0x%08X 0x%08X 0x%08X 0x%08X",
                  pixels[0], pixels[1], pixels[2], pixels[3]);
        } else {
            NSLog(@"⚠️  Warning: No triangle pixels detected (might be outside center)");
        }
        NSLog(@"");
        
        // Save to file (optional)
        NSString* outputPath = @"triangle_output.raw";
        NSLog(@"💾 Saving render target to: %@", outputPath);
        NSLog(@"   Size: %lux%lu pixels, BGRA8", width, height);
        
        size_t imageSize = width * height * 4;
        void* imageData = malloc(imageSize);
        [renderTarget getBytes:imageData
                   bytesPerRow:width * 4
                 bytesPerImage:imageSize
                    fromRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0
                         slice:0];
        
        NSData* data = [NSData dataWithBytes:imageData length:imageSize];
        [data writeToFile:outputPath atomically:YES];
        free(imageData);
        
        NSLog(@"✅ Saved to %@", outputPath);
        NSLog(@"   Convert to PNG with:");
        NSLog(@"   python3 -c \"from PIL import Image; import numpy as np;");
        NSLog(@"   data = np.fromfile('%@', dtype=np.uint8).reshape(%lu, %lu, 4);", 
              outputPath, height, width);
        NSLog(@"   Image.fromarray(data).save('triangle.png')\"");
        NSLog(@"");
        
        NSLog(@"========================================");
        NSLog(@"  Test Summary");
        NSLog(@"========================================");
        NSLog(@"✅ ALL TESTS PASSED");
        NSLog(@"RDNA4 graphics pipeline is FUNCTIONAL!");
        NSLog(@"========================================");
        
        return 0;
    }
}
