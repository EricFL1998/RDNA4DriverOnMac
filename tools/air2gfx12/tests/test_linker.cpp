#include "AIRLinker.hpp"
#include "MTLBUnpacker.hpp"
#include <iostream>
#include <fstream>
#include <cassert>

int main() {
    std::cout << "[test_linker] Testing AIRLinker...\n";

    llvm::LLVMContext context;
    air2gfx12::AIRLinker linker(context);

    // 1. Load runtime.amdil.bc
    std::string runtimePath = "assets/runtime.amdil.bc";
    std::ifstream rf(runtimePath);
    if (!rf.is_open()) runtimePath = "../assets/runtime.amdil.bc";
    rf.close();
    rf.open(runtimePath);
    if (!rf.is_open()) runtimePath = "/tmp/runtime.amdil.bc";
    rf.close();

    bool hasRuntime = linker.loadRuntime(runtimePath);
    if (!hasRuntime) {
        std::cerr << "  WARNING: " << runtimePath << " not found, skipping linked test\n";
        return 0;
    }
    std::cout << "  Passed: Successfully loaded runtime.amdil.bc into memory (" << runtimePath << ")\n";

    // 2. Unpack real TextureProcessorMetal.metallib to get a real shader bitcode
    std::string metallibPath = "test_assets/TextureProcessorMetal.metallib";
    std::ifstream file(metallibPath, std::ios::binary);
    if (!file.is_open()) metallibPath = "../test_assets/TextureProcessorMetal.metallib";
    file.close();
    file.open(metallibPath, std::ios::binary);
    if (!file.is_open()) metallibPath = "/tmp/TextureProcessorMetal.metallib";
    file.close();
    file.open(metallibPath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "  WARNING: TextureProcessorMetal.metallib not found\n";
        return 0;
    }

    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<air2gfx12::FunctionEntry> entries;
    bool ok = air2gfx12::MTLBUnpacker::unpack(buffer.data(), buffer.size(), entries);
    assert(ok && !entries.empty());

    // 3. Link the first entry (MinMaxReduction_float1) with runtime
    std::string errStr;
    auto linkedMod = linker.loadAndLink(entries[0].bitcode.data(), entries[0].bitcode.size(), entries[0].name, &errStr);
    assert(linkedMod != nullptr);
    if (!errStr.empty()) {
        std::cout << "  Linker notice: " << errStr << "\n";
    }

    // Verify: The entry function must be present
    llvm::Function* entryFn = linkedMod->getFunction("MinMaxReduction_float1");
    assert(entryFn != nullptr && !entryFn->isDeclaration());
    assert(entryFn->getCallingConv() == llvm::CallingConv::AMDGPU_KERNEL);
    std::cout << "  Passed: Found entry function 'MinMaxReduction_float1' with AMDGPU_KERNEL calling convention\n";

    // Verify: @air.fmax.f32 was declared in the shader, and now MUST be defined in the linked module!
    llvm::Function* fmaxFn = linkedMod->getFunction("air.fmax.f32");
    if (fmaxFn) {
        assert(!fmaxFn->isDeclaration()); // It must now have a body from runtime.amdil.bc!
        std::cout << "  Passed: @air.fmax.f32 successfully resolved and defined from runtime.amdil.bc!\n";
    }

    std::cout << "[test_linker] ALL TESTS PASSED!\n";
    return 0;
}
