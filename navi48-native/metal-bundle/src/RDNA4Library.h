#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@class RDNA4Device;

/**
 * RDNA4Library - Metal Library Implementation
 * 
 * Loads .metallib files, extracts AIR bitcode, and compiles to GFX12
 * using the AIRNT plugin (libAMDNTPlugin.dylib).
 */
@interface RDNA4Library : NSObject <MTLLibrary>

@property (readonly) id<MTLDevice> device;
@property (copy) NSString* label;
@property (readonly) NSArray<NSString*>* functionNames;

/**
 * Initialize with metallib data
 * @param device Parent device
 * @param data Metallib file data (MTLB container)
 * @param error Output error
 */
- (instancetype)initWithDevice:(RDNA4Device*)device
                          data:(dispatch_data_t)data
                         error:(NSError**)error;

/**
 * Initialize with metallib file path
 * @param device Parent device
 * @param filepath Path to .metallib file
 * @param error Output error
 */
- (instancetype)initWithDevice:(RDNA4Device*)device
                      filepath:(NSString*)filepath
                         error:(NSError**)error;

/**
 * Get function by name
 * @param functionName Kernel/shader function name
 * @return Function object or nil
 */
- (id<MTLFunction>)newFunctionWithName:(NSString*)functionName;

/**
 * Get function with constants (not yet supported)
 */
- (id<MTLFunction>)newFunctionWithName:(NSString*)functionName
                        constantValues:(MTLFunctionConstantValues*)constantValues
                                 error:(NSError**)error;

@end

/**
 * Resource binding layout produced by the AIRNT compiler.
 */
typedef struct {
    uint32_t userSgpr;        // absolute SGPR index of the 64-bit descriptor address
    uint32_t locationIndex;   // Metal binding index
    uint32_t kind;            // 0 texture, 1 sampler, 2 buffer
    uint32_t addressSpace;
} RDNA4Binding;

/**
 * Internal structure to cache compiled shaders
 */
@interface RDNA4CompiledShader : NSObject

@property (nonatomic, strong) NSString* functionName;
@property (nonatomic, assign) MTLFunctionType functionType;
@property (nonatomic, assign) const void* machineCode;  // GFX12 binary
@property (nonatomic, assign) size_t machineCodeSize;
@property (nonatomic, assign) uint32_t sgprCount;
@property (nonatomic, assign) uint32_t vgprCount;
@property (nonatomic, assign) uint32_t wavefrontSize;
@property (nonatomic, assign) uint32_t scratchBytes;
@property (nonatomic, assign) uint32_t ldsBytes;
@property (nonatomic, assign) uint32_t computePgmRsrc1;
@property (nonatomic, assign) uint32_t computePgmRsrc2;

// Graphics-stage resource registers (SPI_SHADER_PGM_RSRC1/2_*). These are a
// different encoding from the compute pair above and have to come out of the
// shader compiler; 0 means "metadata not available yet", and the render
// encoder refuses to program a stage that has none rather than guessing.
@property (nonatomic, assign) uint32_t pgmRsrc1;
@property (nonatomic, assign) uint32_t pgmRsrc2;
@property (nonatomic, assign) uint32_t vertexParameterCount;
@property (nonatomic, strong) NSData* bindings;

@end
