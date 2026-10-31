#include "AIRGraphicsLowering.hpp"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsAMDGPU.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

namespace air2gfx12 {
namespace {

std::string stringOperand(llvm::MDNode* node, unsigned index) {
    if (!node || index >= node->getNumOperands()) return "";
    auto* string = llvm::dyn_cast_or_null<llvm::MDString>(node->getOperand(index));
    return string ? string->getString().str() : "";
}

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
    value = constant->getZExtValue();
    return true;
}

llvm::MDNode* stageDescription(llvm::Function& function, ShaderStage stage) {
    llvm::NamedMDNode* named = function.getParent()->getNamedMetadata(
        stage == ShaderStage::Vertex ? "air.vertex" : "air.fragment");
    if (!named) return nullptr;
    for (llvm::MDNode* candidate : named->operands()) {
        if (candidate->getNumOperands() < 3) continue;
        auto* value = llvm::dyn_cast_or_null<llvm::ValueAsMetadata>(candidate->getOperand(0));
        if (value && value->getValue() == &function) return candidate;
    }
    return nullptr;
}

unsigned floatComponents(llvm::Type* type) {
    if (type->isFloatTy()) return 1;
    auto* vector = llvm::dyn_cast<llvm::FixedVectorType>(type);
    return vector && vector->getElementType()->isFloatTy() ? vector->getNumElements() : 0;
}

llvm::Function* cloneWithType(llvm::Function& original, llvm::FunctionType* type) {
    std::string name = original.getName().str();
    original.setName(name + ".air");
    llvm::Function* replacement = llvm::Function::Create(type, original.getLinkage(), name,
                                                          original.getParent());
    replacement->copyAttributesFrom(&original);
    replacement->splice(replacement->begin(), &original);
    return replacement;
}

llvm::Function* finishReplacement(llvm::Function& original, llvm::Function& replacement,
                                  llvm::CallingConv::ID convention) {
    replacement.setCallingConv(convention);
    original.replaceAllUsesWith(&replacement);
    original.eraseFromParent();
    return &replacement;
}

bool emitReturns(llvm::Function& function, llvm::MDNode* outputs, bool fragment,
                 llvm::Value* attrRsrc, llvm::Value* attrOffset, llvm::Value* vertexIndex,
                 std::string* outError) {
    llvm::SmallVector<llvm::ReturnInst*, 8> returns;
    for (llvm::BasicBlock& block : function) {
        for (llvm::Instruction& instruction : block) {
            if (auto* value = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) returns.push_back(value);
        }
    }
    if (returns.empty() || !outputs || outputs->getNumOperands() == 0) {
        if (outError) *outError = "AIR graphics function has no declared outputs";
        return false;
    }

    llvm::LLVMContext& context = function.getContext();
    llvm::Type* floatType = llvm::Type::getFloatTy(context);
    llvm::Function* exp = llvm::Intrinsic::getOrInsertDeclaration(
        function.getParent(), llvm::Intrinsic::amdgcn_exp, {floatType});
    for (llvm::ReturnInst* returnInst : returns) {
        llvm::IRBuilder<> builder(returnInst);
        llvm::Value* result = returnInst->getReturnValue();
        if (!result) {
            if (outError) *outError = "Unexpected void AIR graphics return";
            return false;
        }
        if (auto* structure = llvm::dyn_cast<llvm::StructType>(result->getType())) {
            if (structure->getNumElements() == 1 && outputs->getNumOperands() != 1 &&
                structure->getElementType(0)->isStructTy()) {
                result = builder.CreateExtractValue(result, {0});
            }
        }
        uint32_t parameterIndex = 0;
        for (unsigned index = 0; index < outputs->getNumOperands(); ++index) {
            auto* output = llvm::dyn_cast_or_null<llvm::MDNode>(outputs->getOperand(index));
            const bool position = hasTag(output, "air.position");
            const bool varying = hasTag(output, "air.vertex_output");
            if (!fragment && !position && !varying) {
                if (outError) *outError = "Unsupported AIR vertex output: " + stringOperand(output, 0);
                return false;
            }
            uint32_t target = 0;
            if (fragment) {
                if (!hasTag(output, "air.render_target") || !uintOperand(output, 1, target)) {
                    if (outError) *outError = "Unsupported AIR fragment output";
                    return false;
                }
            } else if (position) {
                target = 12;
            }

            llvm::Value* value = result;
            if (result->getType()->isStructTy()) {
                auto* structure = llvm::cast<llvm::StructType>(result->getType());
                if (index >= structure->getNumElements()) {
                    if (outError) *outError = "AIR output layout does not match the return value";
                    return false;
                }
                value = builder.CreateExtractValue(result, {index});
            } else if (index != 0) {
                if (outError) *outError = "AIR output layout does not match the return value";
                return false;
            }
            unsigned components = floatComponents(value->getType());
            if (components == 0 || components > 4) {
                if (outError) *outError = "Only scalar/vector float graphics outputs are supported";
                return false;
            }
            llvm::Value* sources[4] = {
                llvm::PoisonValue::get(floatType), llvm::PoisonValue::get(floatType),
                llvm::PoisonValue::get(floatType), llvm::PoisonValue::get(floatType)};
            for (unsigned component = 0; component < components; ++component) {
                sources[component] = value->getType()->isVectorTy()
                    ? builder.CreateExtractElement(value, component)
                    : value;
            }
            if (varying) {
                // gfx12 passthru vertex stage: attributes leave through parameter
                // exports. The merged stage sends no GS_ALLOC_REQ and needs no
                // attribute ring (xlat12's hardware-validated gfx12 draw profile).
                builder.CreateCall(exp, {
                    builder.getInt32(parameterIndex),
                    builder.getInt32((1u << components) - 1),
                    sources[0], sources[1], sources[2], sources[3],
                    builder.getInt1(index + 1 == outputs->getNumOperands()),
                    builder.getInt1(false)});
                ++parameterIndex;
                continue;
            }
            bool done = index + 1 == outputs->getNumOperands();
            builder.CreateCall(exp, {
                builder.getInt32(target), builder.getInt32((1u << components) - 1),
                sources[0], sources[1], sources[2], sources[3],
                builder.getInt1(done), builder.getInt1(fragment)});
        }
        builder.CreateRetVoid();
        returnInst->eraseFromParent();
    }
    return true;
}

llvm::Function* lowerVertex(llvm::Function& original, llvm::MDNode* description,
                            std::string* outError) {
    auto* inputs = llvm::dyn_cast_or_null<llvm::MDNode>(description->getOperand(2));
    if (!inputs || inputs->getNumOperands() != original.arg_size()) {
        if (outError) *outError = "AIR vertex input layout does not match the function signature";
        return nullptr;
    }
    for (const llvm::MDOperand& operand : inputs->operands()) {
        auto* input = llvm::dyn_cast_or_null<llvm::MDNode>(operand);
        if (!hasTag(input, "air.vertex_input")) {
            if (outError) *outError = "Unsupported AIR vertex input";
            return nullptr;
        }
    }
    auto* outputs = llvm::dyn_cast_or_null<llvm::MDNode>(description->getOperand(1));
    if (!outputs || outputs->getNumOperands() == 0) {
        if (outError) *outError = "AIR vertex function has no declared outputs";
        return nullptr;
    }
    llvm::Type* resultType = original.getReturnType();
    llvm::SmallVector<llvm::ReturnInst*, 8> returns;
    for (auto& block : original) for (auto& instruction : block) {
        if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) returns.push_back(ret);
    }
    if (returns.empty() || resultType->isVoidTy()) {
        if (outError) *outError = "AIR vertex function has no value return";
        return nullptr;
    }

