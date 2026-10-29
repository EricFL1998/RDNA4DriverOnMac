# Phase 2.3 完整实现 - 首个真实 Compute Kernel

**日期**: 2026-10-29  
**状态**: ✅ 代码完成，等待黑苹果部署与测试

---

## 🎯 实现内容

### 新增文件（8 个）

| 文件 | 功能 | 行数 |
|------|------|------|
| `RDNA4Driver/shaders/vectoradd.metal` | 向量加法 Metal Shader | 24 |
| `RDNA4Driver/compile_shader.sh` | Shader 编译脚本 | 65 |
| `RDNA4Driver/shaders/compiled/vectoradd_gfx12.h` | Shader 头文件（stub） | 50 |
| `RDNA4Driver/src/hardware/PM4BuilderKernel.hpp` | 内核 PM4 构建器接口 | 80 |
| `RDNA4Driver/src/hardware/PM4BuilderKernel.cpp` | 内核 PM4 构建器实现 | 150 |
| `RDNA4Driver/src/core/RDNA4Driver_TestVectorAdd.cpp` | 完整测试实现 | 210 |
| `docs/PHASE2.3_FULL_IMPLEMENTATION.md` | 本文档 | - |
| `deploy_phase2.3_full.sh` | 完整部署脚本 | - |

### 修改文件（3 个）

| 文件 | 修改内容 |
|------|---------|
| `RDNA4Driver/Makefile` | 添加 PM4BuilderKernel.cpp |
| `RDNA4Driver/src/hardware/ComputeRing.hpp` | 添加 getFenceVRAMOffset() |
| `RDNA4Driver/src/core/RDNA4Driver.cpp` | 追加 testVectorAdd() 实现 |

---

## 🚀 部署步骤

### 步骤 1: 在黑苹果上编译真实 Shader

```bash
cd RDNA4Driver

# 编译 Shader（Metal → AIR → GFX12）
./compile_shader.sh

# 检查生成的文件
ls -lh shaders/compiled/
# 期望看到：
#   vectoradd.air
#   vectoradd.metallib
#   vectoradd_gfx12.s       (GFX12 汇编)
#   vectoradd_gfx12.o       (ELF object)
#   vectoradd_gfx12.bin     (Raw 机器码)
#   vectoradd_gfx12.h       (C 数组，包含真实 shader)
```

### 步骤 2: 重新编译驱动

```bash
cd RDNA4Driver

# 清理
make clean

# 编译（包含新的 PM4BuilderKernel 和 testVectorAdd）
make

# 检查编译产物大小（应该比之前大）
ls -lh build/RDNA4Driver.kext/Contents/MacOS/RDNA4Driver
```

### 步骤 3: 部署新驱动

```bash
# 卸载旧驱动
sudo kextunload /Library/Extensions/RDNA4Driver.kext

# 安装新驱动
sudo cp -R build/RDNA4Driver.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/RDNA4Driver.kext
sudo kextcache -i /

# 重启
sudo reboot
```

### 步骤 4: 添加测试触发器

编辑 OpenCore `config.plist`，在 boot-args 中添加：

```
rdna4=1 rdna4-psp=1 rdna4-smu=1 rdna4-gfx=1 rdna4-test-compute=1
```

**注意**: `rdna4-test-compute=1` 会在驱动启动时自动运行 `testVectorAdd()`

### 步骤 5: 重启验证

```bash
sudo reboot
```

---

## ✅ 验证步骤

重启后，按以下顺序检查：

### 1. 检查驱动加载

```bash
kextstat | grep rdna4
```

期望：
```
com.rdna4.driver (0.2.0)
```

### 2. 检查 Shader 编译

```bash
ls -lh RDNA4Driver/shaders/compiled/vectoradd_gfx12.h
```

期望：文件存在且包含真实 shader 数组（不是 stub）

### 3. 查看测试日志

```bash
sudo dmesg | grep -A50 "Phase 2.3: Vector Add Test"
```

**期望看到（成功案例）**：

