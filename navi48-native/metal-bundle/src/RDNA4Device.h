#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/**
 * RDNA4Device - Native Metal Device Implementation for AMD RDNA4 GPUs
 * 
 * This is a stub implementation that conforms to MTLDevice protocol.
 * Phase 3.2 skeleton - actual GPU communication happens in Phase 3.3+
 */
@interface RDNA4Device : NSObject <MTLDevice>

// Device identification
@property (readonly) NSString* name;
@property (readonly) uint64_t registryID;
@property (readonly) MTLSize maxThreadsPerThreadgroup;
@property (readonly) BOOL isLowPower;
@property (readonly) BOOL isHeadless;
@property (readonly) uint64_t recommendedMaxWorkingSetSize;

// Command queue creation
- (id<MTLCommandQueue>)newCommandQueue;
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)maxCommandBufferCount;

// Buffer creation
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options;
- (id<MTLBuffer>)newBufferWithBytes:(const void*)pointer length:(NSUInteger)length options:(MTLResourceOptions)options;
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void*)pointer length:(NSUInteger)length options:(MTLResourceOptions)options deallocator:(void (^)(void* pointer, NSUInteger length))deallocator;

// Texture creation
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor*)descriptor;

// Compute pipeline creation
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)computeFunction error:(NSError**)error;
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)computeFunction completionHandler:(void (^)(id<MTLComputePipelineState>, NSError*))completionHandler;

// Render pipeline creation
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor*)descriptor error:(NSError**)error;
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor*)descriptor completionHandler:(void (^)(id<MTLRenderPipelineState>, NSError*))completionHandler;

// Library creation
- (id<MTLLibrary>)newDefaultLibrary;
- (id<MTLLibrary>)newLibraryWithFile:(NSString*)filepath error:(NSError**)error;
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError**)error;

// Feature support queries
- (BOOL)supportsFeatureSet:(MTLFeatureSet)featureSet;
- (BOOL)supportsFamily:(MTLGPUFamily)gpuFamily;

@end

/**
 * RDNA4CommandQueue - Command queue implementation
 */
@interface RDNA4CommandQueue : NSObject <MTLCommandQueue>

@property (readonly) id<MTLDevice> device;
@property (copy) NSString* label;

- (id<MTLCommandBuffer>)commandBuffer;
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences;

- (instancetype)initWithDevice:(RDNA4Device*)device;

@end

/**
 * RDNA4CommandBuffer - Command buffer implementation
 */
@interface RDNA4CommandBuffer : NSObject <MTLCommandBuffer>

@property (readonly) id<MTLDevice> device;
@property (readonly) id<MTLCommandQueue> commandQueue;
@property (copy) NSString* label;
@property (readonly) MTLCommandBufferStatus status;

- (id<MTLComputeCommandEncoder>)computeCommandEncoder;
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor*)renderPassDescriptor;
- (id<MTLBlitCommandEncoder>)blitCommandEncoder;

- (void)enqueue;
- (void)commit;
- (void)waitUntilCompleted;
- (void)waitUntilScheduled;

- (instancetype)initWithQueue:(RDNA4CommandQueue*)queue;

- (void)encoderDidEnd:(id)encoder;
- (void)appendCommands:(const void*)bytes length:(NSUInteger)length;

@end

/**
 * RDNA4Buffer - Buffer implementation
 */
@interface RDNA4Buffer : NSObject <MTLBuffer>

@property (readonly) id<MTLDevice> device;
@property (copy) NSString* label;
@property (readonly) NSUInteger length;
@property (readonly) MTLResourceOptions resourceOptions;
@property (readonly) void* contents;

- (instancetype)initWithDevice:(RDNA4Device*)device
                        length:(NSUInteger)length
                       options:(MTLResourceOptions)options;
- (instancetype)initWithDevice:(RDNA4Device*)device
                         bytes:(const void*)pointer
                        length:(NSUInteger)length
                       options:(MTLResourceOptions)options;

@end

/**
 * RDNA4Texture - Texture implementation
 */
@interface RDNA4Texture : NSObject <MTLTexture>

@property (readonly) id<MTLDevice> device;
@property (copy) NSString* label;
@property (readonly) MTLTextureType textureType;
@property (readonly) MTLPixelFormat pixelFormat;
@property (readonly) NSUInteger width;
@property (readonly) NSUInteger height;
@property (readonly) NSUInteger depth;

@end

/**
 * RDNA4RenderPipelineState - Render pipeline state
 */
@interface RDNA4RenderPipelineState : NSObject <MTLRenderPipelineState>

@property (readonly) id<MTLDevice> device;
@property (copy) NSString* label;

@end

// Per-class headers own the remaining declarations.
#import "RDNA4Library.h"
#import "RDNA4Function.h"
#import "RDNA4ComputePipelineState.h"
#import "RDNA4ComputeCommandEncoder.h"
