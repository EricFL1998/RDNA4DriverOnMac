#import "RDNA4Library.h"
#import "RDNA4Device.h"
#import "RDNA4Function.h"
#import <dlfcn.h>

// AIRNT Plugin function pointers (from libAMDNTPlugin.dylib)
typedef void* AIRNTContext;
typedef void* AIRNTExecutable;

typedef int (*AIRNTInit_t)(AIRNTContext* ctx, uint32_t version);
typedef void (*AIRNTDestroy_t)(AIRNTContext ctx);
typedef int (*AIRNTCompile_t)(AIRNTContext ctx, const void* bitcode, size_t size,
                              const char* entryPoint, AIRNTExecutable* exec, char** error);
typedef int (*AIRNTGetMachineCode_t)(AIRNTExecutable exec, const void** code, size_t* size);
typedef int (*AIRNTGetMetadata_t)(AIRNTExecutable exec, uint32_t* sgpr, uint32_t* vgpr,
                                  uint32_t* wave, uint32_t* scratch, uint32_t* lds,
                                  uint32_t* rsrc1, uint32_t* rsrc2);
typedef void (*AIRNTFreeExecutable_t)(AIRNTExecutable exec);

@implementation RDNA4Library {
    RDNA4Device* _device;
    NSMutableDictionary<NSString*, RDNA4CompiledShader*>* _compiledShaders;
    NSData* _metallibData;
    
    // AIRNT plugin handle
    void* _airntHandle;
    AIRNTContext _airntContext;
    AIRNTInit_t _airntInit;
    AIRNTDestroy_t _airntDestroy;
    AIRNTCompile_t _airntCompile;
    AIRNTGetMachineCode_t _airntGetMachineCode;
    AIRNTGetMetadata_t _airntGetMetadata;
    AIRNTFreeExecutable_t _airntFreeExecutable;
}

- (instancetype)initWithDevice:(RDNA4Device*)device
                          data:(dispatch_data_t)data
                         error:(NSError**)error {
    self = [super init];
    if (self) {
        _device = device;
        _compiledShaders = [NSMutableDictionary dictionary];
        
        // Convert dispatch_data_t to NSData
        __block NSMutableData* mutableData = [NSMutableData data];
        dispatch_data_apply(data, ^bool(dispatch_data_t region, size_t offset, const void* buffer, size_t size) {
            [mutableData appendBytes:buffer length:size];
            return true;
        });
        _metallibData = [mutableData copy];
        
        // Load AIRNT plugin
        if (![self loadAIRNTPlugin:error]) {
            return nil;
        }
        
        // Parse metallib to extract function names
        if (![self parseMetallib:error]) {
            return nil;
        }
    }
    return self;
}

