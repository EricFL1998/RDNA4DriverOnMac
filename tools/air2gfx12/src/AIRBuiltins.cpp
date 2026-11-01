#include "AIRBuiltins.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsAMDGPU.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <functional>
#include <vector>

namespace air2gfx12 {
namespace {

using namespace llvm;

Function* intrinsic(Module& module, Intrinsic::ID id, ArrayRef<Type*> types) {
    return Intrinsic::getOrInsertDeclaration(&module, id, types);
}

Constant* splatFloat(Type* type, double value) {
    if (auto* vector = dyn_cast<VectorType>(type)) {
        return ConstantVector::getSplat(vector->getElementCount(),
                                        ConstantFP::get(vector->getElementType(), value));
    }
    return ConstantFP::get(type, value);
}

Value* clamp(Module& module, IRBuilder<>& builder, Value* value, Value* low, Value* high) {
    Type* type = value->getType();
    Value* capped = builder.CreateCall(intrinsic(module, Intrinsic::minnum, {type}), {high, value});
    return builder.CreateCall(intrinsic(module, Intrinsic::maxnum, {type}), {low, capped});
}

Value* dot(IRBuilder<>& builder, Value* left, Value* right) {
    Value* product = builder.CreateFMul(left, right);
    unsigned elements = cast<VectorType>(product->getType())->getElementCount().getKnownMinValue();
    Value* sum = builder.CreateExtractElement(product, uint64_t(0));
    for (unsigned index = 1; index < elements; ++index) {
        sum = builder.CreateFAdd(sum, builder.CreateExtractElement(product, index));
    }
    return sum;
}

bool lowerSampleCall(CallInst* call, StringRef name, std::string* outError) {
    Module& module = *call->getModule();
    LLVMContext& context = module.getContext();
    Type* v8i = FixedVectorType::get(Type::getInt32Ty(context), 8);
    Type* v4i = FixedVectorType::get(Type::getInt32Ty(context), 4);

    // The AIR builtin returns {<4 x T>, i8}; T is f32 for v4f32 and i32 for the
    // u/s v4i32 variants (integer texture formats).
    auto* resultType = dyn_cast<StructType>(call->getType());
    if (!resultType || resultType->getNumElements() != 2) {
        if (outError) *outError = "Unexpected AIR sampling result type: " + name.str();
        return false;
    }
    Type* colorType = resultType->getElementType(0);
    Type* coordType = Type::getFloatTy(context);
    if (name.starts_with("air.sample_texture_2d.")) {
        if (auto* coordVector = dyn_cast<FixedVectorType>(call->getArgOperand(2)->getType())) {
            coordType = coordVector->getElementType();
        }
    } else {
        coordType = call->getArgOperand(2)->getType();
    }

    IRBuilder<> builder(call);
    Value* rsrc = builder.CreateAlignedLoad(v8i, call->getArgOperand(0), Align(4));
    Value* samp = nullptr;
    if (auto* global = dyn_cast<GlobalVariable>(call->getArgOperand(1))) {
        Constant* init = global->hasInitializer() ? global->getInitializer() : nullptr;
        if (init && init->getType() == v4i) {
            samp = init;
        } else if (auto* array = dyn_cast_or_null<ConstantArray>(init)) {
            if (array->getType()->getNumElements() == 2 &&
                array->getType()->getElementType()->isIntegerTy(64)) {
                uint64_t low = cast<ConstantInt>(array->getAggregateElement(0u))->getZExtValue();
                uint64_t high = cast<ConstantInt>(array->getAggregateElement(1u))->getZExtValue();
                samp = ConstantVector::get({
                    ConstantInt::get(Type::getInt32Ty(context), low & 0xffffffffu),
                    ConstantInt::get(Type::getInt32Ty(context), low >> 32),
                    ConstantInt::get(Type::getInt32Ty(context), high & 0xffffffffu),
                    ConstantInt::get(Type::getInt32Ty(context), high >> 32)});
            }
        }
    }
    if (!samp) samp = builder.CreateAlignedLoad(v4i, call->getArgOperand(1), Align(4));
    Value* color = nullptr;

    if (name.starts_with("air.sample_texture_2d.")) {
        Value* coord = call->getArgOperand(2);
        Value* x = builder.CreateExtractElement(coord, uint64_t(0));
        Value* y = builder.CreateExtractElement(coord, 1);
        color = builder.CreateCall(
            intrinsic(module, Intrinsic::amdgcn_image_sample_2d, {colorType, coordType, v8i, v4i}),
            {builder.getInt32(0xF), x, y, rsrc, samp, builder.getTrue(),
             builder.getInt32(0), builder.getInt32(0)});
    } else if (name.starts_with("air.sample_texture_1d_array.")) {
        Value* x = call->getArgOperand(2);
        Value* slice = builder.CreateBitCast(call->getArgOperand(3), coordType);
        color = builder.CreateCall(
            intrinsic(module, Intrinsic::amdgcn_image_sample_1darray, {colorType, coordType, v8i, v4i}),
            {builder.getInt32(0xF), x, slice, rsrc, samp, builder.getTrue(),
             builder.getInt32(0), builder.getInt32(0)});
    } else if (name.starts_with("air.sample_texture_1d.")) {
        Value* x = call->getArgOperand(2);
        color = builder.CreateCall(
            intrinsic(module, Intrinsic::amdgcn_image_sample_1d, {colorType, coordType, v8i, v4i}),
            {builder.getInt32(0xF), x, rsrc, samp, builder.getTrue(),
             builder.getInt32(0), builder.getInt32(0)});
    } else {
        if (outError) *outError = "Unsupported AIR sampling builtin: " + name.str();
        return false;
    }

    Value* result = PoisonValue::get(call->getType());
    result = builder.CreateInsertValue(result, color, {0});
    result = builder.CreateInsertValue(result, builder.getInt8(0), {1});
    call->replaceAllUsesWith(result);
    call->eraseFromParent();
    return true;
}

bool defineMathBuiltin(Module& module, Function& function) {
    const StringRef name = function.getName();
    Type* type = function.arg_size() ? function.getArg(0)->getType() : nullptr;
    BasicBlock* entry = BasicBlock::Create(module.getContext(), "entry", &function);
    IRBuilder<> builder(entry);

    if (name == "__il_abs_f32" || name == "__il_abs_v2f32") {
        builder.CreateRet(builder.CreateCall(intrinsic(module, Intrinsic::fabs, {type}),
                                             {function.getArg(0)}));
    } else if (name == "__il_flr_f32") {
        builder.CreateRet(builder.CreateCall(intrinsic(module, Intrinsic::floor, {type}),
                                             {function.getArg(0)}));
    } else if (name == "__il_frc_f32") {
        Value* floor = builder.CreateCall(intrinsic(module, Intrinsic::floor, {type}),
                                          {function.getArg(0)});
        builder.CreateRet(builder.CreateFSub(function.getArg(0), floor));
    } else if (name == "__il_sqrt_vec_f32" || name == "__il_sqrt_vec_v2f32") {
        builder.CreateRet(builder.CreateCall(intrinsic(module, Intrinsic::sqrt, {type}),
                                             {function.getArg(0)}));
    } else if (name == "__il_rsq_vec_f32") {
        Value* root = builder.CreateCall(intrinsic(module, Intrinsic::sqrt, {type}),
                                         {function.getArg(0)});
        builder.CreateRet(builder.CreateFDiv(splatFloat(type, 1.0), root));
    } else if (name == "__il_mov_sat_f32" || name == "__il_mov_sat_v2f32") {
        builder.CreateRet(clamp(module, builder, function.getArg(0),
                                splatFloat(type, 0.0), splatFloat(type, 1.0)));
    } else if (name == "__il_clamp_f32") {
        builder.CreateRet(clamp(module, builder, function.getArg(0),
                                function.getArg(1), function.getArg(2)));
    } else if (name == "__il_mad_ieee_f32" || name == "__il_mad_ieee_v2f32") {
        builder.CreateRet(builder.CreateCall(intrinsic(module, Intrinsic::fma, {type}),
                                             {function.getArg(0), function.getArg(1),
                                              function.getArg(2)}));
    } else if (name == "__il_min_ieee_f32" || name == "__il_min_ieee_v2f32") {
        builder.CreateRet(builder.CreateCall(intrinsic(module, Intrinsic::minnum, {type}),
                                             {function.getArg(0), function.getArg(1)}));
    } else if (name == "__il_max_ieee_f32") {
        builder.CreateRet(builder.CreateCall(intrinsic(module, Intrinsic::maxnum, {type}),
                                             {function.getArg(0), function.getArg(1)}));
    } else if (name == "__il_pow_f32" || name == "__il_pow_v2f32") {
        builder.CreateRet(builder.CreateCall(intrinsic(module, Intrinsic::pow, {type}),
                                             {function.getArg(0), function.getArg(1)}));
    } else if (name == "__il_sgn_f32" || name == "__il_sgn_v2f32") {
        Value* value = function.getArg(0);
        Value* positive = builder.CreateFCmpOGT(value, splatFloat(type, 0.0));
        Value* negative = builder.CreateFCmpOLT(value, splatFloat(type, 0.0));
        Value* sign = builder.CreateSelect(positive, splatFloat(type, 1.0),
                                           builder.CreateSelect(negative, splatFloat(type, -1.0), value));
        builder.CreateRet(sign);
    } else if (name == "__il_dp2_ieee_v2f32" || name == "__il_dp4_ieee_v4f32") {
        builder.CreateRet(dot(builder, function.getArg(0), function.getArg(1)));
    } else if (name == "__il_dsx_v2f32" || name == "__il_dsy_v2f32") {
        // LLVM 23's AMDGPU backend no longer exposes a dsx/dsy intrinsic. The
        // hardware derivative is the difference between the two pixels of the
        // quad: dx = v[lane^1] - v[lane] on the left pixel and the negation on
        // the right one (the sign is folded back in), dy uses lane^2. readlane
        // gives the other pixel's value; it is slower than DSX/DSY but keeps
        // the shader self-contained (no raw instruction encoding).
        Value* value = function.getArg(0);
        auto* vectorType = cast<FixedVectorType>(value->getType());
        Value* lane = builder.CreateCall(intrinsic(module, Intrinsic::amdgcn_mbcnt_lo, {}),
                                         {builder.getInt32(~0u), builder.getInt32(0)});
        Value* mask = builder.getInt32(name == "__il_dsx_v2f32" ? 1 : 2);
        Value* neighbor = builder.CreateXor(lane, mask);
        Value* odd = builder.CreateICmpNE(builder.CreateAnd(lane, mask), builder.getInt32(0));
        Value* result = PoisonValue::get(value->getType());
        for (unsigned component = 0; component < vectorType->getNumElements(); ++component) {
            Value* element = builder.CreateExtractElement(value, component);
            Value* bits = builder.CreateBitCast(element, builder.getInt32Ty());
            Value* otherBits = builder.CreateCall(
                intrinsic(module, Intrinsic::amdgcn_readlane, {bits->getType()}),
                {bits, neighbor});
            Value* other = builder.CreateBitCast(otherBits, builder.getFloatTy());
            Value* difference = builder.CreateFSub(other, element);
            Value* signedDifference = builder.CreateSelect(
                odd, builder.CreateFNeg(difference), difference);
            result = builder.CreateInsertElement(result, signedDifference, component);
        }
        builder.CreateRet(result);
    } else if (name == "__il_discard_logicalz") {
        Value* discard = builder.CreateICmpEQ(function.getArg(0), builder.getInt32(0));
        builder.CreateCall(intrinsic(module, Intrinsic::amdgcn_kill, {}), {discard});
        builder.CreateRetVoid();
    } else if (name.starts_with("air.all.") || name.starts_with("air.any.")) {
        Value* value = function.getArg(0);
        auto* vectorType = cast<FixedVectorType>(value->getType());
        const bool all = name.starts_with("air.all.");
        Value* result = all ? builder.getTrue() : builder.getFalse();
        for (unsigned component = 0; component < vectorType->getNumElements(); ++component) {
            Value* element = builder.CreateExtractElement(value, component);
            result = all ? builder.CreateAnd(result, element) : builder.CreateOr(result, element);
        }
        builder.CreateRet(result);
    } else if (name == "air.convert.f.f32.s.i32") {
        builder.CreateRet(builder.CreateSIToFP(function.getArg(0), function.getReturnType()));
    } else if (name == "air.convert.s.i32.f.f32") {
        builder.CreateRet(builder.CreateFPToSI(function.getArg(0), function.getReturnType()));
    } else if (name == "air.convert.f.v3f32.u.v3i1") {
        builder.CreateRet(builder.CreateUIToFP(function.getArg(0), function.getReturnType()));
    } else if (name.starts_with("air.normalize_function_constant_predicate.")) {
        Type* returnType = function.getReturnType();
        builder.CreateRet(builder.CreateAnd(function.getArg(0),
                                           ConstantInt::get(returnType, 1)));
    } else {
        function.deleteBody();
        return false;
    }
    return true;
}

} // namespace

bool lowerRuntimeBuiltins(Module& module, std::string* outError) {
    std::vector<Function*> sampleBuiltins;
    for (Function& function : module) {
        if (function.getName().starts_with("air.sample_texture_") && function.isDeclaration()) {
            sampleBuiltins.push_back(&function);
        }
    }
    for (Function* function : sampleBuiltins) {
        std::vector<CallInst*> calls;
        for (User* user : function->users()) {
            if (auto* call = dyn_cast<CallInst>(user)) calls.push_back(call);
        }
        for (CallInst* call : calls) {
            if (!lowerSampleCall(call, function->getName(), outError)) return false;
        }
        if (function->use_empty()) function->eraseFromParent();
    }

    std::vector<Function*> mathBuiltins;
    for (Function& function : module) {
        if (function.isDeclaration() &&
            (function.getName().starts_with("__il_") ||
             function.getName().starts_with("air.convert.") ||
             function.getName().starts_with("air.all.") ||
             function.getName().starts_with("air.any.") ||
             function.getName().starts_with("air.normalize_function_constant_predicate."))) {
            mathBuiltins.push_back(&function);
        }
    }
    for (Function* function : mathBuiltins) {
        defineMathBuiltin(module, *function);
    }
    return true;
}

bool inlineHelperFunctions(Module& module, StringRef entryName) {
    Function* entry = module.getFunction(entryName);
    if (!entry || entry->isDeclaration()) return false;

    // Inline every direct call to a defined helper (the AIR helper bodies and
    // the __il_* definitions) into its caller. The entry ends up as the only
    // .text function, so the loader's single-blob copy has no inter-function
    // relocation and the entry sits at offset 0.
    for (unsigned round = 0; round < 8; ++round) {
        SmallVector<CallBase*, 64> calls;
        for (Function& function : module) {
            if (function.isDeclaration()) continue;
            for (BasicBlock& block : function) {
                for (Instruction& instruction : block) {
                    auto* call = dyn_cast<CallBase>(&instruction);
                    if (!call) continue;
                    Function* callee = call->getCalledFunction();
                    if (callee && callee != entry && !callee->isDeclaration() &&
                        !callee->isIntrinsic()) {
                        calls.push_back(call);
                    }
                }
            }
        }
        if (calls.empty()) break;

        bool inlined = false;
        for (CallBase* call : calls) {
            Function* callee = call->getCalledFunction();
            if (!callee || callee->isDeclaration() || callee == entry) continue;
            InlineFunctionInfo info;
            InlineResult result = InlineFunction(*call, info);
            if (result.isSuccess()) inlined = true;
        }
        if (!inlined) break;
    }

    for (Function& function : make_early_inc_range(module)) {
        if (&function == entry || function.isDeclaration()) continue;
        if (function.use_empty()) function.eraseFromParent();
    }
    return true;
}

bool foldFunctionConstants(Module& module) {
    NamedMDNode* constants = module.getNamedMetadata("air.function_constants");
    if (!constants) return true;

    DenseMap<GlobalVariable*, Constant*> values;
    for (MDNode* node : constants->operands()) {
        if (!node || node->getNumOperands() < 5) continue;
        auto* valueMeta = dyn_cast<ValueAsMetadata>(node->getOperand(0));
        auto* global = valueMeta ? dyn_cast<GlobalVariable>(valueMeta->getValue()) : nullptr;
        auto* constantMeta = dyn_cast<ConstantAsMetadata>(node->getOperand(4));
        if (!global || !constantMeta) continue;
        Constant* value = constantMeta->getValue();
        Type* globalType = global->getValueType();
        if (globalType != value->getType()) {
            auto* integer = dyn_cast<ConstantInt>(value);
            if (!integer || !globalType->isIntegerTy()) continue;
            value = ConstantInt::get(globalType, integer->getZExtValue());
        }
        values[global] = value;
    }

    for (Function& function : module) {
        for (BasicBlock& block : function) {
            for (Instruction& instruction : make_early_inc_range(block)) {
                auto* load = dyn_cast<LoadInst>(&instruction);
                if (!load) continue;
                auto* global = dyn_cast<GlobalVariable>(load->getPointerOperand());
                if (!global) continue;
                auto it = values.find(global);
                if (it == values.end() || it->second->getType() != load->getType()) continue;
                load->replaceAllUsesWith(it->second);
                load->eraseFromParent();
            }
        }
    }
    return true;
}

bool propagateStoredConstants(Module& module) {
    DenseMap<GlobalVariable*, Constant*> values;
    DenseSet<GlobalVariable*> conflicting;

    for (Function& function : module) {
        for (BasicBlock& block : function) {
            for (Instruction& instruction : block) {
                auto* store = dyn_cast<StoreInst>(&instruction);
                if (!store) continue;
                auto* global = dyn_cast<GlobalVariable>(store->getPointerOperand());
                auto* constant = dyn_cast<Constant>(store->getValueOperand());
                if (!global || !constant || constant->getType() != global->getValueType()) continue;
                if (values.count(global)) {
                    conflicting.insert(global);
                    continue;
                }
                values[global] = constant;
            }
        }
    }
    for (GlobalVariable* global : conflicting) values.erase(global);

    for (Function& function : module) {
        for (BasicBlock& block : function) {
            for (Instruction& instruction : make_early_inc_range(block)) {
                auto* load = dyn_cast<LoadInst>(&instruction);
                if (!load) continue;
                auto* global = dyn_cast<GlobalVariable>(load->getPointerOperand());
                if (!global) continue;
                auto it = values.find(global);
                if (it == values.end() || it->second->getType() != load->getType()) continue;
                load->replaceAllUsesWith(it->second);
                load->eraseFromParent();
            }
        }
    }
    return true;
}

bool pruneStaticInit(Module& module) {
    // The shader is uploaded as one .text blob. Anything that would make the
    // entry read a data section has just been folded (sampler constants and
    // function constants), so the static initialiser and its .ctors entry can
    // go; keeping them would leave air.static_init and .bss relocations behind.
    if (GlobalVariable* ctors = module.getGlobalVariable("llvm.global_ctors")) {
        ctors->eraseFromParent();
    }
    for (Function& function : make_early_inc_range(module)) {
        if (function.getName().starts_with("_GLOBAL__sub_I") && function.use_empty()) {
            function.eraseFromParent();
        }
    }
    if (NamedMDNode* samplerStates = module.getNamedMetadata("air.sampler_states")) {
        module.eraseNamedMetadata(samplerStates);
    }
    if (NamedMDNode* constants = module.getNamedMetadata("air.function_constants")) {
        module.eraseNamedMetadata(constants);
    }
    for (GlobalVariable& global : make_early_inc_range(module.globals())) {
        if (!global.isDeclaration() && global.hasLocalLinkage() && global.use_empty()) {
            global.eraseFromParent();
        }
    }
    return true;
}

} // namespace air2gfx12
