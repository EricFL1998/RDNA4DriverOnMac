#ifndef GFXFirmware_hpp
#define GFXFirmware_hpp

#include <stdint.h>
#include <stddef.h>

struct GFXFirmwarePart {
    const uint8_t* bytes;
    uint32_t size;
    uint32_t type;
};

struct GFXFirmware {
    static constexpr uint32_t MaxParts = 16;
    GFXFirmwarePart parts[MaxParts] {};
    uint32_t count = 0;
    static bool parseIMU(const uint8_t* bytes, size_t size, GFXFirmware& out);
    static bool parseSingle(const uint8_t* bytes, size_t size, uint32_t type, GFXFirmware& out);
    static bool parseSOSImage(const uint8_t* bytes, size_t size, uint32_t type, GFXFirmware& out);
    static bool parseCP(const uint8_t* bytes, size_t size, uint32_t ucodeType,
                        const uint32_t* stackTypes, uint32_t stackCount, GFXFirmware& out);
    static bool parseMES(const uint8_t* bytes, size_t size, GFXFirmware& out);
    static bool parseRLC(const uint8_t* bytes, size_t size, GFXFirmware& out);
};

#endif
