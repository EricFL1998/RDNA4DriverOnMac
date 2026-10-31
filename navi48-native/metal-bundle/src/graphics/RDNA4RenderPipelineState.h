#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@class RDNA4Device;
@class RDNA4Function;
@class RDNA4CompiledShader;

/**
 * RDNA4RenderPipelineState - Render Pipeline State
 * 
 * Represents a compiled render pipeline ready for execution.
 * Contains compiled vertex and fragment shaders uploaded to VRAM.
 */
@interface RDNA4RenderPipelineState : NSObject <MTLRenderPipelineState>

@property (readonly) id<MTLDevice> device;
@property (copy) NSString* label;

/**
 * Initialize with descriptor
 * @param device Parent device
 * @param descriptor Pipeline configuration
 * @param error Output error
 */
- (instancetype)initWithDevice:(RDNA4Device*)device
                    descriptor:(MTLRenderPipelineDescriptor*)descriptor
                         error:(NSError**)error;

/**
 * Get vertex shader GPU address (internal use)
 */
- (uint64_t)vertexShaderGPUAddress;

/**
 * Get fragment shader GPU address (internal use)
 */
- (uint64_t)fragmentShaderGPUAddress;

@property (readonly) uint64_t attributeRingGPUAddress;
@property (readonly) uint32_t attributeRingSize;

/**
 * Copy of the attribute ring's leading bytes, for post-draw diagnostics: the
 * vertex stage writes its exports here, so an untouched ring means no waves ran.
 */
- (NSData*)attributeRingContentsOfLength:(NSUInteger)length;

/**
 * Copy of any NGG ring region relative to the ring base, for diagnostics that
 * need the position (0x580000) or primitive (0x980000) ring rather than the
 * attribute ring at offset 0.
 */
- (NSData*)ringContentsAtOffset:(NSUInteger)offset length:(NSUInteger)length;

/**
 * Zero the attribute ring so a post-draw scan for non-zero data is meaningful.
 */
- (void)clearAttributeRing;

/**
 * Get vertex shader metadata
 */
- (RDNA4CompiledShader*)vertexShader;

/**
 * Get fragment shader metadata
 */
- (RDNA4CompiledShader*)fragmentShader;

/**
 * Get vertex descriptor
 */
- (MTLVertexDescriptor*)vertexDescriptor;

/**
 * Get color attachment pixel format at index
 */
- (MTLPixelFormat)colorAttachmentPixelFormatAtIndex:(NSUInteger)index;

/**
 * Get depth attachment pixel format
 */
- (MTLPixelFormat)depthAttachmentPixelFormat;

/**
 * Get stencil attachment pixel format
 */
- (MTLPixelFormat)stencilAttachmentPixelFormat;

/**
 * Get sample count
 */
- (NSUInteger)sampleCount;

/**
 * Check if alpha to coverage is enabled
 */
- (BOOL)isAlphaToCoverageEnabled;

/**
 * Check if alpha to one is enabled
 */
- (BOOL)isAlphaToOneEnabled;

/**
 * Check if rasterization is enabled
 */
- (BOOL)isRasterizationEnabled;

@end
