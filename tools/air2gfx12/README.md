# air2gfx12 - RDNA4 原生 Metal Shader 编译器

[![Build](https://img.shields.io/badge/Build-Passing-success)]() [![Tests](https://img.shields.io/badge/Tests-8%2F8%20Pass-brightgreen)]() [![Shaders](https://img.shields.io/badge/Golden%20Tests-45%2F45%20Pass-blue)]()

将 Apple Metal Shader (`.metallib` / `.air`) 直接编译为 AMD RDNA4 (Navi 48 / GFX1201) 原生机器码的离线编译器工具链。

---

## ✨ 特性

- ✅ **完整 Metal → GFX12 编译流程**：从 `.metallib` 到原生机器码一步到位
- ✅ **自动运行库链接**：智能定位并按需链接 Apple 原厂 `runtime.amdil.bc`（29.8万行 IR，3526 个内建函数）
- ✅ **地址空间自动对齐**：Metal `addrspace(2)` → AMDGPU `addrspace(4)` 无缝映射
- ✅ **硬件资源统计**：自动提取 SGPR/VGPR/Scratch 占用并计算 GFX12 寄存器配置
- ✅ **PM4 命令流生成器**：可直接生成驱动提交用的 Type-3 封包
- ✅ **生产级验证**：45 个真实 Metal Shader 100% 编译通过（平均 0.2 秒/着色器）

---

## 🚀 快速开始

### 安装依赖

```bash
# macOS (Homebrew)
brew install llvm

# 验证 LLVM 版本 (需要 23+)
/opt/homebrew/opt/llvm/bin/llc --version | grep AMDGPU
```

### 编译工具链

```bash
cd tools/air2gfx12
make
```

### 基本用法

```bash
# 查看 metallib 包含的着色器
./bin/air2gfx12 input.metallib --list

# 编译单个 Kernel
./bin/air2gfx12 input.metallib --kernel "MyKernel" \
    -o output.s --emit-obj output.o --stats

# 批量编译所有着色器
./scripts/batch_compile.sh input.metallib output_dir/

# 统计分析资源占用
./scripts/analyze_stats.sh input.metallib stats.csv
```

---

## 📚 文档

| 文档 | 说明 |
|------|------|
| [`USAGE.md`](USAGE.md) | 详细使用指南与命令行选项 |
| [`examples/end_to_end.sh`](examples/end_to_end.sh) | 完整示例：从 Metal 源码到 GFX12 机器码 |
| [`tests/phase1_full_verification.sh`](tests/phase1_full_verification.sh) | 完整验收测试脚本（8 项检查） |

---

## 🏗️ 架构

```
┌─────────────────┐
│  .metallib      │  Apple Metal Shader 库
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ MTLBUnpacker    │  容器解包，提取 AIR Bitcode
└────────┬────────┘
         │
         ▼
┌─────────────────┐     ┌──────────────────────┐
│ AIRLinker       │ ◄───│ runtime.amdil.bc     │  Apple 原厂运行库
│                 │     │ (3526 内建函数)       │
│ • 按需链接      │     └──────────────────────┘
│ • 地址空间映射   │
│ • 调用约定提升   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ LLVM AMDGPU     │  GFX1201 Target Machine
│ Backend         │
└────────┬────────┘
         │
         ├─────────────────┬─────────────────┐
         ▼                 ▼                 ▼
    ┌─────────┐      ┌──────────┐     ┌──────────┐
    │ .s      │      │ .o (ELF) │     │ Metadata │
    │ Assembly│      │  Object  │     │ (Stats)  │
    └─────────┘      └──────────┘     └──────────┘
                                             │
                                             ▼
                                      ┌──────────────┐
                                      │ PM4Builder   │
                                      │ (命令流封包) │
                                      └──────────────┘
```

---

## ✅ 验证状态

### 单元测试 (5/5 通过)

```bash
make test
```

- ✅ `test_unpacker`: MTLB 容器解析
- ✅ `test_linker`: AIR 运行库链接与符号解析
- ✅ `test_emitter`: GFX1201 代码生成
- ✅ `test_metadata`: 寄存器统计与计算
- ✅ `test_pm4`: PM4 命令流封包器

### 生产验证 (45/45 通过)

```bash
make test_golden
```

真实生产环境提取的 `TextureProcessorMetal.metallib`（45 个着色器）：
- MinMaxReduction (float/uint/int × 1/2/3/4) - **12 个 ✅**
- Histogram (float/uint/int × 1/2/3/4) - **12 个 ✅**
- Tonemap (float/uint/int × 1/2/3/4) - **12 个 ✅**
- RenderToTexture (Vertex + 6 Fragments) - **7 个 ✅**
- 其他辅助着色器 - **2 个 ✅**

平均编译耗时：**0.2 秒/着色器**  
总耗时：**9 秒（45 个着色器）**

### 完整 Phase 1 验收 (8/8 通过)

```bash
make verify_phase1
```

包括环境检测、编译构建、全部单元测试、CLI 测试、端到端测试与批量压力测试。

---

## 📊 实测数据示例

**Kernel**: `MinMaxReduction_float1`

| 指标 | 值 |
|------|---|
| **SGPRs** | 73 |
| **VGPRs** | 78 |
| **Wave Size** | 32 |
| **Scratch** | 96 B |
| **Kernarg** | 304 B |
| **GFX12 RSRC1** | `0x10c0253` |
| **GFX12 RSRC2** | `0x1381` |
| **汇编大小** | 8.4 KB (ELF) |

---

## 🛠️ 高级功能

### PM4 命令流生成

```cpp
#include "PM4Builder.hpp"

air2gfx12::PM4Builder pm4;

// 配置 Shader 程序
uint64_t shaderVA = 0x400017B00;
pm4.setComputeProgram(shaderVA, rsrc1, rsrc2);

// 配置工作组大小
pm4.setComputeWorkgroupSize(32, 1, 1);

// 设置用户参数（buffer 地址）
pm4.setComputeUserData(0, bufferGpuAddr);

// 分发计算任务
pm4.emitDispatchDirect(64, 1, 1);

// 刷新缓存
pm4.emitCsPartialFlush();

// 获取封包的命令流
const auto& packets = pm4.getPackets();
```

详见 `src/PM4Builder.hpp`。

---

## 🔗 依赖项目

- **LLVM 23+**: AMDGPU Target Backend
- **Apple Metal SDK**: 提取 `runtime.amdil.bc`
- **Cobalt64**: 参考实现与硬件逆向数据

---

## 📝 License

MIT License - 详见项目根目录 LICENSE 文件

---

## 🙏 致谢

本工具链是 **RDNA4 macOS Native Metal Driver** 项目的 Phase 1 交付物。

感谢：
- **Cobalt64 项目** 提供的硬件逆向数据与参考实现
- **LLVM 社区** 持续维护的 AMDGPU 后端
- **Apple Metal 团队** 提供的高质量运行库

---

*Copyright © 2026 RDNA4 macOS Driver Project.*
