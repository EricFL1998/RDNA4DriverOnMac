//
//  GraphicsPM4Builder.cpp
//  RDNA4Driver - PM4 encoder for the native Metal graphics path
//

#include "GraphicsPM4Builder.hpp"
// The debug hooks below read the environment with getenv(), which exists only
// in the user-space bundle build. The kext compiles this file with -DKERNEL.
#ifndef KERNEL
#include <stdlib.h>
#define N48_ENV(name) getenv(name)
#else
#define N48_ENV(name) nullptr
#endif

namespace {

// PM4 type-3 packets carry "payload dwords minus one" in the count field, and
// the payload of a SET_*_REG packet is the block offset plus the values.
inline uint32_t floatBits(float v) {
    union { float f; uint32_t u; } bits;
    bits.f = v;
    return bits.u;
}

struct RegSpace {
    uint32_t opcode;
    uint32_t baseDwords;
};

bool spaceFor(uint32_t addr, RegSpace* out) {
    if (addr >= GFX12::kContextBase && addr < GFX12::kContextEnd) {
        out->opcode = GFX12::PKT3_SET_CONTEXT_REG;
        out->baseDwords = GFX12::kContextBase >> 2;
        return true;
    }
    if (addr >= GFX12::kShBase && addr < GFX12::kShEnd) {
        out->opcode = GFX12::PKT3_SET_SH_REG;
        out->baseDwords = GFX12::kShBase >> 2;
        return true;
    }
    if (addr >= GFX12::kUconfigBase && addr < GFX12::kUconfigEnd) {
        out->opcode = GFX12::PKT3_SET_UCONFIG_REG;
        out->baseDwords = GFX12::kUconfigBase >> 2;
        return true;
    }
    return false;
}

} // namespace

GraphicsPM4Builder::GraphicsPM4Builder() : count(0), overflow(false) {}

void GraphicsPM4Builder::reset() {
    count = 0;
    overflow = false;
}

const uint32_t* GraphicsPM4Builder::getPackets(size_t* outDwords) const {
    if (outDwords) *outDwords = count;
    return packets;
}

uint32_t GraphicsPM4Builder::type3Header(uint32_t opcode, uint32_t payloadDwords) {
    if (payloadDwords == 0) return 0;
    return (3u << 30) | ((payloadDwords - 1u) << 16) | (opcode << 8);
}

bool GraphicsPM4Builder::appendDword(uint32_t value) {
    if (count >= kCapacityDwords) {
        overflow = true;
        return false;
    }
    packets[count++] = value;
    return true;
}

bool GraphicsPM4Builder::appendRegPacket(uint32_t addr, const uint32_t* values,
                                         size_t valueCount) {
    RegSpace space;
    if (!spaceFor(addr, &space)) return false;
    if (!values || valueCount == 0) return false;
    // A packet cannot straddle the end of its block: the offset field plus the
    // values all have to describe consecutive registers.
    const uint32_t offset = (addr >> 2) - space.baseDwords;
    const uint32_t blockDwords = (space.opcode == GFX12::PKT3_SET_CONTEXT_REG)
        ? ((GFX12::kContextEnd - GFX12::kContextBase) >> 2)
        : (space.opcode == GFX12::PKT3_SET_SH_REG)
            ? ((GFX12::kShEnd - GFX12::kShBase) >> 2)
            : ((GFX12::kUconfigEnd - GFX12::kUconfigBase) >> 2);
    if (offset + valueCount > blockDwords) return false;

    if (!appendDword(type3Header(space.opcode, static_cast<uint32_t>(valueCount) + 1u))) {
        return false;
    }
    if (!appendDword(offset)) return false;
    for (size_t i = 0; i < valueCount; i++) {
        if (!appendDword(values[i])) return false;
    }
    return true;
}

bool GraphicsPM4Builder::setReg(uint32_t addr, uint32_t value) {
    return appendRegPacket(addr, &value, 1);
}

bool GraphicsPM4Builder::setRegSeq(uint32_t addr, const uint32_t* values,
                                  size_t valueCount) {
    return appendRegPacket(addr, values, valueCount);
}

bool GraphicsPM4Builder::setViewport(float originX, float originY, float width,
                                     float height, float znear, float zfar) {
    // Metal's viewport maps onto the PA_CL_VPORT transform: the hardware wants
    // scale/offset pairs and Y is inverted because Metal's origin is top-left.
    const float xscale = width * 0.5f;
    const float xoffset = originX + xscale;
    const float yscale = -height * 0.5f;
    const float yoffset = originY + height * 0.5f;
    const float zscale = zfar - znear;
    const float zoffset = znear;

    const uint32_t values[8] = {
        floatBits(xscale), floatBits(xoffset),
        floatBits(yscale), floatBits(yoffset),
        floatBits(zscale), floatBits(zoffset),
        floatBits(znear), floatBits(zfar),
    };
    return setRegSeq(GFX12::PA_CL_VPORT_XSCALE, values, 8);
}

bool GraphicsPM4Builder::setScissorRect(uint32_t x, uint32_t y,
                                        uint32_t width, uint32_t height) {
    constexpr uint32_t limit = 0x10000u;
    if (width == 0 || height == 0 || x >= limit || y >= limit ||
        width > limit - x || height > limit - y) return false;
    const uint32_t tl = (y << 16) | x;
    const uint32_t br = ((y + height - 1) << 16) | (x + width - 1);
    if (!setReg(GFX12::PA_SC_WINDOW_SCISSOR_TL, tl)) return false;
    if (!setReg(GFX12::PA_SC_VPORT_SCISSOR_0_TL, tl)) return false;
    if (!setReg(GFX12::PA_SC_WINDOW_SCISSOR_BR, br)) return false;
    return setReg(GFX12::PA_SC_VPORT_SCISSOR_0_BR, br);
}

