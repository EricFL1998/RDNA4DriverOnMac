#import "RDNA4Function.h"
#import "RDNA4Device.h"
#import "RDNA4Library.h"

@implementation RDNA4Function {
    RDNA4Device* _device;
    RDNA4CompiledShader* _compiledShader;
    NSString* _name;
    MTLFunctionType _functionType;
}

- (instancetype)initWithDevice:(RDNA4Device*)device
                compiledShader:(RDNA4CompiledShader*)compiledShader
                          name:(NSString*)name {
    self = [super init];
    if (self) {
        _device = device;
        _compiledShader = compiledShader;
        _name = [name copy];
        
        // Determine function type from name or metadata
        // For now, assume all functions are compute kernels
        _functionType = MTLFunctionTypeKernel;
    }
    return self;
}

- (id<MTLDevice>)device {
    return _device;
}

- (MTLFunctionType)functionType {
    return _functionType;
}

- (NSString*)name {
    return _name;
}

- (NSArray<MTLAttribute*>*)vertexAttributes {
    // Not applicable for compute shaders
    return @[];
}

- (NSArray<MTLAttribute*>*)stageInputAttributes {
    // Not applicable for compute shaders
    return @[];
}

- (RDNA4CompiledShader*)compiledShader {
    return _compiledShader;
}

// MTLFunction optional methods
- (MTLFunctionOptions)options {
    return 0;
}

- (NSDictionary<NSString*, MTLArgument*>*)arguments {
    // TODO: Parse shader metadata to extract argument information
    return @{};
}

@end
