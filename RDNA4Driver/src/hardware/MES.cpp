#include "../core/RDNA4Driver.hpp"
#include "VramLayout.hpp"
#include "GMCContext.hpp"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>

//==============================================================================
// MES (Micro Engine Scheduler)
//
// GFX12 (GC 12.0.x) always initializes MES: amdgpu_discovery.c adds the
// mes_v12_0 IP block and sets enable_mes = true unconditionally.  MES owns the
// queue map, so legacy KIQ/HQD programming alone never gets scheduled.
//
// This file ports the bring-up path from mes_v12_0.c:
//   mes_v12_0_enable()       - CP_MES_CNTL per pipe (ME=3)
//   mes_v12_0_mqd_init()     - MES queue MQD (same v12_compute_mqd layout)
//   mes_v12_0_queue_init_register() - HQD programming
//   mes_v12_0_map_legacy_queue()    - ADD_QUEUE with map_legacy_kq=1
//==============================================================================

namespace {

// MES registers (GC segment 1)
constexpr uint32_t kRegMesCntl        = 0x2807;
constexpr uint32_t kRegMesStartLo     = 0x2800;
constexpr uint32_t kRegMesStartHi     = 0x289D;
constexpr uint32_t kRegMesMscratchLo  = 0x2815;
constexpr uint32_t kRegMesMscratchHi  = 0x2814;
constexpr uint32_t kRegMesGp3Lo       = 0x2849;
constexpr uint32_t kRegMesDoorbellCtl1 = 0x283C;
constexpr uint32_t kRegRlcCpSchedulers = 0x098A;
constexpr uint32_t kRegCpUnmappedDoorbell = 0x0880;
constexpr uint32_t kRegCpHqdGfxControl = 0x1E9F;

// MES aggregated doorbell window (amdgpu_mes.c: doorbell_index.mes_aggregated)
constexpr uint32_t kMesAggregatedDoorbellBase = 0x1A0;
constexpr uint32_t kMesPriorityLevels = 5;
constexpr uint32_t kMesSchCtxBytes = 64 * 1024;

// HQD registers (GC segment 0, selected via GRBM ME=3/pipe)
constexpr uint32_t kRegMqdBase        = 0x1FA9;
constexpr uint32_t kRegMqdBaseHi      = 0x1FAA;
constexpr uint32_t kRegMqdControl     = 0x1FCB;
constexpr uint32_t kRegHqdActive      = 0x1FAB;
constexpr uint32_t kRegHqdVmid        = 0x1FAC;
constexpr uint32_t kRegHqdPersistent  = 0x1FAD;
constexpr uint32_t kRegHqdPqBase      = 0x1FB1;
constexpr uint32_t kRegHqdPqBaseHi    = 0x1FB2;
constexpr uint32_t kRegHqdPqRptr      = 0x1FB3;
constexpr uint32_t kRegHqdRptrRep     = 0x1FB4;
constexpr uint32_t kRegHqdRptrRepHi   = 0x1FB5;
constexpr uint32_t kRegHqdWptrPoll    = 0x1FB6;
constexpr uint32_t kRegHqdWptrPollHi  = 0x1FB7;
constexpr uint32_t kRegHqdDoorbell    = 0x1FB8;
constexpr uint32_t kRegHqdPqControl   = 0x1FBA;
constexpr uint32_t kRegHqdWptrLo      = 0x1FDF;
constexpr uint32_t kRegHqdWptrHi      = 0x1FE0;
constexpr uint32_t kRegHqdIbControl   = 0x1FBE;
constexpr uint32_t kRegHqdIqTimer     = 0x1FBF;
constexpr uint32_t kRegHqdQuantum     = 0x1FB0;
constexpr uint32_t kRegHqdEopBase     = 0x1FCE;
constexpr uint32_t kRegHqdEopBaseHi   = 0x1FCF;
constexpr uint32_t kRegHqdEopControl  = 0x1FD0;
constexpr uint32_t kRegPqStatus       = 0x1E58;

// v12_compute_mqd dword offsets
namespace Mqd {
constexpr uint32_t header           = 0;
constexpr uint32_t pipelinestat     = 11;
constexpr uint32_t se0              = 23;
constexpr uint32_t se1              = 24;
constexpr uint32_t se2              = 26;
constexpr uint32_t se3              = 27;
constexpr uint32_t misc             = 32;
constexpr uint32_t mqdLo            = 128;
constexpr uint32_t mqdHi            = 129;
constexpr uint32_t active           = 130;
constexpr uint32_t vmid             = 131;
constexpr uint32_t persistent       = 132;
constexpr uint32_t quantum          = 135;
constexpr uint32_t pqLo             = 136;
constexpr uint32_t pqHi             = 137;
constexpr uint32_t rptrRepLo        = 139;
constexpr uint32_t rptrRepHi        = 140;
constexpr uint32_t wptrPollLo       = 141;
constexpr uint32_t wptrPollHi       = 142;
constexpr uint32_t doorbell         = 143;
constexpr uint32_t pqControl        = 145;
constexpr uint32_t ibControl        = 149;
constexpr uint32_t mqdControl       = 162;
constexpr uint32_t eopLo            = 165;
constexpr uint32_t eopHi            = 166;
constexpr uint32_t eopControl       = 167;
constexpr uint32_t reserved184      = 184;
} // namespace Mqd

constexpr uint32_t kMesRingBytes   = 64 * 1024;
constexpr uint32_t kMesEopBytes    = 4096;
constexpr uint32_t kPqControlSeed  = 0x00308509;
constexpr uint32_t kPersistentSeed = 0x0BE05501;
constexpr uint32_t kIqTimerDefault = 0x00000000;  // regCP_HQD_IQ_TIMER_DEFAULT
constexpr uint32_t kQuantumDefault = 0x00000000;  // regCP_HQD_QUANTUM_DEFAULT

uint32_t orderBase2(uint32_t x) {
    uint32_t r = 0;
    while (r < 31 && (1u << r) < x) r++;
    return r;
}

} // namespace

volatile uint8_t* RDNA4Driver::mesBufferCpu(IOBufferMemoryDescriptor* md,
                                            uint64_t gpuAddr) {
    if (md)
        return static_cast<volatile uint8_t*>(md->getBytesNoCopy());
    if (!gpuAddr || !vram)
        return nullptr;
    return const_cast<uint8_t*>(vram) + (gpuAddr - gmcContext.vramStart);
}

