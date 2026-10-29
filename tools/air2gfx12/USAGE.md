# air2gfx12 使用指南

**air2gfx12** 是为 AMD RDNA4 (Navi 48 / GFX1201) 架构设计的离线 Metal Shader 编译器，可将 Apple 的 `.metallib` 或 `.air` 直接编译为原生 GFX12 机器码。

---

## 快速开始

### 基本用法

```bash
# 查看 metallib 中包含的所有着色器
./bin/air2gfx12 input.metallib --list

# 编译单个 Kernel 为汇编
./bin/air2gfx12 input.metallib --kernel "MyKernel" -o output.s

# 编译为 ELF 目标文件
./bin/air2gfx12 input.metallib --kernel "MyKernel" --emit-obj output.o

# 同时输出汇编与 ELF，并显示硬件寄存器指标
./bin/air2gfx12 input.metallib --kernel "MyKernel" \
    -o output.s --emit-obj output.o --stats

# 使用自定义运行时库
./bin/air2gfx12 input.metallib --kernel "MyKernel" \
    --runtime /path/to/runtime.amdil.bc -o output.s
```

### 命令行选项

| 选项 | 说明 |
|------|------|
| `<input>` | 输入文件（`.metallib` 或 `.air`） |
| `--list` | 列出所有着色器函数并退出 |
| `--kernel <name>` | 指定要编译的内核名称 |
| `-o <path>` | 输出 GFX12 汇编文件 (`.s`) |
| `--emit-obj <path>` | 输出 ELF 目标文件 (`.o`) |
| `--runtime <path>` | 指定 `runtime.amdil.bc` 路径（默认自动查找） |
| `--stats` | 显示 SGPR/VGPR/Scratch 统计与 GFX12 寄存器值 |
| `-v, --verbose` | 详细日志输出 |
| `-h, --help` | 显示帮助信息 |

---

## 工作原理

### 编译流程

```
.metallib → MTLBUnpacker → AIR Bitcode → AIRLinker → LLVM IR
                                           ↓
                              runtime.amdil.bc (3526个内建函数)
                                           ↓
                         地址空间重映射 (addrspace(2)→(4))
                                           ↓
                         LLVM AMDGPU Backend (GFX1201)
                                           ↓
                              GFX12 Assembly / ELF
                                           ↓
                         ShaderMetadata 解析 → SGPR/VGPR/Scratch
                                           ↓
                         PM4Builder → 硬件命令流 (可选)
```

### 核心组件

1. **MTLBUnpacker**: 解析 Apple Metallib 容器格式，提取 AIR Bitcode
2. **AIRLinker**: 
   - 加载用户 Shader 的 AIR Bitcode
   - 按需链接 `runtime.amdil.bc` 中的内建函数（如 `air.fmax.f32`）
   - 自动重映射地址空间（Metal `addrspace(2)` → AMDGPU `addrspace(4)`）
   - 提升入口函数调用约定为 `amdgpu_kernel`
3. **GFX12Emitter**: 配置 LLVM TargetMachine 生成 `gfx1201` 机器码
4. **ShaderMetadata**: 从 ELF `.note` 段或汇编中提取硬件资源占用
5. **PM4Builder**: （可选）生成 GFX12 PM4 命令流用于驱动提交

---

## 输出格式

### GFX12 汇编 (`.s`)

```asm
	.amdgcn_target "amdgcn-amd-amdhsa-unknown-gfx1201"
	.amdhsa_code_object_version 6
	.text
	.p2align	7
	.type	MyKernel,@function
MyKernel:
	s_wait_loadcnt_dscnt 0x0
	s_wait_kmcnt 0x0
	v_dual_mov_b32 v0, s0 :: v_dual_mov_b32 v1, s1
	s_endpgm
	.amdhsa_kernel MyKernel
		.amdhsa_wavefront_size32 1
		.amdhsa_private_segment_fixed_size 96
		.amdhsa_kernarg_size 304
		.vgpr_count: 78
		.sgpr_count: 73
```

