//
//  test_graphics_pm4.cpp
//  Host-side checks for GraphicsPM4Builder. No hardware and no IOKit: it
//  verifies the packet bytes against the PM4 layout and the gfx12 register
//  blocks, which is what used to be wrong.
//
//  build: clang++ -std=c++17 -I RDNA4Driver/src/hardware \
//             RDNA4Driver/tests/test_graphics_pm4.cpp \
//             RDNA4Driver/src/hardware/GraphicsPM4Builder.cpp -o /tmp/test_pm4
//

#include "GraphicsPM4Builder.hpp"

#include <stdio.h>

static int failures;

static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void checkEq(uint32_t got, uint32_t want, const char* what) {
    if (got == want) {
        printf("  ok   %-50s 0x%08x\n", what, got);
    } else {
        printf("  FAIL %-50s got 0x%08x want 0x%08x\n", what, got, want);
        failures++;
    }
}

// PKT3 type-3 header: [31:30]=3, [29:16]=payload-1, [15:8]=opcode.
static uint32_t header(uint32_t opcode, uint32_t payloadDwords) {
    return (3u << 30) | ((payloadDwords - 1u) << 16) | (opcode << 8);
}

static uint32_t blockOffset(uint32_t addr, uint32_t base) {
    return ((addr >> 2) - (base >> 2)) & 0xFFFFu;
}

static void testRegisterOffsetConversion() {
    printf("\n[register offset conversion]\n");
    GraphicsPM4Builder b;
    const uint32_t* p;

    b.reset();
    check(b.setReg(GFX12::PA_CL_VPORT_XSCALE, 0x11111111u), "setReg(context) accepted");
    p = b.getPackets(nullptr);
    checkEq(p[0], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "context header");
    checkEq(p[1], blockOffset(GFX12::PA_CL_VPORT_XSCALE, GFX12::kContextBase),
            "PA_CL_VPORT_XSCALE offset");
    checkEq(p[1], 0x10fu, "PA_CL_VPORT_XSCALE == 0x10f");

    b.reset();
    check(b.setReg(GFX12::SPI_SHADER_PGM_LO_PS, 0x22222222u), "setReg(sh) accepted");
    p = b.getPackets(nullptr);
    checkEq(p[0], header(GFX12::PKT3_SET_SH_REG, 2), "shader header");
    checkEq(p[1], 0x08u, "SPI_SHADER_PGM_LO_PS == 0x08");

    b.reset();
    check(b.setReg(GFX12::VGT_PRIMITIVE_TYPE, GFX12::DI_PT_TRILIST), "setReg(uconfig) accepted");
    p = b.getPackets(nullptr);
    checkEq(p[0], header(GFX12::PKT3_SET_UCONFIG_REG, 2), "uconfig header");
    checkEq(p[1], 0x242u, "VGT_PRIMITIVE_TYPE == 0x242");

    b.reset();
    check(!b.setReg(0x1000u, 0), "address outside every block rejected");
    check(!b.setReg(0x2c000u, 0), "context end is exclusive");
    check(!b.setReg(0x0c000u, 0), "shader end is exclusive");
    check(!b.setReg(0x40000u, 0), "uconfig end is exclusive");
}

static void testSequencePacket() {
    printf("\n[sequence packets]\n");
    GraphicsPM4Builder b;
    const uint32_t* p;

    b.reset();
    check(b.setViewport(0, 0, 640, 480, 0, 1), "setViewport accepted");
    size_t n = 0;
    p = b.getPackets(&n);
    checkEq(static_cast<uint32_t>(n), 10, "viewport is one 10-dword packet");
    checkEq(p[0], header(GFX12::PKT3_SET_CONTEXT_REG, 9), "viewport header covers 8 values");
    checkEq(p[1], 0x10fu, "viewport starts at PA_CL_VPORT_XSCALE");
    checkEq(p[2], 0x43a00000u, "xscale = 320.0");
    checkEq(p[3], 0x43a00000u, "xoffset = 320.0");
    checkEq(p[4], 0xc3700000u, "yscale = -240.0 (Metal Y-down)");
    checkEq(p[5], 0x43700000u, "yoffset = 240.0");
    checkEq(p[6], 0x3f800000u, "Metal zscale = far - near = 1");
    checkEq(p[7], 0u, "Metal zoffset = near = 0");
    checkEq(p[8], 0u, "PA_SC_VPORT_ZMIN_0 = 0");
    checkEq(p[9], 0x3f800000u, "PA_SC_VPORT_ZMAX_0 = 1");

    b.reset();
    check(b.setViewport(0, 0, 640, 480, 0.2f, 0.8f), "custom depth viewport accepted");
    p = b.getPackets(nullptr);
    checkEq(p[6], 0x3f19999au, "custom zscale = 0.8 - 0.2");
    checkEq(p[7], 0x3e4ccccdu, "custom zoffset = 0.2");
    checkEq(p[8], 0x3e4ccccdu, "custom ZMIN = 0.2");
    checkEq(p[9], 0x3f4ccccdu, "custom ZMAX = 0.8");

    b.reset();
    const uint32_t two[2] = { 1, 2 };
    check(!b.setRegSeq(GFX12::kShEnd - 4, two, 2), "sequence crossing the block end rejected");
    check(b.setReg(GFX12::kShEnd - 4, 1), "single register at the last block dword accepted");
}

