#import "RDNA4Buffer.h"
#import "RDNA4Device.h"
#import <IOKit/IOKitLib.h>
#import "RDNA4UserClient.h"

@implementation RDNA4Buffer {
    RDNA4Device* _device;
    NSUInteger _length;
    MTLResourceOptions _options;
    void* _contents;
    uint64_t _gpuAddress;
    BOOL _isShared;
}

- (instancetype)initWithDevice:(RDNA4Device*)device
                        length:(NSUInteger)length
                       options:(MTLResourceOptions)options {
    self = [super init];
    if (self) {
        _device = device;
        _length = length;
        _options = options;
        _isShared = (options & MTLResourceStorageModeShared) != 0;

        if (![self allocateGPUMemory]) {
            return nil;
        }

        // contents is the mapped VRAM window, not a host copy: a malloc'd
        // shadow buffer here is why nothing the app wrote ever reached the GPU.
        _contents = RDNA4GpuPointerForAddress(RDNA4GpuLinkAcquire(), _gpuAddress);
        if (!_contents) {
            NSLog(@"RDNA4Buffer: GPU address 0x%llx is outside the mapped window",
                  (unsigned long long)_gpuAddress);
            [self freeGPUMemory];
            return nil;
        }
    }
    return self;
}

- (instancetype)initWithDevice:(RDNA4Device*)device
                         bytes:(const void*)bytes
                        length:(NSUInteger)length
                       options:(MTLResourceOptions)options {
    self = [self initWithDevice:device length:length options:options];
    if (self && bytes && _contents) {
        memcpy(_contents, bytes, length);
    }
    return self;
}

- (BOOL)allocateGPUMemory {
    RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
    if (!link) {
        NSLog(@"RDNA4Buffer: RDNA4Driver service is not available");
        return NO;
    }

    _gpuAddress = RDNA4GpuAllocate(link, (uint64_t)_length);
    if (_gpuAddress == 0) {
        NSLog(@"RDNA4Buffer: failed to allocate %lu bytes", (unsigned long)_length);
        return NO;
    }
    return YES;
}

- (void)freeGPUMemory {
    if (_gpuAddress == 0) return;
    RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
    if (link) RDNA4GpuFree(link, _gpuAddress);
    _gpuAddress = 0;
}

- (void)dealloc {
    _contents = NULL;
    [self freeGPUMemory];
}

- (id<MTLDevice>)device {
    return _device;
}

- (NSUInteger)length {
    return _length;
}

- (MTLResourceOptions)resourceOptions {
    return _options;
}

- (void*)contents {
    return _contents;
}

- (uint64_t)gpuAddress {
    return _gpuAddress;
}

- (void)didModifyRange:(NSRange)range {
    if (!_isShared || !_contents) {
        return;
    }
    
    // For shared buffers, need to sync CPU->GPU
    // In a real implementation, would use IOConnectMapMemory or DMA
    NSLog(@"RDNA4Buffer: didModifyRange - sync not yet implemented");
}

// MTLBuffer required methods
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor*)descriptor
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)bytesPerRow {
    return nil;  // Not yet supported
}

- (void)addDebugMarker:(NSString*)marker range:(NSRange)range {}
- (void)removeAllDebugMarkers {}

// MTLResource methods
- (MTLCPUCacheMode)cpuCacheMode {
    return (_options & MTLResourceCPUCacheModeWriteCombined) 
        ? MTLCPUCacheModeWriteCombined 
        : MTLCPUCacheModeDefaultCache;
}

- (MTLStorageMode)storageMode {
    if (_options & MTLResourceStorageModeShared) return MTLStorageModeShared;
    if (_options & MTLResourceStorageModePrivate) return MTLStorageModePrivate;
    return MTLStorageModeShared;
}

- (MTLHazardTrackingMode)hazardTrackingMode {
    return MTLHazardTrackingModeDefault;
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
    return MTLPurgeableStateNonVolatile;
}

- (id<MTLHeap>)heap {
    return nil;
}

- (NSUInteger)heapOffset {
    return 0;
}

- (NSUInteger)allocatedSize {
    return _length;
}

- (void)makeAliasable {}
- (BOOL)isAliasable { return NO; }

@end
