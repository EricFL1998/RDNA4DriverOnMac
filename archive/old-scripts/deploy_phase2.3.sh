#!/bin/bash
# Phase 2.3 快速部署脚本（在黑苹果上运行）

set -e

echo "========================================"
echo "  Phase 2.3 快速部署"
echo "========================================"
echo ""

# 1. 检查当前位置
if [ ! -d "RDNA4Driver" ]; then
    echo "❌ 错误: 请在项目根目录运行此脚本"
    exit 1
fi

# 2. 编译 RDNA4Driver
echo "Step 1: 编译 RDNA4Driver..."
cd RDNA4Driver
make clean
make
if [ $? -ne 0 ]; then
    echo "❌ 编译失败"
    exit 1
fi
echo "✅ 编译成功"
cd ..

# 3. 备份当前驱动
echo ""
echo "Step 2: 备份当前驱动..."
BACKUP_NAME="RDNA4Driver.kext.backup-$(date +%Y%m%d-%H%M%S)"
if [ -d "/Library/Extensions/RDNA4Driver.kext" ]; then
    sudo cp -R /Library/Extensions/RDNA4Driver.kext \
        "/Library/Extensions/$BACKUP_NAME"
    echo "✅ 备份到 /Library/Extensions/$BACKUP_NAME"
else
    echo "⚠️  当前无已安装驱动"
fi

# 4. 卸载旧驱动
echo ""
echo "Step 3: 卸载旧驱动..."
if kextstat | grep -q "com.rdna4.driver"; then
    sudo kextunload /Library/Extensions/RDNA4Driver.kext || true
    echo "✅ 旧驱动已卸载"
else
    echo "⚠️  旧驱动未加载"
fi

# 5. 安装新驱动
echo ""
echo "Step 4: 安装新驱动..."
sudo rm -rf /Library/Extensions/RDNA4Driver.kext
sudo cp -R RDNA4Driver/build/RDNA4Driver.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/RDNA4Driver.kext
echo "✅ 新驱动已安装"

# 6. 重建缓存
echo ""
echo "Step 5: 重建 kext 缓存..."
sudo kextcache -i /
echo "✅ 缓存已重建"

# 7. 询问是否立即重启
echo ""
echo "========================================"
echo "  安装完成！"
echo "========================================"
echo ""
echo "需要重启以加载新驱动。"
echo ""
read -p "是否立即重启？(y/N): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "正在重启..."
    sudo reboot
else
    echo ""
    echo "手动重启命令: sudo reboot"
    echo ""
    echo "重启后验证命令:"
    echo "  kextstat | grep rdna4"
    echo "  ioreg -l -n RDNA4Driver | grep ComputeRing"
    echo "  sudo dmesg | grep ComputeRing"
fi
