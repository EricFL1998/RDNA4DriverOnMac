//
//  SystemShadersTest.m
//  RDNA4Metal bundle test — system shader path
//
//  Draws a triangle with the real WindowServer vertex and fragment shaders
//  (SkyLight's SimpleVertex/SimpleColorFragment), a 64x64 render target and an
//  identity MVP bound as a vertex-stage buffer. This is the closest in-repo
//  equivalent of what a Metal application does when it draws into an offscreen
//  texture: the vertex shader fetches attributes, reads a constant buffer
//  through its user SGPRs and writes the varying to the attribute ring, and the
//  pixel shader interpolates it with ds_param_load + v_interp.
//
//  Regression this guards: PA_SC_WINDOW_SCISSOR_BR used to be left at its reset
//  value, so any app that never called setScissorRect had every fragment
//  clipped away and the target stayed at the clear colour.
//
//  Usage: SystemShadersTest <RDNA4Metal bundle binary> [metallib]
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
            return fail(@"usage: SystemShadersTest <bundle-binary> [metallib]");
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
        id<MTLFunction> fs = [library newFunctionWithName:@"SimpleColorFragment"];
        if (!vs || !fs) return fail(@"missing SimpleVertex/SimpleColorFragment");

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

        puts("PASS: system vertex+fragment shaders rendered the triangle");
        return 0;
    }
}