```
RDNA4: ======================================
RDNA4:   Phase 2.3: Vector Add Test (FULL)
RDNA4: ======================================
RDNA4: Configuration:
RDNA4:   Elements: 1024
RDNA4:   Buffer size: 4096 bytes
RDNA4:   Workgroup size: 256 threads
RDNA4:   Num workgroups: 4
RDNA4: 
RDNA4: Step 1: Allocating VRAM buffers...
RDNA4:   Buffer A (input):  GPU 0x8004400008
RDNA4:   Buffer B (input):  GPU 0x8004401008
RDNA4:   Buffer C (output): GPU 0x8004402008
RDNA4:   Shader buffer:     GPU 0x8004403008
RDNA4: 
RDNA4: Step 2: Initializing input data...
RDNA4:   Input A: [0.0, 1.0, 2.0, ..., 1023.0]
RDNA4:   Input B: [0.0, 2.0, 4.0, ..., 2046.0]
RDNA4:   Output C: [cleared to 0.0]
RDNA4: 
RDNA4: Step 3: Uploading shader to VRAM...
RDNA4:   Shader size: 1234 bytes
RDNA4:   Shader at: GPU 0x8004403008 (VRAM+0x4403008)
RDNA4: 
RDNA4: Step 4: Building PM4 command packet...
RDNA4:   ✅ Set compute program (RSRC1=0x..., RSRC2=0x...)
RDNA4:   ✅ Set workgroup size (256, 1, 1)
RDNA4:   ✅ Set buffer pointers (A, B, C)
RDNA4:   ✅ Emit DISPATCH_DIRECT (4, 1, 1)
RDNA4:   ✅ Emit CS_PARTIAL_FLUSH
RDNA4:   ✅ Emit fence write (value=1, addr=0x8004400000)
RDNA4:   Total PM4 packet size: 45 dwords (180 bytes)
RDNA4: 
RDNA4: Step 5: Submitting PM4 commands to GPU...
ComputeRing: wrote 45 dwords, writePtr now at 45
ComputeRing: submitted - doorbell[0] = 45
RDNA4:   ✅ Commands submitted to Compute Ring
RDNA4: 
RDNA4: Step 6: Waiting for GPU completion...
ComputeRing: waiting for fence 1 (current: 0)
ComputeRing: fence 1 reached (value: 1)
RDNA4:   ✅ Fence reached (GPU completed)
RDNA4: 
RDNA4: Step 7: Verifying results...
RDNA4: 
RDNA4: Sample results:
RDNA4:   C[0] = 0.0 (expected 0.0)
RDNA4:   C[1] = 3.0 (expected 3.0)
RDNA4:   C[10] = 30.0 (expected 30.0)
RDNA4:   C[100] = 300.0 (expected 300.0)
RDNA4:   C[1023] = 3069.0 (expected 3069.0)
RDNA4: 
RDNA4: ======================================
RDNA4:   ✅ Vector Add Test: PASSED!
RDNA4: ======================================
RDNA4:   All 1024 elements computed correctly
RDNA4:   GPU compute execution successful!
RDNA4: ======================================
```

### 4. 如果测试失败

**使用 stub shader 的情况**：

```
RDNA4:   ⚠️  Using STUB shader (run compile_shader.sh for real shader)
...
RDNA4:   ❌ Vector Add Test: FAILED!
RDNA4:   1024 errors found (out of 1024 elements)
```

**解决**: 确保 `compile_shader.sh` 已运行，且 `vectoradd_gfx12.h` 不是 stub

**Fence 超时的情况**：

```
RDNA4: Step 6: Waiting for GPU completion...
ComputeRing: fence timeout (wanted 1, got 0)
RDNA4 ERROR: testVectorAdd: fence timeout (GPU may not have executed)
```

**可能原因**：
- GFX 引擎未正确初始化
- PM4 packet 格式错误
- Doorbell 未触发 GPU
- Shader 地址配置错误

**排查**：
```bash
# 检查 GFX 状态
ioreg -l -n RDNA4Driver | grep "GFX,Stage"
# 必须是 "gfx-alive"

# 检查 ComputeRing
ioreg -l -n RDNA4Driver | grep ComputeRing
# 必须是 "ComputeRing,Initialized" = Yes
```

---

## 📊 技术细节

### PM4 命令流结构

```
1. SET_SH_REG (COMPUTE_PGM_LO/HI, RSRC1/RSRC2)
   - 设置 shader 地址和资源配置
   
2. SET_SH_REG (COMPUTE_NUM_THREAD_X/Y/Z)
   - 设置 workgroup 大小 (256, 1, 1)
   
3. SET_SH_REG (COMPUTE_USER_DATA_0/2/4)
   - 设置 buffer A/B/C 地址
   
4. DISPATCH_DIRECT (gridX=4, gridY=1, gridZ=1)
   - 启动 1024 个线程（4 个 workgroup × 256 线程）
   
5. EVENT_WRITE (CS_PARTIAL_FLUSH)
   - 刷新 compute shader 写入
   
6. RELEASE_MEM (fence 地址, fence 值)
   - 写入 fence 表示完成
```