bool GraphicsPM4Builder::setColorAttachment(uint32_t index, uint64_t gpuAddr,
                                            uint32_t width, uint32_t height,
                                            uint32_t cbFormat, uint32_t sampleCount,
                                            uint32_t componentSwap) {
    if (index >= 8) return false;
#ifndef KERNEL
    // Probe: N48_CB_BASE points the colour target somewhere else, so a run can
    // tell "no fragments" from "fragments the CB refused to write".
    {
        const char* overrideBase = N48_ENV("N48_CB_BASE");
        if (overrideBase && *overrideBase) gpuAddr = strtoull(overrideBase, nullptr, 0);
    }
#endif
    // MSAA needs the fragment count and the tile fields of ATTRIB; only the
    // single-sample encoding is verified here.
    if (sampleCount != 1) return false;
    if (width == 0 || height == 0 || width > 0x10000u || height > 0x10000u) return false;

    const uint32_t base = GFX12::CB_COLOR0_BASE + index * GFX12::CB_COLOR_TARGET_STRIDE;
    const uint32_t view = GFX12::CB_COLOR0_VIEW + index * GFX12::CB_COLOR_TARGET_STRIDE;
    const uint32_t view2 = GFX12::CB_COLOR0_VIEW2 + index * GFX12::CB_COLOR_TARGET_STRIDE;
    const uint32_t attrib = GFX12::CB_COLOR0_ATTRIB + index * GFX12::CB_COLOR_TARGET_STRIDE;
    const uint32_t fdcc = GFX12::CB_COLOR0_FDCC_CONTROL + index * GFX12::CB_COLOR_TARGET_STRIDE;
    const uint32_t attrib2 = GFX12::CB_COLOR0_ATTRIB2 + index * GFX12::CB_COLOR_TARGET_STRIDE;
    const uint32_t attrib3 = GFX12::CB_COLOR0_ATTRIB3 + index * GFX12::CB_COLOR_TARGET_STRIDE;
    const uint32_t baseExt = GFX12::CB_COLOR0_BASE_EXT + index * GFX12::CB_COLOR_INFO_STRIDE;
    const uint32_t info = GFX12::CB_COLOR0_INFO + index * GFX12::CB_COLOR_INFO_STRIDE;

    // CB_COLOR0_BASE holds bits [39:8] of the address, BASE_EXT bits [47:40].
    if (!setReg(base, static_cast<uint32_t>(gpuAddr >> 8))) return false;
    if (!setReg(view, 0)) return false;                    // SLICE_START/SLICE_MAX
    if (!setReg(view2, 0)) return false;                   // MIP0_DEPTH
    if (!setReg(attrib, 0)) return false;                  // no MSAA, no force-alpha
    // 0x10000004 = MAX_UNCOMPRESSED_BLOCK_SIZE(1) | ENABLE_MAX_COMP_FRAG_OVERRIDE(1)
    // Required for uncompressed surfaces on GFX12 (Cobalt64 XLAT12_CB0_FDCC_CONTROL).
    if (!setReg(fdcc, 0x10000004u)) return false;
    if (!setReg(attrib2, ((width - 1) << 16) | (height - 1))) return false;
    // ATTRIB3 carries the swizzle mode: COLOR_SW_MODE 0 is ADDR3_LINEAR, with
    // one layer and a 2D resource (ac_init_gfx12_cb_surface). Neither this nor
    // BASE_EXT was written before, so both held power-on values and the first
    // fragments the CB ever received faulted at an address whose low 40 bits
    // were the target's and whose top byte was random.
    if (!setReg(attrib3, 1u | GFX12::CB_ATTRIB3_RESOURCE_2D)) return false;
    if (!setReg(baseExt, static_cast<uint32_t>(gpuAddr >> 40) & 0xFFu)) return false;
    // LINEAR_GENERAL (bit 7 = 0x80) tells the CB this is a linear pitch surface,
    // not a tiled one. Without it the CB treats the buffer as tiled, writing
    // pixels to scrambled addresses or dropping them completely.
    if (!setReg(info, (cbFormat & 0x1Fu) | (1u << 7) | ((componentSwap & 0x3u) << 11))) return false;
    // MODE lives in CB_COLOR_CONTROL and resets to CB_DISABLE, which throws away
    // every colour write no matter what the shaders do. One mode for all eight
    // targets - what radeonsi writes for a plain colour pass.
    if (featureMask & 4u) {
        // MODE lives in CB_COLOR_CONTROL and resets to CB_DISABLE, which throws away
        // every colour write no matter what the shaders do. One mode for all eight
        // targets - what radeonsi writes for a plain colour pass.
        // MODE=NORMAL (bits [4:6] = 1), ROP3=COPY (bits [16:23] = 0xCC, 204).
        // Leaving ROP3 at 0 executes ROP3_CLEAR, which zeroes out every pixel written
        // to the render target regardless of what the fragment shader outputs!
        // Both radeonsi (si_state.c:368) and radv (radv_cmd_buffer.c:13476) write 0xCC.
        const uint32_t cbColorControl = (0xCCu << 16) | (GFX12::CB_MODE_NORMAL << 4);
        if (!setReg(GFX12::CB_COLOR_CONTROL, cbColorControl)) return false;
        // The matching SPI export format: the fragment shader emits `exp mrt0`
        // as four 32-bit floats, so COL_FORMAT must be 32_ABGR (9). The POS
        // enum value 4 read here as FP16_ABGR and the CB then unpacked the
        // export as packed half floats, writing nothing usable.
        if (!setReg(GFX12::SPI_SHADER_COL_FORMAT, GFX12::SPI_SHADER_COL_32_ABGR)) return false;
        // Explicitly enable color writes for target 0 (4 components)
        if (!setReg(GFX12::CB_TARGET_MASK, 0x0000000Fu)) return false;
        if (!setReg(GFX12::CB_SHADER_MASK, 0x0000000Fu)) return false;
    }
#ifndef KERNEL
    // Fragment-existence probe: with N48_DB_PROBE the depth surface is pointed at
    // the colour target and Z writes are enabled, so every fragment the
    // rasterizer produces stamps a depth value into the framebuffer. Pixels
    // appearing then prove SC produced fragments even when the PS never runs.
    if (N48_ENV("N48_DB_PROBE")) {
        const uint64_t z = gpuAddr;
        if (!setReg(0x28014u, ((height - 1) << 16) | (width - 1))) return false; // DB_DEPTH_SIZE_XY
        if (!setReg(0x28018u, 3u)) return false;      // DB_Z_INFO: Z_32_FLOAT, 1 sample
        if (!setReg(0x28020u, static_cast<uint32_t>(z >> 8))) return false;     // DB_Z_READ_BASE
        if (!setReg(0x28024u, static_cast<uint32_t>(z >> 40))) return false;    // DB_Z_READ_BASE_HI
        if (!setReg(0x28028u, static_cast<uint32_t>(z >> 8))) return false;     // DB_Z_WRITE_BASE
        if (!setReg(0x2802cu, static_cast<uint32_t>(z >> 40))) return false;    // DB_Z_WRITE_BASE_HI
        if (!setReg(0x28004u, 0u)) return false;                                // DB_DEPTH_VIEW
        if (!setReg(0x28070u, 0x76u)) return false;   // Z_ENABLE | Z_WRITE_ENABLE | ZFUNC_ALWAYS
    }
#endif
    return true;
}

bool GraphicsPM4Builder::setColorWriteMask(uint32_t targetMask, uint32_t shaderMask) {
    if (!setReg(GFX12::CB_TARGET_MASK, targetMask)) return false;
    return setReg(GFX12::CB_SHADER_MASK, shaderMask);
}

