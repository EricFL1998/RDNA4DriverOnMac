#include "../core/RDNA4Driver.hpp"
#include "VramLayout.hpp"
#include "GMCContext.hpp"
#include <IOKit/IOLib.h>

//==============================================================================
// Phase 2.4: CP/MEC compute queue
//
// Ports the kernel-queue bring-up path from Linux gfx_v12_0.c:
//   gfx_v12_0_compute_mqd_init()  - fills the MQD page
//   gfx_v12_0_kiq_init_register() - programs HQD registers and activates
//   gfx_v12_0_cp_compute_enable() - un-halts the MEC pipes
//
// The queue runs in VMID 0 and uses VRAM physical addresses, matching the
// rest of this driver (no GART/VM translation yet).
//==============================================================================

namespace {

// GC register dword offsets (gc_12_0_0_offset.h)
constexpr uint32_t kRegCP_MEC_RS64_CNTL            = 0x2904;
constexpr uint32_t kRegCP_ME_CNTL                  = 0x0803;
constexpr uint32_t kRegCP_MAX_CONTEXT              = 0x1E4E;
constexpr uint32_t kRegCP_DEVICE_ID                = 0x1DEB;
constexpr uint32_t kRegCP_MQD_BASE_ADDR            = 0x1FA9;
constexpr uint32_t kRegCP_MQD_BASE_ADDR_HI         = 0x1FAA;
constexpr uint32_t kRegCP_MQD_CONTROL              = 0x1FCB;
constexpr uint32_t kRegCP_HQD_ACTIVE               = 0x1FAB;
constexpr uint32_t kRegCP_HQD_VMID                 = 0x1FAC;
constexpr uint32_t kRegCP_HQD_PERSISTENT_STATE     = 0x1FAD;
constexpr uint32_t kRegCP_HQD_PQ_BASE              = 0x1FB1;
constexpr uint32_t kRegCP_HQD_PQ_BASE_HI           = 0x1FB2;
constexpr uint32_t kRegCP_HQD_PQ_RPTR              = 0x1FB3;
constexpr uint32_t kRegCP_HQD_PQ_RPTR_REPORT_ADDR  = 0x1FB4;
constexpr uint32_t kRegCP_HQD_PQ_RPTR_REPORT_ADDR_HI = 0x1FB5;
constexpr uint32_t kRegCP_HQD_PQ_WPTR_POLL_ADDR    = 0x1FB6;
constexpr uint32_t kRegCP_HQD_PQ_WPTR_POLL_ADDR_HI = 0x1FB7;
constexpr uint32_t kRegCP_HQD_PQ_DOORBELL_CONTROL  = 0x1FB8;
constexpr uint32_t kRegCP_HQD_PQ_CONTROL           = 0x1FBA;
constexpr uint32_t kRegCP_HQD_PQ_WPTR_LO           = 0x1FDF;
constexpr uint32_t kRegCP_HQD_PQ_WPTR_HI           = 0x1FE0;
constexpr uint32_t kRegCP_HQD_IB_CONTROL           = 0x1FBE;
constexpr uint32_t kRegCP_HQD_DEQUEUE_REQUEST      = 0x1FC6;
constexpr uint32_t kRegCP_HQD_EOP_BASE_ADDR        = 0x1FCE;
constexpr uint32_t kRegCP_HQD_EOP_BASE_ADDR_HI     = 0x1FCF;
constexpr uint32_t kRegCP_HQD_EOP_CONTROL          = 0x1FD0;
constexpr uint32_t kRegCP_HQD_EOP_RPTR             = 0x1FD1;
constexpr uint32_t kRegCP_MEC_DOORBELL_RANGE_LOWER = 0x1DFC;
constexpr uint32_t kRegCP_MEC_DOORBELL_RANGE_UPPER = 0x1DFD;
constexpr uint32_t kRegCP_PQ_WPTR_POLL_CNTL        = 0x1E23;
constexpr uint32_t kRegCP_PQ_STATUS                = 0x1E58;
constexpr uint32_t kRegCP_STAT                     = 0x0F40;
constexpr uint32_t kRegGRBM_GFX_CNTL               = 0x0900;
constexpr uint32_t kRegRLC_CP_SCHEDULERS           = 0x098A;

// Graphics ring (CP_RB0) registers, all GC segment 0.
constexpr uint32_t kRegCP_RB0_CNTL                 = 0x1DE1;
constexpr uint32_t kRegCP_RB0_BASE                 = 0x1DE0;
constexpr uint32_t kRegCP_RB0_BASE_HI              = 0x1E51;
constexpr uint32_t kRegCP_RB0_WPTR                 = 0x1DF4;
constexpr uint32_t kRegCP_RB0_WPTR_HI              = 0x1DF5;
constexpr uint32_t kRegCP_RB0_RPTR_ADDR            = 0x1DE3;
constexpr uint32_t kRegCP_RB0_RPTR_ADDR_HI         = 0x1DE4;
constexpr uint32_t kRegCP_RB_WPTR_DELAY            = 0x0F61;
constexpr uint32_t kRegCP_RB_VMID                  = 0x1DF1;
constexpr uint32_t kRegCP_RB_ACTIVE                = 0x1F40;
constexpr uint32_t kRegCP_RB_DOORBELL_CONTROL      = 0x1E8D;
constexpr uint32_t kRegCP_RB_DOORBELL_RANGE_LOWER  = 0x1DFA;
constexpr uint32_t kRegCP_RB_DOORBELL_RANGE_UPPER  = 0x1DFB;
constexpr uint32_t kRegCP_RB_WPTR_POLL_ADDR_LO     = 0x1E8B;
constexpr uint32_t kRegCP_RB_WPTR_POLL_ADDR_HI     = 0x1E8C;

// v12_compute_mqd dword offsets (include/v12_structs.h)
namespace MqdOff {
constexpr uint32_t header                        = 0;
constexpr uint32_t compute_pipelinestat_enable   = 11;
constexpr uint32_t compute_static_thread_mgmt_se0 = 23;
constexpr uint32_t compute_static_thread_mgmt_se1 = 24;
constexpr uint32_t compute_static_thread_mgmt_se2 = 26;
constexpr uint32_t compute_static_thread_mgmt_se3 = 27;
constexpr uint32_t compute_misc_reserved         = 32;
constexpr uint32_t cp_mqd_base_addr_lo           = 128;
constexpr uint32_t cp_mqd_base_addr_hi           = 129;
constexpr uint32_t cp_hqd_active                 = 130;
constexpr uint32_t cp_hqd_vmid                   = 131;
constexpr uint32_t cp_hqd_persistent_state       = 132;
constexpr uint32_t cp_hqd_pipe_priority          = 133;
constexpr uint32_t cp_hqd_queue_priority         = 134;
constexpr uint32_t cp_hqd_quantum                = 135;
constexpr uint32_t cp_hqd_pq_base_lo             = 136;
constexpr uint32_t cp_hqd_pq_base_hi             = 137;
constexpr uint32_t cp_hqd_pq_rptr                = 138;
constexpr uint32_t cp_hqd_pq_rptr_report_addr_lo = 139;
constexpr uint32_t cp_hqd_pq_rptr_report_addr_hi = 140;
constexpr uint32_t cp_hqd_pq_wptr_poll_addr_lo   = 141;
constexpr uint32_t cp_hqd_pq_wptr_poll_addr_hi   = 142;
constexpr uint32_t cp_hqd_pq_doorbell_control    = 143;
constexpr uint32_t cp_hqd_pq_control             = 145;
constexpr uint32_t cp_hqd_ib_control             = 149;
constexpr uint32_t cp_hqd_dequeue_request        = 152;
constexpr uint32_t cp_mqd_control                = 162;
constexpr uint32_t cp_hqd_eop_base_addr_lo       = 165;
constexpr uint32_t cp_hqd_eop_base_addr_hi       = 166;
constexpr uint32_t cp_hqd_eop_control            = 167;
constexpr uint32_t cp_hqd_pq_wptr_lo             = 182;
constexpr uint32_t cp_hqd_pq_wptr_hi             = 183;
constexpr uint32_t fence_address_lo              = 446;
constexpr uint32_t fence_address_hi              = 447;
constexpr uint32_t kDwords                       = 512;   // 2 KiB MQD page
} // namespace MqdOff

constexpr uint32_t kMqdBytes       = MqdOff::kDwords * 4;
constexpr uint32_t kMecHpdBytes    = 2048;   // GFX12_MEC_HPD_SIZE
constexpr uint32_t kMecPipeCount   = 2;  // stacks P0/P1 only (types 94/95)

// Register defaults (gc_11_0_0_default.h, gfx12 uses the same values)
constexpr uint32_t kDefaultPqControl       = 0x00308509;
constexpr uint32_t kDefaultEopControl      = 0x00000006;
constexpr uint32_t kDefaultMqdControl      = 0x00000100;
constexpr uint32_t kDefaultPersistentState = 0x0BE05501;
constexpr uint32_t kDefaultIbControl       = 0x00300000;

uint32_t orderBase2(uint32_t x) {
    uint32_t r = 0;
    while (r < 31 && (1u << r) < x) r++;
    return r;
}

void setMqd(uint32_t* mqd, uint32_t index, uint32_t value) {
    if (index < MqdOff::kDwords) mqd[index] = value;
}

uint32_t readMqd(const uint32_t* mqd, uint32_t index) {
    return index < MqdOff::kDwords ? mqd[index] : 0;
}

} // namespace

