#import "RDNA4VertexDescriptor.h"

@implementation RDNA4VertexDescriptor {
    RDNA4VertexAttributeDescriptorArray* _attributes;
    RDNA4VertexBufferLayoutDescriptorArray* _layouts;
}

+ (MTLVertexDescriptor*)vertexDescriptor {
    return [[RDNA4VertexDescriptor alloc] init];
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _attributes = [[RDNA4VertexAttributeDescriptorArray alloc] init];
        _layouts = [[RDNA4VertexBufferLayoutDescriptorArray alloc] init];
    }
    return self;
}

- (MTLVertexAttributeDescriptorArray*)attributes {
    return _attributes;
}

- (MTLVertexBufferLayoutDescriptorArray*)layouts {
    return _layouts;
}

- (void)reset {
    // Reset all attributes
    for (NSUInteger i = 0; i < 31; i++) {
        RDNA4VertexAttributeDescriptor* attr = [_attributes objectAtIndexedSubscript:i];
        attr.format = MTLVertexFormatInvalid;
        attr.offset = 0;
        attr.bufferIndex = 0;
    }
    
    // Reset all layouts
    for (NSUInteger i = 0; i < 31; i++) {
        RDNA4VertexBufferLayoutDescriptor* layout = [_layouts objectAtIndexedSubscript:i];
        layout.stride = 0;
        layout.stepFunction = MTLVertexStepFunctionPerVertex;
        layout.stepRate = 1;
    }
}

- (id)copyWithZone:(NSZone*)zone {
    RDNA4VertexDescriptor* copy = [[RDNA4VertexDescriptor alloc] init];
    
    // Copy attributes
    for (NSUInteger i = 0; i < 31; i++) {
        RDNA4VertexAttributeDescriptor* src = [_attributes objectAtIndexedSubscript:i];
        if (src.format != MTLVertexFormatInvalid) {
            RDNA4VertexAttributeDescriptor* dst = [copy.attributes objectAtIndexedSubscript:i];
            dst.format = src.format;
            dst.offset = src.offset;
            dst.bufferIndex = src.bufferIndex;
        }
    }
    
    // Copy layouts
    for (NSUInteger i = 0; i < 31; i++) {
        RDNA4VertexBufferLayoutDescriptor* src = [_layouts objectAtIndexedSubscript:i];
        if (src.stride > 0) {
            RDNA4VertexBufferLayoutDescriptor* dst = [copy.layouts objectAtIndexedSubscript:i];
            dst.stride = src.stride;
            dst.stepFunction = src.stepFunction;
            dst.stepRate = src.stepRate;
        }
    }
    
    return copy;
}

@end

@implementation RDNA4VertexAttributeDescriptor

- (instancetype)init {
    self = [super init];
    if (self) {
        _format = MTLVertexFormatInvalid;
        _offset = 0;
        _bufferIndex = 0;
    }
    return self;
}

@end

@implementation RDNA4VertexBufferLayoutDescriptor

- (instancetype)init {
    self = [super init];
    if (self) {
        _stride = 0;
        _stepFunction = MTLVertexStepFunctionPerVertex;
        _stepRate = 1;
    }
    return self;
}

@end

@implementation RDNA4VertexAttributeDescriptorArray {
    RDNA4VertexAttributeDescriptor* _attributes[31];
}

- (instancetype)init {
    self = [super init];
    if (self) {
        for (NSUInteger i = 0; i < 31; i++) {
            _attributes[i] = [[RDNA4VertexAttributeDescriptor alloc] init];
        }
    }
    return self;
}

- (RDNA4VertexAttributeDescriptor*)objectAtIndexedSubscript:(NSUInteger)index {
    if (index >= 31) {
        return nil;
    }
    return _attributes[index];
}

- (void)setObject:(RDNA4VertexAttributeDescriptor*)attribute atIndexedSubscript:(NSUInteger)index {
    if (index < 31) {
        _attributes[index] = attribute;
    }
}

@end

@implementation RDNA4VertexBufferLayoutDescriptorArray {
    RDNA4VertexBufferLayoutDescriptor* _layouts[31];
}

- (instancetype)init {
    self = [super init];
    if (self) {
        for (NSUInteger i = 0; i < 31; i++) {
            _layouts[i] = [[RDNA4VertexBufferLayoutDescriptor alloc] init];
        }
    }
    return self;
}

- (RDNA4VertexBufferLayoutDescriptor*)objectAtIndexedSubscript:(NSUInteger)index {
    if (index >= 31) {
        return nil;
    }
    return _layouts[index];
}

- (void)setObject:(RDNA4VertexBufferLayoutDescriptor*)layout atIndexedSubscript:(NSUInteger)index {
    if (index < 31) {
        _layouts[index] = layout;
    }
}

@end