bool RDNA4Driver::mesInit() {
    LOG("mesInit: starting MES bring-up (uc_start=0x%llx)",
        static_cast<unsigned long long>(mesUcStart));

    if (!vram || vramSize == 0 || !doorbell) {
        LOG_ERROR("mesInit: VRAM/doorbell not mapped");
        return false;
    }
    if (mesUcStart == 0) {
        LOG_ERROR("mesInit: MES ucode start address unknown");
        return false;
    }

    // Upstream allocates MES rings/MQDs in GTT (system memory via GART), but on
    // this machine VRAM buffers are what actually get read back (0.2.9 A/B).
    // rdna4-mes-gtt=1 switches back to GART allocation for comparison.
    uint32_t useGtt = 0;
    PE_parse_boot_argn("rdna4-mes-gtt", &useGtt, sizeof(useGtt));
    mesUseGtt = (useGtt != 0);
    setProperty("MES,UseGtt", static_cast<uint64_t>(mesUseGtt ? 1 : 0), 32);

    auto allocBuf = [&](size_t bytes, IOBufferMemoryDescriptor** md) -> uint64_t {
        if (mesUseGtt)
            return gmcAllocGartBuffer(*this, gmcContext, bytes, md);
        *md = nullptr;
        return allocateVRAMBuffer(bytes);
    };

    const uint32_t ringDwords = kMesRingBytes / 4;
    for (uint32_t pipe = 0; pipe < 2; pipe++) {
        mesRingGpuAddr[pipe] = allocBuf(kMesRingBytes, &mesRingMd[pipe]);
        mesMqdGpuAddr[pipe] = allocBuf(4096, &mesMqdMd[pipe]);
        mesEopGpuAddr[pipe] = allocBuf(kMesEopBytes, &mesEopMd[pipe]);
        const uint64_t wp = allocBuf(4096, &mesWpMd[pipe]);
        mesRptrGpuAddr[pipe] = wp;
        mesWptrGpuAddr[pipe] = wp + 2048;
        mesDoorbellIndex[pipe] = (pipe == 0) ? 0x20u : 0x22u;
        mesRingDwords[pipe] = ringDwords;
        mesRingWrite[pipe] = 0;

        if (!mesRingGpuAddr[pipe] || !mesMqdGpuAddr[pipe] || !mesEopGpuAddr[pipe] ||
            !wp) {
            LOG_ERROR("mesInit: pipe %u allocation failed", pipe);
            return false;
        }
    }
    mesStatusGpuAddr = allocBuf(4096, &mesStatusMd);
    if (!mesStatusGpuAddr) {
        LOG_ERROR("mesInit: status buffer allocation failed");
        return false;
    }

    // Zero all backing memory (ring, MQD, EOP, rptr/wptr, status).
    for (uint32_t pipe = 0; pipe < 2; pipe++) {
        volatile uint8_t* p = mesBufferCpu(mesRingMd[pipe], mesRingGpuAddr[pipe]);
        for (uint32_t i = 0; i < kMesRingBytes; i++) p[i] = 0;
        p = mesBufferCpu(mesMqdMd[pipe], mesMqdGpuAddr[pipe]);
        for (uint32_t i = 0; i < 4096; i++) p[i] = 0;
        p = mesBufferCpu(mesEopMd[pipe], mesEopGpuAddr[pipe]);
        for (uint32_t i = 0; i < kMesEopBytes; i++) p[i] = 0;
        p = mesBufferCpu(mesWpMd[pipe], mesRptrGpuAddr[pipe]);
        for (uint32_t i = 0; i < 4096; i++) p[i] = 0;
    }
    {
        volatile uint8_t* p = mesBufferCpu(mesStatusMd, mesStatusGpuAddr);
        for (uint32_t i = 0; i < 64; i++) p[i] = 0;
    }

    // Publish the new GART entries to both hubs.
    gmcInvalidateTlb(*this);

    // --- Enable MES pipes (mes_v12_0_enable) ---
    const uint64_t ucodeAddr = mesUcStart >> 2;

    // --- RLC routing for the MES KIQ pipe (mes_v12_0_kiq_hw_init) ---
    // Must precede mes_enable: RLC has to know which HQD the MES KIQ owns.
    {
        const uint32_t sched = readGcRegister(kRegRlcCpSchedulers, 1);
        if (sched != 0xFFFFFFFF) {
            const uint32_t v = (sched & 0xFFFFFF00u) | (3u << 5) | (1u << 3) | 0x80u;
            writeGcRegister(kRegRlcCpSchedulers, v, 1);
            LOG("mesInit: RLC_CP_SCHEDULERS 0x%08x -> 0x%08x", sched, v);
        }
    }

    // --- Enable MES pipes (mes_v12_0_enable) ---
    // Upstream resets both pipes, then per pipe programs the ucode start and
    // writes ACTIVE bits built from zero.  uni-MES still runs a SCHED (0) and a
    // KIQ (1) pipe: mes_v12_0_map_legacy_queue() submits on the KIQ pipe.
    //
    // Event log: one (LOG_BUFFER + MSCRATCH) block per pipe.  MSCRATCH points
    // at the scratch half; SET_HW_RESOURCES later hands the firmware the log
    // half via event_intr_history_gpu_mc_ptr.
    if (!mesEventLogGpuAddr) {
        mesEventLogGpuAddr = allocateVRAMBuffer(2u * kMesEventLogBytes);
        if (mesEventLogGpuAddr) {
            volatile uint8_t* p = mesBufferCpu(nullptr, mesEventLogGpuAddr);
            for (uint32_t i = 0; i < 2u * kMesEventLogBytes; i++) p[i] = 0;
        }
    }
    setProperty("MES,EventLogGPU", mesEventLogGpuAddr, 64);

    grbmSelect(3, 0, 0, 0);
    {
        const uint64_t scratch0 = mesEventLogGpuAddr ? (mesEventLogGpuAddr + 0xC000) : 0;
        writeGcRegister(kRegMesMscratchLo, static_cast<uint32_t>(scratch0), 1);
        writeGcRegister(kRegMesMscratchHi, static_cast<uint32_t>(scratch0 >> 32), 1);
    }
    {
        uint32_t ctl = readGcRegister(kRegMesCntl, 1);
        if (ctl == 0xFFFFFFFF) {
            LOG_ERROR("mesInit: CP_MES_CNTL not readable");
            return false;
        }
        ctl |= (1u << 16) | (1u << 17);   // PIPE0_RESET | PIPE1_RESET
        writeGcRegister(kRegMesCntl, ctl, 1);
        setProperty("MES,CntlReset", static_cast<uint64_t>(ctl), 32);
    }
    grbmSelect(0, 0, 0, 0);

    for (uint32_t pipe = 0; pipe < 2; pipe++) {
        grbmSelect(3, pipe, 0, 0);
        if (mesEventLogGpuAddr) {
            const uint64_t scratch =
                mesEventLogGpuAddr + pipe * kMesEventLogBytes + 0xC000;
            writeGcRegister(kRegMesMscratchLo, static_cast<uint32_t>(scratch), 1);
            writeGcRegister(kRegMesMscratchHi, static_cast<uint32_t>(scratch >> 32), 1);
        }
        uint32_t ctl = readGcRegister(kRegMesCntl, 1);
        ctl |= (1u << (16 + pipe));
        writeGcRegister(kRegMesCntl, ctl, 1);
        writeGcRegister(kRegMesStartLo, static_cast<uint32_t>(ucodeAddr), 1);
        writeGcRegister(kRegMesStartHi, static_cast<uint32_t>(ucodeAddr >> 32), 1);
        const uint32_t activeValue = (1u << 26) | (pipe ? (1u << 27) : 0u);
        writeGcRegister(kRegMesCntl, activeValue, 1);
        grbmSelect(0, 0, 0, 0);
    }
    IODelay(500);
    grbmSelect(3, 0, 0, 0);
    const uint32_t startLo = readGcRegister(kRegMesStartLo, 1);
    const uint32_t startHi = readGcRegister(kRegMesStartHi, 1);
    // Liveness probe: CP_MES_INSTR_PNTR only advances while the microengine is
    // fetching instructions; a frozen value means the ucode never started.
    const uint32_t pntrA = readGcRegister(0x2813, 1);
    IODelay(1000);
    const uint32_t pntrB = readGcRegister(0x2813, 1);
    const uint32_t gp0 = readGcRegister(0x2845, 1);
    const uint32_t gp1 = readGcRegister(0x2846, 1);
    const uint32_t cntlAfter = readGcRegister(kRegMesCntl, 1);
    grbmSelect(0, 0, 0, 0);
    LOG("mesInit: INSTR_PNTR %u -> %u GP0=0x%08x GP1=0x%08x",
        pntrA, pntrB, gp0, gp1);
    setProperty("MES,Pntr0", static_cast<uint64_t>(pntrA), 32);
    setProperty("MES,Pntr1", static_cast<uint64_t>(pntrB), 32);
    setProperty("MES,Gp0Lo", static_cast<uint64_t>(gp0), 32);
    setProperty("MES,Gp1Lo", static_cast<uint64_t>(gp1), 32);
    LOG("mesInit: start=0x%08x:%08x readback 0x%08x:%08x cntl=0x%08x",
        static_cast<uint32_t>(ucodeAddr >> 32), static_cast<uint32_t>(ucodeAddr),
        startHi, startLo, cntlAfter);
    setProperty("MES,StartReadback",
                (static_cast<uint64_t>(startHi) << 32) | startLo, 64);

    // --- Unmapped doorbell handling ---
    // mes_v12_0_enable_unmapped_doorbell_handling(): PROC_LSB=0xd, ENABLE=1.
    {
        uint32_t v = readGcRegister(kRegCpUnmappedDoorbell, 1);
        if (v != 0xFFFFFFFF) {
            const uint32_t before = v;
            v &= ~0x00001F00u;   // PROC_LSB
            v |= (0xDu << 8) | 1u;
            writeGcRegister(kRegCpUnmappedDoorbell, v, 1);
            LOG("mesInit: CP_UNMAPPED_DOORBELL 0x%08x -> 0x%08x", before, v);
            setProperty("MES,UnmappedDb", static_cast<uint64_t>(v), 32);
        }
    }

    // The CP drops doorbell writes outside CP_MEC_DOORBELL_RANGE. The MES
    // rings use indexes 0x20/0x22 (Cobalt64 convention), so open the range wide
    // before ringing them (Linux sets this in cp_set_doorbell_range).
    writeGcRegister(0x1DFC, 0, 0);          // CP_MEC_DOORBELL_RANGE_LOWER
    writeGcRegister(0x1DFD, 0x0000FFFCu, 0); // CP_MEC_DOORBELL_RANGE_UPPER

    // --- MQD + HQD programming (only after MES is running) ---
    for (uint32_t pipe = 0; pipe < 2; pipe++) {
        volatile uint32_t* m = reinterpret_cast<volatile uint32_t*>(
            mesBufferCpu(mesMqdMd[pipe], mesMqdGpuAddr[pipe]));

        m[Mqd::header] = 0xC0310800u;
        m[Mqd::pipelinestat] = 0x00000001u;
        m[Mqd::se0] = 0xFFFFFFFFu;
        m[Mqd::se1] = 0xFFFFFFFFu;
        m[Mqd::se2] = 0xFFFFFFFFu;
        m[Mqd::se3] = 0xFFFFFFFFu;
        m[Mqd::misc] = 0x00000007u;

        const uint64_t eop = mesEopGpuAddr[pipe] >> 8;
        m[Mqd::eopLo] = static_cast<uint32_t>(eop);
        m[Mqd::eopHi] = static_cast<uint32_t>(eop >> 32);
        const uint32_t eopLog = orderBase2(kMesEopBytes / 4) - 1u;
        m[Mqd::eopControl] = (0x6u & ~0x1Fu) | (eopLog & 0x1Fu);

        const uint64_t pq = mesRingGpuAddr[pipe] >> 8;
        m[Mqd::pqLo] = static_cast<uint32_t>(pq);
        m[Mqd::pqHi] = static_cast<uint32_t>(pq >> 32);

        m[Mqd::mqdLo] = static_cast<uint32_t>(mesMqdGpuAddr[pipe] & 0xFFFFFFFCu);
        m[Mqd::mqdHi] = static_cast<uint32_t>(mesMqdGpuAddr[pipe] >> 32);
        m[Mqd::mqdControl] = 0x00000100u;  // PRIV_STATE, VMID 0

        m[Mqd::rptrRepLo] = static_cast<uint32_t>(mesRptrGpuAddr[pipe] & 0xFFFFFFFCu);
        m[Mqd::rptrRepHi] = static_cast<uint32_t>(mesRptrGpuAddr[pipe] >> 32) & 0xFFFFu;
        m[Mqd::wptrPollLo] = static_cast<uint32_t>(mesWptrGpuAddr[pipe] & 0xFFFFFFF8u);
        m[Mqd::wptrPollHi] = static_cast<uint32_t>(mesWptrGpuAddr[pipe] >> 32) & 0xFFFFu;

        m[Mqd::doorbell] = ((mesDoorbellIndex[pipe] & 0x3FFFu) << 2) | (1u << 30);

        uint32_t pqc = kPqControlSeed;
        pqc = (pqc & ~0x1Fu) | ((orderBase2(ringDwords) - 1u) & 0x1Fu);
        pqc = (pqc & ~0x3F00u) | ((orderBase2(4096u / 4u) - 1u) << 8);
        // NO_UPDATE_RPTR is deliberately left clear so the CP writes its read
        // pointer into the GART-backed rptr page: that write is the only cheap
        // proof that our page-table entries are really translated.
        pqc |= (1u << 28);   // UNORD_DISPATCH
        pqc |= (1u << 30) | (1u << 31);  // PRIV_STATE, KMD_QUEUE
        m[Mqd::pqControl] = pqc;

        m[Mqd::active] = 1;
        m[Mqd::vmid] = 0;
        m[Mqd::persistent] = kPersistentSeed;
        m[Mqd::ibControl] = 0x00300000u;
        m[Mqd::quantum] = kQuantumDefault;
        m[Mqd::reserved184] = (1u << 15);

        // Program the HQD from the MQD (mes_v12_0_queue_init_register).
        grbmSelect(3, pipe, 0, 0);
        writeGcRegister(kRegHqdEopBase, m[Mqd::eopLo], 0);
        writeGcRegister(kRegHqdEopBaseHi, m[Mqd::eopHi], 0);
        writeGcRegister(kRegHqdEopControl, m[Mqd::eopControl], 0);
        writeGcRegister(kRegHqdDoorbell, m[Mqd::doorbell], 0);
        writeGcRegister(kRegMqdBase, m[Mqd::mqdLo], 0);
        writeGcRegister(kRegMqdBaseHi, m[Mqd::mqdHi], 0);
        writeGcRegister(kRegMqdControl, m[Mqd::mqdControl], 0);
        writeGcRegister(kRegHqdPqBase, m[Mqd::pqLo], 0);
        writeGcRegister(kRegHqdPqBaseHi, m[Mqd::pqHi], 0);
        writeGcRegister(kRegHqdPqControl, m[Mqd::pqControl], 0);
        writeGcRegister(kRegHqdRptrRep, m[Mqd::rptrRepLo], 0);
        writeGcRegister(kRegHqdRptrRepHi, m[Mqd::rptrRepHi], 0);
        writeGcRegister(kRegHqdWptrPoll, m[Mqd::wptrPollLo], 0);
        writeGcRegister(kRegHqdWptrPollHi, m[Mqd::wptrPollHi], 0);
        writeGcRegister(kRegHqdWptrLo, 0, 0);
        writeGcRegister(kRegHqdWptrHi, 0, 0);
        writeGcRegister(kRegHqdPqRptr, 0, 0);
        writeGcRegister(kRegHqdVmid, 0, 0);
        writeGcRegister(kRegHqdPersistent, m[Mqd::persistent], 0);
        writeGcRegister(kRegHqdIbControl, m[Mqd::ibControl], 0);
        writeGcRegister(kRegHqdIqTimer, kIqTimerDefault, 0);
        writeGcRegister(kRegHqdQuantum, m[Mqd::quantum], 0);
        writeGcRegister(kRegHqdActive, 1, 0);
        uint32_t pqStatus = readGcRegister(kRegPqStatus, 0);
        if (pqStatus != 0xFFFFFFFF) {
            writeGcRegister(kRegPqStatus, pqStatus | (1u << 1), 0);
        }
        const uint32_t active = readGcRegister(kRegHqdActive, 0);
        const uint32_t dbCtl = readGcRegister(kRegHqdDoorbell, 0);
        const uint32_t pqCtlRb = readGcRegister(kRegHqdPqControl, 0);
        const uint32_t pqBaseLo = readGcRegister(kRegHqdPqBase, 0);
        const uint32_t pqBaseHi = readGcRegister(kRegHqdPqBaseHi, 0);
        const uint32_t rptrRepLo = readGcRegister(kRegHqdRptrRep, 0);
        const uint32_t wptrPollLo = readGcRegister(kRegHqdWptrPoll, 0);
        const uint32_t mqdCtlRb = readGcRegister(kRegMqdControl, 0);
        LOG("mesInit: pipe %u HQD active=0x%08x pqControl=0x%08x doorbell=%u",
            pipe, active, m[Mqd::pqControl], mesDoorbellIndex[pipe]);
        LOG("mesInit: pipe %u readback dbCtl=0x%08x pqCtl=0x%08x pqBase=0x%08x:%08x"
            " rptrRep=0x%08x wptrPoll=0x%08x mqdCtl=0x%08x",
            pipe, dbCtl, pqCtlRb, pqBaseHi, pqBaseLo, rptrRepLo, wptrPollLo, mqdCtlRb);
        setProperty("MES,HqdActive", static_cast<uint64_t>(active), 32);
        setProperty("MES,DbCtl", static_cast<uint64_t>(dbCtl), 32);
        setProperty("MES,PqCtlRb", static_cast<uint64_t>(pqCtlRb), 32);
        setProperty("MES,PqBaseRb",
                    (static_cast<uint64_t>(pqBaseHi) << 32) | pqBaseLo, 64);
        setProperty("MES,WptrPollRb", static_cast<uint64_t>(wptrPollLo), 32);
        setProperty("MES,MqdCtlRb", static_cast<uint64_t>(mqdCtlRb), 32);
    }
    grbmSelect(0, 0, 0, 0);

    gmcHDPFlush(*this);
    OSSynchronizeIO();

    // CP_MES_GP3_LO is written by the MES firmware itself (mes_v12_0_queue_init
    // reads it as the scheduler version).  A sane version proves the ucode is
    // loaded and executing, not just that CP_MES_CNTL was written.
    grbmSelect(3, 0, 0, 0);
    const uint32_t mesGp3 = readGcRegister(0x2849, 1);
    const uint32_t mesCntl = readGcRegister(kRegMesCntl, 1);
    grbmSelect(0, 0, 0, 0);
    LOG("mesInit: CP_MES_GP3_LO=0x%08x CP_MES_CNTL=0x%08x", mesGp3, mesCntl);
    setProperty("MES,Gp3Lo", static_cast<uint64_t>(mesGp3), 32);
    setProperty("MES,Cntl", static_cast<uint64_t>(mesCntl), 32);

    // RISC-V CSRs of the MES microengine: MCAUSE/MEPC tell us whether the core
    // trapped instead of running the scheduler loop.
    grbmSelect(3, 0, 0, 0);
    const uint64_t mesMcause =
        (static_cast<uint64_t>(readGcRegister(0x281B, 1)) << 32) |
        readGcRegister(0x281A, 1);
    const uint64_t mesMepc =
        (static_cast<uint64_t>(readGcRegister(0x2819, 1)) << 32) |
        readGcRegister(0x2818, 1);
    const uint64_t mesMstatus =
        (static_cast<uint64_t>(readGcRegister(0x2817, 1)) << 32) |
        readGcRegister(0x2816, 1);
    const uint64_t mesBadaddr =
        (static_cast<uint64_t>(readGcRegister(0x281D, 1)) << 32) |
        readGcRegister(0x281C, 1);
    const uint64_t mesMtvec =
        (static_cast<uint64_t>(readGcRegister(0x2802, 1)) << 32) |
        readGcRegister(0x2801, 1);
    grbmSelect(0, 0, 0, 0);
    LOG("mesInit: MCAUSE=0x%llx MEPC=0x%llx MSTATUS=0x%llx BADADDR=0x%llx MTVEC=0x%llx",
        static_cast<unsigned long long>(mesMcause),
        static_cast<unsigned long long>(mesMepc),
        static_cast<unsigned long long>(mesMstatus),
        static_cast<unsigned long long>(mesBadaddr),
        static_cast<unsigned long long>(mesMtvec));
    setProperty("MES,Mcause", mesMcause, 64);
    setProperty("MES,Mepc", mesMepc, 64);
    setProperty("MES,Mstatus", mesMstatus, 64);
    setProperty("MES,Badaddr", mesBadaddr, 64);
    setProperty("MES,Mtvec", mesMtvec, 64);

    // --- Aggregated doorbells (mes_v12_0_init_aggregated_doorbell) ---
    for (uint32_t i = 0; i < kMesPriorityLevels; i++) {
        const uint32_t idx = kMesAggregatedDoorbellBase + i * 2;
        grbmSelect(3, 0, 0, 0);
        uint32_t v = readGcRegister(kRegMesDoorbellCtl1 + i, 1);
        if (v == 0xFFFFFFFF) {
            grbmSelect(0, 0, 0, 0);
            continue;
        }
        v &= ~((0x3FFFu << 2) | (1u << 30) | (1u << 31));
        v |= ((idx & 0x3FFFu) << 2) | (1u << 30);
        writeGcRegister(kRegMesDoorbellCtl1 + i, v, 1);
        grbmSelect(0, 0, 0, 0);
    }
    // Route GFX queue update messages to MES (reserved CP internal register).
    {
        uint32_t v = readGcRegister(kRegCpHqdGfxControl, 0);
        writeGcRegister(kRegCpHqdGfxControl, v | (1u << 15), 0);
    }
    setProperty("MES,AggDoorbellBase",
                static_cast<uint64_t>(kMesAggregatedDoorbellBase), 32);

    // --- SET_HW_RESOURCES: hand the scheduler its VMIDs and HQDs ---
    // mes_v12_0_kiq_hw_init() with uni-MES: SET_HW_RESOURCES (+_1) on the KIQ
    // pipe first, then mes_v12_0_hw_init() repeats it on the SCHED pipe.  The
    // ADD_QUEUE(map_legacy_kq) is served by the KIQ pipe, so that pipe must
    // have been configured — until now only the SCHED pipe was.
    bool hwRsrc = mesSetHwResources(1);
    setProperty("MES,HwRsrcKiq", static_cast<uint64_t>(hwRsrc ? 1 : 0), 32);
    if (hwRsrc && (mesGp3 & 0xFFFFu) >= 0x4Bu) {
        mesSetHwResources1(1);
    }
    const bool hwRsrcSched = mesSetHwResources(0);
    setProperty("MES,HwRsrc", static_cast<uint64_t>(hwRsrcSched ? 1 : 0), 32);
    hwRsrc = hwRsrc && hwRsrcSched;
    // SET_HW_RESOURCES_1 on the SCHED pipe stays behind a boot-arg: an
    // unsupported opcode there would stop the scheduler before ADD_QUEUE.
    uint32_t wantHwRsrc1 = 0;
    if (hwRsrcSched && PE_parse_boot_argn("rdna4-mes-hwr1", &wantHwRsrc1,
                                         sizeof(wantHwRsrc1)) && wantHwRsrc1 != 0 &&
        (mesGp3 & 0xFFFFu) >= 0x4Bu) {
        mesSetHwResources1(0);
    }

    // MEC liveness probe: MES cannot map a queue into a MEC block whose RS64
    // core is not executing, so check whether the MEC instruction pointer moves.
    grbmSelect(1, 0, 0, 0);
    const uint32_t mecPntrA = readGcRegister(0x2908, 1);
    IODelay(1000);
    const uint32_t mecPntrB = readGcRegister(0x2908, 1);
    const uint32_t mecCntl = readGcRegister(0x2904, 1);
    grbmSelect(0, 0, 0, 0);
    LOG("mesInit: MEC PNTR %u -> %u RS64_CNTL=0x%08x", mecPntrA, mecPntrB, mecCntl);
    setProperty("Mec,Pntr0", static_cast<uint64_t>(mecPntrA), 32);
    setProperty("Mec,Pntr1", static_cast<uint64_t>(mecPntrB), 32);
    setProperty("Mec,Rs64Cntl", static_cast<uint64_t>(mecCntl), 32);

    setProperty("MES,Stage", "initialized");
    setProperty("MES,UcStart", mesUcStart, 64);
    setProperty("MES,RingGPU0", mesRingGpuAddr[0], 64);
    setProperty("MES,RingGPU1", mesRingGpuAddr[1], 64);
    setProperty("MES,MqdGPU1", mesMqdGpuAddr[1], 64);
    setProperty("MES,StatusGPU", mesStatusGpuAddr, 64);
    return true;
}

