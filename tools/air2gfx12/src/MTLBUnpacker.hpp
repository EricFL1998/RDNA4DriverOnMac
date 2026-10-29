#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace air2gfx12 {

enum class FunctionType : uint8_t {
    Vertex = 0,
    Fragment = 1,
    Compute = 2,
    Unknown = 255
};

struct FunctionEntry {
    std::string name;
    FunctionType type;
    std::vector<uint8_t> bitcode;
};

class MTLBUnpacker {
public:
    static bool isMTLB(const uint8_t* data, size_t size);
    static bool isRawAIR(const uint8_t* data, size_t size);
    static bool unpack(const uint8_t* data, size_t size, std::vector<FunctionEntry>& outEntries);
};

} // namespace air2gfx12