uint32_t RDNA4Driver::readGcRegister(uint32_t regDwordOffset, uint8_t segment) {
    uint32_t byteOffset = 0;
    if (!ipDiscovery.regByteOffset(IPDiscovery::HwGc, 0, segment,
                                   regDwordOffset, byteOffset)) {
        return 0xFFFFFFFF;
    }
    return regRead32(byteOffset);
}

bool RDNA4Driver::writeGcRegister(uint32_t regDwordOffset, uint32_t value,
                                  uint8_t segment) {
    uint32_t byteOffset = 0;
    if (!ipDiscovery.regByteOffset(IPDiscovery::HwGc, 0, segment,
                                   regDwordOffset, byteOffset)) {
        return false;
    }
    regWrite32(byteOffset, value);
    return true;
}

void RDNA4Driver::grbmSelect(uint32_t me, uint32_t pipe, uint32_t queue,
                             uint32_t vmid) {
    // GRBM_GFX_CNTL: PIPEID [1:0], MEID [3:2], VMID [7:4], QUEUEID [10:8].
    // BASE_IDX for this register is 1, not 0.
    const uint32_t value = ((pipe & 0x3u) << 0) |
                           ((me & 0x3u) << 2) |
                           ((vmid & 0xFu) << 4) |
                           ((queue & 0x7u) << 8);
    writeGcRegister(kRegGRBM_GFX_CNTL, value, 1);
}

