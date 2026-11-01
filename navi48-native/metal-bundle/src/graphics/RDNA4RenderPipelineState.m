#import "RDNA4RenderPipelineState.h"
#import "RDNA4Device.h"
#import "RDNA4Function.h"
#import "RDNA4Library.h"
#import "RDNA4RenderPipelineDescriptor.h"
#import "RDNA4VertexDescriptor.h"
#import "RDNA4Buffer.h"
#import "RDNA4UserClient.h"

@implementation RDNA4RenderPipelineState {
    RDNA4Device* _device;
    RDNA4CompiledShader* _vertexShader;
    RDNA4CompiledShader* _fragmentShader;
    NSData* _vertexBindings;
    NSData* _fragmentBindings;
    RDNA4Buffer* _vertexShaderBuffer;
    RDNA4Buffer* _fragmentShaderBuffer;
    RDNA4Buffer* _attributeRingBuffer;
    uint64_t _vertexShaderGPUAddress;
    uint64_t _fragmentShaderGPUAddress;
    uint64_t _attributeRingGPUAddress;
    uint32_t _attributeRingSize;
    
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
        _vertexBindings = _vertexShader.bindings;
        if (fragmentFunc) {
            _fragmentShader = [fragmentFunc compiledShader];
            _fragmentBindings = _fragmentShader.bindings;
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
        if (_vertexShader.vertexParameterCount > 0) {
            // One NGG ring per device, shared by every pipeline. Allocating it
            // per pipeline would consume 10.5 MiB of the 32 MiB client window
            // and exhaust it after three pipelines.
            uint32_t ringSize = 0;
            uint64_t ringBase = 0;
            RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
            if (!getenv("N48_RING_IN_DRIVER_VRAM")) {
                ringBase = [(RDNA4Device*)_device sharedNGGRingAddress:&ringSize];
            }
            if (!ringBase && link) {
                ringBase = RDNA4GpuNggRing(link, &ringSize);
            }
            if (!ringBase) {
                if (error) {
                    *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                                 code:-5
                                             userInfo:@{NSLocalizedDescriptionKey: @"The driver's NGG ring is not available"}];
                }
                return nil;
            }
            _attributeRingGPUAddress = ringBase;
            _attributeRingSize = 0x160000;   // GFX12::kAttributeRingPerSE
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
    _vertexShaderBuffer = (RDNA4Buffer*)[_device newBufferWithBytes:_vertexShader.machineCode
                                                              length:_vertexShader.machineCodeSize
                                                             options:MTLResourceStorageModeShared];
    if (!_vertexShaderBuffer) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:-3
                                     userInfo:@{NSLocalizedDescriptionKey: @"Failed to upload vertex shader"}];
        }
        return NO;
    }
    _vertexShaderGPUAddress = _vertexShaderBuffer.gpuAddress;
    if (_fragmentShader && _fragmentShader.machineCode) {
        _fragmentShaderBuffer = (RDNA4Buffer*)[_device newBufferWithBytes:_fragmentShader.machineCode
                                                                    length:_fragmentShader.machineCodeSize
                                                                   options:MTLResourceStorageModeShared];
        if (!_fragmentShaderBuffer) {
            if (error) {
                *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                             code:-4
                                         userInfo:@{NSLocalizedDescriptionKey: @"Failed to upload fragment shader"}];
            }
            return NO;
        }
        _fragmentShaderGPUAddress = _fragmentShaderBuffer.gpuAddress;
    }
    return YES;
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

- (NSData*)vertexBindings {
    return _vertexBindings;
}

- (NSData*)fragmentBindings {
    return _fragmentBindings;
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

- (NSData*)attributeRingContentsOfLength:(NSUInteger)length {
    if (!_attributeRingGPUAddress) return nil;
    RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
    if (!link) return nil;
    void* ptr = RDNA4GpuPointerForAddress(link, _attributeRingGPUAddress);
    if (!ptr) return nil;
    uint32_t ringSize = _attributeRingSize ? _attributeRingSize : 0x160000;
    if (length > ringSize) length = ringSize;
    return [NSData dataWithBytes:ptr length:length];
}

- (NSData*)ringContentsAtOffset:(NSUInteger)offset length:(NSUInteger)length {
    if (!_attributeRingGPUAddress) return nil;
    const NSUInteger total = 0xA80000u;
    if (offset >= total) return nil;
    if (length > total - offset) length = total - offset;
    RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
    if (!link) return nil;
    void* ptr = RDNA4GpuPointerForAddress(link, _attributeRingGPUAddress + offset);
    if (!ptr) return nil;
    return [NSData dataWithBytes:ptr length:length];
}

- (void)clearAttributeRing {
    if (!_attributeRingGPUAddress) return;
    RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
    if (!link) return;
    void* ptr = RDNA4GpuPointerForAddress(link, _attributeRingGPUAddress);
    if (!ptr) return;
    uint32_t ringSize = _attributeRingSize ? _attributeRingSize : 0x160000;
    memset(ptr, 0, ringSize);
}

@end
