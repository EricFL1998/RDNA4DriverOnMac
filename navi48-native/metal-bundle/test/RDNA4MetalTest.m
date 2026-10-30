//
//  main.m
//  RDNA4MetalTest - Metal 对象层测试程序
//
//  Tests the complete Metal stack: Metal.framework -> RDNA4Metal.bundle -> RDNA4Driver.kext
//

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

void printDeviceInfo(id<MTLDevice> device) {
    NSLog(@"========================================");
    NSLog(@"Device Information");
    NSLog(@"========================================");
    NSLog(@"Name: %@", device.name);
    NSLog(@"Registry ID: 0x%llx", device.registryID);
    NSLog(@"Low Power: %@", device.isLowPower ? @"YES" : @"NO");
    NSLog(@"Headless: %@", device.isHeadless ? @"YES" : @"NO");
    NSLog(@"Removable: %@", device.isRemovable ? @"YES" : @"NO");
    NSLog(@"Max Working Set: %.2f GB", device.recommendedMaxWorkingSetSize / 1024.0 / 1024.0 / 1024.0);
    NSLog(@"Max Threads Per Threadgroup: %lu x %lu x %lu",
          device.maxThreadsPerThreadgroup.width,
          device.maxThreadsPerThreadgroup.height,
          device.maxThreadsPerThreadgroup.depth);
    NSLog(@"========================================\n");
}

BOOL testVectorAddition(id<MTLDevice> device) {
    NSLog(@"========================================");
    NSLog(@"Test: Vector Addition");
    NSLog(@"========================================");
    
    // Load shader library
    NSString* libraryPath = [[NSBundle mainBundle] pathForResource:@"vectoradd" ofType:@"metallib"];
    if (!libraryPath) {
        NSLog(@"ERROR: vectoradd.metallib not found in bundle");
        return NO;
    }
    
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithFile:libraryPath error:&error];
    if (!library) {
        NSLog(@"ERROR: Failed to load library: %@", error);
        return NO;
    }
    NSLog(@"✅ Loaded library: %@", libraryPath);
    
    // Get kernel function
    id<MTLFunction> function = [library newFunctionWithName:@"vectorAdd"];
    if (!function) {
        NSLog(@"ERROR: Function 'vectorAdd' not found");
        return NO;
    }
    NSLog(@"✅ Found function: vectorAdd");
    
    // Create compute pipeline
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) {
        NSLog(@"ERROR: Failed to create pipeline: %@", error);
        return NO;
    }
    NSLog(@"✅ Created compute pipeline");
    NSLog(@"   Max total threads: %lu", pipeline.maxTotalThreadsPerThreadgroup);
    NSLog(@"   Thread execution width: %lu", pipeline.threadExecutionWidth);
    
    // Create buffers
    const NSUInteger numElements = 1024;
    const NSUInteger bufferSize = numElements * sizeof(float);
    
    id<MTLBuffer> bufferA = [device newBufferWithLength:bufferSize options:MTLResourceStorageModeShared];
    id<MTLBuffer> bufferB = [device newBufferWithLength:bufferSize options:MTLResourceStorageModeShared];
    id<MTLBuffer> bufferC = [device newBufferWithLength:bufferSize options:MTLResourceStorageModeShared];
    
    if (!bufferA || !bufferB || !bufferC) {
        NSLog(@"ERROR: Failed to allocate buffers");
        return NO;
    }
    NSLog(@"✅ Allocated buffers (%lu bytes each)", bufferSize);
    
    // Initialize input data
    float* a = (float*)bufferA.contents;
    float* b = (float*)bufferB.contents;
    float* c = (float*)bufferC.contents;
    
    for (NSUInteger i = 0; i < numElements; i++) {
        a[i] = (float)i;
        b[i] = (float)(i * 2);
        c[i] = 0.0f;
    }
    NSLog(@"✅ Initialized input data");
    NSLog(@"   A[0..3] = [%.1f, %.1f, %.1f, %.1f]", a[0], a[1], a[2], a[3]);
    NSLog(@"   B[0..3] = [%.1f, %.1f, %.1f, %.1f]", b[0], b[1], b[2], b[3]);
    
    // Create command queue and buffer
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    
    // Encode compute command
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:bufferA offset:0 atIndex:0];
    [encoder setBuffer:bufferB offset:0 atIndex:1];
    [encoder setBuffer:bufferC offset:0 atIndex:2];
    
    NSUInteger threadgroupSize = MIN(pipeline.threadExecutionWidth, 256);
    NSUInteger numThreadgroups = (numElements + threadgroupSize - 1) / threadgroupSize;
    
    [encoder dispatchThreadgroups:MTLSizeMake(numThreadgroups, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(threadgroupSize, 1, 1)];
    
    [encoder endEncoding];
    
    NSLog(@"✅ Encoded compute command");
    NSLog(@"   Threadgroups: %lu", numThreadgroups);
    NSLog(@"   Threads per group: %lu", threadgroupSize);
    
    // Submit and wait
    NSLog(@"⏳ Submitting to GPU...");
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    
    if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        NSLog(@"ERROR: Command buffer failed with status: %ld", (long)commandBuffer.status);
        if (commandBuffer.error) {
            NSLog(@"Error: %@", commandBuffer.error);
        }
        return NO;
    }
    NSLog(@"✅ GPU execution completed");
    
    // Verify results
    NSLog(@"🔍 Verifying results...");
    NSUInteger errorCount = 0;
    const NSUInteger maxErrorsToPrint = 10;
    
    for (NSUInteger i = 0; i < numElements; i++) {
        float expected = a[i] + b[i];  // i + 2i = 3i
        float actual = c[i];
        
        if (fabsf(actual - expected) > 0.001f) {
            if (errorCount < maxErrorsToPrint) {
                NSLog(@"  ❌ C[%lu]: got %.1f, expected %.1f", i, actual, expected);
            }
            errorCount++;
        }
    }
    
    if (errorCount > 0) {
        NSLog(@"❌ FAILED: %lu errors out of %lu elements", errorCount, numElements);
        if (errorCount > maxErrorsToPrint) {
            NSLog(@"   (... and %lu more errors)", errorCount - maxErrorsToPrint);
        }
        return NO;
    }
    
    NSLog(@"✅ PASSED: All %lu elements correct", numElements);
    NSLog(@"   C[0..3] = [%.1f, %.1f, %.1f, %.1f]", c[0], c[1], c[2], c[3]);
    NSLog(@"   C[1020..1023] = [%.1f, %.1f, %.1f, %.1f]", c[1020], c[1021], c[1022], c[1023]);
    
    return YES;
}