static void testDraws() {
    printf("\n[draws]\n");
    GraphicsPM4Builder b;
    const uint32_t* p;

    b.reset();
    check(b.drawPrimitives(3), "drawPrimitives accepted");
    p = b.getPackets(nullptr);
    checkEq(p[0], header(GFX12::PKT3_DRAW_INDEX_AUTO, 2), "DRAW_INDEX_AUTO header");
    checkEq(p[1], 3u, "vertex count");
    checkEq(p[2], GFX12::DI_SRC_SEL_AUTO_INDEX, "initiator = AUTO_INDEX in bits [1:0]");

    b.reset();
    check(b.drawPrimitives(3, true), "drawPrimitives(opaque) accepted");
    p = b.getPackets(nullptr);
    checkEq(p[2], GFX12::DI_SRC_SEL_AUTO_INDEX | GFX12::DI_FLAG_USE_OPAQUE,
            "initiator carries USE_OPAQUE");

    b.reset();
    check(b.drawIndexedPrimitives(6, 0x80000010ull, 6), "drawIndexedPrimitives accepted");
    p = b.getPackets(nullptr);
    checkEq(p[0], header(GFX12::PKT3_DRAW_INDEX_2, 5), "DRAW_INDEX_2 header");
    checkEq(p[1], 6u, "max index count");
    checkEq(p[2], 0x80000010u, "index base low");
    checkEq(p[3], 0u, "index base high");
    checkEq(p[4], 6u, "index count");
    checkEq(p[5], GFX12::DI_SRC_SEL_DMA, "initiator = DMA for indexed");
}

