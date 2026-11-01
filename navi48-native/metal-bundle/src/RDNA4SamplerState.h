#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@class RDNA4Device;

/**
 * RDNA4SamplerState - Metal sampler backed by a GFX12 S# descriptor.
 *
 * The descriptor lives in the driver's user-visible VRAM window; the encoder
 * passes its GPU address to the shader through the resource binding slot.
 */
@interface RDNA4SamplerState : NSObject <MTLSamplerState>

@property (readonly) id<MTLDevice> device;
@property (copy) NSString* label;

- (instancetype)initWithDevice:(RDNA4Device*)device
                    descriptor:(MTLSamplerDescriptor*)descriptor;

- (uint64_t)gpuAddress;

@end
