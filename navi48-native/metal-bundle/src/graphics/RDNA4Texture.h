#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@class RDNA4Device;

/**
 * RDNA4Texture - Metal Texture Implementation
 * 
 * Supports:
 * - 2D/3D/Cube textures
 * - Various pixel formats
 * - Render targets
 * - Mipmaps
 * - IOSurface backed textures
 */
@interface RDNA4Texture : NSObject <MTLTexture>

@property (readonly) id<MTLDevice> device;
@property (readonly) MTLTextureType textureType;
@property (readonly) MTLPixelFormat pixelFormat;
@property (readonly) NSUInteger width;
@property (readonly) NSUInteger height;
@property (readonly) NSUInteger depth;
@property (readonly) NSUInteger mipmapLevelCount;
@property (readonly) NSUInteger sampleCount;
@property (readonly) NSUInteger arrayLength;
@property (readonly) MTLTextureUsage usage;
@property (copy) NSString* label;

/**
 * Initialize with descriptor
 */
- (instancetype)initWithDevice:(RDNA4Device*)device
                    descriptor:(MTLTextureDescriptor*)descriptor;

/**
 * Initialize with IOSurface
 */
- (instancetype)initWithDevice:(RDNA4Device*)device
                     iosurface:(IOSurfaceRef)iosurface
                    descriptor:(MTLTextureDescriptor*)descriptor
                         plane:(NSUInteger)plane;

/**
 * Get GPU address (internal use)
 */
- (uint64_t)gpuAddress;

/**
 * Get bytes per row for a given mipmap level
 */
- (NSUInteger)bytesPerRowForLevel:(NSUInteger)level;

/**
 * Get bytes per image for a given mipmap level
 */
- (NSUInteger)bytesPerImageForLevel:(NSUInteger)level;

/**
 * Replace region with data
 */
- (void)replaceRegion:(MTLRegion)region
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice
            withBytes:(const void*)pixelBytes
          bytesPerRow:(NSUInteger)bytesPerRow
        bytesPerImage:(NSUInteger)bytesPerImage;

/**
 * Get bytes from region
 */
- (void)getBytes:(void*)pixelBytes
     bytesPerRow:(NSUInteger)bytesPerRow
   bytesPerImage:(NSUInteger)bytesPerImage
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
           slice:(NSUInteger)slice;

/**
 * Create texture view
 */
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)pixelFormat
                                    textureType:(MTLTextureType)textureType
                                         levels:(NSRange)levelRange
                                         slices:(NSRange)sliceRange;

@end
