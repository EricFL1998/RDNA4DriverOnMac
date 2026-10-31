#import "RDNA4Texture.h"
#import "RDNA4Device.h"
#import "RDNA4UserClient.h"
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurface.h>

@implementation RDNA4Texture {
    RDNA4Device* _device;
    MTLTextureType _textureType;
    MTLPixelFormat _pixelFormat;
    NSUInteger _width;
    NSUInteger _height;
    NSUInteger _depth;
    NSUInteger _mipmapLevelCount;
    NSUInteger _sampleCount;
    NSUInteger _arrayLength;
    MTLResourceOptions _resourceOptions;
    MTLTextureUsage _usage;
    
    uint64_t _gpuAddress;
    size_t _allocationSize;
    void* _mappedMemory;
    IOSurfaceRef _iosurface;
    
    BOOL _isRenderTarget;
    BOOL _isShaderReadable;
}

- (instancetype)initWithDevice:(RDNA4Device*)device
                    descriptor:(MTLTextureDescriptor*)descriptor {
    self = [super init];
    if (self) {
        _device = device;
        _textureType = descriptor.textureType;
        _pixelFormat = descriptor.pixelFormat;
        _width = descriptor.width;
        _height = descriptor.height;
        _depth = descriptor.depth;
        _mipmapLevelCount = descriptor.mipmapLevelCount;
        _sampleCount = descriptor.sampleCount;
        _arrayLength = descriptor.arrayLength;
        _resourceOptions = descriptor.resourceOptions;
        _usage = descriptor.usage;
        
        _isRenderTarget = (descriptor.usage & MTLTextureUsageRenderTarget) != 0;
        _isShaderReadable = (descriptor.usage & MTLTextureUsageShaderRead) != 0;
        
        // Calculate allocation size
        _allocationSize = [self calculateAllocationSize];
        
        // Allocate GPU memory
        if (![self allocateGPUMemory]) {
            return nil;
        }
        
        NSLog(@"RDNA4Texture: Created %lux%lu texture, format=%lu, GPU=0x%llx",
              _width, _height, (unsigned long)_pixelFormat, _gpuAddress);
    }
    return self;
}

- (instancetype)initWithDevice:(RDNA4Device*)device
                     iosurface:(IOSurfaceRef)iosurface
                    descriptor:(MTLTextureDescriptor*)descriptor
                         plane:(NSUInteger)plane {
    self = [super init];
    if (self) {
        _device = device;
        _iosurface = iosurface;
        CFRetain(_iosurface);
        
        // Get dimensions from IOSurface
        _width = IOSurfaceGetWidthOfPlane(iosurface, plane);
        _height = IOSurfaceGetHeightOfPlane(iosurface, plane);
        _depth = 1;
        
        _textureType = descriptor.textureType;
        _pixelFormat = descriptor.pixelFormat;
        _mipmapLevelCount = 1;
        _sampleCount = 1;
        _arrayLength = 1;
        _usage = descriptor.usage;
        _resourceOptions = MTLResourceStorageModeShared;
        
        // Get GPU address from IOSurface
        // In real implementation, would need to map IOSurface to GPU
        _gpuAddress = 0;  // TODO: Map IOSurface
        
        NSLog(@"RDNA4Texture: Created from IOSurface %lux%lu", _width, _height);
    }
    return self;
}

- (void)dealloc {
    // Free GPU memory
    if (_gpuAddress != 0 && !_iosurface) {
        [self freeGPUMemory];
    }
    
    if (_iosurface) {
        CFRelease(_iosurface);
    }
}

- (size_t)calculateAllocationSize {
    size_t totalSize = 0;
    for (NSUInteger level = 0; level < _mipmapLevelCount; level++) {
        const NSUInteger levelHeight = MAX(1, _height >> level);
        const NSUInteger levelDepth = MAX(1, _depth >> level);
        totalSize += [self bytesPerRowForLevel:level] * levelHeight * levelDepth;
    }
    totalSize *= _arrayLength;
    return (totalSize + 255) & ~255;
}

