#!/bin/bash
# Quick Build Test - 检查所有文件是否就绪

echo "=== RDNA4 Driver Build Check ==="
echo ""

# 检查文件
echo "📁 Checking files..."
files=(
    "src/core/RDNA4Driver.hpp"
    "src/core/RDNA4Driver.cpp"
    "Info.plist"
    "Makefile"
)

all_ok=true
for file in "${files[@]}"; do
    if [ -f "$file" ]; then
        size=$(wc -c < "$file")
        echo "  ✓ $file ($size bytes)"
    else
        echo "  ✗ $file (missing)"
        all_ok=false
    fi
done

echo ""

# 检查代码行数
echo "📊 Code statistics..."
if [ -f "src/core/RDNA4Driver.cpp" ]; then
    lines=$(wc -l < src/core/RDNA4Driver.cpp)
    echo "  RDNA4Driver.cpp: $lines lines"
fi
if [ -f "src/core/RDNA4Driver.hpp" ]; then
    lines=$(wc -l < src/core/RDNA4Driver.hpp)
    echo "  RDNA4Driver.hpp: $lines lines"
fi

echo ""

# 检查 MacKernelSDK
echo "🔍 Checking dependencies..."
if [ -d "../navi48/MacKernelSDK/Headers" ]; then
    echo "  ✓ MacKernelSDK found"
else
    echo "  ✗ MacKernelSDK not found at ../navi48/MacKernelSDK"
    all_ok=false
fi

echo ""

# 总结
if $all_ok; then
    echo "✅ All files ready for compilation!"
    echo ""
    echo "Next steps (on macOS):"
    echo "  1. cd RDNA4Driver"
    echo "  2. make"
    echo "  3. make install"
    echo "  4. sudo kmutil load -p /Library/Extensions/RDNA4Driver.kext"
    echo "  5. reboot when requested"
else
    echo "❌ Some files are missing"
fi
