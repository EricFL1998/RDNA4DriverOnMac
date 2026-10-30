# Phase 2 修复版本 - 部署指南

## 当前状态

机器卡在启动阶段（HID 驱动加载）。已准备好修复版本。

## 修复版本特性

**改进：**
1. ✅ 寄存器写入前检查可访问性
2. ✅ 每个写入添加 1ms 延迟让硬件响应
3. ✅ 详细的日志记录每个步骤
4. ✅ 失败时安全降级，不阻塞系统启动
5. ✅ 检查 doorbell aperture 是否已启用，避免重复写入

**关键改进：**
```cpp
// 1. 验证寄存器可访问
uint32_t testRead = readNBIORegister(...);
if (testRead == 0xFFFFFFFF) {
    ABORT;  // 不继续破坏硬件
}

// 2. 写入时添加延迟
driver.writeRegister(byteOffset, value);
IOSleep(1);  // 让硬件响应
uint32_t after = driver.readRegister(byteOffset);

// 3. 失败时安全降级
if (!NBIODoorbell::initDoorbellPath(...)) {
    setProperty("NBIO,Stage", "failed-safe-fallback");
    // 继续启动，不崩溃
}
```

## 第一步：恢复系统启动

### 选项 A：单用户模式（Cmd+S）

如果能看到启动画面：

1. **完全关机**（长按电源键）
2. **重启时立即按住 Cmd+S**
3. 在提示符下输入：
   ```bash
   mount -uw /
   rm -rf /Library/Extensions/RDNA4Driver.kext
   kextcache -invalidate /
   reboot
   ```

### 选项 B：恢复模式（Cmd+R）

1. **完全关机**
2. **重启时按住 Cmd+R**
3. 进入恢复模式后，打开**终端**（菜单栏 → 实用工具 → 终端）
4. 输入：
   ```bash
   cd /Volumes/Macintosh\ HD
   rm -rf Library/Extensions/RDNA4Driver.kext
   kextcache -invalidate /
   reboot
   ```

### 选项 C：从 U盘 启动

如果 U盘 有可启动系统：

1. 重启时按住 **Option 键**
2. 选择 U盘 启动
3. 启动后打开终端
4. 挂载主系统盘：
   ```bash
   diskutil list
   # 找到主系统盘，通常是 disk0s1
   mkdir /tmp/maindisk
   mount -t apfs /dev/disk0s1 /tmp/maindisk
   
   # 删除驱动
   rm -rf /tmp/maindisk/Library/Extensions/RDNA4Driver.kext
   
   # 卸载并重启
   umount /tmp/maindisk
   reboot
   ```

## 第二步：验证系统恢复

系统重启后，SSH 连接测试：

```bash
ssh -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
  louxuanchen@192.168.31.57 'echo "System recovered!"'
```

如果能连接，说明系统已恢复。

## 第三步：部署修复版本

### 自动部署（推荐）

```bash
cd /Users/louxuanchen/UnityProjects/Projects/hackintosh
./deploy_phase2_fixed.sh
```

这个脚本会：
1. 上传修复版本
2. 安装并重启
3. 自动监控重启
4. 验证驱动状态
5. 显示 NBIO 日志

### 手动部署

如果自动脚本失败：

```bash
cd /Users/louxuanchen/UnityProjects/Projects/hackintosh

# 上传
scp -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
  -r RDNA4Driver/build/RDNA4Driver.kext louxuanchen@192.168.31.57:/tmp/

# 安装
ssh -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
  louxuanchen@192.168.31.57 << 'EOF'
sudo rm -rf /Library/Extensions/RDNA4Driver.kext
sudo cp -R /tmp/RDNA4Driver.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/RDNA4Driver.kext
sudo chmod -R 755 /Library/Extensions/RDNA4Driver.kext
sudo kextcache -invalidate /
sudo shutdown -r now
EOF
```

## 第四步：验证结果

重启后检查：

```bash
# 连接到实机
ssh -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
  louxuanchen@192.168.31.57

# 检查驱动状态
ioreg -l -w0 -r -n RDNA4Driver | grep -E "NBIO,Stage|GFX,GRBM|SMU,GC"

# 查看 NBIO 日志
sudo log show --last boot --style compact \
  --predicate 'eventMessage CONTAINS "NBIO:"' | grep RDNA4 | tail -30
```

### 成功指标

**如果 NBIO 初始化成功：**
```
NBIO,Stage = "complete"
GFX,GRBMStatus != 0  ← GC 通电成功！
SMU,GCPowerState = "active" 或 "partial"
```

**如果 NBIO 失败但安全降级：**
```
NBIO,Stage = "failed-safe-fallback"
GFX,GRBMStatus = 0  ← GC 仍未通电
系统正常启动（不崩溃）
```

### 日志关键字

成功时应该看到：
```
RDNA4: NBIO: doorbell_path_init: NBIO registers accessible
RDNA4: NBIO: REMAP_HDP_MEM_FLUSH_CNTL: SUCCESS
RDNA4: NBIO: enable_doorbell_aperture: SUCCESS
RDNA4: NBIO: enable_selfring_aperture: configured
RDNA4: NBIO: init_gc_doorbell: GC entries configured
RDNA4: NBIO: doorbell_path_init: complete
```

失败时会看到具体是哪个寄存器写入失败。

## 如果修复版本仍然挂起

如果修复版本仍然导致启动问题：

1. **重复第一步**恢复系统
2. **报告问题**：哪个寄存器写入时挂起（从日志看）
3. **可能需要**：逐个测试寄存器，或者放弃 NBIO 初始化

## 文件位置

- **部署脚本**：`deploy_phase2_fixed.sh`
- **修复版本**：`RDNA4Driver/build/RDNA4Driver.kext`
- **恢复指南**：`PHASE2_FIXED_DEPLOYMENT.md`（本文档）

## 技术细节

修复版本的源代码在：
- `RDNA4Driver/src/hardware/NBIODoorbell.cpp` - 添加了详细日志和错误处理
- `RDNA4Driver/src/core/RDNA4Driver.cpp` - 添加了安全降级逻辑

关键改进：
- 寄存器访问前验证
- 写入后添加 1ms 延迟
- 失败时不阻塞启动
- 详细的日志记录每个步骤

准备好部署了吗？先恢复系统，然后运行 `./deploy_phase2_fixed.sh`！
