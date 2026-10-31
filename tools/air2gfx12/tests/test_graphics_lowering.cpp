#include "AIRGraphicsLowering.hpp"
#include "GFX12Emitter.hpp"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicsAMDGPU.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdio>
#include <fstream>
#include <iterator>

static unsigned failures;
static void check(bool value, const char* message) {
    std::printf("%s: %s\n", value ? "PASS" : "FAIL", message);
    failures += !value;
}

static bool constantIs(llvm::Value* value, uint64_t expected) {
    auto* constant = llvm::dyn_cast<llvm::ConstantInt>(value);
    return constant && constant->getZExtValue() == expected;
}

static const char* vertexIR = R"(
%Out = type { <4 x float>, <3 x float> }
define %Out @vertexShader(<2 x float> %position, <3 x float> %color) {
  %xy = shufflevector <2 x float> %position, <2 x float> zeroinitializer,
                       <4 x i32> <i32 0, i32 1, i32 2, i32 3>
  %xyzw = insertelement <4 x float> %xy, float 1.0, i32 3
  %a = insertvalue %Out poison, <4 x float> %xyzw, 0
  %b = insertvalue %Out %a, <3 x float> %color, 1
  ret %Out %b
}
!air.vertex = !{!0}
!0 = !{ptr @vertexShader, !1, !2}
!1 = !{!3, !4}
!2 = !{!5, !5}
!3 = !{!"air.position"}
!4 = !{!"air.vertex_output"}
!5 = !{!"air.vertex_input"}
)";

static const char* fragmentIR = R"(
define <4 x float> @fragmentShader(<3 x float> %color) {
  %rgb = shufflevector <3 x float> %color, <3 x float> zeroinitializer,
                       <4 x i32> <i32 0, i32 1, i32 2, i32 3>
  %rgba = insertelement <4 x float> %rgb, float 1.0, i32 3
  ret <4 x float> %rgba
}
!air.fragment = !{!0}
!0 = !{ptr @fragmentShader, !1, !2}
!1 = !{!3}
!2 = !{!4}
!3 = !{!"air.render_target", i32 0}
!4 = !{!"air.fragment_input", !"air.center", !"air.perspective"}
)";

