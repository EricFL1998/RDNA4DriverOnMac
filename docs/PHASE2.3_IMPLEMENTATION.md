# Phase 2.3 实现总结

**日期**: 2026-10-29  
**功能**: 首个 Compute Kernel 提交框架

---

## 已完成的工作

### 1. ComputeRing 类（Ring Buffer 管理）

**文件**: `RDNA4Driver/src/hardware/ComputeRing.hpp/cpp`

**功能**:
- Ring Buffer 分配（1MB，256K dwords）
- Doorbell 管理（通知 GPU 有新命令）
- Fence 同步（等待 GPU 完成）
- PM4 数据包写入（支持 wrap-around）

**关键方法**:
```cpp
bool init(vramBase, vramSize, doorbellBase, doorbellIndex);
bool writePM4(const uint32_t* packet, size_t dwordCount);
void submit();  // Ring doorbell
bool waitForFence(uint64_t fenceValue, uint64_t timeoutMS);
```

### 2. RDNA4Driver 扩展

**新增公共接口**:
```cpp
uint64_t allocateVRAMBuffer(size_t size);
void freeVRAMBuffer(uint64_t gpuAddress);
bool submitComputeCommands(const uint32_t* pm4Data, size_t dwordCount);
bool waitForComputeFence(uint64_t fenceValue, uint64_t timeoutMS);
bool testVectorAdd();  // Phase 2.3 验证测试
```

**生命周期集成**:
- `start()`: 在 GFX 初始化后调用 `initComputeRing()`
- `releaseResources()`: 清理 ComputeRing
- Ring 初始化失败视为非致命错误（Phase 2 可选）

### 3. RDNA4Accel 修复

**继承链**: `IOService` → `IOAccelerator`（正确的 Metal 加速器基类）

**文件**:
- `RDNA4Accel/src/RDNA4Accel.hpp`: 添加 `#include <IOKit/graphics/IOAccelerator.h>`
- `RDNA4Accel/src/RDNA4Accel.cpp`: 更新元类定义

---

## 内存布局

### VRAM 分配策略

```
VRAM Base: 0x8000000000 (GMC virtual address)
├── 0x00000000 - 0x03FFFFFF  (64 MB)  GMC 保留区域
├── 0x04000000 - 0x043FFFFF  (1 MB)   Compute Ring Buffer
├── 0x04400000 - 0x04400007  (8 B)    Fence 位置
└── 0x04400008 - ...                  User buffers（动态分配）
```

### Doorbell 映射

```
BAR2 (Doorbell aperture)
├── Doorbell[0] = Compute Ring 0 (Phase 2.3 使用)
├── Doorbell[1] = Compute Ring 1
└── ...
```

---

## 编译与部署

### 在黑苹果上编译

```bash
cd RDNA4Driver

# 清理旧文件
make clean

# 编译（包含 ComputeRing.cpp）
make

# 卸载旧驱动
sudo kextunload /Library/Extensions/RDNA4Driver.kext

# 安装新驱动
sudo cp -R build/RDNA4Driver.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/RDNA4Driver.kext
sudo kextcache -i /

# 重启以加载新驱动
sudo reboot
```

### 启动参数（保持不变）

```
rdna4=1 rdna4-psp=1 rdna4-smu=1 rdna4-gfx=1
```

---

## 验证步骤

### 1. 检查驱动加载

```bash
kextstat | grep rdna4
```

期望看到：
```
com.rdna4.driver (0.2.0)
com.rdna4.accel
```

### 2. 检查 IORegistry 属性

```bash
ioreg -l -n RDNA4Driver | grep -E "ComputeRing|GFX,Stage"
```

期望看到：
```
"GFX,Stage" = "gfx-alive"
"ComputeRing,Initialized" = Yes
```

### 3. 查看系统日志

```bash
sudo dmesg | grep -i "rdna4\|ComputeRing"
```

期望看到：
```
RDNA4: start() - RDNA4 Driver version 0.2.0
...
RDNA4: GFX,Stage = gfx-alive
ComputeRing: init() - ...
ComputeRing: initialized - ring at VRAM+0x4000000, fence at VRAM+0x4400000
RDNA4: Compute Ring initialized successfully
```

### 4. 手动触发测试（可选）

Phase 2.3 的 `testVectorAdd()` 当前在驱动启动时**不会自动调用**（为了避免影响启动）。

要手动触发，需要：
1. 从用户态通过 IOUserClient 调用
2. 或在驱动中添加 boot-arg `rdna4-test-compute=1`

**未来扩展**：
```cpp
// 在 start() 中添加
uint32_t testCompute = 0;
if (PE_parse_boot_argn("rdna4-test-compute", &testCompute, sizeof(testCompute)) 
    && testCompute != 0) {
    testVectorAdd();
}
```

---

## 当前限制（Phase 2.3）

