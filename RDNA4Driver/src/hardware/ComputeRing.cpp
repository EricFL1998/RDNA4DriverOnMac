#include "ComputeRing.hpp"
#include "VramLayout.hpp"
#include <IOKit/IOLib.h>

ComputeRing::ComputeRing()
    : ringBuffer(nullptr)
    , ringSize(0)
    , writePtr(0)
    , readPtr(0)
    , doorbell(nullptr)
    , doorbellIndex(0)
    , currentFence(0)
    , fenceAddress(nullptr)
    , ringVRAMOffset(0)
    , fenceVRAMOffset(0)
{
}

ComputeRing::~ComputeRing() {
    // Cleanup is handled by RDNA4Driver unmapping VRAM
}

bool ComputeRing::init(volatile uint8_t* vramBase, size_t vramSize,
                       volatile uint32_t* doorbellBase, uint32_t dbIndex) {
    IOLog("ComputeRing: init() - vramBase=%p, vramSize=%zu, doorbellIndex=%u\n",
          vramBase, vramSize, dbIndex);
    
    if (!vramBase || !doorbellBase) {
        IOLog("ComputeRing: invalid parameters\n");
        return false;
    }
    
    // The ring, fence and heap live above the PSP/SMU/GFX/GART regions.
    // See VramLayout.hpp: ring 192 MiB, fence 193 MiB, heap 194 MiB.
    ringVRAMOffset = VramLayout::kComputeRing;
    ringSize = static_cast<uint32_t>(VramLayout::kComputeRingSize / sizeof(uint32_t));
    fenceVRAMOffset = VramLayout::kComputeFence;

    const uint64_t ringBytes = static_cast<uint64_t>(ringSize) * sizeof(uint32_t);
    if (ringSize == 0 || ringVRAMOffset > vramSize ||
        ringBytes > vramSize - ringVRAMOffset ||
        fenceVRAMOffset > vramSize ||
        sizeof(uint64_t) > vramSize - fenceVRAMOffset) {
        IOLog("ComputeRing: not enough VRAM (ring+0x%llx fence+0x%llx, have %zu)\n",
              static_cast<unsigned long long>(ringVRAMOffset),
              static_cast<unsigned long long>(fenceVRAMOffset), vramSize);
        return false;
    }
    
    ringBuffer = (volatile uint32_t*)(vramBase + ringVRAMOffset);
    
    // Fence location (8 bytes after the ring)
    fenceAddress = (volatile uint64_t*)(vramBase + fenceVRAMOffset);
    *fenceAddress = 0;  // Initialize fence to 0
    
    // Setup doorbell
    doorbell = doorbellBase + dbIndex;
    doorbellIndex = dbIndex;
    
    // Initialize pointers
    writePtr = 0;
    readPtr = 0;
    currentFence = 0;
    
    IOLog("ComputeRing: initialized - ring at VRAM+0x%llx, fence at VRAM+0x%llx\n",
          ringVRAMOffset, fenceVRAMOffset);
    
    return true;
}

bool ComputeRing::writePM4(const uint32_t* packet, size_t dwordCount) {
    if (!ringBuffer || !packet || dwordCount == 0 || ringSize == 0) {
        IOLog("ComputeRing: writePM4 invalid parameters\n");
        return false;
    }
    
    // Check if ring has enough space
    uint32_t availableSpace = (readPtr > writePtr) 
        ? (readPtr - writePtr - 1) 
        : (ringSize - writePtr + readPtr - 1);
    
    if (dwordCount > availableSpace) {
        IOLog("ComputeRing: ring full (need %zu, have %u)\n", dwordCount, availableSpace);
        return false;
    }
    
    // Write packet to ring (handle wrap-around)
    for (size_t i = 0; i < dwordCount; i++) {
        ringBuffer[writePtr] = packet[i];
        writePtr = (writePtr + 1) % ringSize;
    }
    
    IOLog("ComputeRing: wrote %zu dwords, writePtr now at %u\n", dwordCount, writePtr);
    return true;
}

void ComputeRing::submit() {
    if (!doorbell) {
        IOLog("ComputeRing: doorbell not initialized\n");
        return;
    }
    
    // Ring doorbell to notify GPU of new commands.  Compute queues take a
    // 64-bit doorbell (amdgpu_mm_wdoorbell64) carrying the dword write pointer;
    // the index is a dword index into the BAR2 aperture.
    *reinterpret_cast<volatile uint64_t*>(doorbell) = writePtr;
    
    IOLog("ComputeRing: submitted - doorbell[%u] = %u\n", doorbellIndex, writePtr);
}

bool ComputeRing::waitForFence(uint64_t fenceValue, uint64_t timeoutMS) {
    if (!fenceAddress) {
        IOLog("ComputeRing: fence address not initialized\n");
        return false;
    }
    
    IOLog("ComputeRing: waiting for fence %llu (current: %llu)\n", 
          fenceValue, *fenceAddress);
    
    // Poll once per millisecond. IOSleep gives a coarse but sufficient
    // timeout for a bring-up fence; no timebase conversion is needed.
    for (uint64_t waited = 0; ; waited++) {
        if (*fenceAddress >= fenceValue) {
            break;
        }
        if (waited >= timeoutMS) {
            IOLog("ComputeRing: fence timeout (wanted %llu, got %llu)\n",
                  static_cast<unsigned long long>(fenceValue),
                  static_cast<unsigned long long>(*fenceAddress));
            return false;
        }
        IOSleep(1);
    }
    
    IOLog("ComputeRing: fence %llu reached (value: %llu)\n", fenceValue, *fenceAddress);
    return true;
}

bool ComputeRing::isFull() const {
    uint32_t nextWrite = (writePtr + 1) % ringSize;
    return nextWrite == readPtr;
}

bool ComputeRing::isEmpty() const {
    return writePtr == readPtr;
}
