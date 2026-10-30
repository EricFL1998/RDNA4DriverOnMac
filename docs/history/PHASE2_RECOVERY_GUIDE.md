# Phase 2 系统恢复指南

## 当前状态

机器在部署完整 NBIO 初始化后无法启动（9+ 分钟离线，SSH 无响应）。

## 立即恢复步骤

### 1. 物理检查机器

**检查项：**
- [ ] 显示器是否有画面？
  - 黑屏 → 可能系统挂在内核初始化
  - 冻结画面 → 可能驱动加载时挂起
  - 正常画面 → 可能只是网络问题
- [ ] 键盘是否响应？（尝试 Caps Lock，看指示灯）
- [ ] 能否进入恢复模式？（按住 Cmd+R 重启）

### 2. 恢复系统启动

#### 方案 A：进入恢复模式
```bash
# 在恢复模式的终端中：
cd /Volumes/Macintosh\ HD
rm -rf Library/Extensions/RDNA4Driver.kext
kextcache -invalidate /
reboot
```

#### 方案 B：禁用驱动加载
如果能看到启动界面：
```bash
# 按住 Cmd+S 进入单用户模式
mount -uw /
rm -rf /Library/Extensions/RDNA4Driver.kext
kextcache -invalidate /
reboot
```

#### 方案 C：从时间机器恢复
如果有备份，恢复到 Phase 2 之前的状态。

### 3. 恢复后验证

系统重启后，确认：
```bash
ssh louxuanchen@192.168.31.57
ls -la /Library/Extensions/RDNA4Driver.kext
# 应该不存在或者是旧版本
```

## 部署安全版本

系统恢复后，部署 NBIO 禁用版本：

```bash
# 本地编译已完成，安全版本在：
# RDNA4Driver/build/RDNA4Driver.kext

# 部署（NBIO 默认禁用）：
scp -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
  -r RDNA4Driver/build/RDNA4Driver.kext louxuanchen@192.168.31.57:/tmp/

ssh -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
  louxuanchen@192.168.31.57 \
  'sudo rm -rf /Library/Extensions/RDNA4Driver.kext && \
   sudo cp -R /tmp/RDNA4Driver.kext /Library/Extensions/ && \
   sudo chown -R root:wheel /Library/Extensions/RDNA4Driver.kext && \
   sudo chmod -R 755 /Library/Extensions/RDNA4Driver.kext && \
   sudo kmutil load -p /Library/Extensions/RDNA4Driver.kext && \
   sudo shutdown -r now'
```

### 验证安全版本

重启后：
```bash
ssh louxuanchen@192.168.31.57
ioreg -l -w0 -r -n RDNA4Driver | grep "NBIO,Stage"
# 应该显示: "NBIO,Stage" = "disabled-boot-hang-workaround"
```

## 调试 NBIO 问题

如果要重新测试 NBIO，需要分阶段：

### 阶段 1：只读测试
```cpp
// 修改 NBIODoorbell.cpp initDoorbellPath:
bool NBIODoorbell::initDoorbellPath(...) {
    // 只读寄存器，不写入
    uint32_t aper = readNBIORegister(driver, 0x00c0, 2);
    uint32_t selfring = readNBIORegister(driver, 0x00f5, 2);
    NBIO_LOG("READ TEST: APER=0x%08x SELFRING=0x%08x", aper, selfring);
    return true;  // 不执行任何写入
}
```

### 阶段 2：单步写入测试
每次只启用一个功能：
1. 只测试 enableDoorbellAperture
2. 只测试 remapHDPRegisters
3. 只测试 enableSelfringAperture
4. 最后测试 initGCDoorbell

### 启用 NBIO 测试
系统恢复后，如果要测试 NBIO：
```bash
# 添加 boot-arg:
sudo nvram boot-args="rdna4=1 rdna4-nbio=1"
sudo reboot
```

## 替代方案

如果 NBIO 始终导致问题，可以：

### 方案 1：跳过 NBIO，直接测试 GC
```cpp
void RDNA4Driver::performNBIOInit() {
    LOG("NBIO: skipped - testing GC without doorbell init");
    setProperty("NBIO,Stage", "skipped");
}
```

### 方案 2：使用参考实现的最小化版本
只实现绝对必要的寄存器写入，跳过所有可选步骤。

### 方案 3：在 SMU Setup 后直接轮询 GC
可能 SMU Setup 已经足够启动 GC，只是需要更长的等待时间。

## 文件清单

**安全版本（已编译）：**
- `RDNA4Driver/build/RDNA4Driver.kext` - NBIO 默认禁用

**诊断文档：**
- `PHASE2_BOOT_FAILURE_DIAGNOSIS.md` - 失败分析
- `PHASE2_RECOVERY_GUIDE.md` - 本文档

**实现代码（待调试）：**
- `RDNA4Driver/src/hardware/NBIODoorbell.cpp`
- `RDNA4Driver/src/hardware/NBIODoorbell.hpp`

## 下一步

1. **恢复系统** - 物理访问机器，移除失败的驱动
2. **部署安全版本** - NBIO 禁用，验证系统正常
3. **分析原因** - 对比参考实现，找到导致挂起的寄存器
4. **分阶段测试** - 逐步启用 NBIO 功能，定位问题
5. **修复并重试** - 修正问题后再次实机验证

## 联系我

如果需要进一步的技术支持或有新的发现，请提供：
- 机器物理状态（显示、键盘响应）
- 能否进入恢复模式
- 系统日志（如果能访问）