bool RDNA4Driver::mesSubmitAndWait(uint32_t pipe, const uint32_t* pkt,
                                   uint32_t dwords, uint64_t statusGpuAddr) {
    if (pipe > 1 || !pkt || dwords == 0 || dwords + 64 > mesRingDwords[pipe]) {
        return false;
    }

    // Status/fence slots always come from the MES status page; a caller-supplied
    // address is ignored so the two alloc modes behave identically.
    statusGpuAddr = mesStatusGpuAddr;

    volatile uint32_t* ring = reinterpret_cast<volatile uint32_t*>(
        mesBufferCpu(mesRingMd[pipe], mesRingGpuAddr[pipe]));
    volatile uint8_t* statusBase = mesBufferCpu(mesStatusMd, mesStatusGpuAddr);
    if (!ring || !statusBase) {
        return false;
    }
    volatile uint64_t* api0 = reinterpret_cast<volatile uint64_t*>(statusBase);
    volatile uint64_t* qry =
        reinterpret_cast<volatile uint64_t*>(statusBase + 8);

    *qry = 0;
    *api0 = 0;

    uint32_t w = mesRingWrite[pipe];
    for (uint32_t i = 0; i < dwords; i++) {
        ring[w] = pkt[i];
        w = (w + 1) % mesRingDwords[pipe];
    }

    // QUERY_SCHEDULER_STATUS (mes_v12_api_def.h union MESAPI__QUERY_MES_STATUS)
    uint32_t query[64] = {};
    query[0] = 1u | (11u << 4) | (64u << 12);
    query[1] = 0;  // subopcode
    query[2] = static_cast<uint32_t>(statusGpuAddr + 8);
    query[3] = static_cast<uint32_t>((statusGpuAddr + 8) >> 32);
    query[4] = 1;  // fence value low
    query[5] = 0;
    for (uint32_t i = 0; i < 64; i++) {
        ring[w] = query[i];
        w = (w + 1) % mesRingDwords[pipe];
    }

    mesRingWrite[pipe] = w;

    // MES also polls the ring wptr from memory (MQD WPTR_POLL_ADDR).
    volatile uint32_t* wptrMem = reinterpret_cast<volatile uint32_t*>(
        mesBufferCpu(mesWpMd[pipe], mesWptrGpuAddr[pipe]));
    *wptrMem = w;

    gmcHDPFlush(*this);
    OSSynchronizeIO();

    doorbell[mesDoorbellIndex[pipe]] = w;
    OSSynchronizeIO();

    // Diagnostic: CP_HQD_PQ_WPTR latches the doorbell payload once
    // CP_HQD_PQ_DOORBELL_CONTROL.DOORBELL_EN is set and the index matches.
    // Sampling it separates "doorbell never arrived" from "MES never fetched".
    grbmSelect(3, pipe, 0, 0);
    uint32_t seenWptr = 0;
    for (uint32_t i = 0; i < 20; i++) {
        seenWptr = readGcRegister(0x1FDF, 0);
        if (seenWptr) break;
        IODelay(500);
    }
    if (!seenWptr) {
        // Register kick as a fallback: same HQD slot, bypasses the doorbell.
        writeGcRegister(0x1FDF, w, 0);
        writeGcRegister(0x1FE0, 0, 0);
        IODelay(500);
    }
    const uint32_t wptrReg = readGcRegister(0x1FDF, 0);
    const uint32_t rptrEarly = readGcRegister(0x1FB3, 0);
    const uint32_t dbCtlRb = readGcRegister(0x1FB8, 0);
    grbmSelect(0, 0, 0, 0);
    LOG("mesSubmitAndWait: pipe %u wptrReg=0x%x (doorbell latch 0x%x) rptr=0x%x dbCtl=0x%08x",
        pipe, wptrReg, seenWptr, rptrEarly, dbCtlRb);
    setProperty("MES,WptrAfterDb", static_cast<uint64_t>(seenWptr), 32);
    setProperty("MES,WptrRegAfterKick", static_cast<uint64_t>(wptrReg), 32);
    setProperty("MES,DbCtlAfterKick", static_cast<uint64_t>(dbCtlRb), 32);

    // GART proof: with NO_UPDATE_RPTR clear, the CP writes its read pointer to
    // the rptr page. If that page (a CPU mapping of a GART-mapped buffer) stays
    // zero while the register advanced, our page table is not being used.
    {
        volatile uint32_t* rptrMem = reinterpret_cast<volatile uint32_t*>(
            mesBufferCpu(mesWpMd[pipe], mesRptrGpuAddr[pipe]));
        const uint32_t rptrMemVal = *rptrMem;
        LOG("mesSubmitAndWait: pipe %u rptr page = 0x%x (register 0x%x)",
            pipe, rptrMemVal, rptrEarly);
        setProperty("MES,RptrMem", static_cast<uint64_t>(rptrMemVal), 32);
    }

    for (uint32_t i = 0; i < 1000; i++) {
        if (*qry == 1) {
            if ((static_cast<uint32_t>(*api0) & 0xFFFFFFFFu) != 0) {
                return true;
            }
        }
        IOSleep(1);
    }
    grbmSelect(3, pipe, 0, 0);
    const uint32_t rptr = readGcRegister(0x1FB3, 0);
    const uint32_t wptrLo = readGcRegister(0x1FDF, 0);
    const uint32_t active = readGcRegister(0x1FAB, 0);
    grbmSelect(0, 0, 0, 0);
    volatile uint64_t* api = api0;
    setProperty("MES,PqRptr", static_cast<uint64_t>(rptr), 32);
    setProperty("MES,PqWptr", static_cast<uint64_t>(wptrLo), 32);
    setProperty("MES,PqActive", static_cast<uint64_t>(active), 32);
    setProperty("MES,QryStatus", *qry, 64);
    setProperty("MES,ApiStatus", *api, 64);
    LOG_ERROR("mesSubmitAndWait: pipe %u timed out (qry=0x%llx api=0x%llx rptr=%u wptr=%u active=0x%x)",
              pipe, static_cast<unsigned long long>(*qry),
              static_cast<unsigned long long>(*api), rptr, wptrLo, active);

    // GMC fault status: a page-table walk that cannot resolve shows up here
    // (GCVM 0x15d0..0x15d3, MMVM 0x04f0..0x04f3).
    {
        const struct {
            IPDiscovery::HwId hub;
            uint32_t stLo, stHi, adLo, adHi;
            const char* key;
        } hubs[] = {
            { IPDiscovery::HwGc, 0x15D0, 0x15D1, 0x15D2, 0x15D3, "Gfx" },
            { IPDiscovery::HwMmhub, 0x04F0, 0x04F1, 0x04F2, 0x04F3, "Mmh" },
        };
        for (const auto& h : hubs) {
            uint32_t off = 0;
            uint64_t status = 0xFFFFFFFFFFFFFFFFull;
            uint64_t addr = 0;
            if (discovery().regByteOffset(h.hub, 0, 0, h.stLo, off)) {
                const uint64_t lo = readRegister(off);
                const uint64_t hi = discovery().regByteOffset(h.hub, 0, 0, h.stHi, off)
                                        ? readRegister(off) : 0;
                status = (hi << 32) | lo;
            }
            if (discovery().regByteOffset(h.hub, 0, 0, h.adLo, off)) {
                const uint64_t lo = readRegister(off);
                const uint64_t hi = discovery().regByteOffset(h.hub, 0, 0, h.adHi, off)
                                        ? readRegister(off) : 0;
                addr = (hi << 32) | lo;
            }
            LOG_ERROR("mesSubmitAndWait: %sVM fault status=0x%llx addr=0x%llx",
                      h.key, static_cast<unsigned long long>(status),
                      static_cast<unsigned long long>(addr));
            if (h.hub == IPDiscovery::HwGc) {
                setProperty("MES,GfxFault", status, 64);
                setProperty("MES,GfxFaultAddr", addr, 64);
            } else {
                setProperty("MES,MmhFault", status, 64);
                setProperty("MES,MmhFaultAddr", addr, 64);
            }
        }
    }
    return false;
}

