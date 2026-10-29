# air2gfx12 工程目录结构

```
air2gfx12/
├── README.md                    # 项目主文档（特性、快速开始、验收状态）
├── USAGE.md                     # 详细使用指南
├── Makefile                     # 统一构建脚本
│
├── src/                         # 核心源码
│   ├── main.cpp                 # CLI 主程序
│   ├── MTLBUnpacker.hpp/cpp     # MTLB 容器解包器
│   ├── AIRLinker.hpp/cpp        # AIR 运行库链接与地址空间映射
│   ├── GFX12Emitter.hpp/cpp     # LLVM AMDGPU 后端代码生成器
│   ├── ShaderMetadata.hpp/cpp   # 寄存器统计与元数据提取
│   └── PM4Builder.hpp/cpp       # GFX12 PM4 命令流封包器
│
├── bin/                         # 编译产物
│   ├── air2gfx12                # 主程序 (139 KB)
│   ├── test_unpacker            # 单元测试：解包器
│   ├── test_linker              # 单元测试：链接器
│   ├── test_emitter             # 单元测试：代码生成
│   ├── test_metadata            # 单元测试：元数据解析
│   └── test_pm4                 # 单元测试：PM4 封包器
│
├── assets/                      # 运行时库资产
│   ├── runtime.amdil.bc         # Apple 原厂 AIR 运行库 (2.7 MB, 29.8万行IR)
│   └── runtime.amdil.builtin    # 内建函数符号表 (12 MB)
│
├── test_assets/                 # 测试用 Metallib
│   └── TextureProcessorMetal.metallib  # 生产级着色器库 (45 个 kernel)
│
├── tests/                       # 测试套件
│   ├── test_unpacker.cpp        # 容器解包单元测试
│   ├── test_linker.cpp          # 运行库链接单元测试
│   ├── test_emitter.cpp         # 代码生成单元测试
│   ├── test_metadata.cpp        # 元数据解析单元测试
│   ├── test_pm4.cpp             # PM4 封包器单元测试
│   ├── test_cli.sh              # CLI 功能测试
│   ├── e2e_test.sh              # 端到端集成测试
│   ├── batch_golden_test.sh    # 批量 45 个着色器压力测试
│   └── phase1_full_verification.sh  # 完整 8/8 验收测试
│
├── scripts/                     # 实用工具脚本
│   ├── batch_compile.sh         # 批量编译 metallib 中所有 kernel
│   └── analyze_stats.sh         # 生成资源占用统计报表 (CSV)
│
└── examples/                    # 示例与教程
    └── end_to_end.sh            # 完整示例：Metal 源码 → GFX12 机器码
```

---

## 快速导航

| 我想... | 看这里 |
|---------|--------|
| 了解项目与快速开始 | [`README.md`](README.md) |
| 学习命令行用法 | [`USAGE.md`](USAGE.md) |
| 运行端到端示例 | [`examples/end_to_end.sh`](examples/end_to_end.sh) |
| 批量编译着色器 | [`scripts/batch_compile.sh`](scripts/batch_compile.sh) |
| 分析资源占用 | [`scripts/analyze_stats.sh`](scripts/analyze_stats.sh) |
| 运行完整验收测试 | `make verify_phase1` |
| 查看 PM4 封包示例 | [`src/PM4Builder.hpp`](src/PM4Builder.hpp) |
| 了解架构与实现 | [`USAGE.md#工作原理`](USAGE.md) |

---

## 编译与测试

```bash
# 编译主程序与测试套件
make all

# 运行全部单元测试
make test

# 运行 Phase 1 完整验收（8/8 检查项）
make verify_phase1

# 批量测试 45 个生产着色器
make test_golden

# 清理构建产物
make clean
```

---

## 使用示例

```bash
# 查看 metallib 包含的着色器
./bin/air2gfx12 input.metallib --list

# 编译单个 Kernel 为汇编与 ELF
./bin/air2gfx12 input.metallib --kernel "MyKernel" \
    -o output.s --emit-obj output.o --stats

# 批量编译所有 Kernel
./scripts/batch_compile.sh input.metallib output_dir/

# 生成统计报表
./scripts/analyze_stats.sh input.metallib stats.csv
```

---

*Copyright © 2026 RDNA4 macOS Driver Project.*
