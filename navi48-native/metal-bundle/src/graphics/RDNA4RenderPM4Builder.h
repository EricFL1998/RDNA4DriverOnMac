//
//  RDNA4RenderPM4Builder.h
//  RDNA4Metal - thin Objective-C face over the verified PM4 encoder
//
//  The encoder itself lives in RDNA4Driver/src/hardware/GraphicsPM4Builder.cpp
//  and is compiled into this bundle too. Keeping one implementation means the
//  bundle cannot drift onto register offsets the driver has already rejected.
//

#ifndef RDNA4RenderPM4Builder_h
#define RDNA4RenderPM4Builder_h

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@class RDNA4Texture;

@interface RDNA4RenderPM4Builder : NSObject

- (void)reset;
- (void)setDepthDisabled;
- (void)setBlendDisabled;
- (void)setColorWriteMask:(uint32_t)targetMask shaderMask:(uint32_t)shaderMask;
- (void)setViewport:(MTLViewport)viewport;
- (void)setScissor:(MTLScissorRect)rect;
- (void)setVertexShader:(uint64_t)gpuAddress rsrc1:(uint32_t)rsrc1 rsrc2:(uint32_t)rsrc2
         parameterCount:(uint32_t)parameterCount
   attributeRingAddress:(uint64_t)attributeRingAddress
     attributeRingSize:(uint32_t)attributeRingSize;
- (void)setFragmentShader:(uint64_t)gpuAddress rsrc1:(uint32_t)rsrc1 rsrc2:(uint32_t)rsrc2;
- (void)setVertexUserData:(uint32_t)userSgpr value:(uint64_t)value;
- (void)setFragmentUserData:(uint32_t)userSgpr value:(uint64_t)value;
- (void)setVertexBuffer:(NSUInteger)index address:(uint64_t)addr stride:(uint32_t)stride count:(uint32_t)count;
- (void)setDrawCounts:(uint32_t)vertexCount primitives:(uint32_t)primitiveCount;
- (void)setColorBuffer:(NSUInteger)index texture:(RDNA4Texture*)texture;
- (void)setCullMode:(MTLCullMode)cullMode winding:(MTLWinding)winding;
- (void)drawPrimitives:(MTLPrimitiveType)type start:(NSUInteger)start count:(NSUInteger)count instances:(NSUInteger)instances;
- (void)drawIndexed:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexBuffer:(uint64_t)addr indexType:(MTLIndexType)indexType instances:(NSUInteger)instances;
- (void)flushCaches;
- (void)invalidateShaderCaches;
- (void)enableContextState;
// gfx12 has no CLEAR_STATE, so the context registers start at their reset
// values; this writes the graphics preamble once per command buffer.
- (void)emitGraphicsPreamble;

// The encoded dwords, or nil when the encoder rejected something.
- (NSData*)getPackets;


@end

#endif /* RDNA4RenderPM4Builder_h */
