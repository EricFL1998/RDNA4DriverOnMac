#import "RDNA4Device.h"
#import "RDNA4CommandQueue.h"
#import "RDNA4Buffer.h"
#import "RDNA4Texture.h"
#import "RDNA4Library.h"
#import "RDNA4Function.h"
#import "RDNA4ComputePipelineState.h"

@implementation RDNA4Device {
    NSString* _name;
    uint64_t _registryID;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _name = @"AMD Radeon RX 9070 XT (RDNA4)";
        _registryID = 0x7550;  // Device ID
    }
    return self;
}

#pragma mark - Device Properties

- (NSString*)name {
    return _name;
}

- (uint64_t)registryID {
    return _registryID;
}

- (MTLSize)maxThreadsPerThreadgroup {
    return MTLSizeMake(1024, 1024, 1024);  // RDNA4 limit
}

- (BOOL)isLowPower {
    return NO;  // Discrete GPU
}

- (BOOL)isHeadless {
    return NO;
}

- (BOOL)isRemovable {
    return YES;  // PCIe card
}

- (uint64_t)recommendedMaxWorkingSetSize {
    return 16ULL * 1024 * 1024 * 1024;  // 16 GB VRAM
}

- (MTLDeviceLocation)location {
    return MTLDeviceLocationExternal;
}

- (uint64_t)locationNumber {
    return 0;
}

- (uint64_t)maxTransferRate {
    return 16ULL * 1000 * 1000 * 1000;  // PCIe 4.0 x16 = ~16 GB/s
}

- (BOOL)hasUnifiedMemory {
    return NO;  // Discrete GPU
}

- (NSUInteger)currentAllocatedSize {
    // TODO: Track allocations
    return 0;
}

#pragma mark - Feature Support

- (BOOL)supportsFamily:(MTLGPUFamily)gpuFamily {
    switch (gpuFamily) {
        case MTLGPUFamilyApple1:
        case MTLGPUFamilyApple2:
        case MTLGPUFamilyApple3:
        case MTLGPUFamilyApple4:
        case MTLGPUFamilyApple5:
        case MTLGPUFamilyApple6:
        case MTLGPUFamilyApple7:
        case MTLGPUFamilyApple8:
            return NO;  // Not Apple Silicon
        case MTLGPUFamilyMac1:
        case MTLGPUFamilyMac2:
            return YES;  // AMD discrete GPU
        case MTLGPUFamilyCommon1:
        case MTLGPUFamilyCommon2:
        case MTLGPUFamilyCommon3:
            return YES;
        default:
            return NO;
    }
}

- (BOOL)supportsFeatureSet:(MTLFeatureSet)featureSet {
    // Support macOS GPU Family 2
    return (featureSet == MTLFeatureSet_macOS_GPUFamily2_v1);
}

- (BOOL)supportsTextureSampleCount:(NSUInteger)sampleCount {
    return (sampleCount == 1 || sampleCount == 2 || sampleCount == 4);
}

- (NSUInteger)readWriteTextureSupport {
    return MTLReadWriteTextureTier2;
}

- (NSUInteger)argumentBuffersSupport {
    return MTLArgumentBuffersTier2;
}

- (BOOL)areRasterOrderGroupsSupported {
    return YES;
}

- (BOOL)supports32BitFloatFiltering {
    return YES;
}

- (BOOL)supports32BitMSAA {
    return YES;
}

- (BOOL)supportsQueryTextureLOD {
    return YES;
}

- (BOOL)supportsBCTextureCompression {
    return YES;  // AMD supports BC formats
}

- (BOOL)supportsPullModelInterpolation {
    return YES;
}

- (BOOL)areBarycentricCoordsSupported {
    return YES;
}

- (BOOL)supportsShaderBarycentricCoordinates {
    return YES;
}

#pragma mark - Command Queue

- (id<MTLCommandQueue>)newCommandQueue {
    return [[RDNA4CommandQueue alloc] initWithDevice:self];
}

- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)maxCommandBufferCount {
    return [[RDNA4CommandQueue alloc] initWithDevice:self];
}

#pragma mark - Buffer Creation

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    return [[RDNA4Buffer alloc] initWithDevice:self length:length options:options];
}

- (id<MTLBuffer>)newBufferWithBytes:(const void*)pointer 
                             length:(NSUInteger)length 
                            options:(MTLResourceOptions)options {
    return [[RDNA4Buffer alloc] initWithDevice:self bytes:pointer length:length options:options];
}

- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void*)pointer 
                                   length:(NSUInteger)length 
                                  options:(MTLResourceOptions)options 
                              deallocator:(void (^)(void*, NSUInteger))deallocator {
    NSLog(@"RDNA4Device: newBufferWithBytesNoCopy not yet implemented");
    return nil;
}

#pragma mark - Texture Creation

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor*)descriptor {
    NSLog(@"RDNA4Device: Texture creation not yet implemented");
    return nil;
}

- (id<MTLTexture>)newSharedTextureWithDescriptor:(MTLTextureDescriptor*)descriptor {
    NSLog(@"RDNA4Device: Shared texture not yet implemented");
    return nil;
}

- (id<MTLTexture>)newSharedTextureWithHandle:(MTLSharedTextureHandle*)sharedHandle {
    NSLog(@"RDNA4Device: Shared texture handle not yet implemented");
    return nil;
}

#pragma mark - Library Creation

- (id<MTLLibrary>)newDefaultLibrary {
    NSLog(@"RDNA4Device: Default library not available");
    return nil;
}

- (id<MTLLibrary>)newLibraryWithFile:(NSString*)filepath error:(NSError**)error {
    return [[RDNA4Library alloc] initWithDevice:self filepath:filepath error:error];
}

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError**)error {
    return [[RDNA4Library alloc] initWithDevice:self data:data error:error];
}

