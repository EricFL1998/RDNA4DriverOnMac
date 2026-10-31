#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dlfcn.h>
#import "RDNA4UserClient.h"
#import "RDNA4Library.h"

static int fail(NSString *message) {
    fprintf(stderr, "FAIL: %s\n", message.UTF8String);
    return 1;
}

static int checkShader(id pipeline, NSString *stage) {
    uint64_t address = [[pipeline valueForKey:[stage stringByAppendingString:@"ShaderGPUAddress"]] unsignedLongLongValue];
    if (!address || (address & 255)) return fail([NSString stringWithFormat:@"%@ shader address is invalid: 0x%llx", stage, address]);
    id shader = [pipeline valueForKey:[stage stringByAppendingString:@"Shader"]];
    NSNumber *size = [shader valueForKey:@"machineCodeSize"];
    const void *code = ((RDNA4CompiledShader *)shader).machineCode;
    if (!code || size.unsignedLongLongValue == 0) return fail([NSString stringWithFormat:@"%@ shader is empty", stage]);
    typedef RDNA4GpuLink *(*AcquireFn)(void);
    typedef void *(*PointerFn)(RDNA4GpuLink *, uint64_t);
    AcquireFn acquire = (AcquireFn)dlsym(RTLD_DEFAULT, "RDNA4GpuLinkAcquire");
    PointerFn pointerForAddress = (PointerFn)dlsym(RTLD_DEFAULT, "RDNA4GpuPointerForAddress");
    if (!acquire || !pointerForAddress) return fail(@"bundle GPU mapping symbols are missing");
    void *mapped = pointerForAddress(acquire(), address);
    if (!mapped || memcmp(mapped, code, size.unsignedLongLongValue) != 0) {
        return fail([NSString stringWithFormat:@"%@ shader GPU upload differs from its .text bytes", stage]);
    }
    NSNumber *rsrc1 = [shader valueForKey:@"pgmRsrc1"];
    NSNumber *rsrc2 = [shader valueForKey:@"pgmRsrc2"];
    unsigned expectedRsrc2 = [stage isEqualToString:@"vertex"] ? 0x14u : 0u;
    if (rsrc1.unsignedIntValue == 0 || rsrc2.unsignedIntValue != expectedRsrc2) {
        return fail([NSString stringWithFormat:@"%@ graphics resource metadata is invalid", stage]);
    }
    printf("PASS: %s shader uploaded at 0x%llx, %llu bytes, RSRC1=0x%x RSRC2=0x%x\n",
           stage.UTF8String, address, size.unsignedLongLongValue, rsrc1.unsignedIntValue,
           rsrc2.unsignedIntValue);
    return 0;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 3) return fail(@"usage: RenderPipelineTest /path/to/bundle/binary /path/to/triangle.metal");
        if (!dlopen(argv[1], RTLD_NOW)) return fail([NSString stringWithUTF8String:dlerror()]);
        id<MTLDevice> device = [[NSClassFromString(@"RDNA4Device") alloc] init];
        if (!device) return fail(@"no RDNA4 device");
        NSError *error = nil;
        NSString *source = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[2]]
                                                     encoding:NSUTF8StringEncoding error:&error];
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) return fail([NSString stringWithFormat:@"library: %@", error]);
        MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"vertexShader"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"fragmentShader"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        descriptor.vertexDescriptor = [[MTLVertexDescriptor alloc] init];
        descriptor.vertexDescriptor.attributes[0].format = MTLVertexFormatFloat2;
        descriptor.vertexDescriptor.attributes[0].offset = 0;
        descriptor.vertexDescriptor.attributes[0].bufferIndex = 0;
        descriptor.vertexDescriptor.attributes[1].format = MTLVertexFormatFloat3;
        descriptor.vertexDescriptor.attributes[1].offset = 8;
        descriptor.vertexDescriptor.attributes[1].bufferIndex = 0;
        descriptor.vertexDescriptor.layouts[0].stride = 20;
        descriptor.vertexDescriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        descriptor.vertexDescriptor.layouts[0].stepRate = 1;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) return fail([NSString stringWithFormat:@"pipeline: %@", error]);
        int result = checkShader(pipeline, @"vertex");
        if (result) return result;
        result = checkShader(pipeline, @"fragment");
        if (result) return result;
        puts("PASS: real render pipeline owns both uploaded graphics shaders");
        return 0;
    }
}