bool RDNA4Driver::mesMapLegacyQueue(uint32_t pipe, uint32_t queue,
                                    uint32_t doorbellOffset, uint64_t mqdAddr,
                                    uint64_t wptrAddr, uint64_t statusAddr,
                                    uint32_t queueType) {
    (void)statusAddr;
    uint32_t pkt[64] = {};

    pkt[0] = 1u | (2u << 4) | (64u << 12);  // type=scheduler, op=ADD_QUEUE, 64 dwords
    pkt[18] = doorbellOffset;
    pkt[20] = static_cast<uint32_t>(mqdAddr);
    pkt[21] = static_cast<uint32_t>(mqdAddr >> 32);
    pkt[22] = static_cast<uint32_t>(wptrAddr);
    pkt[23] = static_cast<uint32_t>(wptrAddr >> 32);
    pkt[28] = queueType;     // 0 = GFX, 1 = COMPUTE, 2 = SDMA
    pkt[37] = (1u << 13);    // map_legacy_kq
    // The completion fence must land on the page mesSubmitAndWait polls
    // (mesStatusGpuAddr). A caller-supplied address makes a successful
    // ADD_QUEUE look like a timeout.
    pkt[38] = static_cast<uint32_t>(mesStatusGpuAddr);
    pkt[39] = static_cast<uint32_t>(mesStatusGpuAddr >> 32);
    pkt[40] = 1;             // api completion fence value
    pkt[41] = 0;
    pkt[50] = pipe;
    pkt[51] = queue;

    LOG("mesMapLegacyQueue: queuePipe=%u queue=%u db=%u mqd=0x%llx wptr=0x%llx",
        pipe, queue, doorbellOffset,
        static_cast<unsigned long long>(mqdAddr),
        static_cast<unsigned long long>(wptrAddr));

    // mes_v12_0_map_legacy_queue(): with uni-MES the ADD_QUEUE for a *compute*
    // queue goes to the KIQ pipe (1).  A GFX queue belongs to the scheduler
    // pipe (0): the KIQ acks the request and then leaves the GFX HQD
    // un-activated, which looks like success until nothing executes.
    bool ok;
    if (queueType == 0) {
        ok = mesSubmitAndWait(0, pkt, 64, statusAddr);
        setProperty("MES,SchedPipeOk", static_cast<uint64_t>(ok ? 1 : 0), 32);
        if (!ok) ok = mesSubmitAndWait(1, pkt, 64, statusAddr);
        setProperty("MES,KiqPipeOk", static_cast<uint64_t>(ok ? 1 : 0), 32);
    } else {
        ok = mesSubmitAndWait(1, pkt, 64, statusAddr);
        setProperty("MES,KiqPipeOk", static_cast<uint64_t>(ok ? 1 : 0), 32);
        if (!ok) {
            ok = mesSubmitAndWait(0, pkt, 64, statusAddr);
        }
        setProperty("MES,SchedPipeOk", static_cast<uint64_t>(ok ? 1 : 0), 32);
    }
    setProperty("MES,MapQueueOk", static_cast<uint64_t>(ok ? 1 : 0), 32);

    if (!ok) {
        // First: is the scheduler still alive after the failed ADD_QUEUE? A
        // QUERY-only frame answers that directly — if it also times out, the
        // ADD_QUEUE wedged the firmware rather than being rejected.
        uint32_t q[64] = {};
        q[0] = 1u | (11u << 4) | (64u << 12);
        q[2] = static_cast<uint32_t>(mesStatusGpuAddr);
        q[3] = static_cast<uint32_t>(mesStatusGpuAddr >> 32);
        q[4] = 1;
        q[5] = 0;
        const bool alive = mesSubmitAndWait(0, q, 64, mesStatusGpuAddr);
        setProperty("MES,AliveAfterAdd", static_cast<uint64_t>(alive ? 1 : 0), 32);
        LOG_ERROR("ADD_QUEUE: scheduler alive after failure = %u", alive ? 1 : 0);

        // RISC-V CSRs *after* the failed ADD_QUEUE (the init-time values were
        // taken before any queue was mapped and are always clean).
        grbmSelect(3, 0, 0, 0);
        const uint64_t cause =
            (static_cast<uint64_t>(readGcRegister(0x281B, 1)) << 32) |
            readGcRegister(0x281A, 1);
        const uint64_t epc =
            (static_cast<uint64_t>(readGcRegister(0x2819, 1)) << 32) |
            readGcRegister(0x2818, 1);
        const uint64_t badaddr =
            (static_cast<uint64_t>(readGcRegister(0x281D, 1)) << 32) |
            readGcRegister(0x281C, 1);
        const uint32_t pntrNow = readGcRegister(0x2813, 1);
        grbmSelect(0, 0, 0, 0);
        LOG_ERROR("ADD_QUEUE: MCAUSE=0x%llx MEPC=0x%llx MBADADDR=0x%llx PNTR=0x%x",
                  static_cast<unsigned long long>(cause),
                  static_cast<unsigned long long>(epc),
                  static_cast<unsigned long long>(badaddr), pntrNow);
        setProperty("MES,FailMcause", cause, 64);
        setProperty("MES,FailMepc", epc, 64);
        setProperty("MES,FailBadaddr", badaddr, 64);
        setProperty("MES,FailPntr", static_cast<uint64_t>(pntrNow), 32);

        // Export the firmware's own event history: the first 64 dwords of the
        // log page and of the MSCRATCH page.  Anything non-zero here is MES
        // describing what it did with our ADD_QUEUE.
        if (mesEventLogGpuAddr) {
            const volatile uint32_t* lg = reinterpret_cast<const volatile uint32_t*>(
                mesBufferCpu(nullptr, mesEventLogGpuAddr));
            const volatile uint32_t* sc = reinterpret_cast<const volatile uint32_t*>(
                mesBufferCpu(nullptr, mesEventLogGpuAddr + 0xC000));
            OSData* logData = OSData::withCapacity(64 * 4);
            OSData* scData = OSData::withCapacity(64 * 4);
            uint32_t nzLog = 0, nzSc = 0;
            for (uint32_t i = 0; i < 64; i++) {
                const uint32_t a = lg[i], b = sc[i];
                if (a) nzLog++;
                if (b) nzSc++;
                if (logData) logData->appendBytes(&a, 4);
                if (scData) scData->appendBytes(&b, 4);
            }
            if (logData) { setProperty("MES,EventLog64", logData); logData->release(); }
            if (scData) { setProperty("MES,Scratch64", scData); scData->release(); }
            setProperty("MES,EventLogNonZero", static_cast<uint64_t>(nzLog), 32);
            setProperty("MES,ScratchNonZero", static_cast<uint64_t>(nzSc), 32);
            LOG_ERROR("ADD_QUEUE: event log nonzero=%u scratch nonzero=%u", nzLog, nzSc);
        }

        // Did MES get as far as programming the compute HQD (ME1/pipe0/queue0)?
        // If these are untouched the failure happens before the queue is mapped,
        // not during dispatch.
        grbmSelect(1, 0, 0, 0);
        const uint32_t act = readGcRegister(0x1FAB, 0);
        const uint32_t mqdBase = readGcRegister(0x1FA9, 0);
        const uint32_t pqBase = readGcRegister(0x1FB1, 0);
        const uint32_t pqCtl = readGcRegister(0x1FBA, 0);
        grbmSelect(0, 0, 0, 0);
        LOG_ERROR("ADD_QUEUE: MEC HQD active=0x%x mqdBase=0x%x pqBase=0x%x pqCtl=0x%x",
                  act, mqdBase, pqBase, pqCtl);
        setProperty("MES,CompHqdActive", static_cast<uint64_t>(act), 32);
        setProperty("MES,CompMqdBase", static_cast<uint64_t>(mqdBase), 32);
        setProperty("MES,CompPqBase", static_cast<uint64_t>(pqBase), 32);
        setProperty("MES,CompPqCtl", static_cast<uint64_t>(pqCtl), 32);

        // Full CP/MEC state at the moment of failure — reuses the queue dump
        // (CP_STAT, GRBM_STATUS, RS64 cache bases, MEC PC, HQD regs).
        dumpComputeQueueState();
    }
    return ok;
}

