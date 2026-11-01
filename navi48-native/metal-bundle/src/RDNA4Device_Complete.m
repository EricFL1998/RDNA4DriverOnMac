#import "RDNA4Device.h"
#import "RDNA4CommandQueue.h"
#import "RDNA4Buffer.h"
#import "graphics/RDNA4Texture.h"
#import "RDNA4SamplerState.h"
#import "RDNA4UserClient.h"
#import "RDNA4Library.h"
#import "RDNA4Function.h"
#import "RDNA4ComputePipelineState.h"
#import "graphics/RDNA4RenderPipelineState.h"
#import <objc/runtime.h>

@implementation RDNA4Device {
    NSString* _name;
    uint64_t _registryID;
    uint64_t _vramSize;
    io_service_t _rdna4AcceleratorPort;
    id _rdna4AcceleratorService;
    uint64_t _nggRingAddress;
    uint32_t _nggRingSize;
}

+ (void)load {
    // The private protocol is supplied by the running Metal.framework, not
    // the SDK. The framework base does not itself declare this conformance;
    // MTLAddDevice checks it separately from the _MTLDevice subclass check.
    Protocol *spi = objc_getProtocol("MTLDeviceSPI");
    if (spi) class_addProtocol(self, spi);
}

- (instancetype)init {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceMatching("RDNA4Accel"));
    self = [self initWithAcceleratorPort:service];
    if (service) IOObjectRelease(service);
    return self;
}

- (instancetype)initWithAcceleratorPort:(io_service_t)service {
    // Initialize the framework base before any failing path: its dealloc
    // releases dispatch queues created by -init, even when we return nil.
    self = [super init];
    if (self) {
        Protocol *spi = objc_getProtocol("MTLDeviceSPI");
        if (!spi || !class_conformsToProtocol([self class], spi) ||
            !service || !IOObjectConformsTo(service, "RDNA4Accel") ||
            IORegistryEntryGetRegistryEntryID(service, &_registryID) != KERN_SUCCESS ||
            IOObjectRetain(service) != KERN_SUCCESS) {
            return nil;
        }
        _rdna4AcceleratorPort = service;
        _name = @"AMD Radeon RX 9070 XT (RDNA4)";
        NSNumber *vram = CFBridgingRelease(IORegistryEntryCreateCFProperty(
            service, CFSTR("vram-size"), kCFAllocatorDefault, 0));
        _vramSize = [vram isKindOfClass:[NSNumber class]] ? vram.unsignedLongLongValue : 0;
    }
    return self;
}

- (void)dealloc {
    if (_rdna4AcceleratorPort) IOObjectRelease(_rdna4AcceleratorPort);
}

// MTLIOAccelService owns hotplug notifications and holds a weak device.
// Keep it alive just as MTLIOAccelDevice does; do not create a retain cycle.
- (void)_setAcceleratorService:(id)service {
    _rdna4AcceleratorService = service;
}

- (io_service_t)acceleratorPort { return _rdna4AcceleratorPort; }

// _MTLDevice's native limit/feature-query initialization requires a desktop
// feature profile. 10000 is the Mac profile used by AppleParavirtDevice.
- (NSUInteger)featureProfile { return 10000; }
- (NSString *)vendorName { return @"AMD"; }
- (NSString *)familyName { return @"Radeon"; }
- (NSString *)productName { return @"RX 9070 XT"; }

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
    return _vramSize;
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

- (uint64_t)sharedNGGRingAddress:(uint32_t*)outSize {
    if (_nggRingAddress) {
        if (outSize) *outSize = _nggRingSize;
        return _nggRingAddress;
    }
    RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
    if (!link) return 0;
    // 0x580000 attribute ring + position/primitive rings, plus one 64 KiB
    // alignment slot at each end of the allocation.
    const uint64_t needed = 0xA80000ull;
    uint64_t raw = RDNA4GpuAllocate(link, needed + 2ull * 0x10000ull);
    if (!raw) return 0;
    const uint64_t base = (raw + 0xFFFFull) & ~0xFFFFull;
    void* mapped = RDNA4GpuPointerForAddress(link, base);
    if (!mapped) {
        RDNA4GpuFree(link, raw);
        return 0;
    }
    memset(mapped, 0, needed);
    _nggRingAddress = base;
    _nggRingSize = (uint32_t)needed;
    if (outSize) *outSize = _nggRingSize;
    return base;
}

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
    return [[RDNA4Texture alloc] initWithDevice:self descriptor:descriptor];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor*)descriptor
                                 iosurface:(IOSurfaceRef)iosurface
                                    plane:(NSUInteger)plane {
    if (!iosurface || !descriptor) return nil;
    return [[RDNA4Texture alloc] initWithDevice:self
                                      iosurface:iosurface
                                     descriptor:descriptor
                                          plane:plane];
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

- (id<MTLLibrary>)rdna4LibraryFromCompiledLibrary:(id<MTLLibrary>)compiled
                                             error:(NSError**)error {
    if (!compiled) return nil;
    NSData* data = [(id)compiled valueForKey:@"libraryDataContents"];
    if (!data) return compiled;
    dispatch_data_t dispatchData = dispatch_data_create(data.bytes, data.length,
                                                        dispatch_get_main_queue(),
                                                        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    return [self newLibraryWithData:dispatchData error:error];
}

- (id<MTLLibrary>)newLibraryWithSource:(NSString*)source
                               options:(MTLCompileOptions*)options
                                 error:(NSError**)error {
    id<MTLLibrary> compiled = [super newLibraryWithSource:source options:options error:error];
    return [self rdna4LibraryFromCompiledLibrary:compiled error:error];
}

- (void)newLibraryWithSource:(NSString*)source
                     options:(MTLCompileOptions*)options
           completionHandler:(MTLNewLibraryCompletionHandler)completionHandler {
    [super newLibraryWithSource:source options:options completionHandler:
        ^(id<MTLLibrary> compiled, NSError* error) {
            id<MTLLibrary> library = [self rdna4LibraryFromCompiledLibrary:compiled error:&error];
            if (completionHandler) completionHandler(library, error);
        }];
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
    return [[RDNA4RenderPipelineState alloc] initWithDevice:self
                                                 descriptor:descriptor
                                                      error:error];
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
    if (!descriptor) return nil;
    return [[RDNA4SamplerState alloc] initWithDevice:self descriptor:descriptor];
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
