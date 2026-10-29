#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <ostream>

namespace air2gfx12 {

// GFX12 PM4 Type-3 Opcodes
namespace PM4Op {
    constexpr uint8_t NOP                  = 0x10;
    constexpr uint8_t SET_SH_REG           = 0x76;
    constexpr uint8_t SET_CONTEXT_REG      = 0x69;
    constexpr uint8_t SET_UCONFIG_REG      = 0x79;
    constexpr uint8_t DISPATCH_DIRECT      = 0x15;
    constexpr uint8_t DRAW_INDEX_AUTO      = 0x2D;
    constexpr uint8_t EVENT_WRITE          = 0x46;
    constexpr uint8_t ACQUIRE_MEM          = 0x58;
    constexpr uint8_t RELEASE_MEM          = 0x59;
}

// GFX12 Register Offsets (in SH_REG / CONTEXT_REG dword spaces)
namespace GFX12Reg {
    // Compute SH registers (relative to 0xB000 / 0x2C00)
    constexpr uint32_t COMPUTE_PGM_LO          = 0x20c;
    constexpr uint32_t COMPUTE_PGM_HI          = 0x20d;
    constexpr uint32_t COMPUTE_PGM_RSRC1       = 0x212;
    constexpr uint32_t COMPUTE_PGM_RSRC2       = 0x213;
    constexpr uint32_t COMPUTE_NUM_THREAD_X    = 0x215;
    constexpr uint32_t COMPUTE_NUM_THREAD_Y    = 0x216;
    constexpr uint32_t COMPUTE_NUM_THREAD_Z    = 0x217;
    constexpr uint32_t COMPUTE_USER_DATA_0     = 0x240; // s[0:1], s[2:3] up to s[30:31]

    // Context registers
    constexpr uint32_t PA_SC_SCREEN_SCISSOR_TL = 0x200;
    constexpr uint32_t PA_SC_SCREEN_SCISSOR_BR = 0x201;
    constexpr uint32_t VGT_PRIMITIVE_TYPE      = 0x242;
}

// Compute Dispatch Initiator Flags
namespace DispatchInitiator {
    constexpr uint32_t COMPUTE_SHADER_EN   = (1u << 0);
    constexpr uint32_t FORCE_START_AT_000  = (1u << 2);
    constexpr uint32_t ORDER_MODE          = (0u << 6);
    constexpr uint32_t CS_W32_EN           = (1u << 18); // Wave32 mode
    constexpr uint32_t DEFAULT_INITIATOR   = COMPUTE_SHADER_EN | FORCE_START_AT_000 | CS_W32_EN;
}

class PM4Builder {
public:
    PM4Builder() = default;

    // Clear builder buffer
    void clear();

    // Raw Type-3 packet encoding
    void writeHeader(uint8_t opcode, uint16_t count, uint8_t flags = 0);
    void writeDword(uint32_t val);
    void writeDwords(const uint32_t* vals, size_t count);

    // Common packet emitters
    void emitSetShReg(uint32_t regOffset, const uint32_t* values, uint32_t count);
    void emitSetShReg1(uint32_t regOffset, uint32_t value);
    void emitSetContextReg(uint32_t regOffset, const uint32_t* values, uint32_t count);
    void emitSetContextReg1(uint32_t regOffset, uint32_t value);
    void emitNop(uint32_t count = 0);

    // High-Level Compute Pipeline Builders
    void setComputeProgram(uint64_t gpuVA, uint32_t rsrc1, uint32_t rsrc2);
    void setComputeWorkgroupSize(uint32_t x, uint32_t y = 1, uint32_t z = 1);
    void setComputeUserData(uint32_t userSgprIndex, uint64_t gpuAddress);
    void emitDispatchDirect(uint32_t gridX, uint32_t gridY = 1, uint32_t gridZ = 1,
                            uint32_t initiator = DispatchInitiator::DEFAULT_INITIATOR);
    void emitCsPartialFlush();

    // Inspection
    const std::vector<uint32_t>& getPackets() const { return mBuffer; }
    size_t sizeDwords() const { return mBuffer.size(); }
    size_t sizeBytes() const { return mBuffer.size() * sizeof(uint32_t); }
    void dump(std::ostream& os) const;

private:
    std::vector<uint32_t> mBuffer;
};

} // namespace air2gfx12
