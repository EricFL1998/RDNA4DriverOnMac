#import "RDNA4RenderPassDescriptor.h"

@implementation RDNA4RenderPassDescriptor {
    RDNA4RenderPassColorAttachmentDescriptorArray* _colorAttachments;
    RDNA4RenderPassDepthAttachmentDescriptor* _depthAttachment;
    RDNA4RenderPassStencilAttachmentDescriptor* _stencilAttachment;
}

+ (MTLRenderPassDescriptor*)renderPassDescriptor {
    return [[RDNA4RenderPassDescriptor alloc] init];
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _colorAttachments = [[RDNA4RenderPassColorAttachmentDescriptorArray alloc] init];
        _depthAttachment = [[RDNA4RenderPassDepthAttachmentDescriptor alloc] init];
        _stencilAttachment = [[RDNA4RenderPassStencilAttachmentDescriptor alloc] init];
        _renderTargetArrayLength = 0;
    }
    return self;
}

- (MTLRenderPassColorAttachmentDescriptorArray*)colorAttachments {
    return _colorAttachments;
}

- (MTLRenderPassDepthAttachmentDescriptor*)depthAttachment {
    return _depthAttachment;
}

- (MTLRenderPassStencilAttachmentDescriptor*)stencilAttachment {
    return _stencilAttachment;
}

- (id)copyWithZone:(NSZone*)zone {
    RDNA4RenderPassDescriptor* copy = [[RDNA4RenderPassDescriptor alloc] init];
    
    // Copy color attachments
    for (NSUInteger i = 0; i < 8; i++) {
        RDNA4RenderPassColorAttachmentDescriptor* src = [_colorAttachments objectAtIndexedSubscript:i];
        if (src.texture) {
            RDNA4RenderPassColorAttachmentDescriptor* dst = [copy.colorAttachments objectAtIndexedSubscript:i];
            dst.texture = src.texture;
            dst.level = src.level;
            dst.slice = src.slice;
            dst.depthPlane = src.depthPlane;
            dst.resolveTexture = src.resolveTexture;
            dst.resolveLevel = src.resolveLevel;
            dst.resolveSlice = src.resolveSlice;
            dst.resolveDepthPlane = src.resolveDepthPlane;
            dst.loadAction = src.loadAction;
            dst.storeAction = src.storeAction;
            dst.clearColor = src.clearColor;
        }
    }
    
    // Copy depth attachment
    if (_depthAttachment.texture) {
        copy.depthAttachment.texture = _depthAttachment.texture;
        copy.depthAttachment.level = _depthAttachment.level;
        copy.depthAttachment.slice = _depthAttachment.slice;
        copy.depthAttachment.loadAction = _depthAttachment.loadAction;
        copy.depthAttachment.storeAction = _depthAttachment.storeAction;
        copy.depthAttachment.clearDepth = _depthAttachment.clearDepth;
    }
    
    // Copy stencil attachment
    if (_stencilAttachment.texture) {
        copy.stencilAttachment.texture = _stencilAttachment.texture;
        copy.stencilAttachment.level = _stencilAttachment.level;
        copy.stencilAttachment.slice = _stencilAttachment.slice;
        copy.stencilAttachment.loadAction = _stencilAttachment.loadAction;
        copy.stencilAttachment.storeAction = _stencilAttachment.storeAction;
        copy.stencilAttachment.clearStencil = _stencilAttachment.clearStencil;
    }
    
    copy.visibilityResultBuffer = _visibilityResultBuffer;
    copy.renderTargetArrayLength = _renderTargetArrayLength;
    
    return copy;
}

@end

@implementation RDNA4RenderPassColorAttachmentDescriptor

- (instancetype)init {
    self = [super init];
    if (self) {
        _level = 0;
        _slice = 0;
        _depthPlane = 0;
        _resolveLevel = 0;
        _resolveSlice = 0;
        _resolveDepthPlane = 0;
        _loadAction = MTLLoadActionDontCare;
        _storeAction = MTLStoreActionDontCare;
        _clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    }
    return self;
}

@end

@implementation RDNA4RenderPassDepthAttachmentDescriptor

- (instancetype)init {
    self = [super init];
    if (self) {
        _level = 0;
        _slice = 0;
        _depthPlane = 0;
        _resolveLevel = 0;
        _resolveSlice = 0;
        _resolveDepthPlane = 0;
        _loadAction = MTLLoadActionDontCare;
        _storeAction = MTLStoreActionDontCare;
        _clearDepth = 1.0;
        _depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
    }
    return self;
}

@end

@implementation RDNA4RenderPassStencilAttachmentDescriptor

- (instancetype)init {
    self = [super init];
    if (self) {
        _level = 0;
        _slice = 0;
        _depthPlane = 0;
        _resolveLevel = 0;
        _resolveSlice = 0;
        _resolveDepthPlane = 0;
        _loadAction = MTLLoadActionDontCare;
        _storeAction = MTLStoreActionDontCare;
        _clearStencil = 0;
        _stencilResolveFilter = MTLMultisampleStencilResolveFilterSample0;
    }
    return self;
}

@end

@implementation RDNA4RenderPassColorAttachmentDescriptorArray {
    RDNA4RenderPassColorAttachmentDescriptor* _attachments[8];
}

- (instancetype)init {
    self = [super init];
    if (self) {
        for (NSUInteger i = 0; i < 8; i++) {
            _attachments[i] = [[RDNA4RenderPassColorAttachmentDescriptor alloc] init];
        }
    }
    return self;
}

- (RDNA4RenderPassColorAttachmentDescriptor*)objectAtIndexedSubscript:(NSUInteger)attachmentIndex {
    if (attachmentIndex >= 8) {
        return nil;
    }
    return _attachments[attachmentIndex];
}

- (void)setObject:(RDNA4RenderPassColorAttachmentDescriptor*)attachment atIndexedSubscript:(NSUInteger)attachmentIndex {
    if (attachmentIndex < 8) {
        _attachments[attachmentIndex] = attachment;
    }
}

@end
