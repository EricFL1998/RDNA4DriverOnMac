#ifndef RDNA4MetalDeviceSPI_h
#define RDNA4MetalDeviceSPI_h

#import <Metal/Metal.h>

// Metal.framework exports this class, but the public SDK omits its interface.
// On macOS 26.7.1 (25G241), getMetalPluginClassForService accepts only subclasses
// of _MTLDevice. NSObject<MTLDevice> is silently rejected before initialization.
// Declare no ivars: the framework owns the layout (Objective-C non-fragile ABI).
// RDNA4 uses its own RDNA4Driver user client, not MTLIOAccelDevice's private
// IOAccelDeviceCreateWithAPIProperty / IOAccelSharedUserClient2 transport.
@interface _MTLDevice : NSObject
- (id<MTLLibrary>)newLibraryWithSource:(NSString*)source
                               options:(MTLCompileOptions*)options
                                 error:(NSError**)error;
- (void)newLibraryWithSource:(NSString*)source
                     options:(MTLCompileOptions*)options
           completionHandler:(MTLNewLibraryCompletionHandler)completionHandler;
@end

#endif