- (size_t)bytesPerPixelForFormat:(MTLPixelFormat)format {
    switch (format) {
        case MTLPixelFormatR8Unorm:
        case MTLPixelFormatR8Snorm:
        case MTLPixelFormatR8Uint:
        case MTLPixelFormatR8Sint:
            return 1;
            
        case MTLPixelFormatR16Unorm:
        case MTLPixelFormatR16Snorm:
        case MTLPixelFormatR16Uint:
        case MTLPixelFormatR16Sint:
        case MTLPixelFormatR16Float:
        case MTLPixelFormatRG8Unorm:
        case MTLPixelFormatRG8Snorm:
        case MTLPixelFormatRG8Uint:
        case MTLPixelFormatRG8Sint:
            return 2;
            
        case MTLPixelFormatR32Uint:
        case MTLPixelFormatR32Sint:
        case MTLPixelFormatR32Float:
        case MTLPixelFormatRG16Unorm:
        case MTLPixelFormatRG16Snorm:
        case MTLPixelFormatRG16Uint:
        case MTLPixelFormatRG16Sint:
        case MTLPixelFormatRG16Float:
        case MTLPixelFormatRGBA8Unorm:
        case MTLPixelFormatRGBA8Unorm_sRGB:
        case MTLPixelFormatRGBA8Snorm:
        case MTLPixelFormatRGBA8Uint:
        case MTLPixelFormatRGBA8Sint:
        case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatBGRA8Unorm_sRGB:
            return 4;
            
        case MTLPixelFormatRG32Uint:
        case MTLPixelFormatRG32Sint:
        case MTLPixelFormatRG32Float:
        case MTLPixelFormatRGBA16Unorm:
        case MTLPixelFormatRGBA16Snorm:
        case MTLPixelFormatRGBA16Uint:
        case MTLPixelFormatRGBA16Sint:
        case MTLPixelFormatRGBA16Float:
            return 8;
            
        case MTLPixelFormatRGBA32Uint:
        case MTLPixelFormatRGBA32Sint:
        case MTLPixelFormatRGBA32Float:
            return 16;
            
        case MTLPixelFormatDepth32Float:
            return 4;
            
        case MTLPixelFormatDepth32Float_Stencil8:
            return 8;
            
        default:
            NSLog(@"RDNA4Texture: Unsupported pixel format %lu", (unsigned long)format);
            return 4;
    }
}

- (BOOL)allocateGPUMemory {
    RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
    if (!link) return NO;
    _gpuAddress = RDNA4GpuAllocate(link, _allocationSize);
    if (!_gpuAddress) return NO;
    _mappedMemory = RDNA4GpuPointerForAddress(link, _gpuAddress);
    if (!_mappedMemory) {
        RDNA4GpuFree(link, _gpuAddress);
        _gpuAddress = 0;
        return NO;
    }
    return YES;
}

- (void)freeGPUMemory {
    if (!_gpuAddress) return;
    RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
    if (link) RDNA4GpuFree(link, _gpuAddress);
    _mappedMemory = NULL;
    _gpuAddress = 0;
}

- (uint64_t)gpuAddress {
    return _gpuAddress;
}

- (NSUInteger)bytesPerRowForLevel:(NSUInteger)level {
    NSUInteger levelWidth = MAX(1, _width >> level);
    size_t bytesPerPixel = [self bytesPerPixelForFormat:_pixelFormat];
    
    return levelWidth * bytesPerPixel;
}

- (NSUInteger)bytesPerImageForLevel:(NSUInteger)level {
    NSUInteger levelHeight = MAX(1, _height >> level);
    return [self bytesPerRowForLevel:level] * levelHeight;
}

- (void)replaceRegion:(MTLRegion)region
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice
            withBytes:(const void*)pixelBytes
          bytesPerRow:(NSUInteger)bytesPerRow
        bytesPerImage:(NSUInteger)bytesPerImage {
    
    if (!pixelBytes) {
        NSLog(@"RDNA4Texture: replaceRegion called with NULL pixelBytes");
        return;
    }
    
    const NSUInteger width = MAX(1, _width >> level);
    const NSUInteger height = MAX(1, _height >> level);
    if (!_mappedMemory || level >= _mipmapLevelCount || slice >= _arrayLength ||
        region.origin.x > width || region.origin.y > height ||
        region.size.width > width - region.origin.x ||
        region.size.height > height - region.origin.y) return;
    const size_t bytesPerPixel = [self bytesPerPixelForFormat:_pixelFormat];
    if (bytesPerRow < region.size.width * bytesPerPixel ||
        (slice != 0 && bytesPerImage == 0)) return;

    const uint64_t offset = [self offsetForLevel:level slice:slice];
    const size_t rowBytes = [self bytesPerRowForLevel:level];
    const uint64_t lastByte = offset +
        (region.origin.y + region.size.height - (region.size.height ? 1 : 0)) * rowBytes +
        (region.origin.x + region.size.width) * bytesPerPixel;
    if (lastByte > _allocationSize) return;

    const uint8_t* source = (const uint8_t*)pixelBytes + slice * bytesPerImage;
    uint8_t* destination = (uint8_t*)_mappedMemory + offset;
    for (size_t y = 0; y < region.size.height; ++y) {
        memcpy(destination + (region.origin.y + y) * rowBytes + region.origin.x * bytesPerPixel,
               source + y * bytesPerRow,
               region.size.width * bytesPerPixel);
    }
}

