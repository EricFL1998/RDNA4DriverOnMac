#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@class RDNA4VertexDescriptor;

/**
 * RDNA4RenderPipelineDescriptor - Render Pipeline Configuration
 * 
 * Describes the complete rendering pipeline including shaders, vertex layout,
 * and render target configuration.
 */
@interface RDNA4RenderPipelineDescriptor : NSObject

/**
 * Pipeline label for debugging
 */
@property (copy) NSString* label;

/**
 * Vertex function
 */
@property (nonatomic, retain) id<MTLFunction> vertexFunction;

/**
 * Fragment function
 */
@property (nonatomic, retain) id<MTLFunction> fragmentFunction;

/**
 * Vertex descriptor (vertex input layout)
 */
@property (nonatomic, copy) MTLVertexDescriptor* vertexDescriptor;

/**
 * Color attachments (render target formats)
 */
@property (readonly) MTLRenderPipelineColorAttachmentDescriptorArray* colorAttachments;

/**
 * Depth attachment format
 */
@property (nonatomic) MTLPixelFormat depthAttachmentPixelFormat;

/**
 * Stencil attachment format
 */
@property (nonatomic) MTLPixelFormat stencilAttachmentPixelFormat;

/**
 * Sample count (for MSAA)
 */
@property (nonatomic) NSUInteger sampleCount;

/**
 * Alpha to coverage enabled (for MSAA)
 */
@property (nonatomic) BOOL alphaToCoverageEnabled;

/**
 * Alpha to one enabled (for MSAA)
 */
@property (nonatomic) BOOL alphaToOneEnabled;

/**
 * Rasterization enabled
 */
@property (nonatomic) BOOL rasterizationEnabled;

/**
 * Input primitive topology
 */
@property (nonatomic) MTLPrimitiveTopologyClass inputPrimitiveTopology;

/**
 * Reset to default state
 */
- (void)reset;

/**
 * Copy descriptor
 */
- (id)copyWithZone:(NSZone*)zone;

@end

/**
 * RDNA4RenderPipelineColorAttachmentDescriptor - Color Attachment Configuration
 */
@interface RDNA4RenderPipelineColorAttachmentDescriptor : NSObject

/**
 * Pixel format
 */
@property (nonatomic) MTLPixelFormat pixelFormat;

/**
 * Blending enabled
 */
@property (nonatomic) BOOL blendingEnabled;

/**
 * Source RGB blend factor
 */
@property (nonatomic) MTLBlendFactor sourceRGBBlendFactor;

/**
 * Destination RGB blend factor
 */
@property (nonatomic) MTLBlendFactor destinationRGBBlendFactor;

/**
 * RGB blend operation
 */
@property (nonatomic) MTLBlendOperation rgbBlendOperation;

/**
 * Source alpha blend factor
 */
@property (nonatomic) MTLBlendFactor sourceAlphaBlendFactor;

/**
 * Destination alpha blend factor
 */
@property (nonatomic) MTLBlendFactor destinationAlphaBlendFactor;

/**
 * Alpha blend operation
 */
@property (nonatomic) MTLBlendOperation alphaBlendOperation;

/**
 * Write mask
 */
@property (nonatomic) MTLColorWriteMask writeMask;

@end

/**
 * RDNA4RenderPipelineColorAttachmentDescriptorArray - Array of Color Attachments
 */
@interface RDNA4RenderPipelineColorAttachmentDescriptorArray : NSObject

- (RDNA4RenderPipelineColorAttachmentDescriptor*)objectAtIndexedSubscript:(NSUInteger)attachmentIndex;
- (void)setObject:(RDNA4RenderPipelineColorAttachmentDescriptor*)attachment atIndexedSubscript:(NSUInteger)attachmentIndex;

@end
