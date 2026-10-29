#include "GFX12Emitter.hpp"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <iostream>
#include <fstream>
#include <cassert>

int main() {
    std::cout << "[test_emitter] Testing GFX12Emitter...\n";

    llvm::LLVMContext context;
    auto module = std::make_unique<llvm::Module>("test_module", context);

    // Build a simple amdgpu_kernel function:
    // void test_kernel(ptr addrspace(1) %a) { ret void }
    llvm::Type* voidTy = llvm::Type::getVoidTy(context);
    llvm::PointerType* ptrTy = llvm::PointerType::get(context, 1); // addrspace(1)
    llvm::FunctionType* fnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);

    llvm::Function* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, "test_kernel", *module);
    fn->setCallingConv(llvm::CallingConv::AMDGPU_KERNEL);

    llvm::BasicBlock* bb = llvm::BasicBlock::Create(context, "entry", fn);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRetVoid();

    air2gfx12::GFX12Emitter emitter;

    // 1. Emit assembly
    std::string sPath = "/tmp/test_emit.s";
    std::string err;
    bool okAsm = emitter.emitAssembly(*module, sPath, &err);
    if (!okAsm) {
        std::cerr << "  FAIL: emitAssembly error: " << err << "\n";
        return 1;
    }

    // Verify assembly output
    std::ifstream sFile(sPath);
    assert(sFile.is_open());
    std::string asmContent((std::istreambuf_iterator<char>(sFile)), std::istreambuf_iterator<char>());
    assert(asmContent.find("gfx1201") != std::string::npos);
    assert(asmContent.find("s_endpgm") != std::string::npos);
    std::cout << "  Passed: Assembly emitted with 'gfx1201' and 's_endpgm'\n";

    // 2. Emit object
    std::string oPath = "/tmp/test_emit.o";
    bool okObj = emitter.emitObject(*module, oPath, &err);
    if (!okObj) {
        std::cerr << "  FAIL: emitObject error: " << err << "\n";
        return 1;
    }

    std::ifstream oFile(oPath, std::ios::binary);
    assert(oFile.is_open());
    oFile.seekg(0, std::ios::end);
    size_t oSize = oFile.tellg();
    assert(oSize > 100); // Must be a valid ELF binary
    std::cout << "  Passed: Object binary emitted (" << oSize << " bytes)\n";

    std::cout << "[test_emitter] ALL TESTS PASSED!\n";
    return 0;
}
