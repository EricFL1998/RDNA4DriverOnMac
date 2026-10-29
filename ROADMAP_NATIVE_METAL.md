# RDNA4 (Navi 48) 原生 Metal 全局技术路线图 (Roadmap)

**更新日期**: 2026-10-28  
**目标硬件**: AMD Radeon RX 9070 XT (Navi 48, GFX1201, Device ID 0x7550 / 0x7745)  
**目标系统**: macOS Tahoe 26.x (x86_64)  
**核心理念**: **用户态优先，离线验证闭环，再桥接内核**。

---

## 路线图总览

```
[ Phase 1: 离线工具链验证 (本地 Mac) ] ➔ [ Phase 2: 内核加速桥接 (黑苹果实机) ]
                 │                                        │
                 ▼                                        ▼
[ Phase 3: 动态运行时与 Metal Bundle ]   ➔ [ Phase 4: 生产级优化与生态兼容 ]
```

---

## 阶段规划详述

### Phase 1: 离线编译器与工具链 ✅ **已完成**

> **目标**: 在完全不依赖黑苹果内核驱动的情况下，在本地 Mac 上建立从 `.metallib` 到 `GFX12 ISA` 的完整编译验证闭环。

*   [x] **1.1 `air2gfx12` 离线编译器 CLI 工具** ✅
    *   完整实现：MTLB 解包器、AIR 链接器、GFX12 代码生成器、元数据提取器（1068 行 C++）。
    *   验收：5/5 单元测试通过 + 45/45 生产着色器通过 + 8/8 完整验收通过。
*   [x] **1.2 批量 Shader 黄金测试向量库** ✅
    *   从黑苹果实机提取 `TextureProcessorMetal.metallib`（45 个真实 kernel）。
    *   批量编译平均耗时：0.2 秒/着色器，成功率 100%，总耗时 9 秒。
*   [x] **1.3 运行时库自动定位与元数据统计** ✅
    *   自动定位 `runtime.amdil.bc`（29.8万行 IR，3526 个内建函数），按需链接。
    *   精确提取 SGPR/VGPR/Scratch 并自动计算 GFX12 `COMPUTE_PGM_RSRC1/2`。
*   [x] **1.4 PM4 命令流生成器 (`PM4Builder`) 与单元测试** ✅
    *   封装 Type-3 封包（`SET_SH_REG`, `DISPATCH_DIRECT`, `EVENT_WRITE`）。
    *   单元测试验证 header 编码与网格配置正确性。

**交付物**: `tools/air2gfx12/` - 完整的离线编译器工具链，包含主程序、测试套件、文档与实用脚本。

---

### Phase 2: 内核加速与 DisplayPipe 桥接（黑苹果实机执行）

> **目标**: 解决 `WindowServer` 报错 `Software compositor activated`（CPU 软解降级）问题，向系统成功注册 `IOAccelerator` 并建立硬件渲染管道。

*   [ ] **2.1 RDNA4Driver 发布 Navi48MetalNub 节点**
    *   在主驱动启动完成后，向 IORegistry 发布轻量级软件子节点 `Navi48MetalNub`。
    *   挂接 `callPlatformFunction("n48.metal.ops")` 接口。
*   [ ] **2.2 部署精简版 Navi48Accel.kext (AuxKC)**
    *   利用已有的 `IOAccelFamily2_decl.h` 虚表头文件编译辅助驱动。
    *   仅匹配 `Navi48MetalNub` 类（完全隔离物理 PCI 寄存器，杜绝官方驱动冲突）。
    *   向系统注册为标准的 `IOAccelerator` 服务，提供 `IOAccelDisplayPipe`。
*   [ ] **2.3 验证 WindowServer 退出软解**
    *   系统识别 `gpu 0x0ae3 accel != 0x0`。
    *   验证 `WindowServer` 硬件图形合成器激活。

---

### Phase 3: 自研原生 Metal Bundle 与动态编译 ⚡ **用户态部分已完成**

> **目标**: 将 Phase 1 的离线编译器封装为动态库，注入系统 Metal 框架，实现首个 Compute Kernel 和 3D 三角形在真实 RX 9070 XT 硬件上的执行。

*   [x] **3.1 封装 `libAMDNTPlugin.dylib` (AIRNT 接口)** ✅
    *   将 `air2gfx12` 核心封装为动态库，导出标准 C API。
    *   实现 `AIRNTInit`, `AIRNTCompile`, `AIRNTGetMachineCode`, `AIRNTGetMetadata`。
    *   完整文档、Makefile、单元测试框架。
*   [x] **3.2 构建 `RDNA4Metal.bundle` 骨架** ✅ **（骨架完成）**
    *   实现 Objective-C 接口：`RDNA4Device : NSObject <MTLDevice>`。
    *   实现 `newCommandQueue`, `newBufferWithLength` 等核心方法（骨架状态）。
    *   Bundle 结构、Info.plist 配置完成，可部署到 `/Library/GPUBundles/`。
*   [ ] **3.3 首个 Compute Kernel 硬件执行验证** 🔜 **（需要内核驱动）**
    *   实现 `RDNA4Library` 加载 `.metallib` 并调用 AIRNT plugin 编译。
    *   实现 `RDNA4ComputeCommandEncoder` 生成 PM4 命令流。
    *   通过内核驱动提交到 GPU Compute Ring，验证向量加法结果。
*   [ ] **3.4 首个 3D 三角形渲染** 🔜 **（需要内核驱动 + Phase 3.3 完成）**
    *   实现 `newRenderPipelineStateWithDescriptor`。
    *   配置 Vertex Fetch 与 Fragment Color Export (`EXP` 硬件指令)。
    *   在 2560x1440 帧缓冲上画出首个纯色三角形。

**交付物**: `navi48-native/airnt-plugin/` + `navi48-native/metal-bundle/` - 完整的用户态 Metal 运行时框架。

---

### Phase 4: 生产级优化与生态兼容（长期迭代）

> **目标**: 达到生产级驱动的稳定性与流畅度，支持主流图形应用、游戏与 Metal 计算。

*   [ ] **4.1 Shader 二级磁盘缓存 (PSO Cache)**
    *   对已编译的 GFX12 机器码计算 SHA-256 哈希。
    *   实现磁盘与内存双层缓存，消除游戏与应用启动时的着色器卡顿。
*   [ ] **4.2 完整纹理管线支持**
    *   支持 2D/3D/Cube/Array 纹理与 Mipmap 链。
    *   支持 BC1-BC7 硬件纹理压缩格式。
    *   支持 Hardware Anisotropic Sampler 过滤。
*   [ ] **4.3 性能调优与硬件特性激活**
    *   利用 RDNA4 的双发射与细粒度 `s_delay_alu` 调度。
    *   优化 Wavefront 占有率（Occupancy）与 VGPR/SGPR 分配。
    *   通过 Metal HUD 与 Geekbench Metal 跑分测试。

---

*路线图确立，立即进入 Phase 1.1：`air2gfx12` 离线编译器实施。*