BOOL testMultipleDispatches(id<MTLDevice> device) {
    NSLog(@"\n========================================");
    NSLog(@"Test: Multiple Dispatches");
    NSLog(@"========================================");
    
    // This test verifies that multiple command buffers can be submitted sequentially
    
    NSString* libraryPath = [[NSBundle mainBundle] pathForResource:@"vectoradd" ofType:@"metallib"];
    if (!libraryPath) {
        NSLog(@"ERROR: vectoradd.metallib not found");
        return NO;
    }
    
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithFile:libraryPath error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@"vectorAdd"];
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    
    if (!pipeline) {
        NSLog(@"ERROR: Failed to create pipeline");
        return NO;
    }
    
    id<MTLCommandQueue> queue = [device newCommandQueue];
    
    const NSUInteger numIterations = 5;
    const NSUInteger numElements = 256;
    const NSUInteger bufferSize = numElements * sizeof(float);
    
    for (NSUInteger iter = 0; iter < numIterations; iter++) {
        // Create fresh buffers for each iteration
        id<MTLBuffer> bufferA = [device newBufferWithLength:bufferSize options:MTLResourceStorageModeShared];
        id<MTLBuffer> bufferB = [device newBufferWithLength:bufferSize options:MTLResourceStorageModeShared];
        id<MTLBuffer> bufferC = [device newBufferWithLength:bufferSize options:MTLResourceStorageModeShared];
        
        float* a = (float*)bufferA.contents;
        float* b = (float*)bufferB.contents;
        float* c = (float*)bufferC.contents;
        
        // Initialize with iteration-specific values
        for (NSUInteger i = 0; i < numElements; i++) {
            a[i] = (float)(i + iter * 1000);
            b[i] = (float)(i * 3 + iter);
            c[i] = 0.0f;
        }
        
        // Submit command
        id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:bufferA offset:0 atIndex:0];
        [encoder setBuffer:bufferB offset:0 atIndex:1];
        [encoder setBuffer:bufferC offset:0 atIndex:2];
        [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [encoder endEncoding];
        
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
            NSLog(@"❌ Iteration %lu failed", iter);
            return NO;
        }
        
        // Quick verify
        BOOL iterOK = YES;
        for (NSUInteger i = 0; i < numElements; i++) {
            float expected = a[i] + b[i];
            if (fabsf(c[i] - expected) > 0.001f) {
                iterOK = NO;
                break;
            }
        }
        
        if (iterOK) {
            NSLog(@"✅ Iteration %lu/%lu passed", iter + 1, numIterations);
        } else {
            NSLog(@"❌ Iteration %lu/%lu failed", iter + 1, numIterations);
            return NO;
        }
    }
    
    NSLog(@"✅ All %lu iterations passed", numIterations);
    return YES;
}

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        NSLog(@"========================================");
        NSLog(@"RDNA4 Metal Test");
        NSLog(@"========================================\n");
        
        // Get default device
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            NSLog(@"❌ FATAL: No Metal device found");
            NSLog(@"   Make sure RDNA4Metal.bundle is installed in /Library/GPUBundles/");
            NSLog(@"   and RDNA4Driver.kext / RDNA4Accel.kext are loaded");
            return 1;
        }
        
        NSLog(@"✅ Found Metal device: %@\n", device.name);
        
        // Print device info
        printDeviceInfo(device);
        
        // Run tests
        BOOL allPassed = YES;
        
        // Test 1: Basic vector addition
        if (!testVectorAddition(device)) {
            allPassed = NO;
        }
        
        // Test 2: Multiple dispatches
        if (!testMultipleDispatches(device)) {
            allPassed = NO;
        }
        
        // Summary
        NSLog(@"\n========================================");
        NSLog(@"Test Summary");
        NSLog(@"========================================");
        if (allPassed) {
            NSLog(@"✅ ALL TESTS PASSED");
            NSLog(@"========================================");
            NSLog(@"RDNA4 Metal object layer is FULLY FUNCTIONAL!");
            NSLog(@"User-space Metal applications can now");
            NSLog(@"submit compute work to RDNA4 GPU!");
            NSLog(@"========================================");
            return 0;
        } else {
            NSLog(@"❌ SOME TESTS FAILED");
            NSLog(@"========================================");
            NSLog(@"Check logs above for details.");
            NSLog(@"========================================");
            return 1;
        }
    }
}
