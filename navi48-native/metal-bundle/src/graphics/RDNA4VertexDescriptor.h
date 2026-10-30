#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/**
 * RDNA4VertexDescriptor - Vertex Input Layout
 * 
 * Describes how vertex data is organized and fed into the vertex shader.
 */
@interface RDNA4VertexDescriptor : NSObject

/**
 * Vertex attribute descriptors (up to 31)
 */
@property (readonly) MTLVertexAttributeDescriptorArray* attributes;

/**
 * Vertex buffer layout descriptors (up to 31)
 */
@property (readonly) MTLVertexBufferLayoutDescriptorArray* layouts;

/**
 * Create a new vertex descriptor
 */
+ (MTLVertexDescriptor*)vertexDescriptor;

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
 * RDNA4VertexAttributeDescriptor - Single Vertex Attribute
 */
@interface RDNA4VertexAttributeDescriptor : NSObject

/**
 * Data format (float, float2, float3, float4, etc.)
 */
@property (nonatomic) MTLVertexFormat format;

/**
 * Offset within the vertex structure
 */
@property (nonatomic) NSUInteger offset;

/**
 * Which buffer this attribute comes from (0-30)
 */
@property (nonatomic) NSUInteger bufferIndex;

@end

/**
 * RDNA4VertexBufferLayoutDescriptor - Vertex Buffer Layout
 */
@interface RDNA4VertexBufferLayoutDescriptor : NSObject

/**
 * Stride between vertices in bytes
 */
@property (nonatomic) NSUInteger stride;

/**
 * Step function (per-vertex or per-instance)
 */
@property (nonatomic) MTLVertexStepFunction stepFunction;

/**
 * Step rate (for instanced rendering)
 */
@property (nonatomic) NSUInteger stepRate;

@end

/**
 * RDNA4VertexAttributeDescriptorArray - Array of Vertex Attributes
 */
@interface RDNA4VertexAttributeDescriptorArray : NSObject

- (RDNA4VertexAttributeDescriptor*)objectAtIndexedSubscript:(NSUInteger)index;
- (void)setObject:(RDNA4VertexAttributeDescriptor*)attribute atIndexedSubscript:(NSUInteger)index;

@end

/**
 * RDNA4VertexBufferLayoutDescriptorArray - Array of Buffer Layouts
 */
@interface RDNA4VertexBufferLayoutDescriptorArray : NSObject

- (RDNA4VertexBufferLayoutDescriptor*)objectAtIndexedSubscript:(NSUInteger)index;
- (void)setObject:(RDNA4VertexBufferLayoutDescriptor*)layout atIndexedSubscript:(NSUInteger)index;

@end