static void testShaderPrograms() {
    printf("\n[shader programs]\n");
    GraphicsPM4Builder b;
    const uint32_t* p;
    size_t n = 0;

    // Pixel shader: RSRC3_PS carries the CU-enable mask and is not adjacent to
    // the PGM_LO..RSRC2 run, then that run itself is one packet.
    b.reset();
    check(b.setPixelShader(0x800e002000ull, 0x2c0000u, 0x1000u), "setPixelShader accepted");
    p = b.getPackets(&n);
    checkEq(static_cast<uint32_t>(n), 9, "PS CU mask packet + 6-dword program packet");
    checkEq(p[0], header(GFX12::PKT3_SET_SH_REG, 2), "PS RSRC3 header covers 1 value");
    checkEq(p[1], 0x06u, "SPI_SHADER_PGM_RSRC3_PS offset");
    checkEq(p[2], GFX12::CU_EN_PS_ALL, "PS CU_EN covers every CU");
    checkEq(p[3], header(GFX12::PKT3_SET_SH_REG, 5), "PS header covers 4 values");
    checkEq(p[4], 0x08u, "PS header offset");
    checkEq(p[5], 0x800e0020u, "PS address bits [39:8]");
    checkEq(p[6], 0x00u, "PS address bits [47:40] (VRAM below 2^40)");

    // The [47:40] half only shows up above 2^40; prove the split works there.
    b.reset();
    check(b.setPixelShader(0x80800e002000ull, 0, 0), "high-address pixel shader accepted");
    p = b.getPackets(&n);
    checkEq(p[5], 0x800e0020u, "high address bits [39:8] unchanged");
    checkEq(p[6], 0x80u, "high address bits [47:40]");

    // Vertex shader on gfx12 is the NGG ES/GS pair, and the runs are not
    // contiguous, so it takes six packets.
    b.reset();
    check(b.setVertexShader(0x800e004000ull, 0x2c0000u, 0x1060u), "setVertexShader accepted");
    p = b.getPackets(&n);
    checkEq(static_cast<uint32_t>(n), 20,
            "GS pair + ES lo + ES hi + GS rsrc + GS CU mask + pos format");
    checkEq(p[0], header(GFX12::PKT3_SET_SH_REG, 3), "GS program header");
    checkEq(p[1], 0x84u, "SPI_SHADER_PGM_LO_GS offset");
    checkEq(p[2], 0x800e0040u, "GS address bits [39:8]");
    checkEq(p[3], 0x00u, "GS address bits [47:40]");
    checkEq(p[4], header(GFX12::PKT3_SET_SH_REG, 2), "ES LO header");
    checkEq(p[5], 0x89u, "SPI_SHADER_PGM_LO_ES offset");
    checkEq(p[6], 0x800e0040u, "ES address bits [39:8]");
    checkEq(p[7], header(GFX12::PKT3_SET_SH_REG, 2), "ES HI header");
    checkEq(p[8], 0x86u, "SPI_SHADER_PGM_HI_ES offset");
    checkEq(p[9], 0x00u, "ES address bits [47:40]");
    checkEq(p[10], header(GFX12::PKT3_SET_SH_REG, 3), "GS resources header");
    checkEq(p[11], 0x8au, "SPI_SHADER_PGM_RSRC1_GS offset");
    checkEq(p[12], 0x2c0000u, "GS rsrc1");
    checkEq(p[13], 0x1060u, "GS rsrc2");
    checkEq(p[14], header(GFX12::PKT3_SET_SH_REG, 2), "GS RSRC3 header covers 1 value");
    checkEq(p[15], 0x87u, "SPI_SHADER_PGM_RSRC3_GS offset");
    checkEq(p[16], GFX12::CU_EN_GS_ALL, "GS/ES CU_EN covers every CU");
}

static void testVertexInputs() {
    printf("\n[vertex inputs and GFX12 attribute ring]\n");
    GraphicsPM4Builder b;
    const uint64_t vertex = 0x800e010000ull;
    const uint64_t ring = 0x400f00000ull;
    const uint32_t ringSize = 1408 * 1024;
    check(b.setVertexInputs(vertex, 20, ring, ringSize, 1, 3, 1), "vertex and ring setup accepted");
    size_t count = 0;
    const uint32_t* packets = b.getPackets(&count);
    const uint32_t expected[] = {
        static_cast<uint32_t>(ring >> 16), ((ringSize >> 16) - 1) | (1u << 17),
        0xA0010080u, 0x04400000u, 0x800u,
    };
    for (uint32_t value : expected) {
        bool found = false;
        for (size_t i = 0; i < count; ++i) found |= packets[i] == value;
        check(found, "required ring or vertex value is present");
    }
    // User data starts at SPI_SHADER_USER_DATA_GS_0 == s8 on gfx12, and the
    // lowering reads the vertex base from sgpr16:17, stride from 18, count from
    // 19 - i.e. user data words 8..11.
    const size_t userData = 12;
    checkEq(packets[userData], header(GFX12::PKT3_SET_SH_REG, 21), "user data header covers 20 values");
    checkEq(packets[userData + 1], 0x8cu, "SPI_SHADER_USER_DATA_GS_0 offset");
    checkEq(packets[userData + 10], static_cast<uint32_t>(vertex), "sgpr16 = vertex base low");
    checkEq(packets[userData + 11], static_cast<uint32_t>(vertex >> 32), "sgpr17 = vertex base high");
    checkEq(packets[userData + 12], 20u, "sgpr18 = vertex stride");
    checkEq(packets[userData + 13], 3u, "sgpr19 = vertex count");
    check(!b.setVertexInputs(vertex, 20, ring + 1, ringSize, 1, 3, 1), "unaligned ring rejected");
    check(!b.setVertexInputs(vertex, 0, ring, ringSize, 1, 3, 1), "zero vertex stride rejected");
}

