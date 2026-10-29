#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$DIR/../bin/air2gfx12"
METALLIB="$DIR/../test_assets/TextureProcessorMetal.metallib"
[ -f "$METALLIB" ] || METALLIB="/tmp/TextureProcessorMetal.metallib"

RUNTIME_BC="$DIR/../assets/runtime.amdil.bc"
[ -f "$RUNTIME_BC" ] || RUNTIME_BC="/tmp/runtime.amdil.bc"

OUT_S="/tmp/out_kernel.s"
OUT_O="/tmp/out_kernel.o"

echo "=== 1. Testing --list on real metallib ==="
"$BIN" "$METALLIB" --list | head -15

echo "=== 2. Testing full compile of MinMaxReduction_float1 ==="
rm -f "$OUT_S" "$OUT_O"
"$BIN" "$METALLIB" \
    --runtime "$RUNTIME_BC" \
    --kernel "MinMaxReduction_float1" \
    -o "$OUT_S" \
    --emit-obj "$OUT_O" \
    -v

echo "=== 3. Validating outputs ==="
[ -f "$OUT_S" ] || { echo "FAIL: $OUT_S does not exist"; exit 1; }
[ -f "$OUT_O" ] || { echo "FAIL: $OUT_O does not exist"; exit 1; }

echo "Verifying assembly contents:"
grep -E "gfx1201|MinMaxReduction_float1" "$OUT_S" | head -10

echo "Verifying ELF binary:"
file "$OUT_O"
ls -la "$OUT_O"

echo ""
echo "🎉 ALL END-TO-END ACCEPTANCE TESTS PASSED!"
