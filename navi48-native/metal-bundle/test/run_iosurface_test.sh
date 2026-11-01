#!/bin/bash
#
# Phase 3.5.1 - IOSurface Test Runner
# 编译并运行 IOSurface 支持测试
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="$SCRIPT_DIR"
BUILD_DIR="$TEST_DIR/build"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo -e "${BLUE}==>${NC} $1"
}

echo "========================================"
echo "  Phase 3.5.1 - IOSurface Test"
echo "========================================"
echo ""

# ============================================================
# Step 1: 检查环境
# ============================================================
log_step "Checking environment..."

if [ "$(uname)" != "Darwin" ]; then
    log_error "This test must run on macOS"
    exit 1
fi

if ! command -v clang &> /dev/null; then
    log_error "clang not found. Install Xcode Command Line Tools:"
    echo "    xcode-select --install"
    exit 1
fi

log_info "✅ Environment OK"
echo ""

# ============================================================
# Step 2: 创建 build 目录
# ============================================================
log_step "Creating build directory..."

mkdir -p "$BUILD_DIR"
log_info "✅ Build directory: $BUILD_DIR"
echo ""

# ============================================================
# Step 3: 编译测试程序
# ============================================================
log_step "Compiling IOSurfaceTest..."

clang -fno-objc-arc -framework Foundation \
      -framework Metal \
      -framework IOSurface \
      -framework QuartzCore \
      -framework CoreVideo \
      -o "$BUILD_DIR/IOSurfaceTest" \
      "$TEST_DIR/IOSurfaceTest.m"

if [ $? -eq 0 ]; then
    log_info "✅ Compilation successful"
else
    log_error "Compilation failed"
    exit 1
fi
echo ""

# ============================================================
# Step 4: 检查 shader（可选）
# ============================================================
log_step "Checking for shader library..."

if [ ! -f "$TEST_DIR/triangle.metallib" ]; then
    log_warn "triangle.metallib not found"
    log_warn "Test 6 (GPU rendering) will be skipped"
    log_warn ""
    log_warn "To enable GPU rendering test, compile shader:"
    log_warn "  cd $TEST_DIR"
    log_warn "  xcrun -sdk macosx metal -c triangle.metal -o triangle.air"
    log_warn "  xcrun -sdk macosx metallib triangle.air -o triangle.metallib"
    echo ""
else
    log_info "✅ Shader library found"
    # 复制到 build 目录
    cp "$TEST_DIR/triangle.metallib" "$BUILD_DIR/"
    echo ""
fi

# ============================================================
# Step 5: 运行测试
# ============================================================
log_step "Running IOSurfaceTest..."
echo ""

cd "$BUILD_DIR"
./IOSurfaceTest

TEST_EXIT_CODE=$?

echo ""
log_step "Test completed with exit code: $TEST_EXIT_CODE"
echo ""

# ============================================================
# Step 6: 分析结果
# ============================================================
if [ $TEST_EXIT_CODE -eq 0 ]; then
    log_info "✅✅✅ Phase 3.5.1 IOSurface Test PASSED ✅✅✅"
    echo ""
    echo "Next steps:"
    echo "  1. Verify IOSurface is correctly mapped to GPU memory"
    echo "  2. Implement RDNA4Texture initWithIOSurface method"
    echo "  3. Test with CAMetalLayer (Phase 3.5.2)"
else
    log_error "❌ Phase 3.5.1 IOSurface Test FAILED"
    echo ""
    echo "Troubleshooting:"
    echo "  1. Check if Metal device supports IOSurface"
    echo "  2. Verify IOSurface creation parameters"
    echo "  3. Check texture descriptor compatibility"
fi

echo ""
echo "========================================"
echo "  Test Summary"
echo "========================================"
echo "Exit code: $TEST_EXIT_CODE"
echo "Build directory: $BUILD_DIR"
echo "========================================"

exit $TEST_EXIT_CODE