// Replay register writes so that assertions see the values at draw time,
// including later packets that may overwrite an otherwise correct setup.
static uint32_t registerAtDraw(const GraphicsPM4Builder& builder, uint32_t address) {
    size_t count = 0;
    const uint32_t* packets = builder.getPackets(&count);
    uint32_t value = 0xDEADBEEFu;
    for (size_t i = 0; i < count;) {
        const size_t length = ((packets[i] >> 16) & 0x3FFFu) + 2u;
        if (i + length > count) break;
        const uint32_t opcode = (packets[i] >> 8) & 0xFFu;
        if (opcode == GFX12::PKT3_DRAW_INDEX_AUTO) return value;
        const uint32_t base = opcode == GFX12::PKT3_SET_SH_REG ? 0xB000u :
                              opcode == GFX12::PKT3_SET_CONTEXT_REG ? 0x28000u :
                              opcode == GFX12::PKT3_SET_UCONFIG_REG ? 0x30000u : 0u;
        if (base) {
            const uint32_t start = base + packets[i + 1] * 4u;
            if (address >= start && (address - start) / 4u < length - 2u) {
                value = packets[i + 2u + (address - start) / 4u];
            }
        }
        i += length;
    }
    return value;
}

static void testDrawCountsPreserveVertexAddress() {
    printf("\n[draw counts preserve vertex input registers]\n");
    const bool layouts[] = {false, true};
    for (bool currentABI : layouts) {
        GraphicsPM4Builder b;
        if (!currentABI) b.featureMask &= ~16u;
        check(b.setVertexInputs(0x800E010000ull, 20, 0x400F00000ull,
                                1408 * 1024, 1, 6, 2), "bind six vertices");
        check(b.setDrawCounts(3, 1), "select a three-vertex draw");
        check(b.drawPrimitives(3), "append draw after updating counts");
        const uint32_t base = currentABI ? 0xB250u : 0xB244u;
        if (currentABI) {
            checkEq(registerAtDraw(b, 0xB230u), 0x00F00000u,
                    "attribute descriptor carries ring base low");
            // Stride 0 + swizzle_enable(3): ac_build_attr_ring_descriptor()
            // passes stride 0 and lets the 16-byte swizzle spread the index.
            checkEq(registerAtDraw(b, 0xB234u), 0xC0000004u,
                    "attribute descriptor carries zero stride and 16-byte swizzle");
            checkEq(registerAtDraw(b, 0xB238u), 1408u * 1024u,
                    "attribute descriptor carries ring size");
            checkEq(registerAtDraw(b, 0xB23Cu), 0x0043FFACu,
                    "attribute descriptor uses the GFX12 structured format");
        }
        checkEq(registerAtDraw(b, base), 0x0E010000u,
                "draw preserves vertex buffer address low");
        checkEq(registerAtDraw(b, base + 4), 0x80u,
                "draw preserves vertex buffer address high");
        checkEq(registerAtDraw(b, base + 8), 20u,
                "draw preserves vertex stride in bytes");
        checkEq(registerAtDraw(b, base + 12), 3u,
                "draw updates the vertex count to three");
        checkEq(registerAtDraw(b, base + 16), 1u,
                "draw updates the primitive count to one");
        // GE_GS_THROTTLE must keep the Mesa/Cobalt64 value. The old constant
        // 0x80 cleared NUM_INIT_GRPS[23:30], which is what seeds a subgroup.
        checkEq(registerAtDraw(b, GFX12::GE_GS_THROTTLE), 0x7f9a80e1u,
                "GE_GS_THROTTLE keeps NUM_INIT_GRPS, throttle disabled");
    }
}

static void testRasterDiscardDiagnostic() {
    printf("\n[raster discard diagnostic preserves shader state]\n");
    const uint32_t masks[] = {0u, 0xFFFFFFFFu};
    for (uint32_t preamble : masks) {
        for (unsigned discard = 0; discard < 2; ++discard) {
            GraphicsPM4Builder b;
            if (discard) b.featureMask |= 512u;
            check(b.emitGraphicsPreamble(preamble), "emit selected preamble");
            check(b.setPixelShader(0x800E001000ull, 0x2C0000u, 0x1000u),
                  "program pixel shader with raster discard diagnostic");
            check(b.drawPrimitives(3), "draw with diagnostic state");
            const uint32_t clip = (preamble ? 0x0c090000u : 0u) |
                                  (discard ? 0x400000u : 0u);
            checkEq(registerAtDraw(b, 0x28810u),
                    (preamble || discard) ? clip : 0xDEADBEEFu,
                    "raster discard changes only DX_RASTERIZATION_KILL");
            checkEq(registerAtDraw(b, 0xB020u), 0x800E0010u,
                    "diagnostic preserves PS program address");
            checkEq(registerAtDraw(b, 0xB028u), 0x2C0000u,
                    "diagnostic preserves PS resource settings");
        }
    }
}

