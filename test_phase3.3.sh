#!/bin/bash
# Phase 3.3 自动化测试脚本
# 在黑苹果上运行，自动完成编译、部署、测试全流程

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$SCRIPT_DIR"

echo "========================================"
echo "  Phase 3.3 自动化测试"
echo "  RDNA4 Metal Bundle 完整验证"
echo "========================================"
echo ""

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
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

check_command() {
    if ! command -v $1 &> /dev/null; then
        log_error "$1 not found. Please install it first."
        exit 1
    fi
}

# ============================================================
# 步骤 0: 前置条件检查
# ============================================================

echo "Step 0: 检查前置条件..."

# 检查是否在黑苹果上
if [ "$(uname)" != "Darwin" ]; then
    log_error "This script must run on macOS (Hackintosh)"
    exit 1
fi

# 检查工具
check_command clang
check_command make
check_command xcrun

# 检查 Phase 2.3 状态
log_info "检查 Phase 2.3 驱动状态..."
if ! kextstat | grep -q "com.rdna4.driver"; then
    log_error "RDNA4Driver not loaded. Phase 2.3 must be working first!"
    exit 1
fi

COMPUTE_STATUS=$(ioreg -l -n RDNA4Driver 2>/dev/null | grep "ComputeTest,Stage" | awk -F'"' '{print $4}')
if [ "$COMPUTE_STATUS" != "passed" ]; then
    log_error "ComputeTest not passed. Phase 2.3 must work first!"
    log_error "Current status: $COMPUTE_STATUS"
    exit 1
fi

log_info "✅ Phase 2.3 驱动正常"

# ============================================================
# 步骤 1: 编译 AIRNT Plugin
# ============================================================

echo ""
echo "Step 1: 编译 AIRNT Plugin..."

cd "$PROJECT_ROOT/navi48-native/airnt-plugin"

if [ ! -f "Makefile" ]; then
    log_error "AIRNT Plugin Makefile not found"
    exit 1
fi

log_info "清理旧编译..."
make clean > /dev/null 2>&1 || true

log_info "编译 AIRNT Plugin..."
if make; then
    log_info "✅ AIRNT Plugin 编译成功"
else
    log_error "AIRNT Plugin 编译失败"
    exit 1
fi

if [ ! -f "build/libAMDNTPlugin.dylib" ]; then
    log_error "libAMDNTPlugin.dylib not found after build"
    exit 1
fi

PLUGIN_SIZE=$(stat -f%z "build/libAMDNTPlugin.dylib")
log_info "Plugin size: $(($PLUGIN_SIZE / 1024)) KB"

# ============================================================
# 步骤 2: 编译 Shader
# ============================================================

echo ""
echo "Step 2: 编译向量加法 Shader..."

cd "$PROJECT_ROOT/RDNA4Driver"

if [ ! -f "compile_shader.sh" ]; then
    log_error "compile_shader.sh not found"
    exit 1
fi

log_info "运行 compile_shader.sh..."
chmod +x compile_shader.sh
if ./compile_shader.sh; then
    log_info "✅ Shader 编译成功"
else
    log_error "Shader 编译失败"
    exit 1
fi

# 验证产物
if [ ! -f "shaders/compiled/vectoradd.metallib" ]; then
    log_error "vectoradd.metallib not found"
    exit 1
fi

if [ ! -f "shaders/compiled/vectoradd_gfx12.bin" ]; then
    log_error "vectoradd_gfx12.bin not found"
    exit 1
fi

BIN_SIZE=$(stat -f%z "shaders/compiled/vectoradd_gfx12.bin")
if [ $BIN_SIZE -lt 100 ]; then
    log_warn "Shader binary suspiciously small ($BIN_SIZE bytes)"
fi

log_info "Shader binary size: $BIN_SIZE bytes"

# 检查是否是 stub
if grep -q "USE_STUB_SHADER" "shaders/compiled/vectoradd_gfx12.h" 2>/dev/null; then
    log_warn "⚠️  WARNING: Using STUB shader!"
    log_warn "   Real compilation may have failed"
fi

# ============================================================
# 步骤 3: 编译 Metal Bundle
# ============================================================

echo ""
echo "Step 3: 编译 Metal Bundle..."

cd "$PROJECT_ROOT/navi48-native/metal-bundle"

if [ ! -f "Makefile" ]; then
    log_error "Metal Bundle Makefile not found"
    exit 1
fi

log_info "清理旧编译..."
make clean > /dev/null 2>&1 || true

log_info "编译 Metal Bundle..."
if make; then
    log_info "✅ Metal Bundle 编译成功"
else
    log_error "Metal Bundle 编译失败"
    exit 1
fi

# 验证产物
BUNDLE_PATH="build/RDNA4Metal.bundle"
if [ ! -d "$BUNDLE_PATH" ]; then
    log_error "RDNA4Metal.bundle not found"
    exit 1
fi

if [ ! -f "$BUNDLE_PATH/Contents/MacOS/RDNA4Metal" ]; then
    log_error "RDNA4Metal library not found in bundle"
    exit 1
fi