bool RDNA4Driver::mesSetHwResources(uint32_t pipe) {
    if (!mesSchCtxGpuAddr) {
        mesSchCtxGpuAddr = allocateVRAMBuffer(kMesSchCtxBytes);
    }
    if (!mesStatusFenceGpuAddr) {
        mesStatusFenceGpuAddr = allocateVRAMBuffer(4096);
    }
    if (!mesSchCtxGpuAddr || !mesStatusFenceGpuAddr) {
        LOG_ERROR("mesSetHwResources: scheduler context/fence allocation failed");
        setProperty("MES,HwRsrc", static_cast<uint64_t>(0), 32);
        return false;
    }
    {
        volatile uint8_t* p = mesBufferCpu(mesSchCtxMd, mesSchCtxGpuAddr);
        for (uint32_t i = 0; i < kMesSchCtxBytes; i++) p[i] = 0;
        p = mesBufferCpu(mesStatusFenceMd, mesStatusFenceGpuAddr);
        for (uint32_t i = 0; i < 64; i++) p[i] = 0;
    }
    gmcInvalidateTlb(*this);

    uint32_t pkt[64] = {};
    pkt[0] = 1u | (0u << 4) | (64u << 12);   // MES_SCH_API_SET_HW_RSRC
    pkt[1] = 0xFE;    // vmid_mask_mmhub: VMID 0 stays kernel-only
    pkt[2] = 0xFE;    // vmid_mask_gfxhub
    pkt[3] = 0;       // gds_size
    pkt[4] = 0;       // paging_vmid
    // compute_hqd_mask[8]: queues 0/1 of every MEC pipe stay kernel-owned.
    for (uint32_t i = 0; i < 8; i++)
        pkt[5 + i] = (i < 4) ? 0xFCu : 0u;
    pkt[13] = 0xFE;   // gfx_hqd_mask[0]
    pkt[14] = 0x00;   // gfx_hqd_mask[1]
    pkt[15] = 0x0F;   // sdma_hqd_mask[0]
    pkt[16] = 0x0F;   // sdma_hqd_mask[1]
    for (uint32_t i = 0; i < kMesPriorityLevels; i++)
        pkt[17 + i] = kMesAggregatedDoorbellBase + i * 2;

    pkt[22] = static_cast<uint32_t>(mesSchCtxGpuAddr);
    pkt[23] = static_cast<uint32_t>(mesSchCtxGpuAddr >> 32);
    pkt[24] = static_cast<uint32_t>(mesStatusFenceGpuAddr);
    pkt[25] = static_cast<uint32_t>(mesStatusFenceGpuAddr >> 32);

    // MES resolves its own registers as base[segment] + offset, so the packet
    // must carry every segment base the firmware will touch.
    IPDiscovery::IpEntry gc {}, mmhub {}, osssys {};
    const IPDiscovery& disc = discovery();
    const bool haveGc = disc.findIp(IPDiscovery::HwGc, 0, gc);
    const bool haveMmhub = disc.findIp(IPDiscovery::HwMmhub, 0, mmhub);
    const bool haveOsssys = disc.findIp(IPDiscovery::HwOsssys, 0, osssys);
    for (uint32_t i = 0; i < 5; i++) {
        pkt[26 + i] = (haveGc && i < gc.numBases) ? gc.bases[i] : 0u;
        pkt[34 + i] = (haveMmhub && i < mmhub.numBases) ? mmhub.bases[i] : 0u;
        pkt[42 + i] = (haveOsssys && i < osssys.numBases) ? osssys.bases[i] : 0u;
    }

    pkt[50] = static_cast<uint32_t>(mesStatusGpuAddr);
    pkt[51] = static_cast<uint32_t>(mesStatusGpuAddr >> 32);
    pkt[52] = 1;      // api completion fence value
    pkt[53] = 0;
    // disable_reset | use_different_vmid_compute | enable_level_process_quantum_check
    // | enable_mes_event_int_logging | enable_reg_active_poll
    // | unmapped_doorbell_handling(BASIC).  disable_mes_log is left clear so the
    // firmware's own event history (event_intr_history_gpu_mc_ptr) is written.
    pkt[54] = (1u << 0) | (1u << 1) | (1u << 6) | (1u << 9) | (1u << 10) |
              (1u << 19);
    pkt[55] = 50;     // oversubscription_timer
    pkt[58] = static_cast<uint32_t>(mesEventLogGpuAddr);        // event_intr_history
    pkt[59] = static_cast<uint32_t>(mesEventLogGpuAddr >> 32);

    LOG("mesSetHwResources: vmid=0x%x compute=0x%x gfx=0x%x sdma=0x%x "
        "gc0=0x%x mmhub0=0x%x osssys0=0x%x",
        pkt[1], pkt[5], pkt[13], pkt[15], pkt[26], pkt[34], pkt[42]);
    setProperty("MES,HwRsrcGcBase", static_cast<uint64_t>(pkt[26]), 32);
    setProperty("MES,HwRsrcMmhubBase", static_cast<uint64_t>(pkt[34]), 32);

    const bool ok = mesSubmitAndWait(pipe, pkt, 64, mesStatusGpuAddr);
    if (!ok) {
        LOG_ERROR("mesSetHwResources: pipe %u did not acknowledge SET_HW_RESOURCES", pipe);
    }
    return ok;
}