static void testRenderTargetAndState() {
    printf("\n[render target and state]\n");
    GraphicsPM4Builder b;
    const uint32_t* p;
    size_t n = 0;

    b.reset();
    check(b.setColorAttachment(0, 0x80006000ull, 640, 480, GFX12::CB_FORMAT_8_8_8_8_UNORM),
          "setColorAttachment accepted");
    p = b.getPackets(&n);
    checkEq(static_cast<uint32_t>(n), 39,
            "nine colour-target packets + CB_COLOR_CONTROL + SPI_SHADER_COL_FORMAT + write masks");

    checkEq(p[0], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "CB_COLOR0_BASE header");
    checkEq(p[1], 0x318u, "CB_COLOR0_BASE offset");
    checkEq(p[2], 0x800060u, "color base holds addr >> 8");

    checkEq(p[3], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "CB_COLOR0_VIEW header");
    checkEq(p[4], 0x319u, "CB_COLOR0_VIEW offset");
    checkEq(p[5], 0u, "single slice");

    checkEq(p[6], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "CB_COLOR0_VIEW2 header");
    checkEq(p[7], 0x31au, "CB_COLOR0_VIEW2 offset");
    checkEq(p[8], 0u, "view2 = 0");

    checkEq(p[9], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "CB_COLOR0_ATTRIB offset");
    checkEq(p[10], 0x31bu, "CB_COLOR0_ATTRIB offset");
    checkEq(p[11], 0u, "no MSAA, no force-alpha");

    checkEq(p[12], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "CB_COLOR0_FDCC_CONTROL header");
    checkEq(p[13], 0x31cu, "CB_COLOR0_FDCC_CONTROL offset");
    checkEq(p[14], 0x10000004u, "FDCC control for uncompressed");

    checkEq(p[15], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "CB_COLOR0_ATTRIB2 header");
    checkEq(p[16], 0x31eu, "CB_COLOR0_ATTRIB2 offset");
    checkEq(p[17], (639u << 16) | 479u, "ATTRIB2 = (width-1) << 16 | (height-1)");

    checkEq(p[18], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "CB_COLOR0_ATTRIB3 header");
    checkEq(p[19], 0x31fu, "CB_COLOR0_ATTRIB3 offset");
    checkEq(p[20], 0x01000001u, "ATTRIB3 = one layer, 2D, linear");

    checkEq(p[21], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "CB_COLOR0_BASE_EXT header");
    checkEq(p[22], 0x390u, "CB_COLOR0_BASE_EXT offset");
    checkEq(p[23], 0u, "BASE_EXT = addr >> 40");

    checkEq(p[24], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "CB_COLOR0_INFO header");
    checkEq(p[25], 0x3b0u, "CB_COLOR0_INFO offset");
    checkEq(p[26], GFX12::CB_FORMAT_8_8_8_8_UNORM | (1u << 7), "color format + LINEAR_GENERAL");

    checkEq(p[27], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "CB_COLOR_CONTROL header");
    checkEq(p[28], 0x216u, "CB_COLOR_CONTROL offset");
    checkEq(p[29], (0xCCu << 16) | (GFX12::CB_MODE_NORMAL << 4), "CB_COLOR_CONTROL = normal mode + ROP3_COPY");

    checkEq(p[30], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "SPI_SHADER_COL_FORMAT header");
    checkEq(p[31], 0x195u, "SPI_SHADER_COL_FORMAT offset");
    // COL_FORMAT uses its own enum: 9 is 32_ABGR (what `exp mrt0` emits as four
    // 32-bit floats); the POS enum value 4 would read as FP16_ABGR here.
    checkEq(p[32], GFX12::SPI_SHADER_COL_32_ABGR, "colour export is 32_ABGR");

    b.reset();
    check(!b.setColorAttachment(0, 0x80006000ull, 640, 480,
                                GFX12::CB_FORMAT_8_8_8_8_UNORM, 4),
          "MSAA attachment rejected (encoding not verified)");
    check(!b.setColorAttachment(0, 0x80006000ull, 0, 480,
                                GFX12::CB_FORMAT_8_8_8_8_UNORM),
          "zero-width color target rejected");
    check(!b.setColorAttachment(0, 0x80006000ull, 640, 0,
                                GFX12::CB_FORMAT_8_8_8_8_UNORM),
          "zero-height color target rejected");
    check(!b.setColorAttachment(0, 0x80006000ull, 65537, 480,
                                GFX12::CB_FORMAT_8_8_8_8_UNORM),
          "color target wider than 16-bit size-1 rejected");

    b.reset();
    check(b.setColorAttachment(0, 0x80006000ull, 640, 480,
                                GFX12::CB_FORMAT_8_8_8_8_UNORM, 1, 1),
          "BGRA component swap accepted");
    p = b.getPackets(&n);
    checkEq(p[26], GFX12::CB_FORMAT_8_8_8_8_UNORM | (1u << 7) | (1u << 11),
            "CB_COLOR0_INFO COMP_SWAP = ALT + LINEAR_GENERAL");

    b.reset();
    check(b.setScissorRect(10, 20, 640, 480), "scissor rectangle accepted");
    p = b.getPackets(&n);
    checkEq(static_cast<uint32_t>(n), 12, "scissor writes four registers");
    checkEq(p[2], (20u << 16) | 10u, "scissor top-left is unchanged");
    checkEq(p[5], (20u << 16) | 10u, "viewport scissor top-left is unchanged");
    checkEq(p[8], (499u << 16) | 649u, "window bottom-right is inclusive");
    checkEq(p[11], (499u << 16) | 649u, "viewport bottom-right is inclusive");
    check(!b.setScissorRect(0, 0, 0, 480), "zero-width scissor rejected");
    check(!b.setScissorRect(0, 0, 640, 0), "zero-height scissor rejected");
    check(!b.setScissorRect(0xffff, 0, 2, 1), "scissor overflowing 16-bit BR rejected");

    b.reset();
    check(b.setBlendState(0, false, 0, 0, 0, 0, 0, 0), "blend disable accepted");
    p = b.getPackets(nullptr);
    checkEq(p[2], 0x210021u, "blend disable = ONE/ZERO/ADD");

    b.reset();
    check(b.setCullMode(1, 0, true), "setCullMode accepted");
    p = b.getPackets(nullptr);
    checkEq(p[1], 0x207u, "PA_SU_SC_MODE_CNTL == 0x207");
    checkEq(p[2], GFX12::SC_CULL_FRONT | GFX12::SC_FACE_CW, "cull front + CW face");
}

