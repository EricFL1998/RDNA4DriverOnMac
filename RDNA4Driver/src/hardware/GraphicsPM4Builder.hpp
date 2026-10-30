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
                            uint32_t sampleCount = 1);
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
    bool setPixelShader(uint64_t gpuAddr, uint32_t rsrc1, uint32_t rsrc2);
    bool setShaderUserData(uint32_t userDataBase, uint32_t index, uint64_t value);

    // MTLPrimitiveType / MTLIndexType, translated by the caller.
    bool setPrimitiveType(uint32_t diPrimType);
    bool setIndexType(uint32_t vgtIndexType);

    // --- draws -------------------------------------------------------------
    bool drawPrimitives(uint32_t vertexCount, bool opaque = false);
    bool drawIndexedPrimitives(uint32_t indexCount, uint64_t indexGpuAddr,
                               uint32_t maxIndexCount, bool opaque = false);

    // --- synchronisation ---------------------------------------------------
    bool emitCacheFlush();
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
