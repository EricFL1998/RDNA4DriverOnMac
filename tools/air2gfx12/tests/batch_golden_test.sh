#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$DIR/../bin/air2gfx12"
METALLIB="$DIR/../test_assets/TextureProcessorMetal.metallib"
RUNTIME_BC="$DIR/../assets/runtime.amdil.bc"
OUT_DIR="/tmp/air2gfx12_golden_output"

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/*

echo "=========================================================="
echo "      Phase 1.2: Batch Golden Shader Verification Suite   "
echo "=========================================================="

echo "Parsing metallib: $METALLIB"
KERNELS=$("$BIN" "$METALLIB" --list | grep "\\[" | awk '{print $2}')
COUNT=$(echo "$KERNELS" | wc -l | tr -d ' ')

echo "Discovered $COUNT shaders to compile."
echo "----------------------------------------------------------"

PASSED=0
FAILED=0
START_TIME=$(date +%s)

for KERNEL in $KERNELS; do
    printf "  Compiling [%-35s] ... " "$KERNEL"
    OUT_S="$OUT_DIR/${KERNEL}.s"
    OUT_O="$OUT_DIR/${KERNEL}.o"
    
    if "$BIN" "$METALLIB" --runtime "$RUNTIME_BC" --kernel "$KERNEL" -o "$OUT_S" --emit-obj "$OUT_O" > /dev/null 2>&1; then
        # Verify output exists and is non-empty
        if [ -s "$OUT_S" ] && [ -s "$OUT_O" ]; then
            # Verify target triple in assembly
            if grep -q "gfx1201" "$OUT_S"; then
                echo "✅ PASS"
                PASSED=$((PASSED + 1))
            else
                echo "❌ FAIL (Missing gfx1201 target in .s)"
                FAILED=$((FAILED + 1))
            fi
        else
            echo "❌ FAIL (Empty output files)"
            FAILED=$((FAILED + 1))
        fi
    else
        echo "❌ FAIL (Compiler crash / error)"
        FAILED=$((FAILED + 1))
    fi
done

END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

echo "=========================================================="
echo "Summary: Total: $COUNT | Passed: $PASSED | Failed: $FAILED | Time: ${DURATION}s"
echo "=========================================================="

if [ "$FAILED" -eq 0 ]; then
    echo "🎉 100% GOLDEN SHADER TESTS PASSED!"
    exit 0
else
    echo "⚠️ Some shaders failed compilation!"
    exit 1
fi
