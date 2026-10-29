#ifndef PM4BuilderKernel_hpp
#define PM4BuilderKernel_hpp

#include <IOKit/IOTypes.h>

/**
 * PM4BuilderKernel - Kernel-compatible PM4 packet builder for GFX12 compute
 *
 * Register indices below are PM4 SH-register indices, not raw MMIO offsets.
 * They are derived from the official gc_12_0_0 register block:
 *
 *     index = regCOMPUTE_x + GC_BASE(0x1260) - SET_SH_REG_START(0x2C00)
 *
 * Mesa applies the same transform as (register - 0xB000) / 4, and Linux as
 * SOC15_REG_OFFSET(GC, 0, reg) - PACKET3_SET_SH_REG_START.
 */

// GFX12 PM4 Type-3 opcodes
#define PM4_OP_NOP                  0x10
#define PM4_OP_SET_SH_REG           0x76
#define PM4_OP_SET_SH_REG_INDEX     0x9B
#define PM4_OP_SET_CONTEXT_REG      0x69
#define PM4_OP_DISPATCH_DIRECT      0x15
#define PM4_OP_WRITE_DATA           0x37
#define PM4_OP_INDIRECT_BUFFER      0x3F
#define PM4_OP_EVENT_WRITE          0x46
#define PM4_OP_RELEASE_MEM          0x49

// Type-3 header layout: [31:30]=3, [29:16]=count-1, [15:8]=opcode
// Bit 1 marks a packet as compute (PACKET3_COMPUTE in soc15d.h).
#define PM4_TYPE3                   3u
#define PM4_TYPE3_SHIFT             30
#define PM4_COMPUTE_FLAG            (1u << 1)

// GFX12 compute SH register indices
#define GFX12_COMPUTE_DISPATCH_INITIATOR      0x200
#define GFX12_COMPUTE_DIM_X                   0x201
#define GFX12_COMPUTE_DIM_Y                   0x202
#define GFX12_COMPUTE_DIM_Z                   0x203
#define GFX12_COMPUTE_START_X                 0x204
#define GFX12_COMPUTE_START_Y                 0x205
#define GFX12_COMPUTE_START_Z                 0x206
#define GFX12_COMPUTE_NUM_THREAD_X            0x207
#define GFX12_COMPUTE_NUM_THREAD_Y            0x208
#define GFX12_COMPUTE_NUM_THREAD_Z            0x209
#define GFX12_COMPUTE_PGM_LO                  0x20C
#define GFX12_COMPUTE_PGM_HI                  0x20D
#define GFX12_COMPUTE_PGM_RSRC1               0x212
#define GFX12_COMPUTE_PGM_RSRC2               0x213
#define GFX12_COMPUTE_VMID                    0x214
#define GFX12_COMPUTE_RESOURCE_LIMITS         0x215
#define GFX12_COMPUTE_STATIC_THREAD_MGMT_SE0  0x216
#define GFX12_COMPUTE_STATIC_THREAD_MGMT_SE1  0x217
#define GFX12_COMPUTE_TMPRING_SIZE            0x218
#define GFX12_COMPUTE_STATIC_THREAD_MGMT_SE2  0x219
#define GFX12_COMPUTE_STATIC_THREAD_MGMT_SE3  0x21A
#define GFX12_COMPUTE_PGM_RSRC3               0x228
#define GFX12_COMPUTE_USER_DATA_0             0x240

// COMPUTE_DISPATCH_INITIATOR bits (gc_12_0_0_sh_mask.h)
#define DISPATCH_INITIATOR_COMPUTE_SHADER_EN   (1u << 0)
#define DISPATCH_INITIATOR_FORCE_START_AT_000  (1u << 2)
#define DISPATCH_INITIATOR_ORDER_MODE          (1u << 6)
#define DISPATCH_INITIATOR_CS_W32_EN           (1u << 15)

// Linux, Mesa and Cobalt64's verified dispatch program
// COMPUTE_SHADER_EN | FORCE_START_AT_000 | ORDER_MODE | CS_W32_EN.
#define DISPATCH_INITIATOR_DEFAULT \
    (DISPATCH_INITIATOR_COMPUTE_SHADER_EN | \
     DISPATCH_INITIATOR_FORCE_START_AT_000 | \
     DISPATCH_INITIATOR_ORDER_MODE | \
     DISPATCH_INITIATOR_CS_W32_EN)

// Event fields shared by EVENT_WRITE and RELEASE_MEM (soc15d.h):
// EVENT_TYPE bits [5:0], EVENT_INDEX bits [11:8].
#define EVENT_TYPE_CS_PARTIAL_FLUSH   0x07
#define EVENT_INDEX_CS_PARTIAL_FLUSH  0x04

#define PM4_EVENT_TYPE(x)             ((x) & 0x3F)
#define PM4_EVENT_INDEX(x)            (((x) & 0xF) << 8)

// RELEASE_MEM payload fields (soc15d.h)
#define RELEASE_MEM_EVENT_TYPE(x)     PM4_EVENT_TYPE(x)
#define RELEASE_MEM_EVENT_INDEX(x)    PM4_EVENT_INDEX(x)
#define RELEASE_MEM_GCR_SEQ           (1u << 21)
#define RELEASE_MEM_GCR_GL2_WB        (1u << 22)
#define RELEASE_MEM_INT_SEL(x)        (((x) & 0x3) << 24)
#define RELEASE_MEM_CACHE_POLICY(x)   (((x) & 0x3) << 25)
#define RELEASE_MEM_DATA_SEL(x)       (((x) & 0x7) << 29)
#define RELEASE_MEM_CACHE_FLUSH_AND_INV_TS_EVENT 0x14

class PM4BuilderKernel {
public:
    PM4BuilderKernel();
    ~PM4BuilderKernel();

    // Reset builder (clear packet buffer)
    void reset();

    // Build a PM4 Type-3 header. `count` is the payload dword count minus one.
    static uint32_t makeType3Header(uint8_t opcode, uint32_t count,
                                    uint32_t flags = 0);

    // Compute pipeline configuration
    void setComputeProgram(uint64_t shaderGpuAddr, uint32_t rsrc1, uint32_t rsrc2);
    void setComputeWorkgroupSize(uint32_t x, uint32_t y, uint32_t z);
    void setComputeUserData(uint32_t index, uint64_t gpuAddress);

    // Dispatch commands
    void emitDispatchDirect(uint32_t gridX, uint32_t gridY, uint32_t gridZ);

    // Synchronization
    void emitCsPartialFlush();
    void emitFenceWrite(uint64_t fenceGpuAddr, uint64_t fenceValue);
    // WRITE_DATA (ME engine, memory destination, write-confirm): the smallest
    // packet that proves a queue is draining without involving any shader.
    void emitWriteData(uint64_t dstGpuAddr, uint32_t value);
    // INDIRECT_BUFFER (compute form): the dispatch stream lives in its own
    // VRAM buffer; the VMID the waves run under comes from this packet.
    void emitIndirectBuffer(uint64_t ibGpuAddr, uint32_t lengthDw, uint32_t vmid);

    // Get built packets
    const uint32_t* getPackets() const { return packets; }
    size_t getPacketCount() const { return packetCount; }

private:
    static const size_t MAX_PACKETS = 512;  // 2KB buffer
    uint32_t packets[MAX_PACKETS];
    size_t packetCount;

    // Helper: append dword to packet buffer
    void append(uint32_t dword);
};

#endif /* PM4BuilderKernel_hpp */
