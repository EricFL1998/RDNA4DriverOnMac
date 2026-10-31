# Deployment Instructions for PA_SC_VPORT_SCISSOR Fix

## Files to Deploy to Navi 48 Test Machine

1. **Kernel Extension:**
   - Source: `RDNA4Driver/build/RDNA4Driver.kext`
   - Destination: Replace existing kext on test machine
   - Requires: `sudo kextunload` old version, `sudo kextload` new version

2. **Metal Bundle:**
   - Source: `navi48-native/metal-bundle/build/RDNA4Metal.bundle`
   - Destination: `/Library/GPUBundles/RDNA4Metal.bundle`

## Testing Steps

Run the TriangleTest on the target Navi 48 hardware:

```bash
cd navi48-native/metal-bundle
build/test/TriangleTest
```

## Expected Results (Success Criteria)

### Before Fix:
- ✅ Vertex shader probe: vb[16..27] shows 3 vertex positions
- ✅ Primitive export probe: vb[24] = 0x11111111
- ❌ Pixel shader probe: vb[32] = 0x00000000 (never ran)
- ❌ Framebuffer: 100% clear color 0xFF333333

### After Fix:
- ✅ Vertex shader probe: vb[16..27] shows 3 vertex positions
- ✅ Primitive export probe: vb[24] = 0x11111111
- ✅ **Pixel shader probe: vb[32] = 0xCAFEBABE** (executed!)
- ✅ **Framebuffer: Contains rendered triangle pixels**

## What Changed

Added PA_SC_VPORT_SCISSOR_0 initialization to graphics preamble in `GraphicsPM4Builder.cpp`:

```cpp
{ GFX12::PA_SC_VPORT_SCISSOR_0_TL, 0x00000000u },
{ GFX12::PA_SC_VPORT_SCISSOR_0_BR, kScissorFull },  // 0xFFFFFFFF
```

This ensures the viewport scissor defaults to full-screen instead of (0,0)-(0,0), which was blocking all rasterization.

## Rollback Plan

If the fix causes issues:
1. Restore previous kext and bundle from backup
2. Revert commit in Git: `git revert HEAD`
3. Rebuild and redeploy

## Next Steps After Verification

If successful, this proves:
1. PA/SC rasterizer now generates fragments
2. SPI successfully dispatches pixel waves
3. Complete vertex → primitive → rasterization → pixel shader pipeline works

Then proceed to verify:
- Correct triangle shape and color
- Multiple primitives
- Different viewport sizes
- Indexed rendering
