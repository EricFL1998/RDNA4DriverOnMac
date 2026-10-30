//
//  MesApi.hpp
//  RDNA4Driver - MES (Micro Engine Scheduler) API frames
//
//  Dword positions follow mes_v12_api_def.h under `#pragma pack(push, 8)`:
//  every uint64_t is 8-byte aligned, so `doorbell_offset` (dw18) is followed
//  by padding before `mqd_addr` (dw20), and the packed flags word sits at
//  dw37 right in front of MES_API_STATUS (dw38..41).
//
//  Header-only and IOKit-free so the layout can be checked on the host
//  (tests/test_mes_api.cpp).
//

#ifndef MesApi_hpp
#define MesApi_hpp

#include <stdint.h>

namespace MesApi {

constexpr uint32_t kFrameDwords = 64;

constexpr uint32_t kApiTypeScheduler = 1;

enum Opcode : uint32_t {
    kOpSetHwResources = 0,
    kOpAddQueue = 2,
    kOpRemoveQueue = 3,
    kOpQuerySchedulerStatus = 11,
    kOpSetHwResources1 = 19,
};

enum QueueType : uint32_t {
    kQueueTypeGfx = 0,
    kQueueTypeCompute = 1,
    kQueueTypeSdma = 2,
};

// union MESAPI__ADD_QUEUE
namespace AddQueue {
constexpr uint32_t kDoorbellOffset = 18;
constexpr uint32_t kMqdAddrLo = 20;
constexpr uint32_t kMqdAddrHi = 21;
constexpr uint32_t kWptrAddrLo = 22;
constexpr uint32_t kWptrAddrHi = 23;
constexpr uint32_t kQueueType = 28;
constexpr uint32_t kVmContextCntl = 36;
constexpr uint32_t kFlags = 37;
constexpr uint32_t kFenceAddrLo = 38;
constexpr uint32_t kFenceAddrHi = 39;
constexpr uint32_t kFenceValueLo = 40;
constexpr uint32_t kFenceValueHi = 41;
constexpr uint32_t kPipeId = 50;
constexpr uint32_t kQueueId = 51;

constexpr uint32_t kFlagMapLegacyKq = 1u << 13;
} // namespace AddQueue

// CP_GFX_HQD_* block (gc_12_0_0_offset.h). All of it is BASE_IDX 0.
namespace GfxHqd {
constexpr uint32_t kSegment = 0;
constexpr uint32_t kMqdBaseAddr = 0x1E7E;
constexpr uint32_t kActive = 0x1E80;
constexpr uint32_t kVmid = 0x1E81;
constexpr uint32_t kBase = 0x1E86;
constexpr uint32_t kRptr = 0x1E88;
constexpr uint32_t kDoorbellControl = 0x1E8D;
constexpr uint32_t kCntl = 0x1E8F;
constexpr uint32_t kWptr = 0x1E91;
constexpr uint32_t kMapped = 0x1E94;

// CP_RB0_CNTL and CP_GFX_HQD_CNTL share the same field layout
// (gc_12_0_0_sh_mask.h).  KMD_QUEUE marks the ring as a kernel queue: it is
// what makes the CP fetch it with kernel (VMID 0) privileges.  Upstream sets
// it on the kernel GFX ring; a ring without it is treated as a user queue and
// is never fetched.
constexpr uint32_t kCntlKmdQueue = 1u << 29;
constexpr uint32_t kCntlRbExe = 1u << 28;
constexpr uint32_t kCntlRbNoUpdate = 1u << 27;
constexpr uint32_t kCntlMinAvailSz3 = 3u << 20;
constexpr uint32_t kCntlMinIbAvailSz3 = 3u << 22;   // regCP_*_CNTL_DEFAULT upper bits

// regCP_GFX_HQD_CNTL_DEFAULT / regCP_RB0_CNTL defaults, the kernel flag and
// RB_EXE.  RB_EXE is "ring buffer execute": the CP does not run a ring whose
// CNTL has it clear, which is exactly the state both GFX paths shipped in.
constexpr uint32_t kCntlDefault = 0x00F00000u;

inline uint32_t cntlValue(uint32_t rbBufsz) {
    return kCntlDefault | kCntlRbExe | kCntlKmdQueue | (rbBufsz & 0x3Fu) |
           (((rbBufsz >= 2u ? rbBufsz - 2u : 0u) & 0x3Fu) << 8);
}
} // namespace GfxHqd

// Kernel doorbell map (soc24_init_doorbell_index / amdgpu_doorbell.h, the
// Navi10 layout gfx12 reuses).  The CP owns two windows that must not
// overlap: CP_MEC_DOORBELL_RANGE covers [kiq .. userqueue_end] and
// CP_RB_DOORBELL_RANGE covers [gfx_ring0 .. gfx_userqueue_end].  A doorbell
// inside the MEC window is handled as a compute doorbell whatever queue the
// MES mapped it to.
namespace Doorbell {
constexpr uint32_t kKiq = 0x000;
constexpr uint32_t kMecRing0 = 0x003;
constexpr uint32_t kMesRing0 = 0x00B;
constexpr uint32_t kMesRing1 = 0x00C;
constexpr uint32_t kUserQueueEnd = 0x08A;
constexpr uint32_t kGfxRing0 = 0x08B;
constexpr uint32_t kGfxUserQueueEnd = 0x0FF;

constexpr uint32_t kRegRbRangeLower = 0x1DFA;    // CP_RB_DOORBELL_RANGE_LOWER
constexpr uint32_t kRegRbRangeUpper = 0x1DFB;
constexpr uint32_t kRegMecRangeLower = 0x1DFC;   // CP_MEC_DOORBELL_RANGE_LOWER
constexpr uint32_t kRegMecRangeUpper = 0x1DFD;

// The entries above are 64-bit doorbell slots.  Everything the CP and the
// driver index by hand - CP_RB_DOORBELL_CONTROL.DOORBELL_OFFSET, the BAR2
// dword the driver writes (amdgpu_mm_wdoorbell64) - is the DWORD index, i.e.
// the slot number shifted left by one.
constexpr uint32_t kGfxRingDword = kGfxRing0 << 1;      // 0x116
constexpr uint32_t kMesRing0Dword = kMesRing0 << 1;     // 0x016
constexpr uint32_t kMesRing1Dword = kMesRing1 << 1;     // 0x018
constexpr uint32_t kMecRing0Dword = kMecRing0 << 1;     // 0x006

inline uint32_t byteOffset(uint32_t dwordIndex) { return dwordIndex * 4u; }

// gfx_v12_0_cp_set_doorbell_range(): the range registers take the byte
// offset of a 64-bit doorbell, (index * 2) << 2.
inline uint32_t rangeValue(uint32_t index) { return (index * 2u) << 2; }
} // namespace Doorbell

inline uint32_t header(uint32_t opcode, uint32_t dwords = kFrameDwords) {
    return (kApiTypeScheduler & 0xFu) | ((opcode & 0xFFu) << 4) | ((dwords & 0xFFu) << 12);
}

inline bool completionFailed(uint64_t status) {
    return (status & (1ull << 31)) != 0;
}

inline bool completionSucceeded(uint64_t status, uint32_t expectedFence) {
    return !completionFailed(status) && static_cast<uint32_t>(status) == expectedFence;
}

inline uint32_t nextCompletionFence(uint32_t current) {
    const uint32_t next = (current + 1u) & 0x7fffffffu;
    return next ? next : 1;
}

inline uint32_t completionStatusDword(uint32_t opcode) {
    switch (opcode) {
        case kOpSetHwResources: return 50;
        case kOpAddQueue: return 38;
        case kOpQuerySchedulerStatus:
        case kOpSetHwResources1: return 2;
        default: return 0;
    }
}

struct AddQueueInput {
    uint32_t doorbellOffset;
    uint64_t mqdAddr;
    uint64_t wptrAddr;
    uint32_t queueType;
    uint32_t pipe;
    uint32_t queue;
    uint64_t fenceAddr;
    uint64_t fenceValue;
};

// mes_v12_0_map_legacy_queue(): a kernel queue the driver programs itself.
// Nothing beyond these fields is set; process/gang context stays zero.
inline void buildAddQueueLegacy(uint32_t* pkt, const AddQueueInput& in) {
    for (uint32_t i = 0; i < kFrameDwords; i++) pkt[i] = 0;
    pkt[0] = header(kOpAddQueue);
    pkt[AddQueue::kDoorbellOffset] = in.doorbellOffset;
    pkt[AddQueue::kMqdAddrLo] = static_cast<uint32_t>(in.mqdAddr);
    pkt[AddQueue::kMqdAddrHi] = static_cast<uint32_t>(in.mqdAddr >> 32);
    pkt[AddQueue::kWptrAddrLo] = static_cast<uint32_t>(in.wptrAddr);
    pkt[AddQueue::kWptrAddrHi] = static_cast<uint32_t>(in.wptrAddr >> 32);
    pkt[AddQueue::kQueueType] = in.queueType;
    pkt[AddQueue::kFlags] = AddQueue::kFlagMapLegacyKq;
    pkt[AddQueue::kFenceAddrLo] = static_cast<uint32_t>(in.fenceAddr);
    pkt[AddQueue::kFenceAddrHi] = static_cast<uint32_t>(in.fenceAddr >> 32);
    pkt[AddQueue::kFenceValueLo] = static_cast<uint32_t>(in.fenceValue);
    pkt[AddQueue::kFenceValueHi] = static_cast<uint32_t>(in.fenceValue >> 32);
    pkt[AddQueue::kPipeId] = in.pipe;
    pkt[AddQueue::kQueueId] = in.queue;
}

} // namespace MesApi

#endif /* MesApi_hpp */
