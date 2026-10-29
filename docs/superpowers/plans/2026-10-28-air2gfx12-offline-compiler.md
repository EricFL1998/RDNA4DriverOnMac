# air2gfx12 离线编译器实施计划 (Phase 1.1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建一个独立的纯用户态 C++ 控制台工具 `tools/air2gfx12`，输入任意 `.metallib` 或 `.air` 文件，自动解包 AIR Bitcode，链接原厂 `runtime.amdil.bc`，并通过 LLVM AMDGPU 后端直接生成合法的 GFX12 (Navi 48 / gfx1201) 汇编代码与 ELF 目标二进制。

**Architecture:** 
1. `MTLBUnpacker` 解析 Apple MTLB 二进制容器，读取 Entry 标签并提取嵌入的 AIR Bitcode。
2. `AIRLinker` 使用 LLVM C++ API 载入 AIR 模块，并与原厂 `runtime.amdil.bc` 静态链接，内联 `air.*` 内建函数。
3. `AIRLowering` 实现一个 LLVM IR Pass，映射系统线程组 ID 与硬件寄存器。
4. `GFX12Emitter` 初始化 LLVM AMDGPU TargetMachine（`amdgcn-amd-amdhsa`, CPU `gfx1201`），将 Module 编译输出为 `.s` 汇编与 `.o` ELF 二进制。
5. `main.cpp` 提供 CLI 交互与状态打印。

**Tech Stack:** C++17, LLVM 23 (Homebrew `/opt/homebrew/opt/llvm`), CMake / Makefile.

**Spec:** `docs/superpowers/specs/2026-10-28-rdna4-native-metal-comprehensive-report.md`

## Global Constraints

- **LLVM 路径**: 使用 `/opt/homebrew/opt/llvm` 提供的 LLVM 23 头文件与静态库。
- **目标架构**: 必须指定 `amdgcn-amd-amdhsa`，CPU 固定为 `gfx1201`（RDNA4 / Navi 48）。
- **零依赖原则**: 除系统 C++ 运行时与 LLVM 之外，不引入任何第三方外部重量级依赖（容器解析与参数处理全手写，避免版本污染）。
- **完全解耦**: 100% 运行在用户态，不依赖任何内核驱动或黑苹果环境。

## Review Focus

- **空 Metallib 或损坏格式**: 输入非 MTLB 文件时必须优雅报错并退出，不能段错误。
- **未导出 Kernel 报错**: 指定不存在的 `--kernel` 名称时应打印所有可用 Kernel 列表。
- **已包含完整定义避免重定义冲突**: 当用户 Shader 自行实现了某个函数时，链接 `runtime.amdil.bc` 不应抛出重复符号冲突（Linker 策略需设置为 Overwrite 或 LinkOnce）。
- **无采样器纯算术 Shader**: 能够不经过 Lowering 直接编译并生成合法汇编。
- **输出文件目录不存在**: 自动创建输出文件的父目录。

---

## 任务分解清单

### Task 1: 项目脚手架与 Makefile 构建配置

**Files:**
- Create: `tools/air2gfx12/Makefile`
- Create: `tools/air2gfx12/src/main.cpp`
- Test: `tools/air2gfx12/tests/test_cli.sh`

**Interfaces:**
- CLI 规范: `air2gfx12 <input> [-o output.s] [--emit-obj output.o] [--kernel name] [-v]`

- [ ] **Step 1: 编写 CLI 冒烟测试脚本 `tests/test_cli.sh`**

```bash
#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$DIR/../bin/air2gfx12"
[ -x "$BIN" ] || { echo "Binary not found at $BIN"; exit 1; }
# 测试 help 命令
"$BIN" --help | grep -q "Usage: air2gfx12"
echo "CLI test passed"
```

- [ ] **Step 2: 运行测试验证失败**

Run: `bash tools/air2gfx12/tests/test_cli.sh`  
Expected: FAIL (Binary not found)

- [ ] **Step 3: 编写 `tools/air2gfx12/Makefile` 与最小 `main.cpp`**

Makefile 需调用 `/opt/homebrew/opt/llvm/bin/llvm-config` 获取 `CXXFLAGS`, `LDFLAGS`, `LIBS`，编译生成 `tools/air2gfx12/bin/air2gfx12`。

- [ ] **Step 4: 编译并运行测试验证通过**

