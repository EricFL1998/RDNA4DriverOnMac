#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@class RDNA4Device;
@class RDNA4CompiledShader;

/**
 * RDNA4Function - Metal Function Implementation
 * 
 * Represents a compiled shader function (kernel, vertex, or fragment).
 * Contains the GFX12 machine code and metadata.
 */
@interface RDNA4Function : NSObject <MTLFunction>

@property (readonly) id<MTLDevice> device;
@property (readonly) MTLFunctionType functionType;
@property (readonly) NSString* name;
@property (copy) NSString* label;

/**
 * Vertex attributes (for vertex shaders)
 */
@property (readonly) NSArray<MTLAttribute*>* vertexAttributes;

/**
 * Stage input attributes (for fragment shaders)
 */
@property (readonly) NSArray<MTLAttribute*>* stageInputAttributes;

/**
 * Initialize with compiled shader
 * @param device Parent device
 * @param compiledShader Compiled GFX12 shader
 * @param name Function name
 */
- (instancetype)initWithDevice:(RDNA4Device*)device
                compiledShader:(RDNA4CompiledShader*)compiledShader
                          name:(NSString*)name;

/**
 * Get compiled shader data (internal use)
 */
- (RDNA4CompiledShader*)compiledShader;

/**
 * Resource binding table of this shader (internal use).
 */
- (NSData*)bindings;

@end