static void testSynchronisation() {
    printf("\n[synchronisation]\n");
    GraphicsPM4Builder b;
    const uint32_t* p;
    size_t n = 0;

    b.reset();
    check(b.emitCacheFlush(), "emitCacheFlush accepted");
    p = b.getPackets(&n);
    checkEq(static_cast<uint32_t>(n), 2u, "emitCacheFlush emits 1 EVENT_WRITE packet (2 dwords)");
    checkEq(p[0], header(GFX12::PKT3_EVENT_WRITE, 1), "CACHE_FLUSH_AND_INV_TS header");
    checkEq(p[1], GFX12::EVENT_TYPE_CACHE_FLUSH_AND_INV_TS |
                  (GFX12::EVENT_INDEX_CACHE_FLUSH_AND_INV_TS << 8), "CACHE_FLUSH_AND_INV_TS event");

    b.reset();
    check(b.acquireMem(), "acquireMem accepted");
    p = b.getPackets(&n);
    checkEq(static_cast<uint32_t>(n), 8u, "ACQUIRE_MEM is 8 dwords on GFX12");
    checkEq(p[0], 0xC0065800u, "ACQUIRE_MEM header covers GCR_CNTL");
    checkEq(p[1], 0u, "ACQUIRE_MEM uses PFP, PWS disabled");
    checkEq(p[2], 0xffffffffu, "ACQUIRE_MEM full-range size low");
    checkEq(p[3], 0x00ffffffu, "ACQUIRE_MEM full-range size high");
    checkEq(p[4], 0u, "ACQUIRE_MEM range base low");
    checkEq(p[5], 0u, "ACQUIRE_MEM range base high");
    checkEq(p[6], 10u, "ACQUIRE_MEM poll interval");
    // GFX12 packet GCR_CNTL, not the pre-GFX10 CP_COHER_CNTL layout:
    // GLI_INV=ALL [1:0], GLK_INV [7], GLV_INV [8], GL2_INV [14], GL2_WB [15].
    // Using the old high action bits leaves GLI_INV=0 and executes stale ISA
    // when the next pipeline reuses an uploaded shader's GPU address.
    checkEq(p[7], 0x0000c181u, "invalidate shader/data caches with GFX12 GCR fields");

    b.reset();
    check(b.releaseMem(0x40008000ull, 42), "releaseMem accepted");
    p = b.getPackets(&n);
    checkEq(static_cast<uint32_t>(n), 8, "RELEASE_MEM is 8 dwords");
    checkEq(p[0], header(GFX12::PKT3_RELEASE_MEM, 7), "RELEASE_MEM header");
    checkEq(p[3], 0x40008000u, "fence address low");
    checkEq(p[4], 0u, "fence address high");
    checkEq(p[5], 42u, "fence value low");
    checkEq(p[6], 0u, "fence value high");
}

