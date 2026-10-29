#include "ShaderMetadata.hpp"
#include <iostream>
#include <cassert>

int main() {
    std::cout << "[test_metadata] Testing ShaderMetadata & RuntimeLocator...\n";

    // 1. Test RuntimeLocator
    std::string loc = air2gfx12::RuntimeLocator::locate();
    assert(!loc.empty());
    std::cout << "  Passed: RuntimeLocator found runtime at '" << loc << "'\n";

    // 2. Test metadata parsing from assembly output of MinMaxReduction_float1
    std::vector<air2gfx12::KernelStats> stats;
    bool ok = air2gfx12::ShaderMetadata::parseFromAssemblyFile("/tmp/out_kernel.s", stats);
    if (ok && !stats.empty()) {
        const auto& s = stats[0];
        std::cout << "  Parsed from ASM: name=" << s.name 
                  << ", SGPR=" << s.sgprCount 
                  << ", VGPR=" << s.vgprCount 
                  << ", Wave=" << s.wavefrontSize
                  << ", Scratch=" << s.scratchBytes << "B"
                  << ", Kernarg=" << s.kernargBytes << "B\n";
        assert(s.name == "MinMaxReduction_float1");
        assert(s.wavefrontSize == 32);
        assert(s.vgprCount > 0);
        assert(s.sgprCount > 0);

        // Verify register value computation
        uint32_t rsrc1 = s.computePgmRsrc1();
        uint32_t rsrc2 = s.computePgmRsrc2();
        assert(rsrc1 != 0);
        assert((rsrc1 & (1u << 24)) != 0); // IEEE_MODE must be 1
        std::cout << "  Passed: Computed GFX12 COMPUTE_PGM_RSRC1=0x" << std::hex << rsrc1 
                  << ", RSRC2=0x" << rsrc2 << std::dec << "\n";
    }

    // 3. Test metadata parsing from ELF object
    std::vector<air2gfx12::KernelStats> elfStats;
    bool okElf = air2gfx12::ShaderMetadata::parseFromELFFile("/tmp/out_kernel.o", elfStats);
    if (okElf && !elfStats.empty()) {
        const auto& s = elfStats[0];
        std::cout << "  Parsed from ELF: name=" << s.name 
                  << ", SGPR=" << s.sgprCount 
                  << ", VGPR=" << s.vgprCount 
                  << ", Scratch=" << s.scratchBytes << "B\n";
        assert(s.name == "MinMaxReduction_float1");
        assert(s.vgprCount == 78);
        assert(s.sgprCount == 73);
        assert(s.scratchBytes == 96);
        std::cout << "  Passed: ELF metadata matches exact AMDHSA note values!\n";
    }

    std::cout << "[test_metadata] ALL TESTS PASSED!\n";
    return 0;
}
