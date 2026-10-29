#include "PM4Builder.hpp"
#include <iomanip>

namespace air2gfx12 {

void PM4Builder::clear() {
    mBuffer.clear();
}

void PM4Builder::writeHeader(uint8_t opcode, uint16_t count, uint8_t flags) {
    // Type 3 Packet Header format:
    // [31:30] = 3 (Type-3)
    // [29:16] = count - 1 (number of dwords following the header, minus 1)
    // [15:8]  = opcode
    // [7:0]   = flags / predicate
    uint32_t header = (3u << 30) |
                      ((static_cast<uint32_t>(count - 1) & 0x3FFFu) << 16) |
                      ((static_cast<uint32_t>(opcode) & 0xFFu) << 8) |
                      (static_cast<uint32_t>(flags) & 0xFFu);
    mBuffer.push_back(header);
}

void PM4Builder::writeDword(uint32_t val) {
    mBuffer.push_back(val);
}

void PM4Builder::writeDwords(const uint32_t* vals, size_t count) {
    if (vals && count > 0) {
        mBuffer.insert(mBuffer.end(), vals, vals + count);
    }
}

void PM4Builder::emitSetShReg(uint32_t regOffset, const uint32_t* values, uint32_t count) {
    if (!values || count == 0) return;
    // SET_SH_REG payload:
    // Dword 0: regOffset
    // Dword 1..count: values
    // Total count in header = 1 + count
    writeHeader(PM4Op::SET_SH_REG, 1 + count);
    writeDword(regOffset);
    writeDwords(values, count);
}

void PM4Builder::emitSetShReg1(uint32_t regOffset, uint32_t value) {
    emitSetShReg(regOffset, &value, 1);
}

void PM4Builder::emitSetContextReg(uint32_t regOffset, const uint32_t* values, uint32_t count) {
    if (!values || count == 0) return;
    writeHeader(PM4Op::SET_CONTEXT_REG, 1 + count);
    writeDword(regOffset);
    writeDwords(values, count);
}

void PM4Builder::emitSetContextReg1(uint32_t regOffset, uint32_t value) {
    emitSetContextReg(regOffset, &value, 1);
}

void PM4Builder::emitNop(uint32_t count) {
    writeHeader(PM4Op::NOP, count + 1);
    for (uint32_t i = 0; i < count; ++i) {
        writeDword(0);
    }
}

void PM4Builder::setComputeProgram(uint64_t gpuVA, uint32_t rsrc1, uint32_t rsrc2) {
    // 1. Program Base Address (COMPUTE_PGM_LO / HI)
    // GPU VA must be 256-byte aligned; lower 8 bits are dropped
    uint32_t pgmLo = static_cast<uint32_t>(gpuVA >> 8);
    uint32_t pgmHi = static_cast<uint32_t>(gpuVA >> 40);
    uint32_t pgmAddr[2] = { pgmLo, pgmHi };
    emitSetShReg(GFX12Reg::COMPUTE_PGM_LO, pgmAddr, 2);

    // 2. Program Resources (COMPUTE_PGM_RSRC1 / RSRC2)
    uint32_t rsrc[2] = { rsrc1, rsrc2 };
    emitSetShReg(GFX12Reg::COMPUTE_PGM_RSRC1, rsrc, 2);
}

void PM4Builder::setComputeWorkgroupSize(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t threads[3] = { x, y, z };
    emitSetShReg(GFX12Reg::COMPUTE_NUM_THREAD_X, threads, 3);
}

void PM4Builder::setComputeUserData(uint32_t userSgprIndex, uint64_t gpuAddress) {
    uint32_t regOffset = GFX12Reg::COMPUTE_USER_DATA_0 + (userSgprIndex * 2);
    uint32_t addr[2] = {
        static_cast<uint32_t>(gpuAddress & 0xFFFFFFFF),
        static_cast<uint32_t>(gpuAddress >> 32)
    };
    emitSetShReg(regOffset, addr, 2);
}

void PM4Builder::emitDispatchDirect(uint32_t gridX, uint32_t gridY, uint32_t gridZ, uint32_t initiator) {
    // DISPATCH_DIRECT packet: count = 4 dwords (gridX, gridY, gridZ, initiator)
    // Flags byte = 1 (Compute shader type flag)
    writeHeader(PM4Op::DISPATCH_DIRECT, 4, 1);
    writeDword(gridX);
    writeDword(gridY);
    writeDword(gridZ);
    writeDword(initiator);
}

void PM4Builder::emitCsPartialFlush() {
    // EVENT_WRITE: event type = 7 (CS_PARTIAL_FLUSH), event index = 4
    uint32_t eventData = 7 | (4 << 8);
    writeHeader(PM4Op::EVENT_WRITE, 1, 0);
    writeDword(eventData);
}

void PM4Builder::dump(std::ostream& os) const {
    os << "PM4 Packet Stream (" << mBuffer.size() << " dwords / " << sizeBytes() << " bytes):\n";
    for (size_t i = 0; i < mBuffer.size(); ++i) {
        os << "  [0x" << std::hex << std::setw(4) << std::setfill('0') << i
           << "] 0x" << std::setw(8) << std::setfill('0') << mBuffer[i]
           << std::dec << "\n";
    }
}

} // namespace air2gfx12
