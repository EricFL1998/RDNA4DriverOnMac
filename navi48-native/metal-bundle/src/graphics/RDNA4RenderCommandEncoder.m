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
    // CONTEXT_CONTROL must come first: it makes the CP load and shadow the
    // per-context state, which is what every SET_CONTEXT_REG below lands in.
    [_pm4Builder enableContextState];
    // gfx12 has no CLEAR_STATE packet, so nothing has initialised the context
    // registers since power-on. The preamble writes the state the hardware
    // would otherwise invent.
    [_pm4Builder emitGraphicsPreamble];
    // Context defaults MUST come after CONTEXT_CONTROL, otherwise CONTEXT_CONTROL's
    // LOAD_ENABLE blows away these context registers!
    [_pm4Builder setDepthDisabled];
    [_pm4Builder setColorWriteMask:0xF shaderMask:0xF];
    [_pm4Builder setBlendDisabled];
    [_pm4Builder setCullMode:MTLCullModeNone winding:MTLWindingClockwise];
    // Shader code was uploaded before this command buffer was recorded, so the
    // instruction caches have to be invalidated before the draws run.
    [_pm4Builder invalidateShaderCaches];
    for (NSUInteger i = 0; i < 8; i++) {
        RDNA4RenderPassColorAttachmentDescriptor* attachment =
            [_renderPassDescriptor.colorAttachments objectAtIndexedSubscript:i];
        if (!attachment.texture) continue;
        RDNA4Texture* texture = (RDNA4Texture*)attachment.texture;
        [_pm4Builder setColorBuffer:i texture:texture];
        if (attachment.loadAction == MTLLoadActionClear) {
            if (texture.pixelFormat != MTLPixelFormatBGRA8Unorm) {
                [self failEncoding:@"Only BGRA8 clear is supported"];
                continue;
            }
            NSUInteger rowBytes = [texture bytesPerRowForLevel:0];
            NSMutableData* pixels = [NSMutableData dataWithLength:rowBytes * texture.height];
            const uint32_t r = (uint32_t)(attachment.clearColor.red * 255.0 + 0.5);
            const uint32_t g = (uint32_t)(attachment.clearColor.green * 255.0 + 0.5);
            const uint32_t b = (uint32_t)(attachment.clearColor.blue * 255.0 + 0.5);
            const uint32_t a = (uint32_t)(attachment.clearColor.alpha * 255.0 + 0.5);
            const uint32_t color = r | (g << 8) | (b << 16) | (a << 24);
            uint32_t* target = (uint32_t*)pixels.mutableBytes;
            for (NSUInteger pixel = 0; pixel < texture.width * texture.height; ++pixel) target[pixel] = color;
            [texture replaceRegion:MTLRegionMake2D(0, 0, texture.width, texture.height)
                       mipmapLevel:0 slice:0 withBytes:pixels.bytes
                       bytesPerRow:rowBytes bytesPerImage:pixels.length];
        }
    }
}

- (void)endEncoding {
    if (!_isEncoding) return;
    _isEncoding = NO;
    // The kernel appends a RELEASE_MEM carrying CACHE_FLUSH_AND_INV_TS to every submission,
    // so an explicit EVENT_WRITE here is unnecessary and wedges the CP.
    NSData* commands = [_pm4Builder getPackets];
    if (!commands.length) {
        [self failEncoding:@"Render commands were rejected or are empty"];
        return;
    }
    [_commandBuffer appendRenderCommands:commands.bytes length:commands.length];
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
    
    uint64_t vsAddr = [_currentPipelineState vertexShaderGPUAddress];
    RDNA4CompiledShader* vs = [_currentPipelineState vertexShader];
    RDNA4CompiledShader* fs = [_currentPipelineState fragmentShader];
    const BOOL vsReady = vsAddr != 0 && vs.pgmRsrc1 != 0;
    const BOOL fsReady = fs == nil || (fs.pgmRsrc1 != 0);

    if (vsReady) {
        [_pm4Builder setVertexShader:vsAddr rsrc1:vs.pgmRsrc1 rsrc2:vs.pgmRsrc2
                     parameterCount:vs.vertexParameterCount
               attributeRingAddress:_currentPipelineState.attributeRingGPUAddress
                 attributeRingSize:_currentPipelineState.attributeRingSize];
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

- (void)failEncoding:(NSString*)message {
    _pipelineReady = NO;
    [_commandBuffer recordEncodingError:[NSError errorWithDomain:@"com.rdna4.metal"
                                                            code:-10
                                                        userInfo:@{NSLocalizedDescriptionKey: message}]];
}

- (void)setVertexBuffer:(id<MTLBuffer>)buffer
                 offset:(NSUInteger)offset
                atIndex:(NSUInteger)index {
    if (!buffer) return;
    if (index != 0 || offset > buffer.length || !_currentPipelineState.vertexDescriptor) {
        [self failEncoding:@"Only an explicit single packed vertex buffer is supported"];
        return;
    }
    MTLVertexDescriptor* descriptor = _currentPipelineState.vertexDescriptor;
    NSUInteger expectedOffset = 0;
    for (NSUInteger i = 0; i < 31; ++i) {
        MTLVertexAttributeDescriptor* attribute = descriptor.attributes[i];
        NSUInteger bytes = 0;
        switch (attribute.format) {
            case MTLVertexFormatInvalid: continue;
            case MTLVertexFormatFloat: bytes = 4; break;
            case MTLVertexFormatFloat2: bytes = 8; break;
            case MTLVertexFormatFloat3: bytes = 12; break;
            case MTLVertexFormatFloat4: bytes = 16; break;
            default:
                [self failEncoding:@"Only float vertex attributes are supported"];
                return;
        }
        if (attribute.bufferIndex != index || attribute.offset != expectedOffset) {
            [self failEncoding:@"Vertex attributes must be packed in shader order"];
            return;
        }
        expectedOffset += bytes;
    }
    MTLVertexBufferLayoutDescriptor* layout = descriptor.layouts[index];
    if (layout.stepFunction != MTLVertexStepFunctionPerVertex || layout.stepRate != 1 ||
        layout.stride != expectedOffset) {
        [self failEncoding:@"Vertex layout does not match the packed shader inputs"];
        return;
    }

    _vertexBuffers[index] = buffer;
    _vertexBufferOffsets[index] = offset;
    RDNA4Buffer* rdnaBuffer = (RDNA4Buffer*)buffer;
    uint64_t gpuAddr = rdnaBuffer.gpuAddress + offset;
    uint32_t count = (uint32_t)((buffer.length - offset) / expectedOffset);
    [_pm4Builder setVertexBuffer:index address:gpuAddr stride:(uint32_t)expectedOffset count:count];
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
    
    if (!_hasSetPipeline || !_pipelineReady || !_vertexBuffers[0] ||
        vertexCount == 0 || vertexStart != 0 || instanceCount != 1) {
        [self failEncoding:@"Draw state is incomplete or unsupported"];
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
