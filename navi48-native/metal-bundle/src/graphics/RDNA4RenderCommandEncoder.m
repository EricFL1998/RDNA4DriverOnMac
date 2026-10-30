#import "RDNA4RenderCommandEncoder.h"
#import "RDNA4CommandBuffer.h"
#import "RDNA4RenderPipelineState.h"
#import "RDNA4Buffer.h"
#import "RDNA4Texture.h"
#import "RDNA4RenderPassDescriptor.h"
#import "RDNA4Device.h"

#import "RDNA4RenderPM4Builder.h"

@implementation RDNA4RenderCommandEncoder {
    RDNA4CommandBuffer* _commandBuffer;
    RDNA4Device* _device;
    
    // Render pass configuration
    MTLRenderPassDescriptor* _renderPassDescriptor;
    
    // Current pipeline state
    RDNA4RenderPipelineState* _currentPipelineState;
    
    // Vertex buffers (up to 31)
    id<MTLBuffer> _vertexBuffers[31];
    NSUInteger _vertexBufferOffsets[31];
    
    // Fragment buffers (up to 31)
    id<MTLBuffer> _fragmentBuffers[31];
    NSUInteger _fragmentBufferOffsets[31];
    
    // Textures
    id<MTLTexture> _vertexTextures[31];
    id<MTLTexture> _fragmentTextures[31];
    
    // Render state
    MTLViewport _viewport;
    MTLScissorRect _scissor;
    MTLCullMode _cullMode;
    MTLWinding _frontFaceWinding;
    MTLTriangleFillMode _fillMode;
    
    // PM4 builder
    RDNA4RenderPM4Builder* _pm4Builder;
    // Set only when the pipeline carries usable program registers.
    BOOL _pipelineReady;
    
    // Encoding state
    BOOL _isEncoding;
    BOOL _hasSetPipeline;
}

- (instancetype)initWithCommandBuffer:(RDNA4CommandBuffer*)commandBuffer
               renderPassDescriptor:(MTLRenderPassDescriptor*)descriptor {
    self = [super init];
    if (self) {
        _commandBuffer = commandBuffer;
        _device = (RDNA4Device*)commandBuffer.device;
        _renderPassDescriptor = [descriptor copy];
        
        // Initialize PM4 builder
        _pm4Builder = [[RDNA4RenderPM4Builder alloc] init];
        
        // Set default state
        _cullMode = MTLCullModeNone;
        _frontFaceWinding = MTLWindingCounterClockwise;
        _fillMode = MTLTriangleFillModeFill;
        
        // Default viewport and scissor (will be set explicitly)
        _viewport = (MTLViewport){0, 0, 0, 0, 0, 1};
        _scissor = (MTLScissorRect){0, 0, 0, 0};
        
        _isEncoding = YES;
        _hasSetPipeline = NO;
        
        // Encode render pass setup
        [self encodeRenderPassBegin];
        
        NSLog(@"RDNA4RenderCommandEncoder: initialized");
    }
    return self;
}

- (void)encodeRenderPassBegin {
    // Configure render targets from render pass descriptor
    for (NSUInteger i = 0; i < 8; i++) {
        RDNA4RenderPassColorAttachmentDescriptor* attachment = 
            [_renderPassDescriptor.colorAttachments objectAtIndexedSubscript:i];
        
        if (attachment.texture) {
            RDNA4Texture* texture = (RDNA4Texture*)attachment.texture;
            
            // Set color buffer
            [_pm4Builder setColorBuffer:i texture:texture];
            
            // Handle load action
            if (attachment.loadAction == MTLLoadActionClear) {
                // TODO: Emit clear commands
                NSLog(@"RDNA4RenderCommandEncoder: clear color %lu to (%.2f, %.2f, %.2f, %.2f)",
                      i, attachment.clearColor.red, attachment.clearColor.green,
                      attachment.clearColor.blue, attachment.clearColor.alpha);
            }
        }
    }
    
    // TODO: Handle depth/stencil attachments
}

- (void)endEncoding {
    if (!_isEncoding) {
        NSLog(@"RDNA4RenderCommandEncoder: endEncoding called but not encoding");
        return;
    }
    
    // Flush caches
    [_pm4Builder flushCaches];
    
    // Get encoded commands
    NSData* commands = [_pm4Builder getPackets];
    
    // Add to command buffer
    [_commandBuffer appendRenderCommands:commands.bytes length:commands.length];
    
    _isEncoding = NO;
    
    NSLog(@"RDNA4RenderCommandEncoder: encoding ended, %lu bytes", commands.length);
}

