#!/bin/bash
# Phase 2.3 完整实现 - 一键部署脚本（在黑苹果上运行）

set -e

echo "========================================"
echo "  Phase 2.3 完整部署"
echo "  首个真实 Compute Kernel"
echo "========================================"
echo ""

# 检查位置
if [ ! -d "RDNA4Driver" ]; then
    echo "❌ 错误: 请在项目根目录运行此脚本"
    exit 1
fi

# Step 1: 编译 Shader
echo "Step 1: 编译向量加法 Shader..."
cd RDNA4Driver
if [ ! -f "compile_shader.sh" ]; then
    echo "❌ 找不到 compile_shader.sh"
    exit 1
fi

chmod +x compile_shader.sh
./compile_shader.sh

if [ ! -f "shaders/compiled/vectoradd_gfx12.h" ]; then
    echo "❌ Shader 编译失败"
    exit 1
fi

echo "✅ Shader 编译成功"

# 检查是否使用 stub
if grep -q "USE_STUB_SHADER" shaders/compiled/vectoradd_gfx12.h | head -1; then
    echo "⚠️  警告: 仍在使用 STUB shader"
    echo "   如果 compile_shader.sh 失败，测试将使用 stub（预期失败）"
fi

cd ..

# Step 2: 编译驱动
echo ""
echo "Step 2: 编译 RDNA4Driver..."
cd RDNA4Driver
make clean
make

if [ ! -f "build/RDNA4Driver.kext/Contents/MacOS/RDNA4Driver" ]; then
    echo "❌ 驱动编译失败"
    exit 1
fi

echo "✅ 驱动编译成功"
cd ..

# Step 3: 备份
echo ""
echo "Step 3: 备份当前驱动..."
BACKUP_NAME="RDNA4Driver.kext.backup-phase2.3-$(date +%Y%m%d-%H%M%S)"
if [ -d "/Library/Extensions/RDNA4Driver.kext" ]; then
    sudo cp -R /Library/Extensions/RDNA4Driver.kext \
        "/Library/Extensions/$BACKUP_NAME"
    echo "✅ 备份到 /Library/Extensions/$BACKUP_NAME"
else
    echo "⚠️  当前无已安装驱动"
fi

# Step 4: 卸载旧驱动
echo ""
echo "Step 4: 卸载旧驱动..."
if kextstat | grep -q "com.rdna4.driver"; then
    sudo kextunload /Library/Extensions/RDNA4Driver.kext || true
    echo "✅ 旧驱动已卸载"
else
    echo "⚠️  旧驱动未加载"
fi

# Step 5: 安装新驱动
echo ""
echo "Step 5: 安装新驱动..."
sudo rm -rf /Library/Extensions/RDNA4Driver.kext
sudo cp -R RDNA4Driver/build/RDNA4Driver.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/RDNA4Driver.kext
echo "✅ 新驱动已安装"

# Step 6: 重建缓存
echo ""
echo "Step 6: 重建 kext 缓存..."
sudo kextcache -i /
echo "✅ 缓存已重建"

# Step 7: 检查 boot-args
echo ""
echo "Step 7: 检查启动参数..."
echo ""
echo "请确保 boot-args 包含以下参数:"
echo "  rdna4=1"
echo "  rdna4-psp=1"
echo "  rdna4-smu=1"
echo "  rdna4-gfx=1"
echo "  rdna4-test-compute=1  ⬅️ 新增：自动运行测试"
echo ""
echo "编辑方法:"
echo "  sudo nano /Volumes/EFI/OC/config.plist"
echo "  或使用 OpenCore Configurator"
echo ""

read -p "已添加 rdna4-test-compute=1 参数？(y/N): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo ""
    echo "⚠️  请先添加启动参数，然后重启"
    echo "   或者手动重启后查看日志验证"
    exit 0
fi

# Step 8: 询问是否立即重启
echo ""
echo "========================================"
echo "  安装完成！"
echo "========================================"
echo ""
echo "重启后将自动运行向量加法测试。"
echo ""
echo "验证命令（重启后）:"
echo "  sudo dmesg | grep -A50 'Phase 2.3: Vector Add Test'"
echo ""
read -p "是否立即重启？(y/N): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "正在重启..."
    sudo reboot
else
    echo ""
    echo "手动重启: sudo reboot"
    echo ""
    echo "重启后验证步骤:"
    echo "  1. kextstat | grep rdna4"
    echo "  2. ioreg -l -n RDNA4Driver | grep ComputeRing"
    echo "  3. sudo dmesg | grep 'Vector Add Test'"
    echo "  4. 查找 '✅ Vector Add Test: PASSED!'"
fi