- (id<MTLLibrary>)newLibraryWithSource:(NSString*)source 
                               options:(MTLCompileOptions*)options 
                                 error:(NSError**)error {
    NSLog(@"RDNA4Device: Runtime compilation not yet implemented");
    if (error) {
        *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                     code:-1
                                 userInfo:@{NSLocalizedDescriptionKey: @"Runtime compilation not supported"}];
    }
    return nil;
}

- (void)newLibraryWithSource:(NSString*)source 
                     options:(MTLCompileOptions*)options 
           completionHandler:(MTLNewLibraryCompletionHandler)completionHandler {
    NSError* error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:-1
                                     userInfo:@{NSLocalizedDescriptionKey: @"Runtime compilation not supported"}];
    if (completionHandler) {
        completionHandler(nil, error);
    }
}

- (id<MTLLibrary>)newLibraryWithURL:(NSURL*)url error:(NSError**)error {
    return [self newLibraryWithFile:[url path] error:error];
}

#pragma mark - Compute Pipeline

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)computeFunction 
                                                             error:(NSError**)error {
    return [[RDNA4ComputePipelineState alloc] initWithDevice:self 
                                                     function:(RDNA4Function*)computeFunction
                                                        error:error];
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor*)descriptor
                                                              options:(MTLPipelineOption)options
                                                           reflection:(MTLAutoreleasedComputePipelineReflection*)reflection
                                                                error:(NSError**)error {
    // Use function from descriptor
    return [self newComputePipelineStateWithFunction:descriptor.computeFunction error:error];
}

- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)computeFunction 
                          completionHandler:(void (^)(id<MTLComputePipelineState>, NSError*))completionHandler {
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        NSError* error = nil;
        id<MTLComputePipelineState> state = [self newComputePipelineStateWithFunction:computeFunction error:&error];
        if (completionHandler) {
            completionHandler(state, error);
        }
    });
}

- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor*)descriptor
                                      options:(MTLPipelineOption)options
                            completionHandler:(void (^)(id<MTLComputePipelineState>, MTLComputePipelineReflection*, NSError*))completionHandler {
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        NSError* error = nil;
        id<MTLComputePipelineState> state = [self newComputePipelineStateWithFunction:descriptor.computeFunction error:&error];
        if (completionHandler) {
            completionHandler(state, nil, error);
        }
    });
}

#pragma mark - Render Pipeline

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor*)descriptor 
                                                             error:(NSError**)error {
    NSLog(@"RDNA4Device: Render pipeline not yet implemented");
    if (error) {
        *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                     code:-2
                                 userInfo:@{NSLocalizedDescriptionKey: @"Render pipeline not yet supported"}];
    }
    return nil;
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor*)descriptor 
                           completionHandler:(void (^)(id<MTLRenderPipelineState>, NSError*))completionHandler {
    NSError* error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:-2
                                     userInfo:@{NSLocalizedDescriptionKey: @"Render pipeline not yet supported"}];
    if (completionHandler) {
        completionHandler(nil, error);
    }
}

#pragma mark - Sampler State

- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor*)descriptor {
    NSLog(@"RDNA4Device: Sampler state not yet implemented");
    return nil;
}

#pragma mark - Depth/Stencil State

- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor*)descriptor {
    NSLog(@"RDNA4Device: Depth/stencil state not yet implemented");
    return nil;
}

#pragma mark - Heaps

- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor*)descriptor {
    NSLog(@"RDNA4Device: Heap allocation not yet implemented");
    return nil;
}

- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    return (MTLSizeAndAlign){length, 256};  // 256-byte alignment
}

- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor*)desc {
    return (MTLSizeAndAlign){0, 0};  // Not yet implemented
}

#pragma mark - Fences and Events

- (id<MTLFence>)newFence {
    NSLog(@"RDNA4Device: Fence objects not yet implemented");
    return nil;
}

- (id<MTLEvent>)newEvent {
    NSLog(@"RDNA4Device: Event objects not yet implemented");
    return nil;
}

- (id<MTLSharedEvent>)newSharedEvent {
    NSLog(@"RDNA4Device: Shared events not yet implemented");
    return nil;
}

- (id<MTLSharedEvent>)newSharedEventWithHandle:(MTLSharedEventHandle*)sharedEventHandle {
    NSLog(@"RDNA4Device: Shared event handle not yet implemented");
    return nil;
}

#pragma mark - Argument Encoders

- (id<MTLArgumentEncoder>)newArgumentEncoderWithArguments:(NSArray<MTLArgumentDescriptor*>*)arguments {
    NSLog(@"RDNA4Device: Argument encoders not yet implemented");
    return nil;
}

#pragma mark - Indirect Command Buffers

- (id<MTLIndirectCommandBuffer>)newIndirectCommandBufferWithDescriptor:(MTLIndirectCommandBufferDescriptor*)descriptor
                                                        maxCommandCount:(NSUInteger)maxCount
                                                                options:(MTLResourceOptions)options {
    NSLog(@"RDNA4Device: Indirect command buffers not yet implemented");
    return nil;
}

#pragma mark - Resource Size Queries

- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format {
    return 256;
}

- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)format {
    return 256;
}

- (NSUInteger)maxBufferLength {
    return 16ULL * 1024 * 1024 * 1024;  // 16 GB
}

#pragma mark - Timestamp Queries

- (void)sampleTimestamps:(NSUInteger*)cpuTimestamp gpuTimestamp:(NSUInteger*)gpuTimestamp {
    // Not yet implemented
    if (cpuTimestamp) *cpuTimestamp = 0;
    if (gpuTimestamp) *gpuTimestamp = 0;
}

@end
