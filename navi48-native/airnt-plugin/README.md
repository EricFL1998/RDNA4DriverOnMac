# AIRNT Plugin - RDNA4 Native Metal Compiler Plugin

将 `air2gfx12` 编译器封装为标准 AIRNT (Apple Intermediate Representation Native Target) 动态库插件，供 Metal.framework 调用。

---

## 架构

```
Metal.framework (Apple)
        ↓ dlopen()
libAMDNTPlugin.dylib (本项目)
        ↓ 内部使用
air2gfx12 核心组件
  - MTLBUnpacker
  - AIRLinker
  - GFX12Emitter
  - ShaderMetadata
```

---

## API 接口

### 初始化与清理

```c
AIRNTContext context;
AIRNTInit(&context, AIRNT_API_VERSION);
// ... 使用 context ...
AIRNTDestroy(context);
```

### 编译 AIR Bitcode

```c
AIRNTCompileOptions opts = {
    .entryPointName = "vectorAdd",
    .functionType = AIRNT_FUNCTION_TYPE_KERNEL,
    .runtimeLibraryPath = NULL,  // Auto-detect
    .optimizationLevel = 2,
    .emitDebugInfo = false,
    .verbose = false
};

AIRNTExecutable exec;
char* error = NULL;
AIRNTStatus status = AIRNTCompile(
    context,
    airBitcode, bitcodeSize,
    &opts,
    &exec,
    &error
);

if (status != AIRNT_SUCCESS) {
    fprintf(stderr, "Compilation failed: %s\n", error);
    AIRNTFreeString(error);
    return;
}

// Get machine code (ELF)
const void* code;
size_t codeSize;
AIRNTGetMachineCode(exec, &code, &codeSize);

// Get metadata
AIRNTShaderMetadata meta;
AIRNTGetMetadata(exec, &meta);
printf("SGPRs: %u, VGPRs: %u\n", meta.sgprCount, meta.vgprCount);

// Get assembly (for debugging)
char* assembly;
AIRNTGetAssembly(exec, &assembly);
printf("%s\n", assembly);
AIRNTFreeString(assembly);

AIRNTFreeExecutable(exec);
```

---

## 构建

```bash
cd navi48-native/airnt-plugin
make
```

生成 `libAMDNTPlugin.dylib`（动态链接库）。

---

## 部署

```bash
# 将 dylib 部署到系统路径（需要 SIP 禁用或签名）
sudo cp libAMDNTPlugin.dylib /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/
```

---

## 函数类型支持

| Metal 类型 | AIRNT 常量 | 状态 |
|-----------|-----------|------|
| Compute Kernel | `AIRNT_FUNCTION_TYPE_KERNEL` | ✅ 支持 |
| Vertex Shader | `AIRNT_FUNCTION_TYPE_VERTEX` | 🔜 计划中 |
| Fragment Shader | `AIRNT_FUNCTION_TYPE_FRAGMENT` | 🔜 计划中 |
| Tile Shader | `AIRNT_FUNCTION_TYPE_TILE` | ❌ 不适用 (Apple Silicon) |

---

## 元数据结构

```c
typedef struct {
    uint32_t sgprCount;           // Scalar GPRs used
    uint32_t vgprCount;           // Vector GPRs used
    uint32_t wavefrontSize;       // 32 or 64
    uint32_t scratchBytes;        // Private memory per workitem
    uint32_t kernargBytes;        // Kernel argument size
    uint32_t ldsBytes;            // Local Data Share (threadgroup)
    uint32_t computePgmRsrc1;     // GFX12 COMPUTE_PGM_RSRC1
    uint32_t computePgmRsrc2;     // GFX12 COMPUTE_PGM_RSRC2
} AIRNTShaderMetadata;
```

---

## 错误处理

所有编译函数都返回 `AIRNTStatus`：

```c
typedef enum {
    AIRNT_SUCCESS                = 0,
    AIRNT_ERROR_INVALID_INPUT    = -1,
    AIRNT_ERROR_LINK_FAILED      = -2,
    AIRNT_ERROR_CODEGEN_FAILED   = -3,
    AIRNT_ERROR_OUT_OF_MEMORY    = -4,
    AIRNT_ERROR_NOT_IMPLEMENTED  = -5
} AIRNTStatus;
```

错误信息通过 `outErrorMessage` 返回（调用方需用 `AIRNTFreeString` 释放）。

---

## 限制

- **FlatBuffers 支持**: `AIRNTCompilePipeline` 尚未实现（需要逆向 Metal 内部管线格式）
- **优化级别**: 目前所有级别都使用 LLVM `-O2`
- **调试信息**: `emitDebugInfo` 标志暂不生效

---

## 测试

```bash
# 编译测试程序
make test

# 运行测试
./bin/test_airnt_plugin
```

---

*Part of RDNA4 macOS Native Metal Driver Project - Phase 3*