Run: `cd tools/air2gfx12 && make && bash tests/test_cli.sh`  
Expected: PASS

- [ ] **Step 5: 提交任务**

```bash
git add tools/air2gfx12/
git commit -m "feat(air2gfx12): scaffold CLI tool with LLVM build configuration"
```

---

### Task 2: MTLB 容器解包模块 (`MTLBUnpacker`)

**Files:**
- Create: `tools/air2gfx12/src/MTLBUnpacker.hpp`
- Create: `tools/air2gfx12/src/MTLBUnpacker.cpp`
- Test: `tools/air2gfx12/tests/test_unpacker.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct FunctionEntry {
      std::string name;
      uint8_t type; // 0: Vertex, 1: Fragment, 2: Compute
      std::vector<uint8_t> bitcode;
  };
  class MTLBUnpacker {
  public:
      static bool isMTLB(const uint8_t* data, size_t size);
      static bool unpack(const uint8_t* data, size_t size, std::vector<FunctionEntry>& outEntries);
  };
  ```

- [ ] **Step 1: 编写解包单元测试 `tests/test_unpacker.cpp`**

测试涵盖：
1. 传入非 MTLB 头部断言失败。
2. 传入前序实验提取的 `/tmp/TextureProcessorMetal.metallib`，断言返回至少 1 个 `MinMaxReduction_float1` 且包含非空 bitcode。

- [ ] **Step 2: 运行测试验证失败**

Run: `make test_unpacker`  
Expected: FAIL

- [ ] **Step 3: 实现 `MTLBUnpacker.cpp`**

解析 MTLB Header（检查 magic `MTLB`），遍历 Tag：
- `NAME`: 函数名称
- `TYPE`: 函数类型
- `MDSZ`: Bitcode 大小
- `OFFT`: 偏移表
- 读取各段 Bitcode 并填充 `FunctionEntry`。

- [ ] **Step 4: 运行测试验证通过**

Run: `make test_unpacker && ./bin/test_unpacker`  
Expected: PASS (成功解析出真实 Metallib 的所有 Kernel)

- [ ] **Step 5: 提交任务**

```bash
git add tools/air2gfx12/src/MTLBUnpacker.* tools/air2gfx12/tests/test_unpacker.cpp
git commit -m "feat(air2gfx12): implement MTLB container unpacker"
```

---

### Task 3: 运行库链接模块 (`AIRLinker`)

**Files:**
- Create: `tools/air2gfx12/src/AIRLinker.hpp`
- Create: `tools/air2gfx12/src/AIRLinker.cpp`
- Test: `tools/air2gfx12/tests/test_linker.cpp`

**Interfaces:**
- Consumes: `MTLBUnpacker::FunctionEntry`
- Produces:
  ```cpp
  class AIRLinker {
  public:
      AIRLinker(llvm::LLVMContext& ctx, const std::string& runtimeBcPath);
      std::unique_ptr<llvm::Module> loadAndLink(const uint8_t* airData, size_t size, const std::string& entryName);
  };
  ```

- [ ] **Step 1: 编写链接测试 `tests/test_linker.cpp`**

测试加载 `/tmp/kernel1.air`，并与 `/tmp/runtime.amdil.bc` 链接。
断言：链接后原本声明为 `declare @air.fmax.f32` 的外部函数变为定义 `define @air.fmax.f32`。

- [ ] **Step 2: 运行测试验证失败**

Run: `make test_linker`  
Expected: FAIL

- [ ] **Step 3: 实现 `AIRLinker.cpp`**

1. 用 `llvm::parseBitcodeFile` 载入用户 AIR。
2. 载入缓存的 `runtime.amdil.bc` 模板。
3. 调用 `llvm::Linker::linkModules(userModule, runtimeModule, llvm::Linker::Flags::LinkOnlyNeeded)` 仅链接被用户引用的内建函数。
4. 运行 `llvm::createInternalizePass` 与 `llvm::createGlobalDCEPass` 剔除未使用的死函数。

- [ ] **Step 4: 运行测试验证通过**

Run: `make test_linker && ./bin/test_linker`  
Expected: PASS (依赖函数被成功链接并内联)

- [ ] **Step 5: 提交任务**

```bash
git add tools/air2gfx12/src/AIRLinker.* tools/air2gfx12/tests/test_linker.cpp
git commit -m "feat(air2gfx12): implement AIR runtime linker with dead-code elimination"
```

