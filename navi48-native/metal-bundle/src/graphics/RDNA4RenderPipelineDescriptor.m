#import "RDNA4RenderPipelineDescriptor.h"
#import "RDNA4VertexDescriptor.h"

@implementation RDNA4RenderPipelineDescriptor {
    RDNA4RenderPipelineColorAttachmentDescriptorArray* _colorAttachments;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _colorAttachments = [[RDNA4RenderPipelineColorAttachmentDescriptorArray alloc] init];
        _sampleCount = 1;
        _alphaToCoverageEnabled = NO;
        _alphaToOneEnabled = NO;
        _rasterizationEnabled = YES;
        _inputPrimitiveTopology = MTLPrimitiveTopologyClassUnspecified;
        _depthAttachmentPixelFormat = MTLPixelFormatInvalid;
        _stencilAttachmentPixelFormat = MTLPixelFormatInvalid;
    }
    return self;
}

- (MTLRenderPipelineColorAttachmentDescriptorArray*)colorAttachments {
    return _colorAttachments;
}

- (void)reset {
    self.label = nil;
    self.vertexFunction = nil;
    self.fragmentFunction = nil;
    self.vertexDescriptor = nil;
    
    // Reset color attachments
    for (NSUInteger i = 0; i < 8; i++) {
        RDNA4RenderPipelineColorAttachmentDescriptor* attachment = [_colorAttachments objectAtIndexedSubscript:i];
        attachment.pixelFormat = MTLPixelFormatInvalid;
        attachment.blendingEnabled = NO;
        attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
        attachment.destinationRGBBlendFactor = MTLBlendFactorZero;
        attachment.rgbBlendOperation = MTLBlendOperationAdd;
        attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
        attachment.destinationAlphaBlendFactor = MTLBlendFactorZero;
        attachment.alphaBlendOperation = MTLBlendOperationAdd;
        attachment.writeMask = MTLColorWriteMaskAll;
    }
    
    _depthAttachmentPixelFormat = MTLPixelFormatInvalid;
    _stencilAttachmentPixelFormat = MTLPixelFormatInvalid;
    _sampleCount = 1;
    _alphaToCoverageEnabled = NO;
    _alphaToOneEnabled = NO;
    _rasterizationEnabled = YES;
    _inputPrimitiveTopology = MTLPrimitiveTopologyClassUnspecified;
}

- (id)copyWithZone:(NSZone*)zone {
    RDNA4RenderPipelineDescriptor* copy = [[RDNA4RenderPipelineDescriptor alloc] init];
    
    copy.label = self.label;
    copy.vertexFunction = self.vertexFunction;
    copy.fragmentFunction = self.fragmentFunction;
    copy.vertexDescriptor = [self.vertexDescriptor copy];
    
    // Copy color attachments
    for (NSUInteger i = 0; i < 8; i++) {
        RDNA4RenderPipelineColorAttachmentDescriptor* src = [_colorAttachments objectAtIndexedSubscript:i];
        if (src.pixelFormat != MTLPixelFormatInvalid) {
            RDNA4RenderPipelineColorAttachmentDescriptor* dst = [copy.colorAttachments objectAtIndexedSubscript:i];
            dst.pixelFormat = src.pixelFormat;
            dst.blendingEnabled = src.blendingEnabled;
            dst.sourceRGBBlendFactor = src.sourceRGBBlendFactor;
            dst.destinationRGBBlendFactor = src.destinationRGBBlendFactor;
            dst.rgbBlendOperation = src.rgbBlendOperation;
            dst.sourceAlphaBlendFactor = src.sourceAlphaBlendFactor;
            dst.destinationAlphaBlendFactor = src.destinationAlphaBlendFactor;
            dst.alphaBlendOperation = src.alphaBlendOperation;
            dst.writeMask = src.writeMask;
        }
    }
    
    copy.depthAttachmentPixelFormat = self.depthAttachmentPixelFormat;
    copy.stencilAttachmentPixelFormat = self.stencilAttachmentPixelFormat;
    copy.sampleCount = self.sampleCount;
    copy.alphaToCoverageEnabled = self.alphaToCoverageEnabled;
    copy.alphaToOneEnabled = self.alphaToOneEnabled;
    copy.rasterizationEnabled = self.rasterizationEnabled;
    copy.inputPrimitiveTopology = self.inputPrimitiveTopology;
    
    return copy;
}

@end

@implementation RDNA4RenderPipelineColorAttachmentDescriptor

- (instancetype)init {
    self = [super init];
    if (self) {
        _pixelFormat = MTLPixelFormatInvalid;
        _blendingEnabled = NO;
        _sourceRGBBlendFactor = MTLBlendFactorOne;
        _destinationRGBBlendFactor = MTLBlendFactorZero;
        _rgbBlendOperation = MTLBlendOperationAdd;
        _sourceAlphaBlendFactor = MTLBlendFactorOne;
        _destinationAlphaBlendFactor = MTLBlendFactorZero;
        _alphaBlendOperation = MTLBlendOperationAdd;
        _writeMask = MTLColorWriteMaskAll;
    }
    return self;
}

@end

@implementation RDNA4RenderPipelineColorAttachmentDescriptorArray {
    RDNA4RenderPipelineColorAttachmentDescriptor* _attachments[8];
}

- (instancetype)init {
    self = [super init];
    if (self) {
        for (NSUInteger i = 0; i < 8; i++) {
            _attachments[i] = [[RDNA4RenderPipelineColorAttachmentDescriptor alloc] init];
        }
    }
    return self;
}

- (RDNA4RenderPipelineColorAttachmentDescriptor*)objectAtIndexedSubscript:(NSUInteger)attachmentIndex {
    if (attachmentIndex >= 8) {
        return nil;
    }
    return _attachments[attachmentIndex];
}

- (void)setObject:(RDNA4RenderPipelineColorAttachmentDescriptor*)attachment atIndexedSubscript:(NSUInteger)attachmentIndex {
    if (attachmentIndex < 8) {
        _attachments[attachmentIndex] = attachment;
    }
}

@end
