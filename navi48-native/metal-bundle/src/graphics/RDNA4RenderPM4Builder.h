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
- (void)setViewport:(MTLViewport)viewport;
- (void)setScissor:(MTLScissorRect)rect;
- (void)setVertexShader:(uint64_t)gpuAddress rsrc1:(uint32_t)rsrc1 rsrc2:(uint32_t)rsrc2;
- (void)setFragmentShader:(uint64_t)gpuAddress rsrc1:(uint32_t)rsrc1 rsrc2:(uint32_t)rsrc2;
- (void)setVertexBuffer:(NSUInteger)index address:(uint64_t)addr stride:(uint32_t)stride count:(uint32_t)count;
- (void)setColorBuffer:(NSUInteger)index texture:(RDNA4Texture*)texture;
- (void)setCullMode:(MTLCullMode)cullMode winding:(MTLWinding)winding;
- (void)drawPrimitives:(MTLPrimitiveType)type start:(NSUInteger)start count:(NSUInteger)count instances:(NSUInteger)instances;
- (void)drawIndexed:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexBuffer:(uint64_t)addr indexType:(MTLIndexType)indexType instances:(NSUInteger)instances;
- (void)flushCaches;

// The encoded dwords, or nil when the encoder rejected something.
- (NSData*)getPackets;

// Vertex-buffer descriptors need the per-stage user-SGPR base, which depends
// on shader metadata we do not have yet. They are counted here instead of
// being encoded as packets that would point at the wrong register.
@property (readonly) NSUInteger unsupportedVertexBuffers;

@end

#endif /* RDNA4RenderPM4Builder_h */
