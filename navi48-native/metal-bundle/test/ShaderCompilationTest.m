#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dlfcn.h>

static int fail(NSString *message) {
    fprintf(stderr, "FAIL: %s\n", message.UTF8String);
    return 1;
}

static int checkFunction(id<MTLLibrary> library, NSString *name, MTLFunctionType expectedType) {
    id<MTLFunction> function = [library newFunctionWithName:name];
    if (!function) return fail([NSString stringWithFormat:@"%@ could not be extracted and compiled", name]);
    if (function.functionType != expectedType) {
        return fail([NSString stringWithFormat:@"%@ has function type %lu, expected %lu",
                     name, (unsigned long)function.functionType, (unsigned long)expectedType]);
    }
    id shader = [(id)function valueForKey:@"compiledShader"];
    NSNumber *codeSize = [shader valueForKey:@"machineCodeSize"];
    if (![codeSize isKindOfClass:[NSNumber class]] || codeSize.unsignedLongLongValue == 0) {
        return fail([NSString stringWithFormat:@"%@ has no GFX12 machine code", name]);
    }
    NSNumber *sgprCount = [shader valueForKey:@"sgprCount"];
    NSNumber *vgprCount = [shader valueForKey:@"vgprCount"];
    if (sgprCount.unsignedIntValue == 0 || vgprCount.unsignedIntValue == 0) {
        return fail([NSString stringWithFormat:@"%@ has no graphics register statistics", name]);
    }
    printf("PASS: %s type=%lu GFX12 bytes=%llu SGPR=%u VGPR=%u\n", name.UTF8String,
           (unsigned long)function.functionType, codeSize.unsignedLongLongValue,
           sgprCount.unsignedIntValue, vgprCount.unsignedIntValue);
    return 0;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 3) return fail(@"usage: ShaderCompilationTest /path/to/bundle/binary /path/to/triangle.metal");
        void *handle = dlopen(argv[1], RTLD_NOW);
        if (!handle) return fail([NSString stringWithUTF8String:dlerror()]);
        NSError *error = nil;
        NSString *source = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[2]]
                                                     encoding:NSUTF8StringEncoding error:&error];
        if (!source) return fail([NSString stringWithFormat:@"source: %@", error]);
        Class deviceClass = NSClassFromString(@"RDNA4Device");
        id<MTLDevice> device = [[deviceClass alloc] init];
        if (!device) return fail(@"no RDNA4 Metal device");
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) return fail([NSString stringWithFormat:@"library: %@", error]);
        if (![library isKindOfClass:NSClassFromString(@"RDNA4Library")]) {
            return fail(@"source compilation did not produce an RDNA4Library");
        }
        if (![library.functionNames containsObject:@"vertexShader"] ||
            ![library.functionNames containsObject:@"fragmentShader"]) {
            return fail([NSString stringWithFormat:@"missing function names: %@", library.functionNames]);
        }
        int result = checkFunction(library, @"vertexShader", MTLFunctionTypeVertex);
        if (result) return result;
        result = checkFunction(library, @"fragmentShader", MTLFunctionTypeFragment);
        if (result) return result;
        puts("PASS: real Metal source produced both typed GFX12 shaders");
        return 0;
    }
}