- (instancetype)initWithDevice:(RDNA4Device*)device
                      filepath:(NSString*)filepath
                         error:(NSError**)error {
    NSData* data = [NSData dataWithContentsOfFile:filepath options:0 error:error];
    if (!data) {
        return nil;
    }
    
    dispatch_data_t dispatchData = dispatch_data_create(data.bytes, data.length,
                                                        dispatch_get_main_queue(),
                                                        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    
    self = [self initWithDevice:device data:dispatchData error:error];
    return self;
}

- (void)dealloc {
    if (_airntContext) {
        _airntDestroy(_airntContext);
    }
    if (_airntHandle) {
        dlclose(_airntHandle);
    }
}

- (BOOL)loadAIRNTPlugin:(NSError**)error {
    // Try to load libAMDNTPlugin.dylib from bundle or system paths
    NSArray* searchPaths = @[
        @"../MacOS/libAMDNTPlugin.dylib",  // Relative to bundle Resources
        @"/Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib",
        @"~/Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib"
    ];
    
    for (NSString* path in searchPaths) {
        NSString* expandedPath = [path stringByExpandingTildeInPath];
        _airntHandle = dlopen([expandedPath UTF8String], RTLD_LAZY);
        if (_airntHandle) {
            NSLog(@"RDNA4Library: Loaded AIRNT plugin from %@", expandedPath);
            break;
        }
    }
    
    if (!_airntHandle) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:-1
                                     userInfo:@{NSLocalizedDescriptionKey: @"Failed to load libAMDNTPlugin.dylib"}];
        }
        return NO;
    }
    
    // Load function pointers
    _airntInit = (AIRNTInit_t)dlsym(_airntHandle, "AIRNTInit");
    _airntDestroy = (AIRNTDestroy_t)dlsym(_airntHandle, "AIRNTDestroy");
    _airntCompile = (AIRNTCompile_t)dlsym(_airntHandle, "AIRNTCompile");
    _airntGetMachineCode = (AIRNTGetMachineCode_t)dlsym(_airntHandle, "AIRNTGetMachineCode");
    _airntGetMetadata = (AIRNTGetMetadata_t)dlsym(_airntHandle, "AIRNTGetMetadata");
    _airntFreeExecutable = (AIRNTFreeExecutable_t)dlsym(_airntHandle, "AIRNTFreeExecutable");
    
    if (!_airntInit || !_airntCompile) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:-2
                                     userInfo:@{NSLocalizedDescriptionKey: @"Invalid AIRNT plugin"}];
        }
        return NO;
    }
    
    // Initialize AIRNT context
    int result = _airntInit(&_airntContext, 0x00010000);  // API version 1.0
    if (result != 0) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:result
                                     userInfo:@{NSLocalizedDescriptionKey: @"AIRNTInit failed"}];
        }
        return NO;
    }
    
    return YES;
}

- (BOOL)parseMetallib:(NSError**)error {
    // Simple MTLB parser - extract function names from NAME tags
    // Full implementation would parse the complete MTLB structure
    
    const uint8_t* bytes = (const uint8_t*)_metallibData.bytes;
    size_t length = _metallibData.length;
    
    // Check MTLB magic (0x4D54 4C42 = "MTLB")
    if (length < 4 || memcmp(bytes, "MTLB", 4) != 0) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:-3
                                     userInfo:@{NSLocalizedDescriptionKey: @"Invalid metallib format"}];
        }
        return NO;
    }
    
    // For now, we'll extract function names on-demand when newFunctionWithName is called
    // A full implementation would parse all NAME tags here
    
    NSLog(@"RDNA4Library: Loaded metallib (%zu bytes)", length);
    return YES;
}

- (id<MTLFunction>)newFunctionWithName:(NSString*)functionName {
    // Check if already compiled
    RDNA4CompiledShader* cached = _compiledShaders[functionName];
    if (cached) {
        return [[RDNA4Function alloc] initWithDevice:_device
                                        compiledShader:cached
                                                  name:functionName];
    }
    
    // Extract AIR bitcode for this function from metallib
    NSData* airBitcode = [self extractAIRBitcodeForFunction:functionName];
    if (!airBitcode) {
        NSLog(@"RDNA4Library: Function '%@' not found in metallib", functionName);
        return nil;
    }
    
    // Compile AIR to GFX12 using AIRNT plugin
    AIRNTExecutable executable;
    char* errorMsg = NULL;
    int result = _airntCompile(_airntContext,
                               airBitcode.bytes,
                               airBitcode.length,
                               [functionName UTF8String],
                               &executable,
                               &errorMsg);
    
    if (result != 0) {
        NSLog(@"RDNA4Library: Compilation failed for '%@': %s", functionName,
              errorMsg ? errorMsg : "unknown error");
        if (errorMsg) free(errorMsg);
        return nil;
    }
    
    // Get machine code
    const void* machineCode;
    size_t machineCodeSize;
    _airntGetMachineCode(executable, &machineCode, &machineCodeSize);
    
    // Get metadata
    uint32_t sgpr, vgpr, wave, scratch, lds, rsrc1, rsrc2;
    _airntGetMetadata(executable, &sgpr, &vgpr, &wave, &scratch, &lds, &rsrc1, &rsrc2);
    
    // Create compiled shader object
    RDNA4CompiledShader* compiled = [[RDNA4CompiledShader alloc] init];
    compiled.functionName = functionName;
    
    // Copy machine code (need to keep it alive)
    void* copiedCode = malloc(machineCodeSize);
    memcpy(copiedCode, machineCode, machineCodeSize);
    compiled.machineCode = copiedCode;
    compiled.machineCodeSize = machineCodeSize;
    
    compiled.sgprCount = sgpr;
    compiled.vgprCount = vgpr;
    compiled.wavefrontSize = wave;
    compiled.scratchBytes = scratch;
    compiled.ldsBytes = lds;
    compiled.computePgmRsrc1 = rsrc1;
    compiled.computePgmRsrc2 = rsrc2;
    
    // Cache it
    _compiledShaders[functionName] = compiled;
    
    // Clean up AIRNT executable
    _airntFreeExecutable(executable);
    
    NSLog(@"RDNA4Library: Compiled '%@' - %zu bytes, SGPR=%u, VGPR=%u",
          functionName, machineCodeSize, sgpr, vgpr);
    
    return [[RDNA4Function alloc] initWithDevice:_device
                                    compiledShader:compiled
                                              name:functionName];
}

