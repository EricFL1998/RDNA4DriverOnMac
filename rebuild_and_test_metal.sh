#!/bin/bash
#
# rebuild_and_test_metal.sh - 重新编译并测试 Metal Bundle
#
# Phase 3.4 设备注册修复
#

set -e  # Exit on error

echo "========================================"
echo "  RDNA4Metal Bundle 重新编译"
echo "  Phase 3.4 - Device Registration Fix"
echo "========================================"
echo ""

# 定位到正确目录
cd "$(dirname "$0")"
cd navi48-native/metal-bundle

echo "Step 1: 清理旧构建"
echo "========================================="
make clean
echo "✅ 清理完成"
echo ""

echo "Step 2: 编译 Metal Bundle"
echo "========================================="
make
if [ $? -ne 0 ]; then
    echo "❌ 编译失败"
    exit 1
fi
echo "✅ 编译成功"
echo ""

echo "Step 3: 检查构建产物"
echo "========================================="
if [ ! -f "build/RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal" ]; then
    echo "❌ 主库文件不存在"
    exit 1
fi

if [ ! -f "build/RDNA4Metal.bundle/Contents/Info.plist" ]; then
    echo "❌ Info.plist 不存在"
    exit 1
fi

echo "✅ 主库: $(ls -lh build/RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal | awk '{print $5}')"
echo "✅ Info.plist 存在"
echo ""

echo "Step 4: 检查符号导出"
echo "========================================="
SYMBOLS=$(nm build/RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal | grep RDNA4Create || true)
if [ -z "$SYMBOLS" ]; then
    echo "⚠️  警告: RDNA4CreateDevice 符号未找到"
    echo "   可能需要修复导出配置"
else
    echo "✅ 找到导出符号:"
    echo "$SYMBOLS"
fi
echo ""

echo "Step 5: 安装到系统"
echo "========================================="
echo "需要 sudo 权限..."
sudo make install
if [ $? -ne 0 ]; then
    echo "❌ 安装失败"
    exit 1
fi
echo "✅ 安装成功"
echo ""

echo "Step 6: 验证安装"
echo "========================================="
if [ ! -d "/Library/GPUBundles/RDNA4Metal.bundle" ]; then
    echo "❌ Bundle 未安装到 /Library/GPUBundles/"
    exit 1
fi

echo "✅ Bundle 位置: /Library/GPUBundles/RDNA4Metal.bundle"
echo "✅ 权限: $(ls -ld /Library/GPUBundles/RDNA4Metal.bundle | awk '{print $3":"$4}')"
echo ""

echo "Step 7: 编译设备注册测试"
echo "========================================="
if [ ! -f "test/DeviceRegistrationTest.m" ]; then
    echo "⚠️  DeviceRegistrationTest.m 不存在，跳过"
else
    clang -framework Foundation -framework Metal \
          -o build/test/DeviceRegistrationTest \
          test/DeviceRegistrationTest.m
    
    if [ $? -eq 0 ]; then
        echo "✅ 设备注册测试编译成功"
    else
        echo "❌ 设备注册测试编译失败"
    fi
fi
echo ""

echo "Step 8: 清除系统缓存"
echo "========================================="
echo "清除 Metal 缓存..."
sudo rm -rf /System/Library/Caches/com.apple.metal 2>/dev/null || true
sudo touch /Library/GPUBundles/
echo "✅ 缓存已清除"
echo ""

echo "========================================"
echo "  构建和安装完成！"
echo "========================================"
echo ""
echo "测试命令:"
echo ""
echo "1. 设备注册测试:"
echo "   ./build/test/DeviceRegistrationTest"
echo ""
echo "2. Phase 3.3 测试 (Compute):"
echo "   ./build/test/RDNA4MetalTest"
echo ""
echo "3. Phase 3.4 测试 (Graphics):"
echo "   cd build/test && ./TriangleTest"
echo ""
echo "4. 查看系统日志:"
echo "   log stream --predicate 'subsystem == \"com.apple.Metal\"' --level debug"
echo ""
echo "5. 查看 RDNA4Metal 日志:"
echo "   log show --predicate 'eventMessage CONTAINS \"RDNA4\"' --last 5m"
echo ""

echo "========================================"
echo "  运行设备注册测试"
echo "========================================"
echo ""

if [ -f "build/test/DeviceRegistrationTest" ]; then
    ./build/test/DeviceRegistrationTest
else
    echo "⚠️  DeviceRegistrationTest 不存在，手动运行测试"
fi

echo ""
echo "✅ 全部完成！"
