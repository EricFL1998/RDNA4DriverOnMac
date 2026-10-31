#import "RDNA4Library.h"
#import "RDNA4Device.h"
#import "RDNA4Function.h"
#import <dlfcn.h>

#import "../../airnt-plugin/include/airnt_plugin.h"
#include "../../../tools/air2gfx12/src/MTLBUnpacker.hpp"

typedef AIRNTStatus (*AIRNTInit_t)(AIRNTContext* ctx, uint32_t version);
typedef void (*AIRNTDestroy_t)(AIRNTContext ctx);
typedef AIRNTStatus (*AIRNTCompile_t)(AIRNTContext ctx, const void* bitcode, size_t size,
                                      const AIRNTCompileOptions* options,
                                      AIRNTExecutable* exec, char** error);
typedef AIRNTStatus (*AIRNTGetMachineCode_t)(AIRNTExecutable exec, const void** code, size_t* size);
typedef AIRNTStatus (*AIRNTGetMetadata_t)(AIRNTExecutable exec, AIRNTShaderMetadata* metadata);
typedef void (*AIRNTFreeExecutable_t)(AIRNTExecutable exec);

@implementation RDNA4Library {
    RDNA4Device* _device;
    NSMutableDictionary<NSString*, RDNA4CompiledShader*>* _compiledShaders;
    NSData* _metallibData;
    std::vector<air2gfx12::FunctionEntry> _functionEntries;
    NSArray<NSString*>* _functionNames;
    
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
    const uint8_t* bytes = (const uint8_t*)_metallibData.bytes;
    size_t length = _metallibData.length;
    if (!air2gfx12::MTLBUnpacker::unpack(bytes, length, _functionEntries)) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:-3
                                     userInfo:@{NSLocalizedDescriptionKey: @"Invalid metallib or no function bitcode"}];
        }
        return NO;
    }

    NSMutableArray<NSString*>* names = [NSMutableArray arrayWithCapacity:_functionEntries.size()];
    for (const auto& entry : _functionEntries) {
        [names addObject:[NSString stringWithUTF8String:entry.name.c_str()]];
    }
    _functionNames = [names copy];
    NSLog(@"RDNA4Library: Loaded metallib (%zu bytes, %lu functions)",
          length, (unsigned long)_functionEntries.size());
    return YES;
}

- (const air2gfx12::FunctionEntry*)functionEntryForName:(NSString*)functionName {
    for (const auto& entry : _functionEntries) {
        if (entry.name == functionName.UTF8String) return &entry;
    }
    return nullptr;
}