static void testPreambleCoversWholeTable() {
    printf("\n[preamble covers the whole table]\n");
    GraphicsPM4Builder b;
    check(b.emitGraphicsPreamble(), "full preamble accepted");
    // The table is longer than 64 entries. A 32-bit selection mask could only
    // reach the first 32, so "all bits set" still skipped more than half of the
    // state - including GE_MAX_VTX_INDX and the launch-guarantee pair.
    checkEq(registerAtDraw(b, 0x28040u), 0u, "entry 22 DB_GL1_INTERFACE_CONTROL emitted");
    checkEq(registerAtDraw(b, 0x28b50u), 0x78808080u, "entry 52 VGT_TESS_DISTRIBUTION emitted");
    checkEq(registerAtDraw(b, 0x30950u), 0x7f9a80e1u, "entry 63 GE_GS_THROTTLE emitted");
    checkEq(registerAtDraw(b, 0x30964u), 0xffffffffu, "entry 64 GE_MAX_VTX_INDX emitted");
    checkEq(registerAtDraw(b, 0x31128u), 0x00008a4du, "entry 71 SPI_GRP_LAUNCH_GUARANTEE_ENABLE emitted");
    checkEq(registerAtDraw(b, 0x3112cu), 0x00401123u, "entry 72 SPI_GRP_LAUNCH_GUARANTEE_CTRL emitted");
    checkEq(registerAtDraw(b, 0x0b0c8u), 0u, "trailing accumulator run emitted");

    GraphicsPM4Builder masked;
    check(masked.emitGraphicsPreamble(0u, 0u), "empty selection accepted");
    checkEq(registerAtDraw(masked, 0x30964u), 0xDEADBEEFu,
            "entry 64 skipped when its mask bit is clear");
    checkEq(registerAtDraw(masked, 0x0b0c8u), 0xDEADBEEFu,
            "trailing group skipped when its mask bit is clear");
    checkEq(registerAtDraw(masked, 0x28204u), 0xDEADBEEFu,
            "entry 5 skipped when its mask bit is clear");
}

static void testPreambleInitialisesPowerOnGarbage() {
    printf("\n[preamble initialises power-on garbage that blocks fragments]\n");
    // gfx12 has no CLEAR_STATE. These registers read back as random values
    // after every cold boot (two boots gave e.g. PA_SU_HARDWARE_SCREEN_OFFSET
    // 0x0c62040e and DB_EQAA 0x00134504) and each one, left alone, stopped the
    // triangle on real hardware: the screen offsets moved it off the target,
    // the DB state made the depth block write to an unmapped surface, and
    // DB_EQAA/DB_ALPHA_TO_MASK made the DB swallow every fragment before SPI
    // ever saw a pixel wave. Values are the ones Mesa programs for a
    // single-sample pass with no depth attachment.
    GraphicsPM4Builder b;
    check(b.emitGraphicsPreamble(), "full preamble accepted");

    checkEq(registerAtDraw(b, 0x28234u), 0u, "PA_SU_HARDWARE_SCREEN_OFFSET = 0");
    checkEq(registerAtDraw(b, 0x28200u), 0u, "PA_SC_WINDOW_OFFSET = 0");
    checkEq(registerAtDraw(b, 0x28c54u), 0x00100000u,
            "PA_SC_CONSERVATIVE_RASTERIZATION_CNTL = NULL_SQUAD_AA_MASK_ENABLE");

    checkEq(registerAtDraw(b, GFX12::DB_Z_INFO), 0u, "DB_Z_INFO = Z_INVALID");
    checkEq(registerAtDraw(b, GFX12::DB_STENCIL_INFO), 0x20000000u,
            "DB_STENCIL_INFO = STENCIL_INVALID | TILE_STENCIL_DISABLE");
    checkEq(registerAtDraw(b, 0x2800cu), 0u, "DB_RENDER_OVERRIDE = 0");
    checkEq(registerAtDraw(b, 0x28010u), 0x08000000u,
            "DB_RENDER_OVERRIDE2 = CENTROID_COMPUTATION_MODE(1)");

    checkEq(registerAtDraw(b, 0x28078u), 0u, "DB_EQAA = 0 (single sample)");
    checkEq(registerAtDraw(b, 0x2807cu), 0x00018700u,
            "DB_ALPHA_TO_MASK = Mesa offsets, ALPHA_TO_MASK_ENABLE off");
}

