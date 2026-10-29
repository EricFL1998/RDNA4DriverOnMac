# RDNA4 Driver Phase 2 规划：GPU 初始化和 GC 通电

## 目标

让 GC (Graphics Core) 通电并响应，验证 RLC 自动加载完成，读取到非零的 GC 寄存器值。

## 核心问题

Phase 1 完成后，GC 寄存器读到 0，原因是：
1. **GC 处于 power-gated 状态** - 需要 SMU 启动电源
2. **EnableAllSmuFeatures 失败** - 需要完整的 SMU hw_setup
3. **TLB invalidate 引擎不响应** - 可能需要额外初始化

## Phase 2 路线

### 2.1 SMU 完整初始化

**目标：** 实现参考实现的 `smu_smc_hw_setup` 完整流程。

**关键步骤：**

1. **Driver Table 配置**
   - 在 VRAM 分配 driver table 空间（参考实现用 4KB）
   - 通过 `SetDriverDramAddrHigh` 和 `SetDriverDramAddrLow` 告知 SMU
   - 这是 SMU 和驱动共享状态的内存区域

2. **Feature Mask 配置**
   - 调用 `SetAllowedFeaturesMaskLow` 和 `SetAllowedFeaturesMaskHigh`
   - 告诉 SMU 哪些 features 应该启用（GFX、UVD、VCE、DPM 等）
   - 参考实现使用 0xFFFFFFFF（允许所有）

3. **Power Table 初始化**
   - 调用 `UseDefaultPPTable` 让 SMU 加载默认的 power/performance 表
   - 这配置了电压、频率、风扇曲线等

4. **DcBtc 运行**
   - 调用 `RunDcBtc` (Display Configuration BTC)
   - 这是 SMU 的显示相关初始化

5. **Power Source 通知**
   - 调用 `NotifyPowerSource` 告知 SMU 当前电源类型（AC/DC）
   - 参数 0 = AC power

6. **启用所有 Features**
   - 最后调用 `EnableAllSmuFeatures`
   - 这会启动 GC、Memory Controller、Display 等所有子系统

**预期结果：**
- EnableAllSmuFeatures 成功返回
- GC 开始通电
- 时钟开始运行

**实现文件：** `RDNA4Driver/src/hardware/SMUSetup.cpp`

**SMU 消息常量：**
```cpp
constexpr uint32_t SetAllowedFeaturesMaskLow  = 0x04;
constexpr uint32_t SetAllowedFeaturesMaskHigh = 0x05;
constexpr uint32_t SetDriverDramAddrHigh      = 0x0A;
constexpr uint32_t SetDriverDramAddrLow       = 0x0B;
constexpr uint32_t UseDefaultPPTable          = 0x13;
constexpr uint32_t NotifyPowerSource          = 0x1A;
constexpr uint32_t RunDcBtc                   = 0x1D;
```

---

### 2.2 IH (Interrupt Handler) 初始化

**目标：** 初始化中断环，让 GPU 能处理中断。

参考实现在 GMC 之前调用 IHInit。虽然我们的轮询方式不需要中断，但 IH 初始化可能是某些硬件唤醒的前提。

**关键步骤：**
1. 分配 IH ring buffer（在 VRAM 或系统内存）
2. 配置 IH 寄存器（ring base、size、write pointer）
3. 可选：注册 MSI 中断处理（macOS 用 `registerInterrupt`）

**简化实现：**
- 只配置 ring buffer 基址和大小
- 不启用 CPU 中断（polling mode）
- 这样 GPU 内部仍能产生中断信号，但不会打扰 CPU

**实现文件：** `RDNA4Driver/src/hardware/IHInit.cpp`

---

### 2.3 Doorbell 路径初始化

**目标：** 配置 NBIO doorbell 路径，让 CP/SDMA 能通过 doorbell 接收命令。

参考实现在 IHInit 阶段调用 `ensure_doorbell_path`（实际是 NBIO doorbell init）。

**关键步骤：**
1. 配置 NBIO doorbell range 寄存器
2. 映射 BAR2（doorbell BAR）到虚拟地址空间
3. 告诉 NBIO doorbell 的物理地址范围

