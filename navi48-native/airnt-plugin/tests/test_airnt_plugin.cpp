#include "../include/airnt_plugin.h"
#include <llvm/AsmParser/Parser.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static std::vector<uint8_t> makeInternalAIRFunction(const char* name) {
    llvm::LLVMContext llvmContext;
    llvm::SMDiagnostic diagnostic;
    std::string ir = std::string("define internal void @") + name + "() { ret void }";
    auto module = llvm::parseAssemblyString(ir, diagnostic, llvmContext);
    assert(module != nullptr);
    llvm::SmallVector<char, 0> bytes;
    llvm::raw_svector_ostream stream(bytes);
    llvm::WriteBitcodeToFile(*module, stream);
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

static std::vector<uint8_t> makeAIRBitcode(const std::string& ir) {
    llvm::LLVMContext llvmContext;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(ir, diagnostic, llvmContext);
    if (!module) diagnostic.print("AIR test", llvm::errs());
    assert(module != nullptr);
    llvm::SmallVector<char, 0> bytes;
    llvm::raw_svector_ostream stream(bytes);
    llvm::WriteBitcodeToFile(*module, stream);
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

static void testStageIO(AIRNTContext context, const char* name, AIRNTFunctionType type,
                        const char* ir, const char* expected) {
    auto bitcode = makeAIRBitcode(ir);
    AIRNTCompileOptions options = {};
    options.entryPointName = name;
    options.functionType = type;
    options.optimizationLevel = 2;
    AIRNTExecutable executable = nullptr;
    char* error = nullptr;
    AIRNTStatus status = AIRNTCompile(context, bitcode.data(), bitcode.size(),
                                      &options, &executable, &error);
    if (status != AIRNT_SUCCESS) {
        fprintf(stderr, "FAIL compiling %s I/O: %s\n", name, error ? error : "unknown error");
    }
    assert(status == AIRNT_SUCCESS);
    char* assembly = nullptr;
    assert(AIRNTGetAssembly(executable, &assembly) == AIRNT_SUCCESS);
    if (!strstr(assembly, expected)) {
        fprintf(stderr, "FAIL: %s lacks %s:\n%s\n", name, expected, assembly);
    }
    assert(strstr(assembly, expected) != nullptr);
    AIRNTFreeString(assembly);
    AIRNTShaderMetadata metadata = {};
    assert(AIRNTGetMetadata(executable, &metadata) == AIRNT_SUCCESS);
    // The merged NGG stage takes s8..s18 as user data (descriptor, reserved,
    // vertex base, stride, count); the fragment stage has no user SGPRs.
    assert(metadata.pgmRsrc2 == (type == AIRNT_FUNCTION_TYPE_VERTEX ? (11u << 1) : 0u));
    AIRNTFreeExecutable(executable);
}

static void testStageLacks(AIRNTContext context, const char* name, AIRNTFunctionType type,
                           const char* ir, const char* unexpected) {
    auto bitcode = makeAIRBitcode(ir);
    AIRNTCompileOptions options = {};
    options.entryPointName = name;
    options.functionType = type;
    options.optimizationLevel = 2;
    AIRNTExecutable executable = nullptr;
    char* error = nullptr;
    assert(AIRNTCompile(context, bitcode.data(), bitcode.size(), &options, &executable, &error) ==
           AIRNT_SUCCESS);
    char* assembly = nullptr;
    assert(AIRNTGetAssembly(executable, &assembly) == AIRNT_SUCCESS);
    if (strstr(assembly, unexpected)) {
        fprintf(stderr, "FAIL: %s must not contain %s:\n%s\n", name, unexpected, assembly);
    }
    assert(strstr(assembly, unexpected) == nullptr);
    AIRNTFreeString(assembly);
    AIRNTFreeExecutable(executable);
}

static void testTriangleStageIO(AIRNTContext context) {
    const char* vertexIR = R"IR(
define <{ <4 x float>, <3 x float> }> @vertexShader(<2 x float> %in.0, <3 x float> %in.1) {
entry:
  %p = shufflevector <2 x float> %in.0, <2 x float> poison, <4 x i32> <i32 0, i32 1, i32 poison, i32 poison>
  %position = shufflevector <4 x float> %p, <4 x float> <float poison, float poison, float 0.0, float 1.0>, <4 x i32> <i32 0, i32 1, i32 6, i32 7>
  %r0 = insertvalue <{ <4 x float>, <3 x float> }> undef, <4 x float> %position, 0
  %r1 = insertvalue <{ <4 x float>, <3 x float> }> %r0, <3 x float> %in.1, 1
  ret <{ <4 x float>, <3 x float> }> %r1
}
!air.vertex = !{!0}
!0 = !{ptr @vertexShader, !1, !4}
!1 = !{!2, !3}
!2 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!3 = !{!"air.vertex_output", !"generated(5colorDv3_f)", !"air.arg_type_name", !"float3", !"air.arg_name", !"color"}
!4 = !{!5, !6}
!5 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float2", !"air.arg_name", !"position"}
!6 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"float3", !"air.arg_name", !"color"}
)IR";
    const char* fragmentIR = R"IR(