- (NSData*)extractAIRBitcodeForFunction:(NSString*)functionName {
    // Parse MTLB container to extract AIR bitcode for specific function
    // MTLB format:
    //   Header: MTLB magic + version
    //   Tags: NAME, TYPE, HASH, OFFS, SIZE, etc.
    //   Data: AIR bitcode blobs
    
    const uint8_t* bytes = (const uint8_t*)_metallibData.bytes;
    size_t length = _metallibData.length;
    
    // Skip header (16 bytes typically)
    size_t offset = 16;
    
    // Scan for NAME tag matching functionName
    while (offset + 8 < length) {
        uint32_t tag = *(uint32_t*)(bytes + offset);
        uint32_t size = *(uint32_t*)(bytes + offset + 4);
        offset += 8;
        
        if (tag == 0x454D414E) {  // 'NAME' in little-endian
            // Check if this name matches
            if (offset + size <= length) {
                NSString* name = [[NSString alloc] initWithBytes:(bytes + offset)
                                                          length:size
                                                        encoding:NSUTF8StringEncoding];
                if ([name isEqualToString:functionName]) {
                    // Found it! Now find corresponding DATA/BITC tag
                    // For simplicity, assume next DATA tag is the bitcode
                    // Real implementation would use OFFS/SIZE tags
                    return [self findNextBitcodeFromOffset:offset + size];
                }
            }
        }
        
        offset += size;
    }
    
    return nil;
}

- (NSData*)findNextBitcodeFromOffset:(size_t)startOffset {
    const uint8_t* bytes = (const uint8_t*)_metallibData.bytes;
    size_t length = _metallibData.length;
    size_t offset = startOffset;
    
    while (offset + 8 < length) {
        uint32_t tag = *(uint32_t*)(bytes + offset);
        uint32_t size = *(uint32_t*)(bytes + offset + 4);
        offset += 8;
        
        // Look for DATA or BITC tag
        if (tag == 0x41544144 || tag == 0x43544942) {  // 'DATA' or 'BITC'
            if (offset + size <= length) {
                return [NSData dataWithBytes:(bytes + offset) length:size];
            }
        }
        
        offset += size;
    }
    
    return nil;
}

- (id<MTLFunction>)newFunctionWithName:(NSString*)functionName
                        constantValues:(MTLFunctionConstantValues*)constantValues
                                 error:(NSError**)error {
    // Function constants not yet supported
    if (error) {
        *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                     code:-4
                                 userInfo:@{NSLocalizedDescriptionKey: @"Function constants not yet supported"}];
    }
    return nil;
}

- (NSArray<NSString*>*)functionNames {
    // Return cached function names
    return [_compiledShaders allKeys];
}

@end

@implementation RDNA4CompiledShader

- (void)dealloc {
    if (_machineCode) {
        free((void*)_machineCode);
    }
}

@end
