#import "RDNA4RenderPipelineState.h"
#import "RDNA4Device.h"
#import "RDNA4Function.h"
#import "RDNA4Library.h"
#import "RDNA4RenderPipelineDescriptor.h"
#import "RDNA4VertexDescriptor.h"
#import <IOKit/IOKitLib.h>

@implementation RDNA4RenderPipelineState {
    RDNA4Device* _device;
    RDNA4CompiledShader* _vertexShader;
    RDNA4CompiledShader* _fragmentShader;
    uint64_t _vertexShaderGPUAddress;
    uint64_t _fragmentShaderGPUAddress;
    
    MTLVertexDescriptor* _vertexDescriptor;
    MTLPixelFormat _colorAttachmentFormats[8];
    MTLPixelFormat _depthAttachmentFormat;
    MTLPixelFormat _stencilAttachmentFormat;
    NSUInteger _sampleCount;
    BOOL _alphaToCoverageEnabled;
    BOOL _alphaToOneEnabled;
    BOOL _rasterizationEnabled;
}

- (instancetype)initWithDevice:(RDNA4Device*)device
                    descriptor:(MTLRenderPipelineDescriptor*)descriptor
                         error:(NSError**)error {
    self = [super init];
    if (self) {
        _device = device;
        self.label = descriptor.label;
        
        // Validate descriptor
        if (!descriptor.vertexFunction) {
            if (error) {
                *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                             code:-1
                                         userInfo:@{NSLocalizedDescriptionKey: @"Vertex function is required"}];
            }
            return nil;
        }
        
        // Get compiled shaders
        RDNA4Function* vertexFunc = (RDNA4Function*)descriptor.vertexFunction;
        RDNA4Function* fragmentFunc = (RDNA4Function*)descriptor.fragmentFunction;
        
        _vertexShader = [vertexFunc compiledShader];
        if (fragmentFunc) {
            _fragmentShader = [fragmentFunc compiledShader];
        }
        
        if (!_vertexShader || !_vertexShader.machineCode) {
            if (error) {
                *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                             code:-2
                                         userInfo:@{NSLocalizedDescriptionKey: @"Invalid vertex shader"}];
            }
            return nil;
        }
        
        // Upload shaders to VRAM
        if (![self uploadShadersToVRAM:error]) {
            return nil;
        }
        
        // Store vertex descriptor
        _vertexDescriptor = [descriptor.vertexDescriptor copy];
        
        // Store color attachment formats
        for (NSUInteger i = 0; i < 8; i++) {
            RDNA4RenderPipelineColorAttachmentDescriptor* attachment = 
                [descriptor.colorAttachments objectAtIndexedSubscript:i];
            _colorAttachmentFormats[i] = attachment.pixelFormat;
        }
        
        // Store depth/stencil formats
        _depthAttachmentFormat = descriptor.depthAttachmentPixelFormat;
        _stencilAttachmentFormat = descriptor.stencilAttachmentPixelFormat;
        
        // Store other properties
        _sampleCount = descriptor.sampleCount;
        _alphaToCoverageEnabled = descriptor.alphaToCoverageEnabled;
        _alphaToOneEnabled = descriptor.alphaToOneEnabled;
        _rasterizationEnabled = descriptor.rasterizationEnabled;
        
        NSLog(@"RDNA4RenderPipelineState: Created pipeline '%@'", self.label);
        NSLog(@"  Vertex shader: GPU 0x%llx", _vertexShaderGPUAddress);
        if (_fragmentShaderGPUAddress) {
            NSLog(@"  Fragment shader: GPU 0x%llx", _fragmentShaderGPUAddress);
        }
    }
    return self;
}

