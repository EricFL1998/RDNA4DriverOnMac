#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/**
 * RDNA4RenderPassDescriptor - Render Pass Configuration
 * 
 * Describes render targets and their load/store actions for a rendering pass.
 */
@interface RDNA4RenderPassDescriptor : NSObject

/**
 * Color attachments (up to 8)
 */
@property (readonly) MTLRenderPassColorAttachmentDescriptorArray* colorAttachments;

/**
 * Depth attachment
 */
@property (readonly) MTLRenderPassDepthAttachmentDescriptor* depthAttachment;

/**
 * Stencil attachment
 */
@property (readonly) MTLRenderPassStencilAttachmentDescriptor* stencilAttachment;

/**
 * Visibility result buffer (occlusion query)
 */
@property (nonatomic, retain) id<MTLBuffer> visibilityResultBuffer;

/**
 * Render target array length (for layered rendering)
 */
@property (nonatomic) NSUInteger renderTargetArrayLength;

/**
 * Create a new render pass descriptor
 */
+ (MTLRenderPassDescriptor*)renderPassDescriptor;

/**
 * Copy descriptor
 */
- (id)copyWithZone:(NSZone*)zone;

@end

/**
 * RDNA4RenderPassColorAttachmentDescriptor - Color Attachment Configuration
 */
@interface RDNA4RenderPassColorAttachmentDescriptor : NSObject

/**
 * The texture to render into
 */
@property (nonatomic, retain) id<MTLTexture> texture;

/**
 * Mipmap level within the texture
 */
@property (nonatomic) NSUInteger level;

/**
 * Slice within the texture (for 3D/Array/Cube textures)
 */
@property (nonatomic) NSUInteger slice;

/**
 * Depth plane (for 3D textures)
 */
@property (nonatomic) NSUInteger depthPlane;

/**
 * Resolve texture (for MSAA)
 */
@property (nonatomic, retain) id<MTLTexture> resolveTexture;

/**
 * Resolve level
 */
@property (nonatomic) NSUInteger resolveLevel;

/**
 * Resolve slice
 */
@property (nonatomic) NSUInteger resolveSlice;

/**
 * Resolve depth plane
 */
@property (nonatomic) NSUInteger resolveDepthPlane;

/**
 * Load action (clear, load, dontcare)
 */
@property (nonatomic) MTLLoadAction loadAction;

/**
 * Store action (store, dontcare, multisampleResolve)
 */
@property (nonatomic) MTLStoreAction storeAction;

/**
 * Clear color (used when loadAction == MTLLoadActionClear)
 */
@property (nonatomic) MTLClearColor clearColor;

@end

/**
 * RDNA4RenderPassDepthAttachmentDescriptor - Depth Attachment Configuration
 */
@interface RDNA4RenderPassDepthAttachmentDescriptor : NSObject

@property (nonatomic, retain) id<MTLTexture> texture;
@property (nonatomic) NSUInteger level;
@property (nonatomic) NSUInteger slice;
@property (nonatomic) NSUInteger depthPlane;
@property (nonatomic, retain) id<MTLTexture> resolveTexture;
@property (nonatomic) NSUInteger resolveLevel;
@property (nonatomic) NSUInteger resolveSlice;
@property (nonatomic) NSUInteger resolveDepthPlane;
@property (nonatomic) MTLLoadAction loadAction;
@property (nonatomic) MTLStoreAction storeAction;
@property (nonatomic) double clearDepth;
@property (nonatomic) MTLMultisampleDepthResolveFilter depthResolveFilter;

@end

/**
 * RDNA4RenderPassStencilAttachmentDescriptor - Stencil Attachment Configuration
 */
@interface RDNA4RenderPassStencilAttachmentDescriptor : NSObject

@property (nonatomic, retain) id<MTLTexture> texture;
@property (nonatomic) NSUInteger level;
@property (nonatomic) NSUInteger slice;
@property (nonatomic) NSUInteger depthPlane;
@property (nonatomic, retain) id<MTLTexture> resolveTexture;
@property (nonatomic) NSUInteger resolveLevel;
@property (nonatomic) NSUInteger resolveSlice;
@property (nonatomic) NSUInteger resolveDepthPlane;
@property (nonatomic) MTLLoadAction loadAction;
@property (nonatomic) MTLStoreAction storeAction;
@property (nonatomic) uint32_t clearStencil;
@property (nonatomic) MTLMultisampleStencilResolveFilter stencilResolveFilter;

@end

/**
 * RDNA4RenderPassColorAttachmentDescriptorArray - Array of Color Attachments
 */
@interface RDNA4RenderPassColorAttachmentDescriptorArray : NSObject

- (RDNA4RenderPassColorAttachmentDescriptor*)objectAtIndexedSubscript:(NSUInteger)attachmentIndex;
- (void)setObject:(RDNA4RenderPassColorAttachmentDescriptor*)attachment atIndexedSubscript:(NSUInteger)attachmentIndex;

@end
