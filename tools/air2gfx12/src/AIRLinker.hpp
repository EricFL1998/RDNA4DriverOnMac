#pragma once

#include "GFX12Emitter.hpp"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace air2gfx12 {

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

private:
    llvm::LLVMContext& mContext;
    std::unique_ptr<llvm::Module> mRuntimeTemplate;
    std::string mRuntimePath;
};

} // namespace air2gfx12