bool RDNA4Driver::mesSetHwResources1(uint32_t pipe) {
    if (!mesResource1GpuAddr) {
        mesResource1GpuAddr = allocateVRAMBuffer(4096);
        if (!mesResource1GpuAddr) {
            LOG_ERROR("mesSetHwResources1: cleaner shader fence allocation failed");
            return false;
        }
        volatile uint8_t* p = mesBufferCpu(nullptr, mesResource1GpuAddr);
        for (uint32_t i = 0; i < 4096; i++) p[i] = 0;
    }

    uint32_t pkt[64] = {};
    pkt[0] = 1u | (19u << 4) | (64u << 12);   // MES_SCH_API_SET_HW_RSRC_1
    pkt[2] = static_cast<uint32_t>(mesStatusGpuAddr);       // api_status
    pkt[3] = static_cast<uint32_t>(mesStatusGpuAddr >> 32);
    pkt[4] = 1;                                            // fence value
    pkt[5] = 0;
    pkt[13] = 0xAu;                                        // mes_kiq_unmap_timeout
    pkt[16] = static_cast<uint32_t>(mesResource1GpuAddr);   // cleaner_shader_fence
    pkt[17] = static_cast<uint32_t>(mesResource1GpuAddr >> 32);

    const bool ok = mesSubmitAndWait(pipe, pkt, 64, mesStatusGpuAddr);
    setProperty(pipe ? "MES,HwRsrc1Kiq" : "MES,HwRsrc1",
                static_cast<uint64_t>(ok ? 1 : 0), 32);
    if (!ok) {
        LOG_ERROR("mesSetHwResources1: pipe %u did not acknowledge SET_HW_RESOURCES_1", pipe);
    }
    return ok;
}