static void testVertex() {
    llvm::LLVMContext context;
    llvm::SMDiagnostic error;
    auto module = llvm::parseAssemblyString(vertexIR, error, context);
    check(bool(module), "parse vertex fixture");
    if (!module) return;
    std::string reason;
    auto* entry = air2gfx12::lowerGraphicsFunction(*module->getFunction("vertexShader"),
        air2gfx12::ShaderStage::Vertex, &reason);
    check(entry != nullptr, "lower vertex fixture");
    if (!entry) { std::puts(reason.c_str()); return; }
    check(!llvm::verifyModule(*module, &llvm::errs()), "vertex IR is valid");
    unsigned primitiveExports = 0, positionExports = 0, wrongExports = 0;
    unsigned plainVec4Stores = 0, byteAddressedLoads = 0, workitemReads = 0;
    bool fullExec = false;
    for (auto& block : *entry) for (auto& instruction : block) {
        if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
            // A plain global store lands at a linear address. The PS reads the
            // ring through ds_param_load, which walks the hardware's swizzled
            // layout, so vertex data stored this way is never found: the
            // triangle rendered black with correct values sitting in the ring.
            if (auto* vec = llvm::dyn_cast<llvm::FixedVectorType>(
                    store->getValueOperand()->getType()))
                if (vec->getNumElements() == 4 && vec->getElementType()->isFloatTy())
                    ++plainVec4Stores;
        }
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
            auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
            byteAddressedLoads += gep && gep->getSourceElementType()->isIntegerTy(8);
        }
        auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (!call || !call->getCalledFunction()) continue;
        switch (call->getCalledFunction()->getIntrinsicID()) {
        case llvm::Intrinsic::amdgcn_workitem_id_x: ++workitemReads; break;
        case llvm::Intrinsic::amdgcn_init_exec:
            fullExec |= constantIs(call->getArgOperand(0), UINT64_MAX);
            break;
        case llvm::Intrinsic::amdgcn_exp:
            if (constantIs(call->getArgOperand(0), 20)) {
                ++primitiveExports;
                check(constantIs(call->getArgOperand(6), 1), "primitive export marks DONE");
            } else if (constantIs(call->getArgOperand(0), 12)) {
                ++positionExports;
                // ac_nir_export_position() sets DONE on the last position
                // export; SPI treats it as the end of the vertex's exports.
                check(constantIs(call->getArgOperand(6), 1), "position export marks DONE");
            } else ++wrongExports;
            break;
        default: break;
        }
    }
    check(fullExec, "NGG entry enables the full wave before lane predicates");
    check(workitemReads == 0, "vertex IDs do not use the compute workitem intrinsic");
    check(byteAddressedLoads == 5, "packed 20-byte vertices use byte-addressed loads");
    check(positionExports == 1 && primitiveExports == 1, "NGG exports both positions and primitive connectivity");
    check(wrongExports == 0 && plainVec4Stores == 1, "vertex colors go to the attribute ring, never MRT exports");
    // LLVM reserves s0:s1 for the GS program address. Six declared system
    // inputs therefore occupy s2..s7, before the user data starting at s8.
    check(entry->arg_size() == 17, "vertex entry accounts for the implicit GS program pair");
    if (entry->arg_size() == 17) {
        check(entry->getArg(11)->getType()->isIntegerTy(64) &&
              entry->hasParamAttribute(11, llvm::Attribute::InReg), "vertex pointer is scalar s16:17");
        check(!entry->hasParamAttribute(16, llvm::Attribute::InReg), "VertexID is the fourth VGPR input");
    }
    air2gfx12::GFX12Emitter emitter(air2gfx12::ShaderStage::Vertex);
    check(emitter.emitAssembly(*module, "/tmp/air2gfx12-test-vertex.s", &reason),
          "LLVM accepts the NGG vertex shader");
    std::ifstream assembly("/tmp/air2gfx12-test-vertex.s");
    std::string code((std::istreambuf_iterator<char>(assembly)), {});
    check(code.find("s18, s[16:17]") != std::string::npos,
          "compiled address calculation consumes the PM4 pointer and stride registers");
    check(code.find("global_store_b128") != std::string::npos,
          "compiled ring write is a 16-byte flat store");
    // ac_nir_lower_intrinsics_to_args.c: the ring slot is selected by
    // gs_attr_offset (s5) shifted into bytes; without it every store lands in
    // the first slot and the fragment stage reads zeros.
    // The varying is published into the vertex buffer, which the shader can
    // write; the attribute ring's own write path drops the store on this chip.
    check(code.find("offset:64") != std::string::npos, "varying slots are published in the vertex buffer");
}

static void testFragment() {
    llvm::LLVMContext context;
    llvm::SMDiagnostic error;
    auto module = llvm::parseAssemblyString(fragmentIR, error, context);
    check(bool(module), "parse fragment fixture");
    if (!module) return;
    std::string reason;
    auto* entry = air2gfx12::lowerGraphicsFunction(*module->getFunction("fragmentShader"),
        air2gfx12::ShaderStage::Fragment, &reason);
    check(entry != nullptr, "lower fragment fixture");
    if (!entry) return;
    unsigned legacyParamLoads = 0;
    for (auto& block : *entry) for (auto& instruction : block) {
        auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (!call || !call->getCalledFunction() ||
            call->getCalledFunction()->getIntrinsicID() != llvm::Intrinsic::amdgcn_lds_param_load) continue;
        ++legacyParamLoads;
    }
    check(legacyParamLoads == 0,
          "interpolation does not depend on the parameter cache (ds_param_load)");
    check(!llvm::verifyModule(*module, &llvm::errs()), "fragment IR is valid");
}

int main() {
    testVertex();
    testFragment();
    std::printf("RESULT: %u failures\n", failures);
    return failures ? 1 : 0;
}