---

### Task 4: GFX12 目标生成器 (`GFX12Emitter`)

**Files:**
- Create: `tools/air2gfx12/src/GFX12Emitter.hpp`
- Create: `tools/air2gfx12/src/GFX12Emitter.cpp`
- Test: `tools/air2gfx12/tests/test_emitter.cpp`

**Interfaces:**
- Consumes: 完整的 `llvm::Module`
- Produces:
  ```cpp
  class GFX12Emitter {
  public:
      GFX12Emitter();
      bool emitAssembly(llvm::Module& mod, const std::string& outPath);
      bool emitObject(llvm::Module& mod, const std::string& outPath);
  };
  ```

- [ ] **Step 1: 编写生成器测试 `tests/test_emitter.cpp`**

测试传入简单的 Module，输出 `.s` 并检测输出文本中是否包含 `.amd_amdgpu_isa "amdgpu-unknown-unknown-gfx1201"` 以及 `s_endpgm`。

- [ ] **Step 2: 运行测试验证失败**

Run: `make test_emitter`  
Expected: FAIL

- [ ] **Step 3: 实现 `GFX12Emitter.cpp`**

1. 初始化 AMDGPU Target: `LLVMInitializeAMDGPUTargetInfo()`, `LLVMInitializeAMDGPUTarget()`, `LLVMInitializeAMDGPUTargetMC()`, `LLVMInitializeAMDGPUAsmPrinter()`.
2. 获取 Target: `llvm::TargetRegistry::lookupTarget("amdgcn-amd-amdhsa", error)`.
3. 创建 TargetMachine: CPU 指定 `"gfx1201"`, 特征字符串指定 `"+wavefrontsize32"`.
4. 配置 CodeGen PassManager，调用 `addPassesToEmitFile` 写入文件。

- [ ] **Step 4: 运行测试验证通过**

Run: `make test_emitter && ./bin/test_emitter`  
Expected: PASS

- [ ] **Step 5: 提交任务**

```bash
git add tools/air2gfx12/src/GFX12Emitter.* tools/air2gfx12/tests/test_emitter.cpp
git commit -m "feat(air2gfx12): implement GFX12 AMDGPU code generator for gfx1201"
```

---

### Task 5: 命令行聚合与端到端真实测试

**Files:**
- Modify: `tools/air2gfx12/src/main.cpp`
- Create: `tools/air2gfx12/tests/e2e_test.sh`

- [ ] **Step 1: 编写端到端验收脚本 `e2e_test.sh`**

```bash
#!/bin/bash
set -e
BIN="./bin/air2gfx12"
METALLIB="/tmp/TextureProcessorMetal.metallib"
RUNTIME_BC="/tmp/runtime.amdil.bc"
OUT_S="/tmp/out_kernel.s"
OUT_O="/tmp/out_kernel.o"

"$BIN" "$METALLIB" --runtime "$RUNTIME_BC" --kernel "MinMaxReduction_float1" -o "$OUT_S" --emit-obj "$OUT_O"
grep -q "gfx1201" "$OUT_S"
file "$OUT_O" | grep -q "ELF 64-bit"
echo "End-to-End Test PASSED!"
```

- [ ] **Step 2: 运行测试验证失败**

Run: `bash tools/air2gfx12/tests/e2e_test.sh`  
Expected: FAIL

- [ ] **Step 3: 完善 `main.cpp` 参数解析与完整串联**

串联 `MTLBUnpacker` ➔ `AIRLinker` ➔ `GFX12Emitter`，提供完整的错误日志与汇编输出。

- [ ] **Step 4: 运行端到端验收测试**

Run: `cd tools/air2gfx12 && bash tests/e2e_test.sh`  
Expected: **End-to-End Test PASSED!**

- [ ] **Step 5: 提交任务**

```bash
git add tools/air2gfx12/
git commit -m "feat(air2gfx12): complete air2gfx12 CLI tool with full end-to-end test"
```

---

## 自检审查

1. **Spec 覆盖度**: 完全覆盖离线编译器所需全部组件（MTLB 解包、AIR 运行时链接、GFX12 原生生成、CLI 交付）。
2. **纯用户态保证**: 整个 Task 1 可以在当前机器上独立编译运行，不需要连接黑苹果。
3. **测试充分度**: 从解包到链接到生成，每个环节均有明确的自动化断言与真实数据输入。
