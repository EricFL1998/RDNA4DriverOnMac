#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@class RDNA4Device;
@class RDNA4Function;
@class RDNA4CompiledShader;

/**
 * RDNA4ComputePipelineState - Compute Pipeline State
 * 
 * Represents a compiled compute pipeline ready for execution.
 * Contains the shader uploaded to VRAM and all execution metadata.
 */
@interface RDNA4ComputePipelineState : NSObject <MTLComputePipelineState>

@property (readonly) id<MTLDevice> device;
@property (copy) NSString* label;
@property (readonly) NSUInteger maxTotalThreadsPerThreadgroup;
@property (readonly) NSUInteger threadExecutionWidth;
@property (readonly) NSUInteger staticThreadgroupMemoryLength;

/**
 * Initialize with function
 * @param device Parent device
 * @param function Compiled compute function
 * @param error Output error
 */
- (instancetype)initWithDevice:(RDNA4Device*)device
                      function:(RDNA4Function*)function
                         error:(NSError**)error;

/**
 * Get shader GPU address (internal use)
 */
- (uint64_t)shaderGPUAddress;

/**
 * Get COMPUTE_PGM_RSRC1 register value
 */
- (uint32_t)computePgmRsrc1;

/**
 * Get COMPUTE_PGM_RSRC2 register value
 */
- (uint32_t)computePgmRsrc2;

/**
 * Get compiled shader metadata (internal use)
 */
- (RDNA4CompiledShader*)compiledShader;

@end