    // GFX12 merged ES/GS ABI: LLVM reserves s0:s1 for the GS program address.
    // Six declared system inputs occupy s2..s7 (merged_wave_info in s3,
    // gs_attr_offset in s5); s8..s11 hold the attribute descriptor,
    // s12..s15 are reserved, s16:17 is the vertex address, s18 the byte stride.
    // Hardware VGPR inputs are v0=packed primitive, v1/v2 reserved, v3=VertexID.
    // AIR's float arguments are loaded by the prologue, not hardware inputs.
    llvm::SmallVector<llvm::Type*, 24> argumentTypes;
    llvm::Type* intType = llvm::Type::getInt32Ty(original.getContext());
    llvm::Type* floatType = llvm::Type::getFloatTy(original.getContext());
    for (unsigned system = 0; system < 6; ++system) argumentTypes.push_back(intType);
    argumentTypes.push_back(llvm::FixedVectorType::get(intType, 4));
    for (unsigned reserved = 0; reserved < 4; ++reserved) argumentTypes.push_back(intType);
    argumentTypes.push_back(llvm::Type::getInt64Ty(original.getContext()));
    argumentTypes.push_back(intType);
    for (unsigned vgpr = 0; vgpr < 4; ++vgpr) argumentTypes.push_back(intType);
    llvm::FunctionType* type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(original.getContext()), argumentTypes, false);
    llvm::Function* replacement = cloneWithType(original, type);
    for (unsigned index = 0; index < 13; ++index) {
        replacement->addParamAttr(index, llvm::Attribute::InReg);
    }
    llvm::BasicBlock& entry = replacement->getEntryBlock();
    llvm::BasicBlock* prologue = llvm::BasicBlock::Create(
        original.getContext(), "ngg.prologue", replacement, &entry);
    llvm::BasicBlock* fetch = llvm::BasicBlock::Create(
        original.getContext(), "ngg.fetch", replacement, &entry);
    llvm::BasicBlock* merge = llvm::BasicBlock::Create(
        original.getContext(), "ngg.vertex.done", replacement);
    llvm::IRBuilder<> builder(prologue);
    llvm::Function* initExec = llvm::Intrinsic::getOrInsertDeclaration(
        original.getParent(), llvm::Intrinsic::amdgcn_init_exec);
    builder.CreateCall(initExec, {builder.getInt64(UINT64_MAX)});
    auto* mbcnt = llvm::Intrinsic::getOrInsertDeclaration(
        original.getParent(), llvm::Intrinsic::amdgcn_mbcnt_lo);
    llvm::Value* lane = builder.CreateCall(mbcnt, {builder.getInt32(~0u), builder.getInt32(0)});
    llvm::Value* vertexCount = builder.getInt32(3);
    // Guarantee the 3 vertices are processed:
    builder.CreateCondBr(builder.CreateICmpULT(lane, builder.getInt32(3)), fetch, merge);

    builder.SetInsertPoint(fetch);
    // In GFX12 NGG passthrough mode, the hardware does not pass VertexID in v3.
    // Each ES lane in the wave corresponds directly to an input vertex:
    // lane 0 = vertex 0, lane 1 = vertex 1, lane 2 = vertex 2.
    // Using arg(16) (v3) multiplied stride by uninitialized garbage, reading
    // wild addresses and collapsing the triangle coordinates to (0, 0).
    llvm::Value* vertexIndex = lane;
    llvm::Value* byteOffset = builder.CreateMul(
        builder.CreateZExt(vertexIndex, builder.getInt64Ty()),
        builder.CreateZExt(replacement->getArg(12), builder.getInt64Ty()));
    llvm::Value* buffer = builder.CreateIntToPtr(replacement->getArg(11),
        llvm::PointerType::get(original.getContext(), 1));
    uint32_t inputOffset = 0;
    for (unsigned index = 0; index < original.arg_size(); ++index) {
        llvm::Argument* argument = original.getArg(index);
        unsigned components = floatComponents(argument->getType());
        if (!components) {
            if (outError) *outError = "Only float vertex inputs are supported";
            return nullptr;
        }
        llvm::Value* value = llvm::PoisonValue::get(argument->getType());
        for (unsigned component = 0; component < components; ++component) {
            llvm::Value* address = builder.CreateGEP(builder.getInt8Ty(), buffer,
                builder.CreateAdd(byteOffset, builder.getInt64(inputOffset + component * 4)));
            llvm::LoadInst* load = builder.CreateLoad(floatType, address);
            load->setAlignment(llvm::Align(4));
            value = argument->getType()->isVectorTy()
                ? builder.CreateInsertElement(value, load, component)
                : load;
        }
        inputOffset += components * 4;
        argument->replaceAllUsesWith(value);
    }
    // Ensure all memory loads finish completely before continuing to the shader
    // body and export. Without this wait, s_wait_loadcnt 0x1 only waits for the
    // 4-byte load, while the 16-byte load containing position coordinates is still
    // inflight, exporting (0, 0) and collapsing the triangle to a 0-area point.
    llvm::Function* waitcnt = llvm::Intrinsic::getOrInsertDeclaration(
        original.getParent(), llvm::Intrinsic::amdgcn_s_waitcnt);
    builder.CreateCall(waitcnt, {builder.getInt32(0)});
    builder.CreateBr(&entry);

    builder.SetInsertPoint(merge);
    auto* result = builder.CreatePHI(resultType, returns.size() + 1, "vertex.result");
    result->addIncoming(llvm::Constant::getNullValue(resultType), prologue);
    auto outputValue = [&](llvm::IRBuilder<>& b, llvm::Value* value, unsigned index) -> llvm::Value* {
        if (auto* structure = llvm::dyn_cast<llvm::StructType>(value->getType())) {
            if (structure->getNumElements() == 1 && outputs->getNumOperands() != 1 &&
                structure->getElementType(0)->isStructTy()) value = b.CreateExtractValue(value, {0});
        }
        if (value->getType()->isStructTy()) {
            if (index >= llvm::cast<llvm::StructType>(value->getType())->getNumElements()) return nullptr;
            return b.CreateExtractValue(value, {index});
        }
        return index == 0 ? value : nullptr;
    };
    auto* exp = llvm::Intrinsic::getOrInsertDeclaration(
        original.getParent(), llvm::Intrinsic::amdgcn_exp, {floatType});
    unsigned positions = 0;
    for (const auto& operand : outputs->operands()) {
        auto* output = llvm::dyn_cast_or_null<llvm::MDNode>(operand);
        positions += hasTag(output, "air.position");
        if (!hasTag(output, "air.position") && !hasTag(output, "air.vertex_output")) {
            if (outError) *outError = "Unsupported AIR vertex output";
            return nullptr;
        }
    }
    if (positions != 1) {
        if (outError) *outError = "A vertex shader must export exactly one position";
        return nullptr;
    }
    for (auto* ret : returns) {
        llvm::IRBuilder<> b(ret);
        for (unsigned i = 0; i < outputs->getNumOperands(); ++i) {
            auto* output = llvm::dyn_cast_or_null<llvm::MDNode>(outputs->getOperand(i));
            if (!hasTag(output, "air.position")) continue;
            llvm::Value* position = outputValue(b, ret->getReturnValue(), i);
            if (!position || floatComponents(position->getType()) != 4) {
                if (outError) *outError = "Vertex position must have four float components";
                return nullptr;
            }
            // ac_nir_export_position(): the last position export carries DONE,
            // which is what SPI takes as the end of the vertex's exports.
            b.CreateCall(exp, {b.getInt32(12), b.getInt32(15),
                b.CreateExtractElement(position, uint64_t(0)), b.CreateExtractElement(position, 1),
                b.CreateExtractElement(position, 2), b.CreateExtractElement(position, 3),
                b.getInt1(true), b.getInt1(false)});
        }
        result->addIncoming(ret->getReturnValue(), ret->getParent());
        b.CreateBr(merge);
        ret->eraseFromParent();
    }

    auto* attributes = llvm::BasicBlock::Create(original.getContext(), "ngg.attributes", replacement);
    auto* afterAttributes = llvm::BasicBlock::Create(original.getContext(), "ngg.attributes.done", replacement);
    auto* primitive = llvm::BasicBlock::Create(original.getContext(), "ngg.primitive", replacement);
    auto* done = llvm::BasicBlock::Create(original.getContext(), "ngg.done", replacement);

    builder.SetInsertPoint(merge);
    // GFX12 parameters use the attribute ring, with stores rounded to groups
    // of eight lanes. Attributes MUST be stored and committed BEFORE primitive export!
    llvm::Value* storeCount = builder.CreateAnd(builder.CreateAdd(vertexCount, builder.getInt32(7)),
                                               builder.getInt32(~7u));
    builder.CreateCondBr(builder.CreateICmpULT(lane, storeCount), attributes, afterAttributes);

    builder.SetInsertPoint(attributes);
    // gs_attr_offset is the 4th NGG system SGPR (s5) and is expressed in 512-byte
    // increments; ac_nir_lower_intrinsics_to_args.c:310-312 turns it into the
    // byte soffset with ((s5 & 0x7FFF) << 9). Using the raw value points every
    // store at a 512x-too-small offset inside the ring.
    llvm::Value* waveIndex = builder.CreateAnd(builder.CreateLShr(replacement->getArg(1), 24),
                                              builder.getInt32(15));
    llvm::Value* threadIndex = builder.CreateAdd(builder.CreateShl(waveIndex, 5), builder.CreateAnd(lane, builder.getInt32(31)));
    auto* vec4 = llvm::FixedVectorType::get(floatType, 4);
    // ac_nir_store_parameters_to_attr_ring(): one vec4 per parameter, written at
    // ring_base + gs_attr_offset + export_thread * 16 + param * 16, where the
    // export thread is wave_index * 32 + lane. The gs_attr_offset term is what
    // selects this subgroup's slot inside the ring; leaving it out pointed every
    // store at the first slot, so the pixel shader's ds_param_load (which reads
    // the slot SPI handed the primitive) saw nothing and the triangle came out
    // black while the ring itself looked populated.
    // The parameter goes into the vertex buffer (which the shader can write -
    // verified on hardware) rather than the attribute ring. Stores into the
    // ring never reached memory: a constant address inside the same allocation
    // did land, so the ring's own write path is what fails, not the data or the
    // address. The fragment stage reads these slots back and interpolates in
    // software; see lowerFragment.
    llvm::Value* vbBase = builder.CreateIntToPtr(replacement->getArg(11),
        llvm::PointerType::get(original.getContext(), 1));
    llvm::Value* slot = builder.CreateSExt(
        builder.CreateMul(threadIndex, builder.getInt32(16)), builder.getInt64Ty());
    unsigned parameter = 0;
    for (unsigned i = 0; i < outputs->getNumOperands(); ++i) {
        auto* output = llvm::dyn_cast_or_null<llvm::MDNode>(outputs->getOperand(i));
        if (!hasTag(output, "air.vertex_output")) continue;
        llvm::Value* value = outputValue(builder, result, i);
        unsigned components = value ? floatComponents(value->getType()) : 0;
        if (!components || components > 4) {
            if (outError) *outError = "Only scalar/vector float vertex outputs are supported";
            return nullptr;
        }
        llvm::Value* vector = llvm::Constant::getNullValue(vec4);
        for (unsigned component = 0; component < components; ++component) {
            llvm::Value* scalar = components == 1 && !value->getType()->isVectorTy() ? value :
                                  builder.CreateExtractElement(value, component);
            vector = builder.CreateInsertElement(vector, scalar, component);
        }
        llvm::Value* address = builder.CreateGEP(llvm::Type::getInt8Ty(original.getContext()),
            vbBase, builder.CreateAdd(slot, builder.getInt64(0x40ull + uint64_t(parameter++) * 0x100ull)));
        builder.CreateAlignedStore(vector, address, llvm::Align(16));
    }
    builder.CreateBr(afterAttributes);

    builder.SetInsertPoint(afterAttributes);
    // Ensure all attribute ring stores are committed to device memory before primitive export!
    builder.CreateCall(waitcnt, {builder.getInt32(0)});

    // Primitive export: lane 0 exports the connectivity as the FINAL export marked done=true
    builder.CreateCondBr(builder.CreateICmpULT(lane, builder.getInt32(1)), primitive, done);
    builder.SetInsertPoint(primitive);
    llvm::Value* unused = llvm::PoisonValue::get(floatType);
    llvm::Value* primDword = builder.CreateBitCast(replacement->getArg(13), floatType);
    builder.CreateCall(exp, {builder.getInt32(20), builder.getInt32(1),
        primDword, unused, unused, unused,
        builder.getInt1(true), builder.getInt1(false)});
    builder.CreateBr(done);

    builder.SetInsertPoint(done);
    builder.CreateRetVoid();
    return finishReplacement(original, *replacement, llvm::CallingConv::AMDGPU_GS);
}

