#include "GFXFirmware.hpp"

bool GFXFirmware::parseIMU(const uint8_t* bytes, size_t size, GFXFirmware& out) {
    out = {};
    if (!bytes || size < 48) return false;
    uint32_t headerSize = 0, version = 0, payloadOffset = 0;
    uint32_t iramSize = 0, dramSize = 0;
    __builtin_memcpy(&headerSize, bytes + 4, 4);
    __builtin_memcpy(&version, bytes + 8, 4);
    __builtin_memcpy(&payloadOffset, bytes + 24, 4);
    __builtin_memcpy(&iramSize, bytes + 32, 4);
    __builtin_memcpy(&dramSize, bytes + 40, 4);
    if ((version & 0xffffu) != 1 || headerSize < 48 || iramSize == 0 || dramSize == 0)
        return false;
    // imu_iram_ucode_size_bytes at +32, imu_dram_ucode_size_bytes at +40.
    // IMU_D starts immediately after the IRAM payload; its size is the
    // independent DRAM size, not the IRAM size.
    const uint64_t iramEnd = static_cast<uint64_t>(payloadOffset) + iramSize;
    const uint64_t dramEnd = iramEnd + dramSize;
    if (iramEnd > size || dramEnd > size)
        return false;
    out.parts[0] = {bytes + payloadOffset, iramSize, 68};
    out.parts[1] = {bytes + static_cast<size_t>(iramEnd), dramSize, 69};
    out.count = 2;
    return true;
}

namespace {
uint32_t read32(const uint8_t* bytes, uint32_t offset) {
    uint32_t value = 0;
    __builtin_memcpy(&value, bytes + offset, 4);
    return value;
}

bool addPart(GFXFirmware& out, const uint8_t* bytes, size_t size, uint32_t offset, uint32_t length,
             uint32_t type) {
    if (length == 0) return true;
    if (out.count >= GFXFirmware::MaxParts) return false;
    if (static_cast<uint64_t>(offset) + length > size) return false;
    out.parts[out.count++] = {bytes + offset, length, type};
    return true;
}
}

bool GFXFirmware::parseSOSImage(const uint8_t* bytes, size_t size, uint32_t type, GFXFirmware& out) {
    out = {};
    if (!bytes || size < 40) return false;
    const uint32_t version = read32(bytes, 8);
    const uint16_t minor = static_cast<uint16_t>(version >> 16);
    const uint32_t payloadOffset = read32(bytes, 24);
    const uint32_t descriptorOffset = 36 + (minor >= 1 ? 4 : 0);
    const uint32_t count = read32(bytes, 32);
    if ((version & 0xffffu) != 2 || read32(bytes, 0) != size || count == 0 || count > 32) return false;
    for (uint32_t index = 0; index < count; index++) {
        const uint32_t base = descriptorOffset + index * 16;
        if (static_cast<uint64_t>(base) + 16 > size) return false;
        if (read32(bytes, base) != type) continue;
        return addPart(out, bytes, size, payloadOffset + read32(bytes, base + 8), read32(bytes, base + 12), type)
            && out.count == 1;
    }
    return false;
}

bool GFXFirmware::parseSingle(const uint8_t* bytes, size_t size, uint32_t type, GFXFirmware& out) {
    out = {};
    if (!bytes || size < 40) return false;
    const uint32_t version = read32(bytes, 8);
    if (read32(bytes, 0) != size || read32(bytes, 4) < 40 || (version & 0xffffu) == 0) return false;
    if (!addPart(out, bytes, size, read32(bytes, 24), read32(bytes, 20), type)) return false;
    return out.count == 1;
}

bool GFXFirmware::parseCP(const uint8_t* bytes, size_t size, uint32_t ucodeType,
                         const uint32_t* stackTypes, uint32_t stackCount, GFXFirmware& out) {
    out = {};
    if (!bytes || !stackTypes || size < 60 || stackCount == 0 || stackCount > 2) return false;
    const uint32_t version = read32(bytes, 8);
    if (read32(bytes, 0) != size || read32(bytes, 4) < 60 || (version & 0xffffu) != 2) return false;
    if (!addPart(out, bytes, size, read32(bytes, 24), read32(bytes, 36), ucodeType)) return false;
    for (uint32_t index = 0; index < stackCount; index++) {
        if (!addPart(out, bytes, size, read32(bytes, 48), read32(bytes, 44), stackTypes[index]))
            return false;
    }
    return out.count == stackCount + 1;
}

bool GFXFirmware::parseMES(const uint8_t* bytes, size_t size, GFXFirmware& out) {
    out = {};
    if (!bytes || size < 72) return false;
    const uint32_t version = read32(bytes, 8);
    if (read32(bytes, 0) != size || read32(bytes, 4) < 72 || (version & 0xffffu) != 1) return false;
    if (!addPart(out, bytes, size, read32(bytes, 40), read32(bytes, 36), 33)) return false;
    if (!addPart(out, bytes, size, read32(bytes, 52), read32(bytes, 48), 34)) return false;
    if (!addPart(out, bytes, size, read32(bytes, 40), read32(bytes, 36), 81)) return false;
    if (!addPart(out, bytes, size, read32(bytes, 52), read32(bytes, 48), 82)) return false;
    return out.count == 4;
}

bool GFXFirmware::parseRLC(const uint8_t* bytes, size_t size, GFXFirmware& out) {
    out = {};
    if (!bytes || size < 172) return false;
    const uint32_t fileSize = read32(bytes, 0);
    const uint32_t headerSize = read32(bytes, 4);
    const uint32_t version = read32(bytes, 8);
    const uint16_t major = static_cast<uint16_t>(version);
    const uint16_t minor = static_cast<uint16_t>(version >> 16);
    if (fileSize != size || headerSize < 172 || major != 2 || minor < 2) return false;
    if (!addPart(out, bytes, size, read32(bytes, 136), read32(bytes, 132), 20)) return false;
    if (!addPart(out, bytes, size, read32(bytes, 152), read32(bytes, 148), 21)) return false;
    if (!addPart(out, bytes, size, read32(bytes, 160), read32(bytes, 156), 26)) return false;
    if (!addPart(out, bytes, size, read32(bytes, 168), read32(bytes, 164), 48)) return false;
    if (!addPart(out, bytes, size, read32(bytes, 24), read32(bytes, 20), 8)) return false;
    return out.count == 5 && out.parts[out.count - 1].type == 8;
}
