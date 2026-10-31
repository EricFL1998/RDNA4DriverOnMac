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

#define _reject(w) do { if (!_rejected) _failure = (w); _rejected = YES; } while (0)

@implementation RDNA4RenderPM4Builder {
    // instrumented failure recorder declared in the class extension below
    GraphicsPM4Builder _builder;
    BOOL _rejected;
    const char* _failure;
    uint64_t _attributeRingAddress;
    uint32_t _attributeRingSize;
    uint32_t _parameterCount;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        [self reset];
    }
    return self;
}

- (void)reset {
    _builder.reset();
    _rejected = NO;
    _failure = nullptr;
    _attributeRingAddress = 0;
    _attributeRingSize = 0;
    _parameterCount = 0;
}

- (void)setDepthDisabled {
    if (!_builder.setDepthDisabled()) _reject(__func__);
}

- (void)setColorWriteMask:(uint32_t)targetMask shaderMask:(uint32_t)shaderMask {
    if (!_builder.setColorWriteMask(targetMask, shaderMask)) _reject(__func__);
}

- (void)setBlendDisabled {
    if (!_builder.setBlendState(0, false, 0, 0, 0, 0, 0, 0)) _reject(__func__);
}

- (void)setViewport:(MTLViewport)viewport {
    if (!_builder.setViewport(viewport.originX, viewport.originY,
                              viewport.width, viewport.height,
                              viewport.znear, viewport.zfar)) {
        _reject(__func__);
    }
}

- (void)setScissor:(MTLScissorRect)rect {
    if (!_builder.setScissorRect((uint32_t)rect.x, (uint32_t)rect.y,
                                 (uint32_t)rect.width, (uint32_t)rect.height)) {
        _reject(__func__);
    }
}

- (void)setVertexShader:(uint64_t)gpuAddress rsrc1:(uint32_t)rsrc1 rsrc2:(uint32_t)rsrc2
         parameterCount:(uint32_t)parameterCount
   attributeRingAddress:(uint64_t)attributeRingAddress
     attributeRingSize:(uint32_t)attributeRingSize {
    _parameterCount = parameterCount;
    _attributeRingAddress = attributeRingAddress;
    _attributeRingSize = attributeRingSize;
    if (!_builder.setVertexShader(gpuAddress, rsrc1, rsrc2)) _reject(__func__);
}

- (void)setFragmentShader:(uint64_t)gpuAddress rsrc1:(uint32_t)rsrc1 rsrc2:(uint32_t)rsrc2 {
    if (!_builder.setPixelShader(gpuAddress, rsrc1, rsrc2)) _reject(__func__);
}

- (void)setVertexBuffer:(NSUInteger)index address:(uint64_t)addr stride:(uint32_t)stride count:(uint32_t)count {
    if (index != 0 || !_builder.setVertexInputs(addr, stride, _attributeRingAddress,
                                                _attributeRingSize, _parameterCount,
                                                count, count / 3u)) {
        NSLog(@"RDNA4RenderPM4Builder: rejected vertex inputs, ring=0x%llx size=%u stride=%u",
              _attributeRingAddress, _attributeRingSize, stride);
        _reject(__func__);
    }
}

- (void)setColorBuffer:(NSUInteger)index texture:(RDNA4Texture*)texture {
    if (!texture) return;
    if (!_builder.setColorAttachment((uint32_t)index, [texture gpuAddress],
                                     (uint32_t)[texture width], (uint32_t)[texture height],
                                     cbFormatFor([texture pixelFormat]),
                                     (uint32_t)[texture sampleCount],
                                     // The fragment shader exports R,G,B,A; the
                                     // CB must store them in that byte order
                                     // for the surface to read back as the
                                     // caller expects (COMP_SWAP none).
                                     0u)) {
        _reject(__func__);
    }
}

- (void)setCullMode:(MTLCullMode)cullMode winding:(MTLWinding)winding {
    const uint32_t cullFront = (cullMode == MTLCullModeFront) ? 1u : 0u;
    const uint32_t cullBack = (cullMode == MTLCullModeBack) ? 1u : 0u;
    if (!_builder.setCullMode(cullFront, cullBack, winding == MTLWindingClockwise)) {
        _reject(__func__);
    }
}

- (void)setDrawCounts:(uint32_t)vertexCount primitives:(uint32_t)primitiveCount {
    if (!_builder.setDrawCounts(vertexCount, primitiveCount)) _reject(__func__);
}

