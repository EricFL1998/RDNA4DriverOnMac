#include "GARTUtils.hpp"

bool gmcResetGartTable(volatile uint8_t* vram, size_t vramSize,
                       GMCContext& gmc, uint64_t dummyPte) {
    constexpr uint32_t firstAllocatedPage = 0x1000;
    if (!vram || gmc.gartTableSize == 0 || gmc.gartNextPage < firstAllocatedPage) {
        return false;
    }
    const uint32_t totalPages = gmc.gartTableSize / sizeof(uint64_t);
    if (gmc.gartNextPage > totalPages ||
        gmc.gartTableVramOffset > vramSize ||
        gmc.gartTableSize > vramSize - gmc.gartTableVramOffset) {
        return false;
    }

    volatile uint64_t* table = reinterpret_cast<volatile uint64_t*>(
        vram + gmc.gartTableVramOffset);
    for (uint32_t i = firstAllocatedPage; i < gmc.gartNextPage; i++) {
        table[i] = dummyPte;
    }
    gmc.gartNextPage = firstAllocatedPage;
    return true;
}

void gmcReleaseGartBuffer(IOBufferMemoryDescriptor** md) {
    if (!md || !*md) return;
    (*md)->complete();
    (*md)->release();
    *md = nullptr;
}