bool RDNA4Driver::initComputeQueue(uint64_t ringGpuAddr, uint32_t ringSizeBytes,
                                   uint64_t mqdGpuAddr, uint64_t eopGpuAddr,
                                   uint64_t rptrGpuAddr, uint32_t doorbellIndex) {
    LOG("initComputeQueue: ring=0x%llx size=%u mqd=0x%llx eop=0x%llx rptr=0x%llx db=%u",
        static_cast<unsigned long long>(ringGpuAddr), ringSizeBytes,
        static_cast<unsigned long long>(mqdGpuAddr),
        static_cast<unsigned long long>(eopGpuAddr),
        static_cast<unsigned long long>(rptrGpuAddr), doorbellIndex);

    if (!vram || vramSize == 0) {
        LOG_ERROR("initComputeQueue: VRAM not mapped");
        return false;
    }
    if (ringSizeBytes < 0x1000 || (ringSizeBytes & (ringSizeBytes - 1)) != 0) {
        LOG_ERROR("initComputeQueue: ring size %u is not a power of two", ringSizeBytes);
        return false;
    }
    if ((ringGpuAddr & 0xFFu) != 0) {
        LOG_ERROR("initComputeQueue: ring address 0x%llx is not 256-byte aligned",
                  static_cast<unsigned long long>(ringGpuAddr));
        return false;
    }
    if (ringGpuAddr < gmcContext.vramStart ||
        (!computeMqdMd && mqdGpuAddr < gmcContext.vramStart) ||
        eopGpuAddr < gmcContext.vramStart ||
        rptrGpuAddr < gmcContext.vramStart) {
        LOG_ERROR("initComputeQueue: address below VRAM base 0x%llx",
                  static_cast<unsigned long long>(gmcContext.vramStart));
        return false;
    }

    const uint64_t mqdOffset = computeMqdMd ? 0 : (mqdGpuAddr - gmcContext.vramStart);
    const uint64_t eopOffset = eopGpuAddr - gmcContext.vramStart;
    const uint64_t rptrOffset = rptrGpuAddr - gmcContext.vramStart;
    if ((!computeMqdMd && mqdOffset + kMqdBytes > vramSize) ||
        eopOffset + kMecHpdBytes > vramSize ||
        rptrOffset + 16 > vramSize) {
        LOG_ERROR("initComputeQueue: MQD/EOP/rptr outside VRAM");
        return false;
    }

    // MES reads the MQD through the GART aperture, so the MQD can live in a
    // system-memory buffer instead of VRAM.
    uint32_t* mqd = computeMqdMd
        ? static_cast<uint32_t*>(computeMqdMd->getBytesNoCopy())
        : reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(vram) + mqdOffset);
    uint8_t* eop = const_cast<uint8_t*>(vram) + eopOffset;
    volatile uint32_t* rptrReport = reinterpret_cast<volatile uint32_t*>(
        const_cast<uint8_t*>(vram) + rptrOffset);

    // Zero the MQD page and the EOP/writeback buffers.
    for (uint32_t i = 0; i < MqdOff::kDwords; i++) mqd[i] = 0;
    for (uint32_t i = 0; i < kMecHpdBytes; i++) eop[i] = 0;
    *rptrReport = 0;

    // --- MQD contents (gfx_v12_0_compute_mqd_init) ---
    setMqd(mqd, MqdOff::header, 0xC0310800u);
    setMqd(mqd, MqdOff::compute_pipelinestat_enable, 0x00000001u);
    setMqd(mqd, MqdOff::compute_static_thread_mgmt_se0, 0xFFFFFFFFu);
    setMqd(mqd, MqdOff::compute_static_thread_mgmt_se1, 0xFFFFFFFFu);
    setMqd(mqd, MqdOff::compute_static_thread_mgmt_se2, 0xFFFFFFFFu);
    setMqd(mqd, MqdOff::compute_static_thread_mgmt_se3, 0xFFFFFFFFu);
    setMqd(mqd, MqdOff::compute_misc_reserved, 0x00000007u);

    // EOP base is stored as address >> 8.
    const uint64_t eopBase = eopGpuAddr >> 8;
    setMqd(mqd, MqdOff::cp_hqd_eop_base_addr_lo, static_cast<uint32_t>(eopBase));
    setMqd(mqd, MqdOff::cp_hqd_eop_base_addr_hi, static_cast<uint32_t>(eopBase >> 32));
    const uint32_t eopSizeLog = orderBase2(kMecHpdBytes / 4) - 1u;
    setMqd(mqd, MqdOff::cp_hqd_eop_control,
           (kDefaultEopControl & ~0x1Fu) | (eopSizeLog & 0x1Fu));

    // Doorbell control: offset field is in dwords, DOORBELL_EN is bit 30.
    setMqd(mqd, MqdOff::cp_hqd_pq_doorbell_control,
           ((doorbellIndex & 0x3FFFu) << 2) | (1u << 30));

    setMqd(mqd, MqdOff::cp_hqd_dequeue_request, 0);
    setMqd(mqd, MqdOff::cp_hqd_pq_rptr, 0);
    setMqd(mqd, MqdOff::cp_hqd_pq_wptr_lo, 0);
    setMqd(mqd, MqdOff::cp_hqd_pq_wptr_hi, 0);

    setMqd(mqd, MqdOff::cp_mqd_base_addr_lo,
           static_cast<uint32_t>(mqdGpuAddr & 0xFFFFFFFCu));
    setMqd(mqd, MqdOff::cp_mqd_base_addr_hi, static_cast<uint32_t>(mqdGpuAddr >> 32));

    // CP_MQD_CONTROL: PRIV_STATE (bit 8), VMID = 0.
    setMqd(mqd, MqdOff::cp_mqd_control, kDefaultMqdControl);

    // PQ base is stored as address >> 8, same encoding as CP_RB0_BASE.
    const uint64_t pqBase = ringGpuAddr >> 8;
    setMqd(mqd, MqdOff::cp_hqd_pq_base_lo, static_cast<uint32_t>(pqBase));
    setMqd(mqd, MqdOff::cp_hqd_pq_base_hi, static_cast<uint32_t>(pqBase >> 32));

    // CP_HQD_PQ_CONTROL: QUEUE_SIZE [4:0], RPTR_BLOCK_SIZE [13:8],
    // UNORD_DISPATCH bit 28, PRIV_STATE bit 30, KMD_QUEUE bit 31.
    const uint32_t queueSizeLog = orderBase2(ringSizeBytes / 4) - 1u;
    const uint32_t rptrBlockLog = orderBase2(4096u / 4) - 1u;
    uint32_t pqControl = kDefaultPqControl;
    pqControl = (pqControl & ~0x1Fu) | (queueSizeLog & 0x1Fu);
    pqControl = (pqControl & ~0x3F00u) | ((rptrBlockLog & 0x3Fu) << 8);
    pqControl |= (1u << 28);
    pqControl |= (1u << 30) | (1u << 31);
    setMqd(mqd, MqdOff::cp_hqd_pq_control, pqControl);

    // Read/write pointer writeback addresses.
    setMqd(mqd, MqdOff::cp_hqd_pq_rptr_report_addr_lo,
           static_cast<uint32_t>(rptrGpuAddr & 0xFFFFFFFCu));
    setMqd(mqd, MqdOff::cp_hqd_pq_rptr_report_addr_hi,
           static_cast<uint32_t>(rptrGpuAddr >> 32) & 0xFFFFu);
    // wptr poll address must be the page MES polls (ADD_QUEUE wptr_addr), not
    // the rptr report page — the two are different hardware paths.
    const uint64_t wptrPoll = computeWptrPollGpuAddr ? computeWptrPollGpuAddr
                                                    : rptrGpuAddr;
    setMqd(mqd, MqdOff::cp_hqd_pq_wptr_poll_addr_lo,
           static_cast<uint32_t>(wptrPoll & 0xFFFFFFFCu));
    setMqd(mqd, MqdOff::cp_hqd_pq_wptr_poll_addr_hi,
           static_cast<uint32_t>(wptrPoll >> 32) & 0xFFFFu);

    setMqd(mqd, MqdOff::cp_hqd_vmid, 0);
    setMqd(mqd, MqdOff::cp_hqd_persistent_state, kDefaultPersistentState);
    setMqd(mqd, MqdOff::cp_hqd_ib_control, kDefaultIbControl);
    setMqd(mqd, MqdOff::cp_hqd_pipe_priority, 0);
    setMqd(mqd, MqdOff::cp_hqd_queue_priority, 0);
    // QUANTUM_EN bit 0, QUANTUM_SCALE bit 4, QUANTUM_DURATION bit 8.
    setMqd(mqd, MqdOff::cp_hqd_quantum, (1u << 0) | (1u << 4) | (1u << 8));
    // A queue mapped by MES must not claim to be active in its MQD: the map
    // packet is what brings the HQD up (amdgpu_ring_to_mqd_prop only sets
    // hqd_active for the KIQ). Saying 1 here makes MES see an inconsistent
    // queue and it never acks ADD_QUEUE.
    setMqd(mqd, MqdOff::cp_hqd_active, computeQueueViaMes ? 0u : 1u);

    // Fence address used by CP for the queue's own fence writes.
    uint64_t fenceGpuAddr = gmcContext.vramStart + VramLayout::kComputeFence;
    setMqd(mqd, MqdOff::fence_address_lo, static_cast<uint32_t>(fenceGpuAddr));
    setMqd(mqd, MqdOff::fence_address_hi, static_cast<uint32_t>(fenceGpuAddr >> 32));

    computeMqdGpuAddr = mqdGpuAddr;

    // PM4 SH-register indices in PM4BuilderKernel assume GC segment 0 sits at
    // 0x1260 dwords. Derive the real base from IP discovery and log it so a
    // mismatch is visible instead of silently writing the wrong registers.
    uint32_t gcBaseBytes = 0;
    if (ipDiscovery.regByteOffset(IPDiscovery::HwGc, 0, 0, 0, gcBaseBytes)) {
        LOG("initComputeQueue: GC base = 0x%x dwords (PM4 assumes 0x1260)",
            gcBaseBytes / 4);
    }

    // The MQD lives in VRAM; make the CPU writes visible before CP fetches it.
    gmcHDPFlush(*this);
    OSSynchronizeIO();

    LOG("initComputeQueue: MQD header=0x%08x pqControl=0x%08x queueSizeLog=%u",
        readMqd(mqd, MqdOff::header), readMqd(mqd, MqdOff::cp_hqd_pq_control),
        queueSizeLog);
    LOG("initComputeQueue: eopCtrl=0x%08x doorbellCtrl=0x%08x",
        readMqd(mqd, MqdOff::cp_hqd_eop_control),
        readMqd(mqd, MqdOff::cp_hqd_pq_doorbell_control));

    setProperty("ComputeQueue,RingGPU", ringGpuAddr, 64);
    setProperty("ComputeQueue,RingSize", static_cast<uint64_t>(ringSizeBytes), 32);
    setProperty("ComputeQueue,MqdGPU", mqdGpuAddr, 64);
    setProperty("ComputeQueue,EopGPU", eopGpuAddr, 64);
    setProperty("ComputeQueue,RptrGPU", rptrGpuAddr, 64);
    setProperty("ComputeQueue,DoorbellIndex", static_cast<uint64_t>(doorbellIndex), 32);
    return true;
}