- (void)drawPrimitives:(MTLPrimitiveType)type start:(NSUInteger)start count:(NSUInteger)count instances:(NSUInteger)instances {
    (void)start; (void)instances;
    uint32_t primitiveCount = 0;
    switch (type) {
        case MTLPrimitiveTypePoint: primitiveCount = (uint32_t)count; break;
        case MTLPrimitiveTypeLine: primitiveCount = (uint32_t)count / 2u; break;
        case MTLPrimitiveTypeLineStrip: primitiveCount = (uint32_t)(count - 1); break;
        case MTLPrimitiveTypeTriangle: primitiveCount = (uint32_t)count / 3u; break;
        case MTLPrimitiveTypeTriangleStrip: primitiveCount = (uint32_t)(count - 2); break;
    }
    if (!primitiveCount || !_builder.setDrawCounts((uint32_t)count, primitiveCount)) {
        _reject(__func__);
        return;
    }
    if (!_builder.setPrimitiveType(diPrimTypeFor(type))) { _reject(__func__); return; }
    // The CP consumes the instance count from this packet, not from the
    // VGT_NUM_INSTANCES register write.
    if (!_builder.setNumInstances((uint32_t)(instances ? instances : 1u))) {
        _reject(__func__);
        return;
    }
    if (!_builder.drawPrimitives((uint32_t)count)) _reject(__func__);
}

- (void)drawIndexed:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexBuffer:(uint64_t)addr indexType:(MTLIndexType)indexType instances:(NSUInteger)instances {
    (void)instances;
    if (!_builder.setPrimitiveType(diPrimTypeFor(type))) { _reject(__func__); return; }
    if (!_builder.setIndexType(indexType == MTLIndexTypeUInt32 ? GFX12::VGT_INDEX_32
                                                               : GFX12::VGT_INDEX_16)) {
        _reject(__func__);
        return;
    }
    if (!_builder.drawIndexedPrimitives((uint32_t)count, addr, (uint32_t)count)) _reject(__func__);
}

- (void)flushCaches {
    if (!_builder.emitCacheFlush()) _reject(__func__);
}

- (void)invalidateShaderCaches {
    if (!_builder.acquireMem()) _reject(__func__);
}

- (void)enableContextState {
    if (!_builder.emitContextControl()) _reject(__func__);
}

- (void)emitGraphicsPreamble {
    // Bisect hooks: N48_PREAMBLE_MASK covers table entries 0..63,
    // N48_PREAMBLE_MASK_HI covers entries 64..72 (bit k = entry 64+k) and the
    // trailing group (bit 63). Both default to every entry selected.
    uint64_t mask = GraphicsPM4Builder::kPreambleAll;
    uint64_t maskHigh = GraphicsPM4Builder::kPreambleAll;
    const char* env = getenv("N48_PREAMBLE_MASK");
    if (env) mask = strtoull(env, NULL, 0);
    env = getenv("N48_PREAMBLE_MASK_HI");
    if (env) maskHigh = strtoull(env, NULL, 0);
    env = getenv("N48_FEATURE_MASK");
    if (env) _builder.featureMask = (uint32_t)strtoul(env, NULL, 0);
    // gfx12 has no CLEAR_STATE: every context register still holds its reset
    // value, so the draw depends on this table being on the wire. This wrapper
    // used to compute the mask and drop it, which left every entry unwritten.
    if (!_builder.emitGraphicsPreamble(mask, maskHigh)) _reject(__func__);
}

- (NSData*)getPackets {
    if (_rejected || _builder.overflowed()) {
        NSLog(@"RDNA4RenderPM4Builder: rejected in %s (overflow=%d)",
              _failure ? _failure : "?", _builder.overflowed() ? 1 : 0);
        return nil;
    }
    size_t dwords = 0;
    const uint32_t* packets = _builder.getPackets(&dwords);
    if (dwords == 0) return nil;
    const char* dumpPath = getenv("N48_DUMP_PM4");
    if (dumpPath && *dumpPath) {
        FILE* file = fopen(dumpPath, "wb");
        if (file) {
            fwrite(packets, sizeof(uint32_t), dwords, file);
            fclose(file);
            NSLog(@"RDNA4RenderPM4Builder: dumped %zu dwords to %s", dwords, dumpPath);
        }
    }
    return [NSData dataWithBytes:packets length:dwords * sizeof(uint32_t)];
}

@end