bool GraphicsPM4Builder::setBlendState(uint32_t index, bool enable,
                                       uint32_t srcColor, uint32_t dstColor, uint32_t colorOp,
                                       uint32_t srcAlpha, uint32_t dstAlpha, uint32_t alphaOp) {
    if (index >= 8) return false;
    if (!enable) {
        // Disabled blend is expressed as ONE/ZERO/ADD, there is no enable bit.
        srcColor = dstColor = srcAlpha = dstAlpha = GFX12::BLEND_ONE;
        dstColor = dstAlpha = GFX12::BLEND_ZERO;
        colorOp = alphaOp = GFX12::COMB_ADD;
    }
    const uint32_t value =
        ((srcColor & 0x1Fu) << GFX12::BLEND_COLOR_SRC_SHIFT) |
        ((colorOp & 0x7u) << GFX12::BLEND_COLOR_FCN_SHIFT) |
        ((dstColor & 0x1Fu) << GFX12::BLEND_COLOR_DST_SHIFT) |
        ((srcAlpha & 0x1Fu) << GFX12::BLEND_ALPHA_SRC_SHIFT) |
        ((alphaOp & 0x7u) << GFX12::BLEND_ALPHA_FCN_SHIFT) |
        ((dstAlpha & 0x1Fu) << GFX12::BLEND_ALPHA_DST_SHIFT);
    return setReg(GFX12::CB_BLEND0_CONTROL + index * GFX12::CB_BLEND_STRIDE, value);
}

bool GraphicsPM4Builder::setCullMode(uint32_t cullFront, uint32_t cullBack,
                                     bool frontFaceCW) {
    uint32_t value = 0;
    if (cullFront) value |= GFX12::SC_CULL_FRONT;
    if (cullBack) value |= GFX12::SC_CULL_BACK;
    if (frontFaceCW) value |= GFX12::SC_FACE_CW;
    return setReg(GFX12::PA_SU_SC_MODE_CNTL, value);
}

bool GraphicsPM4Builder::setDepthDisabled() {
    if (!setReg(GFX12::DB_DEPTH_CONTROL, 0)) return false;
    return setReg(GFX12::DB_RENDER_CONTROL, 0);
}

bool GraphicsPM4Builder::setVertexShader(uint64_t gpuAddr, uint32_t gsRsrc1,
                                         uint32_t gsRsrc2) {
    const uint32_t lo = static_cast<uint32_t>(gpuAddr >> 8);
    const uint32_t hi = static_cast<uint32_t>((gpuAddr >> 40) & 0xFFu);

    // LO_GS/HI_GS are adjacent, RSRC1_GS/RSRC2_GS are adjacent, but LO_ES and
    // HI_ES sit in neither run: three packets, not one sequence.
    const uint32_t gsProgram[2] = { lo, hi };
    if (!setRegSeq(GFX12::SPI_SHADER_PGM_LO_GS, gsProgram, 2)) return false;
    if (!setReg(GFX12::SPI_SHADER_PGM_LO_ES, lo)) return false;
    if (!setReg(GFX12::SPI_SHADER_PGM_HI_ES, hi)) return false;

    const uint32_t gsResources[2] = { gsRsrc1, gsRsrc2 };
    if (!setRegSeq(GFX12::SPI_SHADER_PGM_RSRC1_GS, gsResources, 2)) return false;
    // RSRC3_GS carries the GS/ES CU enable mask. gfx12 has no CLEAR_STATE, so
    // this register holds whatever the last submission left - including the
    // all-zero reset value, which enables no CU at all and leaves SPI with
    // nowhere to dispatch a vertex wave. Both Mesa drivers program it in the
    // gfx12 graphics preamble (ac_cmdbuf.c gfx12_init_graphics_preamble_state,
    // radeonsi si_state_shaders.cpp); 0xfffffdfd is the Navi48 mask with the
    // one-CU-per-SA harvest this card reports.
    if (!setReg(GFX12::SPI_SHADER_PGM_RSRC3_GS, GFX12::CU_EN_GS_ALL)) return false;
    // pos0 is exported as 4 components; leaving this at NONE makes the SPI drop
    // the export, so nothing ever reaches the rasterizer.
    return setReg(GFX12::SPI_SHADER_POS_FORMAT, GFX12::SPI_SHADER_4COMP);
}

