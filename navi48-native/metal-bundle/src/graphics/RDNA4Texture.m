#import "RDNA4Texture.h"
#import "RDNA4Device.h"
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
    size_t bytesPerPixel = [self bytesPerPixelForFormat:_pixelFormat];
    size_t totalSize = 0;
    
    for (NSUInteger level = 0; level < _mipmapLevelCount; level++) {
        NSUInteger levelWidth = MAX(1, _width >> level);
        NSUInteger levelHeight = MAX(1, _height >> level);
        NSUInteger levelDepth = MAX(1, _depth >> level);
        
        size_t levelSize = levelWidth * levelHeight * levelDepth * bytesPerPixel;
        totalSize += levelSize;
    }
    
    // Multiply by array length
    totalSize *= _arrayLength;
    
    // Add alignment padding
    totalSize = (totalSize + 255) & ~255;
    
    return totalSize;
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
    // Get IOUserClient connection
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                       IOServiceMatching("RDNA4Accel"));
    if (!service) {
        NSLog(@"RDNA4Texture: RDNA4Accel service not found");
        return NO;
    }
    
    io_connect_t connection;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    
    if (kr != KERN_SUCCESS) {
        NSLog(@"RDNA4Texture: Failed to open IOUserClient");
        return NO;
    }
    
    // Allocate texture memory (256-byte aligned for tiling)
    uint64_t input = _allocationSize;
    uint64_t output;
    uint32_t outputCount = 1;
    
    kr = IOConnectCallMethod(connection, 1,  // method_AllocateBuffer
                            &input, 1,
                            NULL, 0,
                            &output, &outputCount,
                            NULL, NULL);
    
    IOServiceClose(connection);
    
    if (kr != KERN_SUCCESS || output == 0) {
        NSLog(@"RDNA4Texture: Failed to allocate GPU memory");
        return NO;
    }
    
    _gpuAddress = output;
    return YES;
}

- (void)freeGPUMemory {
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                       IOServiceMatching("RDNA4Accel"));
    if (service) {
        io_connect_t connection;
        if (IOServiceOpen(service, mach_task_self(), 0, &connection) == KERN_SUCCESS) {
            uint64_t input = _gpuAddress;
            IOConnectCallMethod(connection, 2,  // method_FreeBuffer
                               &input, 1,
                               NULL, 0,
                               NULL, NULL,
                               NULL, NULL);
            IOServiceClose(connection);
        }
        IOObjectRelease(service);
    }
}

- (uint64_t)gpuAddress {
    return _gpuAddress;
}

- (NSUInteger)bytesPerRowForLevel:(NSUInteger)level {
    NSUInteger levelWidth = MAX(1, _width >> level);
    size_t bytesPerPixel = [self bytesPerPixelForFormat:_pixelFormat];
    
    // Align to 256 bytes
    NSUInteger bytesPerRow = levelWidth * bytesPerPixel;
    return (bytesPerRow + 255) & ~255;
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
    
    // Calculate destination offset in GPU memory
    uint64_t destOffset = [self offsetForLevel:level slice:slice];
    uint64_t destAddress = _gpuAddress + destOffset;
    
    // For now, use simple upload
    // TODO: Implement tiled texture upload
    
    size_t regionWidth = region.size.width;
    size_t regionHeight = region.size.height;
    size_t bytesPerPixel = [self bytesPerPixelForFormat:_pixelFormat];
    
    // Get IOUserClient connection
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                       IOServiceMatching("RDNA4Accel"));
    if (!service) return;
    
    io_connect_t connection;
    if (IOServiceOpen(service, mach_task_self(), 0, &connection) != KERN_SUCCESS) {
        IOObjectRelease(service);
        return;
    }
    
    // Upload row by row
    for (size_t y = 0; y < regionHeight; y++) {
        const uint8_t* srcRow = (const uint8_t*)pixelBytes + y * bytesPerRow;
        uint64_t destRow = destAddress + (region.origin.y + y) * [self bytesPerRowForLevel:level]
                          + region.origin.x * bytesPerPixel;
        
        size_t rowSize = regionWidth * bytesPerPixel;
        
        uint64_t inputs[1] = {destRow};
        IOConnectCallMethod(connection, 5,  // method_UploadData
                           inputs, 1,
                           srcRow, rowSize,
                           NULL, NULL,
                           NULL, NULL);
    }
    
    IOServiceClose(connection);
    IOObjectRelease(service);
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
    
    // Calculate source offset in GPU memory
    uint64_t srcOffset = [self offsetForLevel:level slice:slice];
    uint64_t srcAddress = _gpuAddress + srcOffset;
    
    size_t regionWidth = region.size.width;
    size_t regionHeight = region.size.height;
    size_t bytesPerPixel = [self bytesPerPixelForFormat:_pixelFormat];
    
    // Get IOUserClient connection
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                       IOServiceMatching("RDNA4Accel"));
    if (!service) return;
    
    io_connect_t connection;
    if (IOServiceOpen(service, mach_task_self(), 0, &connection) != KERN_SUCCESS) {
        IOObjectRelease(service);
        return;
    }
    
    // Download row by row
    for (size_t y = 0; y < regionHeight; y++) {
        uint8_t* destRow = (uint8_t*)pixelBytes + y * bytesPerRow;
        uint64_t srcRow = srcAddress + (region.origin.y + y) * [self bytesPerRowForLevel:level]
                         + region.origin.x * bytesPerPixel;
        
        size_t rowSize = regionWidth * bytesPerPixel;
        
        uint64_t inputs[1] = {srcRow};
        IOConnectCallMethod(connection, 6,  // method_DownloadData
                           inputs, 1,
                           NULL, 0,
                           NULL, NULL,
                           destRow, &rowSize);
    }
    
    IOServiceClose(connection);
    IOObjectRelease(service);
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