- (void)getBytes:(void*)pixelBytes
     bytesPerRow:(NSUInteger)bytesPerRow
   bytesPerImage:(NSUInteger)bytesPerImage
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
           slice:(NSUInteger)slice {
    
    if (!pixelBytes) {
        NSLog(@"RDNA4Texture: getBytes called with NULL pixelBytes");
        return;
    }
    
    const NSUInteger width = MAX(1, _width >> level);
    const NSUInteger height = MAX(1, _height >> level);
    if (!_mappedMemory || level >= _mipmapLevelCount || slice >= _arrayLength ||
        region.origin.x > width || region.origin.y > height ||
        region.size.width > width - region.origin.x ||
        region.size.height > height - region.origin.y) return;
    const size_t bytesPerPixel = [self bytesPerPixelForFormat:_pixelFormat];
    if (bytesPerRow < region.size.width * bytesPerPixel ||
        (slice != 0 && bytesPerImage == 0)) return;

    const uint64_t offset = [self offsetForLevel:level slice:slice];
    const size_t rowBytes = [self bytesPerRowForLevel:level];
    const uint64_t lastByte = offset +
        (region.origin.y + region.size.height - (region.size.height ? 1 : 0)) * rowBytes +
        (region.origin.x + region.size.width) * bytesPerPixel;
    if (lastByte > _allocationSize) return;

    const uint8_t* source = (const uint8_t*)_mappedMemory + offset;
    uint8_t* destination = (uint8_t*)pixelBytes + slice * bytesPerImage;
    for (size_t y = 0; y < region.size.height; ++y) {
        memcpy(destination + y * bytesPerRow,
               source + (region.origin.y + y) * rowBytes + region.origin.x * bytesPerPixel,
               region.size.width * bytesPerPixel);
    }
}

- (uint64_t)offsetForLevel:(NSUInteger)level slice:(NSUInteger)slice {
    uint64_t offset = 0;
    
    // Add offsets for previous mipmap levels
    for (NSUInteger l = 0; l < level; l++) {
        offset += [self bytesPerImageForLevel:l];
    }
    
    // Add offset for slice
    offset += slice * [self bytesPerImageForLevel:level];
    
    return offset;
}

- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)pixelFormat
                                    textureType:(MTLTextureType)textureType
                                         levels:(NSRange)levelRange
                                         slices:(NSRange)sliceRange {
    // Texture views not yet fully supported
    NSLog(@"RDNA4Texture: newTextureViewWithPixelFormat not yet implemented");
    return nil;
}

// MTLTexture protocol methods

- (id<MTLDevice>)device {
    return _device;
}

- (MTLTextureType)textureType {
    return _textureType;
}

- (MTLPixelFormat)pixelFormat {
    return _pixelFormat;
}

- (NSUInteger)width {
    return _width;
}

- (NSUInteger)height {
    return _height;
}

- (NSUInteger)depth {
    return _depth;
}

- (NSUInteger)mipmapLevelCount {
    return _mipmapLevelCount;
}

- (NSUInteger)sampleCount {
    return _sampleCount;
}

- (NSUInteger)arrayLength {
    return _arrayLength;
}

- (MTLTextureUsage)usage {
    return _usage;
}

- (MTLResourceOptions)resourceOptions {
    return _resourceOptions;
}

- (MTLCPUCacheMode)cpuCacheMode {
    return MTLCPUCacheModeDefaultCache;
}

- (MTLStorageMode)storageMode {
    return MTLStorageModePrivate;  // GPU only by default
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
    return _allocationSize;
}

- (void)makeAliasable {}
- (BOOL)isAliasable { return NO; }

- (id<MTLResource>)parentTexture {
    return nil;
}

- (NSUInteger)parentRelativeLevel {
    return 0;
}

- (NSUInteger)parentRelativeSlice {
    return 0;
}

- (id<MTLBuffer>)buffer {
    return nil;
}

- (NSUInteger)bufferOffset {
    return 0;
}

- (NSUInteger)bufferBytesPerRow {
    return 0;
}

- (IOSurfaceRef)iosurface {
    return _iosurface;
}

- (NSUInteger)iosurfacePlane {
    return 0;
}

@end