define <4 x float> @fragmentShader(<4 x float> %in.0, <3 x float> %in.1) {
entry:
  %color = shufflevector <3 x float> %in.1, <3 x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 poison>
  %result = insertelement <4 x float> %color, float 1.0, i64 3
  ret <4 x float> %result
}
!air.fragment = !{!0}
!0 = !{ptr @fragmentShader, !1, !2}
!1 = !{!3}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!2 = !{!4, !5}
!4 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"position", !"air.arg_unused"}
!5 = !{i32 1, !"air.fragment_input", !"generated(5colorDv3_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float3", !"air.arg_name", !"color"}
)IR";
    // The vertex stage runs as a gfx12 NGG passthrough shader
    // (VGT_SHADER_STAGES_EN.PRIMGEN_PASSTHRU_NO_MSG), which must not send
    // GS_ALLOC_REQ (ac_nir_lower_ngg.c, has_ngg_passthru_no_msg).
    testStageLacks(context, "vertexShader", AIRNT_FUNCTION_TYPE_VERTEX, vertexIR, "GS_ALLOC_REQ");
    testStageIO(context, "vertexShader", AIRNT_FUNCTION_TYPE_VERTEX, vertexIR, "global_load");
    // ac_nir_export_position(): the position export carries DONE.
    testStageIO(context, "vertexShader", AIRNT_FUNCTION_TYPE_VERTEX, vertexIR, "export pos0");
    testStageIO(context, "vertexShader", AIRNT_FUNCTION_TYPE_VERTEX, vertexIR, "done");
    // Parameters go to the attribute ring with a structured store. The ring is
    // a GPU VA in addrspace(1), so the emitted form is global_store_b128 (the
    // same SADDR form the hardware triangle test validated).
    testStageIO(context, "vertexShader", AIRNT_FUNCTION_TYPE_VERTEX, vertexIR, "global_store_b128");
    testStageIO(context, "fragmentShader", AIRNT_FUNCTION_TYPE_FRAGMENT, fragmentIR, "ds_param_load");
    testStageIO(context, "fragmentShader", AIRNT_FUNCTION_TYPE_FRAGMENT, fragmentIR, "v_interp_p10_f32");
    testStageIO(context, "fragmentShader", AIRNT_FUNCTION_TYPE_FRAGMENT, fragmentIR, "export mrt0");
    testStageLacks(context, "fragmentShader", AIRNT_FUNCTION_TYPE_FRAGMENT, fragmentIR, "global_store");
}

static void testBindings(AIRNTContext context) {
    const char* ir = R"IR(
define <4 x float> @bindShader(<4 x float> %pos, ptr addrspace(1) %tex,
                               ptr addrspace(2) %samp, ptr addrspace(2) %buf) {
entry:
  ret <4 x float> zeroinitializer
}
!air.fragment = !{!0}
!0 = !{ptr @bindShader, !1, !2}
!1 = !{!3}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!2 = !{!4, !5, !6, !7}
!4 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective",
       !"air.arg_type_name", !"float4", !"air.arg_name", !"pos", !"air.arg_unused"}
!5 = !{i32 1, !"air.texture", !"air.location_index", i32 2, i32 1, !"air.sample",
       !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}
!6 = !{i32 2, !"air.sampler", !"air.location_index", i32 1, i32 1,
       !"air.arg_type_name", !"sampler", !"air.arg_name", !"samp"}
!7 = !{i32 3, !"air.buffer", !"air.location_index", i32 0, i32 1,
       !"air.address_space", i32 2, !"air.arg_type_name", !"float4",
       !"air.arg_name", !"buf"}
)IR";
    auto bitcode = makeAIRBitcode(ir);
    AIRNTCompileOptions options = {};
    options.entryPointName = "bindShader";
    options.functionType = AIRNT_FUNCTION_TYPE_FRAGMENT;
    AIRNTExecutable executable = nullptr;
    char* error = nullptr;
    assert(AIRNTCompile(context, bitcode.data(), bitcode.size(), &options,
                        &executable, &error) == AIRNT_SUCCESS);

    const AIRNTBinding* bindings = nullptr;
    size_t count = 0;
    assert(AIRNTGetBindings(executable, &bindings, &count) == AIRNT_SUCCESS);
    assert(count == 3);
    // Fragment system slots s2..s4 precede the resources, so the descriptors
    // land at s5, s7 and s9 in AIR declaration order.
    assert(bindings[0].kind == AIRNT_BINDING_TEXTURE);
    assert(bindings[0].locationIndex == 2);
    assert(bindings[0].userSgpr == 5);
    assert(bindings[1].kind == AIRNT_BINDING_SAMPLER);
    assert(bindings[1].locationIndex == 1);
    assert(bindings[1].userSgpr == 7);
    assert(bindings[2].kind == AIRNT_BINDING_BUFFER);
    assert(bindings[2].locationIndex == 0);
    assert(bindings[2].userSgpr == 9);
    assert(bindings[2].addressSpace == 2);
    AIRNTFreeExecutable(executable);
    printf("  ✅ Binding reflection (3 resources, s5/s7/s9)\n");
}

