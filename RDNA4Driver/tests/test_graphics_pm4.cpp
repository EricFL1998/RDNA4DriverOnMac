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
    checkEq(static_cast<uint32_t>(n), 8, "viewport is one 8-dword packet");
    checkEq(p[0], header(GFX12::PKT3_SET_CONTEXT_REG, 7), "viewport header covers 6 values");
    checkEq(p[1], 0x10fu, "viewport starts at PA_CL_VPORT_XSCALE");
    checkEq(p[2], 0x43a00000u, "xscale = 320.0");
    checkEq(p[3], 0x43a00000u, "xoffset = 320.0");
    checkEq(p[4], 0xc3700000u, "yscale = -240.0 (Metal Y-down)");
    checkEq(p[5], 0x43700000u, "yoffset = 240.0");

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

    // Pixel shader: LO/HI/RSRC1/RSRC2 are adjacent, so one packet.
    b.reset();
    check(b.setPixelShader(0x800e002000ull, 0x2c0000u, 0x1000u), "setPixelShader accepted");
    p = b.getPackets(&n);
    checkEq(static_cast<uint32_t>(n), 6, "PS program is one 6-dword packet");
    checkEq(p[0], header(GFX12::PKT3_SET_SH_REG, 5), "PS header covers 4 values");
    checkEq(p[1], 0x08u, "PS header offset");
    checkEq(p[2], 0x800e0020u, "PS address bits [39:8]");
    checkEq(p[3], 0x00u, "PS address bits [47:40] (VRAM below 2^40)");

    // The [47:40] half only shows up above 2^40; prove the split works there.
    b.reset();
    check(b.setPixelShader(0x80800e002000ull, 0, 0), "high-address pixel shader accepted");
    p = b.getPackets(&n);
    checkEq(p[2], 0x800e0020u, "high address bits [39:8] unchanged");
    checkEq(p[3], 0x80u, "high address bits [47:40]");

    // Vertex shader on gfx12 is the NGG ES/GS pair, and the runs are not
    // contiguous, so it takes four packets.
    b.reset();
    check(b.setVertexShader(0x800e004000ull, 0x2c0000u, 0x1060u), "setVertexShader accepted");
    p = b.getPackets(&n);
    checkEq(static_cast<uint32_t>(n), 14, "VS is GS pair + ES lo + ES hi + GS rsrc");
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
    checkEq(static_cast<uint32_t>(n), 15, "five 3-dword register packets");

    checkEq(p[0], header(GFX12::PKT3_SET_CONTEXT_REG, 2), "CB_COLOR0_BASE header");
    checkEq(p[1], 0x318u, "CB_COLOR0_BASE offset");
    checkEq(p[2], 0x800060u, "color base holds addr >> 8");

    checkEq(p[4], 0x319u, "CB_COLOR0_VIEW offset");
    checkEq(p[5], 0u, "single slice");

    checkEq(p[7], 0x31bu, "CB_COLOR0_ATTRIB offset");
    checkEq(p[8], 0u, "no MSAA, no force-alpha");

    checkEq(p[10], 0x31eu, "CB_COLOR0_ATTRIB2 offset");
    checkEq(p[11], (640u << 16) | 480u, "ATTRIB2 = width << 16 | height");

    checkEq(p[13], 0x3b0u, "CB_COLOR0_INFO offset");
    checkEq(p[14], GFX12::CB_FORMAT_8_8_8_8_UNORM, "color format");

    b.reset();
    check(!b.setColorAttachment(0, 0x80006000ull, 640, 480,
                                GFX12::CB_FORMAT_8_8_8_8_UNORM, 4),
          "MSAA attachment rejected (encoding not verified)");

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
    p = b.getPackets(nullptr);
    checkEq(p[0], header(GFX12::PKT3_EVENT_WRITE, 1), "EVENT_WRITE header (1 payload dword)");
    checkEq(p[1], GFX12::EVENT_TYPE_CACHE_FLUSH_AND_INV_TS |
                  (GFX12::EVENT_INDEX_CACHE_FLUSH_AND_INV_TS << 8), "event type/index");

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
    testRenderTargetAndState();
    testSynchronisation();
    testOverflow();

    printf("\nRESULT: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