bool GraphicsPM4Builder::setVertexInputs(uint64_t vertexBuffer, uint32_t stride,
                                         uint64_t ringBuffer, uint32_t ringSize,
                                         uint32_t parameterCount, uint32_t vertexCount,
                                         uint32_t primitiveCount) {
    if (!vertexBuffer || !ringBuffer || !stride || !vertexCount || parameterCount > 32 ||
        (ringBuffer & 0xFFFFu) || (ringSize & 0xFFFFu) || ringSize < 65536) return false;
    const uint32_t throttle[4] = {
        0x12355123u, 0x0001544Du, static_cast<uint32_t>(ringBuffer >> 16),
        ((ringSize >> 16) - 1u) | (1u << 17),
    };
    if (!setRegSeq(0x31110u, throttle, 4)) return false;

    // Ring layout: the attribute ring owns the first 0x580000 bytes (0x160000
    // per SE), the position ring the next 0x400000 and the primitive ring the
    // last 0x100000. Sizing the position ring off the per-SE attribute size -
    // 0x160000 - puts it inside the attribute ring, which is what the geometry
    // engine then walks.
    const uint64_t pos = ringBuffer + GFX12::kGePosRingOffset;
    const uint64_t prim = ringBuffer + GFX12::kGePrimRingOffset;
    const uint32_t rings[4] = {
        static_cast<uint32_t>(pos >> 16), (16384u * 16u) >> 5,
        static_cast<uint32_t>(prim >> 16), 0x0C6E0000u | ((16368u * 4u) >> 5),
    };
    if (!setRegSeq(0x309a0u, rings, 4)) return false;

    // SPI_SHADER_USER_DATA_GS_0 is s8 on gfx12: the merged NGG stage takes
    // s0..s7 from the program registers (PGM_LO/HI_GS, PGM_HI_ES, RSRC3_GS,
    // RSRC4_GS, PGM_LO_ES, RSRC1/2_GS) - radeonsi's si_init_all_descriptors
    // offsets the GS user-data bank by (PGM_LO_GS - USER_DATA_GS_0) / 4 = -8.
    // AIRGraphicsLowering puts the vertex base in sgpr16:17, the stride in 18
    // and the vertex count in 19, so those three live at user data word 8.
    // Words 0..3 are the attribute-ring descriptor. The vertex pointer, stride
    // and count sit at word 8 (sgpr16) for the s8 user-data base; the same three
    // values are mirrored eight words higher so a stage that bases user data at
    // s0 still reads a valid pointer instead of address zero.
    // Attribute-ring descriptor, built like ac_build_attr_ring_descriptor():
    // BASE_ADDR | SWIZZLE_ENABLE_GFX11(3, 16 B granules) with STRIDE 0 - the
    // index is spread by the swizzle, not by a stride (Mesa passes stride 0 and
    // relies on swizzle_enable=3 / index_stride=32 elements). A non-zero stride
    // here made every structured store land outside the ring.
    const uint32_t userData[20] = {
        static_cast<uint32_t>(ringBuffer),
        static_cast<uint32_t>((ringBuffer >> 32) & 0xFFFFu) | (3u << 30),
        ringSize, 0x0043FFACu, 0u, 0u, 0u, 0u,
        static_cast<uint32_t>(vertexBuffer),
        static_cast<uint32_t>(vertexBuffer >> 32),
        stride,
        vertexCount,
        0u, 0u, 0u, 0u,
        static_cast<uint32_t>(vertexBuffer),
        static_cast<uint32_t>(vertexBuffer >> 32),
        stride,
        0u,
    };
    if (featureMask & 16u) {
        if (!setRegSeq(GFX12::SPI_SHADER_USER_DATA_GS_0, userData, 20)) return false;
    } else {
        const uint32_t legacy[10] = {
            static_cast<uint32_t>(ringBuffer),
            static_cast<uint32_t>((ringBuffer >> 32) & 0xFFFFu) | (3u << 30),
            ringSize, 0x0043FFACu, 0u,
            static_cast<uint32_t>(vertexBuffer), static_cast<uint32_t>(vertexBuffer >> 32), stride,
            vertexCount, primitiveCount,
        };
        if (!setRegSeq(GFX12::SPI_SHADER_USER_DATA_GS_0, legacy, 10)) return false;
    }
    // The fragment stage interpolates the varyings from the copies the vertex
    // stage writes into this buffer, so it needs the address: PS user data
    // words 2..3 (USER_SGPR=4, see setPixelShader).
    if (!setShaderUserData(GFX12::SPI_SHADER_USER_DATA_PS_0, 2, vertexBuffer)) return false;
    // Values from Cobalt64's hardware-validated gfx12 draw profile
    // (xlat12_ib.c: xlat12_ib_m2tri_profile): GE_CNTL 0xA0010080 for a
    // 128-vertex/128-prim subgroup, GE_MAX_OUTPUT_PER_SUBGROUP 128.
    if (!setReg(0x3096cu, 0xA0010080u)) return false;
    if (!setReg(GFX12::GE_MAX_OUTPUT_PER_SUBGROUP, 128u)) return false;
    // For passthrough vertex pipelines without an API geometry shader,
    // VGT_GS_MAX_VERT_OUT is 1 (Mesa si_state_shaders.cpp:1175, Cobalt64
    // gs_max_vert_out = 1). Writing 128 causes the GE subgroup allocator to
    // expect 128x vertex expansion into LDS, which fails on zero-LDS
    // configurations and starves the SPI of waves (the 0-wave dispatch bug).
    if (!setReg(GFX12::VGT_GS_MAX_VERT_OUT, 1u)) return false;
    if (!setReg(GFX12::GE_NGG_SUBGRP_CNTL, 1u)) return false;
    // No API GS -> gs_num_invocations = 0 -> VGT_GS_INSTANCE_CNT must be 0
    // (Mesa si_state_shaders.cpp:1160, Cobalt64 gs_instance_cnt = 0).
    if (!setReg(GFX12::VGT_GS_INSTANCE_CNT, 0u)) return false;
    // GE_GS_THROTTLE: the Mesa/Cobalt64 validated value. Writing a bare 0x80
    // here (the previous constant) cleared NUM_INIT_GRPS[23:30] to zero and
    // stomped the preamble's 0x7f9a80e1, changing how the GE seeds groups.
    if (!setReg(GFX12::GE_GS_THROTTLE, 0x7f9a80e1u)) return false;
    if (!setReg(GFX12::SPI_SHADER_PGM_RSRC4_GS, 0x007F0BFFu)) return false;
    if (!setReg(GFX12::SPI_SHADER_PGM_RSRC4_PS, 0x000007FFu)) return false;
    // gfx12 keeps only GS_EN (bit 5), GS_W32_EN (bit 22) and
    // PRIMGEN_PASSTHRU_NO_MSG (bit 26) here. Testing the passthru
    // configuration that radv uses for every vertex-only gfx12 pipeline.
    if (!setReg(GFX12::VGT_SHADER_STAGES_EN, 0x04400000u)) return false;
    if (!setReg(GFX12::VGT_GS_OUT_PRIM_TYPE, 2u)) return false;
    // SPI_SHADER_IDX_FORMAT is the primitive-index export format. NGG sends its
    // primitive over IDX0, so leaving this at NONE (0) drops the connectivity
    // and the rasterizer never sees a triangle.
    if (!setReg(GFX12::SPI_SHADER_IDX_FORMAT, 1u)) return false;
    // NUM_INTERP tells SPI how many param locations the PS reads. NO_PC_EXPORT
    // (bit 10) says the vertex half exported no parameter cache data; the two
    // must agree or SPI waits for params that never arrive.
    uint32_t gsOutConfig = parameterCount ? (parameterCount << 11) : (1u << 10);
    if (N48_ENV("N48_NO_PC_EXPORT")) gsOutConfig = (1u << 10);
    if (!setReg(GFX12::SPI_SHADER_GS_OUT_CONFIG_PS, gsOutConfig)) return false;
    {
        // gfx12 SPI_PS_IN_CONTROL keeps only PARAM_GEN/BC_OPTIMIZE_DISABLE/PS_W32_EN;
        // NUM_INTERP moved to SPI_SHADER_GS_OUT_CONFIG_PS above.
        if (!setReg(GFX12::SPI_PS_IN_CONTROL, 1u << 15)) return false;
        // SPI_PS_INPUT_ENA/ADDR are 0x2865c/0x28660 on gfx12. 0x286e4/0x286e8 - the
        // gfx10 addresses used here before - are SPI_BARYC_SSAA_CNTL and
        // SPI_TMPRING_SIZE on this chip, so the interpolant enables never landed.
        // SPI_PS_INPUT_ENA/ADDR must NEVER be 0: if 0, the SPI treats the shader
        // as not consuming any fragment locations and refuses to dispatch pixel waves!
        // Enable both PERSP_SAMPLE_ENA (bit 0 = 1) and PERSP_CENTER_ENA (bit 1 = 2) = 3u,
        // covering both sample and center interpolants (Metal [[stage_in]], Cobalt64 m2_col_fs).
        const uint32_t interpEnables = 3u;
        if (!setReg(GFX12::SPI_PS_INPUT_ENA, interpEnables)) return false;
        if (!setReg(GFX12::SPI_PS_INPUT_ADDR, interpEnables)) return false;
        if (!setReg(GFX12::SPI_BARYC_SSAA_CNTL, 4u)) return false;    // COVERED_CENTROID_IS_CENTER
    }
    if (!setReg(GFX12::VGT_NUM_INSTANCES, 1u)) return false;
    if (!setReg(GFX12::SPI_BARYC_CNTL, 1)) return false;
    if (!setReg(GFX12::SPI_PS_INPUT_CNTL_0, 0)) return false;
    // PA_SC_HISZ_CONTROL: ROUND(2) = 0x2 is required minimum value on GFX12 for EVERY pixel shader
    // (Mesa si_state_shaders.cpp:1701, Cobalt64 xlat12_ib.c:938). Without it, SC/HiZ rejects all fragments!
    if (!setReg(GFX12::PA_SC_HISZ_CONTROL, 2u)) return false;
    // Bypass rate combiners on GFX12 (Mesa si_state.c:808)
    if (!setReg(GFX12::PA_CL_VS_OUT_CNTL, 0x60000000u)) return false;
    // GFX12 standard viewport transform: VTX_W0_FMT (bit 10 = 0x400) |
    // VPORT X/Y/Z SCALE/OFFSET_ENA (bits [5:0] = 0x3F) = 0x43F.
    // Converts clip-space [-1, 1] NDC to screen pixels via PA_CL_VPORT.
    return setReg(GFX12::PA_CL_VTE_CNTL, 0x43Fu);
}

