#import "RDNA4ComputeCommandEncoder.h"
#import "RDNA4CommandBuffer.h"
#import "RDNA4ComputePipelineState.h"
#import "RDNA4Buffer.h"
#import "RDNA4Device.h"

// Import PM4Builder (kernel version would be copied to user-space or use similar logic)
// For user-space, we'll implement a simplified PM4 builder

@interface RDNA4PM4Builder : NSObject
- (void)reset;
- (void)setComputeProgram:(uint64_t)addr rsrc1:(uint32_t)rsrc1 rsrc2:(uint32_t)rsrc2;
- (void)setComputeWorkgroupSize:(uint32_t)x y:(uint32_t)y z:(uint32_t)z;
- (void)setComputeUserData:(uint32_t)index address:(uint64_t)addr;
- (void)emitDispatchDirect:(uint32_t)gridX y:(uint32_t)gridY z:(uint32_t)gridZ;
- (void)emitCsPartialFlush;
- (void)emitFenceWrite:(uint64_t)fenceAddr value:(uint64_t)fenceValue;
- (NSData*)getPackets;
@end

@implementation RDNA4ComputeCommandEncoder {
    RDNA4CommandBuffer* _commandBuffer;
    RDNA4ComputeEncoderState* _state;
    NSMutableData* _pm4Data;
}

- (instancetype)initWithCommandBuffer:(RDNA4CommandBuffer*)commandBuffer {
    self = [super init];
    if (self) {
        _commandBuffer = commandBuffer;
        _state = [[RDNA4ComputeEncoderState alloc] init];
        _state.buffers = [NSMutableDictionary dictionary];
        _state.bufferOffsets = [NSMutableDictionary dictionary];
        _pm4Data = [NSMutableData data];
    }
    return self;
}

- (id<MTLDevice>)device {
    return _commandBuffer.device;
}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)state {
    if (_state.hasEnded) {
        NSLog(@"RDNA4ComputeCommandEncoder: Cannot modify after endEncoding");
        return;
    }
    
    _state.pipelineState = (RDNA4ComputePipelineState*)state;
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (_state.hasEnded) {
        NSLog(@"RDNA4ComputeCommandEncoder: Cannot modify after endEncoding");
        return;
    }
    
    if (index > 31) {
        NSLog(@"RDNA4ComputeCommandEncoder: Buffer index out of range (max 31)");
        return;
    }
    
    _state.buffers[@(index)] = buffer;
    _state.bufferOffsets[@(index)] = @(offset);
}

- (void)setBuffers:(const id<MTLBuffer>*)buffers
           offsets:(const NSUInteger*)offsets
         withRange:(NSRange)range {
    for (NSUInteger i = 0; i < range.length; i++) {
        NSUInteger index = range.location + i;
        if (buffers[i]) {
            [self setBuffer:buffers[i] offset:offsets[i] atIndex:index];
        }
    }
}

- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index {
    // LDS allocation - not yet implemented
    NSLog(@"RDNA4ComputeCommandEncoder: setThreadgroupMemoryLength not yet implemented");
}

- (void)dispatchThreadgroups:(MTLSize)threadgroupsPerGrid
       threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup {
    if (_state.hasEnded) {
        NSLog(@"RDNA4ComputeCommandEncoder: Cannot dispatch after endEncoding");
        return;
    }
    
    if (!_state.pipelineState) {
        NSLog(@"RDNA4ComputeCommandEncoder: No pipeline state set");
        return;
    }
    
    // Build PM4 commands
    RDNA4PM4Builder* pm4 = [[RDNA4PM4Builder alloc] init];
    
    // Set compute shader program
    [pm4 setComputeProgram:_state.pipelineState.shaderGPUAddress
                     rsrc1:_state.pipelineState.computePgmRsrc1
                     rsrc2:_state.pipelineState.computePgmRsrc2];
    
    // Set workgroup size
    [pm4 setComputeWorkgroupSize:(uint32_t)threadsPerThreadgroup.width
                               y:(uint32_t)threadsPerThreadgroup.height
                               z:(uint32_t)threadsPerThreadgroup.depth];
    
    // Set buffer addresses (user data SGPRs)
    for (NSNumber* indexNum in _state.buffers) {
        NSUInteger index = [indexNum unsignedIntegerValue];
        RDNA4Buffer* buffer = (RDNA4Buffer*)_state.buffers[indexNum];
        NSUInteger offset = [_state.bufferOffsets[indexNum] unsignedIntegerValue];
        
        uint64_t gpuAddress = [buffer gpuAddress] + offset;
        [pm4 setComputeUserData:(uint32_t)index address:gpuAddress];
    }
    
    // Dispatch
    [pm4 emitDispatchDirect:(uint32_t)threadgroupsPerGrid.width
                          y:(uint32_t)threadgroupsPerGrid.height
                          z:(uint32_t)threadgroupsPerGrid.depth];
    
    // Flush
    [pm4 emitCsPartialFlush];
    
    // Append to command buffer's PM4 data
    [_pm4Data appendData:[pm4 getPackets]];
    
    NSLog(@"RDNA4ComputeCommandEncoder: Dispatched (%zu,%zu,%zu) threadgroups of (%zu,%zu,%zu) threads",
          threadgroupsPerGrid.width, threadgroupsPerGrid.height, threadgroupsPerGrid.depth,
          threadsPerThreadgroup.width, threadsPerThreadgroup.height, threadsPerThreadgroup.depth);
}

- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)indirectBuffer
                          indirectBufferOffset:(NSUInteger)indirectBufferOffset
                         threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup {
    NSLog(@"RDNA4ComputeCommandEncoder: Indirect dispatch not yet implemented");
}

- (void)endEncoding {
    if (_state.hasEnded) {
        return;
    }
    
    _state.hasEnded = YES;
    
    // Notify command buffer that this encoder has finished
    [_commandBuffer encoderDidEnd:self];
}

- (NSData*)encodedCommands {
    return [_pm4Data copy];
}

// Stub implementations for other MTLComputeCommandEncoder methods
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {}
- (void)setBytes:(const void*)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {}
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage {}
- (void)useHeap:(id<MTLHeap>)heap {}
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope {}
- (void)updateFence:(id<MTLFence>)fence {}
- (void)waitForFence:(id<MTLFence>)fence {}
- (void)pushDebugGroup:(NSString*)string {}
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString*)string {}

@end

@implementation RDNA4ComputeEncoderState
@end

// Simple PM4Builder for user-space
@implementation RDNA4PM4Builder {
    NSMutableData* _packets;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _packets = [NSMutableData data];
    }
    return self;
}

- (void)reset {
    [_packets setLength:0];
}

- (uint32_t)makeType3Header:(uint8_t)opcode count:(uint32_t)count {
    return (3u << 30) | ((count & 0x3FFF) << 16) | ((uint32_t)opcode << 8);
}

- (void)appendDword:(uint32_t)dword {
    [_packets appendBytes:&dword length:4];
}

- (void)setComputeProgram:(uint64_t)addr rsrc1:(uint32_t)rsrc1 rsrc2:(uint32_t)rsrc2 {
    uint32_t lo = (uint32_t)(addr >> 8);  // Bits [39:8]
    uint32_t hi = (uint32_t)(addr >> 40); // Bits [47:40]
    
    [self appendDword:[self makeType3Header:0x76 count:4]]; // SET_SH_REG
    [self appendDword:0x20c]; // COMPUTE_PGM_LO
    [self appendDword:lo];
    [self appendDword:hi];
    [self appendDword:rsrc1];
    [self appendDword:rsrc2];
}

- (void)setComputeWorkgroupSize:(uint32_t)x y:(uint32_t)y z:(uint32_t)z {
    [self appendDword:[self makeType3Header:0x76 count:3]]; // SET_SH_REG
    [self appendDword:0x215]; // COMPUTE_NUM_THREAD_X
    [self appendDword:x];
    [self appendDword:y];
    [self appendDword:z];
}

- (void)setComputeUserData:(uint32_t)index address:(uint64_t)addr {
    uint32_t lo = (uint32_t)(addr & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(addr >> 32);
    uint32_t regOffset = 0x240 + (index * 2);
    
    [self appendDword:[self makeType3Header:0x76 count:2]]; // SET_SH_REG
    [self appendDword:regOffset];
    [self appendDword:lo];
    [self appendDword:hi];
}

- (void)emitDispatchDirect:(uint32_t)gridX y:(uint32_t)gridY z:(uint32_t)gridZ {
    uint32_t initiator = (1u << 0) | (1u << 2) | (1u << 18); // COMPUTE_SHADER_EN | FORCE_START | CS_W32_EN
    
    [self appendDword:[self makeType3Header:0x15 count:3]]; // DISPATCH_DIRECT
    [self appendDword:gridX];
    [self appendDword:gridY];
    [self appendDword:gridZ];
    [self appendDword:initiator];
}

- (void)emitCsPartialFlush {
    [self appendDword:[self makeType3Header:0x46 count:0]]; // EVENT_WRITE
    [self appendDword:0x07]; // CS_PARTIAL_FLUSH
}

- (void)emitFenceWrite:(uint64_t)fenceAddr value:(uint64_t)fenceValue {
    uint32_t addrLo = (uint32_t)(fenceAddr & 0xFFFFFFFF);
    uint32_t addrHi = (uint32_t)(fenceAddr >> 32);
    uint32_t dataLo = (uint32_t)(fenceValue & 0xFFFFFFFF);
    uint32_t dataHi = (uint32_t)(fenceValue >> 32);
    
    uint32_t control = (1u << 12) | (2u << 29); // EVENT_WRITE | DATA64
    
    [self appendDword:[self makeType3Header:0x59 count:5]]; // RELEASE_MEM
    [self appendDword:control | 0x07]; // CS_PARTIAL_FLUSH
    [self appendDword:0];
    [self appendDword:addrLo];
    [self appendDword:addrHi];
    [self appendDword:dataLo];
    [self appendDword:dataHi];
}

- (NSData*)getPackets {
    return [_packets copy];
}

@end
