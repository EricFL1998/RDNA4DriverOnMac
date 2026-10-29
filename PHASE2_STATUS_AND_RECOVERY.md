# Phase 2 当前状态和恢复步骤

**时间：** 2026-10-28 20:50+  
**状态：** 机器离线，需要物理恢复

## 当前情况

1. **机器状态：** 启动失败，SSH 无响应（离线 15+ 分钟）
2. **原因：** NBIO doorbell 初始化导致系统挂起
3. **已准备：** 完全禁用 NBIO 的安全版本已编译

## 恢复步骤（需要物理访问）

### 第一步：恢复系统启动

**方法 A - 恢复模式（推荐）：**
```bash
1. 关机（长按电源键）
2. 重启时按住 Cmd+R 进入恢复模式
3. 在恢复模式终端中执行：
   cd /Volumes/Macintosh\ HD
   rm -rf Library/Extensions/RDNA4Driver.kext
   kextcache -invalidate /
   reboot
```

**方法 B - 单用户模式：**
```bash
1. 重启时按住 Cmd+S
2. 在提示符下执行：
   mount -uw /
   rm -rf /Library/Extensions/RDNA4Driver.kext
   kextcache -invalidate /
   reboot
```

### 第二步：部署安全版本

系统恢复后，在本地执行：

```bash
# 部署完全禁用 NBIO 的版本
cd /Users/louxuanchen/UnityProjects/Projects/hackintosh

scp -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
  -r RDNA4Driver/build/RDNA4Driver.kext louxuanchen@192.168.31.57:/tmp/

ssh -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
  louxuanchen@192.168.31.57 << 'EOF'
sudo rm -rf /Library/Extensions/RDNA4Driver.kext
sudo cp -R /tmp/RDNA4Driver.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/RDNA4Driver.kext
sudo chmod -R 755 /Library/Extensions/RDNA4Driver.kext
sudo kmutil load -p /Library/Extensions/RDNA4Driver.kext
sudo shutdown -r now
EOF
```

### 第三步：验证

重启后验证系统正常：

```bash
# 连接测试
ssh louxuanchen@192.168.31.57

# 检查驱动状态
ioreg -l -w0 -r -n RDNA4Driver | grep -E "PSP,Stage|SMU,Stage|GFX,Stage"

# 应该看到：
# PSP,Stage = "alive"
# SMU,Stage = "alive"
# GFX,Stage = "autoload-pending"
```

## 当前版本说明

**安全版本特性：**
- ✅ 保留 SMU 完整初始化
- ✅ 保留 Phase 1 所有功能
- ❌ **完全禁用 NBIO 初始化**（注释掉 performNBIOInit() 调用）
- ✅ 系统应该能正常启动

**代码位置：**
```cpp
// RDNA4Driver/src/core/RDNA4Driver.cpp start() 函数
// Phase 1.5: NBIO Doorbell 初始化 - TEMPORARILY DISABLED
// NBIO initialization caused boot hang, disabled until debugged
// performNBIOInit();  ← 已注释
```

## Phase 2 工作总结

### 已完成
1. ✅ SMU 完整初始化（Driver Table, Feature Mask, Power Table, DcBtc, EnableAllFeatures）
2. ✅ SMU 消息 ID 修正
3. ✅ NBIO Doorbell 完整实现（~240 行代码）
4. ✅ 初始化顺序调整

### 问题
- ❌ NBIO 初始化导致系统启动挂起
- 需要进一步调试寄存器配置

### 下一步（系统恢复后）
1. 验证安全版本（无 NBIO）能正常启动
2. 分析 NBIO 挂起原因
3. 分阶段测试 NBIO 功能
4. 或者尝试其他方法启动 GC（可能 SMU Setup 已经足够）

## 文件位置

**编译好的安全版本：**
```
RDNA4Driver/build/RDNA4Driver.kext  ← 部署这个
```

**文档：**
- `PHASE2_RECOVERY_GUIDE.md` - 详细恢复指南
- `PHASE2_BOOT_FAILURE_DIAGNOSIS.md` - 故障诊断
- `PHASE2_IMPLEMENTATION_SUMMARY.md` - 实现总结
- `PHASE2_STATUS_AND_RECOVERY.md` - 本文档

**源代码：**
- NBIO 实现：`RDNA4Driver/src/hardware/NBIODoorbell.cpp`
- 主驱动：`RDNA4Driver/src/core/RDNA4Driver.cpp`（NBIO 已禁用）

## 等待用户操作

当前需要：
1. **物理访问机器** - 恢复系统启动
2. **部署安全版本** - 验证系统正常
3. **反馈状态** - 告知是否成功恢复

恢复后可以继续调试 NBIO 问题，或者尝试不依赖 NBIO 启动 GC。
