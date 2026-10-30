#import "RDNA4CommandBuffer.h"
#import "RDNA4UserClient.h"
#import "RDNA4CommandQueue.h"
#import "RDNA4ComputeCommandEncoder.h"
#import "RDNA4Device.h"
#import <IOKit/IOKitLib.h>

@implementation RDNA4CommandBuffer {
    RDNA4CommandQueue* _commandQueue;
    MTLCommandBufferStatus _status;
    NSMutableArray<RDNA4ComputeCommandEncoder*>* _encoders;
    NSMutableData* _allCommands;
    uint64_t _fenceValue;
    dispatch_semaphore_t _completionSemaphore;
}

- (instancetype)initWithQueue:(RDNA4CommandQueue*)queue {
    self = [super init];
    if (self) {
        _commandQueue = queue;
        _status = MTLCommandBufferStatusNotEnqueued;
        _encoders = [NSMutableArray array];
        _allCommands = [NSMutableData data];
        _completionSemaphore = dispatch_semaphore_create(0);
        _fenceValue = 0;
    }
    return self;
}

- (id<MTLDevice>)device {
    return _commandQueue.device;
}

- (id<MTLCommandQueue>)commandQueue {
    return _commandQueue;
}

- (MTLCommandBufferStatus)status {
    return _status;
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
    RDNA4ComputeCommandEncoder* encoder = [[RDNA4ComputeCommandEncoder alloc] initWithCommandBuffer:self];
    [_encoders addObject:encoder];
    return encoder;
}

- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor*)renderPassDescriptor {
    NSLog(@"RDNA4CommandBuffer: Render encoding not yet supported");
    return nil;
}

- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
    NSLog(@"RDNA4CommandBuffer: Blit encoding not yet supported");
    return nil;
}

- (void)encoderDidEnd:(RDNA4ComputeCommandEncoder*)encoder {
    // Collect encoded commands from encoder
    NSData* commands = [encoder encodedCommands];
    if (commands.length > 0) {
        [_allCommands appendData:commands];
    }
}

- (void)enqueue {
    if (_status != MTLCommandBufferStatusNotEnqueued) {
        return;
    }
    _status = MTLCommandBufferStatusEnqueued;
}

- (void)commit {
    if (_status == MTLCommandBufferStatusNotEnqueued) {
        [self enqueue];
    }
    
    _status = MTLCommandBufferStatusCommitted;
    
    // Submit all commands to GPU via IOUserClient
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        [self submitToGPU];
    });
}

- (void)submitToGPU {
    RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
    if (!link) {
        NSLog(@"RDNA4CommandBuffer: RDNA4Driver service is not available");
        _status = MTLCommandBufferStatusError;
        [self notifyCompletion];
        return;
    }

    _status = MTLCommandBufferStatusScheduled;

    // The driver allocates the fence value and appends the RELEASE_MEM packet
    // itself; a user-space counter here could never match the GPU's slot.
    uint64_t fence = 0;
    if (RDNA4GpuSubmit(link, _allCommands.bytes, _allCommands.length, &fence) != 0) {
        NSLog(@"RDNA4CommandBuffer: command submission failed");
        _status = MTLCommandBufferStatusError;
        [self notifyCompletion];
        return;
    }
    _fenceValue = fence;

    // Timeout is milliseconds, matching the driver's WaitFence contract.
    if (RDNA4GpuWait(link, fence, 5000) == 0) {
        _status = MTLCommandBufferStatusCompleted;
    } else {
        NSLog(@"RDNA4CommandBuffer: fence %llu did not complete", (unsigned long long)fence);
        _status = MTLCommandBufferStatusError;
    }
    [self notifyCompletion];
}


- (void)notifyCompletion {
    dispatch_semaphore_signal(_completionSemaphore);
    
    // Call completion handlers
    // In real implementation, would iterate through registered handlers
}

- (void)waitUntilCompleted {
    if (_status >= MTLCommandBufferStatusCompleted) {
        return;
    }
    
    dispatch_semaphore_wait(_completionSemaphore, DISPATCH_TIME_FOREVER);
}

- (void)waitUntilScheduled {
    while (_status < MTLCommandBufferStatusScheduled) {
        usleep(1000);
    }
}

- (void)addCompletedHandler:(MTLCommandBufferHandler)block {
    // Store handler and call when completed
    // Simplified - real implementation would queue these
    if (_status >= MTLCommandBufferStatusCompleted) {
        block(self);
    }
}

- (void)addScheduledHandler:(MTLCommandBufferHandler)block {
    if (_status >= MTLCommandBufferStatusScheduled) {
        block(self);
    }
}

- (void)presentDrawable:(id<MTLDrawable>)drawable {
    NSLog(@"RDNA4CommandBuffer: presentDrawable not yet supported");
}

- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)presentationTime {
    NSLog(@"RDNA4CommandBuffer: presentDrawable not yet supported");
}

- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)duration {
    NSLog(@"RDNA4CommandBuffer: presentDrawable not yet supported");
}

- (NSError*)error {
    return nil;  // TODO: Track errors
}

- (CFTimeInterval)kernelStartTime {
    return 0;
}

- (CFTimeInterval)kernelEndTime {
    return 0;
}

- (CFTimeInterval)GPUStartTime {
    return 0;
}

- (CFTimeInterval)GPUEndTime {
    return 0;
}

- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value {}
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value {}
- (void)pushDebugGroup:(NSString*)string {}
- (void)popDebugGroup {}

@end
