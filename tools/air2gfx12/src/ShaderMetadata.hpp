#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace air2gfx12 {

struct KernelStats {
    std::string name;
    uint32_t sgprCount = 0;
    uint32_t vgprCount = 0;
    uint32_t scratchBytes = 0;
    uint32_t kernargBytes = 0;
    uint32_t wavefrontSize = 32;
    uint32_t ldsBytes = 0;
    
    // Computes GFX12 COMPUTE_PGM_RSRC1 register value
    uint32_t computePgmRsrc1() const;
    // Computes GFX12 COMPUTE_PGM_RSRC2 register value
    uint32_t computePgmRsrc2() const;
};

class ShaderMetadata {
public:
    // Parse metadata from ELF object buffer or file
    static bool parseFromELF(const uint8_t* elfData, size_t size, std::vector<KernelStats>& outStats);
    static bool parseFromELFFile(const std::string& elfPath, std::vector<KernelStats>& outStats);
    
    // Parse metadata from assembly text (.s)
    static bool parseFromAssembly(const std::string& asmText, std::vector<KernelStats>& outStats);
    static bool parseFromAssemblyFile(const std::string& asmPath, std::vector<KernelStats>& outStats);
};

// Auto-locates the runtime.amdil.bc file
class RuntimeLocator {
public:
    static std::string locate(const std::string& userSpecifiedPath = "");
};

} // namespace air2gfx12
