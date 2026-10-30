#include "PM4BuilderKernel.hpp"
#include <IOKit/IOLib.h>

// Use stub shader for now (real shader compiled on hackintosh)
#define USE_STUB_SHADER
#include "../shaders/compiled/vectoradd_gfx12.h"

bool RDNA4Driver::testVectorAdd() {
    LOG("======================================");
    LOG("  Vector Add Test (FULL)");
    LOG("======================================");
    
    // Test parameters
    const size_t numElements = 1024;
    const size_t bufferSize = numElements * sizeof(float);
    const size_t workgroupSize = 256;  // Threads per workgroup
    const size_t numWorkgroups = (numElements + workgroupSize - 1) / workgroupSize;  // 4
    
    LOG("Configuration:");
    LOG("  Elements: %zu", numElements);
    LOG("  Buffer size: %zu bytes", bufferSize);
    LOG("  Workgroup size: %zu threads", workgroupSize);
    LOG("  Num workgroups: %zu", numWorkgroups);
    
    // Step 1: Allocate buffers
    LOG("");
    LOG("Step 1: Allocating VRAM buffers...");
    uint64_t bufferA = allocateVRAMBuffer(bufferSize);
    uint64_t bufferB = allocateVRAMBuffer(bufferSize);
    uint64_t bufferC = allocateVRAMBuffer(bufferSize);
    uint64_t shaderBuffer = allocateVRAMBuffer(16 * 1024);  // 16KB for shader
    
    if (!bufferA || !bufferB || !bufferC || !shaderBuffer) {
        LOG_ERROR("testVectorAdd: buffer allocation failed");
        return false;
    }
    
    LOG("  Buffer A (input):  GPU 0x%llx", bufferA);
    LOG("  Buffer B (input):  GPU 0x%llx", bufferB);
    LOG("  Buffer C (output): GPU 0x%llx", bufferC);
    LOG("  Shader buffer:     GPU 0x%llx", shaderBuffer);
    
    // Step 2: Initialize input data
    LOG("");
    LOG("Step 2: Initializing input data...");
    
    uint64_t vramOffsetA = bufferA - gmcContext.vramStart;
    uint64_t vramOffsetB = bufferB - gmcContext.vramStart;
    uint64_t vramOffsetC = bufferC - gmcContext.vramStart;
    
    volatile float* vramA = (volatile float*)((uint8_t*)vram + vramOffsetA);
    volatile float* vramB = (volatile float*)((uint8_t*)vram + vramOffsetB);
    volatile float* vramC = (volatile float*)((uint8_t*)vram + vramOffsetC);
    
    for (size_t i = 0; i < numElements; i++) {
        vramA[i] = (float)i;
        vramB[i] = (float)(i * 2);
        vramC[i] = 0.0f;  // Clear output
    }
    
    LOG("  Input A: [0.0, 1.0, 2.0, ..., %f]", (float)(numElements-1));
    LOG("  Input B: [0.0, 2.0, 4.0, ..., %f]", (float)(numElements-1)*2);
    LOG("  Output C: [cleared to 0.0]");
    
    // Step 3: Upload shader to VRAM
    LOG("");
    LOG("Step 3: Uploading shader to VRAM...");
    
    uint64_t shaderVramOffset = shaderBuffer - gmcContext.vramStart;
    uint8_t* shaderVram = (uint8_t*)vram + shaderVramOffset;
    
    // Copy shader binary
    for (size_t i = 0; i < g_vectorAddShaderSize; i++) {
        shaderVram[i] = g_vectorAddShader[i];
    }
    
    LOG("  Shader size: %u bytes", g_vectorAddShaderSize);
    LOG("  Shader at: GPU 0x%llx (VRAM+0x%llx)", shaderBuffer, shaderVramOffset);
    
#ifdef USE_STUB_SHADER
    LOG("  ⚠️  Using STUB shader (run compile_shader.sh for real shader)");
#endif
    
    // Step 4: Build PM4 command packet
    LOG("");
    LOG("Step 4: Building PM4 command packet...");
    
    PM4BuilderKernel pm4;
    
    // Set compute shader program
    pm4.setComputeProgram(shaderBuffer, g_vectorAddRSRC1, g_vectorAddRSRC2);
    LOG("  ✅ Set compute program (RSRC1=0x%08x, RSRC2=0x%08x)", 
        g_vectorAddRSRC1, g_vectorAddRSRC2);
    
    // Set workgroup size
    pm4.setComputeWorkgroupSize(workgroupSize, 1, 1);
    LOG("  ✅ Set workgroup size (%zu, 1, 1)", workgroupSize);
    
    // Set buffer addresses (user data SGPRs)
    pm4.setComputeUserData(0, bufferA);  // s[0:1] = buffer A
    pm4.setComputeUserData(1, bufferB);  // s[2:3] = buffer B
    pm4.setComputeUserData(2, bufferC);  // s[4:5] = buffer C
    LOG("  ✅ Set buffer pointers (A, B, C)");
    
    // Dispatch compute
    pm4.emitDispatchDirect(numWorkgroups, 1, 1);
    LOG("  ✅ Emit DISPATCH_DIRECT (%zu, 1, 1)", numWorkgroups);
    
    // Flush and fence
    pm4.emitCsPartialFlush();
    LOG("  ✅ Emit CS_PARTIAL_FLUSH");
    
    uint64_t fenceValue = computeRing->allocateFence();
    uint64_t fenceGpuAddr = gmcContext.vramStart + computeRing->getFenceVRAMOffset();
    pm4.emitFenceWrite(fenceGpuAddr, fenceValue);
    LOG("  ✅ Emit fence write (value=%llu, addr=0x%llx)", fenceValue, fenceGpuAddr);
    
    LOG("  Total PM4 packet size: %zu dwords (%zu bytes)", 
        pm4.getPacketCount(), pm4.getPacketCount() * 4);
    
    // Step 5: Submit commands to GPU
    LOG("");
    LOG("Step 5: Submitting PM4 commands to GPU...");
    
    if (!submitComputeCommands(pm4.getPackets(), pm4.getPacketCount())) {
        LOG_ERROR("testVectorAdd: command submission failed");
        return false;
    }
    
    LOG("  ✅ Commands submitted to Compute Ring");
    
    // Step 6: Wait for completion
    LOG("");
    LOG("Step 6: Waiting for GPU completion...");
    
    if (!waitForComputeFence(fenceValue, 5000)) {  // 5 second timeout
        LOG_ERROR("testVectorAdd: fence timeout (GPU may not have executed)");
        return false;
    }
    
    LOG("  ✅ Fence reached (GPU completed)");
    
    // Step 7: Verify results
    LOG("");
    LOG("Step 7: Verifying results...");
    
    bool pass = true;
    size_t errorCount = 0;
    const size_t maxErrors = 10;
    
    for (size_t i = 0; i < numElements; i++) {
        float expected = (float)i + (float)(i * 2);  // a[i] + b[i] = i + 2i = 3i
        float actual = vramC[i];
        
        if (actual != expected) {
            if (errorCount < maxErrors) {
                LOG_ERROR("  Mismatch at [%zu]: got %f, expected %f", i, actual, expected);
            }
            errorCount++;
            pass = false;
        }
    }
    
    if (errorCount > maxErrors) {
        LOG_ERROR("  ... and %zu more errors", errorCount - maxErrors);
    }
    
    // Print sample results
    LOG("");
    LOG("Sample results:");
    LOG("  C[0] = %f (expected 0.0)", vramC[0]);
    LOG("  C[1] = %f (expected 3.0)", vramC[1]);
    LOG("  C[10] = %f (expected 30.0)", vramC[10]);
    LOG("  C[100] = %f (expected 300.0)", vramC[100]);
    LOG("  C[1023] = %f (expected 3069.0)", vramC[1023]);
    
    LOG("");
    LOG("======================================");
    if (pass) {
        LOG("  ✅ Vector Add Test: PASSED!");
        LOG("======================================");
        LOG("  All %zu elements computed correctly", numElements);
        LOG("  GPU compute execution successful!");
    } else {
        LOG("  ❌ Vector Add Test: FAILED!");
        LOG("======================================");
        LOG("  %zu errors found (out of %zu elements)", errorCount, numElements);
        LOG("");
        LOG("Possible causes:");
        LOG("  1. Stub shader used (need real shader from compile_shader.sh)");
        LOG("  2. PM4 packet format incorrect");
        LOG("  3. GFX engine not properly initialized");
        LOG("  4. Buffer addresses misconfigured");
    }
    LOG("======================================");
    
    return pass;
}
