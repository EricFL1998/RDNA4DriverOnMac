#!/bin/bash
set -e

# air2gfx12 统计分析工具
# 用途: 分析编译产物的资源占用分布

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <metallib> [output_csv]"
    exit 1
fi

METALLIB="$1"
OUTPUT_CSV="${2:-shader_stats.csv}"
BIN="$(dirname "$0")/../bin/air2gfx12"

if [ ! -f "$BIN" ]; then
    echo "Error: air2gfx12 not found"
    exit 1
fi

echo "Analyzing $METALLIB ..."
echo ""

# Write CSV header
echo "Kernel,Type,SGPRs,VGPRs,Wave,ScratchBytes,KernargBytes,RSRC1,RSRC2" > "$OUTPUT_CSV"

# Extract kernel list
KERNELS=$("$BIN" "$METALLIB" --list | grep "^\[")

while IFS= read -r LINE; do
    KERNEL=$(echo "$LINE" | awk '{print $2}')
    TYPE=$(echo "$LINE" | awk '{print $3}' | tr -d '(,')
    
    printf "  Compiling %-40s ... " "$KERNEL"
    
    # Compile and capture stats
    STATS=$("$BIN" "$METALLIB" --kernel "$KERNEL" \
        -o "/tmp/${KERNEL}.s" --stats 2>&1 | grep "\[Stats\]" || true)
    
    if [ -n "$STATS" ]; then
        # Parse: [Stats] Kernel: X | SGPRs: Y | VGPRs: Z | Wave: W | Scratch: NB | GFX12 RSRC1: 0xHEX RSRC2: 0xHEX
        SGPR=$(echo "$STATS" | grep -oP 'SGPRs: \K\d+' || echo "0")
        VGPR=$(echo "$STATS" | grep -oP 'VGPRs: \K\d+' || echo "0")
        WAVE=$(echo "$STATS" | grep -oP 'Wave: \K\d+' || echo "32")
        SCRATCH=$(echo "$STATS" | grep -oP 'Scratch: \K\d+' || echo "0")
        RSRC1=$(echo "$STATS" | grep -oP 'RSRC1: \K0x[0-9a-f]+' || echo "0x0")
        RSRC2=$(echo "$STATS" | grep -oP 'RSRC2: \K0x[0-9a-f]+' || echo "0x0")
        
        # Extract kernarg from assembly
        KERNARG=$(grep ".amdhsa_kernarg_size" "/tmp/${KERNEL}.s" 2>/dev/null | awk '{print $2}' || echo "0")
        
        echo "$KERNEL,$TYPE,$SGPR,$VGPR,$WAVE,$SCRATCH,$KERNARG,$RSRC1,$RSRC2" >> "$OUTPUT_CSV"
        echo "✅ (SGPR:$SGPR VGPR:$VGPR)"
    else
        echo "❌"
        echo "$KERNEL,$TYPE,,,,,,,," >> "$OUTPUT_CSV"
    fi
done <<< "$KERNELS"

echo ""
echo "=== Resource Usage Summary ==="
echo ""

# Analyze distribution
python3 -c "
import csv
import sys

with open('$OUTPUT_CSV', 'r') as f:
    reader = csv.DictReader(f)
    rows = [r for r in reader if r['SGPRs']]

if not rows:
    print('No valid data')
    sys.exit(0)

sgprs = [int(r['SGPRs']) for r in rows if r['SGPRs']]
vgprs = [int(r['VGPRs']) for r in rows if r['VGPRs']]
scratch = [int(r['ScratchBytes']) for r in rows if r['ScratchBytes']]

print(f'Total kernels: {len(rows)}')
print()
print('SGPRs:   min={:3d}  max={:3d}  avg={:.1f}'.format(min(sgprs), max(sgprs), sum(sgprs)/len(sgprs)))
print('VGPRs:   min={:3d}  max={:3d}  avg={:.1f}'.format(min(vgprs), max(vgprs), sum(vgprs)/len(vgprs)))
print('Scratch: min={:3d}B max={:3d}B avg={:.1f}B'.format(min(scratch), max(scratch), sum(scratch)/len(scratch)))
print()
print('By type:')
types = {}
for r in rows:
    t = r['Type']
    types[t] = types.get(t, 0) + 1
for t, c in sorted(types.items()):
    print(f'  {t:10s}: {c:2d} kernels')
"

echo ""
echo "Detailed results saved to: $OUTPUT_CSV"
echo ""
echo "You can open it with: open $OUTPUT_CSV"
