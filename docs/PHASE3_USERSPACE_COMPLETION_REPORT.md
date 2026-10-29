# Phase 3 用户态部分完成总结

**日期**: 2026-10-29  
**阶段**: Phase 3 - 动态运行时与 Metal Bundle（用户态部分）  
**状态**: ✅ **可独立编译的骨架已完成**

---

## 一、交付物清单

### 3.1 AIRNT Plugin（编译器插件） - 已完成

**位置**: `navi48-native/airnt-plugin/`

| 组件 | 文件 | 功能 | 状态 |
|------|------|------|------|
| C API 接口 | `include/airnt_plugin.h` | 标准 AIRNT 接口定义 | ✅ 完成 |
| 核心实现 | `src/airnt_plugin.cpp` | 封装 air2gfx12 为动态库 | ✅ 完成 |
| 构建系统 | `Makefile` | 生成 `libAMDNTPlugin.dylib` | ✅ 完成 |
| 文档 | `README.md` | API 使用指南与示例 | ✅ 完成 |
| 单元测试 | `tests/test_airnt_plugin.cpp` | 基础 API 测试 | ✅ 完成 |

**核心功能**:
- ✅ `AIRNTInit/Destroy` - 插件生命周期管理
- ✅ `AIRNTCompile` - AIR bitcode → GFX12 机器码编译
- ✅ `AIRNTGetMachineCode` - 获取编译后的 ELF 二进制
- ✅ `AIRNTGetMetadata` - 提取 SGPR/VGPR/Scratch/RSRC1/RSRC2
- ✅ `AIRNTGetAssembly` - 获取 GFX12 汇编（调试用）
- 🔜 `AIRNTCompilePipeline` - FlatBuffers 管线编译（Phase 3.3 实现）

### 3.2 Metal Bundle 骨架 - 已完成

**位置**: `navi48-native/metal-bundle/`

| 组件 | 文件 | 功能 | 状态 |
|------|------|------|------|
| 设备接口 | `src/RDNA4Device.h` | MTLDevice 协议声明 | ✅ 完成 |
| 设备实现 | `src/RDNA4Device.m` | 骨架实现（stub） | ✅ 完成 |
| Bundle 配置 | `Info.plist` | Bundle 元信息 | ✅ 完成 |
| 构建系统 | `Makefile` | 生成 `RDNA4Metal.bundle` | ✅ 完成 |
| 文档 | `README.md` | 架构说明与部署指南 | ✅ 完成 |

**已实现的类**（骨架状态）:

| Objective-C 类 | 对应 Metal 协议 | 功能 | 状态 |
|---------------|----------------|------|------|
| `RDNA4Device` | `MTLDevice` | 设备属性与资源创建 | ✅ 骨架 |
| `RDNA4CommandQueue` | `MTLCommandQueue` | 命令队列 | ✅ 骨架 |
| `RDNA4CommandBuffer` | `MTLCommandBuffer` | 命令缓冲 | ✅ 骨架 |
| `RDNA4Buffer` | `MTLBuffer` | Buffer 资源（Host 内存） | ✅ 骨架 |
| `RDNA4Texture` | `MTLTexture` | 纹理资源 | 🔜 Phase 3.4 |
| `RDNA4Library` | `MTLLibrary` | Metallib 加载 | 🔜 Phase 3.3 |
| `RDNA4Function` | `MTLFunction` | Shader 函数 | 🔜 Phase 3.3 |
| `RDNA4ComputePipelineState` | `MTLComputePipelineState` | Compute Pipeline | 🔜 Phase 3.3 |
| `RDNA4RenderPipelineState` | `MTLRenderPipelineState` | Render Pipeline | 🔜 Phase 3.4 |
| `RDNA4ComputeCommandEncoder` | `MTLComputeCommandEncoder` | Compute 编码器 | 🔜 Phase 3.3 |

**核心特性**:
- ✅ 设备属性正确返回（16GB VRAM, 1024³ 最大线程组, Wave32）
- ✅ 命令队列与命令缓冲创建
- ✅ Buffer 资源创建（临时使用 host malloc）
- ✅ 特性集查询（`MTLFeatureSet_macOS_GPUFamily2_v1`）
- 🔜 实际 GPU 通信（需要 Phase 3.3 与内核驱动联调）

---

## 二、技术架构

### AIRNT Plugin 调用流程

```
Metal.framework
    ↓ 调用
libAMDNTPlugin.dylib
    ↓ 使用
air2gfx12 核心组件
    ├── MTLBUnpacker  (解包 .metallib)
    ├── AIRLinker     (链接 runtime.amdil.bc)
    ├── GFX12Emitter  (生成 GFX1201 机器码)
    └── ShaderMetadata (提取寄存器占用)
    ↓ 输出
GFX12 ELF 二进制 + 元数据
```

