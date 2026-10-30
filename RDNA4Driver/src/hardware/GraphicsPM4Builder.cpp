//
//  GraphicsPM4Builder.cpp
//  RDNA4Driver - PM4 encoder for the native Metal graphics path
//

#include "GraphicsPM4Builder.hpp"

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
    const float zscale = (zfar - znear) * 0.5f;
    const float zoffset = (zfar + znear) * 0.5f;

    const uint32_t values[6] = {
        floatBits(xscale), floatBits(xoffset),
        floatBits(yscale), floatBits(yoffset),
        floatBits(zscale), floatBits(zoffset),
    };
    return setRegSeq(GFX12::PA_CL_VPORT_XSCALE, values, 6);
}

bool GraphicsPM4Builder::setScissorRect(uint32_t x, uint32_t y,
                                        uint32_t width, uint32_t height) {
    const uint32_t tl = (y << 16) | (x & 0xFFFFu);
    const uint32_t br = ((y + height) << 16) | ((x + width) & 0xFFFFu);
    if (!setReg(GFX12::PA_SC_WINDOW_SCISSOR_TL, tl)) return false;
    if (!setReg(GFX12::PA_SC_VPORT_SCISSOR_0_TL, tl)) return false;
    if (!setReg(GFX12::PA_SC_WINDOW_SCISSOR_BR, br)) return false;
    return setReg(GFX12::PA_SC_VPORT_SCISSOR_0_BR, br);
}

bool GraphicsPM4Builder::setColorAttachment(uint32_t index, uint64_t gpuAddr,
                                            uint32_t width, uint32_t height,
                                            uint32_t cbFormat, uint32_t sampleCount) {
    if (index >= 8) return false;
    // MSAA needs the fragment count and the tile fields of ATTRIB; only the
    // single-sample encoding is verified here.
    if (sampleCount != 1) return false;

    const uint32_t base = GFX12::CB_COLOR0_BASE + index * GFX12::CB_COLOR_TARGET_STRIDE;
    const uint32_t view = GFX12::CB_COLOR0_VIEW + index * GFX12::CB_COLOR_TARGET_STRIDE;
    const uint32_t attrib = GFX12::CB_COLOR0_ATTRIB + index * GFX12::CB_COLOR_TARGET_STRIDE;
    const uint32_t attrib2 = GFX12::CB_COLOR0_ATTRIB2 + index * GFX12::CB_COLOR_TARGET_STRIDE;
    const uint32_t info = GFX12::CB_COLOR0_INFO + index * GFX12::CB_COLOR_INFO_STRIDE;

    // CB_COLOR0_BASE holds bits [47:8] of the address.
    if (!setReg(base, static_cast<uint32_t>(gpuAddr >> 8))) return false;
    if (!setReg(view, 0)) return false;                    // SLICE_START/SLICE_MAX
    if (!setReg(attrib, 0)) return false;                  // no MSAA, no force-alpha
    if (!setReg(attrib2, (width << 16) | (height & 0xFFFFu))) return false;
    return setReg(info, cbFormat & 0x1Fu);
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
    return setRegSeq(GFX12::SPI_SHADER_PGM_RSRC1_GS, gsResources, 2);
}

bool GraphicsPM4Builder::setPixelShader(uint64_t gpuAddr, uint32_t rsrc1,
                                        uint32_t rsrc2) {
    const uint32_t lo = static_cast<uint32_t>(gpuAddr >> 8);
    const uint32_t hi = static_cast<uint32_t>((gpuAddr >> 40) & 0xFFu);
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

bool GraphicsPM4Builder::emitCacheFlush() {
    const uint32_t event = (GFX12::EVENT_TYPE_CACHE_FLUSH_AND_INV_TS & 0x3Fu) |
                           ((GFX12::EVENT_INDEX_CACHE_FLUSH_AND_INV_TS & 0xFu) << 8);
    if (!appendDword(type3Header(GFX12::PKT3_EVENT_WRITE, 1))) return false;
    return appendDword(event);
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
