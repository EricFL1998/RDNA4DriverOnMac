#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@class RDNA4CommandBuffer;
@class RDNA4ComputePipelineState;

/**
 * RDNA4ComputeCommandEncoder - Compute Command Encoder
 * 
 * Encodes compute commands into a command buffer.
 * Accumulates state (pipeline, buffers, etc.) and generates PM4 packets
 * when dispatchThreadgroups is called.
 */
@interface RDNA4ComputeCommandEncoder : NSObject <MTLComputeCommandEncoder>

@property (readonly) id<MTLDevice> device;
@property (copy) NSString* label;

/**
 * Initialize with command buffer
 * @param commandBuffer Parent command buffer
 */
- (instancetype)initWithCommandBuffer:(RDNA4CommandBuffer*)commandBuffer;

/**
 * Set compute pipeline state
 */
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state;

/**
 * Set buffer at index
 */
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;

/**
 * Set buffers in range
 */
- (void)setBuffers:(const id<MTLBuffer>*)buffers
           offsets:(const NSUInteger*)offsets
         withRange:(NSRange)range;

/**
 * Set threadgroup memory length
 */
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index;

/**
 * Dispatch threadgroups
 */
- (void)dispatchThreadgroups:(MTLSize)threadgroupsPerGrid
       threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup;

/**
 * Dispatch threadgroups with indirect buffer
 */
- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)indirectBuffer
                          indirectBufferOffset:(NSUInteger)indirectBufferOffset
                         threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup;

/**
 * End encoding
 */
- (void)endEncoding;

/**
 * Get encoded PM4 commands (internal use)
 */
- (NSData*)encodedCommands;

@end

/**
 * Internal structure to track encoder state
 */
@interface RDNA4ComputeEncoderState : NSObject

@property (nonatomic, strong) RDNA4ComputePipelineState* pipelineState;
@property (nonatomic, strong) NSMutableDictionary<NSNumber*, id<MTLBuffer>>* buffers;
@property (nonatomic, strong) NSMutableDictionary<NSNumber*, NSNumber*>* bufferOffsets;
@property (nonatomic, assign) BOOL hasEnded;

@end