bool RDNA4Driver::initShaderMemoryConfig() {
    // gfx_v12_0_constants_init(): every VMID gets SH_MEM_CONFIG =
    // ADDRESS_MODE_64 | ALIGNMENT_MODE_UNALIGNED | INITIAL_INST_PREFETCH 3
    // (0xC00C), and VMIDs >= 1 get the LDS/scratch aperture bases.  This is
    // what tells the shader cores how to form addresses; a wave launched
    // without it cannot execute global loads/stores and leaves every output
    // untouched while the CP still retires the dispatch and the fence.
    constexpr uint32_t kRegShMemBases = 0x09E3;
    constexpr uint32_t kRegShMemConfig = 0x09E4;
    constexpr uint32_t kDefaultShMemConfig = 0xC00Cu;
    constexpr uint32_t kShMemBasesKfd = (0x1u << 16) | 0x2u;  // SHARED=LDS(1), PRIVATE=SCRATCH(2)
    constexpr uint32_t kRegGrbmCntl = 0x0DA0;

    // GRBM_CNTL.READ_TIMEOUT = 0xFF (gfx_v12_0.c:1812).
    {
        const uint32_t v = readGcRegister(kRegGrbmCntl, 0);
        if (v != 0xFFFFFFFF) {
            writeGcRegister(kRegGrbmCntl, (v & ~0xFFFu) | 0xFFu, 0);
        }
    }

    for (uint32_t vmid = 0; vmid < 16; vmid++) {
        grbmSelect(0, 0, 0, vmid);
        writeGcRegister(kRegShMemConfig, kDefaultShMemConfig, 1);
        if (vmid != 0) {
            writeGcRegister(kRegShMemBases, kShMemBasesKfd, 1);
        }
    }
    grbmSelect(0, 0, 0, 0);

    grbmSelect(0, 0, 0, 0);
    const uint32_t rb = readGcRegister(kRegShMemConfig, 1);
    grbmSelect(0, 0, 0, 0);
    LOG("SH_MEM_CONFIG programmed for VMIDs 0..15 (readback vmid0 = 0x%08x)", rb);
    setProperty("GFX,ShMemConfig", static_cast<uint64_t>(rb), 32);
    return rb == kDefaultShMemConfig;
}

bool RDNA4Driver::initComputeVmid() {
    // gfx_v12_0_init_compute_vmid(): the KFD VMIDs (8..15) additionally get
    // SPI_GDBG_PER_VMID_CNTL.TRAP_EN so a faulting wave traps instead of
    // hanging the SPI.  DB_RING_CONTROL.COUNTER_CONTROL=1 comes from
    // constants_init (one gfx pipe -> all occlusion counters to pipe 0).
    constexpr uint32_t kRegShMemBases = 0x09E3;
    constexpr uint32_t kRegShMemConfig = 0x09E4;
    constexpr uint32_t kRegSpiGdbgPerVmidCntl = 0x1F72;  // BASE_IDX 0
    constexpr uint32_t kRegDbRingControl = 0x13BB;       // BASE_IDX 0
    constexpr uint32_t kDefaultShMemConfig = 0xC00Cu;
    constexpr uint32_t kShMemBasesKfd = (0x1u << 16) | 0x2u;
    constexpr uint32_t kTrapEn = 0x8u;

    uint32_t trapRb = 0;
    for (uint32_t vmid = 8; vmid < 16; vmid++) {
        grbmSelect(0, 0, 0, vmid);
        writeGcRegister(kRegShMemConfig, kDefaultShMemConfig, 1);
        writeGcRegister(kRegShMemBases, kShMemBasesKfd, 1);
        uint32_t v = readGcRegister(kRegSpiGdbgPerVmidCntl, 0);
        if (v == 0xFFFFFFFF) {
            v = 0;
        }
        writeGcRegister(kRegSpiGdbgPerVmidCntl, v | kTrapEn, 0);
        trapRb = readGcRegister(kRegSpiGdbgPerVmidCntl, 0);
    }
    grbmSelect(0, 0, 0, 0);

    const uint32_t dbBefore = readGcRegister(kRegDbRingControl, 0);
    if (dbBefore != 0xFFFFFFFF) {
        writeGcRegister(kRegDbRingControl, (dbBefore & ~0x3u) | 0x1u, 0);
    }
    const uint32_t dbAfter = readGcRegister(kRegDbRingControl, 0);
    LOG("compute VMIDs 8..15 configured (SPI_GDBG_PER_VMID_CNTL vmid15=0x%08x), "
        "DB_RING_CONTROL 0x%08x -> 0x%08x", trapRb, dbBefore, dbAfter);
    setProperty("GFX,SpiGdbgVmid15", static_cast<uint64_t>(trapRb), 32);
    setProperty("GFX,DbRingControl", static_cast<uint64_t>(dbAfter), 32);
    return (trapRb & kTrapEn) != 0;
}