| 功能 | 状态 | 说明 |
|------|------|------|
| ✅ Ring Buffer 分配 | 完成 | 1MB ring at VRAM+64MB |
| ✅ Doorbell 通知 | 完成 | 写入 write pointer 到 doorbell[0] |
| ✅ Buffer 分配 | 基础实现 | 简单线性分配器（无碎片管理） |
| ✅ PM4 写入 | 完成 | 支持 wrap-around |
| ⚠️ Fence 同步 | 实现但未验证 | GPU 需要写回 fence 值 |
| ❌ Shader 编译 | 未集成 | 需要调用 air2gfx12 |
| ❌ DISPATCH_DIRECT | 未实现 | 需要正确的 PM4 封包 |
| ❌ 结果验证 | 未实现 | 需要读回 VRAM 并比对 |

---

## 下一步（Phase 2.3 完整实现）

### 步骤 1: 集成 air2gfx12 编译器

将 Phase 1 的 `air2gfx12` 编译向量加法 shader：

```bash
cd tools/air2gfx12

# 编译向量加法
cat > /tmp/vectoradd.metal << 'EOF'
#include <metal_stdlib>
using namespace metal;

kernel void vectorAdd(device const float* a [[buffer(0)]],
                      device const float* b [[buffer(1)]],
                      device float* result [[buffer(2)]],
                      uint id [[thread_position_in_grid]])
{
    result[id] = a[id] + b[id];
}
EOF

xcrun -sdk macosx metal -c /tmp/vectoradd.metal -o /tmp/vectoradd.air
xcrun -sdk macosx metallib /tmp/vectoradd.air -o /tmp/vectoradd.metallib

./bin/air2gfx12 /tmp/vectoradd.metallib --kernel "vectorAdd" \
    -o /tmp/vectoradd.s --emit-obj /tmp/vectoradd.o --stats
```

### 步骤 2: 提取机器码并集成到驱动

```cpp
// 从 ELF 提取 .text 段
llvm-objcopy -O binary --only-section=.text /tmp/vectoradd.o /tmp/vectoradd.bin

// 在驱动中硬编码或动态加载
uint8_t vectorAddShader[] = { /* 机器码 */ };
```

### 步骤 3: 构建完整的 PM4 封包

使用 Phase 1 的 `PM4Builder`：

```cpp
#include "../../tools/air2gfx12/src/PM4Builder.hpp"

air2gfx12::PM4Builder pm4;

// 设置 Shader 程序
pm4.setComputeProgram(shaderGpuAddr, rsrc1, rsrc2);

// 设置工作组大小
pm4.setComputeWorkgroupSize(256, 1, 1);

// 设置 Buffer 地址
pm4.setComputeUserData(0, bufferA);  // a
pm4.setComputeUserData(2, bufferB);  // b
pm4.setComputeUserData(4, bufferC);  // result

// 分发计算
pm4.emitDispatchDirect(numElements / 256, 1, 1);

// 刷新缓存
pm4.emitCsPartialFlush();

// 写 fence
pm4.emitFenceWrite(fenceGpuAddr, fenceValue);

// 获取封包
const auto& packets = pm4.getPackets();
submitComputeCommands(packets.data(), packets.size());
```

### 步骤 4: 验证结果

```cpp
// 等待完成
waitForComputeFence(fenceValue, 5000);  // 5 秒超时

// 读回 VRAM
volatile float* vramC = (volatile float*)((uint8_t*)vram + ...);
bool pass = true;
for (size_t i = 0; i < numElements; i++) {
    float expected = (float)i + (float)(i * 2);
    if (vramC[i] != expected) {
        LOG_ERROR("Mismatch at [%zu]: got %f, expected %f", i, vramC[i], expected);
        pass = false;
        break;
    }
}

if (pass) {
    LOG("✅ Vector Add Test PASSED!");
} else {
    LOG_ERROR("❌ Vector Add Test FAILED!");
}
```

---

## 故障排查

### 问题 1: ComputeRing 初始化失败

**日志**: `ComputeRing: init() - vramBase=NULL`

**原因**: VRAM 未映射或 BAR0 映射失败

**解决**: 检查 `mapVRAM()` 是否成功

### 问题 2: Doorbell 写入无响应

**日志**: `ComputeRing: submitted - doorbell[0] = 123`，但 GPU 无反应

**原因**: 
- Doorbell 索引错误
- GFX 引擎未初始化
- Ring buffer 地址配置错误

**解决**: 
- 确认 `GFX,Stage = gfx-alive`
- 检查 doorbell BAR2 映射是否正确

### 问题 3: Fence 永不更新

**日志**: `ComputeRing: fence timeout`

**原因**: 
- GPU 未执行 shader
- Fence 地址未配置
- PM4 命令格式错误

**解决**:
- 确认 PM4 包含 `RELEASE_MEM` 或 fence write
- 检查 fence VRAM 地址是否正确

---

*Phase 2.3 框架完成于 2026-10-29. 下一步: 集成 air2gfx12 与 PM4Builder 完成首个真实 Compute Kernel.*
