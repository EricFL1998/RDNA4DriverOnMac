#import "RDNA4ComputePipelineState.h"
#import "RDNA4UserClient.h"
#import "RDNA4Device.h"
#import "RDNA4Function.h"
#import "RDNA4Library.h"
#import <IOKit/IOKitLib.h>

@implementation RDNA4ComputePipelineState {
    RDNA4Device* _device;
    RDNA4CompiledShader* _compiledShader;
    uint64_t _shaderGPUAddress;
    NSUInteger _maxTotalThreadsPerThreadgroup;
    NSUInteger _threadExecutionWidth;
    NSUInteger _staticThreadgroupMemoryLength;
}

- (instancetype)initWithDevice:(RDNA4Device*)device
                      function:(RDNA4Function*)function
                         error:(NSError**)error {
    self = [super init];
    if (self) {
        _device = device;
        _compiledShader = [function compiledShader];
        
        if (!_compiledShader || !_compiledShader.machineCode) {
            if (error) {
                *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                             code:-1
                                         userInfo:@{NSLocalizedDescriptionKey: @"Invalid compiled shader"}];
            }
            return nil;
        }
        
        // Allocate VRAM for shader code
        if (![self uploadShaderToVRAM:error]) {
            return nil;
        }
        
        // Set metadata
        _maxTotalThreadsPerThreadgroup = 1024;  // RDNA4 limit
        _threadExecutionWidth = _compiledShader.wavefrontSize;  // Typically 32
        _staticThreadgroupMemoryLength = _compiledShader.ldsBytes;
        
        NSLog(@"RDNA4ComputePipelineState: Created pipeline for '%@'", function.name);
        NSLog(@"  Shader GPU address: 0x%llx", _shaderGPUAddress);
        NSLog(@"  RSRC1: 0x%08x, RSRC2: 0x%08x", 
              _compiledShader.computePgmRsrc1, _compiledShader.computePgmRsrc2);
    }
    return self;
}

- (BOOL)uploadShaderToVRAM:(NSError**)error {
    RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
    if (!link) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:-2
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                   @"RDNA4Driver service not available"}];
        }
        return NO;
    }

    const size_t codeSize = _compiledShader.machineCodeSize;
    if (codeSize == 0 || !_compiledShader.machineCode) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:-3
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                   @"compiled shader is empty"}];
        }
        return NO;
    }

    // Shader code must be executable by the SPI, so it goes straight into the
    // mapped VRAM window: the driver hands out the address, the mapping is the
    // write path.
    const uint64_t allocSize = ((uint64_t)codeSize + 0x3FFF) & ~0x3FFFull;
    const uint64_t gpuAddress = RDNA4GpuAllocate(link, allocSize);
    if (gpuAddress == 0) {
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:-4
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                   @"failed to allocate shader VRAM"}];
        }
        return NO;
    }

    void* mapped = RDNA4GpuPointerForAddress(link, gpuAddress);
    if (!mapped || allocSize > link->windowSize) {
        RDNA4GpuFree(link, gpuAddress);
        if (error) {
            *error = [NSError errorWithDomain:@"com.rdna4.metal"
                                         code:-5
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                   @"shader buffer is not mappable"}];
        }
        return NO;
    }

    memcpy(mapped, _compiledShader.machineCode, codeSize);
    // A stray fetch past the blob hits s_endpgm instead of uninitialised VRAM.
    for (size_t off = codeSize; off + 4 <= (size_t)allocSize; off += 4) {
        *(uint32_t*)((uint8_t*)mapped + off) = 0xBFB00000u;
    }

    _shaderGPUAddress = gpuAddress;
    return YES;
}

- (void)dealloc {
    if (_shaderGPUAddress != 0) {
        RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
        if (link) RDNA4GpuFree(link, _shaderGPUAddress);
        _shaderGPUAddress = 0;
    }
}

- (id<MTLDevice>)device {
    return _device;
}

- (NSUInteger)maxTotalThreadsPerThreadgroup {
    return _maxTotalThreadsPerThreadgroup;
}

- (NSUInteger)threadExecutionWidth {
    return _threadExecutionWidth;
}

- (NSUInteger)staticThreadgroupMemoryLength {
    return _staticThreadgroupMemoryLength;
}

- (uint64_t)shaderGPUAddress {
    return _shaderGPUAddress;
}

- (uint32_t)computePgmRsrc1 {
    return _compiledShader.computePgmRsrc1;
}

- (uint32_t)computePgmRsrc2 {
    return _compiledShader.computePgmRsrc2;
}

- (RDNA4CompiledShader*)compiledShader {
    return _compiledShader;
}

// MTLComputePipelineState optional methods
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)imageblockDimensions {
    return 0;  // Not applicable for compute
}

- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function {
    return nil;  // Not yet supported
}

- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor*)descriptor {
    return nil;  // Not yet supported
}

- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor*)descriptor {
    return nil;  // Not yet supported
}

@end
