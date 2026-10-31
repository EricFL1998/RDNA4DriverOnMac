//
//  GraphicsPM4Builder.hpp
//  RDNA4Driver - PM4 encoder for the native Metal graphics path
//
//  The API mirrors what MTLRenderCommandEncoder does, not what a Vulkan
//  pipeline does: viewport, scissor, render target, blend, cull, shader
//  programs, draw. Everything below it is plain PM4, so the same encoder can
//  be compiled into the kext and into RDNA4Metal.bundle; it has no IOKit
//  dependency and never allocates.
//

#ifndef GraphicsPM4Builder_hpp
#define GraphicsPM4Builder_hpp

#include <stdint.h>
#include <stddef.h>
#include "GraphicsRegs.hpp"

class GraphicsPM4Builder {
public:
    static const size_t kCapacityDwords = 4096;

    GraphicsPM4Builder();

    void reset();
    const uint32_t* getPackets(size_t* outDwords) const;
    bool overflowed() const { return overflow; }

    // Raw register write. `addr` is a GFX12:: address; the packet type and the
    // block-relative offset are derived from it, so a wrong offset cannot be
    // expressed through this interface.
    bool setReg(uint32_t addr, uint32_t value);
    bool setRegSeq(uint32_t addr, const uint32_t* values, size_t valueCount);

    // --- MTLRenderCommandEncoder equivalents -------------------------------
    bool setViewport(float originX, float originY, float width, float height,
                     float znear, float zfar);
    bool setScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

    // `cbFormat` is a GFX12::CB_FORMAT_* value taken from the pipeline's
    // MTLPixelFormat. gfx12 has no PITCH/SLICE registers: the size goes into
    // CB_COLORn_ATTRIB2 and the hardware derives pitch from the format.
    bool setColorAttachment(uint32_t index, uint64_t gpuAddr, uint32_t width,
                            uint32_t height, uint32_t cbFormat,
                            uint32_t sampleCount = 1, uint32_t componentSwap = 0);
    bool setColorWriteMask(uint32_t targetMask, uint32_t shaderMask);
    bool setBlendState(uint32_t index, bool enable,
                       uint32_t srcColor, uint32_t dstColor, uint32_t colorOp,
                       uint32_t srcAlpha, uint32_t dstAlpha, uint32_t alphaOp);
    bool setCullMode(uint32_t cullFront, uint32_t cullBack, bool frontFaceCW);
    bool setDepthDisabled();

    // Faithful Metal programs have no separate vertex stage on gfx12: the VS
    // runs as the NGG ES/GS pair, so the same address is programmed into both
    // and the resource registers belong to GS. VGT_SHADER_STAGES_EN on gfx12
    // exposes GS_EN only.
    bool setVertexShader(uint64_t gpuAddr, uint32_t gsRsrc1, uint32_t gsRsrc2);
    bool setVertexInputs(uint64_t vertexBuffer, uint32_t stride,
                         uint64_t ringBuffer, uint32_t ringSize,
                         uint32_t parameterCount, uint32_t vertexCount,
                         uint32_t primitiveCount);
    bool setDrawCounts(uint32_t vertexCount, uint32_t primitiveCount);
    bool setPixelShader(uint64_t gpuAddr, uint32_t rsrc1, uint32_t rsrc2);
    bool setShaderUserData(uint32_t userDataBase, uint32_t index, uint64_t value);

    // MTLPrimitiveType / MTLIndexType, translated by the caller.
    bool setPrimitiveType(uint32_t diPrimType);
    bool setIndexType(uint32_t vgtIndexType);

    // --- draws -------------------------------------------------------------
    // PKT3_NUM_INSTANCES: what the CP actually consumes for a draw.
    bool setNumInstances(uint32_t count);
    bool drawPrimitives(uint32_t vertexCount, bool opaque = false);
    bool drawIndexedPrimitives(uint32_t indexCount, uint64_t indexGpuAddr,
                               uint32_t maxIndexCount, bool opaque = false);

    // --- synchronisation ---------------------------------------------------
    bool emitCacheFlush();
    // gfx12 dropped CLEAR_STATE (Mesa asserts gfx_level < GFX12 in
    // ac_set_tracked_regs_to_clear_state and threads gfx12 through its own
    // ac_init_graphics_preamble_state). Every context register therefore keeps
    // its reset value until this preamble writes it, so this must run once per
    // command buffer before any draw.
    // `mask` exists only for bisecting which entry wedges a queue; callers pass
    // all bits. It must be at least as wide as the table: kPreamble has more
    // than 32 entries, and a 32-bit mask silently strands every entry past the
    // first word - including GE_MAX_VTX_INDX, PA_SC_NGG_MODE_CNTL and
    // SPI_GRP_LAUNCH_GUARANTEE_*, so a "full" preamble would still be partial.
    static constexpr uint64_t kPreambleAll = ~static_cast<uint64_t>(0);
    bool emitGraphicsPreamble(uint64_t mask = kPreambleAll,
                              uint64_t maskHigh = kPreambleAll);
    // Per-entry bisect switch for the branches outside the preamble table
    // (bit 2/4 = colour state, bit 4 = 20-word user data, bit 8 = NUM_INSTANCES
    // packet, bit 9 = raster discard).
    uint32_t featureMask = 0x9Fu;
    // Enable per-context register load/shadow so the CP applies the
    // SET_CONTEXT_REG updates that follow. Without this the front end never
    // sees the graphics state and no vertex wave is dispatched.
    bool emitContextControl();
    // Invalidate the shader instruction/constant caches so the SPI runs the code
    // that was just uploaded, not whatever the cache still holds.
    bool acquireMem();
    bool releaseMem(uint64_t gpuAddr, uint64_t value);

private:
    uint32_t packets[kCapacityDwords];
    size_t count;
    bool overflow;

    static uint32_t type3Header(uint32_t opcode, uint32_t payloadDwords);
    bool appendDword(uint32_t value);
    bool appendRegPacket(uint32_t addr, const uint32_t* values, size_t valueCount);
    bool appendDrawInitiator(uint32_t sourceSelect, bool opaque);
};

#endif /* GraphicsPM4Builder_hpp */