bool RDNA4Driver::initClearStateBuffer() {
    // gfx_v12_0_rlc_resume() on the PSP path == gfx_v12_0_init_csb() +
    // gfx_v12_0_rlc_enable_srm().  The clear-state buffer is what the RLC
    // uses to initialise context registers when a queue is scheduled; its
    // layout (gfx_v12_0_get_csb_buffer, clearstate_gfx12.h) is
    //   [0]            clustercount
    //   per extent:    reg_count, reg_index(PACKET3_SET_CONTEXT_REG space),
    //                  reg_count x initial value (all zero on gfx12)
    struct Extent { uint32_t regIndex; uint32_t regCount; };
    static const Extent kExtents[] = {
        { 0x0000a03e, 34 },
        { 0x0000a0cc,  2 },
        { 0x0000a0d8,  1 },
        { 0x0000a0db,  6 },
        { 0x0000a2e5, 11 },
        { 0x0000a3c0,  8 },
    };
    constexpr uint32_t kRegRlcCsibAddrLo = 0x0987;  // BASE_IDX 1
    constexpr uint32_t kRegRlcCsibAddrHi = 0x0988;  // BASE_IDX 1
    constexpr uint32_t kRegRlcCsibLength = 0x0989;  // BASE_IDX 1
    constexpr uint32_t kRegRlcSrmCntl    = 0x4C80;  // BASE_IDX 1
    constexpr uint32_t kSrmEnable        = 0x1u;
    constexpr uint32_t kSrmAutoIncrAddr  = 0x2u;

    uint32_t dwords = 1;
    for (const Extent& e : kExtents) {
        dwords += 2 + e.regCount;
    }

    if (clearStateGpuAddr == 0) {
        clearStateGpuAddr = allocateVRAMBuffer(dwords * 4u);
        if (clearStateGpuAddr == 0) {
            LOG_ERROR("initClearStateBuffer: VRAM allocation failed");
            return false;
        }
    }
    if (!vram || clearStateGpuAddr < gmcContext.vramStart ||
        clearStateGpuAddr - gmcContext.vramStart + dwords * 4u > vramSize) {
        LOG_ERROR("initClearStateBuffer: CSB 0x%llx outside the BAR0 aperture",
                  static_cast<unsigned long long>(clearStateGpuAddr));
        return false;
    }
    volatile uint32_t* csb = reinterpret_cast<volatile uint32_t*>(
        const_cast<uint8_t*>(vram) + (clearStateGpuAddr - gmcContext.vramStart));

    uint32_t count = 1;
    for (const Extent& e : kExtents) {
        csb[count++] = e.regCount;
        csb[count++] = e.regIndex;
        for (uint32_t i = 0; i < e.regCount; i++) {
            csb[count++] = 0;
        }
    }
    csb[0] = static_cast<uint32_t>(sizeof(kExtents) / sizeof(kExtents[0]));
    gmcHDPFlush(*this);

    const uint64_t off = clearStateGpuAddr - gmcContext.vramStart;
    LOG("CSB at GPU 0x%llx: [0]=%u [1]=%u [2]=0x%x (want 6/34/0xa03e), %u dwords",
        static_cast<unsigned long long>(clearStateGpuAddr),
        vramRead32(off), vramRead32(off + 4), vramRead32(off + 8), dwords);

    writeGcRegister(kRegRlcCsibAddrHi, static_cast<uint32_t>(clearStateGpuAddr >> 32), 1);
    writeGcRegister(kRegRlcCsibAddrLo, static_cast<uint32_t>(clearStateGpuAddr & 0xFFFFFFFCu), 1);
    writeGcRegister(kRegRlcCsibLength, dwords, 1);
    const uint32_t hiRb = readGcRegister(kRegRlcCsibAddrHi, 1);
    const uint32_t loRb = readGcRegister(kRegRlcCsibAddrLo, 1);
    const uint32_t lenRb = readGcRegister(kRegRlcCsibLength, 1);

    const uint32_t srmBefore = readGcRegister(kRegRlcSrmCntl, 1);
    uint32_t srmAfter = srmBefore;
    if (srmBefore != 0xFFFFFFFF) {
        writeGcRegister(kRegRlcSrmCntl, srmBefore | kSrmEnable | kSrmAutoIncrAddr, 1);
        srmAfter = readGcRegister(kRegRlcSrmCntl, 1);
    }

    LOG("RLC_CSIB ADDR=0x%08x%08x LENGTH=%u, RLC_SRM_CNTL 0x%08x -> 0x%08x",
        hiRb, loRb, lenRb, srmBefore, srmAfter);
    setProperty("RLC,CsibAddr", (static_cast<uint64_t>(hiRb) << 32) | loRb, 64);
    setProperty("RLC,CsibLength", static_cast<uint64_t>(lenRb), 32);
    setProperty("RLC,SrmCntl", static_cast<uint64_t>(srmAfter), 32);
    return lenRb == dwords && (srmAfter & kSrmEnable) != 0;
}

bool RDNA4Driver::enableMecRs64Pipes() {
    // --- Un-halt MEC pipes (gfx_v12_0_cp_compute_enable) ---
    // CP_MEC_RS64_CNTL has BASE_IDX 1: it lives in GC segment 1, not segment 0.
    // Linux does this in cp_resume() for every ASIC, MES or not: a queue MES
    // maps into the MEC block cannot come up while its RS64 pipe is in reset.
    uint32_t mec = readGcRegister(kRegCP_MEC_RS64_CNTL, 1);
    if (mec == 0xFFFFFFFF) {
        LOG_ERROR("enableMecRs64Pipes: CP_MEC_RS64_CNTL not readable");
        return false;
    }
    LOG("MEC: CP_MEC_RS64_CNTL before: 0x%08x", mec);
    setProperty("Mec,Before", static_cast<uint64_t>(mec), 32);
    mec &= ~((0xFu << 16) | (1u << 4) | (1u << 30));
    mec |= (((1u << kMecPipeCount) - 1u) << 26);
    writeGcRegister(kRegCP_MEC_RS64_CNTL, mec, 1);
    IODelay(50);
    const uint32_t mecAfter = readGcRegister(kRegCP_MEC_RS64_CNTL, 1);
    LOG("MEC: CP_MEC_RS64_CNTL after:  0x%08x", mecAfter);
    setProperty("Mec,After", static_cast<uint64_t>(mecAfter), 32);

    // Tell the RLC which queue is the kernel interface queue
    // (gfx_v12_0_kiq_setting).
    uint32_t sched = readGcRegister(kRegRLC_CP_SCHEDULERS, 1);
    if (sched != 0xFFFFFFFF) {
        const uint32_t value = ((sched & 0xFFFFFF00u) | (1u << 5) | 0u | 0u) | 0x80u;
        writeGcRegister(kRegRLC_CP_SCHEDULERS, value, 1);
        setProperty("ComputeQueue,RlcSchedulers",
                    static_cast<uint64_t>(readGcRegister(kRegRLC_CP_SCHEDULERS, 1)), 32);
    }
    return true;
}