bool GraphicsPM4Builder::setDrawCounts(uint32_t vertexCount, uint32_t primitiveCount) {
    const uint32_t counts[2] = { vertexCount, primitiveCount };
    // The current ABI puts the vertex address in words 8..9, stride in 10,
    // and counts in 11..12. The older layout put counts in 8..9; retaining
    // that offset for the current layout corrupts the address just before draw.
    const uint32_t countWord = (featureMask & 16u) ? 11u : 8u;
    return setRegSeq(GFX12::SPI_SHADER_USER_DATA_GS_0 + countWord * 4u, counts, 2);
}

bool GraphicsPM4Builder::setPixelShader(uint64_t gpuAddr, uint32_t rsrc1,
                                        uint32_t rsrc2) {
    const uint32_t lo = static_cast<uint32_t>(gpuAddr >> 8);
    const uint32_t hi = static_cast<uint32_t>((gpuAddr >> 40) & 0xFFu);
    // Same CU-enable argument as the GS side: RSRC3_PS gates which CUs may run
    // pixel waves. It is not part of the PGM_LO..RSRC2 run, so it is its own
    // packet.
    if (!setReg(GFX12::SPI_SHADER_PGM_RSRC3_PS, GFX12::CU_EN_PS_ALL)) return false;
    // The fragment stage always needs user data words 2..3: LLVM's fragment
    // convention places the first inreg argument at s2, and the lowering reads
    // the vertex buffer address it interpolates the varyings from there. A
    // shader with texture/sampler/buffer resources needs more: the plugin
    // computes USER_SGPR_COUNT from the binding table (resources start at s5),
    // and hard-coding 4 here silently starved those shaders - the hardware never
    // loaded s5:s6, so the descriptor pointer was stale and the scalar load
    // faulted. Keep the requested count and only raise it to the minimum.
    const uint32_t requestedUserSgprs = rsrc2 & 0x3Eu;
    const uint32_t minimumUserSgprs = 4u << 1;
    rsrc2 = (rsrc2 & ~0x3Eu) |
            (requestedUserSgprs > minimumUserSgprs ? requestedUserSgprs : minimumUserSgprs);
    const uint32_t program[4] = { lo, hi, rsrc1, rsrc2 };
    return setRegSeq(GFX12::SPI_SHADER_PGM_LO_PS, program, 4);
}

bool GraphicsPM4Builder::setShaderUserData(uint32_t userDataBase, uint32_t index,
                                           uint64_t value) {
    const uint32_t slot[2] = {
        static_cast<uint32_t>(value & 0xFFFFFFFFu),
        static_cast<uint32_t>(value >> 32),
    };
    return setRegSeq(userDataBase + index * 4u, slot, 2);
}

bool GraphicsPM4Builder::setPrimitiveType(uint32_t diPrimType) {
    return setReg(GFX12::VGT_PRIMITIVE_TYPE, diPrimType);
}

bool GraphicsPM4Builder::setIndexType(uint32_t vgtIndexType) {
    return setReg(GFX12::VGT_INDEX_TYPE, vgtIndexType);
}

bool GraphicsPM4Builder::appendDrawInitiator(uint32_t sourceSelect, bool opaque) {
    uint32_t initiator = sourceSelect & 0x3u;
    if (opaque) initiator |= GFX12::DI_FLAG_USE_OPAQUE;
    return appendDword(initiator);
}

bool GraphicsPM4Builder::setNumInstances(uint32_t instanceCount) {
    if (!appendDword(type3Header(GFX12::PKT3_NUM_INSTANCES, 1))) return false;
    return appendDword(instanceCount);
}

bool GraphicsPM4Builder::drawPrimitives(uint32_t vertexCount, bool opaque) {
    if (!appendDword(type3Header(GFX12::PKT3_DRAW_INDEX_AUTO, 2))) return false;
    if (!appendDword(vertexCount)) return false;
    return appendDrawInitiator(GFX12::DI_SRC_SEL_AUTO_INDEX, opaque);
}

bool GraphicsPM4Builder::drawIndexedPrimitives(uint32_t indexCount,
                                               uint64_t indexGpuAddr,
                                               uint32_t maxIndexCount, bool opaque) {
    if (!appendDword(type3Header(GFX12::PKT3_DRAW_INDEX_2, 5))) return false;
    if (!appendDword(maxIndexCount)) return false;
    if (!appendDword(static_cast<uint32_t>(indexGpuAddr & 0xFFFFFFFCu))) return false;
    if (!appendDword(static_cast<uint32_t>(indexGpuAddr >> 32))) return false;
    if (!appendDword(indexCount)) return false;
    return appendDrawInitiator(GFX12::DI_SRC_SEL_DMA, opaque);
}

