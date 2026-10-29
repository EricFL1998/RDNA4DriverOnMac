# RDNA4 原生 Metal 预研方案

## 目标定义

参考苹果为旧 AMD GPU（Polaris/Vega/Navi）实现的原生 Metal 驱动架构，为 RDNA4 (GFX12/Navi 48) 写一个自己的原生 Metal 实现。不做完全从零的 shader 编译器，而是复用 LLVM AMDGPU 后端。

---

## 一、苹果原生 Metal 驱动架构（逆向分析）

### 苹果的驱动栈

```
Metal App
    ↓ Metal.framework (公开 API)
    ↓
MTLDevice → AMDRadeonX6000MTLDriver.bundle
    ├── libSC.dylib (Shader Compiler：AIR → GCN/RDNA ISA)
    ├── 命令编码器（Metal命令 → PM4 命令流）
    ├── 管线状态对象（PSO 缓存、编译）
    └── 资源管理（Buffer/Texture → GPU内存）
    ↓
AMDRadeonX6000.kext (内核驱动)
    ↓
GPU 硬件
```

### 关键组件分析

| 组件 | 苹果的实现 | 文件位置 |
|------|-----------|---------|
| **MTLDriver Bundle** | `AMDRadeonX6000MTLDriver.bundle` | `/System/Library/Extensions/` |
| **Shader 编译器** | `libSC.dylib`（内部链接到 LLVM） | `AMDShared.bundle/Contents/PlugIns/` |
| **内核驱动** | `AMDRadeonX6000.kext` | `/System/Library/Extensions/` |
| **Framework** | `AGXMetalGXX.bundle`（Apple Silicon）/ `AMDRadeonX6000MTLDriver.bundle`（AMD） | `/System/Library/Extensions/` |

### 苹果 Metal Bundle 的注册机制

苹果的 Metal 设备发现流程：
1. IOKit 内核驱动注册 `IOAccelerator` 服务
2. `GPUWrangler` / `IOGPUFamily` 框架在用户空间发现加速器
3. Metal.framework 根据 GPU 类型加载对应的 `MTLDriver.bundle`
4. Bundle 内的类实现 `MTLDevice` 协议的全部方法

### Cobalt64 的实现方式（已验证）

```
Metal App
    ↓ Metal.framework
    ↓
IOAcceleratorFamily (苹果私有框架)
    ↓
Navi48Accelerator (AuxKC 的 aux kext，IOAccelerator 子类)
    ↓ ← N48MetalOps 表 (ABI v4, 160 bytes)
Navi48Bringup.kext (ESP-injected 主驱动)
    ├── xlat12 (gfx10.3 → gfx12 PM4 翻译)
    ├── Navi48MetalNub (状态机)
    └── Navi48NativeClient (用户空间通道)
    ↓
GPU 硬件
```

Cobalt64 的关键洞察：**它不做 shader 编译，而是让苹果的 Metal 编译器为 gfx10.3 生成 PM4，然后用 xlat12 翻译器把寄存器地址和字段映射到 gfx12**。这是一种"寄生"方式——优点是快速可用，缺点是受限于苹果为 gfx10.3 做的优化，且无法利用 GFX12 的新指令。

---

## 二、我们的方案：基于 LLVM 的原生路径

### 核心技术洞察

**AIR 本质就是 LLVM bitcode**。这不是假设，是已验证的事实：

1. `.air` 文件的 magic number 是 `DE C0 17 0B`（标准 LLVM bitcode wrapper）
2. `.metallib` 是一个 MTLB 容器，内部封装了 AIR bitcode + 函数元数据
3. `llvm-dis` 可以直接反汇编 `.air` 文件为可读的 LLVM IR
4. `llc -march=amdgcn` 可以直接将 AIR bitcode 编译为 AMDGPU 汇编