- (id<MTLDevice>)device {
    return _device;
}

- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipelineState {
    if (!_isEncoding) {
        NSLog(@"RDNA4RenderCommandEncoder: setRenderPipelineState called after encoding ended");
        return;
    }
    
    _currentPipelineState = (RDNA4RenderPipelineState*)pipelineState;
    _hasSetPipeline = YES;
    
    // Program registers have to match the shader the compiler produced. Until
    // the compiler emits SPI_SHADER_PGM_RSRC1/2_GS/PS, drawing would run with
    // resource limits that do not describe the code, so draws are refused.
    uint64_t vsAddr = [_currentPipelineState vertexShaderGPUAddress];
    RDNA4CompiledShader* vs = [_currentPipelineState vertexShader];
    RDNA4CompiledShader* fs = [_currentPipelineState fragmentShader];
    const BOOL vsReady = vsAddr != 0 && vs.pgmRsrc1 != 0;
    const BOOL fsReady = fs == nil || (fs.pgmRsrc1 != 0);

    if (vsReady) {
        [_pm4Builder setVertexShader:vsAddr rsrc1:vs.pgmRsrc1 rsrc2:vs.pgmRsrc2];
    }
    if (fs && fsReady) {
        [_pm4Builder setFragmentShader:[_currentPipelineState fragmentShaderGPUAddress]
                                 rsrc1:fs.pgmRsrc1 rsrc2:fs.pgmRsrc2];
    }
    _pipelineReady = vsReady && fsReady;
    if (!_pipelineReady) {
        NSLog(@"RDNA4RenderCommandEncoder: shader metadata missing "
              @"(SPI_SHADER_PGM_RSRC1/2) - draws will be refused, not guessed");
    }
    
    NSLog(@"RDNA4RenderCommandEncoder: set pipeline state");
}

- (void)setVertexBuffer:(id<MTLBuffer>)buffer
                 offset:(NSUInteger)offset
                atIndex:(NSUInteger)index {
    if (index >= 31) {
        return;
    }
    
    _vertexBuffers[index] = buffer;
    _vertexBufferOffsets[index] = offset;
    
    if (buffer) {
        RDNA4Buffer* rdnaBuffer = (RDNA4Buffer*)buffer;
        uint64_t gpuAddr = [rdnaBuffer gpuAddress] + offset;
        
        // Calculate stride and count from buffer size
        // (In real implementation, would come from vertex descriptor)
        uint32_t stride = 16;  // Assume 16 bytes per vertex
        uint32_t count = (uint32_t)(buffer.length / stride);
        
        [_pm4Builder setVertexBuffer:index address:gpuAddr stride:stride count:count];
        
        NSLog(@"RDNA4RenderCommandEncoder: set vertex buffer %lu, GPU=0x%llx", index, gpuAddr);
    }
}

- (void)setVertexBuffers:(const id<MTLBuffer>*)buffers
                 offsets:(const NSUInteger*)offsets
               withRange:(NSRange)range {
    for (NSUInteger i = 0; i < range.length; i++) {
        NSUInteger index = range.location + i;
        if (index < 31) {
            [self setVertexBuffer:buffers[i] offset:offsets[i] atIndex:index];
        }
    }
}

- (void)setFragmentBuffer:(id<MTLBuffer>)buffer
                   offset:(NSUInteger)offset
                  atIndex:(NSUInteger)index {
    if (index >= 31) {
        return;
    }
    
    _fragmentBuffers[index] = buffer;
    _fragmentBufferOffsets[index] = offset;
    
    // TODO: Emit PM4 for fragment buffer
}

- (void)setVertexTexture:(id<MTLTexture>)texture
                 atIndex:(NSUInteger)index {
    if (index >= 31) {
        return;
    }
    
    _vertexTextures[index] = texture;
    
    // TODO: Emit PM4 for vertex texture
}

- (void)setFragmentTexture:(id<MTLTexture>)texture
                   atIndex:(NSUInteger)index {
    if (index >= 31) {
        return;
    }
    
    _fragmentTextures[index] = texture;
    
    // TODO: Emit PM4 for fragment texture
}