### ELF 目标文件 (`.o`)

标准 64 位 ELF 可重定位文件，包含：
- `.text` 段：GFX12 机器码
- `.note.AMDGPU` 段：AMDHSA 元数据（YAML 格式）
- 可用 `llvm-readelf --notes` 查看完整元数据

### 硬件统计 (`--stats`)

```
[Stats] Kernel: MyKernel | SGPRs: 73 | VGPRs: 78 | Wave: 32 | Scratch: 96B
        | GFX12 RSRC1: 0x10c0253 RSRC2: 0x1381
```

---

## 高级用法

### 批量编译所有 Kernel

```bash
#!/bin/bash
METALLIB="input.metallib"
OUT_DIR="output"
mkdir -p "$OUT_DIR"

# 提取所有 kernel 名称
KERNELS=$(./bin/air2gfx12 "$METALLIB" --list | grep "^\[" | awk '{print $2}')

for KERNEL in $KERNELS; do
    echo "Compiling $KERNEL..."
    ./bin/air2gfx12 "$METALLIB" --kernel "$KERNEL" \
        -o "$OUT_DIR/${KERNEL}.s" \
        --emit-obj "$OUT_DIR/${KERNEL}.o" \
        --stats
done
```

### 与驱动集成

编译出的 `.o` 文件可以：
1. 用 `llvm-objcopy` 提取 `.text` 段作为纯机器码
2. 通过 PM4 命令流将 GPU 代码上传到 VRAM
3. 配置 `COMPUTE_PGM_LO/HI` 寄存器指向代码地址
4. 发出 `DISPATCH_DIRECT` 命令执行

参考 `src/PM4Builder.hpp` 查看命令流封装示例。

---

## 依赖与要求

- **LLVM 23+** (需要 AMDGPU Target 与 GFX12 支持)
- **macOS** 或 **Linux** (已在 macOS 14+ / Ubuntu 22.04+ 验证)
- **Apple Runtime Library**: `runtime.amdil.bc` (从 Metal 开发工具提取)

### 安装 LLVM (macOS)

```bash
brew install llvm
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
```

### 获取 runtime.amdil.bc

从 macOS 系统中提取：
```bash
# 方法 1: 从 Metal 开发工具
find /Applications/Xcode.app -name "runtime.amdil.bc"

# 方法 2: 从黑苹果实机提取
scp user@hackintosh:/path/to/runtime.amdil.bc ./assets/
```

---

## 测试与验证

### 运行测试套件

```bash
# 单元测试
make test

# 完整 Phase 1 验收测试（8/8 检查项）
make verify_phase1

# 批量 45 个生产着色器压力测试
make test_golden
```

### 预期输出

所有测试应显示 `✅ PASS` 或 `ALL TESTS PASSED!`。

---

## 故障排查

### 问题: `Cannot find runtime.amdil.bc`

**解决**: 手动指定路径
```bash
./bin/air2gfx12 input.metallib --runtime /path/to/runtime.amdil.bc ...
```

### 问题: `LLVM ERROR: Cannot select ...`

**原因**: LLVM 后端不支持某些 AIR 内建或地址空间  
**解决**: 
1. 检查 `AIRLinker.cpp` 的地址空间映射是否完整
2. 确认 LLVM 版本 >= 23（GFX12 在 LLVM 18+ 才稳定）

### 问题: 编译成功但寄存器统计为 0

**原因**: 汇编格式解析失败  
**解决**: 使用 `--emit-obj` 输出 ELF，从 ELF `.note` 段读取元数据更可靠

---

## 贡献与支持

本工具是 **RDNA4 macOS Native Metal Driver** 项目的一部分。

- **项目主页**: [GitHub Repo](https://github.com/your-repo)
- **问题反馈**: 提交 GitHub Issue
- **技术文档**: `docs/superpowers/specs/` 目录

---

*Copyright © 2026 RDNA4 macOS Driver Project. Licensed under MIT.*
