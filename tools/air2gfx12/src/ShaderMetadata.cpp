#include "ShaderMetadata.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cctype>

namespace air2gfx12 {

static uint32_t parseTrailingUInt(const std::string& str) {
    size_t i = 0;
    while (i < str.size() && !std::isdigit(static_cast<unsigned char>(str[i]))) i++;
    if (i >= str.size()) return 0;
    char* end = nullptr;
    return static_cast<uint32_t>(strtoul(str.c_str() + i, &end, 10));
}

uint32_t KernelStats::computePgmRsrc1() const {
    // GFX12 COMPUTE_PGM_RSRC1 layout:
    // [5:0]   VGPRS: Granularity is 4 VGPRs for wave32
    // [11:6]  SGPRS: Granularity is 8 SGPRs
    // [13:12] PRIORITY
    // [15:14] FLOAT_MODE
    // [21]    PRIV (privilege mode)
    // [22]    DX10_CLAMP
    // [23]    DEBUG_MODE
    // [24]    IEEE_MODE
    uint32_t vgprBlocks = (vgprCount > 0) ? (vgprCount - 1) / 4 : 0;
    if (vgprBlocks > 63) vgprBlocks = 63;
    
    uint32_t sgprBlocks = (sgprCount > 0) ? (sgprCount - 1) / 8 : 0;
    if (sgprBlocks > 15) sgprBlocks = 15;

    uint32_t val = (vgprBlocks & 0x3F) | ((sgprBlocks & 0x3F) << 6);
    val |= (1u << 24); // IEEE_MODE = 1
    val |= (0xC0u << 12); // FLOAT_MODE
    return val;
}

uint32_t KernelStats::graphicsPgmRsrc1() const {
    // SPI_SHADER_PGM_RSRC1: VGPRS is in units of 4 VGPRs (SGPRS in units of 8).
    // Encoding the VGPR count in units of 8 under-allocates the wave - 9 VGPRs
    // became 8 - and SPI then refuses to launch it.
    uint32_t vgprBlocks = vgprCount > 0 ? (vgprCount - 1) / 4 : 0;
    uint32_t sgprBlocks = sgprCount > 0 ? (sgprCount - 1) / 8 : 0;
    return (vgprBlocks & 0x3fu) | ((sgprBlocks & 0xfu) << 6) | (0xf0u << 12) | (1u << 25);
}

uint32_t KernelStats::computePgmRsrc2() const {
    // GFX12 COMPUTE_PGM_RSRC2 layout:
    // [0]     SCRATCH_EN: 1 if scratchBytes > 0
    // [1]     USER_SGPR
    // [6:1]   USER_SGPR_COUNT
    // [7]     TRAP_HANDLER
    // [8]     TGID_X_EN
    // [9]     TGID_Y_EN
    // [10]    TGID_Z_EN
    // [12:11] TGID_COMP_CNT
    // [19:15] LDS_SIZE
    uint32_t val = 0;
    if (scratchBytes > 0) {
        val |= (1u << 0); // SCRATCH_EN
    }
    val |= (1u << 7); // TGID_X_EN
    val |= (1u << 8); // TGID_Y_EN
    val |= (1u << 9); // TGID_Z_EN
    val |= (2u << 11); // TIDIG_COMP_CNT = 2 (X, Y, Z)
    return val;
}

bool ShaderMetadata::parseFromAssembly(const std::string& asmText, std::vector<KernelStats>& outStats) {
    outStats.clear();
    std::istringstream stream(asmText);
    std::string line;
    
    KernelStats current;
    bool inKernel = false;

    while (std::getline(stream, line)) {
        if (line.find(".amdhsa_kernel ") != std::string::npos) {
            if (inKernel && !current.name.empty()) {
                outStats.push_back(current);
            }
            current = KernelStats();
            inKernel = true;
            size_t pos = line.find(".amdhsa_kernel ");
            current.name = line.substr(pos + 15);
            current.name.erase(current.name.find_last_not_of(" \t\r\n") + 1);
        } else if (inKernel) {
            if (line.find(".amdhsa_private_segment_fixed_size") != std::string::npos) {
                current.scratchBytes = parseTrailingUInt(line);
            } else if (line.find(".amdhsa_kernarg_size") != std::string::npos) {
                current.kernargBytes = parseTrailingUInt(line);
            } else if (line.find(".amdhsa_group_segment_fixed_size") != std::string::npos) {
                current.ldsBytes = parseTrailingUInt(line);
            } else if (line.find(".amdhsa_wavefront_size32 1") != std::string::npos) {
                current.wavefrontSize = 32;
            } else if (line.find(".num_vgpr") != std::string::npos && line.find("max(") != std::string::npos) {
                size_t p = line.find("max(");
                current.vgprCount = parseTrailingUInt(line.substr(p + 4));
            } else if (line.find(".numbered_sgpr") != std::string::npos && line.find("max(") != std::string::npos) {
                size_t p = line.find("max(");
                current.sgprCount = parseTrailingUInt(line.substr(p + 4));
            } else if (line.find(".sgpr_count:") != std::string::npos) {
                current.sgprCount = parseTrailingUInt(line);
            } else if (line.find(".vgpr_count:") != std::string::npos) {
                current.vgprCount = parseTrailingUInt(line);
            }
        }
    }

    if (inKernel && !current.name.empty()) {
        outStats.push_back(current);
    }

    return !outStats.empty();
}

bool ShaderMetadata::parseGraphicsFromAssembly(const std::string& asmText,
                                                const std::string& entryName,
                                                KernelStats& outStats) {
    outStats = KernelStats();
    outStats.name = entryName;
    const std::string declaration = "\t.type\t" + entryName + ",@function";
    const std::string prefix = "\t.set .L" + entryName + ".";
    std::istringstream stream(asmText);
    std::string line;
    bool inFunction = false;
    bool hasStats = false;
    while (std::getline(stream, line)) {
        if (line == declaration) {
            inFunction = true;
            continue;
        }
        if (!inFunction) continue;
        const std::pair<const char*, uint32_t*> fields[] = {
            {"num_vgpr", &outStats.vgprCount},
            {"numbered_sgpr", &outStats.sgprCount},
            {"private_seg_size", &outStats.scratchBytes},
        };
        for (const auto& field : fields) {
            const std::string marker = prefix + field.first;
            if (line.compare(0, marker.size(), marker) != 0) continue;
            size_t comma = line.find(',', marker.size());
            if (comma != std::string::npos) {
                *field.second = parseTrailingUInt(line.substr(comma + 1));
                hasStats = true;
            }
        }
    }
    return hasStats;
}

bool ShaderMetadata::parseFromAssemblyFile(const std::string& asmPath, std::vector<KernelStats>& outStats) {
    std::ifstream file(asmPath);
    if (!file.is_open()) return false;
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parseFromAssembly(text, outStats);
}

bool ShaderMetadata::parseFromELFFile(const std::string& elfPath, std::vector<KernelStats>& outStats) {
    std::ifstream file(elfPath, std::ios::binary);
    if (!file.is_open()) return false;
    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parseFromELF(buffer.data(), buffer.size(), outStats);
}

bool ShaderMetadata::parseFromELF(const uint8_t* elfData, size_t size, std::vector<KernelStats>& outStats) {
    outStats.clear();
    if (!elfData || size < 64) return false;

    std::string dataStr(reinterpret_cast<const char*>(elfData), size);
    size_t notePos = dataStr.find("amdhsa.kernels:");
    if (notePos == std::string::npos) {
        return false;
    }

    std::string yamlPart = dataStr.substr(notePos, 8192);
    std::istringstream stream(yamlPart);
    std::string line;

    KernelStats current;
    bool inKernel = false;

    while (std::getline(stream, line)) {
        if (line.find(".name:") != std::string::npos) {
            if (inKernel && !current.name.empty()) {
                outStats.push_back(current);
            }
            current = KernelStats();
            inKernel = true;
            size_t p = line.find(".name:");
            current.name = line.substr(p + 6);
            current.name.erase(0, current.name.find_first_not_of(" \t"));
            current.name.erase(current.name.find_last_not_of(" \t\r\n") + 1);
        } else if (inKernel) {
            if (line.find(".sgpr_count:") != std::string::npos) {
                current.sgprCount = parseTrailingUInt(line);
            } else if (line.find(".vgpr_count:") != std::string::npos) {
                current.vgprCount = parseTrailingUInt(line);
            } else if (line.find(".private_segment_fixed_size:") != std::string::npos) {
                current.scratchBytes = parseTrailingUInt(line);
            } else if (line.find(".kernarg_segment_size:") != std::string::npos) {
                current.kernargBytes = parseTrailingUInt(line);
            } else if (line.find(".wavefront_size:") != std::string::npos) {
                current.wavefrontSize = parseTrailingUInt(line);
            }
        }
    }

    if (inKernel && !current.name.empty()) {
        outStats.push_back(current);
    }

    return !outStats.empty();
}

std::string RuntimeLocator::locate(const std::string& userSpecifiedPath) {
    if (!userSpecifiedPath.empty()) {
        std::ifstream f(userSpecifiedPath);
        if (f.is_open()) return userSpecifiedPath;
    }

    const std::vector<std::string> candidates = {
        "assets/runtime.amdil.bc",
        "../assets/runtime.amdil.bc",
        "tools/air2gfx12/assets/runtime.amdil.bc",
        "/tmp/runtime.amdil.bc",
        "/Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/runtime.amdil.bc"
    };

    for (const auto& path : candidates) {
        std::ifstream f(path);
        if (f.is_open()) return path;
    }

    return "";
}

} // namespace air2gfx12
