#import "RDNA4Texture.h"
#import "RDNA4Device.h"
#import "RDNA4UserClient.h"
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurface.h>
#include "gfx12_descriptors.h"

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
    uint64_t _descriptorAddress;
    size_t _allocationSize;
    void* _mappedMemory;
    void* _gpuMappedMemory;
    IOSurfaceRef _iosurface;
    NSUInteger _iosurfacePlane;
    BOOL _gpuRendered;
    
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
        [self buildImageDescriptor];
        
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
        _isRenderTarget = (descriptor.usage & MTLTextureUsageRenderTarget) != 0;
        _isShaderReadable = (descriptor.usage & MTLTextureUsageShaderRead) != 0;

        _iosurfacePlane = plane;
        _mappedMemory = IOSurfaceGetBaseAddressOfPlane(iosurface, plane);
        _allocationSize = [self calculateAllocationSize];
        _gpuRendered = NO;

        // The GPU needs a real GPU address. Allocate a staging buffer in the
        // driver window and mirror the IOSurface into it; CPU reads/writes keep
        // using the IOSurface, and readback copies the staging buffer back.
        RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
        if (link) {
            _gpuAddress = RDNA4GpuAllocate(link, _allocationSize);
            if (_gpuAddress) {
                _gpuMappedMemory = RDNA4GpuPointerForAddress(link, _gpuAddress);
                if (_gpuMappedMemory) memset(_gpuMappedMemory, 0, _allocationSize);
            }
        }
        [self synchronizeFromIOSurface];
        [self buildImageDescriptor];
        
        NSLog(@"RDNA4Texture: Created from IOSurface %lux%lu", _width, _height);
    }
    return self;
}

