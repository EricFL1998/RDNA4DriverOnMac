#ifndef IPDiscovery_hpp
#define IPDiscovery_hpp

#include <stdint.h>
#include <stddef.h>

class IPDiscovery {
public:
    enum HwId : uint16_t {
        HwMp1    = 1,
        HwGc     = 11,
        HwMmhub  = 34,
        HwOsssys = 40,
        HwHdp    = 41,
        HwSdma0  = 42,
        HwNbif   = 108,
        HwUmc    = 150,
        HwMp0    = 255,
        HwDmu    = 271,
    };

    static constexpr size_t MaxBases = 16;

    struct IpEntry {
        uint16_t hwId;
        uint8_t instance;
        uint8_t major, minor, revision;
        uint8_t numBases;
        uint32_t bases[MaxBases];
    };

    bool init(const uint8_t* buffer, size_t bufferSize);

    bool isValid() const { return valid; }
    size_t binaryOffset() const { return binBase; }
    uint16_t ipCount() const { return numIps; }

    bool ipAt(uint16_t index, IpEntry& out) const;
    bool findIp(uint16_t hwId, uint8_t instance, IpEntry& out) const;
    bool regByteOffset(uint16_t hwId, uint8_t instance, uint8_t segment,
                       uint32_t regDwordOffset, uint32_t& byteOffset) const;

private:
    const uint8_t* data { nullptr };
    size_t size { 0 };
    size_t binBase { 0 };
    size_t dieBase { 0 };
    size_t ipdEnd { 0 };
    uint16_t numIps { 0 };
    bool valid { false };

    bool validateBinary(size_t offset);
    bool readU8(size_t off, uint8_t& v) const;
    bool readU16(size_t off, uint16_t& v) const;
    bool readU32(size_t off, uint32_t& v) const;
};

#endif