- (BOOL)uploadShadersToVRAM:(NSError**)error {
    // Get IOUserClient connection
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                       IOServiceMatching("RDNA4Accel"));
    if (!service) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:-3
                                     userInfo:@{NSLocalizedDescriptionKey: @"RDNA4Accel service not found"}];
        }
        return NO;
    }
    
    io_connect_t connection;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    
    if (kr != KERN_SUCCESS) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:kr
                                     userInfo:@{NSLocalizedDescriptionKey: @"Failed to open IOUserClient"}];
        }
        return NO;
    }
    
    // Allocate VRAM for vertex shader
    size_t vsSize = (_vertexShader.machineCodeSize + 16383) & ~16383;  // 16KB aligned
    uint64_t outputs[2];
    uint32_t outputCount = 1;
    uint64_t input = vsSize;
    
    kr = IOConnectCallMethod(connection, 1,  // method_AllocateBuffer
                            &input, 1,
                            NULL, 0,
                            outputs, &outputCount,
                            NULL, NULL);
    
    if (kr != KERN_SUCCESS || outputs[0] == 0) {
        IOServiceClose(connection);
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:kr
                                     userInfo:@{NSLocalizedDescriptionKey: @"Failed to allocate vertex shader VRAM"}];
        }
        return NO;
    }
    
    _vertexShaderGPUAddress = outputs[0];
    
    // Upload vertex shader code
    uint64_t uploadInputs[1] = {_vertexShaderGPUAddress};
    kr = IOConnectCallMethod(connection, 5,  // method_UploadData
                            uploadInputs, 1,
                            _vertexShader.machineCode, _vertexShader.machineCodeSize,
                            NULL, NULL,
                            NULL, NULL);
    
    if (kr != KERN_SUCCESS) {
        IOServiceClose(connection);
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:kr
                                     userInfo:@{NSLocalizedDescriptionKey: @"Failed to upload vertex shader"}];
        }
        return NO;
    }
    
    // Allocate and upload fragment shader if present
    if (_fragmentShader && _fragmentShader.machineCode) {
        size_t fsSize = (_fragmentShader.machineCodeSize + 16383) & ~16383;
        input = fsSize;
        
        kr = IOConnectCallMethod(connection, 1,  // method_AllocateBuffer
                                &input, 1,
                                NULL, 0,
                                outputs, &outputCount,
                                NULL, NULL);
        
        if (kr != KERN_SUCCESS || outputs[0] == 0) {
            IOServiceClose(connection);
            if (error) {
                *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                             code:kr
                                         userInfo:@{NSLocalizedDescriptionKey: @"Failed to allocate fragment shader VRAM"}];
            }
            return NO;
        }
        
        _fragmentShaderGPUAddress = outputs[0];
        
        // Upload fragment shader code
        uploadInputs[0] = _fragmentShaderGPUAddress;
        kr = IOConnectCallMethod(connection, 5,  // method_UploadData
                                uploadInputs, 1,
                                _fragmentShader.machineCode, _fragmentShader.machineCodeSize,
                                NULL, NULL,
                                NULL, NULL);
        
        if (kr != KERN_SUCCESS) {
            IOServiceClose(connection);
            if (error) {
                *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                             code:kr
                                         userInfo:@{NSLocalizedDescriptionKey: @"Failed to upload fragment shader"}];
            }
            return NO;
        }
    }
    
    IOServiceClose(connection);
    return YES;
}

- (void)dealloc {
    // Free shader memory
    if (_vertexShaderGPUAddress != 0) {
        io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                           IOServiceMatching("RDNA4Accel"));
        if (service) {
            io_connect_t connection;
            if (IOServiceOpen(service, mach_task_self(), 0, &connection) == KERN_SUCCESS) {
                uint64_t input = _vertexShaderGPUAddress;
                IOConnectCallMethod(connection, 2, &input, 1, NULL, 0, NULL, NULL, NULL, NULL);
                
                if (_fragmentShaderGPUAddress != 0) {
                    input = _fragmentShaderGPUAddress;
                    IOConnectCallMethod(connection, 2, &input, 1, NULL, 0, NULL, NULL, NULL, NULL);
                }
                
                IOServiceClose(connection);
            }
            IOObjectRelease(service);
        }
    }
}

- (id<MTLDevice>)device {
    return _device;
}

- (uint64_t)vertexShaderGPUAddress {
    return _vertexShaderGPUAddress;
}

- (uint64_t)fragmentShaderGPUAddress {
    return _fragmentShaderGPUAddress;
}

- (RDNA4CompiledShader*)vertexShader {
    return _vertexShader;
}

- (RDNA4CompiledShader*)fragmentShader {
    return _fragmentShader;
}

- (MTLVertexDescriptor*)vertexDescriptor {
    return _vertexDescriptor;
}

- (MTLPixelFormat)colorAttachmentPixelFormatAtIndex:(NSUInteger)index {
    if (index >= 8) {
        return MTLPixelFormatInvalid;
    }
    return _colorAttachmentFormats[index];
}

- (MTLPixelFormat)depthAttachmentPixelFormat {
    return _depthAttachmentFormat;
}

- (MTLPixelFormat)stencilAttachmentPixelFormat {
    return _stencilAttachmentFormat;
}

- (NSUInteger)sampleCount {
    return _sampleCount;
}

- (BOOL)isAlphaToCoverageEnabled {
    return _alphaToCoverageEnabled;
}

- (BOOL)isAlphaToOneEnabled {
    return _alphaToOneEnabled;
}

- (BOOL)isRasterizationEnabled {
    return _rasterizationEnabled;
}

// MTLRenderPipelineState optional methods

- (NSUInteger)maxTotalThreadsPerThreadgroup {
    return 0;  // Not applicable for render pipeline
}

- (NSUInteger)threadExecutionWidth {
    return 0;  // Not applicable for render pipeline
}

- (NSUInteger)staticThreadgroupMemoryLength {
    return 0;  // Not applicable for render pipeline
}

- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)imageblockDimensions {
    return 0;
}

- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function stage:(MTLRenderStages)stage {
    return nil;  // Not yet supported
}

- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor*)descriptor stage:(MTLRenderStages)stage {
    return nil;  // Not yet supported
}

- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor*)descriptor stage:(MTLRenderStages)stage {
    return nil;  // Not yet supported
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithAdditionalBinaryFunctions:(NSArray<id<MTLFunction>>*)functions error:(NSError**)error {
    return nil;  // Not yet supported
}

@end
