#include <metal_stdlib>
using namespace metal;

/**
 * vectorAdd - Simple vector addition kernel for Phase 2.3 testing
 * 
 * Computes: result[i] = a[i] + b[i]
 * 
 * Arguments:
 *   a      - Input buffer A (read-only)
 *   b      - Input buffer B (read-only)
 *   result - Output buffer (write-only)
 *   
 * Thread layout:
 *   1D grid, 1 thread per element
 */
kernel void vectorAdd(
    device const float* a [[buffer(0)]],
    device const float* b [[buffer(1)]],
    device float* result [[buffer(2)]],
    uint id [[thread_position_in_grid]])
{
    result[id] = a[id] + b[id];
}
