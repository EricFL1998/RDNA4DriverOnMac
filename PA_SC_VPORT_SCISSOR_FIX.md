# PA_SC_VPORT_SCISSOR Fix for Navi 48 Rasterization Issue

## Problem Summary
Vertex shaders executed successfully and exported coordinates, but the hardware rasterizer (PA/SC) never generated any fragments, resulting in zero pixel shader dispatches despite correct primitive export.

## Root Cause (Systematic Debugging Phase 1)
Through systematic investigation following the debugging skill:

1. **Evidence gathered:**
   - Vertices exported correctly: vb[16..27] showed all 3 vertex positions
   - Primitive export executed: vb[24] = 0x11111111 confirmed `exp prim` ran
   - Pixel shader never invoked: vb[32] = 0x00000000 (PS probe never wrote)
   - Framebuffer remained 100% clear color

2. **Configuration verified:**
   - Viewport: 800×600 ✓
   - Scissor: 800×600 ✓  
   - PA_CL_VTE_CNTL: 0x43F (transform enabled) ✓
   - Render target: BGRA8, LINEAR_GENERAL, correct pitch ✓
   - CB_COLOR_CONTROL: enabled via featureMask bit 2 ✓

3. **Critical finding:**
   - Preamble enables `VPORT_SCISSOR_ENABLE` in PA_SC_MODE_CNTL_0 (value 0x22, bit 1 set)
   - Preamble initializes PA_SC_WINDOW_SCISSOR and PA_SC_GENERIC_SCISSOR to full range
   - **But preamble does NOT initialize PA_SC_VPORT_SCISSOR_0_TL/BR**
   - GFX12 has no CLEAR_STATE packet, so registers hold reset value (0,0)-(0,0)
   - Result: Zero-area viewport scissor clips ALL fragments before PS dispatch

## Fix Applied
Added PA_SC_VPORT_SCISSOR_0 initialization to preamble table in `GraphicsPM4Builder.cpp`:

```cpp
// PA_SC_MODE_CNTL_0.VPORT_SCISSOR_ENABLE gates fragments through the viewport
// scissor; without initializing these registers the hardware clips to the reset
// range (0,0)-(0,0) and drops every fragment before the pixel shader runs.
{ GFX12::PA_SC_VPORT_SCISSOR_0_TL, 0x00000000u },
{ GFX12::PA_SC_VPORT_SCISSOR_0_BR, kScissorFull },
```

This matches the pattern used for PA_SC_WINDOW_SCISSOR and PA_SC_GENERIC_SCISSOR.

## Files Changed
- `RDNA4Driver/src/hardware/GraphicsPM4Builder.cpp` (lines 479-480 added)
- Rebuilt: `RDNA4Driver.kext` and `RDNA4Metal.bundle`

## Testing Required
Deploy updated kext and bundle to Navi 48 test machine and run TriangleTest to verify:
1. Pixel shader probe shows execution (vb[32] != 0)
2. Framebuffer contains rendered triangle pixels
3. No regression in vertex shader execution

## Date
2026-10-31
