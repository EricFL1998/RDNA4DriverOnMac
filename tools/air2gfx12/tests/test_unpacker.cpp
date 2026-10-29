#include "MTLBUnpacker.hpp"
#include <iostream>
#include <fstream>
#include <cassert>

int main() {
    std::cout << "[test_unpacker] Testing MTLBUnpacker...\n";

    // 1. Test non-MTLB data
    uint8_t garbage[] = { 0x01, 0x02, 0x03, 0x04 };
    assert(!air2gfx12::MTLBUnpacker::isMTLB(garbage, sizeof(garbage)));
    std::vector<air2gfx12::FunctionEntry> entries;
    assert(!air2gfx12::MTLBUnpacker::unpack(garbage, sizeof(garbage), entries));
    std::cout << "  Passed: Non-MTLB rejection\n";

    // 2. Test unpacking real TextureProcessorMetal.metallib
    std::string metallibPath = "test_assets/TextureProcessorMetal.metallib";
    std::ifstream file(metallibPath, std::ios::binary);
    if (!file.is_open()) {
        metallibPath = "../test_assets/TextureProcessorMetal.metallib";
        file.open(metallibPath, std::ios::binary);
    }
    if (!file.is_open()) {
        metallibPath = "/tmp/TextureProcessorMetal.metallib";
        file.open(metallibPath, std::ios::binary);
    }
    if (!file.is_open()) {
        std::cerr << "  WARNING: TextureProcessorMetal.metallib not found, skipping real file test\n";
        return 0;
    }

    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    assert(air2gfx12::MTLBUnpacker::isMTLB(buffer.data(), buffer.size()));

    bool ok = air2gfx12::MTLBUnpacker::unpack(buffer.data(), buffer.size(), entries);
    assert(ok);
    assert(entries.size() == 45);

    // Verify first entry
    assert(entries[0].name == "MinMaxReduction_float1");
    assert(entries[0].type == air2gfx12::FunctionType::Compute);
    assert(entries[0].bitcode.size() == 6048);
    // Check bitcode magic (0xdec0170b)
    assert(entries[0].bitcode[0] == 0xde && entries[0].bitcode[1] == 0xc0);

    // Verify last entry
    assert(entries[44].name == "RenderToTextureFragment_stencil");
    assert(entries[44].type == air2gfx12::FunctionType::Fragment);
    assert(entries[44].bitcode.size() == 3584);

    std::cout << "  Passed: Successfully unpacked real metallib with 45 functions!\n";
    std::cout << "[test_unpacker] ALL TESTS PASSED!\n";
    return 0;
}