### VRAM 布局

```
VRAM Base: 0x8000000000
├── 0x00000000 - 0x03FFFFFF   64 MB     GMC 保留
├── 0x04000000 - 0x043FFFFF    1 MB     Compute Ring
├── 0x04400000 - 0x04400007    8 B      Fence
├── 0x04400008 - 0x04401007    4 KB     Buffer A (1024 floats)
├── 0x04401008 - 0x04402007    4 KB     Buffer B (1024 floats)
├── 0x04402008 - 0x04403007    4 KB     Buffer C (1024 floats)
└── 0x04403008 - 0x04407007   16 KB     Shader code
```

### Shader 计算流程

```
1. GPU 从 Compute Ring 读取 PM4 命令
2. 加载 Shader 代码到 Instruction Cache
3. 配置 SGPR (buffer 地址) 和 VGPR
4. 启动 4 个 Wave (每个 Wave 256 线程)
5. 每个线程:
   - 读取 thread_position_in_grid (0-1023)
   - 从 buffer A/B 读取 float
   - 执行加法
   - 写入 buffer C
6. 所有 Wave 完成后写入 Fence
7. CPU 检测到 Fence 更新
```

---

## 🐛 故障排查

### 问题 1: compile_shader.sh 失败

**错误**: `xcrun: error: unable to find utility "metal"`

**原因**: Xcode Command Line Tools 未安装

**解决**:
```bash
xcode-select --install
```

### 问题 2: air2gfx12 not found

**错误**: `../../tools/air2gfx12/bin/air2gfx12: No such file or directory`

**原因**: air2gfx12 未编译

**解决**:
```bash
cd ../../tools/air2gfx12
make
```

### 问题 3: 编译驱动时找不到 PM4BuilderKernel

**错误**: `fatal error: 'PM4BuilderKernel.hpp' file not found`

**原因**: Makefile 未更新

**解决**: 确认 Makefile 中包含 `PM4BuilderKernel.cpp`

### 问题 4: Fence 永不更新

**症状**: `ComputeRing: fence timeout (wanted 1, got 0)`

**排查步骤**:

1. 检查 GFX 状态：
   ```bash
   ioreg -l -n RDNA4Driver | grep "GFX,Stage"
   ```
   必须是 `"gfx-alive"`

2. 检查 doorbell 映射：
   ```bash
   ioreg -l -n RDNA4Driver | grep "NBIO,DoorbellBar"
   ```
   必须有有效地址

3. 检查 PM4 packet：
   在 `submitComputeCommands` 中添加日志打印前 10 个 dword

4. 检查 shader 地址对齐：
   Shader 地址必须 256 字节对齐

### 问题 5: 结果全是 0

**症状**: 所有 C[i] = 0.0，fence 正常

**可能原因**:
- Shader 未执行（可能是 stub）
- Buffer 地址配置错误
- User data SGPR 配置错误

**排查**:
```bash
# 确认不是 stub
grep "USE_STUB_SHADER" RDNA4Driver/shaders/compiled/vectoradd_gfx12.h
# 应该没有输出（或在 #ifdef 后）

# 确认 shader 有内容
xxd RDNA4Driver/shaders/compiled/vectoradd_gfx12.bin | head
```

---

## 🎉 成功标准

当以下所有条件满足，Phase 2.3 完整实现成功：

- ✅ `compile_shader.sh` 生成真实 GFX12 shader
- ✅ 驱动编译成功，包含 PM4BuilderKernel
- ✅ 驱动加载无错误
- ✅ testVectorAdd() 自动运行
- ✅ 所有 1024 个元素计算正确
- ✅ Fence 正常更新
- ✅ 日志显示 "✅ Vector Add Test: PASSED!"

达成后，标志着：
- 🎯 **首个真实 Compute Kernel 在 RDNA4 GPU 上成功执行**
- 🎯 **完整的 Metal → GFX12 → GPU 软硬件栈打通**
- 🎯 **可以开始 Phase 3: Metal Bundle 集成**

---

*Phase 2.3 完整实现文档创建于 2026-10-29*