- (void)setViewport:(MTLViewport)viewport {
    _viewport = viewport;
    [_pm4Builder setViewport:viewport];
    
    NSLog(@"RDNA4RenderCommandEncoder: set viewport (%.1f, %.1f, %.1fx%.1f)",
          viewport.originX, viewport.originY, viewport.width, viewport.height);
}

- (void)setScissorRect:(MTLScissorRect)rect {
    _scissor = rect;
    [_pm4Builder setScissor:rect];
    
    NSLog(@"RDNA4RenderCommandEncoder: set scissor (%lu, %lu, %lux%lu)",
          rect.x, rect.y, rect.width, rect.height);
}

- (void)setCullMode:(MTLCullMode)cullMode {
    _cullMode = cullMode;
    [_pm4Builder setCullMode:cullMode winding:_frontFaceWinding];
}

- (void)setFrontFacingWinding:(MTLWinding)frontFacingWinding {
    _frontFaceWinding = frontFacingWinding;
    [_pm4Builder setCullMode:_cullMode winding:frontFacingWinding];
}

- (void)setTriangleFillMode:(MTLTriangleFillMode)fillMode {
    _fillMode = fillMode;
    // TODO: Emit PM4 for fill mode
}

- (void)setBlendColorRed:(float)red
                   green:(float)green
                    blue:(float)blue
                   alpha:(float)alpha {
    // TODO: Emit PM4 for blend color
}

- (void)setDepthBias:(float)depthBias
          slopeScale:(float)slopeScale
               clamp:(float)clamp {
    // TODO: Emit PM4 for depth bias
}

- (void)drawPrimitives:(MTLPrimitiveType)primitiveType
           vertexStart:(NSUInteger)vertexStart
           vertexCount:(NSUInteger)vertexCount {
    [self drawPrimitives:primitiveType
             vertexStart:vertexStart
             vertexCount:vertexCount
           instanceCount:1];
}

- (void)drawPrimitives:(MTLPrimitiveType)primitiveType
           vertexStart:(NSUInteger)vertexStart
           vertexCount:(NSUInteger)vertexCount
         instanceCount:(NSUInteger)instanceCount {
    
    if (!_hasSetPipeline) {
        NSLog(@"RDNA4RenderCommandEncoder: drawPrimitives called without pipeline state");
        return;
    }
    
    [_pm4Builder drawPrimitives:primitiveType
                          start:vertexStart
                          count:vertexCount
                      instances:instanceCount];
    
    NSLog(@"RDNA4RenderCommandEncoder: draw %lu vertices (type=%lu, start=%lu, instances=%lu)",
          vertexCount, (unsigned long)primitiveType, vertexStart, instanceCount);
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType
                   indexCount:(NSUInteger)indexCount
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer
            indexBufferOffset:(NSUInteger)indexBufferOffset {
    
    [self drawIndexedPrimitives:primitiveType
                     indexCount:indexCount
                      indexType:indexType
                    indexBuffer:indexBuffer
              indexBufferOffset:indexBufferOffset
                  instanceCount:1];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType
                   indexCount:(NSUInteger)indexCount
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer
            indexBufferOffset:(NSUInteger)indexBufferOffset
                instanceCount:(NSUInteger)instanceCount {
    
    if (!_hasSetPipeline) {
        NSLog(@"RDNA4RenderCommandEncoder: drawIndexedPrimitives called without pipeline state");
        return;
    }
    
    RDNA4Buffer* rdnaIndexBuffer = (RDNA4Buffer*)indexBuffer;
    uint64_t indexBufferAddr = [rdnaIndexBuffer gpuAddress] + indexBufferOffset;
    
    [_pm4Builder drawIndexed:primitiveType
                  indexCount:indexCount
                 indexBuffer:indexBufferAddr
                   indexType:indexType
                   instances:instanceCount];
    
    NSLog(@"RDNA4RenderCommandEncoder: draw indexed %lu indices (type=%lu, instances=%lu)",
          indexCount, (unsigned long)primitiveType, instanceCount);
}

- (NSData*)getEncodedCommands {
    return [_pm4Builder getPackets];
}

// MTLCommandEncoder protocol methods

- (void)insertDebugSignpost:(NSString*)string {}
- (void)pushDebugGroup:(NSString*)string {}
- (void)popDebugGroup {}

@end
