#pragma once

#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>
#include <string>
#include <memory>

namespace air2gfx12 {

class GFX12Emitter {
public:
    GFX12Emitter();
    ~GFX12Emitter();

    // Configure the module for GFX12 (triple, datalayout)
    void prepareModule(llvm::Module& module);

    // Emit assembly text (.s) to file
    bool emitAssembly(llvm::Module& module, const std::string& outPath, std::string* outError = nullptr);

    // Emit ELF object binary (.o) to file
    bool emitObject(llvm::Module& module, const std::string& outPath, std::string* outError = nullptr);

private:
    std::unique_ptr<llvm::TargetMachine> mTargetMachine;
    std::string mTriple;
    std::string mCPU;
};

} // namespace air2gfx12
