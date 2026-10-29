# Phase 3 实施计划

**目标：** 完整的固件加载链 - PSP → SMU → GC 通电

## 当前状态

Phase 2 已完成：
- ✅ NBIO Doorbell 初始化成功
- ✅ GMC 初始化成功
- ✅ SMU Setup 代码已实现
- ⏸️ PSP/SMU/GFX 被 boot-args 禁用

## Phase 3 目标

### 核心目标
**让 GC (Graphics Core) 通电并响应寄存器读取**

### 验证标准
```
✅ GFX,GRBMStatus != 0           ← GC 寄存器响应（核心！）
✅ SMU,GCPowerState = "active"   ← GC 通电成功
✅ GFX,BootloadStatus & 0x80000000 ← RLC autoload 完成
```

## 实施步骤

### 步骤 1：设置 Boot-args

需要在 OpenCore config.plist 或通过 nvram 设置：
```
rdna4=1
rdna4-psp=1    ← 启用 PSP 固件加载
rdna4-smu=1    ← 启用 SMU 完整初始化
rdna4-gfx=1    ← 启用 GFX 初始化
```

**注意：** macOS 的 nvram 修改可能需要：
1. 关闭 SIP（System Integrity Protection）
2. 或者在恢复模式中修改
3. 或者直接编辑 OC config.plist

### 步骤 2：验证 PSP 初始化

**成功指标：**
```
日志: "PSP: SOS loaded and alive"
属性: "PSP,Stage" = "alive"
```

**可能问题：**
- PSP 固件加载超时
- TMR (Trusted Memory Region) 分配失败
- SOS 响应超时

### 步骤 3：验证 SMU Setup

**成功指标：**
```
日志: "SMU: Setup complete"
属性: "SMU,SetupStage" = "complete"
日志: "SMU: GC responding! BOOTLOAD_STATUS = 0x..."
```

**关键消息序列：**
1. SetDriverTableAddress
2. SetFeatureMask
3. UseDefaultPPTable
4. RunDcBtc
5. NotifyPowerSource
6. EnableAllSmuFeatures
7. 轮询 GC BOOTLOAD_STATUS

### 步骤 4：验证 GC 通电

**成功指标：**
```
GFX,GRBMStatus = 0x???????? (非零)
GFX,BootloadStatus = 0x8??????? (bit 31 置位)
```

## 可能遇到的问题

### 问题 1：PSP 固件加载失败

**症状：** PSP 超时，无响应  
**原因：**
- TMR 地址配置错误
- VRAM 未正确初始化
- PSP 寄存器配置错误

**解决：**
- 检查 TMR 分配（8MB 对齐）
- 验证 VRAM 地址范围
- 添加更多 PSP 寄存器日志

### 问题 2：SMU 消息失败

**症状：** SMU 消息超时或返回错误  
**原因：**
- 消息 ID 错误
- 参数错误
- SMU 固件未就绪

**解决：**
- 对比参考实现的消息序列
- 添加 SMU 响应码日志
- 检查 SMU 固件版本

### 问题 3：GC 不响应

**症状：** GRBM_STATUS = 0  
**原因：**
- GC 未通电
- NBIO doorbell 路径配置错误
- SMU 未完成 GC 电源管理
- RLC autoload 未完成

**解决：**
- 验证 SMU GCPowerState
- 检查 NBIO doorbell 配置
- 延长 GC 通电等待时间
- 读取更多 GC 寄存器

## 调试策略

### 1. 分阶段启用

不要一次启用所有功能，按顺序测试：

**阶段 1：** 只启用 PSP
```bash
rdna4=1 rdna4-psp=1
```
验证 PSP 加载成功再继续。

**阶段 2：** 启用 PSP + SMU
```bash
rdna4=1 rdna4-psp=1 rdna4-smu=1
```
验证 SMU Setup 完成再继续。

**阶段 3：** 全部启用
```bash
rdna4=1 rdna4-psp=1 rdna4-smu=1 rdna4-gfx=1
```
验证 GC 通电。

### 2. 详细日志

每个阶段都要检查：
```bash
# 查看所有驱动日志
sudo log show --last boot --predicate 'eventMessage CONTAINS "RDNA4:"' --style compact

# 查看特定模块
sudo log show --last boot --predicate 'eventMessage CONTAINS "PSP:"' --style compact
sudo log show --last boot --predicate 'eventMessage CONTAINS "SMU:"' --style compact
sudo log show --last boot --predicate 'eventMessage CONTAINS "GFX:"' --style compact
```

### 3. 寄存器转储

如果失败，转储关键寄存器：
```cpp
// PSP 寄存器
MP0_SMN_C2PMSG_*
PSP_RING_BASE, PSP_RING_SIZE, PSP_RING_HEAD, PSP_RING_TAIL

// SMU 寄存器
MP1_SMN_C2PMSG_*

// GC 寄存器
GRBM_STATUS, GRBM_STATUS2
CP_STAT, RLC_BOOTLOAD_STATUS
```

## 成功路径

如果一切顺利：

```
1. 系统启动
2. RDNA4Driver 加载
3. NBIO 初始化 ✅
4. GMC 初始化 ✅
5. PSP 固件加载 → "PSP: SOS alive"
6. SMU 初始化 → "SMU: alive"
7. SMU Setup → "SMU: Setup complete"
8. GC 通电等待 → "SMU: GC responding!"
9. GFX 初始化 → "GFX: GRBM_STATUS = 0x..."
10. Phase 3 完成 ✅
```

## 下一步行动

1. **修改 Boot-args**
   - 方式 A：编辑 OpenCore config.plist
   - 方式 B：在恢复模式设置 nvram

2. **阶段 1 测试** - 只启用 PSP
   - 重启
   - 检查 PSP 日志
   - 验证 PSP 成功

3. **阶段 2 测试** - 启用 PSP + SMU
   - 重启
   - 检查 SMU 日志
   - 验证 SMU Setup

4. **阶段 3 测试** - 全部启用
   - 重启
   - 检查 GC 状态
   - 验证通电成功

准备好了吗？让我们从设置 boot-args 开始！
