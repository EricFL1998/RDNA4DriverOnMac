#pragma once
#include <IOKit/IOTypes.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

struct GMCContext {
    // VRAM 信息
    uint64_t vramStart { 0 };
    uint64_t vramEnd { 0 };
    uint64_t vramSize { 0 };

    // GART 配置
    uint64_t gartStart { 0 };
    uint64_t gartEnd { 0 };
    uint64_t gartSize { 0 };

    // Dummy page（用于页错误重定向和 GART 空表项）
    IOBufferMemoryDescriptor* dummyPage { nullptr };
    uint64_t dummyPagePhys { 0 };

    // GART 页表位于 VRAM（VRAM 相对偏移），硬件只接受 GPU 可寻址地址
    uint64_t gartTableVramOffset { 0 };
    uint32_t gartTableSize { 0 };

    // Bump allocator over the GART aperture for system-memory buffers
    // (MES rings, MQDs, writeback slots). Index 0..0xFFF stays untouched.
    uint32_t gartNextPage { 0x1000 };

    bool inited { false };
    bool gartEnabled { false };
};

class RDNA4Driver;
bool gmcInit(RDNA4Driver& driver, GMCContext& gmc);
bool gmcEnableGFXHUBGART(RDNA4Driver& driver, GMCContext& gmc);
void gmcFini(GMCContext& gmc);
void gmcHDPFlush(RDNA4Driver& driver);

// Allocates physically contiguous system memory, maps it into the GART page
// table and returns its GPU (GART aperture) address.  On success *outMd holds a
// retained descriptor the caller must keep alive and release.
uint64_t gmcAllocGartBuffer(RDNA4Driver& driver, GMCContext& gmc, size_t size,
                            IOBufferMemoryDescriptor** outMd);
// Flushes both hubs' TLBs after page table edits.
void gmcInvalidateTlb(RDNA4Driver& driver);