static void testColorAttachmentWritesGfx12AddressAndSwizzle() {
    printf("\n[colour attachment writes BASE_EXT and ATTRIB3]\n");
    // CB_COLOR0_BASE holds address bits [39:8]; bits [47:40] live in
    // CB_COLOR0_BASE_EXT and the swizzle mode in CB_COLOR0_ATTRIB3. Neither was
    // written, so the CB took a power-on value for both: the first fragments
    // ever produced faulted at an address with the right low 40 bits and a
    // random top byte (0xba800f045000, 0x81800ecb8000 on two boots).
    GraphicsPM4Builder b;
    check(b.setColorAttachment(0, 0x80006000ull, 640, 480, GFX12::CB_FORMAT_8_8_8_8_UNORM),
          "setColorAttachment accepted");
    checkEq(registerAtDraw(b, 0x28e40u), 0u, "CB_COLOR0_BASE_EXT = addr >> 40");
    checkEq(registerAtDraw(b, 0x28c7cu), 0x01000001u,
            "CB_COLOR0_ATTRIB3 = MIP0_DEPTH(1) | RESOURCE_TYPE(2D) | LINEAR");

    GraphicsPM4Builder high;
    check(high.setColorAttachment(1, 0xAB0080006000ull, 640, 480, GFX12::CB_FORMAT_8_8_8_8_UNORM),
          "setColorAttachment with address above 40 bits accepted");
    checkEq(registerAtDraw(high, 0x28e44u), 0xABu, "CB_COLOR1_BASE_EXT = addr >> 40");
    checkEq(registerAtDraw(high, 0x28ca0u), 0x01000001u, "CB_COLOR1_ATTRIB3 at +0x24 stride");
}

static void testNumInstancesPacket() {
    printf("\n[instance count packet]\n");
    GraphicsPM4Builder b;
    const uint32_t* p;
    b.reset();
    check(b.setNumInstances(1), "setNumInstances accepted");
    p = b.getPackets(nullptr);
    checkEq(GFX12::PKT3_NUM_INSTANCES, 0x2fu, "PKT3_NUM_INSTANCES is 0x2F (0x2A is INDEX_TYPE)");
    checkEq(p[0], header(0x2fu, 1), "NUM_INSTANCES header");
    checkEq(p[1], 1u, "instance count payload");
}

static void testOverflow() {
    printf("\n[overflow]\n");
    GraphicsPM4Builder b;
    b.reset();
    bool all = true;
    for (int i = 0; i < 2000 && all; i++) {
        all = b.setReg(GFX12::PA_CL_VPORT_XSCALE, static_cast<uint32_t>(i));
    }
    check(!all, "builder stops accepting once full");
    check(b.overflowed(), "overflow flag raised");
}

int main() {
    printf("GraphicsPM4Builder packet checks\n");
    testRegisterOffsetConversion();
    testSequencePacket();
    testDraws();
    testShaderPrograms();
    testVertexInputs();
    testDrawCountsPreserveVertexAddress();
    testRasterDiscardDiagnostic();
    testRenderTargetAndState();
    testSynchronisation();
    testPreambleCoversWholeTable();
    testPreambleInitialisesPowerOnGarbage();
    testColorAttachmentWritesGfx12AddressAndSwizzle();
    testNumInstancesPacket();
    testOverflow();

    printf("\nRESULT: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
