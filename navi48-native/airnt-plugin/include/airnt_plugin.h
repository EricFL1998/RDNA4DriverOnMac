#ifndef AIRNT_PLUGIN_H
#define AIRNT_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// AIRNT Plugin API Version
#define AIRNT_API_VERSION 0x00010000  // 1.0.0

// Function Types (from Metal Pipeline)
typedef enum {
    AIRNT_FUNCTION_TYPE_KERNEL    = 0,  // Compute Kernel
    AIRNT_FUNCTION_TYPE_VERTEX    = 1,  // Vertex Shader
    AIRNT_FUNCTION_TYPE_FRAGMENT  = 2,  // Fragment Shader
    AIRNT_FUNCTION_TYPE_TILE      = 3,  // Tile Shader (Apple Silicon)
    AIRNT_FUNCTION_TYPE_OBJECT    = 4,  // Object Shader (Apple Silicon)
    AIRNT_FUNCTION_TYPE_MESH      = 5   // Mesh Shader (Apple Silicon)
} AIRNTFunctionType;

// Compilation Status
typedef enum {
    AIRNT_SUCCESS                = 0,
    AIRNT_ERROR_INVALID_INPUT    = -1,
    AIRNT_ERROR_LINK_FAILED      = -2,
    AIRNT_ERROR_CODEGEN_FAILED   = -3,
    AIRNT_ERROR_OUT_OF_MEMORY    = -4,
    AIRNT_ERROR_NOT_IMPLEMENTED  = -5
} AIRNTStatus;

// Opaque Handle Types
typedef struct AIRNTContext_t* AIRNTContext;
typedef struct AIRNTExecutable_t* AIRNTExecutable;

// Shader Metadata (extracted after compilation)
typedef struct {
    uint32_t sgprCount;           // Scalar GPRs used
    uint32_t vgprCount;           // Vector GPRs used
    uint32_t wavefrontSize;       // 32 or 64
    uint32_t scratchBytes;        // Private memory per workitem
    uint32_t kernargBytes;        // Kernel argument size
    uint32_t ldsBytes;            // Local Data Share (threadgroup memory)
    uint32_t computePgmRsrc1;     // GFX12 COMPUTE_PGM_RSRC1
    uint32_t computePgmRsrc2;     // GFX12 COMPUTE_PGM_RSRC2
    uint32_t pgmRsrc1;            // GFX12 SPI_SHADER_PGM_RSRC1_GS/PS
    uint32_t pgmRsrc2;            // GFX12 SPI_SHADER_PGM_RSRC2_GS/PS
    uint32_t vertexParameterCount;
} AIRNTShaderMetadata;

// Resource binding layout of the generated shader. The driver writes the
// descriptor address (low 32 bits at userSgpr, high 32 bits at userSgpr + 1)
// for the Metal resource bound at locationIndex.
typedef enum {
    AIRNT_BINDING_TEXTURE = 0,
    AIRNT_BINDING_SAMPLER = 1,
    AIRNT_BINDING_BUFFER  = 2
} AIRNTBindingKind;

typedef struct {
    uint32_t userSgpr;        // Absolute SGPR index of the 64-bit descriptor address
    uint32_t locationIndex;   // Metal binding index
    uint32_t kind;            // AIRNTBindingKind
    uint32_t addressSpace;    // AIR address space (1 = device, 2/4 = constant)
} AIRNTBinding;

// Compilation Options
typedef struct {
    const char* entryPointName;   // Kernel/Function name
    AIRNTFunctionType functionType;
    const char* runtimeLibraryPath; // Path to runtime.amdil.bc (NULL = auto-detect)
    uint32_t optimizationLevel;   // 0=O0, 1=O1, 2=O2, 3=O3
    bool emitDebugInfo;           // Include DWARF debug info
    bool verbose;                 // Log compilation steps
} AIRNTCompileOptions;

//==============================================================================
// Plugin Lifecycle
//==============================================================================

