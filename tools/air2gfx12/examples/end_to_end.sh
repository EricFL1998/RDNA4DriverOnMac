#!/bin/bash
# air2gfx12 完整示例：从 Xcode Metal 项目到 GFX12 机器码

set -e

echo "==================================================================="
echo "  air2gfx12 End-to-End Example Workflow"
echo "==================================================================="
echo ""

# 步骤 1: 创建示例 Metal Shader
echo "[Step 1] Creating example Metal shader..."
cat > /tmp/example.metal <<'EOF'
#include <metal_stdlib>
using namespace metal;

kernel void vectorAdd(device const float* a [[buffer(0)]],
                      device const float* b [[buffer(1)]],
                      device float* result [[buffer(2)]],
                      uint id [[thread_position_in_grid]])
{
    result[id] = a[id] + b[id];
}

kernel void matrixMultiply(device const float* A [[buffer(0)]],
                          device const float* B [[buffer(1)]],
                          device float* C [[buffer(2)]],
                          constant uint& N [[buffer(3)]],
                          uint2 gid [[thread_position_in_grid]])
{
    uint row = gid.y;
    uint col = gid.x;
    float sum = 0.0f;
    for (uint k = 0; k < N; k++) {
        sum += A[row * N + k] * B[k * N + col];
    }
    C[row * N + col] = sum;
}
EOF

# 步骤 2: 编译为 AIR
echo "[Step 2] Compiling Metal to AIR..."
xcrun -sdk macosx metal -c /tmp/example.metal -o /tmp/example.air
echo "  Created: /tmp/example.air"

# 步骤 3: 链接为 Metallib
echo "[Step 3] Linking AIR to Metallib..."
xcrun -sdk macosx metallib /tmp/example.air -o /tmp/example.metallib
echo "  Created: /tmp/example.metallib"

# 步骤 4: 查看包含的 Kernel
echo "[Step 4] Listing kernels..."
./bin/air2gfx12 /tmp/example.metallib --list

# 步骤 5: 编译为 GFX12 汇编
echo ""
echo "[Step 5] Compiling to GFX12 assembly..."
./bin/air2gfx12 /tmp/example.metallib \
    --kernel "vectorAdd" \
    -o /tmp/vectorAdd.s \
    --stats

echo ""
echo "  Assembly preview (first 30 lines):"
head -30 /tmp/vectorAdd.s

# 步骤 6: 编译为 ELF 目标文件
echo ""
echo "[Step 6] Compiling to ELF object..."
./bin/air2gfx12 /tmp/example.metallib \
    --kernel "vectorAdd" \
    --emit-obj /tmp/vectorAdd.o

file /tmp/vectorAdd.o
ls -lh /tmp/vectorAdd.o

# 步骤 7: 查看 ELF 元数据
echo ""
echo "[Step 7] Inspecting ELF metadata..."
if command -v llvm-readelf >/dev/null 2>&1; then
    llvm-readelf --notes /tmp/vectorAdd.o | head -80
else
    echo "  (llvm-readelf not found, skipping)"
fi

# 步骤 8: 提取纯机器码
echo ""
echo "[Step 8] Extracting raw machine code..."
if command -v llvm-objcopy >/dev/null 2>&1; then
    llvm-objcopy -O binary --only-section=.text /tmp/vectorAdd.o /tmp/vectorAdd.bin
    ls -lh /tmp/vectorAdd.bin
    hexdump -C /tmp/vectorAdd.bin | head -20
else
    echo "  (llvm-objcopy not found, skipping)"
fi

echo ""
echo "==================================================================="
echo "✅ Example workflow complete!"
echo "==================================================================="
echo ""
echo "Generated files:"
echo "  - /tmp/example.metal       : Original Metal source"
echo "  - /tmp/example.air         : AIR intermediate"
echo "  - /tmp/example.metallib    : Metallib container"
echo "  - /tmp/vectorAdd.s         : GFX12 assembly"
echo "  - /tmp/vectorAdd.o         : ELF relocatable object"
echo "  - /tmp/vectorAdd.bin       : Raw machine code"
echo ""
echo "Next steps:"
echo "  - Use PM4Builder to generate command packets"
echo "  - Upload code to GPU VRAM via kernel driver"
echo "  - Configure COMPUTE_PGM_LO/HI registers"
echo "  - Issue DISPATCH_DIRECT packet"
echo ""
