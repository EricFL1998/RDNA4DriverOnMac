//
//  RDNA4RenderPM4Builder.mm
//  RDNA4Metal - PM4 encoding for the render path
//

#import "RDNA4RenderPM4Builder.h"
#import "RDNA4Texture.h"

#include "GraphicsPM4Builder.hpp"

namespace {

uint32_t cbFormatFor(MTLPixelFormat format) {
    switch (format) {
        case MTLPixelFormatR8Unorm:            return GFX12::CB_FORMAT_8_UNORM;
        case MTLPixelFormatRG8Unorm:           return GFX12::CB_FORMAT_8_8_UNORM;
        case MTLPixelFormatRGBA8Unorm:
        case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatRGBA8Unorm_sRGB:
        case MTLPixelFormatBGRA8Unorm_sRGB:    return GFX12::CB_FORMAT_8_8_8_8_UNORM;
        case MTLPixelFormatR16Float:           return GFX12::CB_FORMAT_16_FLOAT;
        case MTLPixelFormatR32Float:           return GFX12::CB_FORMAT_32_FLOAT;
        default:                               return GFX12::CB_FORMAT_8_8_8_8_UNORM;
    }
}

uint32_t diPrimTypeFor(MTLPrimitiveType type) {
    switch (type) {
        case MTLPrimitiveTypePoint:         return GFX12::DI_PT_POINTLIST;
        case MTLPrimitiveTypeLine:          return GFX12::DI_PT_LINELIST;
        case MTLPrimitiveTypeLineStrip:     return GFX12::DI_PT_LINESTRIP;
        case MTLPrimitiveTypeTriangle:      return GFX12::DI_PT_TRILIST;
        case MTLPrimitiveTypeTriangleStrip: return GFX12::DI_PT_TRISTRIP;
    }
    return GFX12::DI_PT_TRILIST;
}

} // namespace

@implementation RDNA4RenderPM4Builder {
    GraphicsPM4Builder _builder;
    BOOL _rejected;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _builder.reset();
        _rejected = NO;
        _unsupportedVertexBuffers = 0;
    }
    return self;
}

- (void)reset {
    _builder.reset();
    _rejected = NO;
    _unsupportedVertexBuffers = 0;
}

- (void)setViewport:(MTLViewport)viewport {
    if (!_builder.setViewport(viewport.originX, viewport.originY,
                              viewport.width, viewport.height,
                              viewport.znear, viewport.zfar)) {
        _rejected = YES;
    }
}

- (void)setScissor:(MTLScissorRect)rect {
    if (!_builder.setScissorRect((uint32_t)rect.x, (uint32_t)rect.y,
                                 (uint32_t)rect.width, (uint32_t)rect.height)) {
        _rejected = YES;
    }
}

- (void)setVertexShader:(uint64_t)gpuAddress rsrc1:(uint32_t)rsrc1 rsrc2:(uint32_t)rsrc2 {
    // gfx12 has no separate vertex stage: the VS runs as the NGG ES/GS pair.
    if (!_builder.setVertexShader(gpuAddress, rsrc1, rsrc2)) _rejected = YES;
}

- (void)setFragmentShader:(uint64_t)gpuAddress rsrc1:(uint32_t)rsrc1 rsrc2:(uint32_t)rsrc2 {
    if (!_builder.setPixelShader(gpuAddress, rsrc1, rsrc2)) _rejected = YES;
}

- (void)setVertexBuffer:(NSUInteger)index address:(uint64_t)addr stride:(uint32_t)stride count:(uint32_t)count {
    (void)index; (void)addr; (void)stride; (void)count;
    _unsupportedVertexBuffers++;
}

- (void)setColorBuffer:(NSUInteger)index texture:(RDNA4Texture*)texture {
    if (!texture) return;
    if (!_builder.setColorAttachment((uint32_t)index, [texture gpuAddress],
                                     (uint32_t)[texture width], (uint32_t)[texture height],
                                     cbFormatFor([texture pixelFormat]),
                                     (uint32_t)[texture sampleCount])) {
        _rejected = YES;
    }
}

- (void)setCullMode:(MTLCullMode)cullMode winding:(MTLWinding)winding {
    const uint32_t cullFront = (cullMode == MTLCullModeFront) ? 1u : 0u;
    const uint32_t cullBack = (cullMode == MTLCullModeBack) ? 1u : 0u;
    if (!_builder.setCullMode(cullFront, cullBack, winding == MTLWindingClockwise)) {
        _rejected = YES;
    }
}

- (void)drawPrimitives:(MTLPrimitiveType)type start:(NSUInteger)start count:(NSUInteger)count instances:(NSUInteger)instances {
    // gfx12 takes the primitive type from VGT_PRIMITIVE_TYPE, not from the
    // draw packet, and the first vertex comes from a user SGPR rather than the
    // packet - so `start` is not encodable here yet.
    (void)start; (void)instances;
    if (!_builder.setPrimitiveType(diPrimTypeFor(type))) { _rejected = YES; return; }
    if (!_builder.drawPrimitives((uint32_t)count)) _rejected = YES;
}

- (void)drawIndexed:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexBuffer:(uint64_t)addr indexType:(MTLIndexType)indexType instances:(NSUInteger)instances {
    (void)instances;
    if (!_builder.setPrimitiveType(diPrimTypeFor(type))) { _rejected = YES; return; }
    if (!_builder.setIndexType(indexType == MTLIndexTypeUInt32 ? GFX12::VGT_INDEX_32
                                                               : GFX12::VGT_INDEX_16)) {
        _rejected = YES;
        return;
    }
    if (!_builder.drawIndexedPrimitives((uint32_t)count, addr, (uint32_t)count)) _rejected = YES;
}

- (void)flushCaches {
    if (!_builder.emitCacheFlush()) _rejected = YES;
}

- (NSData*)getPackets {
    if (_rejected || _builder.overflowed()) return nil;
    size_t dwords = 0;
    const uint32_t* packets = _builder.getPackets(&dwords);
    if (dwords == 0) return nil;
    return [NSData dataWithBytes:packets length:dwords * sizeof(uint32_t)];
}

@end
