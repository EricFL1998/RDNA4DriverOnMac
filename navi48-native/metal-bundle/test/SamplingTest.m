//
//  SamplingTest.m
//  RDNA4Metal bundle test — texture sampling path
//
//  Draws the same system vertex shader triangle as SystemShadersTest, but with
//  the real WindowServer textured pixel shader (SkyLight's SimpleTextureFragment)
//  bound to a 4x4 texture and a linear sampler. The pixel shader loads its T#
//  and S# from user data words 5 and 7 and issues image_sample; this is the
//  only in-repo test that exercises the texture unit end to end.
//
//  Regression this guards: the driver used to hard-code SPI_SHADER_PGM_RSRC2_PS
//  USER_SGPR_COUNT to 4, so the hardware never loaded s5/s7 for shaders with
//  resources. The descriptor loads then read stale registers and the resulting
//  bad address hung the queue (command buffer status 5).
//
//  Usage: SamplingTest <RDNA4Metal bundle binary> [metallib]
//

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dlfcn.h>

static int fail(NSString *message) {
    fprintf(stderr, "FAIL: %s\n", message.UTF8String);
    return 1;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 2) {
            return fail(@"usage: SamplingTest <bundle-binary> [metallib]");
        }
        void *bundleHandle = dlopen(argv[1], RTLD_NOW);
        if (!bundleHandle) {
            return fail([NSString stringWithUTF8String:dlerror()]);
        }
        NSString *metallib = argc > 2
            ? [NSString stringWithUTF8String:argv[2]]
            : @"/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/"
              @"Resources/SkyLightShaders.air64.metallib";

        id<MTLDevice> device = [[NSClassFromString(@"RDNA4Device") alloc] init];
        if (!device) return fail(@"no RDNA4 device");

        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithFile:metallib error:&error];
        if (!library) return fail([NSString stringWithFormat:@"library: %@", error]);

        id<MTLFunction> vs = [library newFunctionWithName:@"SimpleVertex"];
        id<MTLFunction> fs = [library newFunctionWithName:@"SimpleTextureFragment"];
        if (!vs || !fs) return fail(@"missing SimpleVertex/SimpleTextureFragment");

        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vs;
        descriptor.fragmentFunction = fs;
        MTLVertexDescriptor *vertices = [MTLVertexDescriptor new];
        vertices.attributes[0].format = MTLVertexFormatFloat2;
        vertices.attributes[0].offset = 0;
        vertices.attributes[0].bufferIndex = 0;
        vertices.attributes[1].format = MTLVertexFormatFloat2;
        vertices.attributes[1].offset = 8;
        vertices.attributes[1].bufferIndex = 0;
        vertices.layouts[0].stride = 16;
        vertices.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        vertices.layouts[0].stepRate = 1;
        descriptor.vertexDescriptor = vertices;
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

        id<MTLRenderPipelineState> pipeline =
            [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) return fail([NSString stringWithFormat:@"pipeline: %@", error]);

        static const float triangle[3][4] = {
            {-0.8f, -0.8f, 0.0f, 0.0f},
            { 0.8f, -0.8f, 1.0f, 0.0f},
            { 0.0f,  0.8f, 0.5f, 1.0f},
        };
        id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:triangle
                                                         length:sizeof(triangle)
                                                        options:MTLResourceStorageModeShared];
        static const float identity[16] = {
            1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
        };
        id<MTLBuffer> matrixBuffer = [device newBufferWithBytes:identity
                                                         length:sizeof(identity)
                                                        options:MTLResourceStorageModeShared];
        if (!vertexBuffer || !matrixBuffer) return fail(@"buffer allocation");

        MTLTextureDescriptor *textureDescriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:4 height:4 mipmapped:NO];
        textureDescriptor.usage = MTLTextureUsageShaderRead;
        textureDescriptor.storageMode = MTLResourceStorageModeShared;
        id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
        if (!texture) return fail(@"texture allocation");
        uint32_t red[16];
        for (int i = 0; i < 16; ++i) red[i] = 0xFF0000FFu; // BGRA: B=0xFF, G=0, R=0
        [texture replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0
                     withBytes:red bytesPerRow:16];

        MTLSamplerDescriptor *samplerDescriptor = [MTLSamplerDescriptor new];
        samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
        samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDescriptor];
        if (!sampler) return fail(@"sampler allocation");

        MTLTextureDescriptor *targetDescriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:64 height:64 mipmapped:NO];
        targetDescriptor.usage = MTLTextureUsageRenderTarget;
        targetDescriptor.storageMode = MTLResourceStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:targetDescriptor];
        if (!target) return fail(@"render target allocation");

        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        [encoder setVertexBuffer:matrixBuffer offset:0 atIndex:1];
        [encoder setFragmentTexture:texture atIndex:0];
        [encoder setFragmentSamplerState:sampler atIndex:0];
        [encoder setViewport:(MTLViewport){0, 0, 64, 64, 0, 1}];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
            return fail([NSString stringWithFormat:@"command buffer status %ld",
                                                   (long)commandBuffer.status]);
        }

        static uint32_t pixels[64 * 64];
        [target getBytes:pixels bytesPerRow:64 * 4 bytesPerImage:64 * 64 * 4
               fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 slice:0];
        int nonBlack = 0;
        for (int i = 0; i < 64 * 64; ++i) {
            if ((pixels[i] & 0x00FFFFFFu) != 0) ++nonBlack;
        }
        uint32_t center = pixels[32 * 64 + 32];
        printf("nonBlack=%d center=0x%08X\n", nonBlack, center);
        if (nonBlack == 0) return fail(@"all pixels black");

        puts("PASS: texture sampling rendered the triangle");
        return 0;
    }
}