namespace {
// Values below are the ones Mesa's gfx12 graphics preamble programs
// (ac_cmdbuf.c: gfx12_init_graphics_preamble_state) for the registers this
// path can express; the multi-field words were re-derived from
// mesa/src/amd/registers/gfx12.json and agree with Cobalt64's gfx12 profile
// (xlat12_ib.c: xlat12_ib_m2tri_profile).
constexpr uint32_t kZero4[4] = { 0u, 0u, 0u, 0u };

// The scissor pairs are inclusive on gfx12 and their coordinates are 16-bit
// signed. 0xFFFFFFFF would therefore read as (-1,-1) - an empty rectangle that
// clips every fragment - while 0x3FFF3FFF (16383,16383) is the largest valid
// coordinate and covers any surface.
constexpr uint32_t kScissorFull = 0x3FFF3FFFu;

struct PreambleReg {
    uint32_t addr;
    uint32_t value;
};

constexpr PreambleReg kPreamble[] = {
    // Shader stage control. RSRC3 carries the per-stage CU enable masks;
    // without them SPI has no CU to dispatch a wave to.
    { GFX12::SPI_SHADER_PGM_RSRC3_GS, GFX12::CU_EN_GS_ALL },
    { GFX12::SPI_SHADER_PGM_RSRC3_PS, GFX12::CU_EN_PS_ALL },
    { GFX12::SPI_SHADER_PGM_RSRC3_HS, 0xffffffffu },
    { GFX12::SPI_SHADER_REQ_CTRL_PS,  0x00000007u },   // SOFT_GROUPING_EN(1) | NUMBER_OF_REQUESTS_PER_CU(3) (Mesa ac_cmdbuf.c:661)

    // Scissor and clip. A zero CLIP_RULE or an all-zero AA mask throws away
    // every pixel the rasterizer produces.
    { GFX12::PA_SC_SCREEN_SCISSOR_TL,  0x00000000u },
    { GFX12::PA_SC_SCREEN_SCISSOR_BR,  kScissorFull },
    { GFX12::PA_SC_WINDOW_SCISSOR_TL,  0x00000000u },
    // The window scissor gates every fragment independently of the viewport
    // scissor, and its reset value clips to (0,0). Leaving it uninitialised
    // drops all fragments for any app that never calls setScissorRect, which is
    // the common case for a full-target draw.
    { GFX12::PA_SC_WINDOW_SCISSOR_BR,  kScissorFull },
    { GFX12::PA_SC_GENERIC_SCISSOR_TL, 0x00000000u },
    { GFX12::PA_SC_GENERIC_SCISSOR_BR, kScissorFull },
    // PA_SC_MODE_CNTL_0.VPORT_SCISSOR_ENABLE gates fragments through the viewport
    // scissor; without initializing these registers the hardware clips to the reset
    // range (0,0)-(0,0) and drops every fragment before the pixel shader runs.
    { GFX12::PA_SC_VPORT_SCISSOR_0_TL, 0x00000000u },
    { GFX12::PA_SC_VPORT_SCISSOR_0_BR, kScissorFull },
    { GFX12::PA_SC_CLIPRECT_RULE,      0x0000ffffu },
    { GFX12::PA_SC_EDGERULE,          0xaa959a6au },   // Top-left rule: ER_TRI=0xA, ER_RECT=0xA (Mesa si_state.c:1010)
    { GFX12::PA_SC_AA_MASK_X0Y0_X1Y0,  kScissorFull },
    { GFX12::PA_SC_AA_MASK_X0Y1_X1Y1,  kScissorFull },
    { GFX12::PA_SC_AA_CONFIG,          0u },
    { GFX12::PA_SC_MODE_CNTL_0,        0x00000022u },   // VPORT_SCISSOR_ENABLE(1) | ALTERNATE_RBS_PER_TILE(1)
    { GFX12::PA_SC_MODE_CNTL_1,        0x06020181u },   // WALK_SIZE(1) for linear target, SUPERTILE/TILE_WALK_ORDER, EOV
    { GFX12::PA_SC_BINNER_CNTL_0,      0x19FC0123u },   // BINNING_DISABLED (bits[1:0]=3) from Cobalt64/Mesa (0 was BINNING_ALLOWED!)
    // Disable all clipping (CLIP_DISABLE bit 16, ZCLIP_NEAR bit 26, ZCLIP_FAR bit 27)
    // while keeping DX_CLIP_SPACE_DEF (bit 19). This prevents vertices with Z=0.0
    // from being clipped away by floating point edge rounding at the near plane!
    { GFX12::PA_CL_CLIP_CNTL,          0x0c090000u },
    { GFX12::PA_SU_PRIM_FILTER_CNTL,   0u },            // Mesa ac_cmdbuf.c:820: must be 0 on GFX12
    { GFX12::PA_SU_SC_MODE_CNTL,       0u },            // No face culling, front face CCW, disable polymode
    { GFX12::PA_SU_VTX_CNTL,           0x29u },         // PIX_CENTER(1) | QUANT_MODE(5: 1/256th) - standard Mesa/radv
    // SX_PS_DOWNCONVERT: 4 bits per MRT. 0 (NO_CONVERSION) is what Mesa derives
    // for a COLOR_8_8_8_8 target fed by a 32_ABGR export; the previous 0xff put
    // 0xF in MRT0..3, a reserved SX_DOWNCONVERT_FORMAT value, and the SX/CB then
    // had no usable export format for the fragment.
    { GFX12::SX_PS_DOWNCONVERT,        0x00000000u },
    { GFX12::DB_SHADER_CONTROL,        0u },

    // Primitive/geometry state.
    { GFX12::VGT_DRAW_PAYLOAD_CNTL,    0u },
    { GFX12::GE_USER_VGPR_EN,          0u },

    // The rest of Mesa's gfx12 preamble (ac_cmdbuf.c
    // gfx12_init_graphics_preamble_state), which a driver has to write in full
    // because gfx12 has no CLEAR_STATE. Addresses are dword*4 of the gfx12
    // register JSON; values are the Mesa field values.
    { 0x28040u, 0x00000000u },   // DB_GL1_INTERFACE_CONTROL
    { 0x28048u, 0x00000000u },   // DB_MEM_TEMPORAL
    { 0x28064u, 0x00000000u },   // DB_VIEWPORT_CONTROL
    { 0x28068u, 0x00000000u },   // DB_SPI_VRS_CENTER_LOCATION
    { 0x28080u, 0x00000000u },   // TA_BC_BASE_ADDR
    { 0x28084u, 0x00000000u },   // TA_BC_BASE_ADDR_HI
    { 0x2808cu, 0x00000101u },   // DB_STENCIL_OPVAL
    { 0x280f8u, 0x00000000u },   // SC_MEM_TEMPORAL
    { 0x280fcu, 0x00000000u },   // SC_MEM_SPEC_READ
    { 0x28358u, 0x00000000u },   // PA_SC_SCREEN_EXTENT_CONTROL
    { 0x2835cu, 0x00000000u },   // PA_SC_TILE_STEERING_OVERRIDE
    { 0x283e0u, 0x00000000u },   // PA_SC_VRS_INFO
    // VRS off: an override rate or combiner mode left over from a previous
    // submission changes how many fragments SC produces per pixel, and a stale
    // non-zero value can suppress them entirely. Both registers are written
    // explicitly because gfx12 has no CLEAR_STATE.
    { GFX12::PA_SC_VRS_OVERRIDE_CNTL, 0x00000000u },   // VRS_OVERRIDE_RATE_COMBINER_MODE=0, VRS_RATE=0
    { 0x28848u, 0x00000000u },   // PA_CL_VRS_CNTL
    { 0x286e4u, 0x00000004u },   // SPI_BARYC_SSAA_CNTL
    { 0x287d4u, 0x00000000u },   // PA_CL_POINT_X_RAD
    { 0x287d8u, 0x00000000u },   // PA_CL_POINT_Y_RAD
    { 0x287dcu, 0x00000000u },   // PA_CL_POINT_SIZE
    { 0x287e0u, 0x00000000u },   // PA_CL_POINT_CULL_RAD
    { GFX12::PA_CL_VS_OUT_CNTL,        0x60000000u },   // BYPASS_VTX_RATE_COMBINER | BYPASS_PRIM_RATE_COMBINER
    { 0x28820u, 0x00000000u },   // PA_CL_NANINF_CNTL
    { 0x28824u, 0x00000000u },   // PA_SU_LINE_STIPPLE_CNTL
    { 0x28828u, 0x00000000u },   // PA_SU_LINE_STIPPLE_SCALE
    { 0x28830u, 0x00000002u },   // PA_SU_SMALL_PRIM_FILTER_CNTL: TRIANGLE_FILTER_DISABLE=1, SMALL_PRIM_FILTER=0
    { 0x2883cu, 0x00000000u },   // PA_SU_OVER_RASTERIZATION_CNTL
    { 0x28840u, 0x00000002u },   // PA_STEREO_CNTL
    { 0x28a18u, 0x42800000u },   // VGT_HOS_MAX_TESS_LEVEL (64.0f)
    { 0x28a1cu, 0x00000000u },   // VGT_HOS_MIN_TESS_LEVEL
    { 0x28a50u, 0x00000000u },   // GE_SE_ENHANCE
    { 0x28a70u, 0x00000000u },   // GE_IA_ENHANCE
    { 0x28a80u, 0x00000000u },   // GE_WD_ENHANCE
    { 0x28a9cu, 0x00000000u },   // VGT_REUSE_OFF
    { 0x28abcu, 0x00000000u },   // DB_HTILE_SURFACE
    { 0x28b50u, 0x78808080u },   // VGT_TESS_DISTRIBUTION
    { GFX12::PA_SC_HIZ_INFO,           0x00000000u },   // PA_SC_HIZ_INFO
    { 0x28b98u, 0x00000000u },   // PA_SC_HIS_INFO
    { GFX12::PA_SC_HISZ_CONTROL,       0x00000002u },   // ROUND(2) required minimum value on GFX12
    { 0x28bc0u, 0x00000000u },   // PA_SC_HISZ_RENDER_OVERRIDE
    { 0x28c40u, 0x00000800u },   // PA_SC_BINNER_OUTPUT_TIMEOUT_COUNTER
    { 0x28c48u, 0x01ff00feu },   // PA_SC_BINNER_CNTL_1
    { 0x28c4cu, 0x00800000u },   // PA_SC_BINNER_CNTL_2
    { 0x28c50u, 0x00000010u },   // PA_SC_NGG_MODE_CNTL: MAX_DEALLOCS_IN_WAVE(16), Mesa gfx12 preamble
    { 0x28c58u, 0x00000001u },   // VGT_VERTEX_REUSE_BLOCK_CNTL: VTX_REUSE_DEPTH(1)
    { 0x30924u, 0x00000000u },   // GE_MIN_VTX_INDX
    { 0x30928u, 0x00000000u },   // GE_INDX_OFFSET
    { 0x3092cu, 0x00000004u },   // GE_MULTI_PRIM_IB_RESET_EN
    { 0x30950u, 0x7f9a80e1u },   // GE_GS_THROTTLE
    { 0x30964u, 0xffffffffu },   // GE_MAX_VTX_INDX
    { 0x30968u, 0x00000000u },   // VGT_INSTANCE_BASE_ID
    { 0x3097cu, 0x00000000u },   // GE_STEREO_CNTL
    { 0x309b4u, 0x00000000u },   // VGT_PRIMITIVEID_RESET
    { 0x3098cu, 0x00000000u },   // GE_VRS_RATE
    { 0x30a00u, 0x00000000u },   // PA_SU_LINE_STIPPLE_VALUE
    { 0x30a04u, 0x00000000u },   // PA_SC_LINE_STIPPLE_STATE
    { 0x31128u, 0x00008a4du },   // SPI_GRP_LAUNCH_GUARANTEE_ENABLE
    { 0x3112cu, 0x00401123u },   // SPI_GRP_LAUNCH_GUARANTEE_CTRL

    // Power-on state that stops fragments. None of these is in Mesa's static
    // preamble - radeonsi/radv write them from viewport, depth-stencil and
    // MSAA state instead - and gfx12 has no CLEAR_STATE, so on this card each
    // of them reads back as a different random value after every cold boot
    // (PA_SU_HARDWARE_SCREEN_OFFSET 0x0c62040e, DB_EQAA 0x00134504, ...).
    // With the offsets the triangle lands tens of thousands of pixels outside
    // the target; with the DB state the depth block writes to an unmapped
    // surface (VM fault) or, once that is fixed, swallows every fragment
    // because EQAA says 32 mask samples and alpha-to-mask is on - SPI never
    // receives a pixel wave. Each group was confirmed necessary on hardware
    // by adding it alone and watching the PS counters and the framebuffer.
    { GFX12::PA_SU_HARDWARE_SCREEN_OFFSET, 0x00000000u },   // Mesa: centre of the viewport; 0 is valid
    { GFX12::PA_SC_WINDOW_OFFSET,          0x00000000u },
    { GFX12::PA_SC_CONSERVATIVE_RASTERIZATION_CNTL, 0x00100000u },   // NULL_SQUAD_AA_MASK_ENABLE (si_state.c)
    { GFX12::DB_Z_INFO,                    0x00000000u },   // Z_INVALID: no depth attachment
    { GFX12::DB_STENCIL_INFO,              0x20000000u },   // STENCIL_INVALID | TILE_STENCIL_DISABLE
    { GFX12::DB_RENDER_OVERRIDE,           0x00000000u },
    { GFX12::DB_RENDER_OVERRIDE2,          0x08000000u },   // CENTROID_COMPUTATION_MODE(1) (radv null DS)
    { GFX12::DB_EQAA,                      0x00000000u },   // single sample
    { GFX12::DB_ALPHA_TO_MASK,             0x00018700u },   // Mesa dither offsets, ALPHA_TO_MASK_ENABLE off
};
} // namespace

