#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import "RDNA4MetalDeviceSPI.h"

/**
 * RDNA4Device - Native Metal Device Implementation for AMD RDNA4 GPUs
 * 
 * Metal discovers this _MTLDevice subclass through RDNA4Accel's registry
 * properties. GPU communication uses the separate RDNA4Driver user client.
 */
@interface RDNA4Device : _MTLDevice <MTLDevice>

// Called by Metal's MTLIOAccelService, with the matched accelerator service.
- (instancetype)initWithAcceleratorPort:(io_service_t)service;

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

// One NGG ring shared by every pipeline on this device. Allocating it per
// pipeline consumes 10.5 MiB of the 32 MiB client window each time.
- (uint64_t)sharedNGGRingAddress:(uint32_t*)outSize;
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
// Render commands go to the GFX queue, compute commands to the MES queue.
- (void)appendRenderCommands:(const void*)bytes length:(NSUInteger)length;

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

// RDNA4Texture and RDNA4RenderPipelineState are real classes now; declaring
// stubs here as well would make every translation unit that includes both
// headers see two interfaces for the same name.
@class RDNA4Texture;
@class RDNA4RenderPipelineState;

// Per-class headers own the remaining declarations.
#import "RDNA4Library.h"
#import "RDNA4Function.h"
#import "RDNA4ComputePipelineState.h"
#import "RDNA4ComputeCommandEncoder.h"