if [ ! -f "$BUNDLE_PATH/Contents/MacOS/libAMDNTPlugin.dylib" ]; then
    log_error "libAMDNTPlugin.dylib not found in bundle"
    exit 1
fi

log_info "Bundle structure:"
ls -lh "$BUNDLE_PATH/Contents/MacOS/"

# ============================================================
# 步骤 4: 编译测试 App
# ============================================================

echo ""
echo "Step 4: 编译测试应用..."

log_info "编译 RDNA4MetalTest..."
if make test; then
    log_info "✅ 测试应用编译成功"
else
    log_error "测试应用编译失败"
    exit 1
fi

if [ ! -f "build/test/RDNA4MetalTest" ]; then
    log_error "RDNA4MetalTest executable not found"
    exit 1
fi

# 复制 metallib 到测试目录
log_info "复制 metallib 到测试目录..."
cp "$PROJECT_ROOT/RDNA4Driver/shaders/compiled/vectoradd.metallib" \
   build/test/ || log_warn "Failed to copy metallib"

# ============================================================
# 步骤 5: 部署 Metal Bundle
# ============================================================

echo ""
echo "Step 5: 部署 Metal Bundle 到系统..."

INSTALL_PATH="/Library/GPUBundles/RDNA4Metal.bundle"

# 备份现有版本
if [ -d "$INSTALL_PATH" ]; then
    BACKUP_NAME="RDNA4Metal.bundle.backup-$(date +%Y%m%d-%H%M%S)"
    log_info "备份现有 bundle 到 $BACKUP_NAME"
    sudo cp -R "$INSTALL_PATH" "/Library/GPUBundles/$BACKUP_NAME"
fi

# 安装新版本
log_info "安装新 bundle..."
sudo rm -rf "$INSTALL_PATH"
sudo cp -R "$BUNDLE_PATH" "$INSTALL_PATH"
sudo chown -R root:wheel "$INSTALL_PATH"

log_info "✅ Metal Bundle 已部署到 $INSTALL_PATH"

# 清除缓存
log_info "清除 Metal 缓存..."
sudo rm -rf /System/Library/Caches/com.apple.metal 2>/dev/null || true
sudo touch /Library/GPUBundles/

# ============================================================
# 步骤 6: 运行测试
# ============================================================

echo ""
echo "Step 6: 运行测试..."
echo "========================================"

cd "$PROJECT_ROOT/navi48-native/metal-bundle"

# 运行测试
TEST_OUTPUT=$(./build/test/RDNA4MetalTest 2>&1)
TEST_EXIT_CODE=$?

echo "$TEST_OUTPUT"

echo ""
echo "========================================"

# 分析结果
if [ $TEST_EXIT_CODE -eq 0 ]; then
    if echo "$TEST_OUTPUT" | grep -q "ALL TESTS PASSED"; then
        log_info "✅✅✅ Phase 3.3 测试全部通过！✅✅✅"
        log_info ""
        log_info "恭喜！用户态 Metal 集成完全正常！"
        log_info "任何标准 Metal 应用现在都可以在 RDNA4 GPU 上运行！"
        echo ""
        echo "========================================" 
        echo "  测试结果: 成功 ✅"
        echo "========================================"
        exit 0
    else
        log_warn "测试运行完成但有部分失败"
        exit 1
    fi
else
    log_error "测试执行失败 (exit code: $TEST_EXIT_CODE)"
    
    # 提供诊断信息
    echo ""
    echo "诊断信息:"
    echo "----------------------------------------"
    
    if echo "$TEST_OUTPUT" | grep -q "No Metal device found"; then
        log_error "无法找到 Metal 设备"
        log_info "检查步骤:"
        echo "  1. ioreg -l -w 0 | grep -i rdna4"
        echo "  2. ls -la $INSTALL_PATH"
        echo "  3. sudo dmesg | grep -i metal"
    fi
    
    if echo "$TEST_OUTPUT" | grep -q "Failed to load library"; then
        log_error "无法加载 metallib"
        log_info "检查步骤:"
        echo "  1. ls -la build/test/vectoradd.metallib"
        echo "  2. xxd build/test/vectoradd.metallib | head"
    fi
    
    if echo "$TEST_OUTPUT" | grep -q "libAMDNTPlugin"; then
        log_error "AIRNT Plugin 加载失败"
        log_info "检查步骤:"
        echo "  1. ls -la $INSTALL_PATH/Contents/MacOS/libAMDNTPlugin.dylib"
        echo "  2. otool -L $INSTALL_PATH/Contents/MacOS/libAMDNTPlugin.dylib"
    fi
    
    if echo "$TEST_OUTPUT" | grep -q "FAILED.*elements"; then
        log_error "结果验证失败"
        log_info "可能原因:"
        echo "  1. 使用了 stub shader"
        echo "  2. GPU 未正确执行"
        echo "  3. Buffer 地址配置错误"
        log_info "检查: grep USE_STUB_SHADER $PROJECT_ROOT/RDNA4Driver/shaders/compiled/vectoradd_gfx12.h"
    fi
    
    echo "----------------------------------------"
    echo ""
    log_info "详细测试指南: docs/PHASE3.3_TESTING_GUIDE.md"
    
    exit 1
fi