**NBIO 寄存器：**
- `BIF_DOORBELL_CNTL` - doorbell 控制
- Doorbell range 寄存器（per-engine）

**实现文件：** `RDNA4Driver/src/hardware/NBIODoorbell.cpp`

---

### 2.4 验证 GC 状态

完成上述步骤后，再次读取 GC 寄存器：

**验证点：**
1. **GRBM_STATUS (0x0DA4)** - 应读到非零值，显示各引擎状态
2. **RLC_BOOTLOAD_STATUS (0x4E7C)** - bit 31 应为 1，表示 RLC 自动加载完成
3. **TLB invalidate** - 应能正常响应（不再超时）

**成功标准：**
- `GFX,GRBMStatus != 0`
- `GFX,BootloadStatus & 0x80000000 != 0`
- `GFX,Stage = "gfx-alive"`

---

## 实现顺序

```
1. SMU Setup (2.1)
   ↓
2. 验证 EnableAllSmuFeatures 成功
   ↓
3. IH Init (2.2) - 可选，先跳过试试
   ↓
4. Doorbell Init (2.3) - 可选，先跳过试试
   ↓
5. 读取 GC 寄存器验证 (2.4)
   ↓
6. 如果仍失败，补充 IH/Doorbell 初始化
```

**优先级：** 先实现 2.1（SMU Setup），这是最关键的。2.2 和 2.3 可以根据测试结果决定是否需要。

---

## Phase 2 风险和挑战

### 风险 1：Driver Table 内容未知

参考实现只分配空间并告知 SMU 地址，不填充内容。SMU 会写入 telemetry、metrics 等数据。如果 SMU 期望特定的初始值，可能会失败。

**缓解措施：** 先用全零初始化，观察 SMU 响应。

### 风险 2：Feature Mask 设置错误

启用错误的 feature 可能导致：
- 风扇全速运转
- 功耗飙升
- 显示闪烁或黑屏

**缓解措施：**
1. 从保守的 feature mask 开始（只启用 GFX 和 UVD）
2. 观察温度和风扇转速
3. 准备随时强制重启

### 风险 3：时序问题

SMU 命令之间可能需要延迟。参考实现没有明确的 delay，但可能依赖硬件的自然延迟。

**缓解措施：** 在关键步骤之间加入 `IOSleep(10)` 观察效果。

---

## 成功后的下一步（Phase 3 预览）

如果 Phase 2 成功（GC 通电并响应）：

**Phase 3：引擎初始化**
1. CP (Command Processor) 初始化 - 提交命令包
2. SDMA (DMA 引擎) 初始化 - 测试内存拷贝
3. MES (Micro Engine Scheduler) 启动
4. 第一个 PM4 命令包测试

**Phase 4：计算测试**
1. Compute shader 加载
2. Dispatch 测试
3. 结果验证

---

## 参考资料

**参考实现关键文件：**
- `navi48/Cobalt64-main/src/navi48-bringup/src/amd/smu_v14_0.cpp` - SMU setup
- `navi48/Cobalt64-main/src/navi48-bringup/src/amd/amdgpu_init.cpp` - 初始化顺序
- `navi48/Cobalt64-main/src/navi48-bringup/src/amd/ih_v7_0.cpp` - IH 初始化
- `navi48/Cobalt64-main/src/navi48-bringup/src/amd/nbif_v6_3_1.cpp` - NBIO doorbell

**上游 Linux 驱动：**
- `drivers/gpu/drm/amd/pm/swsmu/smu14/smu_v14_0.c` - SMU14 实现
- `drivers/gpu/drm/amd/amdgpu/amdgpu_device.c` - 设备初始化顺序

---

## 时间估计

- **2.1 SMU Setup:** 4-6 小时（实现 + 调试）
- **2.2 IH Init:** 2-3 小时（如果需要）
- **2.3 Doorbell Init:** 1-2 小时（如果需要）
- **2.4 验证和调试:** 2-4 小时

**总计：** 9-15 小时（1-2 个工作日）

---

## 开始实现

准备好了就从 2.1 SMU Setup 开始。需要我现在开始实现吗？