- (id<MTLFunction>)newFunctionWithName:(NSString*)functionName {
    RDNA4CompiledShader* cached = _compiledShaders[functionName];
    if (cached) {
        return [[RDNA4Function alloc] initWithDevice:_device
                                        compiledShader:cached
                                                  name:functionName];
    }

    const air2gfx12::FunctionEntry* entry = [self functionEntryForName:functionName];
    if (!entry) {
        NSLog(@"RDNA4Library: Function '%@' not found in metallib", functionName);
        return nil;
    }

    MTLFunctionType functionType;
    AIRNTCompileOptions options = {0};
    switch (entry->type) {
        case air2gfx12::FunctionType::Vertex:
            functionType = MTLFunctionTypeVertex;
            options.functionType = AIRNT_FUNCTION_TYPE_VERTEX;
            break;
        case air2gfx12::FunctionType::Fragment:
            functionType = MTLFunctionTypeFragment;
            options.functionType = AIRNT_FUNCTION_TYPE_FRAGMENT;
            break;
        default:
            functionType = MTLFunctionTypeKernel;
            options.functionType = AIRNT_FUNCTION_TYPE_KERNEL;
            break;
    }

    AIRNTExecutable executable = nullptr;
    char* errorMsg = NULL;
    options.entryPointName = entry->name.c_str();
    options.optimizationLevel = 2;
    AIRNTStatus result = _airntCompile(_airntContext, entry->bitcode.data(), entry->bitcode.size(),
                                       &options, &executable, &errorMsg);
    if (result != AIRNT_SUCCESS) {
        NSLog(@"RDNA4Library: Compilation failed for '%@': %s", functionName,
              errorMsg ? errorMsg : "unknown error");
        if (errorMsg) free(errorMsg);
        return nil;
    }

    const void* machineCode = nullptr;
    size_t machineCodeSize = 0;
    if (_airntGetMachineCode(executable, &machineCode, &machineCodeSize) != AIRNT_SUCCESS ||
        !machineCode || machineCodeSize == 0) {
        NSLog(@"RDNA4Library: No GFX12 machine code for '%@'", functionName);
        _airntFreeExecutable(executable);
        return nil;
    }

    AIRNTShaderMetadata metadata = {0};
    _airntGetMetadata(executable, &metadata);
    RDNA4CompiledShader* compiled = [[RDNA4CompiledShader alloc] init];
    compiled.functionName = functionName;
    compiled.functionType = functionType;
    void* copiedCode = malloc(machineCodeSize);
    memcpy(copiedCode, machineCode, machineCodeSize);
    // Bisect hook: move the EXP `done` flag from the primitive export to the
    // position export, the layout radv/m2tri use for an NGG passthrough VS.
    {
        const char* patch = getenv("N48_PATCH_EXP_DONE");
        if (patch && *patch && machineCodeSize >= 4) {
            uint32_t* words = (uint32_t*)copiedCode;
            for (size_t i = 0; i < machineCodeSize / 4; i++) {
                if (words[i] == 0xF80000CFu) words[i] = 0xF80001CFu;  // pos0: add done
            }
        }
        const char* clearPrim = getenv("N48_CLEAR_PRIM_DONE");
        if (clearPrim && *clearPrim && machineCodeSize >= 4) {
            uint32_t* words = (uint32_t*)copiedCode;
            for (size_t i = 0; i < machineCodeSize / 4; i++) {
                if (words[i] == 0xF8000941u) words[i] = 0xF8000841u;  // prim: clear done
            }
        }
    }
    compiled.machineCode = copiedCode;
    compiled.machineCodeSize = machineCodeSize;
    compiled.sgprCount = metadata.sgprCount;
    compiled.vgprCount = metadata.vgprCount;
    compiled.wavefrontSize = metadata.wavefrontSize;
    compiled.scratchBytes = metadata.scratchBytes;
    compiled.ldsBytes = metadata.ldsBytes;
    compiled.computePgmRsrc1 = metadata.computePgmRsrc1;
    compiled.computePgmRsrc2 = metadata.computePgmRsrc2;
    compiled.pgmRsrc1 = metadata.pgmRsrc1;
    compiled.pgmRsrc2 = metadata.pgmRsrc2;
    compiled.vertexParameterCount = metadata.vertexParameterCount;
    _compiledShaders[functionName] = compiled;
    _airntFreeExecutable(executable);

    NSLog(@"RDNA4Library: Compiled '%@' - %zu bytes, SGPR=%u, VGPR=%u",
          functionName, machineCodeSize, metadata.sgprCount, metadata.vgprCount);
    const char* dumpDir = getenv("N48_DUMP_SHADERS");
    if (dumpDir && *dumpDir) {
        NSString* path = [NSString stringWithFormat:@"%s/%@.bin", dumpDir, functionName];
        [[NSData dataWithBytes:compiled.machineCode length:machineCodeSize] writeToFile:path atomically:YES];
        NSLog(@"RDNA4Library: dumped '%@' to %@", functionName, path);
    }
    return [[RDNA4Function alloc] initWithDevice:_device
                                    compiledShader:compiled
                                              name:functionName];
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
    return _functionNames;
}

@end

@implementation RDNA4CompiledShader

- (void)dealloc {
    if (_machineCode) {
        free((void*)_machineCode);
    }
}

@end
