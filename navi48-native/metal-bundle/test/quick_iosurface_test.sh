#!/bin/bash
#
# Phase 3.5.1 - Quick IOSurface Test (No shaders required)
# 快速测试 IOSurface 基础功能
#

cd "$(dirname "$0")"
mkdir -p build

echo "========================================"
echo "  Quick IOSurface Test"
echo "========================================"
echo ""

# 编译
echo "Compiling..."
clang -framework Foundation -framework Metal -framework IOSurface -framework CoreVideo \
      -o build/QuickIOSurfaceTest \
      QuickIOSurfaceTest.m 2>&1

if [ $? -ne 0 ]; then
    echo "❌ Compilation failed"
    exit 1
fi

echo "✅ Compiled"
echo ""

# 运行
echo "Running test..."
echo ""
./build/QuickIOSurfaceTest

EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo "✅ Test PASSED"
else
    echo "❌ Test FAILED (exit code: $EXIT_CODE)"
fi

exit $EXIT_CODE
