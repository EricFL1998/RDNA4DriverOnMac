#ifndef GARTUtils_hpp
#define GARTUtils_hpp

#include "GMCContext.hpp"

bool gmcResetGartTable(volatile uint8_t* vram, size_t vramSize,
                       GMCContext& gmc, uint64_t dummyPte);
void gmcReleaseGartBuffer(IOBufferMemoryDescriptor** md);

#endif
