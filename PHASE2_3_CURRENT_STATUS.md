# Phase 2 & 3 当前状态

**日期：** 2026-10-28 23:30+  
**状态：** Phase 2 完成 ✅，Phase 3 部署中 🔄

## Phase 2 成就 ✅

### 核心实现
1. **NBIO Doorbell 完整初始化** - 240+ 行代码
   - ✅ HDP Remap
   - ✅ Doorbell Aperture Enable
   - ✅ Self-ring Aperture 配置
   - ✅ GC Doorbell Entries (0, 3)
   - ✅ 实机验证：`NBIO,Stage = "complete"`

2. **SMU 完整初始化实现** - 6 个新函数
   - ✅ Driver Table 配置
   - ✅ Feature Mask 设置
   - ✅ Power Table 加载
   - ✅ DcBtc 运行
   - ✅ Power Source 通知
   - ✅ Enable All Features
   - ✅ GC 通电等待轮询

3. **系统稳定性**
   - ✅ 正常启动，无崩溃
   - ✅ 16GB VRAM 检测
   - ✅ 46 个 IP blocks 发现
   - ✅ GMC 初始化成功

### 技术挑战与解决
- ❌ 首次启动挂起 → ✅ 放宽 boot-arg 检查
- ❌ 内核集合缓存 → ✅ 完全删除重装
- ❌ 驱动需要批准 → ✅ 用户手动批准

## Phase 3 目标 🎯

### 核心目标
**让 GC (Graphics Core) 通电并响应寄存器读取**

### 验证标准
```
✅ PSP,Stage = "alive"
✅ SMU,SetupStage = "complete"
✅ SMU,GCPowerState = "active"
✅ GFX,GRBMStatus != 0           ← 核心目标！
✅ GFX,BootloadStatus & 0x80000000
```

### 代码修改
已将 PSP、SMU、GFX 的 boot-arg 检查改为：
```cpp
// 旧代码（需要 rdna4-xxx=1）
uint32_t enabled = 0;
if (!PE_parse_boot_argn("rdna4-psp", &enabled, sizeof(enabled)) || enabled == 0) {
    return;  // 跳过
}

// 新代码（默认启用，除非 rdna4-xxx=0）
uint32_t enabled = 1;  // 默认启用
bool hasBootArg = PE_parse_boot_argn("rdna4-psp", &enabled, sizeof(enabled));
if (hasBootArg && enabled == 0) {
    return;  // 明确禁用才跳过
}
LOG("PSP: starting initialization...");
```

## 遇到的技术挑战

### 问题：内核集合缓存顽固
**症状：** 代码修改后，系统仍然加载旧版本

**尝试的解决方案：**
1. ❌ `kextcache -invalidate /`
2. ❌ `kmutil install --update-all`
3. ❌ 触摸 kext 文件更新时间戳
4. ❌ 重建内核集合
5. 🔄 **完全删除 → 重启 → 重装 → 重启**（进行中）

**根本原因：**
macOS 11+ 的 KernelCollection 系统非常激进地缓存 kext。即使 `/Library/Extensions/` 中的文件更新了，系统可能仍然从预编译的内核集合中加载旧代码。

**终极解决方案：**
1. 完全删除 kext
2. 重建内核集合（不含该 kext）
3. 重启（清除所有缓存）
4. 重新安装新版本
5. 再次重建内核集合
6. 再次重启

## 当前部署状态 🔄

**正在执行：**
```
步骤 1: 删除旧驱动 → 重建 KC → 重启 ✅
步骤 2: 等待系统上线 🔄
步骤 3: 安装新驱动 → 验证二进制 → 重建 KC → 重启 ⏳
步骤 4: 验证 Phase 3 结果 ⏳
```

## 预期结果

如果这次成功，应该看到：

### PSP 初始化
```
RDNA4: PSP: starting initialization (rdna4-psp=1, present=0)
RDNA4: PSP: TMR allocated at 0x...
RDNA4: PSP: C2PMSG registers mapped
RDNA4: PSP: loading SOS firmware...
RDNA4: PSP: waiting for SOS response...
RDNA4: PSP: SOS loaded and alive
属性: "PSP,Stage" = "alive"
```

### SMU 初始化
```
RDNA4: SMU: starting initialization
RDNA4: SMU: firmware version 0x...
RDNA4: SMU: driver table set at 0x...
RDNA4: SMU: feature mask set
RDNA4: SMU: default power table loaded
RDNA4: SMU: RunDcBtc complete
RDNA4: SMU: power source notified
RDNA4: SMU: all features enabled
RDNA4: SMU: Setup complete
属性: "SMU,SetupStage" = "complete"
```

### GC 通电
```
RDNA4: SMU: waiting for GC to power on...
RDNA4: SMU: GC responding! BOOTLOAD_STATUS = 0x8...
属性: "SMU,GCPowerState" = "active"
```

### GFX 初始化
```
RDNA4: GFX: starting initialization
RDNA4: GFX: reading GRBM_STATUS...
RDNA4: GFX: GRBM_STATUS = 0x... (non-zero!)
RDNA4: GFX: GC is alive and responding!
属性: "GFX,GRBMStatus" = 0x...  (非零)
```

## 如果仍然失败

如果这次仍然加载旧代码，可能需要：

1. **检查是否有多个内核集合**
   ```bash
   ls -la /Library/Apple/System/Library/PrelinkedKernels/
   ls -la /System/Library/KernelCollections/
   ```

2. **手动删除所有内核集合缓存**
   ```bash
   sudo rm -rf /Library/Apple/System/Library/PrelinkedKernels/*
   sudo rm -rf /System/Library/KernelCollections/BootKernelExtensions.kc*
   sudo rm -rf /System/Library/KernelCollections/SystemKernelExtensions.kc*
   ```

3. **或者：修改 Info.plist 版本号**
   每次修改代码后，递增版本号（0.1.0 → 0.1.1 → 0.1.2...）
   可能会强制系统识别为新 kext

## 代码统计

### Phase 2 + Phase 3
- **新增代码：** ~350 行
- **修改代码：** ~50 行
- **新增文件：** 2 个（NBIODoorbell.cpp/.hpp）
- **编译次数：** 20+
- **实机测试：** 15+ 次重启
- **调试时间：** ~3 小时

## 下一步

等待当前重启完成，运行 Phase 3 完整验证。如果成功：
- ✅ PSP 固件加载
- ✅ SMU Setup 完成
- ✅ GC 通电响应

**这将是 RDNA4 黑苹果驱动开发的重大突破！** 🎉