static void testGraphicsStage(AIRNTContext context, const char* name, AIRNTFunctionType type) {
    auto bitcode = makeInternalAIRFunction(name);
    AIRNTCompileOptions options = {};
    options.entryPointName = name;
    options.functionType = type;
    options.optimizationLevel = 2;
    AIRNTExecutable executable = nullptr;
    char* error = nullptr;
    AIRNTStatus status = AIRNTCompile(context, bitcode.data(), bitcode.size(),
                                      &options, &executable, &error);
    if (status != AIRNT_SUCCESS) {
        fprintf(stderr, "FAIL compiling %s: %s\n", name, error ? error : "unknown error");
    }
    assert(status == AIRNT_SUCCESS);
    char* assembly = nullptr;
    assert(AIRNTGetAssembly(executable, &assembly) == AIRNT_SUCCESS);
    assert(assembly != nullptr);
    if (strstr(assembly, "s_endpgm") == nullptr) {
        fprintf(stderr, "FAIL: %s was not emitted as a graphics stage:\n%s\n", name, assembly);
    }
    assert(strstr(assembly, "s_endpgm") != nullptr);
    assert(strstr(assembly, "amdgcn-unknown-mesa3d") != nullptr);
    const uint8_t* machineCode = nullptr;
    size_t machineCodeSize = 0;
    assert(AIRNTGetMachineCode(executable, reinterpret_cast<const void**>(&machineCode),
                               &machineCodeSize) == AIRNT_SUCCESS);
    assert(machineCodeSize > 0);
    assert(!(machineCode[0] == 0x7f && machineCode[1] == 'E' &&
             machineCode[2] == 'L' && machineCode[3] == 'F'));
    AIRNTShaderMetadata metadata = {};
    assert(AIRNTGetMetadata(executable, &metadata) == AIRNT_SUCCESS);
    printf("  %s: SGPR=%u VGPR=%u\n", name, metadata.sgprCount, metadata.vgprCount);
    assert(metadata.sgprCount == 0);
    assert(metadata.vgprCount == 0);
    AIRNTFreeString(assembly);
    AIRNTFreeExecutable(executable);
}

int main() {
    printf("[test_airnt_plugin] Testing AIRNT Plugin API...\n");
    AIRNTContext context;
    AIRNTStatus status = AIRNTInit(&context, AIRNT_API_VERSION);
    assert(status == AIRNT_SUCCESS);
    printf("  ✅ Plugin initialized\n");
    uint32_t version = AIRNTGetVersion();
    assert(version == AIRNT_API_VERSION);
    printf("  ✅ Version check passed (0x%08x)\n", version);
    testGraphicsStage(context, "vertexShader", AIRNT_FUNCTION_TYPE_VERTEX);
    testGraphicsStage(context, "fragmentShader", AIRNT_FUNCTION_TYPE_FRAGMENT);
    testTriangleStageIO(context);
    testBindings(context);
    
    // 4. Test error handling
    AIRNTExecutable exec;
    char* error = NULL;
    AIRNTCompileOptions opts = {0};
    opts.entryPointName = "testKernel";
    opts.functionType = AIRNT_FUNCTION_TYPE_KERNEL;
    
    // Invalid input should fail gracefully
    status = AIRNTCompile(context, NULL, 0, &opts, &exec, &error);
    assert(status != AIRNT_SUCCESS);
    if (error) {
        printf("  ✅ Error handling works: %s\n", error);
        AIRNTFreeString(error);
    }
    
    // 5. Test FlatBuffers stub
    status = AIRNTCompilePipeline(context, NULL, 0, &exec, &error);
    assert(status == AIRNT_ERROR_NOT_IMPLEMENTED);
    if (error) {
        printf("  ✅ FlatBuffers stub returns NOT_IMPLEMENTED: %s\n", error);
        AIRNTFreeString(error);
    }
    
    // 6. Cleanup
    AIRNTDestroy(context);
    printf("  ✅ Context destroyed\n");
    
    printf("\n[test_airnt_plugin] ALL BASIC TESTS PASSED!\n");
    printf("NOTE: Full integration test requires real AIR bitcode input.\n");
    printf("      Use 'xcrun metal -c shader.metal -o shader.air' to generate test input.\n");
    
    return 0;
}
