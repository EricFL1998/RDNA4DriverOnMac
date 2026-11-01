#include "AIRLinker.hpp"
#include "AIRGraphicsLowering.hpp"
#include "AIRBuiltins.hpp"
#include <llvm/IR/Constants.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Error.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

namespace air2gfx12 {

namespace {

bool hasTag(llvm::MDNode* node, const char* tag) {
    if (!node) return false;
    for (const llvm::MDOperand& operand : node->operands()) {
        auto* string = llvm::dyn_cast_or_null<llvm::MDString>(operand);
        if (string && string->getString() == tag) return true;
    }
    return false;
}

bool uintOperand(llvm::MDNode* node, unsigned index, uint32_t& value) {
    if (!node || index >= node->getNumOperands()) return false;
    auto* constant = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(node->getOperand(index));
    if (!constant) return false;
    value = static_cast<uint32_t>(constant->getZExtValue());
    return true;
}

std::string stringOperand(llvm::MDNode* node, unsigned index) {
    if (!node || index >= node->getNumOperands()) return {};
    auto* string = llvm::dyn_cast_or_null<llvm::MDString>(node->getOperand(index));
    return string ? string->getString().str() : std::string();
}

// The lowered graphics signature carries one 64-bit inreg argument per used
// resource, in AIR declaration order. The vertex stage keeps the first two
// resources in the reserved i32 pairs s12:s13 and s14:s15 (inside the GS
// user-data bank the driver can actually deliver) and appends the rest after
// the 17 system slots (s19+). The fragment stage starts at s5.
void extractBindings(llvm::Module& module, const std::string& entryName,
                     ShaderStage stage, std::vector<ShaderBinding>& out) {
    out.clear();
    const char* metadataName = stage == ShaderStage::Vertex ? "air.vertex" :
                               stage == ShaderStage::Fragment ? "air.fragment" : nullptr;
    if (!metadataName || entryName.empty()) return;
    llvm::NamedMDNode* named = module.getNamedMetadata(metadataName);
    if (!named) return;

    llvm::MDNode* description = nullptr;
    for (llvm::MDNode* candidate : named->operands()) {
        if (!candidate || candidate->getNumOperands() < 3) continue;
        auto* value = llvm::dyn_cast_or_null<llvm::ValueAsMetadata>(candidate->getOperand(0));
        if (value && value->getValue()->getName().str() == entryName) {
            description = candidate;
            break;
        }
    }
    if (!description) return;

    auto* inputs = llvm::dyn_cast_or_null<llvm::MDNode>(description->getOperand(2));
    if (!inputs) return;

    uint32_t ordinal = 0;
    for (const llvm::MDOperand& operand : inputs->operands()) {
        auto* input = llvm::dyn_cast_or_null<llvm::MDNode>(operand);
        if (!input || hasTag(input, "air.arg_unused")) continue;

        ShaderBinding binding;
        if (hasTag(input, "air.texture")) binding.kind = 0;
        else if (hasTag(input, "air.sampler")) binding.kind = 1;
        else if (hasTag(input, "air.buffer")) binding.kind = 2;
        else continue;

        for (unsigned index = 0; index + 1 < input->getNumOperands(); ++index) {
            auto* tag = llvm::dyn_cast_or_null<llvm::MDString>(input->getOperand(index));
            if (!tag) continue;
            const llvm::StringRef name = tag->getString();
            if (name == "air.location_index") {
                uint32_t value = 0;
                if (uintOperand(input, index + 1, value)) binding.locationIndex = value;
            } else if (name == "air.address_space") {
                uint32_t value = 0;
                if (uintOperand(input, index + 1, value)) binding.addressSpace = value;
            } else if (name == "air.arg_name") {
                binding.name = stringOperand(input, index + 1);
            }
        }

        if (binding.addressSpace == 0) binding.addressSpace = binding.kind == 0 ? 1u : 2u;
        if (stage == ShaderStage::Vertex) {
            // The vertex stage keeps the first two resources in the reserved
            // i32 pairs s12:s13 and s14:s15 and appends the rest after the 17
            // system slots. The fragment stage starts its resources at s5.
            binding.userSgpr = ordinal < 2u ? 12u + 2u * ordinal
                                            : 19u + 2u * (ordinal - 2u);
        } else {
            binding.userSgpr = 5u + 2u * ordinal;
        }
        ++ordinal;
        out.push_back(std::move(binding));
    }
}

} // namespace

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

    extractBindings(*userModule, entryName, stage, mBindings);

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

    if (!lowerRuntimeBuiltins(*loweredModule, outError)) {
        return nullptr;
    }

    inlineHelperFunctions(*loweredModule, entryName);
    foldFunctionConstants(*loweredModule);
    propagateStoredConstants(*loweredModule);
    pruneStaticInit(*loweredModule);

    return loweredModule;
}

} // namespace air2gfx12