bool RDNA4Driver::activateComputeQueue() {
    LOG("activateComputeQueue: enabling MEC pipes and starting HQD");
    enableCpGfx();

    if (computeMqdGpuAddr == 0) {
        LOG_ERROR("activateComputeQueue: no MQD, call initComputeQueue first");
        return false;
    }

    if (!enableMecRs64Pipes()) {
        return false;
    }

    // Select ME1 / PIPE0 / QUEUE0 / VMID0 for the HQD register window.
    grbmSelect(1, 0, 0, 0);
    setProperty("ComputeQueue,GrbmCntl",
                static_cast<uint64_t>(readGcRegister(kRegGRBM_GFX_CNTL, 1)), 32);
    setProperty("ComputeQueue,CpStat",
                static_cast<uint64_t>(readGcRegister(kRegCP_STAT, 0)), 32);

    // Disable wptr polling; the doorbell drives submission.
    uint32_t pollCntl = readGcRegister(kRegCP_PQ_WPTR_POLL_CNTL);
    if (pollCntl != 0xFFFFFFFF) {
        writeGcRegister(kRegCP_PQ_WPTR_POLL_CNTL, pollCntl & ~(1u << 31));
    }

    // If a queue is already active in this slot, dequeue it first.
    if (readGcRegister(kRegCP_HQD_ACTIVE) & 1u) {
        LOG("  HQD already active, requesting dequeue");
        writeGcRegister(kRegCP_HQD_DEQUEUE_REQUEST, 1);
        for (int i = 0; i < 1000; i++) {
            if ((readGcRegister(kRegCP_HQD_ACTIVE) & 1u) == 0) break;
            IODelay(1);
        }
        writeGcRegister(kRegCP_HQD_DEQUEUE_REQUEST, 0);
    }

    const uint64_t mqdGpuAddr = computeMqdGpuAddr;
    const uint32_t* mqd = reinterpret_cast<const uint32_t*>(
        const_cast<uint8_t*>(vram) + (mqdGpuAddr - gmcContext.vramStart));

    // --- Program the HQD from the MQD (gfx_v12_0_kiq_init_register) ---
    writeGcRegister(kRegCP_HQD_EOP_BASE_ADDR, readMqd(mqd, MqdOff::cp_hqd_eop_base_addr_lo));
    writeGcRegister(kRegCP_HQD_EOP_BASE_ADDR_HI, readMqd(mqd, MqdOff::cp_hqd_eop_base_addr_hi));
    writeGcRegister(kRegCP_HQD_EOP_CONTROL, readMqd(mqd, MqdOff::cp_hqd_eop_control));
    writeGcRegister(kRegCP_HQD_PQ_DOORBELL_CONTROL,
                    readMqd(mqd, MqdOff::cp_hqd_pq_doorbell_control));

    writeGcRegister(kRegCP_MQD_BASE_ADDR, readMqd(mqd, MqdOff::cp_mqd_base_addr_lo));
    writeGcRegister(kRegCP_MQD_BASE_ADDR_HI, readMqd(mqd, MqdOff::cp_mqd_base_addr_hi));
    writeGcRegister(kRegCP_MQD_CONTROL, readMqd(mqd, MqdOff::cp_mqd_control));

    writeGcRegister(kRegCP_HQD_PQ_BASE, readMqd(mqd, MqdOff::cp_hqd_pq_base_lo));
    writeGcRegister(kRegCP_HQD_PQ_BASE_HI, readMqd(mqd, MqdOff::cp_hqd_pq_base_hi));
    writeGcRegister(kRegCP_HQD_PQ_CONTROL, readMqd(mqd, MqdOff::cp_hqd_pq_control));
    writeGcRegister(kRegCP_HQD_PQ_RPTR_REPORT_ADDR,
                    readMqd(mqd, MqdOff::cp_hqd_pq_rptr_report_addr_lo));
    writeGcRegister(kRegCP_HQD_PQ_RPTR_REPORT_ADDR_HI,
                    readMqd(mqd, MqdOff::cp_hqd_pq_rptr_report_addr_hi));
    writeGcRegister(kRegCP_HQD_PQ_WPTR_POLL_ADDR,
                    readMqd(mqd, MqdOff::cp_hqd_pq_wptr_poll_addr_lo));
    writeGcRegister(kRegCP_HQD_PQ_WPTR_POLL_ADDR_HI,
                    readMqd(mqd, MqdOff::cp_hqd_pq_wptr_poll_addr_hi));

    writeGcRegister(kRegCP_HQD_PQ_WPTR_LO, 0);
    writeGcRegister(kRegCP_HQD_PQ_WPTR_HI, 0);
    writeGcRegister(kRegCP_HQD_PQ_RPTR, 0);
    writeGcRegister(kRegCP_HQD_VMID, 0);
    writeGcRegister(kRegCP_HQD_PERSISTENT_STATE,
                    readMqd(mqd, MqdOff::cp_hqd_persistent_state));
    writeGcRegister(kRegCP_HQD_IB_CONTROL, readMqd(mqd, MqdOff::cp_hqd_ib_control));

    // Doorbell range: one 8-byte slot starting at doorbellIndex.
    const uint32_t dbBase = readMqd(mqd, MqdOff::cp_hqd_pq_doorbell_control);
    const uint32_t dbIndex = (dbBase >> 2) & 0x3FFFu;
    writeGcRegister(kRegCP_MEC_DOORBELL_RANGE_LOWER, dbIndex * 8u);
    writeGcRegister(kRegCP_MEC_DOORBELL_RANGE_UPPER, (dbIndex + 1u) * 8u);

    // Start the EOP fetcher before activating the HQD (kfd hqd_load does this
    // with CP_HQD_EOP_RPTR.INIT_FETCHER; skipping it leaves the queue idle).
    writeGcRegister(kRegCP_HQD_EOP_RPTR, 1u << 31);

    // Activate.
    writeGcRegister(kRegCP_HQD_ACTIVE, 1);
    uint32_t pqStatus = readGcRegister(kRegCP_PQ_STATUS);
    if (pqStatus != 0xFFFFFFFF) {
        writeGcRegister(kRegCP_PQ_STATUS, pqStatus | (1u << 1));
    }

    IODelay(50);

    const uint32_t active = readGcRegister(kRegCP_HQD_ACTIVE);
    const uint32_t vmid = readGcRegister(kRegCP_HQD_VMID);
    const uint32_t pqBaseLo = readGcRegister(kRegCP_HQD_PQ_BASE);
    const uint32_t pqBaseHi = readGcRegister(kRegCP_HQD_PQ_BASE_HI);
    const uint32_t pqCtl = readGcRegister(kRegCP_HQD_PQ_CONTROL);

    grbmSelect(0, 0, 0, 0);

    LOG("  CP_HQD_ACTIVE=0x%08x VMID=0x%08x PQ_BASE=0x%08x:%08x PQ_CONTROL=0x%08x",
        active, vmid, pqBaseHi, pqBaseLo, pqCtl);
    LOG("  CP_MEC_DOORBELL_RANGE=[%u, %u) bytes", dbIndex * 8u, (dbIndex + 1u) * 8u);

    setProperty("ComputeQueue,Active", static_cast<uint64_t>(active & 1u), 32);
    setProperty("ComputeQueue,HqdActive", static_cast<uint64_t>(active), 32);
    setProperty("ComputeQueue,HqdVmid", static_cast<uint64_t>(vmid), 32);
    setProperty("ComputeQueue,HqdPqControl", static_cast<uint64_t>(pqCtl), 32);
    setProperty("ComputeQueue,HqdPqBase", (static_cast<uint64_t>(pqBaseHi) << 32) | pqBaseLo, 64);

    if ((active & 1u) == 0) {
        LOG_ERROR("activateComputeQueue: CP_HQD_ACTIVE did not stick");
        return false;
    }

    LOG("activateComputeQueue: queue active");
    return true;
}

