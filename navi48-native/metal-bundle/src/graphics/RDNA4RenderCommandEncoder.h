#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@class RDNA4CommandBuffer;
@class RDNA4RenderPipelineState;
@class RDNA4Buffer;
@class RDNA4Texture;

/**
 * RDNA4RenderCommandEncoder - Render Command Encoder
 * 
 * Encodes rendering commands into a command buffer.
 * 这是编码绘制命令的主接口。
 */
@interface RDNA4RenderCommandEncoder : NSObject <MTLRenderCommandEncoder>

@property (readonly) id<MTLDevice> device;
@property (copy) NSString* label;

/**
 * Initialize with command buffer and render pass descriptor
 */
- (instancetype)initWithCommandBuffer:(RDNA4CommandBuffer*)commandBuffer
               renderPassDescriptor:(MTLRenderPassDescriptor*)descriptor;

/**
 * End encoding (must be called before committing command buffer)
 */
- (void)endEncoding;

/**
 * Set render pipeline state
 */
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipelineState;

/**
 * Set vertex buffer
 */
- (void)setVertexBuffer:(id<MTLBuffer>)buffer
                 offset:(NSUInteger)offset
                atIndex:(NSUInteger)index;

/**
 * Set vertex buffers (multiple at once)
 */
- (void)setVertexBuffers:(const id<MTLBuffer>*)buffers
                 offsets:(const NSUInteger*)offsets
               withRange:(NSRange)range;

/**
 * Set fragment buffer
 */
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer
                   offset:(NSUInteger)offset
                  atIndex:(NSUInteger)index;

/**
 * Set vertex texture
 */
- (void)setVertexTexture:(id<MTLTexture>)texture
                 atIndex:(NSUInteger)index;

/**
 * Set fragment texture
 */
- (void)setFragmentTexture:(id<MTLTexture>)texture
                   atIndex:(NSUInteger)index;

/**
 * Set viewport
 */
- (void)setViewport:(MTLViewport)viewport;

/**
 * Set scissor rectangle
 */
- (void)setScissorRect:(MTLScissorRect)rect;

/**
 * Set cull mode
 */
- (void)setCullMode:(MTLCullMode)cullMode;

/**
 * Set front facing winding
 */
- (void)setFrontFacingWinding:(MTLWinding)frontFacingWinding;

/**
 * Set triangle fill mode
 */
- (void)setTriangleFillMode:(MTLTriangleFillMode)fillMode;

/**
 * Set blend color
 */
- (void)setBlendColorRed:(float)red
                   green:(float)green
                    blue:(float)blue
                   alpha:(float)alpha;

/**
 * Set depth bias
 */
- (void)setDepthBias:(float)depthBias
          slopeScale:(float)slopeScale
               clamp:(float)clamp;

/**
 * Draw primitives (non-indexed)
 */
- (void)drawPrimitives:(MTLPrimitiveType)primitiveType
           vertexStart:(NSUInteger)vertexStart
           vertexCount:(NSUInteger)vertexCount;

/**
 * Draw primitives with instance count
 */
- (void)drawPrimitives:(MTLPrimitiveType)primitiveType
           vertexStart:(NSUInteger)vertexStart
           vertexCount:(NSUInteger)vertexCount
         instanceCount:(NSUInteger)instanceCount;

/**
 * Draw indexed primitives
 */
- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType
                   indexCount:(NSUInteger)indexCount
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer
            indexBufferOffset:(NSUInteger)indexBufferOffset;

/**
 * Draw indexed primitives with instances
 */
- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType
                   indexCount:(NSUInteger)indexCount
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer
            indexBufferOffset:(NSUInteger)indexBufferOffset
                instanceCount:(NSUInteger)instanceCount;

/**
 * Get encoded PM4 commands (internal use)
 */
- (NSData*)getEncodedCommands;

@end
