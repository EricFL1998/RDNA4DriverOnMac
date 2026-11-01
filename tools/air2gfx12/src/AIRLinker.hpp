#pragma once

#include "GFX12Emitter.hpp"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace air2gfx12 {

// One Metal resource binding as the generated shader expects it: the descriptor
// address goes into userSgpr (low 32 bits) and userSgpr+1 (high 32 bits).
struct ShaderBinding {
    uint32_t userSgpr = 0;
    uint32_t locationIndex = 0;
    uint32_t kind = 0;          // 0 texture, 1 sampler, 2 buffer
    uint32_t addressSpace = 0;
    std::string name;
};

class AIRLinker {
public:
    explicit AIRLinker(llvm::LLVMContext& context);

    // Load runtime.amdil.bc once to reuse for multiple shaders
    bool loadRuntime(const std::string& runtimeBcPath);

    // Parse user AIR bitcode into a module, lower address spaces, and link required air.* functions
    std::unique_ptr<llvm::Module> loadAndLink(const uint8_t* bitcodeData, size_t size,
                                              const std::string& entryName = "",
                                              std::string* outError = nullptr);
    std::unique_ptr<llvm::Module> loadAndLink(const uint8_t* bitcodeData, size_t size,
                                              const std::string& entryName,
                                              ShaderStage stage,
                                              std::string* outError);

    // Binding table of the last loadAndLink() call (graphics stages only).
    const std::vector<ShaderBinding>& bindings() const { return mBindings; }

private:
    llvm::LLVMContext& mContext;
    std::unique_ptr<llvm::Module> mRuntimeTemplate;
    std::string mRuntimePath;
    std::vector<ShaderBinding> mBindings;
};

} // namespace air2gfx12