/**
 * Initialize AIRNT plugin context
 * @param outContext Output context handle
 * @param apiVersion Client expected API version (use AIRNT_API_VERSION)
 * @return AIRNT_SUCCESS or error code
 */
AIRNTStatus AIRNTInit(AIRNTContext* outContext, uint32_t apiVersion);

/**
 * Destroy AIRNT plugin context
 * @param context Context to destroy
 */
void AIRNTDestroy(AIRNTContext context);

/**
 * Query plugin version
 * @return 32-bit version (major.minor.patch)
 */
uint32_t AIRNTGetVersion(void);

//==============================================================================
// Compilation API
//==============================================================================

/**
 * Compile AIR bitcode to GFX12 executable
 * @param context Plugin context
 * @param airBitcode Input AIR bitcode buffer
 * @param bitcodeSize Size of bitcode in bytes
 * @param options Compilation options
 * @param outExecutable Output executable handle
 * @param outErrorMessage Optional error message (caller must free with AIRNTFreeString)
 * @return AIRNT_SUCCESS or error code
 */
AIRNTStatus AIRNTCompile(AIRNTContext context,
                         const void* airBitcode,
                         size_t bitcodeSize,
                         const AIRNTCompileOptions* options,
                         AIRNTExecutable* outExecutable,
                         char** outErrorMessage);

/**
 * Get compiled machine code
 * @param executable Compiled executable
 * @param outCode Output code buffer (do not free, owned by executable)
 * @param outSize Output code size in bytes
 * @return AIRNT_SUCCESS or error code
 */
AIRNTStatus AIRNTGetMachineCode(AIRNTExecutable executable,
                                const void** outCode,
                                size_t* outSize);

/**
 * Get shader metadata (SGPR/VGPR/Scratch etc)
 * @param executable Compiled executable
 * @param outMetadata Output metadata structure
 * @return AIRNT_SUCCESS or error code
 */
AIRNTStatus AIRNTGetMetadata(AIRNTExecutable executable,
                             AIRNTShaderMetadata* outMetadata);

/**
 * Get the resource binding table of the compiled shader
 * @param executable Compiled executable
 * @param outBindings Output binding array (owned by the executable; may be NULL when count is 0)
 * @param outCount Output number of bindings
 * @return AIRNT_SUCCESS or error code
 */
AIRNTStatus AIRNTGetBindings(AIRNTExecutable executable,
                             const AIRNTBinding** outBindings,
                             size_t* outCount);

/**
 * Get GFX12 assembly listing (for debugging)
 * @param executable Compiled executable
 * @param outAssembly Output assembly string (caller must free with AIRNTFreeString)
 * @return AIRNT_SUCCESS or error code
 */
AIRNTStatus AIRNTGetAssembly(AIRNTExecutable executable,
                             char** outAssembly);

/**
 * Free compiled executable
 * @param executable Executable to free
 */
void AIRNTFreeExecutable(AIRNTExecutable executable);

/**
 * Free string returned by plugin
 * @param str String to free
 */
void AIRNTFreeString(char* str);

//==============================================================================
// Pipeline Compilation (FlatBuffers interface, used by Metal.framework)
//==============================================================================

/**
 * Compile pipeline from FlatBuffers descriptor (Metal internal format)
 * @param context Plugin context
 * @param flatbufferData FlatBuffers serialized pipeline descriptor
 * @param dataSize Size of descriptor in bytes
 * @param outExecutable Output executable handle
 * @param outErrorMessage Optional error message
 * @return AIRNT_SUCCESS or error code
 */
AIRNTStatus AIRNTCompilePipeline(AIRNTContext context,
                                 const void* flatbufferData,
                                 size_t dataSize,
                                 AIRNTExecutable* outExecutable,
                                 char** outErrorMessage);

#ifdef __cplusplus
}
#endif

#endif // AIRNT_PLUGIN_H
