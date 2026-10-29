#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$DIR/.."

echo "======================================================================"
echo "          PHASE 1: NATIVE METAL TOOLCHAIN FULL VERIFICATION           "
echo "======================================================================"
echo "Target Architecture: AMD Radeon RX 9070 XT (Navi 48 / GFX1201)"
echo "Toolchain: Homebrew LLVM 23.1.4 (AMDGPU Target Backend)"
echo "Workspace: $ROOT"
echo "Date: $(date)"
echo "======================================================================"
echo ""

STEP=1

run_step() {
    local NAME="$1"
    local CMD="$2"
    echo "[$STEP/8] Running: $NAME ..."
    if eval "$CMD"; then
        echo "  --> ✅ PASS: $NAME"
    else
        echo "  --> ❌ FAIL: $NAME"
        exit 1
    fi
    echo ""
    STEP=$((STEP + 1))
}

# 1. Clean build
run_step "Clean and Rebuild air2gfx12 Toolchain & Test Suite" \
    "cd '$ROOT' && make clean && make all && make build_tests"

# 2. MTLB Unpacker Unit Test
run_step "Unit Test: MTLB Container Unpacker" \
    "'$ROOT/bin/test_unpacker'"

# 3. AIR Runtime Linker Unit Test
run_step "Unit Test: AIR Linker & Symbol Resolution" \
    "'$ROOT/bin/test_linker'"

# 4. GFX12 Emitter Unit Test
run_step "Unit Test: GFX1201 CodeGen Target Machine" \
    "'$ROOT/bin/test_emitter'"

# 5. Metadata & Register Calculation Unit Test
run_step "Unit Test: Shader Metadata & GFX12 Registers" \
    "'$ROOT/bin/test_metadata'"

# 6. PM4 Packet Stream Builder Unit Test
run_step "Unit Test: GFX12 PM4 Type-3 Command Builder" \
    "'$ROOT/bin/test_pm4'"

# 7. CLI & End-to-End Test
run_step "Functional Test: CLI & Single-Kernel End-to-End" \
    "bash '$ROOT/tests/test_cli.sh' && bash '$ROOT/tests/e2e_test.sh'"

# 8. Batch Golden Shaders Stress Test (45 production shaders)
run_step "Stress Test: 45 Production Metal Shaders Batch Compilation" \
    "bash '$ROOT/tests/batch_golden_test.sh'"

echo "======================================================================"
echo "🏆 PHASE 1 FULL VERIFICATION SCORECARD: 8/8 CHECKS PASSED (100%)"
echo "======================================================================"
echo "Deliverables Ready:"
echo "  1. CLI Compiler:         $ROOT/bin/air2gfx12"
echo "  2. Runtime Library:      $ROOT/assets/runtime.amdil.bc"
echo "  3. Production Test Base: $ROOT/test_assets/TextureProcessorMetal.metallib"
echo "  4. PM4 Packet Builder:   $ROOT/src/PM4Builder.hpp/cpp"
echo "  5. Metadata Analyzer:    $ROOT/src/ShaderMetadata.hpp/cpp"
echo "======================================================================"