bool RDNA4Driver::deactivateComputeQueue() {
    if (computeMqdGpuAddr == 0) return true;

    grbmSelect(1, 0, 0, 0);
    const uint32_t active = readGcRegister(kRegCP_HQD_ACTIVE);
    if (active & 1u) {
        writeGcRegister(kRegCP_HQD_DEQUEUE_REQUEST, 1);
        for (int i = 0; i < 1000; i++) {
            if ((readGcRegister(kRegCP_HQD_ACTIVE) & 1u) == 0) break;
            IODelay(1);
        }
        if (readGcRegister(kRegCP_HQD_ACTIVE) & 1u) {
            LOG_ERROR("deactivateComputeQueue: queue still active");
            grbmSelect(0, 0, 0, 0);
            return false;
        }
    }
    grbmSelect(0, 0, 0, 0);
    setProperty("ComputeQueue,Active", static_cast<uint64_t>(0), 32);
    LOG("deactivateComputeQueue: queue stopped");
    return true;
}

bool RDNA4Driver::enableCpGfx() {
    // gfx_v12_0_cp_gfx_start: CP_MAX_CONTEXT and CP_DEVICE_ID must be valid
    // before the CP accepts work. CP_DEVICE_ID resets to 0 and Linux writes 1.
    writeGcRegister(kRegCP_MAX_CONTEXT, 7, 0);
    writeGcRegister(kRegCP_DEVICE_ID, 1, 0);

    // gfx_v12_0_cp_gfx_enable: clear CP_ME_CNTL.ME_HALT (bit 28) and
    // PFP_HALT (bit 26), then wait for CP_STAT to drain. Without this the
    // ME/PFP never fetch from CP_RB0 no matter how the ring is programmed.
    uint32_t me = readGcRegister(kRegCP_ME_CNTL, 1);
    if (me == 0xFFFFFFFF) {
        LOG_ERROR("enableCpGfx: CP_ME_CNTL not readable");
        return false;
    }
    const uint32_t value = me & ~((1u << 28) | (1u << 26));
    writeGcRegister(kRegCP_ME_CNTL, value, 1);

    uint32_t cpStat = 0xFFFFFFFF;
    for (uint32_t i = 0; i < 50000; i++) {
        cpStat = readGcRegister(kRegCP_STAT, 0);
        if (cpStat == 0) break;
        IODelay(1);
    }

    const uint32_t after = readGcRegister(kRegCP_ME_CNTL, 1);
    LOG("enableCpGfx: CP_ME_CNTL 0x%08x -> 0x%08x (readback 0x%08x), CP_STAT=0x%08x",
        me, value, after, cpStat);
    setProperty("GfxRing,MeCntlBefore", static_cast<uint64_t>(me), 32);
    setProperty("GfxRing,MeCntlAfter", static_cast<uint64_t>(after), 32);
    setProperty("GfxRing,CpStatAfterEnable", static_cast<uint64_t>(cpStat), 32);
    setProperty("GfxRing,CpDeviceId",
                static_cast<uint64_t>(readGcRegister(kRegCP_DEVICE_ID, 0)), 32);
    setProperty("GfxRing,CpMaxContext",
                static_cast<uint64_t>(readGcRegister(kRegCP_MAX_CONTEXT, 0)), 32);
    return true;
}

bool RDNA4Driver::initGfxRing(uint64_t ringGpuAddr, uint32_t ringSizeBytes,
                              uint64_t rptrGpuAddr, uint64_t wptrGpuAddr,
                              uint32_t doorbellIndex) {
    LOG("initGfxRing: ring=0x%llx size=%u rptr=0x%llx wptr=0x%llx db=%u",
        static_cast<unsigned long long>(ringGpuAddr), ringSizeBytes,
        static_cast<unsigned long long>(rptrGpuAddr),
        static_cast<unsigned long long>(wptrGpuAddr), doorbellIndex);

    if ((ringSizeBytes & (ringSizeBytes - 1)) != 0 || ringSizeBytes < 0x1000) {
        LOG_ERROR("initGfxRing: ring size %u is not a power of two", ringSizeBytes);
        return false;
    }
    if ((ringGpuAddr & 0xFFu) != 0) {
        LOG_ERROR("initGfxRing: ring address is not 256-byte aligned");
        return false;
    }

    // ME/PFP must be unhalted before the ring can be fetched.
    enableCpGfx();

    // gfx_v12_0_cp_gfx_resume: RB_BUFSZ = log2(ring_bytes / 8),
    // RB_BLKSZ = RB_BUFSZ - 2.
    const uint32_t rbBufsz = orderBase2(ringSizeBytes / 8u);
    const uint32_t rbCntl = (rbBufsz & 0x3Fu) |
                            (((rbBufsz >= 2u ? rbBufsz - 2u : 0u) & 0x3Fu) << 8);

    grbmSelect(0, 0, 0, 0);   // graphics ME0 / PIPE0

    writeGcRegister(kRegCP_RB_WPTR_DELAY, 0, 0);
    writeGcRegister(kRegCP_RB_VMID, 0, 0);
    writeGcRegister(kRegCP_RB0_CNTL, rbCntl, 0);
    writeGcRegister(kRegCP_RB0_WPTR, 0, 0);
    writeGcRegister(kRegCP_RB0_WPTR_HI, 0, 0);
    writeGcRegister(kRegCP_RB0_RPTR_ADDR,
                    static_cast<uint32_t>(rptrGpuAddr & 0xFFFFFFFCu), 0);
    writeGcRegister(kRegCP_RB0_RPTR_ADDR_HI,
                    static_cast<uint32_t>(rptrGpuAddr >> 32) & 0xFFFFu, 0);
    writeGcRegister(kRegCP_RB_WPTR_POLL_ADDR_LO,
                    static_cast<uint32_t>(wptrGpuAddr), 0);
    writeGcRegister(kRegCP_RB_WPTR_POLL_ADDR_HI,
                    static_cast<uint32_t>(wptrGpuAddr >> 32), 0);

    // Upstream writes CP_RB0_CNTL twice with a 1 ms gap; there is a CP
    // latch that needs the redundant write.
    IOSleep(1);
    writeGcRegister(kRegCP_RB0_CNTL, rbCntl, 0);

    const uint64_t base = ringGpuAddr >> 8;
    writeGcRegister(kRegCP_RB0_BASE, static_cast<uint32_t>(base), 0);
    writeGcRegister(kRegCP_RB0_BASE_HI, static_cast<uint32_t>(base >> 32), 0);
    writeGcRegister(kRegCP_RB_ACTIVE, 1, 0);

    uint32_t dbc = readGcRegister(kRegCP_RB_DOORBELL_CONTROL, 0);
    dbc = (dbc & ~(0x3FFFu << 2)) | ((doorbellIndex & 0x3FFFu) << 2);
    dbc |= (1u << 30);
    writeGcRegister(kRegCP_RB_DOORBELL_CONTROL, dbc, 0);
    writeGcRegister(kRegCP_RB_DOORBELL_RANGE_LOWER,
                    (doorbellIndex & 0x3FFu) << 2, 0);
    writeGcRegister(kRegCP_RB_DOORBELL_RANGE_UPPER, 0x3FFu << 2, 0);

    IOSleep(1);
    const uint32_t cntlRead = readGcRegister(kRegCP_RB0_CNTL, 0);
    const uint32_t baseRead = readGcRegister(kRegCP_RB0_BASE, 0);
    const uint32_t baseHiRead = readGcRegister(kRegCP_RB0_BASE_HI, 0);
    const uint32_t activeRead = readGcRegister(kRegCP_RB_ACTIVE, 0);
    const uint32_t dbcRead = readGcRegister(kRegCP_RB_DOORBELL_CONTROL, 0);

    grbmSelect(0, 0, 0, 0);

    LOG("initGfxRing: CNTL=0x%08x BASE=0x%08x:%08x ACTIVE=0x%08x DBC=0x%08x",
        cntlRead, baseHiRead, baseRead, activeRead, dbcRead);
    setProperty("GfxRing,CNTL", static_cast<uint64_t>(cntlRead), 32);
    setProperty("GfxRing,Base", (static_cast<uint64_t>(baseHiRead) << 32) | baseRead, 64);
    setProperty("GfxRing,Active", static_cast<uint64_t>(activeRead), 32);
    setProperty("GfxRing,DoorbellControl", static_cast<uint64_t>(dbcRead), 32);
    setProperty("GfxRing,RingGPU", ringGpuAddr, 64);
    setProperty("GfxRing,RptrGPU", rptrGpuAddr, 64);
    setProperty("GfxRing,WptrGPU", wptrGpuAddr, 64);
    return (activeRead & 1u) != 0;
}

