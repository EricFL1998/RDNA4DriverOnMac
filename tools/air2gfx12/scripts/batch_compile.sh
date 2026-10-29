#!/bin/bash
set -e

# air2gfx12 批量编译工具
# 用途: 将 metallib 中的所有 kernel 批量编译为 GFX12 汇编与 ELF

METALLIB="$1"
OUT_DIR="${2:-output}"
RUNTIME="${3:-assets/runtime.amdil.bc}"

if [ -z "$METALLIB" ]; then
    echo "Usage: $0 <input.metallib> [output_dir] [runtime.amdil.bc]"
    exit 1
fi

if [ ! -f "$METALLIB" ]; then
    echo "Error: $METALLIB not found"
    exit 1
fi

BIN="$(dirname "$0")/../bin/air2gfx12"
if [ ! -f "$BIN" ]; then
    echo "Error: air2gfx12 binary not found at $BIN"
    echo "Please run 'make' first"
    exit 1
fi

mkdir -p "$OUT_DIR"

echo "==================================================================="
echo "    air2gfx12 Batch Compiler"
echo "==================================================================="
echo "Input:   $METALLIB"
echo "Output:  $OUT_DIR"
echo "Runtime: $RUNTIME"
echo "==================================================================="
echo ""

# Extract kernel names
KERNELS=$("$BIN" "$METALLIB" --list | grep "^\[" | awk '{print $2}')
COUNT=$(echo "$KERNELS" | wc -l | tr -d ' ')

echo "Found $COUNT kernels to compile."
echo ""

COMPILED=0
FAILED=0

for KERNEL in $KERNELS; do
    printf "  [%3d/%3d] Compiling %-40s ... " "$((COMPILED + FAILED + 1))" "$COUNT" "$KERNEL"
    
    if "$BIN" "$METALLIB" --runtime "$RUNTIME" --kernel "$KERNEL" \
        -o "$OUT_DIR/${KERNEL}.s" \
        --emit-obj "$OUT_DIR/${KERNEL}.o" \
        > "$OUT_DIR/${KERNEL}.log" 2>&1; then
        echo "✅"
        COMPILED=$((COMPILED + 1))
    else
        echo "❌"
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo "==================================================================="
echo "Summary: $COMPILED compiled, $FAILED failed"
echo "==================================================================="

if [ "$FAILED" -gt 0 ]; then
    echo "Check logs in $OUT_DIR/*.log for details"
    exit 1
fi
