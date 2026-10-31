#include "AIRLinker.hpp"
#include "AIRGraphicsLowering.hpp"
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

std::unique_ptr<llvm::Module> AIRLinker::loadAndLink(const uint8_t* bitcodeData, size_t size,
                                                     const std::string& entryName,
                                                     std::string* outError) {
    return loadAndLink(bitcodeData, size, entryName, ShaderStage::Kernel, outError);
}

std::unique_ptr<llvm::Module> AIRLinker::loadAndLink(const uint8_t* bitcodeData, size_t size,
                                                     const std::string& entryName,
                                                     ShaderStage stage,
                                                     std::string* outError) {
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

    if (!entryName.empty()) {
        llvm::Function* entry = userModule->getFunction(entryName);
        if (!entry || entry->isDeclaration()) {
            if (outError) *outError = "Entry function not found: " + entryName;
            return nullptr;
        }
        entry = lowerGraphicsFunction(*entry, stage, outError);
        if (!entry) return nullptr;
        entry->setLinkage(llvm::GlobalValue::ExternalLinkage);
        llvm::CallingConv::ID convention = llvm::CallingConv::AMDGPU_KERNEL;
        if (stage == ShaderStage::Vertex) convention = llvm::CallingConv::AMDGPU_GS;
        if (stage == ShaderStage::Fragment) convention = llvm::CallingConv::AMDGPU_PS;
        entry->setCallingConv(convention);
    } else {
        for (auto& fn : userModule->functions()) {
            if (!fn.isDeclaration() && !fn.hasInternalLinkage() && fn.getReturnType()->isVoidTy()) {
                fn.setCallingConv(llvm::CallingConv::AMDGPU_KERNEL);
            }
        }
    }

    std::string irStr;
    llvm::raw_string_ostream os(irStr);
    userModule->print(os, nullptr);
    size_t pos = 0;
    while ((pos = irStr.find("addrspace(2)", pos)) != std::string::npos) {
        irStr.replace(pos, 12, "addrspace(4)");
        pos += 12;
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
