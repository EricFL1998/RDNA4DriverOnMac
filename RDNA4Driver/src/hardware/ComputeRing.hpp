#ifndef ComputeRing_hpp
#define ComputeRing_hpp

#include <IOKit/IOTypes.h>
#include <IOKit/IOMemoryDescriptor.h>

/**
 * ComputeRing - GFX Compute Ring Buffer Manager
 * 
 * Manages the GPU compute command ring buffer for submitting PM4 packets.
 * Phase 2.3: Basic ring write and fence synchronization.
 */
class ComputeRing {
public:
    ComputeRing();
    ~ComputeRing();
    
    // Initialize ring buffer (allocate VRAM + map doorbell)
    bool init(volatile uint8_t* vramBase, size_t vramSize,
              volatile uint32_t* doorbellBase, uint32_t doorbellIndex);
    
    // Write PM4 packet to ring
    bool writePM4(const uint32_t* packet, size_t dwordCount);
    
    // Submit all pending commands (ring doorbell)
    void submit();
    
    // Wait for fence value
    bool waitForFence(uint64_t fenceValue, uint64_t timeoutMS);
    
    // Get current fence value
    uint64_t getCurrentFence() const { return currentFence; }
    // Reserve the next fence value for a submission.  Values are monotonic so
    // a stale slot from an earlier packet can never satisfy a later wait.
    uint64_t allocateFence() { return ++currentFence; }
    
    // Get ring statistics
    uint32_t getWritePointer() const { return writePtr; }
    uint32_t getReadPointer() const { return readPtr; }
    bool isFull() const;
    bool isEmpty() const;

    // Update the CP's read pointer so released ring space becomes reusable.
    void setReadPointer(uint32_t rptr);
    
    // Get VRAM offsets
    uint64_t getFenceVRAMOffset() const { return fenceVRAMOffset; }
    
private:
    // Ring buffer in VRAM
    volatile uint32_t* ringBuffer;
    uint32_t ringSize;       // Size in dwords
    uint32_t writePtr;       // Write pointer (dwords)
    uint32_t readPtr;        // Read pointer (dwords)
    
    // Doorbell for ring submission
    volatile uint32_t* doorbell;
    uint32_t doorbellIndex;
    
    // Fence tracking
    uint64_t currentFence;
    volatile uint64_t* fenceAddress;  // GPU-writable fence location
    
    // Ring buffer allocation
    uint64_t ringVRAMOffset;
    uint64_t fenceVRAMOffset;
};

#endif /* ComputeRing_hpp */
