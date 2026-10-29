#include "IPDiscovery.hpp"

static constexpr uint32_t kBinarySignature = 0x28211407;
static constexpr uint32_t kIpdsSignature = 0x53445049;

bool IPDiscovery::readU8(size_t off, uint8_t& v) const {
    if (off >= size) return false;
    v = data[off];
    return true;
}

bool IPDiscovery::readU16(size_t off, uint16_t& v) const {
    if (off + 2 > size) return false;
    v = static_cast<uint16_t>(data[off] | (data[off + 1] << 8));
    return true;
}

bool IPDiscovery::readU32(size_t off, uint32_t& v) const {
    if (off + 4 > size) return false;
    v = static_cast<uint32_t>(data[off]) |
        (static_cast<uint32_t>(data[off + 1]) << 8) |
        (static_cast<uint32_t>(data[off + 2]) << 16) |
        (static_cast<uint32_t>(data[off + 3]) << 24);
    return true;
}

bool IPDiscovery::validateBinary(size_t offset) {
    uint16_t vmajor, checksum, binSize;
    if (!readU16(offset + 4, vmajor) || !readU16(offset + 8, checksum) ||
        !readU16(offset + 10, binSize))
        return false;
    if (binSize < 16 || offset > size || binSize > size - offset)
        return false;

    size_t binaryEnd = offset + binSize;
    uint16_t sum = 0;
    for (size_t i = offset + 10; i < binaryEnd; i++)
        sum = static_cast<uint16_t>(sum + data[i]);
    if (sum != checksum)
        return false;

    size_t tableListDelta = vmajor >= 2 ? 16 : 12;
    if (tableListDelta > binSize || binSize - tableListDelta < 8)
        return false;

    size_t tableList = offset + tableListDelta;
    uint16_t ipdOffset, ipdSize;
    if (!readU16(tableList, ipdOffset) || !readU16(tableList + 4, ipdSize) ||
        ipdOffset == 0 || ipdSize < 80 || ipdOffset > binSize ||
        ipdSize > binSize - ipdOffset)
        return false;

    size_t ipd = offset + ipdOffset;
    uint32_t sig;
    uint16_t numDies;
    if (!readU32(ipd, sig) || sig != kIpdsSignature ||
        !readU16(ipd + 12, numDies) || numDies == 0)
        return false;

    uint16_t dieOffset;
    if (!readU16(ipd + 16, dieOffset) || dieOffset < ipdOffset ||
        dieOffset >= ipdOffset + ipdSize || ipdOffset + ipdSize - dieOffset < 4)
        return false;

    size_t die = offset + dieOffset;
    uint16_t nips;
    if (!readU16(die + 2, nips) || nips == 0 || nips > 256)
        return false;

    binBase = offset;
    dieBase = die;
    ipdEnd = offset + ipdOffset + ipdSize;
    numIps = nips;
    return true;
}

bool IPDiscovery::init(const uint8_t* buffer, size_t bufferSize) {
    data = buffer;
    size = bufferSize;
    binBase = 0;
    dieBase = 0;
    ipdEnd = 0;
    numIps = 0;
    valid = false;
    if (!data || size < 512)
        return false;

    for (size_t off = 0; off <= size - 512; off += 4) {
        uint32_t sig;
        if (readU32(off, sig) && sig == kBinarySignature && validateBinary(off)) {
            valid = true;
            return true;
        }
    }
    return false;
}

bool IPDiscovery::ipAt(uint16_t index, IpEntry& out) const {
    if (!valid || index >= numIps)
        return false;

    size_t p = dieBase + 4;
    for (uint16_t i = 0; ; i++) {
        if (p > ipdEnd || ipdEnd - p < 8)
            return false;

        uint16_t hwId;
        uint8_t inst, nbase, major, minor, rev;
        if (!readU16(p, hwId) || !readU8(p + 2, inst) || !readU8(p + 3, nbase) ||
            !readU8(p + 4, major) || !readU8(p + 5, minor) || !readU8(p + 6, rev))
            return false;

        size_t recordSize = 8 + 4u * nbase;
        if (recordSize > ipdEnd - p)
            return false;

        if (i == index) {
            out.hwId = hwId;
            out.instance = inst;
            out.major = major;
            out.minor = minor;
            out.revision = rev;
            out.numBases = nbase < MaxBases ? nbase : MaxBases;
            for (uint8_t b = 0; b < out.numBases; b++)
                if (!readU32(p + 8 + 4u * b, out.bases[b]))
                    return false;
            return true;
        }
        p += recordSize;
    }
}

bool IPDiscovery::findIp(uint16_t hwId, uint8_t instance, IpEntry& out) const {
    for (uint16_t i = 0; i < numIps; i++) {
        if (!ipAt(i, out))
            return false;
        if (out.hwId == hwId && out.instance == instance)
            return true;
    }
    return false;
}

bool IPDiscovery::regByteOffset(uint16_t hwId, uint8_t instance, uint8_t segment,
                                uint32_t regDwordOffset, uint32_t& byteOffset) const {
    IpEntry ip;
    if (!findIp(hwId, instance, ip) || segment >= ip.numBases)
        return false;
    byteOffset = (ip.bases[segment] + regDwordOffset) * 4;
    return true;
}
