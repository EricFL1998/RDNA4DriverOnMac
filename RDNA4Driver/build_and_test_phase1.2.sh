#!/bin/bash
#
# Phase 1.2 构建和测试脚本
#

set -e

echo "======================================"
echo "Phase 1.2: IP Discovery 构建测试"
echo "======================================"
echo ""

# 1. 清理旧的构建
echo "1. 清理旧构建..."
make clean

# 2. 编译
echo ""
echo "2. 编译驱动..."
make

# 3. 检查编译结果
echo ""
echo "3. 检查编译结果..."
if [ -f "build/RDNA4Driver.kext/Contents/MacOS/RDNA4Driver" ]; then
    echo "✅ 编译成功"
    ls -lh build/RDNA4Driver.kext/Contents/MacOS/RDNA4Driver
else
    echo "❌ 编译失败"
    exit 1
fi

# 4. 检查符号
echo ""
echo "4. 检查 IP Discovery 符号..."
nm build/RDNA4Driver.kext/Contents/MacOS/RDNA4Driver | grep -i "ipdiscovery" | head -5

# 5. 提示下一步
echo ""
echo "======================================"
echo "✅ Phase 1.2 编译完成！"
echo "======================================"
echo ""
echo "下一步操作："
echo ""
echo "在黑苹果上测试："
echo "  1. scp -r build/RDNA4Driver.kext user@hackintosh:/tmp/"
echo "  2. ssh user@hackintosh"
echo "  3. sudo cp -r /tmp/RDNA4Driver.kext /Library/Extensions/"
echo "  4. sudo kextcache -i /"
echo "  5. sudo reboot"
echo ""
echo "重启后查看日志："
echo "  log show --predicate 'process == \"kernel\"' --last 5m | grep RDNA4"
echo ""
echo "应该看到 IP Discovery 输出："
echo "  RDNA4: IP Discovery starting..."
echo "  RDNA4: ===== Discovered IP Blocks ====="
echo "  RDNA4:   [ 0] PSP      v12.0.0 @ 0x00010000"
echo "  ..."
echo ""
