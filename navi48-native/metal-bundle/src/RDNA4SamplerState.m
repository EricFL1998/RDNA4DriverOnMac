#import "RDNA4SamplerState.h"
#import "RDNA4Device.h"
#import "RDNA4UserClient.h"
#include "gfx12_descriptors.h"

@implementation RDNA4SamplerState {
    RDNA4Device* _device;
    uint64_t _gpuAddress;
    uint32_t _descriptor[4];
}

- (instancetype)initWithDevice:(RDNA4Device*)device
                    descriptor:(MTLSamplerDescriptor*)descriptor {
    self = [super init];
    if (!self || !descriptor) return nil;

    _device = device;

    Gfx12SamplerDesc state = {};
    switch (descriptor.minFilter) {
        case MTLSamplerMinMagFilterLinear: state.minFilter = kGfx12FilterLinear; break;
        default: state.minFilter = kGfx12FilterPoint; break;
    }
    switch (descriptor.magFilter) {
        case MTLSamplerMinMagFilterLinear: state.magFilter = kGfx12FilterLinear; break;
        default: state.magFilter = kGfx12FilterPoint; break;
    }
    switch (descriptor.mipFilter) {
        case MTLSamplerMipFilterNearest: state.mipFilter = kGfx12MipFilterPoint; break;
        case MTLSamplerMipFilterLinear: state.mipFilter = kGfx12MipFilterLinear; break;
        default: state.mipFilter = kGfx12MipFilterNone; break;
    }
    state.clampX = state.clampY = state.clampZ = kGfx12WrapClampToEdge;
    switch (descriptor.sAddressMode) {
        case MTLSamplerAddressModeRepeat: state.clampX = kGfx12WrapRepeat; break;
        case MTLSamplerAddressModeMirrorRepeat: state.clampX = kGfx12WrapMirror; break;
        case MTLSamplerAddressModeClampToZero:
        case MTLSamplerAddressModeClampToBorderColor: state.clampX = kGfx12WrapClampToBorder; break;
        case MTLSamplerAddressModeMirrorClampToEdge: state.clampX = kGfx12WrapMirrorOnceToEdge; break;
        default: break;
    }
    switch (descriptor.tAddressMode) {
        case MTLSamplerAddressModeRepeat: state.clampY = kGfx12WrapRepeat; break;
        case MTLSamplerAddressModeMirrorRepeat: state.clampY = kGfx12WrapMirror; break;
        case MTLSamplerAddressModeClampToZero:
        case MTLSamplerAddressModeClampToBorderColor: state.clampY = kGfx12WrapClampToBorder; break;
        case MTLSamplerAddressModeMirrorClampToEdge: state.clampY = kGfx12WrapMirrorOnceToEdge; break;
        default: break;
    }
    state.unnormalizedCoords = descriptor.normalizedCoordinates ? 0 : 1;
    state.minLod = (float)descriptor.lodMinClamp;
    state.maxLod = (descriptor.lodMaxClamp > 17.0f) ? 17.0f : (float)descriptor.lodMaxClamp;
    gfx12MakeSamplerDescriptor(&state, _descriptor);

    RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
    if (!link) return nil;
    _gpuAddress = RDNA4GpuAllocate(link, sizeof(_descriptor));
    if (!_gpuAddress) return nil;
    void* mapped = RDNA4GpuPointerForAddress(link, _gpuAddress);
    if (!mapped) {
        RDNA4GpuFree(link, _gpuAddress);
        _gpuAddress = 0;
        return nil;
    }
    memcpy(mapped, _descriptor, sizeof(_descriptor));
    return self;
}

- (void)dealloc {
    if (_gpuAddress) {
        RDNA4GpuLink* link = RDNA4GpuLinkAcquire();
        if (link) RDNA4GpuFree(link, _gpuAddress);
        _gpuAddress = 0;
    }
}

- (id<MTLDevice>)device {
    return _device;
}

- (uint64_t)gpuAddress {
    return _gpuAddress;
}

@end