bool GraphicsPM4Builder::emitGraphicsPreamble(uint64_t mask, uint64_t maskHigh) {
    // Bit 9 is a raster-discard diagnostic, not an omitted PS program.
    // Omitting PS writes leaves stale/zero state and cannot isolate a stall.
    // Preserve the shader setup and stop primitives before rasterization with
    // PA_CL_CLIP_CNTL.DX_RASTERIZATION_KILL (gfx12 bit 22).
    const bool discard = (featureMask & 512u) != 0;
    bool clipWritten = false;
    size_t index = 0;
    for (const PreambleReg& reg : kPreamble) {
        // The table is longer than one 64-bit word: entries 0..63 come from
        // `mask`, the tail from `maskHigh`. A single 32-bit word used to strand
        // everything past entry 31, so a "full" preamble was still partial.
        bool selected;
        if (index < 64u) {
            selected = (mask & (1ull << index)) != 0;
        } else if (index - 64u < 64u) {
            selected = (maskHigh & (1ull << (index - 64u))) != 0;
        } else {
            selected = false;
        }
        if (selected) {
            uint32_t value = reg.value;
            if (reg.addr == GFX12::PA_CL_CLIP_CNTL) {
                clipWritten = true;
                if (discard) value |= 1u << 22;
            }
            if (!setReg(reg.addr, value)) return false;
        }
        index++;
    }
    // P=0 is a clean-boot diagnostic: retain the reset clip state except for
    // the discard bit. Do not enable unrelated preamble entries.
    if (discard && !clipWritten &&
        !setReg(GFX12::PA_CL_CLIP_CNTL, 1u << 22)) return false;
    // The top bit of the second mask word selects the trailing group
    // (accumulators, CB_MEM, instance count). It cannot share a bit with a
    // table entry now that entries past 63 live in that word.
    if (maskHigh & (1ull << 63)) {
        // The four accumulator registers are zeroed as a run; they are what the
        // wave launch allocator reads before it has seen a draw.
        if (!setRegSeq(0x0b0c8u, kZero4, 4)) return false;   // SPI_SHADER_USER_ACCUM_PS_0..3
        if (!setRegSeq(0x0b2c8u, kZero4, 4)) return false;   // SPI_SHADER_USER_ACCUM_ESGS_0..3
        if (!setRegSeq(0x0b4c8u, kZero4, 4)) return false;   // SPI_SHADER_USER_ACCUM_LSHS_0..3
        // CB_MEM0_INFO..7: the eight per-MRT memory hints.
        for (uint32_t i = 0; i < 8; i++) {
            if (!setReg(GFX12::CB_MEM0_INFO + i * 4u, 0u)) return false;
        }
        // Reset whatever a previous submission left in the shader-visible index
        // and colour export formats; the per-pipeline writes set the real values.
        if (!setReg(GFX12::SPI_SHADER_Z_FORMAT, 0u)) return false;
        if (!setReg(GFX12::VGT_NUM_INSTANCES, 1u)) return false;
    }
#ifndef KERNEL
    // Bisect hook: N48_REG_OVERRIDES="0x28a4c=0x60201ac,0x28a48=0x20" lets a
    // test run replace individual register values without a rebuild of the
    // values themselves. Applied after the table so it wins.
    {
        const char* overrides = N48_ENV("N48_REG_OVERRIDES");
        while (overrides && *overrides) {
            char* end = nullptr;
            const uint32_t addr = (uint32_t)strtoul(overrides, &end, 0);
            if (!end || *end != '=') break;
            const uint32_t value = (uint32_t)strtoul(end + 1, &end, 0);
            if (!setReg(addr, value)) return false;
            overrides = (end && *end == ',') ? end + 1 : nullptr;
        }
    }
#endif
    // Enable all render backends on GFX11+ (Mesa ac_cmdbuf.c:837-845).
    // Without this EVENT_WRITE(PIXEL_PIPE_STAT_CONTROL), all pixel pipes
    // stay disabled and drop every fragment that the rasterizer outputs.
    // N48_PPSC="lo,hi" overrides the instance-enable words so a run can test
    // whether a wrong mask (bit positions) is disabling every render backend.
    uint32_t ppscLo = 0xFFFFFC00u;   // STRIDE(2) | INSTANCE_EN_LO
    uint32_t ppscHi = 0x000007FFu;   // INSTANCE_EN_HI
#ifndef KERNEL
    {
        const char* override = N48_ENV("N48_PPSC");
        if (override && *override) {
            char* end = nullptr;
            const uint32_t lo = (uint32_t)strtoul(override, &end, 0);
            if (end && *end) {
                ppscLo = lo;
                ppscHi = (uint32_t)strtoul(end + 1, nullptr, 0);
            }
        }
    }
#endif
    if (!appendDword(type3Header(GFX12::PKT3_EVENT_WRITE, 3))) return false;
    if (!appendDword(0x00000138u)) return false; // EVENT_TYPE(56) | EVENT_INDEX(1)
    if (!appendDword(ppscLo)) return false;
    return appendDword(ppscHi);
}

