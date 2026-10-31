#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#import "../src/graphics/RDNA4RenderCommandEncoder.h"
#import "../src/graphics/RDNA4Texture.h"
#import "../src/graphics/RDNA4RenderPipelineState.h"
#import "../src/RDNA4Buffer.h"

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
        // Bisect hook: stop after shader compilation. A hang here is host-side
        // (LLVM/plugin), a hang after it is the GPU submission.
        if ([NSProcessInfo processInfo].environment[@"N48_COMPILE_ONLY"]) {
            NSLog(@"N48_COMPILE_ONLY: stopping before any pipeline or submission");
            return 0;
        }
        // Create render pipeline
        NSLog(@"========================================");
        NSLog(@"  Creating Render Pipeline");
        NSLog(@"========================================");
        
        MTLRenderPipelineDescriptor* pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
        pipelineDesc.label = @"Triangle Pipeline";
        pipelineDesc.vertexFunction = vertexFunction;
        pipelineDesc.fragmentFunction = fragmentFunction;
        pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pipelineDesc.vertexDescriptor = [[MTLVertexDescriptor alloc] init];
        pipelineDesc.vertexDescriptor.attributes[0].format = MTLVertexFormatFloat2;
        pipelineDesc.vertexDescriptor.attributes[0].offset = 0;
        pipelineDesc.vertexDescriptor.attributes[0].bufferIndex = 0;
        pipelineDesc.vertexDescriptor.attributes[1].format = MTLVertexFormatFloat3;
        pipelineDesc.vertexDescriptor.attributes[1].offset = 8;
        pipelineDesc.vertexDescriptor.attributes[1].bufferIndex = 0;
        pipelineDesc.vertexDescriptor.layouts[0].stride = 20;
        pipelineDesc.vertexDescriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        pipelineDesc.vertexDescriptor.layouts[0].stepRate = 1;
        
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
        if ([NSProcessInfo processInfo].environment[@"N48_REVERSE_WINDING"]) {
            Vertex tmp = vertices[1];
            vertices[1] = vertices[2];
            vertices[2] = tmp;
            NSLog(@"⚠️ Using REVERSED winding (0, 2, 1)");
        }
        if ([NSProcessInfo processInfo].environment[@"N48_GIANT_TRI"]) {
            vertices[0].position[0] = -2.0f; vertices[0].position[1] = -2.0f;
            vertices[1].position[0] =  4.0f; vertices[1].position[1] = -2.0f;
            vertices[2].position[0] = -2.0f; vertices[2].position[1] =  4.0f;
            NSLog(@"⚠️ Using GIANT fullscreen triangle");
        }
        
        id<MTLBuffer> vertexBuffer = [device newBufferWithLength:256
                                                        options:MTLResourceStorageModeShared];
        memset(vertexBuffer.contents, 0, 256);
        memcpy(vertexBuffer.contents, vertices, sizeof(vertices));
        if (!vertexBuffer) {
            NSLog(@"❌ Failed to create vertex buffer");
            return 1;
        }
        NSLog(@"✅ Created vertex buffer (256 bytes, GPU=0x%llx)",
              (unsigned long long)[(RDNA4Buffer*)vertexBuffer gpuAddress]);
        
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
        if ([(RDNA4Texture*)renderTarget bytesPerRowForLevel:0] != width * 4) {
            NSLog(@"❌ Render target pitch does not match GFX12's width-derived pitch");
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
        if (!encoder) {
            NSLog(@"❌ Failed to create render command encoder");
            return 1;
        }
        encoder.label = @"Triangle Encoder";
        
        [encoder setRenderPipelineState:pipelineState];
        [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        
        if ([NSProcessInfo processInfo].environment[@"N48_DUMP_RING"]) {
            [(RDNA4RenderPipelineState*)pipelineState clearAttributeRing];
        }
        
        // Set viewport
        MTLViewport viewport = {0, 0, (double)width, (double)height, 0, 1};
        [encoder setViewport:viewport];
        
        // Set scissor
        MTLScissorRect scissor = {0, 0, width, height};
        [encoder setScissorRect:scissor];
        
        if (![NSProcessInfo processInfo].environment[@"N48_SKIP_DRAW"]) {
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:3];
        }
        
        [encoder endEncoding];
        NSString* pm4DumpPath = [NSProcessInfo processInfo].environment[@"N48_TRIANGLE_PM4"];
        if (pm4DumpPath) {
            [[(RDNA4RenderCommandEncoder*)encoder getEncodedCommands] writeToFile:pm4DumpPath atomically:YES];
        }
        
        NSLog(@"✅ Encoded draw commands");
        NSLog(@"");
        // Bisect hook: encode (which exercises every register write and the PM4
        // builder) but never submit, so a wedged GPU cannot be hit at all.
        if ([NSProcessInfo processInfo].environment[@"N48_NO_SUBMIT"]) {
            NSLog(@"N48_NO_SUBMIT: not committing the command buffer");
            return 0;
        }
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
        
        if ([NSProcessInfo processInfo].environment[@"N48_DUMP_RING"]) {
            NSData* ring = [(RDNA4RenderPipelineState*)pipelineState
                attributeRingContentsOfLength:64];
            const uint32_t* words = (const uint32_t*)ring.bytes;
            (void)words;
            NSLog(@"attribute ring base=%p dwords=%lu", (void*)ring.bytes,
                  (unsigned long)(ring.length / 4));
            NSLog(@"  ring GPU=0x%llx size=%u",
                  (unsigned long long)[(RDNA4RenderPipelineState*)pipelineState attributeRingGPUAddress],
                  [(RDNA4RenderPipelineState*)pipelineState attributeRingSize]);
            NSData* scan = [(RDNA4RenderPipelineState*)pipelineState
                attributeRingContentsOfLength:512 * 1024];
            const uint32_t* dense = (const uint32_t*)scan.bytes;
            NSUInteger total = scan.length / 4;
            NSUInteger nonZero = 0;
            for (NSUInteger i = 0; i < total; ++i) if (dense[i]) ++nonZero;
            NSLog(@"ring scan: %lu/%lu dwords non-zero", (unsigned long)nonZero,
                  (unsigned long)total);
            // The NGG position and primitive rings live at fixed offsets inside
            // the same allocation; scanning them shows whether the GE consumed
            // the exports and wrote primitives at all.
            struct { const char* name; NSUInteger offset; NSUInteger length; } rings[] = {
                { "attr", 0x0,       0x580000 },
                { "pos",  0x580000,  0x400000 },
                { "prim", 0x980000,  0x100000 },
            };
            for (size_t r = 0; r < sizeof(rings) / sizeof(rings[0]); ++r) {
                NSData* data = [(RDNA4RenderPipelineState*)pipelineState
                    ringContentsAtOffset:rings[r].offset length:rings[r].length];
                if (!data) { NSLog(@"ring %s: not mapped", rings[r].name); continue; }
                const uint32_t* w = (const uint32_t*)data.bytes;
                NSUInteger n = data.length / 4, nz = 0, first = NSNotFound;
                for (NSUInteger i = 0; i < n; ++i) {
                    if (!w[i]) continue;
                    ++nz;
                    if (first == NSNotFound) first = i;
                }
                NSLog(@"ring %-4s: %lu/%lu dwords non-zero (first=%ld)",
                      rings[r].name, (unsigned long)nz, (unsigned long)n, (long)first);
                if (nz) {
                    NSUInteger shown = 0;
                    for (NSUInteger i = 0; i < n && shown < 6; ++i) {
                        if (!w[i]) continue;
                        NSLog(@"   %s[%lu] = %08x", rings[r].name, (unsigned long)i, w[i]);
                        ++shown;
                    }
                }
            }
            const uint32_t* vb = (const uint32_t*)vertexBuffer.contents;
            for (int k = 0; k < 40; k += 4) {
                NSLog(@"vb[%02d..%02d] = %08x %08x %08x %08x", k, k+3, vb[k], vb[k+1], vb[k+2], vb[k+3]);
            }
            NSUInteger shown = 0;
            for (NSUInteger i = 0; i < total && shown < 8; ++i) {
                if (!dense[i]) continue;
                NSLog(@"  nonzero ring[%lu] = %08x", (unsigned long)i, dense[i]);
                ++shown;
            }
        }
        NSLog(@"========================================");
        NSLog(@"  Verifying Results");
        NSLog(@"========================================");
        size_t imageSize = width * height * 4;
        uint32_t* pixels = (uint32_t*)malloc(imageSize);
        [renderTarget getBytes:pixels
                   bytesPerRow:width * 4
                 bytesPerImage:imageSize
                    fromRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0
                         slice:0];
        NSUInteger insideCount = 0;
        NSUInteger renderedCount = 0;
        NSUInteger mismatches = 0;
        const float area = -120000.0f;
        for (NSUInteger y = 0; y < height; ++y) {
            for (NSUInteger x = 0; x < width; ++x) {
                const uint32_t actual = pixels[y * width + x];
                if (actual != 0xFF333333u) {
                    ++renderedCount;
                    if (renderedCount <= 10) {
                        NSLog(@"  rendered pixel at (%lu, %lu) = 0x%08X (R=%u G=%u B=%u A=%u)",
                              (unsigned long)x, (unsigned long)y, actual,
                              (actual) & 0xFF, (actual >> 8) & 0xFF,
                              (actual >> 16) & 0xFF, (actual >> 24) & 0xFF);
                    }
                }
                const float px = x + 0.5f;
                const float py = y + 0.5f;
                const float w0 = ((200.0f - px) * (450.0f - py) - (450.0f - py) * (600.0f - px)) / area;
                const float w1 = ((600.0f - px) * (150.0f - py) - (450.0f - py) * (400.0f - px)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (fabsf(w0) < 0.01f || fabsf(w1) < 0.01f || fabsf(w2) < 0.01f) continue;
                const BOOL inside = (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f);
                if (!inside) {
                    if (actual != 0xFF333333u) ++mismatches;
                    continue;
                }
                ++insideCount;
                const int expected[4] = {
                    (int)(w0 * 255.0f + 0.5f), (int)(w1 * 255.0f + 0.5f),
                    (int)(w2 * 255.0f + 0.5f), 255,
                };
                const uint8_t* channels = (const uint8_t*)&actual;
                for (int channel = 0; channel < 4; ++channel) {
                    int difference = channels[channel] - expected[channel];
                    if (difference < 0) difference = -difference;
                    if (difference > 10) {
                        ++mismatches;
                        break;
                    }
                }
            }
        }
        const uint32_t center = pixels[(height / 2) * width + width / 2];
        NSLog(@"Rendered non-clear pixels: %lu, Triangle inside pixels: %lu, mismatches: %lu, center=0x%08X",
              (unsigned long)renderedCount, (unsigned long)insideCount, (unsigned long)mismatches, center);
        NSData* output = [NSData dataWithBytes:pixels length:imageSize];
        [output writeToFile:@"triangle_output.raw" atomically:YES];
        free(pixels);
        if (renderedCount < 1000) {
            NSLog(@"❌ GPU did not render triangle pixels (rendered=%lu)", (unsigned long)renderedCount);
            return 1;
        }
        NSLog(@"✅ GPU produced the expected triangle and interpolated colors");
        return 0;
    }
}
