#include "MTLBUnpacker.hpp"
#include <cstring>
#include <iostream>

namespace air2gfx12 {

bool MTLBUnpacker::isMTLB(const uint8_t* data, size_t size) {
    if (!data || size < 4) return false;
    return (data[0] == 'M' && data[1] == 'T' && data[2] == 'L' && data[3] == 'B');
}

bool MTLBUnpacker::isRawAIR(const uint8_t* data, size_t size) {
    if (!data || size < 4) return false;
    // Check for LLVM bitcode magic (0x0b17c0de wrapper or 0xdec04342 / 'BC\xc0\xde')
    if (data[0] == 0xde && data[1] == 0xc0 && data[2] == 0x17 && data[3] == 0x0b) return true;
    if (data[0] == 'B' && data[1] == 'C' && data[2] == 0xc0 && data[3] == 0xde) return true;
    return false;
}

template <typename T>
static T readLE(const uint8_t* ptr) {
    T val = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        val |= (static_cast<T>(ptr[i]) << (8 * i));
    }
    return val;
}

bool MTLBUnpacker::unpack(const uint8_t* data, size_t size, std::vector<FunctionEntry>& outEntries) {
    outEntries.clear();
    if (!data || size < 0x58) return false;

    // Handle raw AIR bitcode file
    if (isRawAIR(data, size)) {
        FunctionEntry entry;
        entry.name = "main";
        entry.type = FunctionType::Compute;
        entry.bitcode.assign(data, data + size);
        outEntries.push_back(std::move(entry));
        return true;
    }

    if (!isMTLB(data, size)) {
        return false;
    }

    // Read offsets from MTLB header
    uint64_t entriesOff = readLE<uint64_t>(data + 0x18);
    uint64_t bitcodeOff = readLE<uint64_t>(data + 0x48);

    if (entriesOff >= size || bitcodeOff >= size) {
        return false;
    }

    size_t pos = entriesOff;
    if (pos + 4 > size) return false;

    uint32_t numEntries = readLE<uint32_t>(data + pos);
    pos += 4;

    for (uint32_t i = 0; i < numEntries; ++i) {
        if (pos + 4 > size) return false;
        uint32_t entrySz = readLE<uint32_t>(data + pos);
        if (pos + entrySz > size || entrySz < 4) return false;

        const uint8_t* entryBytes = data + pos + 4;
        size_t entryBytesLen = entrySz - 4;
        pos += entrySz;

        size_t tpos = 0;
        std::string name;
        FunctionType fnType = FunctionType::Unknown;
        uint64_t bcOffRel = 0;
        uint64_t bcSize = 0;

        while (tpos + 4 <= entryBytesLen) {
            if (entryBytes[tpos] == 'E' && entryBytes[tpos+1] == 'N' &&
                entryBytes[tpos+2] == 'D' && entryBytes[tpos+3] == 'T') {
                break;
            }

            if (tpos + 6 > entryBytesLen) break;
            uint16_t tagLen = readLE<uint16_t>(entryBytes + tpos + 4);
            const uint8_t* payload = entryBytes + tpos + 6;
            if (tpos + 6 + tagLen > entryBytesLen) break;

            if (memcmp(entryBytes + tpos, "NAME", 4) == 0) {
                // String with optional null terminator
                size_t strLen = tagLen;
                if (strLen > 0 && payload[strLen - 1] == '\0') strLen--;
                name = std::string(reinterpret_cast<const char*>(payload), strLen);
            } else if (memcmp(entryBytes + tpos, "TYPE", 4) == 0 && tagLen >= 1) {
                uint8_t t = payload[0];
                if (t == 0) fnType = FunctionType::Vertex;
                else if (t == 1) fnType = FunctionType::Fragment;
                else if (t == 2) fnType = FunctionType::Compute;
            } else if (memcmp(entryBytes + tpos, "MDSZ", 4) == 0 && tagLen >= 8) {
                bcSize = readLE<uint64_t>(payload);
            } else if (memcmp(entryBytes + tpos, "OFFT", 4) == 0 && tagLen >= 24) {
                bcOffRel = readLE<uint64_t>(payload + 16);
            }

            tpos += 6 + tagLen;
        }

        uint64_t absBitcodeOffset = bitcodeOff + bcOffRel;
        if (absBitcodeOffset + bcSize <= size && bcSize > 0) {
            FunctionEntry entry;
            entry.name = name;
            entry.type = fnType;
            entry.bitcode.assign(data + absBitcodeOffset, data + absBitcodeOffset + bcSize);
            outEntries.push_back(std::move(entry));
        }
    }

    return !outEntries.empty();
}

} // namespace air2gfx12