- (void)dealloc {
    // Free GPU memory
    if (_gpuAddress != 0) {
        [self freeGPUMemory];
    }
    if (_descriptorAddress != 0) {
        RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
        if (link) RDNA4GpuFree(link, _descriptorAddress);
        _descriptorAddress = 0;
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

    // GFX12 colour surfaces need a row pitch of at least 128 bytes: an 8-pixel
    // BGRA8 target (32-byte rows) never received a fragment, while 16 pixels
    // and above worked. The colour target writes MIP0_WIDTH from this pitch and
    // the image descriptor carries it in DEPTH/PITCH_MSB.
    NSUInteger rowBytes = levelWidth * bytesPerPixel;
    return (rowBytes + 127) & ~(NSUInteger)127;
}

- (NSUInteger)pitchTexelsForLevel:(NSUInteger)level {
    return [self bytesPerRowForLevel:level] / [self bytesPerPixelForFormat:_pixelFormat];
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
    [self synchronizeFromIOSurface];
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

    if (_gpuRendered) {
        [self synchronizeFromGPU];
        _gpuRendered = NO;
    }

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
    return _iosurfacePlane;
}

- (void)markGPURendered {
    _gpuRendered = YES;
}

- (void)synchronizeFromIOSurface {
    if (!_iosurface || !_mappedMemory || !_gpuMappedMemory) return;
    const size_t rowBytes = [self bytesPerRowForLevel:0];
    const size_t surfaceRowBytes = IOSurfaceGetBytesPerRowOfPlane(_iosurface, _iosurfacePlane);
    if (!rowBytes || !surfaceRowBytes) return;
    for (NSUInteger y = 0; y < _height; ++y) {
        memcpy((uint8_t*)_gpuMappedMemory + y * rowBytes,
               (const uint8_t*)_mappedMemory + y * surfaceRowBytes,
               MIN(rowBytes, surfaceRowBytes));
    }
}

- (void)synchronizeFromGPU {
    if (!_iosurface || !_mappedMemory || !_gpuMappedMemory) return;
    const size_t rowBytes = [self bytesPerRowForLevel:0];
    const size_t surfaceRowBytes = IOSurfaceGetBytesPerRowOfPlane(_iosurface, _iosurfacePlane);
    if (!rowBytes || !surfaceRowBytes) return;
    for (NSUInteger y = 0; y < _height; ++y) {
        memcpy((uint8_t*)_mappedMemory + y * surfaceRowBytes,
               (const uint8_t*)_gpuMappedMemory + y * rowBytes,
               MIN(rowBytes, surfaceRowBytes));
    }
}

- (uint32_t)gfx12FormatForPixelFormat:(MTLPixelFormat)format {
    switch (format) {
        case MTLPixelFormatR8Unorm: return kGfx12Fmt_8_UNORM;
        case MTLPixelFormatR8Uint: return kGfx12Fmt_8_UINT;
        case MTLPixelFormatRG8Unorm: return kGfx12Fmt_8_8_UNORM;
        case MTLPixelFormatR16Float: return kGfx12Fmt_16_FLOAT;
        case MTLPixelFormatRG16Float: return kGfx12Fmt_16_16_FLOAT;
        case MTLPixelFormatRGBA16Float: return kGfx12Fmt_16_16_16_16_FLOAT;
        case MTLPixelFormatR32Float: return kGfx12Fmt_32_FLOAT;
        case MTLPixelFormatRG32Float: return kGfx12Fmt_32_32_FLOAT;
        case MTLPixelFormatRGBA32Float: return kGfx12Fmt_32_32_32_32_FLOAT;
        case MTLPixelFormatR32Uint: return kGfx12Fmt_32_UINT;
        case MTLPixelFormatRGBA8Uint: return kGfx12Fmt_8_8_8_8_UINT;
        case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatRGBA8Unorm_sRGB: return kGfx12Fmt_8_8_8_8_SRGB;
        default: return kGfx12Fmt_8_8_8_8_UNORM;
    }
}

- (void)buildImageDescriptor {
    if (!_gpuAddress) return;

    const size_t bpp = MAX((size_t)1, [self bytesPerPixelForFormat:_pixelFormat]);
    const size_t rowBytes = [self bytesPerRowForLevel:0];
    Gfx12ImageDesc descriptor = {};
    descriptor.gpuAddress = _gpuAddress;
    descriptor.width = (uint32_t)MAX((NSUInteger)1, _width);
    descriptor.height = (uint32_t)MAX((NSUInteger)1, _height);
    descriptor.depth = (uint32_t)MAX((NSUInteger)1, _arrayLength);
    descriptor.pitchTexels = (uint32_t)(rowBytes / bpp);
    descriptor.format = [self gfx12FormatForPixelFormat:_pixelFormat];
    descriptor.swizzleMode = kGfx12SwizzleLinear;
    descriptor.numLevels = (uint32_t)MAX((NSUInteger)1, _mipmapLevelCount);
    switch (_textureType) {
        case MTLTextureType1D: descriptor.type = kGfx12ImgType1D; break;
        case MTLTextureType3D: descriptor.type = kGfx12ImgType3D; break;
        case MTLTextureTypeCube: descriptor.type = kGfx12ImgTypeCube; break;
        case MTLTextureType1DArray: descriptor.type = kGfx12ImgType1DArray; break;
        case MTLTextureType2DArray: descriptor.type = kGfx12ImgType2DArray; break;
        default: descriptor.type = kGfx12ImgType2D; break;
    }
    const bool bgra = (_pixelFormat == MTLPixelFormatBGRA8Unorm ||
                       _pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB);
    descriptor.dstSel[0] = bgra ? kGfx12SelZ : kGfx12SelX;
    descriptor.dstSel[1] = kGfx12SelY;
    descriptor.dstSel[2] = bgra ? kGfx12SelX : kGfx12SelZ;
    descriptor.dstSel[3] = kGfx12SelW;

    uint32_t words[8];
    gfx12MakeImageDescriptor(&descriptor, words);
    RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
    if (!link) return;
    _descriptorAddress = RDNA4GpuAllocate(link, sizeof(words));
    void* mapped = _descriptorAddress ? RDNA4GpuPointerForAddress(link, _descriptorAddress) : NULL;
    if (!mapped) {
        if (_descriptorAddress) RDNA4GpuFree(link, _descriptorAddress);
        _descriptorAddress = 0;
        return;
    }
    memcpy(mapped, words, sizeof(words));
}

- (uint64_t)descriptorAddress {
    return _descriptorAddress;
}

- (void)getBytes:(void*)pixelBytes
     bytesPerRow:(NSUInteger)bytesPerRow
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level {
    [self getBytes:pixelBytes
       bytesPerRow:bytesPerRow
     bytesPerImage:bytesPerRow * MAX((NSUInteger)1, region.size.height)
        fromRegion:region
      mipmapLevel:level
            slice:0];
}

- (void)replaceRegion:(MTLRegion)region
          mipmapLevel:(NSUInteger)level
            withBytes:(const void*)pixelBytes
          bytesPerRow:(NSUInteger)bytesPerRow {
    [self replaceRegion:region
            mipmapLevel:level
                  slice:0
              withBytes:pixelBytes
            bytesPerRow:bytesPerRow
          bytesPerImage:bytesPerRow * MAX((NSUInteger)1, region.size.height)];
}

@end