### Metal Bundle 加载流程

```
系统启动时：
IOKit 发现 AMD 1002:7550 设备
    ↓
Metal.framework 扫描 /Library/GPUBundles/
    ↓
加载 RDNA4Metal.bundle
    ↓ dlopen()
加载 RDNA4Device 类
    ↓
Metal.framework 调用 [RDNA4Device alloc] init]
    ↓
应用调用 MTLCreateSystemDefaultDevice()
    ↓ 返回
RDNA4Device 实例
```

---

## 三、编译与测试

### 编译 AIRNT Plugin

```bash
cd navi48-native/airnt-plugin
make
# 生成: libAMDNTPlugin.dylib
```

### 编译 Metal Bundle

```bash
cd navi48-native/metal-bundle
make
# 生成: build/RDNA4Metal.bundle/
```

### 部署（开发环境）

```bash
cd navi48-native/metal-bundle
make install_dev
# 安装到: ~/Library/GPUBundles/RDNA4Metal.bundle
```

---

## 四、当前限制（骨架阶段）

| 功能 | Phase 3.2 状态 | Phase 3.3 目标 |
|------|---------------|---------------|
| Buffer 分配 | Host malloc（临时） | GPU VRAM 分配 |
| Shader 编译 | 返回 nil | 调用 AIRNT plugin 编译 |
| Command 提交 | 立即标记完成（stub） | 提交 PM4 到 GPU Ring |
| 计算结果读回 | 无操作 | GPU→Host DMA 传输 |
| Fence 同步 | 无操作 | 硬件 Fence 等待 |

---

## 五、下一步（Phase 3.3，需要内核驱动）

Phase 3.3 将实现首个真实的 Compute Kernel 硬件执行：

1. **`RDNA4Library` 实现**
   - 加载 `.metallib` 并提取 AIR bitcode
   - 解析函数列表与元数据

2. **`RDNA4ComputePipelineState` 实现**
   - 调用 AIRNT plugin 编译 AIR → GFX12
   - 上传机器码到 GPU VRAM
   - 配置 `COMPUTE_PGM_RSRC1/2`

3. **`RDNA4ComputeCommandEncoder` 实现**
   - 生成 PM4 命令流（使用 Phase 1 的 `PM4Builder`）
   - 配置 Buffer 地址、线程组大小
   - 编码 `DISPATCH_DIRECT`

4. **`RDNA4CommandBuffer.commit` 实现**
   - 通过内核驱动提交 PM4 到 GPU Compute Ring
   - 等待 Fence 同步
   - 从 VRAM 读回结果

5. **验证目标**
   - 向量加法 Kernel: `result[i] = a[i] + b[i]`
   - 输入: 两个 1024 元素浮点数组
   - 验证: 所有元素计算正确

---

## 六、文件树

```
navi48-native/
├── airnt-plugin/
│   ├── include/
│   │   └── airnt_plugin.h           # C API 接口定义
│   ├── src/
│   │   └── airnt_plugin.cpp         # 插件实现
│   ├── tests/
│   │   └── test_airnt_plugin.cpp    # 单元测试
│   ├── Makefile                     # 构建脚本
│   ├── README.md                    # API 文档
│   └── libAMDNTPlugin.dylib         # 编译产物
│
└── metal-bundle/
    ├── src/
    │   ├── RDNA4Device.h            # Objective-C 接口声明
    │   └── RDNA4Device.m            # 骨架实现
    ├── Info.plist                   # Bundle 配置
    ├── Makefile                     # 构建脚本
    ├── README.md                    # 架构文档
    └── build/
        └── RDNA4Metal.bundle/       # 编译产物
            ├── Contents/
            │   ├── Info.plist
            │   ├── MacOS/
            │   │   ├── RDNA4Metal
            │   │   └── libAMDNTPlugin.dylib
            │   └── Resources/
```

---

## 七、与 Phase 1 的关系

Phase 3 直接复用 Phase 1 的所有成果：

- **AIRNT Plugin** 内部使用 `air2gfx12` 的 5 个核心模块
- **PM4Builder** 将在 Phase 3.3 用于生成命令流
- **ShaderMetadata** 提供寄存器配置给驱动

Phase 1 → Phase 3 → Phase 2（内核驱动）形成完整的用户态→内核态闭环。

---

*Copyright © 2026 RDNA4 macOS Driver Project. Phase 3 用户态部分交付于 2026-10-29.*
