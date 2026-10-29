# RDNA4 macOS 原生图形驱动工程

[![Status](https://img.shields.io/badge/Status-Active_Bringup-success.svg)]()
[![Hardware](https://img.shields.io/badge/GPU-Radeon_RX_9070_XT-red.svg)]()
[![Target](https://img.shields.io/badge/Architecture-GFX1201_(Navi_48)-blue.svg)]()
[![macOS](https://img.shields.io/badge/macOS-Tahoe_26.x-brightgreen.svg)]()

为 macOS 提供 AMD RDNA4 (Navi 48 / GFX1201) 架构的生产级图形与计算驱动体系，支持原生 Metal 加速与 DCN 4.1 高清显示。

---

## 🚀 项目最新实测进展 (2026-10-28)

*   **硬件驱动层 (Kernel-Space)**:
    *   ✅ **设备与 BAR 映射**: 成功识别 AMD Radeon RX 9070 XT (`1002:7550 rev c0`)，BAR0 (256MB VRAM) / BAR2 (2MB Doorbell) / BAR5 (512KB MMIO) 映射正常。
    *   ✅ **IP Discovery**: 从 on-die TMR 成功读取并解析出 **46 个硬件 IP 模块**。
    *   ✅ **系统固件初始化**: PSP 固件加载成功 (`PSP,Stage = alive`, `SOSSignOfLife = 0x0680f469`)；SMU 电源与时钟就绪 (`SMU,Stage = alive`)；GFX 成功进入 `imu-loaded` 阶段。
*   **显示输出层 (Framebuffer)**:
    *   ✅ **物理显示器点亮**: `RDNA4FBMinimal` 驱动物理屏幕输出 **2560 x 1440 @ 32位色深**，`AppleDisplay` 正常挂载。
*   **Metal 工具链与原生编译器 (User-Space)**:
    *   ✅ **`air2gfx12` 离线编译器闭环**: 成功实现直接从 `.metallib` / `.air` 生成原生 `gfx1201` 机器码。
    *   ✅ **原厂运行库完全解析**: 成功提取并链接原厂 `runtime.amdil.bc`（包含 3526 个 `air.*` 内建实现）。
    *   ✅ **批量真实着色器验证**: **45/45 个生产级 Metal Shader 100% 编译通过**（平均单着色器仅耗时 0.2 秒）。
    *   ✅ **GFX12 PM4 封包器**: 实现了 Type-3 封包器，支持自动计算硬件寄存器 `COMPUTE_PGM_RSRC1/2`。

---

## 📂 项目工程结构

```
hackintosh/
├── RDNA4Driver/               # 核心主驱动工程 (IOKit C++, PCIe/PSP/SMU/GFX/IPDiscovery)
├── RDNA4Framebuffer/          # 核心显示驱动工程 (DCN 4.1 Framebuffer)
├── tools/
│   ├── air2gfx12/             # 🆕 自研原生 Metal Shader 离线编译器与验证套件
│   │   ├── bin/               # 编译器主程序 (air2gfx12) 与单元测试二进制
│   │   ├── src/               # MTLBUnpacker, AIRLinker, GFX12Emitter, PM4Builder
│   │   ├── assets/            # 苹果原厂运行库 (runtime.amdil.bc 29.8万行IR)
│   │   ├── test_assets/       # 生产级 Metallib 测试资产 (45+ 真实着色器)
│   │   └── tests/             # 单元测试与 45 个黄金着色器批量验收测试脚本
│   └── ...                    # 调试辅助与恢复脚本
├── docs/                      # 技术文档与规格说明
│   └── superpowers/specs/     # 包含超详细技术逆向报告与计划
├── ROADMAP_NATIVE_METAL.md    # 🆕 官方四阶段技术路线图
├── dist/                      # 部署包与构建产物
├── navi48/                    # 上游参考代码库 (Cobalt64, MacKernelSDK)
├── navi48-native/             # 底层图形库源码 (Mesa RADV, metal2vulkan)
├── ssh-keys/                  # 调试机 SSH 访问凭证
├── tests/                     # 自动化测试脚本
└── archive/                   # 📦 历史文档与旧测试归档 (保持根目录整洁)
```

---

## 🗺️ 官方路线图速览

详细路线图请参阅 👉 **[ROADMAP_NATIVE_METAL.md](ROADMAP_NATIVE_METAL.md)**

```
[ Phase 1: 离线工具链验证 (已完成 ✅) ] ➔ [ Phase 2: 内核加速桥接 (下一步) ]
                 │                                        │
                 ▼                                        ▼
[ Phase 3: 动态运行时与 Metal Bundle ]   ➔ [ Phase 4: 生产级优化与生态兼容 ]
```

---

## 🛠️ 快速上手与验证

### 1. 本地验证 Metal 原生编译器 (无需真实显卡)
```bash
cd tools/air2gfx12

# 运行 Phase 1 全套自动化验收测试 (8/8 绿灯)
make verify_phase1

# 将任意 Metal 库编译为 GFX12 汇编并查看硬件寄存器指标
bin/air2gfx12 test_assets/TextureProcessorMetal.metallib \
    --kernel "MinMaxReduction_float1" \
    -o /tmp/k1.s --stats

# 批量编译所有着色器
scripts/batch_compile.sh test_assets/TextureProcessorMetal.metallib /tmp/output/

# 生成资源占用统计报表
scripts/analyze_stats.sh test_assets/TextureProcessorMetal.metallib /tmp/stats.csv
```

**详细文档**: [`tools/air2gfx12/README.md`](tools/air2gfx12/README.md) | [`tools/air2gfx12/USAGE.md`](tools/air2gfx12/USAGE.md)

### 2. 编译内核驱动 (实验性，需要黑苹果实机)
```bash
cd RDNA4Driver
make clean && make
```

**恢复指南**: 如果驱动导致启动故障，请参阅 [`docs/RECOVERY.md`](docs/RECOVERY.md)

---

## 📚 核心文档导航

*   **官方技术路线图**: [`ROADMAP_NATIVE_METAL.md`](ROADMAP_NATIVE_METAL.md)
*   **air2gfx12 编译器主文档**: [`tools/air2gfx12/README.md`](tools/air2gfx12/README.md)
*   **air2gfx12 使用指南**: [`tools/air2gfx12/USAGE.md`](tools/air2gfx12/USAGE.md)
*   **air2gfx12 工程结构**: [`tools/air2gfx12/STRUCTURE.md`](tools/air2gfx12/STRUCTURE.md)
*   **驱动故障恢复指南**: [`docs/RECOVERY.md`](docs/RECOVERY.md)
*   **超详细逆向预研报告**: [`docs/superpowers/specs/2026-10-28-rdna4-native-metal-comprehensive-report.md`](docs/superpowers/specs/2026-10-28-rdna4-native-metal-comprehensive-report.md)
*   **离线编译器实施计划**: [`docs/superpowers/plans/2026-10-28-air2gfx12-offline-compiler.md`](docs/superpowers/plans/2026-10-28-air2gfx12-offline-compiler.md)
*   **历史文档归档目录**: `archive/old-docs/`

---

*Copyright © 2026 RDNA4 macOS Driver Project.*
