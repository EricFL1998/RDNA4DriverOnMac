#include "GFX12Emitter.hpp"
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>

namespace air2gfx12 {

GFX12Emitter::GFX12Emitter(ShaderStage stage)
    : mTriple(stage == ShaderStage::Kernel ? "amdgcn-amd-amdhsa" : "amdgcn--mesa3d"),
      mCPU("gfx1201") {
    // Initialize LLVM AMDGPU target
    LLVMInitializeAMDGPUTargetInfo();
    LLVMInitializeAMDGPUTarget();
    LLVMInitializeAMDGPUTargetMC();
    LLVMInitializeAMDGPUAsmPrinter();

    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(llvm::Triple(mTriple), err);
    if (!target) {
        return;
    }

    llvm::TargetOptions opt;
    opt.AllowFPOpFusion = llvm::FPOpFusion::Fast;

    std::string features = "+wavefrontsize32";
    mTargetMachine.reset(target->createTargetMachine(
        llvm::Triple(mTriple),
        mCPU,
        features,
        opt,
        llvm::Reloc::PIC_,
        llvm::CodeModel::Small,
        llvm::CodeGenOptLevel::Default
    ));
}

GFX12Emitter::~GFX12Emitter() = default;

void GFX12Emitter::prepareModule(llvm::Module& module) {
    module.setTargetTriple(llvm::Triple(mTriple));
    if (mTargetMachine) {
        module.setDataLayout(mTargetMachine->createDataLayout());
    }
}

bool GFX12Emitter::emitAssembly(llvm::Module& module, const std::string& outPath, std::string* outError) {
    if (!mTargetMachine) {
        if (outError) *outError = "TargetMachine not initialized for " + mTriple;
        return false;
    }

    prepareModule(module);

    std::error_code ec;
    llvm::raw_fd_ostream dest(outPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        if (outError) *outError = "Could not open file: " + ec.message();
        return false;
    }

    llvm::legacy::PassManager pass;
    if (mTargetMachine->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
        if (outError) *outError = "TargetMachine can't emit a file of this type";
        return false;
    }

    pass.run(module);
    dest.flush();
    return true;
}

bool GFX12Emitter::emitObject(llvm::Module& module, const std::string& outPath, std::string* outError) {
    if (!mTargetMachine) {
        if (outError) *outError = "TargetMachine not initialized for " + mTriple;
        return false;
    }

    prepareModule(module);

    std::error_code ec;
    llvm::raw_fd_ostream dest(outPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        if (outError) *outError = "Could not open file: " + ec.message();
        return false;
    }

    llvm::legacy::PassManager pass;
    if (mTargetMachine->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        if (outError) *outError = "TargetMachine can't emit a file of this type";
        return false;
    }

    pass.run(module);
    dest.flush();
    return true;
}

} // namespace air2gfx12