bool GraphicsPM4Builder::emitCacheFlush() {
    const uint32_t event = (GFX12::EVENT_TYPE_CACHE_FLUSH_AND_INV_TS & 0x3Fu) |
                           ((GFX12::EVENT_INDEX_CACHE_FLUSH_AND_INV_TS & 0xFu) << 8);
    if (!appendDword(type3Header(GFX12::PKT3_EVENT_WRITE, 1))) return false;
    return appendDword(event);
}

bool GraphicsPM4Builder::emitContextControl() {
    // Kernel-queue variant: only update the enable bits. The per-context
    // load/shadow bits wedge a queue whose state the firmware already owns.
    const uint32_t load = 1u << 31;
    const uint32_t shadow = 1u << 31;
    if (!appendDword(type3Header(GFX12::PKT3_CONTEXT_CONTROL, 2))) return false;
    if (!appendDword(load)) return false;
    return appendDword(shadow);
}

bool GraphicsPM4Builder::acquireMem() {
    // GFX12 ACQUIRE_MEM: engine select, coherence range, poll interval, then
    // GCR_CNTL. Invalidate instruction/scalar/vector caches and write back /
    // invalidate L2 so uploads to reused GPU addresses cannot execute stale
    // shader code. GFX12 does not use the older GL1/metadata cache actions.
    const uint32_t gcrCntl = GFX12::GCR_GLI_INV_ALL | GFX12::GCR_GLK_INV |
                             GFX12::GCR_GLV_INV | GFX12::GCR_GL2_INV | GFX12::GCR_GL2_WB;
    if (!appendDword(type3Header(GFX12::PKT3_ACQUIRE_MEM, 7))) return false;
    if (!appendDword(0u)) return false;
    if (!appendDword(0xffffffffu)) return false;
    if (!appendDword(0xffffffu)) return false;
    if (!appendDword(0u)) return false;
    if (!appendDword(0u)) return false;
    if (!appendDword(0x0000000Au)) return false;
    return appendDword(gcrCntl);
}

bool GraphicsPM4Builder::releaseMem(uint64_t gpuAddr, uint64_t value) {
    // Same layout the compute path already uses successfully on this card:
    // control, data selector, address, payload, spare dword.
    const uint32_t control = (1u << 21) | (1u << 22) | (3u << 25) |
                             (GFX12::EVENT_TYPE_CACHE_FLUSH_AND_INV_TS & 0x3Fu) |
                             ((5u & 0xFu) << 8);
    const uint32_t dataSel = (2u << 29) | (2u << 24);

    if (!appendDword(type3Header(GFX12::PKT3_RELEASE_MEM, 7))) return false;
    if (!appendDword(control)) return false;
    if (!appendDword(dataSel)) return false;
    if (!appendDword(static_cast<uint32_t>(gpuAddr & 0xFFFFFFFCu))) return false;
    if (!appendDword(static_cast<uint32_t>(gpuAddr >> 32))) return false;
    if (!appendDword(static_cast<uint32_t>(value & 0xFFFFFFFFu))) return false;
    if (!appendDword(static_cast<uint32_t>(value >> 32))) return false;
    return appendDword(0);
}