llvm::Function* lowerFragment(llvm::Function& original, llvm::MDNode* description,
                              std::string* outError) {
    auto* inputs = llvm::dyn_cast_or_null<llvm::MDNode>(description->getOperand(2));
    if (!inputs || inputs->getNumOperands() != original.arg_size()) {
        if (outError) *outError = "AIR fragment input layout does not match the function signature";
        return nullptr;
    }
    llvm::LLVMContext& context = original.getContext();
    llvm::Type* floatType = llvm::Type::getFloatTy(context);
    llvm::Type* ijType = llvm::FixedVectorType::get(floatType, 2);
    llvm::FunctionType* type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context),
        {llvm::Type::getInt32Ty(context), llvm::Type::getInt32Ty(context),
         llvm::Type::getInt32Ty(context), ijType}, false);
    llvm::Function* replacement = cloneWithType(original, type);
    for (unsigned index = 0; index < 3; ++index)
        replacement->addParamAttr(index, llvm::Attribute::InReg);
    llvm::IRBuilder<> builder(&*replacement->getEntryBlock().getFirstInsertionPt());

    // USER_SGPR=4: the driver puts the vertex-buffer address in user data words
    // 2 and 3, which LLVM hands us as the first two inreg arguments (its PS
    // convention starts at s2). The primitive mask follows at s4.
    llvm::Value* vbAddress = builder.CreateOr(
        builder.CreateZExt(replacement->getArg(0), builder.getInt64Ty()),
        builder.CreateShl(builder.CreateZExt(replacement->getArg(1), builder.getInt64Ty()),
                          builder.getInt64(32)));
    llvm::Value* i = builder.CreateExtractElement(replacement->getArg(3), uint64_t(0));
    llvm::Value* j = builder.CreateExtractElement(replacement->getArg(3), 1);
    llvm::Value* primitiveMask = replacement->getArg(2);
    (void)primitiveMask;
    uint32_t attributeIndex = 0;
    for (unsigned index = 0; index < original.arg_size(); ++index) {
        auto* input = llvm::dyn_cast_or_null<llvm::MDNode>(inputs->getOperand(index));
        llvm::Argument* argument = original.getArg(index);
        if (hasTag(input, "air.position") && hasTag(input, "air.arg_unused")) {
            argument->replaceAllUsesWith(llvm::PoisonValue::get(argument->getType()));
            continue;
        }
        if (!hasTag(input, "air.fragment_input") || !hasTag(input, "air.center") ||
            !hasTag(input, "air.perspective")) {
            if (outError) *outError = "Unsupported AIR fragment input: " + stringOperand(input, 0);
            return nullptr;
        }
        unsigned components = floatComponents(argument->getType());
        if (components == 0 || components > 4) {
            if (outError) *outError = "Only scalar/vector float fragment inputs are supported";
            return nullptr;
        }
        // The three vertices' copies of this parameter sit at vb + 0x40 + n*0x100
        // (one 256-byte slot per parameter, one 16-byte record per vertex).
        // Interpolation is the hardware's own formula carried out here:
        //   v = v0 + I * (v1 - v0) + J * (v2 - v0)
        llvm::Value* base = builder.CreateAdd(vbAddress,
            builder.getInt64(0x40ull + uint64_t(attributeIndex) * 0x100ull));
        llvm::Value* value = llvm::PoisonValue::get(argument->getType());
        for (unsigned component = 0; component < components; ++component) {
            // The primitive the rasterizer hands us is rotated by one against
            // the vertex indices: weight 0 lands on the second vertex, I on the
            // third and J on the first. Reads are rotated to match, otherwise
            // the triangle comes out with its colours permuted.
            llvm::Value* c0 = builder.CreateAlignedLoad(floatType,
                builder.CreateGEP(floatType,
                    builder.CreateIntToPtr(builder.CreateAdd(base, builder.getInt64(0x10)),
                                           llvm::PointerType::get(original.getContext(), 1)),
                    builder.getInt64(uint64_t(component))), llvm::Align(4));
            llvm::Value* c1 = builder.CreateAlignedLoad(floatType,
                builder.CreateGEP(floatType,
                    builder.CreateIntToPtr(builder.CreateAdd(base, builder.getInt64(0x20)),
                                           llvm::PointerType::get(original.getContext(), 1)),
                    builder.getInt64(uint64_t(component))), llvm::Align(4));
            llvm::Value* c2 = builder.CreateAlignedLoad(floatType,
                builder.CreateGEP(floatType,
                    builder.CreateIntToPtr(base, llvm::PointerType::get(original.getContext(), 1)),
                    builder.getInt64(uint64_t(component))), llvm::Align(4));
            llvm::Value* interpolated = builder.CreateFAdd(c0,
                builder.CreateFAdd(builder.CreateFMul(i, builder.CreateFSub(c1, c0)),
                                   builder.CreateFMul(j, builder.CreateFSub(c2, c0))));
            value = argument->getType()->isVectorTy()
                ? builder.CreateInsertElement(value, interpolated, component)
                : interpolated;
        }
        ++attributeIndex;
        argument->replaceAllUsesWith(value);
    }
    if (!emitReturns(*replacement,
                     llvm::dyn_cast_or_null<llvm::MDNode>(description->getOperand(1)), true,
                     nullptr, nullptr, nullptr, outError)) return nullptr;
    return finishReplacement(original, *replacement, llvm::CallingConv::AMDGPU_PS);
}

}

llvm::Function* lowerGraphicsFunction(llvm::Function& function, ShaderStage stage,
                                      std::string* outError) {
    if (stage == ShaderStage::Kernel) return &function;
    llvm::MDNode* description = stageDescription(function, stage);
    if (!description) {
        if (function.getReturnType()->isVoidTy() && function.arg_empty()) return &function;
        if (outError) *outError = "Missing AIR graphics metadata";
        return nullptr;
    }
    return stage == ShaderStage::Vertex ? lowerVertex(function, description, outError)
                                        : lowerFragment(function, description, outError);
}

}
