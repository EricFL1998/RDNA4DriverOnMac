//
//  GraphicsRing.hpp
//  RDNA4Driver - Graphics command ring
//
//  Owns the ring buffer and fence page for the GFX queue. Queue bring-up
//  (MQD, MES mapping) lives in GraphicsQueue.cpp; this class only writes PM4
//  into the ring, rings the doorbell and polls the fence.
//

#ifndef GraphicsRing_hpp
#define GraphicsRing_hpp

#include <IOKit/IOTypes.h>
#include <stddef.h>

class GraphicsRing {
public:
    GraphicsRing();
    ~GraphicsRing();

    bool init(volatile uint8_t* vramBase, size_t vramSize,
              volatile uint32_t* doorbellBase, uint32_t dbIndex,
              uint64_t ringVramOffset, uint64_t ringBytes,
              uint64_t fenceVramOffset);

    bool writePM4(const uint32_t* pm4, size_t dwordCount);
    void setReadPointer(uint32_t rptr);
    void submit();

    uint32_t getWritePointer() const { return writePtr; }
    uint32_t getReadPointer() const { return readPtr; }
    size_t getRingDwords() const { return ringSizeDwords; }

    uint64_t allocateFence() { return ++currentFence; }
    bool waitForFence(uint64_t fenceValue, uint64_t timeoutMS);
    uint64_t getFenceVRAMOffset() const { return fenceVramOffset; }

private:
    volatile uint32_t* ring { nullptr };
    volatile uint64_t* fence { nullptr };
    volatile uint32_t* doorbell { nullptr };

    uint32_t doorbellIndex { 0 };
    size_t ringSizeDwords { 0 };
    uint32_t writePtr { 0 };
    uint32_t readPtr { 0 };
    uint64_t currentFence { 0 };
    uint64_t ringVramOffset { 0 };
    uint64_t fenceVramOffset { 0 };
};

#endif /* GraphicsRing_hpp */
