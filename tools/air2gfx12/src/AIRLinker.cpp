#include "AIRLinker.hpp"
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Error.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

namespace air2gfx12 {

AIRLinker::AIRLinker(llvm::LLVMContext& context)
    : mContext(context) {}

bool AIRLinker::loadRuntime(const std::string& runtimeBcPath) {
    mRuntimePath = runtimeBcPath;
    auto bufOrErr = llvm::MemoryBuffer::getFile(runtimeBcPath);
    if (!bufOrErr) {
        return false;
    }

    auto modOrErr = llvm::parseBitcodeFile(bufOrErr.get()->getMemBufferRef(), mContext);
    if (!modOrErr) {
        return false;
    }

    mRuntimeTemplate = std::move(modOrErr.get());
    return true;
}

std::unique_ptr<llvm::Module> AIRLinker::loadAndLink(const uint8_t* bitcodeData, size_t size, const std::string& entryName, std::string* outError) {
    if (!bitcodeData || size == 0) {
        if (outError) *outError = "Empty bitcode buffer";
        return nullptr;
    }

    llvm::StringRef dataRef(reinterpret_cast<const char*>(bitcodeData), size);
    llvm::MemoryBufferRef mbRef(dataRef, "user_shader");

    auto modOrErr = llvm::parseBitcodeFile(mbRef, mContext);
    if (!modOrErr) {
        if (outError) *outError = "Failed to parse user AIR bitcode";
        return nullptr;
    }

    std::unique_ptr<llvm::Module> userModule = std::move(modOrErr.get());

    // If runtime is loaded, link only the needed symbols
    if (mRuntimeTemplate) {
        // Clone runtime template so it can be linked multiple times
        std::unique_ptr<llvm::Module> runtimeClone = llvm::CloneModule(*mRuntimeTemplate);

        llvm::Linker linker(*userModule);
        bool err = linker.linkInModule(std::move(runtimeClone), llvm::Linker::Flags::LinkOnlyNeeded);
        if (err) {
            if (outError) *outError = "Linker failed to link runtime.amdil.bc";
            return nullptr;
        }
    }

    // Lower Metal-specific conventions to AMDGPU:
    // 1. Metal addrspace(2) is Constant memory -> in AMDGPU, Constant memory is addrspace(4)
    // 2. Kernel entry calling convention: set to amdgpu_kernel
    std::string irStr;
    llvm::raw_string_ostream os(irStr);
    userModule->print(os, nullptr);

    // Replace addrspace(2) with addrspace(4)
    size_t pos = 0;
    while ((pos = irStr.find("addrspace(2)", pos)) != std::string::npos) {
        irStr.replace(pos, 12, "addrspace(4)");
        pos += 12;
    }

    // Replace entry function definition calling convention if entryName is specified
    if (!entryName.empty()) {
        std::string pattern = "define void @" + entryName;
        size_t kpos = irStr.find(pattern);
        if (kpos != std::string::npos) {
            irStr.replace(kpos, pattern.length(), "define amdgpu_kernel void @" + entryName);
        }
    } else {
        // If entryName not given, look for any non-internal function with void return
        // and add amdgpu_kernel if not already present
        for (auto& fn : userModule->functions()) {
            if (!fn.isDeclaration() && !fn.hasInternalLinkage() && fn.getReturnType()->isVoidTy()) {
                std::string fnPattern = "define void @" + fn.getName().str();
                size_t fnPos = irStr.find(fnPattern);
                if (fnPos != std::string::npos) {
                    irStr.replace(fnPos, fnPattern.length(), "define amdgpu_kernel void @" + fn.getName().str());
                }
            }
        }
    }

    llvm::SMDiagnostic smErr;
    auto loweredModule = llvm::parseAssemblyString(irStr, smErr, mContext);
    if (!loweredModule) {
        if (outError) *outError = "Failed to parse lowered IR: " + smErr.getMessage().str();
        return userModule; // Fallback
    }

    return loweredModule;
}

} // namespace air2gfx12
