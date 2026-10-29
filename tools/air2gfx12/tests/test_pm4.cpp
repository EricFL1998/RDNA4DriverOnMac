#include "PM4Builder.hpp"
#include <iostream>
#include <cassert>

int main() {
    std::cout << "[test_pm4] Testing PM4Builder for GFX12...\n";

    air2gfx12::PM4Builder pm4;

    // 1. Test basic packet header encoding
    // NOP with 1 dword: header should be (3 << 30) | (0 << 16) | (0x10 << 8) | 0 = 0xC0001000
    pm4.emitNop(0);
    assert(pm4.sizeDwords() == 1);
    assert(pm4.getPackets()[0] == 0xC0001000);
    std::cout << "  Passed: NOP packet header (0x" << std::hex << pm4.getPackets()[0] << std::dec << ")\n";

    pm4.clear();

    // 2. Build full compute dispatch sequence
    uint64_t shaderVA = 0x400017B00; // 256-byte aligned GPU virtual address
    uint32_t rsrc1 = 0x10C0253;      // Computed earlier
    uint32_t rsrc2 = 0x1381;         // Computed earlier

    // (a) Set shader program and resources
    pm4.setComputeProgram(shaderVA, rsrc1, rsrc2);

    // Verify SET_SH_REG for COMPUTE_PGM_LO/HI:
    // Header for SET_SH_REG (0x76), count = 1 + 2 = 3 (so count-1 = 2):
    // (3 << 30) | (2 << 16) | (0x76 << 8) = 0xC0027600
    assert(pm4.getPackets()[0] == 0xC0027600);
    assert(pm4.getPackets()[1] == air2gfx12::GFX12Reg::COMPUTE_PGM_LO);
    assert(pm4.getPackets()[2] == static_cast<uint32_t>(shaderVA >> 8));
    assert(pm4.getPackets()[3] == static_cast<uint32_t>(shaderVA >> 40));
    std::cout << "  Passed: Shader address packet encoded accurately\n";

    // (b) Set workgroup size: 32 threads in X (wave32), 1 in Y, 1 in Z
    pm4.setComputeWorkgroupSize(32, 1, 1);

    // (c) Pass kernel buffer parameters (User Data 0 and 1)
    uint64_t bufferA = 0x500000000;
    uint64_t bufferB = 0x500100000;
    pm4.setComputeUserData(0, bufferA);
    pm4.setComputeUserData(1, bufferB);

    // (d) Dispatch: grid size (64, 1, 1)
    pm4.emitDispatchDirect(64, 1, 1);

    // (e) Cache flush and wait
    pm4.emitCsPartialFlush();

    // Verify total packets
    std::cout << "  Generated " << pm4.sizeDwords() << " PM4 dwords (" << pm4.sizeBytes() << " bytes)\n";
    assert(pm4.sizeDwords() > 20);

    // Verify DISPATCH_DIRECT header:
    // Opcode = 0x15, count = 4, flags = 1 (Compute):
    // (3 << 30) | (3 << 16) | (0x15 << 8) | 1 = 0xC0031501
    bool foundDispatch = false;
    for (size_t i = 0; i < pm4.sizeDwords(); ++i) {
        if (pm4.getPackets()[i] == 0xC0031501) {
            foundDispatch = true;
            assert(pm4.getPackets()[i+1] == 64); // Grid X
            assert(pm4.getPackets()[i+2] == 1);  // Grid Y
            assert(pm4.getPackets()[i+3] == 1);  // Grid Z
            break;
        }
    }
    assert(foundDispatch);
    std::cout << "  Passed: DISPATCH_DIRECT packet found and verified with grid=(64,1,1)\n";

    std::cout << "[test_pm4] ALL TESTS PASSED!\n";
    return 0;
}
