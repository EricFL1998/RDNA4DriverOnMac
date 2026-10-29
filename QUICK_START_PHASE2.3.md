# Phase 2.3 快速开始

## 🚀 在黑苹果上部署（3 分钟）

### 方法 1: 使用自动脚本（推荐）

```bash
# 传输整个项目到黑苹果
scp -r ~/hackintosh user@hackintosh:/tmp/

# SSH 登录黑苹果
ssh user@hackintosh

# 运行部署脚本
cd /tmp/hackintosh
./deploy_phase2.3.sh

# 脚本会自动：
# - 编译 RDNA4Driver
# - 备份旧驱动
# - 安装新驱动
# - 重建缓存
# - 提示重启
```

### 方法 2: 手动步骤

```bash
# 在黑苹果上
cd ~/hackintosh/RDNA4Driver
make clean && make
sudo kextunload /Library/Extensions/RDNA4Driver.kext
sudo cp -R build/RDNA4Driver.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/RDNA4Driver.kext
sudo kextcache -i /
sudo reboot
```

## ✅ 重启后验证

```bash
# 1. 检查驱动加载
kextstat | grep rdna4
# 期望: com.rdna4.driver (0.2.0)

# 2. 检查 ComputeRing
ioreg -l -n RDNA4Driver | grep ComputeRing
# 期望: "ComputeRing,Initialized" = Yes

# 3. 查看日志
sudo dmesg | grep ComputeRing
# 期望: ComputeRing: initialized - ring at VRAM+0x4000000
```

## 📚 详细文档

- `docs/DEPLOYMENT_CHECKLIST_PHASE2.3.md` - 完整部署清单
- `docs/PHASE2.3_IMPLEMENTATION.md` - 技术文档
- `docs/PHASE2.3_COMPLETION_SUMMARY.md` - 完成总结

## 🔜 下一步

验证成功后，继续 Phase 2.3 完整实现（真实 Compute Kernel）。
