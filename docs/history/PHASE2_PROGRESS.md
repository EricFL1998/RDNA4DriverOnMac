# Phase 2 进展总结

## 已完成的工作

### SMU 完整初始化流程

实现了参考实现的 `smu_smc_hw_setup` 完整流程：

1. **Driver Table 配置** ✅
   - 在 VRAM 末尾分配 4KB driver table
   - 通过 `SetDriverDramAddrHigh/Low` 告知 SMU
   - 成功：SMU 接受了地址配置

2. **Feature Mask 配置** ❌
   - 调用 `SetAllowedFeaturesMaskLow/High` (0xFFFFFFFF)
   - 失败，但参考实现标记为 non-fatal

3. **Power Table 初始化** ✅
   - 调用 `UseDefaultPPTable`
   - 成功：SMU 加载了默认 power table

4. **DcBtc 运行** ✅
   - 调用 `RunDcBtc` (Display Configuration BTC)
   - 成功：这是关键的显示相关初始化

5. **Power Source 通知** ❌
   - 调用 `NotifyPowerSource` (AC power)
   - 失败，参考实现标记为 non-fatal

6. **Enable All Features** ❌
   - 调用 `EnableAllSmuFeatures`
   - 失败：参考实现注释说可能是 PMFW 自动管理 features

## SMU 消息 ID 修正

发现并修正了消息 ID 错误：
- `UseDefaultPPTable`: 0x13 → 0x14 ✅
- `NotifyPowerSource`: 0x1A → 0x35 ✅
- `RunDcBtc`: 0x1D → 0x36 ✅

修正后 UseDefaultPPTable 和 RunDcBtc 成功。

## 当前状态

**成功的步骤：**
- SetDriverTableAddress
- UseDefaultPPTable  
- RunDcBtc

**失败的步骤：**
- SetFeatureMask (non-fatal)
- NotifyPowerSource (non-fatal)
- EnableAllSmuFeatures (可能是 PMFW 自动管理)

## 下一步验证

等待当前测试完成，检查：

### 关键问题：GC 是否通电？

**验证方法：**
1. 读取 `GRBM_STATUS` (0x0DA4) - 应为非零
2. 读取 `RLC_BOOTLOAD_STATUS` (0x4E7C) - bit 31 应为 1
3. 检查 TLB invalidate 是否响应

**可能的结果：**

1. **如果 GC 已通电**（GRBM != 0）
   - ✅ Phase 2 成功！
   - 说明 RunDcBtc 触发了 GC 启动
   - EnableAllSmuFeatures 不是必需的

2. **如果 GC 仍为 0**
   - 需要其他初始化步骤
   - 可能需要 IH (Interrupt Handler) 初始化
   - 可能需要 Doorbell 初始化
   - 或者需要等待更长时间

## Phase 2.2 和 2.3 准备

如果 GC 仍未通电，下一步实现：

### 2.2 IH 初始化
- 分配 IH ring buffer
- 配置 IH 寄存器
- 简化实现：只配置，不启用中断

### 2.3 Doorbell 初始化
- 配置 NBIO doorbell 寄存器
- 映射 BAR2 doorbell 区域

## 技术分析

### RunDcBtc 的重要性

RunDcBtc (Display Configuration BTC) 成功是重大进展，因为：
1. 它是显示子系统的初始化
2. 可能触发了部分 GPU 通电
3. 参考实现在此之后可以读取 telemetry

### EnableAllSmuFeatures 失败的可能原因

参考实现的注释："if UnknownCmd, PMFW feature control is autoload-internal"

这意味着某些 PMFW 版本自动管理 features，不需要驱动显式启用。如果是这种情况：
- RunDcBtc 可能已经触发了必要的初始化
- GC 可能已经自动通电
- 只需要验证状态即可

## 测试中

当前正在测试跳过 EnableAllSmuFeatures 失败检查，直接验证 GC 状态。
