#import "RDNA4CommandQueue.h"
#import "RDNA4CommandBuffer.h"
#import "RDNA4Device.h"

@implementation RDNA4CommandQueue {
    RDNA4Device* _device;
}

- (instancetype)initWithDevice:(RDNA4Device*)device {
    self = [super init];
    if (self) {
        _device = device;
    }
    return self;
}

- (id<MTLDevice>)device {
    return _device;
}

- (id<MTLCommandBuffer>)commandBuffer {
    return [[RDNA4CommandBuffer alloc] initWithQueue:self];
}

- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences {
    // Unretained references optimization not yet implemented
    return [self commandBuffer];
}

- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor*)descriptor {
    // Command buffer descriptors not yet supported
    return [self commandBuffer];
}

- (void)insertDebugCaptureBoundary {}

@end
