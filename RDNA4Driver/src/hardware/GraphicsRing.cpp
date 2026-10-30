//
//  GraphicsRing.cpp
//  RDNA4Driver - Graphics command ring
//

#include "GraphicsRing.hpp"
#include <IOKit/IOLib.h>

GraphicsRing::GraphicsRing() {}

GraphicsRing::~GraphicsRing() {}

bool GraphicsRing::init(volatile uint8_t* vramBase, size_t vramSize,
                        volatile uint32_t* doorbellBase, uint32_t dbIndex,
                        uint64_t ringVramOffsetIn, uint64_t ringBytes,
                        uint64_t fenceVramOffsetIn) {
    if (!vramBase || !doorbellBase || ringBytes == 0 ||
        ringVramOffsetIn > vramSize || ringBytes > vramSize - ringVramOffsetIn ||
        fenceVramOffsetIn > vramSize ||
        sizeof(uint64_t) > vramSize - fenceVramOffsetIn) {
        IOLog("GraphicsRing: invalid parameters\n");
        return false;
    }
    const size_t dwords = static_cast<size_t>(ringBytes / 4u);
    if (dwords < 8 || (dwords & (dwords - 1u)) != 0) {
        IOLog("GraphicsRing: %zu dwords is not a power of two\n", dwords);
        return false;
    }

    ringVramOffset = ringVramOffsetIn;
    fenceVramOffset = fenceVramOffsetIn;
    ringSizeDwords = dwords;
    ring = reinterpret_cast<volatile uint32_t*>(
        const_cast<uint8_t*>(vramBase) + ringVramOffset);
    fence = reinterpret_cast<volatile uint64_t*>(
        const_cast<uint8_t*>(vramBase) + fenceVramOffset);
    *fence = 0;

    doorbellIndex = dbIndex;
    doorbell = doorbellBase + dbIndex;
    writePtr = 0;
    readPtr = 0;
    currentFence = 0;

    IOLog("GraphicsRing: %zu dwords at VRAM+0x%llx, fence at VRAM+0x%llx, "
          "doorbell %u\n", ringSizeDwords,
          static_cast<unsigned long long>(ringVramOffset),
          static_cast<unsigned long long>(fenceVramOffset), doorbellIndex);
    return true;
}

bool GraphicsRing::writePM4(const uint32_t* pm4, size_t dwordCount) {
    if (!ring || !pm4 || dwordCount == 0 || ringSizeDwords == 0) return false;

    const uint32_t available = (readPtr > writePtr)
        ? (readPtr - writePtr - 1u)
        : (static_cast<uint32_t>(ringSizeDwords) - writePtr + readPtr - 1u);
    if (dwordCount > available) {
        IOLog("GraphicsRing: ring full (need %zu, have %u)\n", dwordCount, available);
        return false;
    }

    for (size_t i = 0; i < dwordCount; i++) {
        ring[writePtr] = pm4[i];
        writePtr = (writePtr + 1u) % static_cast<uint32_t>(ringSizeDwords);
    }
    return true;
}

void GraphicsRing::setReadPointer(uint32_t rptr) {
    if (ringSizeDwords == 0 || rptr >= ringSizeDwords) return;
    readPtr = rptr;
}

void GraphicsRing::submit() {
    if (!doorbell) return;
    // gfx_v12_0_ring_set_wptr_gfx(): WDOORBELL64(ring->doorbell_index, wptr).
    // amdgpu_mm_wdoorbell64 indexes a u32* and stores 64 bits, so the doorbell
    // dword and the one above it are written together: the CP samples the pair
    // as one 64-bit write pointer, and a half-write is not guaranteed to
    // register at all.
    *reinterpret_cast<volatile uint64_t*>(
        const_cast<uint32_t*>(doorbell)) = static_cast<uint64_t>(writePtr);
}

bool GraphicsRing::waitForFence(uint64_t fenceValue, uint64_t timeoutMS) {
    if (!fence) return false;
    for (uint64_t waited = 0; ; waited++) {
        if (*fence >= fenceValue) return true;
        if (waited >= timeoutMS) {
            IOLog("GraphicsRing: fence timeout (wanted %llu, got %llu)\n",
                  static_cast<unsigned long long>(fenceValue),
                  static_cast<unsigned long long>(*fence));
            return false;
        }
        IOSleep(1);
    }
}