void RDNA4Driver::dumpComputeQueueState() {
    // Called after a fence timeout: capture enough state to tell whether the
    // doorbell fired, the CP consumed the ring, or a translation fault stalled
    // the queue. All of these registers are GC segment 0.
    grbmSelect(1, 0, 0, 0);

    setProperty("Diag,HqdActive", static_cast<uint64_t>(readGcRegister(0x1FAB)), 32);
    setProperty("Diag,PqRptr", static_cast<uint64_t>(readGcRegister(0x1FB3)), 32);
    setProperty("Diag,PqWptrLo", static_cast<uint64_t>(readGcRegister(0x1FDF)), 32);
    setProperty("Diag,PqWptrHi", static_cast<uint64_t>(readGcRegister(0x1FE0)), 32);
    setProperty("Diag,PqControl", static_cast<uint64_t>(readGcRegister(0x1FBA)), 32);
    setProperty("Diag,EopRptr", static_cast<uint64_t>(readGcRegister(0x1FD1)), 32);
    setProperty("Diag,EopWptr", static_cast<uint64_t>(readGcRegister(0x1FD2)), 32);
    setProperty("Diag,PqStatus", static_cast<uint64_t>(readGcRegister(0x1E58)), 32);
    setProperty("Diag,CpStat", static_cast<uint64_t>(readGcRegister(0x0F40)), 32);
    setProperty("Diag,CpDebug", static_cast<uint64_t>(readGcRegister(0x1E1F)), 32);
    setProperty("Diag,GrbmStatus", static_cast<uint64_t>(readGcRegister(0x0DA4)), 32);
    setProperty("Diag,GrbmStatus2", static_cast<uint64_t>(readGcRegister(0x0DA2)), 32);
    setProperty("Diag,FaultStatusLo", static_cast<uint64_t>(readGcRegister(0x15D0)), 32);
    setProperty("Diag,FaultStatusHi", static_cast<uint64_t>(readGcRegister(0x15D1)), 32);
    setProperty("Diag,FaultAddrLo", static_cast<uint64_t>(readGcRegister(0x15D2)), 32);
    setProperty("Diag,FaultAddrHi", static_cast<uint64_t>(readGcRegister(0x15D3)), 32);

    // MEC/ME state lives in GC segment 1.
    setProperty("Diag,MecPc", static_cast<uint64_t>(readGcRegister(0x2908, 1)), 32);
    setProperty("Diag,MecCntl", static_cast<uint64_t>(readGcRegister(0x0802, 1)), 32);
    setProperty("Diag,MeCntl", static_cast<uint64_t>(readGcRegister(0x0803, 1)), 32);
    setProperty("Diag,MecStartReg", static_cast<uint64_t>(readGcRegister(0x2900, 1)), 32);
    setProperty("Diag,MecStartRegHi", static_cast<uint64_t>(readGcRegister(0x2938, 1)), 32);
    setProperty("Diag,CpcIcBaseLo", static_cast<uint64_t>(readGcRegister(0x584C, 1)), 32);
    setProperty("Diag,CpcIcBaseHi", static_cast<uint64_t>(readGcRegister(0x584D, 1)), 32);
    setProperty("Diag,CpcIcBaseCntl", static_cast<uint64_t>(readGcRegister(0x584E, 1)), 32);
    setProperty("Diag,MecMdBaseLo", static_cast<uint64_t>(readGcRegister(0x5870, 1)), 32);
    setProperty("Diag,MecMdBaseHi", static_cast<uint64_t>(readGcRegister(0x5871, 1)), 32);
    setProperty("Diag,PfpIcBaseLo", static_cast<uint64_t>(readGcRegister(0x5840, 1)), 32);
    setProperty("Diag,MeIcBaseLo", static_cast<uint64_t>(readGcRegister(0x5844, 1)), 32);

    grbmSelect(0, 0, 0, 0);

    if (doorbell) {
        setProperty("Diag,Doorbell0", static_cast<uint64_t>(doorbell[0]), 32);
    }

    if (vram && computeRing) {
        const uint64_t fenceOffset = computeRing->getFenceVRAMOffset();
        if (fenceOffset + sizeof(uint64_t) <= vramSize) {
            volatile uint64_t* fence = reinterpret_cast<volatile uint64_t*>(
                const_cast<uint8_t*>(vram) + fenceOffset);
            setProperty("Diag,FenceValue", static_cast<uint64_t>(*fence), 64);
        }
    }

    if (vram && computeMqdGpuAddr >= gmcContext.vramStart) {
        const uint64_t mqdOffset = computeMqdGpuAddr - gmcContext.vramStart;
        if (mqdOffset + 4 * 141 <= vramSize) {
            const volatile uint32_t* mqd = reinterpret_cast<const volatile uint32_t*>(
                const_cast<uint8_t*>(vram) + mqdOffset);
            const uint64_t rptrAddr = (static_cast<uint64_t>(mqd[140]) << 32) | mqd[139];
            if (rptrAddr >= gmcContext.vramStart &&
                rptrAddr - gmcContext.vramStart + 4 <= vramSize) {
                volatile uint32_t* report = reinterpret_cast<volatile uint32_t*>(
                    const_cast<uint8_t*>(vram) + (rptrAddr - gmcContext.vramStart));
                setProperty("Diag,RptrReport", static_cast<uint64_t>(*report), 32);
            }
        }
    }

    if (vram && computeRing) {
        volatile uint32_t* ring = reinterpret_cast<volatile uint32_t*>(
            const_cast<uint8_t*>(vram) + VramLayout::kComputeRing);
        setProperty("Diag,RingWptr", static_cast<uint64_t>(computeRing->getWritePointer()), 32);
        setProperty("Diag,RingDword0", static_cast<uint64_t>(ring[0]), 32);
        setProperty("Diag,RingDword1", static_cast<uint64_t>(ring[1]), 32);
    }
}