AIR 与标准 LLVM IR 的差异（来自 [metal-ir-pipeline](https://github.com/imperatormk/metal-ir-pipeline) 的 26 个 pass 分析）：

| 差异 | 描述 | 处理难度 |
|------|------|---------|
| **Typed pointers** | AIR 用 `float*`、`i32*` 而非 opaque `ptr` | 中（LLVM 内部有转换） |
| **AIR intrinsics** | `air.convert.*`、`air.sample_texture.*` 等 | 高（需要映射到 AMDGPU 指令） |
| **Address spaces** | `addrspace(0)`=device, `(1)`=constant, `(2)`=device_readonly, `(3)`=threadgroup | 中（映射到 AMDGPU 地址空间） |
| **No bfloat16** | Metal v1 bitcode 无 bfloat 类型 | 低 |
| **No sub-32-bit int ops** | `sitofp i8→float` 需要经过 i32 | 低 |
| **MTLB container** | 自定义容器格式封装 bitcode | 低（格式已完全逆向） |

### 技术路径

```
Metal App
    ↓ Metal.framework
    ↓
RDNA4Metal.bundle (我们的实现)
    │
    ├── [metallib 解析器]
    │     ↓ 提取 AIR bitcode + 函数元数据
    │
    ├── [AIR → AMDGPU IR 转换]
    │     ├── air.* intrinsics → AMDGPU intrinsics 映射
    │     ├── address space 重映射 (Metal → AMDGPU)
    │     ├── typed pointers → opaque pointers
    │     └── Metal 系统值 → AMDGPU 内置变量
    │     ↓ 标准 LLVM IR (target: amdgcn-amd-amdhsa, gfx1201)
    │
    ├── [LLVM AMDGPU 后端] (现成的!)
    │     ├── 指令选择
    │     ├── 寄存器分配
    │     ├── 指令调度
    │     └── 二进制生成
    │     ↓ GFX12 机器码 (ELF)
    │
    ├── [Shader 提交]
    │     ├── 提取 shader 二进制
    │     ├── 生成 shader header (SPI_SHADER_PGM_RSRC1/2)
    │     └── 配置 shader descriptor
    │     ↓ PM4 命令流
    │
    └── [命令编码器]
          ├── Render command → PM4 draw packets
          ├── Compute command → PM4 dispatch packets
          └── Blit command → PM4 DMA packets
          ↓
RDNA4.kext (内核驱动)
    ↓
GPU 硬件
```

### 与苹果方案的关键区别

| 维度 | 苹果的方式 | 我们的方式 |
|------|-----------|-----------|
| **Shader 编译器** | 自研 libSC（闭源） | LLVM AMDGPU 后端（开源，已支持 gfx1201） |
| **前端** | 直接处理 AIR | AIR → 标准 LLVM IR → AMDGPU |
| **优化** | libSC 内置 GPU 特定优化 | LLVM 标准优化 + AMDGPU 后端优化 |
| **命令编码** | 直接为目标 ISA 生成 PM4 | 同样直接生成 GFX12 PM4（参考 Mesa/Cobalt64） |
| **Bundle 注册** | 通过 IOAcceleratorFamily | 同样通过 IOAcceleratorFamily |

---

## 三、需要的数据和资源

### 3.1 AIR 格式数据

**已有：**
- [x] metallib 容器格式完全逆向（[worthdoingbadly.com](https://worthdoingbadly.com/metalbitcode/)）
- [x] AIR bitcode 就是 LLVM bitcode（已验证）
- [x] AIR intrinsics 完整列表可从 metal 编译器输出的 .ll 文件提取
- [x] [metal-ir-pipeline](https://github.com/imperatormk/metal-ir-pipeline) 记录了 AIR 与标准 IR 的全部 26 处差异

**需要收集：**
- [ ] **AIR intrinsics 完整映射表**：编译各种 Metal shader 样例，收集所有 `air.*` intrinsics，建立到 AMDGPU intrinsics 的映射
- [ ] **Metal 系统值映射**：`[[vertex_id]]`、`[[position]]`、`[[thread_position_in_grid]]` 等到 AMDGPU 系统寄存器的映射
- [ ] **纹理采样指令映射**：`air.sample_texture.*` 到 `IMAGE_SAMPLE` 的参数映射

**收集方法：**
```bash
# 在黑苹果上编译 Metal shader 到 AIR
xcrun -sdk macosx metal -c shader.metal -o shader.air

# 反汇编为 LLVM IR
xcrun -sdk macosx metal -S -emit-llvm shader.metal -o shader.ll
# 或
llvm-dis shader.air -o shader.ll

# 查看所有 AIR intrinsics
grep "call.*@air\." shader.ll | sort -u

# 查看 metallib 结构
xcrun -sdk macosx metal-nm library.metallib
xcrun -sdk macosx metal-objdump -d shader.air
```

### 3.2 GFX12 ISA 数据

**已有：**
- [x] LLVM AMDGPU 后端完整支持 gfx1201（本机 LLVM 23.1.4 已验证）
- [x] Mesa RADV 源码（`src/amd/compiler/`）包含完整的 GFX12 指令编码
- [x] Cobalt64 的 xlat12 翻译表（`xlat12_tables.h`）包含 gfx10.3→gfx12 寄存器映射
- [x] Linux amdgpu 驱动源码

**需要收集：**
- [ ] **GFX12 shader header 格式**：`SPI_SHADER_PGM_RSRC1/2`、`SPI_SHADER_PGM_LO/HI` 寄存器
- [ ] **PM4 draw/dispatch packet 格式**：从 Mesa RADV `radv_cmd_buffer.c` 提取
- [ ] **资源描述符格式**：Buffer/Texture/Sampler descriptor 的 GFX12 布局

**收集方法：**
```bash
# 从 LLVM 源码获取 GFX12 指令定义
# llvm/lib/Target/AMDGPU/GCNRegisterBankInfo.cpp
# llvm/lib/Target/AMDGPU/SIInstructions.td

# 从 Mesa 获取 PM4 命令格式
# src/amd/common/ac_gpu_info.h (GFX12 caps)
# src/amd/common/sid.h (PM4 opcodes)
# src/amd/vulkan/radv_cmd_buffer.c (command encoding)

# 从 Linux amdgpu 获取寄存器定义
# drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_offset.h
# drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h
```

### 3.3 IOAcceleratorFamily 数据

**已有：**
- [x] Cobalt64 的 N48MetalOps ABI（完整的 IOAccelerator 对接层）
- [x] Cobalt64 的 `native_metal_pure.h`（IOAccelConfig 格式、stamp page、task VA window）
- [x] AppleParavirtGPU 的 `populateAccelConfig` 反汇编结果

**需要收集：**
- [ ] **IOAccelerator 类层次结构**：需要实现哪些虚函数
- [ ] **IOAccelConfig 字段含义**：0x90 字节结构的每个字段的作用
- [ ] **MTLDevice 注册流程**：Metal.framework 如何发现和初始化 MTLDevice

**收集方法：**
- 反汇编 `IOAcceleratorFamily2.kext` 的类层次
- 参考 Cobalt64 的 aux kext 实现
- 参考 AppleParavirtGPU（macOS 虚拟化 GPU 驱动，最简单的参考实现）

### 3.4 Metal API 覆盖数据

**需要确定最小可行集合（MVP）：**
- [ ] **MTLDevice**：设备属性、特性查询
- [ ] **MTLCommandQueue / MTLCommandBuffer**：命令提交
- [ ] **MTLRenderCommandEncoder**：基础渲染命令
- [ ] **MTLComputeCommandEncoder**：基础计算命令
- [ ] **MTLBlitCommandEncoder**：基础数据传输
- [ ] **MTLRenderPipelineState**：渲染管线创建（触发 shader 编译）
- [ ] **MTLBuffer / MTLTexture**：资源创建和管理
- [ ] **MTLLibrary / MTLFunction**：从 metallib 加载 shader

---

## 四、分阶段实现方案

### Phase A：AIR → GFX12 编译管道 (核心，6-8 周)

这是整个方案的技术核心。目标：输入 `.metallib`，输出 GFX12 shader 二进制。

```
输入: .metallib (MTLB 容器 + AIR bitcode)
    ↓
[1] metallib 解析器 (1 周)
    - 解析 MTLB header
    - 提取函数表 (NAME/TYPE/HASH/MDSZ/OFFT/VERS tags)
    - 提取 AIR bitcode blob
    ↓
[2] AIR → AMDGPU IR 转换 pass (3-4 周) ← 核心难点
    - air.* intrinsics → AMDGPU 内建函数
    - Metal address space → AMDGPU address space
    - Metal 系统值 (vertex_id 等) → AMDGPU 系统寄存器
    - 纹理/采样器操作映射
    - typed pointers → opaque pointers
    ↓
[3] LLVM AMDGPU 后端编译 (1 周)
    - 调用 LLVM API 编译为 gfx1201 目标
    - 提取 .text section (shader 二进制)
    - 提取资源使用信息 (VGPR/SGPR 数量)
    ↓
[4] Shader 二进制封装 (1 周)
    - 生成 shader program descriptor
    - 填写 SPI_SHADER_PGM_RSRC1/2
    - 准备 shader 加载到 GPU
    ↓
输出: GFX12 shader 二进制 + 元数据
```

**验证里程碑：**
- 编译一个 trivial compute shader (add two buffers) 并在 GPU 上执行
- 编译 HelloTriangle 的 vertex + fragment shader

### Phase B：Metal 框架层 (4-6 周)

在 shader 编译管道工作后，实现 Metal API 接口：

```
[1] MTLDevice Bundle 注册 (1 周)
    - IOAccelerator aux kext (参考 Cobalt64)
    - RDNA4Metal.bundle 框架
    - MTLDevice 属性返回

[2] 命令提交管道 (2-3 周)
    - MTLCommandQueue / MTLCommandBuffer
    - Render/Compute/Blit CommandEncoder
    - PM4 命令流生成 (参考 Mesa RADV + Cobalt64 xlat12)
    - 命令提交到 GPU ring buffer

[3] 管线状态对象 (1-2 周)
    - MTLRenderPipelineState (触发 VS+FS 编译)
    - MTLComputePipelineState (触发 CS 编译)
    - PSO 缓存
```

### Phase C：资源管理 (3-4 周)

```
[1] Buffer (1 周)
    - MTLBuffer 创建/映射/提交
    - StorageMode: shared / private / managed

[2] Texture (2-3 周)
    - MTLTexture 创建
    - 格式映射 (MTLPixelFormat → GFX12 格式)
    - Mipmap / Array / Cube
    - 采样器状态
```

### Phase D：优化和完善 (持续)

```
- Shader 编译缓存 (避免重复编译)
- 更多 Metal API 覆盖
- 性能优化
- 稳定性测试
```

---

## 五、关键风险和缓解

| 风险 | 影响 | 概率 | 缓解措施 |
|------|------|------|---------|
| **AIR intrinsics 映射不完整** | Shader 编译失败 | 中 | 从简单 shader 开始，逐步扩展 |
| **LLVM AMDGPU 后端生成的代码在 GFX12 上不工作** | GPU hang/crash | 低 | LLVM 的 gfx1201 已经过 ROCm/Mesa 验证 |
| **IOAccelerator 对接困难** | Metal 设备无法注册 | 中 | 完全参考 Cobalt64 的已验证实现 |
| **PM4 命令流格式错误** | GPU hang | 中 | 参考 Mesa RADV + Cobalt64 xlat12 的已验证代码 |
| **Metal API 覆盖不足** | 应用兼容性问题 | 高 | MVP 先只支持基础渲染+计算 |
| **macOS 版本更新导致 AIR 格式变化** | 编译管道失效 | 低 | AIR 基于 LLVM IR，苹果很少做破坏性改动 |

---

## 六、与其他方案的对比

| 维度 | 苹果原生 (不可能) | Cobalt64 xlat12 | Mesa RADV 转换 | **我们：LLVM 原生** |
|------|-------------------|----------------|---------------|-------------------|
| **Shader 编译** | 自研 libSC | 复用苹果编译器 | SPIR-V → RADV | AIR → LLVM AMDGPU |
| **利用 GFX12 新指令** | 完整 | ❌ 受限于 gfx10.3 | ✅ | ✅ |
| **独立于苹果驱动** | N/A | ❌ 依赖苹果的 Metal bundle | ✅ | ✅ |
| **编译质量** | 最优 | 好（苹果优化+翻译损耗） | 好（Mesa 优化） | 好（LLVM 优化） |
| **开发难度** | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐⭐ |
| **开发周期** | 12-18 个月 | 已完成 | 2-3 个月 | **4-6 个月** |
| **可维护性** | N/A | 每个 macOS 版本可能需要更新翻译表 | 依赖 Mesa 更新 | 自主可控 |

---

## 七、立即可做的验证实验

### 实验 1：AIR → x86 编译验证（1 小时）

在本机验证 AIR bitcode 可以被 LLVM 处理：

```bash
# 1. 写一个最简单的 Metal compute shader
cat > test.metal << 'EOF'
#include <metal_stdlib>
kernel void add_arrays(device float* a [[buffer(0)]],
                       device float* b [[buffer(1)]],
                       device float* c [[buffer(2)]],
                       uint id [[thread_position_in_grid]]) {
    c[id] = a[id] + b[id];
}
EOF

# 2. 在黑苹果上编译为 AIR 和 LLVM IR
xcrun -sdk macosx metal -S -emit-llvm test.metal -o test.ll
xcrun -sdk macosx metal -c test.metal -o test.air

# 3. 查看 AIR IR 中的 intrinsics
grep "call.*@air\." test.ll

# 4. 尝试用 LLVM AMDGPU 后端编译
# (需要先处理 AIR intrinsics)
```

### 实验 2：LLVM AMDGPU GFX12 代码生成验证（30 分钟）

```bash
# 手写一个等价的标准 LLVM IR，验证 GFX12 代码生成
cat > test_gfx12.ll << 'EOF'
define amdgpu_kernel void @add_arrays(
    float addrspace(1)* %a,
    float addrspace(1)* %b,
    float addrspace(1)* %c) {
entry:
    %id = call i32 @llvm.amdgcn.workitem.id.x()
    %idx = zext i32 %id to i64
    %pa = getelementptr float, float addrspace(1)* %a, i64 %idx
    %pb = getelementptr float, float addrspace(1)* %b, i64 %idx
    %pc = getelementptr float, float addrspace(1)* %c, i64 %idx
    %va = load float, float addrspace(1)* %pa
    %vb = load float, float addrspace(1)* %pb
    %vc = fadd float %va, %vb
    store float %vc, float addrspace(1)* %pc
    ret void
}
declare i32 @llvm.amdgcn.workitem.id.x()
EOF

# 编译为 GFX12 汇编
/opt/homebrew/opt/llvm/bin/llc -march=amdgcn -mcpu=gfx1201 test_gfx12.ll -o test_gfx12.s

# 查看生成的 GFX12 汇编
cat test_gfx12.s
```

### 实验 3：metallib 解析验证（1 小时）

```python
# 写一个 Python 脚本解析 metallib
import struct

def parse_metallib(path):
    with open(path, 'rb') as f:
        magic = f.read(4)
        assert magic == b'MTLB', f"Not a metallib: {magic}"
        # ... 按已知格式解析
```

---

## 八、工具和依赖

### 已有（本机可用）
- [x] LLVM 23.1.4 with AMDGPU backend (`/opt/homebrew/opt/llvm/`)
- [x] gfx1201 target 支持已验证
- [x] Cobalt64 源码（完整的 IOAccelerator 对接参考）
- [x] Mesa RADV 源码（`navi48-native/mesa/`）
- [x] MacKernelSDK
- [x] 黑苹果测试机（Navi 48 GPU）

### 需要准备
- [ ] Xcode metal 工具链（黑苹果上）
- [ ] metal-ir-pipeline 工具（clone 并编译）
- [ ] 一组 Metal shader 测试样例（从 Apple 示例项目提取）
- [ ] libLLVM 开发库（用于在 Bundle 中链接 LLVM）

---

## 九、时间线总结

```
Phase A: AIR → GFX12 编译管道     6-8 周  ← 核心 MVP
Phase B: Metal 框架层              4-6 周
Phase C: 资源管理                  3-4 周
Phase D: 优化完善                  持续

总计到首次三角形渲染: 约 13-18 周 (3-4.5 个月)
总计到基本可用:       约 16-22 周 (4-5.5 个月)
```

---

*最后更新: 2026-10-28*
