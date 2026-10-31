//
//  GraphicsQueue.cpp
//  RDNA4Driver - GFX queue bring-up on the MES scheduler
//
//  gfx12 has no usable legacy CP_RB0 path: the RS64 firmware halts itself on
//  the first doorbell. The kernel GFX queue is brought up the way the upstream
//  gfx_v12_0 driver does it - build a v12_gfx_mqd, have the MES map it with
//  ADD_QUEUE(map_legacy_kq) for queue_type GFX, then submit through the ring.
//

#include "../core/RDNA4Driver.hpp"
#include "../hardware/GMCContext.hpp"
#include "../hardware/GraphicsPM4Builder.hpp"
#include "../hardware/MesApi.hpp"
#include "../hardware/PM4BuilderKernel.hpp"

#include <IOKit/IOLib.h>
#include <string.h>

namespace {

// v12_gfx_mqd: 512 dwords. Only the fields the CP firmware consumes are set;
// everything else stays zero, which is what the reference init leaves.
enum : uint32_t {
    kMqdBaseAddrLo       = 128,
    kMqdBaseAddrHi       = 129,
    kGfxHqdActive        = 130,
    kGfxHqdVmid          = 131,
    kGfxHqdQueuePriority = 134,
    kGfxHqdQuantum       = 135,
    kGfxHqdBase          = 136,
    kGfxHqdBaseHi        = 137,
    kGfxHqdRptr          = 138,
    kGfxHqdRptrAddr      = 139,
    kGfxHqdRptrAddrHi    = 140,
    kRbWptrPollAddrLo    = 141,
    kRbWptrPollAddrHi    = 142,
    kRbDoorbellControl   = 143,
    kGfxHqdOffset        = 144,
    kGfxHqdCntl          = 145,
    kGfxHqdWptr          = 149,
    kGfxHqdMapped        = 156,
    kGfxHqdQueMgrControl = 157,
    kGfxMqdControl       = 162,
};

constexpr uint32_t kDefaultGfxMqdControl = 0x00000100u;  // PRIV_STATE = 1
constexpr uint32_t kDefaultGfxHqdQuantum = 0x00000a01u;  // QUANTUM_EN

constexpr uint32_t kGfxRingBytes = 64u * 1024u;   // 16384 dwords
constexpr uint32_t kNggSentinel = 0xD35D0001u;
// mmGB_ADDR_CONFIG: gc_12_0_0_offset.h dword offset (the gfx12 JSON keeps the
// same dword numbering; 0x263E is the older gfx9 value).
// gc_12_0_0 mm offsets. The gfx12 register JSON lists byte addresses (0x98F8
// for GB_ADDR_CONFIG); these are the dword offsets the MMIO helper takes.
constexpr uint32_t kRegGbAddrConfig = 0x13DEu;   // regGB_ADDR_CONFIG (gc_12_0_0_offset.h, BASE_IDX 0)
constexpr uint32_t kRegCcRbBackendDisable = 0x13DDu;  // regCC_RB_BACKEND_DISABLE (gc_12_0_0_offset.h)
constexpr uint32_t kCpRb0Rptr    = 0x0F60u;       // gc_12_0_0_offset.h
constexpr uint32_t kCpMaxContext  = 0x1E4Eu;       // gc_12_0_0_offset.h:105
constexpr uint32_t kCpDeviceId    = 0x1DEBu;       // gc_12_0_0_offset.h:94

// log2(dwords) - 1, the RB_BUFSZ/RB_BLKSZ encoding the firmware expects.
uint32_t ringBufszFor(uint32_t dwords) {
    uint32_t log2 = 0;
    while ((1u << log2) < dwords) log2++;
    return log2 - 1u;
}

} // namespace

bool RDNA4Driver::initGraphicsRing() {
    if (!vram || vramSize == 0 || !doorbell) {
        LOG_ERROR("initGraphicsRing: no VRAM or doorbell aperture");
        return false;
    }
    // The scheduler that maps this queue is the one the compute path started.
    if (!computeRing) {
        LOG_ERROR("initGraphicsRing: MES is not up (compute ring missing)");
        return false;
    }

    if (graphicsRing) {
        delete graphicsRing;
        graphicsRing = nullptr;
    }

    const uint64_t ringGpu = allocateVRAMBuffer(kGfxRingBytes);
    const uint64_t fenceGpu = allocateVRAMBuffer(4096);
    // The CP reports its read pointer here; it must be its own page so the
    // write-back does not share cache lines with the ring itself.
    graphicsRptrGpuAddr = allocateVRAMBuffer(4096);
    const uint64_t statusGpu = allocateVRAMBuffer(4096);
    if (!ringGpu || !fenceGpu || !graphicsRptrGpuAddr || !statusGpu) {
        LOG_ERROR("initGraphicsRing: VRAM allocation failed");
        return false;
    }
    if (ringGpu < gmcContext.vramStart) {
        LOG_ERROR("initGraphicsRing: ring address below the FB aperture");
        return false;
    }
    graphicsRingGpuAddr = ringGpu;
    const uint64_t rptrOffset = graphicsRptrGpuAddr - gmcContext.vramStart;
    if (rptrOffset > vramSize || 4096 > vramSize - rptrOffset) {
        LOG_ERROR("initGraphicsRing: rptr page outside VRAM");
        return false;
    }
    memset(const_cast<uint8_t*>(vram) + rptrOffset, 0, 4096);

    graphicsRing = new GraphicsRing();
    if (!graphicsRing ||
        !graphicsRing->init(vram, vramSize, doorbell, graphicsDoorbellIndex,
                            ringGpu - gmcContext.vramStart, kGfxRingBytes,
                            fenceGpu - gmcContext.vramStart)) {
        LOG_ERROR("initGraphicsRing: ring init failed");
        delete graphicsRing;
        graphicsRing = nullptr;
        return false;
    }

    // MES reads the MQD and polls the write pointer through the GART aperture,
    // so both are system buffers rather than VRAM.
    const uint64_t mqdGpu = gmcAllocGartBuffer(*this, gmcContext, 4096, &graphicsMqdMd);
    const uint64_t wptrGpu = gmcAllocGartBuffer(*this, gmcContext, 4096, &graphicsWptrMd);
    if (!mqdGpu || !wptrGpu || !graphicsMqdMd || !graphicsWptrMd) {
        LOG_ERROR("initGraphicsRing: GART allocation failed (mqd=0x%llx wptr=0x%llx)",
                  static_cast<unsigned long long>(mqdGpu),
                  static_cast<unsigned long long>(wptrGpu));
        return false;
    }
    graphicsMqdGpuAddr = mqdGpu;
    graphicsWptrGpuAddr = wptrGpu;
    *static_cast<volatile uint32_t*>(graphicsWptrMd->getBytesNoCopy()) = 0;

    uint32_t* mqd = static_cast<uint32_t*>(graphicsMqdMd->getBytesNoCopy());
    memset(mqd, 0, 512 * sizeof(uint32_t));

    mqd[kMqdBaseAddrLo] = static_cast<uint32_t>(mqdGpu & 0xFFFFFFFCu);
    mqd[kMqdBaseAddrHi] = static_cast<uint32_t>(mqdGpu >> 32);
    mqd[kGfxMqdControl] = kDefaultGfxMqdControl;   // VMID 0, PRIV_STATE 1
    mqd[kGfxHqdVmid] = 0;
    mqd[kGfxHqdQueuePriority] = 0;
    mqd[kGfxHqdQuantum] = kDefaultGfxHqdQuantum;

    // CP_GFX_HQD_BASE / _HI hold the ring address >> 8 (gfx_v12_0_gfx_mqd_init).
    const uint64_t hqdBase = ringGpu >> 8;
    mqd[kGfxHqdBase] = static_cast<uint32_t>(hqdBase);
    mqd[kGfxHqdBaseHi] = static_cast<uint32_t>(hqdBase >> 32);

    mqd[kGfxHqdRptrAddr] = static_cast<uint32_t>(graphicsRptrGpuAddr & 0xFFFFFFFCu);
    mqd[kGfxHqdRptrAddrHi] = static_cast<uint32_t>(graphicsRptrGpuAddr >> 32) & 0xFFFFu;
    mqd[kRbWptrPollAddrLo] = static_cast<uint32_t>(wptrGpu & 0xFFFFFFFCu);
    mqd[kRbWptrPollAddrHi] = static_cast<uint32_t>(wptrGpu >> 32) & 0xFFFFu;

    graphicsRbBufsz = ringBufszFor(kGfxRingBytes / 4u);
    mqd[kGfxHqdCntl] = MesApi::GfxHqd::cntlValue(graphicsRbBufsz);

    mqd[kRbDoorbellControl] = ((graphicsDoorbellIndex & 0x3FFFu) << 2) | (1u << 30);
    mqd[kGfxHqdOffset] = 0;
    mqd[kGfxHqdRptr] = 0;
    // gfx_v12_0_gfx_mqd_init(): a kernel GFX queue is handed to the MES with
    // ACTIVE already set (only compute MQDs leave it clear for map_queues).
    mqd[kGfxHqdActive] = 1;

    // These two GART pages are mapped after mesInit()'s last TLB invalidate,
    // so publish them before the firmware reads the MQD.  (0.8.2 on hardware:
    // this alone did not change the outcome - the HQD still comes up with
    // ACTIVE and the doorbell from the packet but BASE/CNTL/MAPPED zero.)
    gmcHDPFlush(*this);
    gmcInvalidateTlb(*this);

    // Record firmware writeback. An unchanged MQD does not prove that the
    // firmware never read it; correlate this with live HQD/fault snapshots.
    uint32_t mqdBefore[512];
    memcpy(mqdBefore, mqd, sizeof(mqdBefore));

    snapshotGraphicsQueue("BeforeMap");
    graphicsQueueMapped = mesMapLegacyQueue(0, 0, graphicsDoorbellIndex, mqdGpu,
                                            wptrGpu, statusGpu, MesApi::kQueueTypeGfx);
    setProperty("GraphicsQueue,Mapped", static_cast<uint64_t>(graphicsQueueMapped ? 1 : 0), 32);
    setProperty("GraphicsQueue,RingGpu", ringGpu, 64);
    setProperty("GraphicsQueue,MqdGpu", mqdGpu, 64);
    setProperty("GraphicsQueue,DoorbellIndex", graphicsDoorbellIndex, 32);
    setProperty("GraphicsQueue,RbBufsz", graphicsRbBufsz, 32);
    LOG("initGraphicsRing: ring=0x%llx doorbell=%u bufsz=%u mapped=%u",
        static_cast<unsigned long long>(ringGpu), graphicsDoorbellIndex,
        graphicsRbBufsz, graphicsQueueMapped ? 1u : 0u);
    if (!graphicsQueueMapped) return false;

    writeGcRegister(0x2040, 0xCAFEDEAD, 1);
    setProperty("GraphicsQueue,DirectScratchReadback",
                static_cast<uint64_t>(readGcRegister(0x2040, 1)), 32);
    writeGcRegister(0xA10F, 0x12345678, 1);
    setProperty("GraphicsQueue,DirectViewportReadback",
                static_cast<uint64_t>(readGcRegister(0xA10F, 1)), 32);

    // What the firmware wrote back into the MQD is the proof it consumed it.
    gmcHDPFlush(*this);
    const volatile uint32_t* m = static_cast<const volatile uint32_t*>(
        graphicsMqdMd->getBytesNoCopy());
    setProperty("GraphicsQueue,MqdActive", static_cast<uint64_t>(m[kGfxHqdActive]), 32);
    setProperty("GraphicsQueue,MqdMapped", static_cast<uint64_t>(m[kGfxHqdMapped]), 32);
    exportMqdDiff("GraphicsQueue", mqdBefore, m, 512);

    // gfx_v12_0_cp_gfx_start() runs right after the MES maps the queue: the CP
    // is already running on the async path, but it still needs its context
    // count and device id before it will fetch from a mapped HQD. GRBM routing
    // must be set to me0/pipe0/queue0/vmid0 *before* writing these registers,
    // or the writes land nowhere (the root cause of "PM4 writes don't stick").
    grbmSelect(0, 0, 0, 0);
    writeGcRegister(kCpMaxContext, 8u - 1u, 0);
    writeGcRegister(kCpDeviceId, 1u, 0);
    gmcHDPFlush(*this);
    // The firmware programs the GFX HQD block directly; reading it back is how
    // we tell "the MES applied the queue" from "the MES only acked the API".
    // The GFX HQD lives on me0/pipe0/queue0 (me1 is the PFP, which has no HQD
    // of its own - reading there returns zeros and looks like "not loaded").
    // The whole CP_GFX_HQD_* block is BASE_IDX 0 (gc_12_0_0_offset.h).
    graphicsHqdActive = readGcRegister(MesApi::GfxHqd::kActive, MesApi::GfxHqd::kSegment);
    setProperty("GraphicsQueue,HqdActiveReg", static_cast<uint64_t>(graphicsHqdActive), 32);

    setProperty("GraphicsQueue,HqdMappedReg",
                static_cast<uint64_t>(readGcRegister(MesApi::GfxHqd::kMapped, MesApi::GfxHqd::kSegment)), 32);
    setProperty("GraphicsQueue,HqdBaseReg",
                static_cast<uint64_t>(readGcRegister(MesApi::GfxHqd::kBase, MesApi::GfxHqd::kSegment)), 32);
    setProperty("GraphicsQueue,HqdWptrReg",
                static_cast<uint64_t>(readGcRegister(MesApi::GfxHqd::kWptr, MesApi::GfxHqd::kSegment)), 32);
    setProperty("GraphicsQueue,HqdCntlReg",
                static_cast<uint64_t>(readGcRegister(MesApi::GfxHqd::kCntl, MesApi::GfxHqd::kSegment)), 32);
    setProperty("GraphicsQueue,HqdDoorbellReg",
                static_cast<uint64_t>(readGcRegister(MesApi::GfxHqd::kDoorbellControl, MesApi::GfxHqd::kSegment)), 32);
    setProperty("GraphicsQueue,MaxContext", static_cast<uint64_t>(readGcRegister(kCpMaxContext, 0)), 32);
    setProperty("GraphicsQueue,DeviceId", static_cast<uint64_t>(readGcRegister(kCpDeviceId, 0)), 32);

    // Did the firmware hand the MQD to the CP at all?  CP_GFX_MQD_BASE_ADDR
    // should hold (mqd_addr & ~3) once it starts the HQD load.  Alongside it
    // the legacy RB registers, which is where a *legacy* kernel queue would
    // have its ring programmed if the firmware took that route instead.
    setProperty("GraphicsQueue,GfxMqdBaseReg",
                (static_cast<uint64_t>(readGcRegister(0x1E7F, 0)) << 32) |
                    readGcRegister(0x1E7E, 0), 64);
    setProperty("GraphicsQueue,GfxMqdControlReg",
                static_cast<uint64_t>(readGcRegister(0x1E9A, 0)), 32);
    setProperty("GraphicsQueue,Rb0Base",
                (static_cast<uint64_t>(readGcRegister(0x1E51, 0)) << 32) |
                    readGcRegister(0x1DE0, 0), 64);
    setProperty("GraphicsQueue,Rb0CntlReg", static_cast<uint64_t>(readGcRegister(0x1DE1, 0)), 32);
    setProperty("GraphicsQueue,RbActiveReg", static_cast<uint64_t>(readGcRegister(0x1F40, 0)), 32);
    setProperty("GraphicsQueue,RbVmidReg", static_cast<uint64_t>(readGcRegister(0x1DF1, 0)), 32);
    setProperty("GraphicsQueue,Rb0RptrAddr",
                (static_cast<uint64_t>(readGcRegister(0x1DE4, 0)) << 32) |
                    readGcRegister(0x1DE3, 0), 64);
    // Which HQD slot did the firmware actually use?  gfx_hqd_mask[0] = 0xFE
    // tells MES it owns queues 1..7 of pipe 0, so a legacy queue could be
    // placed on one of those instead of queue 0 even though ADD_QUEUE asked
    // for pipe 0 / queue 0.
    for (uint32_t q = 0; q < 8; q++) {
        grbmSelect(0, 0, q, 0);
        char key[48];
        snprintf(key, sizeof(key), "GraphicsQueue,HqdActiveQ%u", q);
        setProperty(key, static_cast<uint64_t>(readGcRegister(MesApi::GfxHqd::kActive, 0)), 32);
        snprintf(key, sizeof(key), "GraphicsQueue,HqdBaseQ%u", q);
        setProperty(key, static_cast<uint64_t>(readGcRegister(MesApi::GfxHqd::kBase, 0)), 32);
        snprintf(key, sizeof(key), "GraphicsQueue,HqdMappedQ%u", q);
        setProperty(key, static_cast<uint64_t>(readGcRegister(MesApi::GfxHqd::kMapped, 0)), 32);
    }
    grbmSelect(0, 0, 0, 0);
    // The MQD we handed over, for comparison with CP_GFX_MQD_BASE_ADDR.
    setProperty("GraphicsQueue,MqdBaseAddrField",
                (static_cast<uint64_t>(m[kMqdBaseAddrHi]) << 32) | m[kMqdBaseAddrLo], 64);
    setProperty("GraphicsQueue,MqdHqdBaseField",
                (static_cast<uint64_t>(m[kGfxHqdBaseHi]) << 32) | m[kGfxHqdBase], 64);
    setProperty("GraphicsQueue,MqdHqdCntlField", static_cast<uint64_t>(m[kGfxHqdCntl]), 32);
    setProperty("GraphicsQueue,MqdDoorbellField", static_cast<uint64_t>(m[kRbDoorbellControl]), 32);

    // CP_RB0_RPTR is the ring-0 read pointer: once the firmware has taken the
    // queue it tracks the ring instead of sitting at its reset value.
    setProperty("GraphicsQueue,Rptr", static_cast<uint64_t>(readGcRegister(kCpRb0Rptr, 0)), 32);
    // The NGG rings belong to the pipeline, so they live in driver VRAM rather
    // than the client window. Clients query the base through GetNggRing.
    if (!nggRingGpuAddr) {
        // The SPI/GE ring base registers carry bits [47:16], so the ring must
        // start on a 64 KiB boundary; allocate the slack and round up.
        const uint64_t raw = allocateVRAMBuffer(GFX12::kNggRingTotal + 0x10000u);
        const uint64_t aligned = (raw + 0xFFFFu) & ~static_cast<uint64_t>(0xFFFFu);
        if (raw && aligned >= gmcContext.vramStart) {
            nggRingGpuAddr = aligned;
            nggRingSize = GFX12::kNggRingTotal;
            const uint64_t offset = aligned - gmcContext.vramStart;
            if (offset < vramSize && nggRingSize <= vramSize - offset) {
                memset(const_cast<uint8_t*>(vram) + offset, 0, nggRingSize);
            }
            gmcHDPFlush(*this);
        } else {
            LOG_ERROR("initGraphicsRing: NGG ring allocation failed");
        }
    }
    setProperty("GraphicsQueue,NggRingGpu", nggRingGpuAddr, 64);
    setProperty("GraphicsQueue,NggRingSize", static_cast<uint64_t>(nggRingSize), 32);

    // Indirect-buffer storage for graphics work. Two 64 KiB slots, 256 B
    // aligned (the packet keeps addr >> 2).
    if (!graphicsIbGpuAddr) {
        constexpr uint32_t kSlotBytes = 64u * 1024u;
        constexpr uint32_t kSlots = 2u;
        const uint64_t raw = allocateVRAMBuffer(kSlotBytes * kSlots + 0x1000u);
        const uint64_t aligned = (raw + 0xFFu) & ~static_cast<uint64_t>(0xFFu);
        if (raw && aligned >= gmcContext.vramStart) {
            const uint64_t off = aligned - gmcContext.vramStart;
            if (off < vramSize && kSlotBytes * kSlots <= vramSize - off) {
                memset(const_cast<uint8_t*>(vram) + off, 0, kSlotBytes * kSlots);
                graphicsIbGpuAddr = aligned;
                graphicsIbSlotBytes = kSlotBytes;
                graphicsIbSlot = 0;
                gmcHDPFlush(*this);
            }
        }
        if (!graphicsIbGpuAddr) LOG_ERROR("initGraphicsRing: IB allocation failed");
    }
    setProperty("GraphicsQueue,IbGpu", graphicsIbGpuAddr, 64);
    setProperty("GraphicsQueue,IbSlotBytes", static_cast<uint64_t>(graphicsIbSlotBytes), 32);
    snapshotGraphicsQueue("AfterMap");
    return true;
}

void RDNA4Driver::snapshotGraphicsQueue(const char* stage) {
    // Preserve the selector and observe both engines. These snapshots bracket
    // mapping and submission; initialization-time ioreg values are not live.
    const uint32_t savedSelect = readGcRegister(0x0900, 1);
    if (savedSelect == 0xFFFFFFFFu) return;
    grbmSelect(0, 0, 0, 0);
    struct Reg { const char* name; uint32_t offset; uint8_t segment; };
    static const Reg regs[] = {
        {"MeCntl", 0x0803, 1}, {"Pc0", 0x2A44, 1}, {"Pc1", 0x2A45, 1},
        {"CpStat", 0x0F40, 0}, {"CpBusy", 0x0F3F, 0},
        {"CpStall1", 0x0F3D, 0}, {"CpStall2", 0x0F3E, 0},
        {"CpStall3", 0x0F3C, 0}, {"CpfStatus", 0x0E27, 0},
        {"CpfBusy", 0x0E28, 0}, {"CpfStall", 0x0E29, 0},
        {"IntStat", 0x0F97, 0}, {"RoqRb", 0x0F80, 0},
        {"MqdLo", 0x1E7E, 0}, {"MqdHi", 0x1E7F, 0},
        {"MqdCtl", 0x1E9A, 0}, {"Active", 0x1E80, 0},
        {"Base", 0x1E86, 0}, {"BaseHi", 0x1E87, 0},
        {"Cntl", 0x1E8F, 0}, {"Rptr", 0x1E88, 0},
        {"Wptr", 0x1E91, 0}, {"WptrHi", 0x1E92, 0},
        {"Mapped", 0x1E94, 0}, {"QueMgr", 0x1E95, 0},
        {"HqStatus", 0x1E98, 0}, {"Doorbell", 0x1E8D, 0},
        {"RbRptr", 0x0F60, 0}, {"RbWptr", 0x1DF4, 0},
        {"PfpIcOp", 0x5843, 1}, {"MeIcOp", 0x5847, 1},
        {"DcOp", 0x2A09, 1}, {"DcCtl", 0x2A08, 1},
        {"FaultLo", 0x15D0, 0}, {"FaultHi", 0x15D1, 0},
        {"FaultAddrLo", 0x15D2, 0}, {"FaultAddrHi", 0x15D3, 0},
        {"ViewportXScale", 0xA10F, 1}, {"ScratchReg0", 0x2040, 1},
    };
    char key[64];
    auto publish = [&](const char* name, uint64_t value) {
        snprintf(key, sizeof(key), "GfxDiag,%s,%s", stage, name);
        setProperty(key, value, 64);
    };
    for (const Reg& reg : regs) {
        publish(reg.name, readGcRegister(reg.offset, reg.segment));
    }
    for (uint8_t segment = 0; segment < 5; segment++) {
        char name[32];
        snprintf(name, sizeof(name), "ViewportXScaleSeg%u", segment);
        publish(name, readGcRegister(0xA10F, segment));
    }
    grbmSelect(1, 0, 0, 0);
    publish("CmpActive", readGcRegister(0x1FAB, 0));
    publish("CmpBase", readGcRegister(0x1FB1, 0));
    publish("CmpMqdLo", readGcRegister(0x1FA9, 0));
    publish("CmpMqdHi", readGcRegister(0x1FAA, 0));
    publish("CmpRptr", readGcRegister(0x1FB3, 0));
    publish("CmpWptr", readGcRegister(0x1FDF, 0));
    publish("CmpDoorbell", readGcRegister(0x1FB8, 0));
    writeGcRegister(0x0900, savedSelect, 1);
    if (graphicsRing) {
        publish("SwWptr", graphicsRing->getWritePointer());
        const uint64_t off = graphicsRing->getFenceVRAMOffset();
        if (vram && off <= vramSize && sizeof(uint64_t) <= vramSize - off) {
            publish("Fence", *reinterpret_cast<volatile uint64_t*>(
                const_cast<uint8_t*>(vram) + off));
        }
    }
    if (graphicsRptrGpuAddr >= gmcContext.vramStart) {
        const uint64_t off = graphicsRptrGpuAddr - gmcContext.vramStart;
        if (vram && off <= vramSize && sizeof(uint32_t) <= vramSize - off) {
            publish("RptrWb", *reinterpret_cast<volatile uint32_t*>(
                const_cast<uint8_t*>(vram) + off));
        }
    }
    LOG("GFX diagnostic snapshot: %s", stage);
}

bool RDNA4Driver::resetGraphicsQueue() {
    if (!graphicsMqdMd || !graphicsWptrMd || !graphicsMqdGpuAddr) return false;
    MesApi::ResetQueueInput input {};
    input.doorbellOffset = graphicsDoorbellIndex;
    input.mqdAddr = graphicsMqdGpuAddr;
    input.wptrAddr = graphicsWptrGpuAddr;
    input.pipe = 0;
    input.queue = 0;
    input.vmid = 0;
    uint32_t packet[MesApi::kFrameDwords];
    MesApi::buildResetQueueLegacy(packet, input);
    // mesSubmitAndWait refuses to run once it has latched a timeout, which
    // would make the reset - the one call that can recover the queue - unable
    // to submit at all. The reset is the recovery path, so it always gets to
    // try; a failure re-latches the flag.
    if (mesTimedOut) {
        setProperty("GraphicsQueue,MesTimedOutBeforeReset", static_cast<uint64_t>(1), 32);
        mesTimedOut = false;
    }
    bool ok = mesSubmitAndWait(0, packet, MesApi::kFrameDwords, mesStatusGpuAddr);
    // reset_legacy_gfx has to touch the GFX pipe; when that pipe is the thing
    // that wedged, the firmware consumes the frame and never acks. Fall back to
    // the queue-only form, which leaves the pipe alone.
    if (!ok) {
        MesApi::buildResetQueueLegacy(packet, input, MesApi::kResetFlag_queueOnly);
        ok = mesSubmitAndWait(0, packet, MesApi::kFrameDwords, mesStatusGpuAddr);
        setProperty("GraphicsQueue,MesResetQueueOnly", static_cast<uint64_t>(ok ? 1 : 0), 32);
    }
    // The firmware does not ack either RESET form on a wedged GFX pipe, so the
    // last resort is to take the queue away from MES and hand it back: a plain
    // ADD_QUEUE is the one MES call this driver has seen succeed here.
    if (!ok) {
        MesApi::buildRemoveQueue(packet, input);
        const bool removed = mesSubmitAndWait(0, packet, MesApi::kFrameDwords, mesStatusGpuAddr);
        setProperty("GraphicsQueue,MesRemoveOk", static_cast<uint64_t>(removed ? 1 : 0), 32);
        if (removed) {
            ok = mesMapLegacyQueue(0, 0, graphicsDoorbellIndex, graphicsMqdGpuAddr,
                                   graphicsWptrGpuAddr, mesStatusGpuAddr, MesApi::kQueueTypeGfx);
            setProperty("GraphicsQueue,MesRecreateOk", static_cast<uint64_t>(ok ? 1 : 0), 32);
        }
    }
    setProperty("GraphicsQueue,MesResetOk", static_cast<uint64_t>(ok ? 1 : 0), 32);
    // The reset puts the CP back at ring dword 0; the software ring and the MQD's
    // cached read/write pointers have to agree with it, or the next submission is
    // never consumed.
    if (ok && graphicsRing) {
        graphicsRing->reset();
        if (graphicsMqdMd) {
            uint32_t* mqd = static_cast<uint32_t*>(graphicsMqdMd->getBytesNoCopy());
            mqd[kGfxHqdRptr] = 0;
            mqd[kGfxHqdWptr] = 0;
        }
        if (graphicsWptrMd) {
            *static_cast<volatile uint32_t*>(graphicsWptrMd->getBytesNoCopy()) = 0;
        }
        gmcHDPFlush(*this);
    }
    LOG("resetGraphicsQueue: MES reset_legacy_gfx %s", ok ? "acked" : "failed");
    return ok;
}

bool RDNA4Driver::submitGraphicsCommands(const uint32_t* pm4Data, size_t dwordCount,
                                          uint64_t* fenceOut) {
    if (!graphicsRing || !graphicsQueueMapped) {
        LOG_ERROR("submitGraphicsCommands: GFX queue is not up");
        return false;
    }
    if (!pm4Data || dwordCount == 0 || !fenceOut || !graphicsLock) return false;

    IOLockLock(graphicsLock);
    // GB_ADDR_CONFIG carries the memory interleave / RB mapping every SC, CB and
    // DB address depends on. It is firmware-programmed: this box reads
    // 0x08200545, the value real Navi 48 hardware reports, so it only needs to be
    // published (the register is not ours to rewrite). Reading it at the WRONG
    // dword offset (0x263E instead of regGB_ADDR_CONFIG's 0x13DE) is what once
    // made the driver believe the interleave was zero.
    {
        grbmSelect(0, 0, 0, 0);
        setProperty("GfxDiag,GbAddrConfig", static_cast<uint64_t>(readGcRegister(kRegGbAddrConfig, 0)), 32);
    }
    const uint64_t fence = graphicsRing->allocateFence();
    const uint64_t fenceAddr = graphicsFenceGpuAddr();
    if (!fenceAddr) {
        IOLockUnlock(graphicsLock);
        return false;
    }

    PM4BuilderKernel fencePacket;
    fencePacket.emitFenceWrite(fenceAddr, fence);
    const size_t fenceDwords = fencePacket.getPacketCount();

    // Graphics packets belong to the PFP. The ring is consumed by the ME, which
    // implements compute-style packets only: an inline draw and every SET_*_REG
    // in the graphics blocks are accepted and skipped, so the ring drains, the
    // fence completes and nothing renders.
    //
    // Standard GFX ring submission (gfx_v12_0_ring_emit_ib_gfx, Cobalt64
    // native_s1c_pure.h:build_submit):
    //   1. CONTEXT_CONTROL (3 dw): load_enable=1, shadow=0
    //   2. INDIRECT_BUFFER (4 dw): NO VALID bit (the GFX ring does not set it;
    //      only COMPUTE rings need bit 23). Control = length_dw | (vmid << 24).
    //   3. RELEASE_MEM fence (8 dw)
    const bool useIb = graphicsIbGpuAddr && graphicsIbSlotBytes &&
                       dwordCount <= graphicsIbSlotBytes / 4u;
    constexpr size_t kIbWrapperDwords = 7u; // 3 dw cntxcntl + 4 dw ib
    const size_t ringDwords = useIb ? (kIbWrapperDwords + fenceDwords)
                                    : (dwordCount + fenceDwords);
    uint32_t* stream = static_cast<uint32_t*>(IOMalloc(ringDwords * sizeof(uint32_t)));
    if (!stream) {
        IOLockUnlock(graphicsLock);
        return false;
    }
    if (useIb) {
        const uint32_t slot = graphicsIbSlot;
        graphicsIbSlot = (graphicsIbSlot + 1u) & 1u;
        const uint64_t ibAddr = graphicsIbGpuAddr +
                                static_cast<uint64_t>(slot) * graphicsIbSlotBytes;
        const uint64_t ibOff = (ibAddr - gmcContext.vramStart);
        if (ibOff >= vramSize || dwordCount * sizeof(uint32_t) > vramSize - ibOff) {
            IOFree(stream, ringDwords * sizeof(uint32_t));
            IOLockUnlock(graphicsLock);
            return false;
        }
        memcpy(const_cast<uint8_t*>(vram) + ibOff, pm4Data,
               dwordCount * sizeof(uint32_t));
        gmcHDPFlush(*this);

        // 1. CONTEXT_CONTROL: load_enable only
        stream[0] = (3u << 30) | (1u << 16) | (GFX12::PKT3_CONTEXT_CONTROL << 8);
        stream[1] = 0x80000000u;
        stream[2] = 0u;

        // 2. INDIRECT_BUFFER: no VALID bit (gfx_v12_0_ring_emit_ib_gfx)
        stream[3] = (3u << 30) | (2u << 16) | (GFX12::PKT3_INDIRECT_BUFFER << 8);
        stream[4] = static_cast<uint32_t>(ibAddr & 0xFFFFFFFCu);
        stream[5] = static_cast<uint32_t>(ibAddr >> 32);
        stream[6] = static_cast<uint32_t>(dwordCount) & 0xFFFFFu; // vmid 0

        // 3. EOP fence
        for (size_t i = 0; i < fenceDwords; i++) {
            stream[kIbWrapperDwords + i] = fencePacket.getPackets()[i];
        }
    } else {
        for (size_t i = 0; i < dwordCount; i++) stream[i] = pm4Data[i];
        for (size_t i = 0; i < fenceDwords; i++) {
            stream[dwordCount + i] = fencePacket.getPackets()[i];
        }
    }

    // Reclaim whatever the CP already consumed before checking for space.
    if (graphicsRptrGpuAddr >= gmcContext.vramStart) {
        const uint64_t off = graphicsRptrGpuAddr - gmcContext.vramStart;
        if (off + sizeof(uint32_t) <= vramSize) {
            const uint32_t rptr = *reinterpret_cast<volatile uint32_t*>(
                const_cast<uint8_t*>(vram) + off);
            graphicsRing->setReadPointer(rptr);
        }
    }

    bool ok = graphicsRing->writePM4(stream, ringDwords);
    if (ok) {
        // The CP reads the ring through the BAR, so the PM4 bytes have to be
        // pushed out before the doorbell tells it to look.
        armNggRingSentinel();
        gmcHDPFlush(*this);
        const uint32_t wptr = graphicsRing->getWritePointer();
        if (graphicsWptrMd) {
            *static_cast<volatile uint32_t*>(graphicsWptrMd->getBytesNoCopy()) = wptr;
            gmcHDPFlush(*this);
        }
        // gfx_v12_0_ring_set_wptr_gfx(): wptr page + doorbell only. The HQD
        // belongs to the MES now; CP_RB0_WPTR is not this queue's pointer.
        snapshotGraphicsQueue("BeforeKick");
        graphicsRing->submit();
        snapshotGraphicsQueue("AfterKick");
    }

    if (ok && graphicsMqdMd) {
        // If the queue is running, the firmware advances these; a stuck pair is
        // how we tell "the doorbell did nothing" from "the CP is not there".
        const volatile uint32_t* m = static_cast<const volatile uint32_t*>(
            graphicsMqdMd->getBytesNoCopy());
        setProperty("GraphicsQueue,MqdRptr", static_cast<uint64_t>(m[kGfxHqdRptr]), 32);
        setProperty("GraphicsQueue,MqdWptr", static_cast<uint64_t>(m[kGfxHqdWptr]), 32);
    }
    IOFree(stream, ringDwords * sizeof(uint32_t));
    IOLockUnlock(graphicsLock);
    if (ok) *fenceOut = fence;
    if (!ok) LOG_ERROR("submitGraphicsCommands: ring refused %zu dwords (ib=%u)",
                       ringDwords, useIb ? 1u : 0u);
    return ok;
}

bool RDNA4Driver::waitForGraphicsFence(uint64_t fenceValue, uint64_t timeoutMS) {
    if (!graphicsRing) return false;
    const bool reached = graphicsRing->waitForFence(fenceValue, timeoutMS);
    if (graphicsLock) IOLockLock(graphicsLock);
    snapshotGraphicsQueue(reached ? "Complete" : "Timeout");
    // Both on success and on timeout: a wedged draw is exactly the case where
    // knowing whether a vertex wave ran matters most.
    scanNggRing();
    if (reached) logGraphicsStateReadback();
    if (!reached) resetGraphicsQueue();
    if (graphicsLock) IOLockUnlock(graphicsLock);
    return reached;
}

namespace {
// The attribute ring is split per shader engine: SE n owns
// [base + n*0x160000, +0x160000). The GE picks the SE for a subgroup, so
// arming or scanning only the first region can only ever observe SE0.
constexpr uint32_t kNggSeCount = GFX12::kAttributeRingTotal / GFX12::kAttributeRingPerSE;
constexpr uint32_t kNggSentinelBytesPerSe = 4096;
} // namespace

void RDNA4Driver::armNggRingSentinel() {
    // Stamp a known pattern into the head of every SE's attribute ring region
    // before a submission. Whatever a vertex wave stores lands somewhere in
    // one of them, so a post-fence mismatch proves a store happened even when
    // the exact offset (gs_attr_offset) and the SE are unknown.
    if (!nggRingGpuAddr || !nggRingSize || !vram) return;
    if (nggRingGpuAddr < gmcContext.vramStart) return;
    const uint64_t base = nggRingGpuAddr - gmcContext.vramStart;
    for (uint32_t se = 0; se < kNggSeCount; se++) {
        const uint64_t region = static_cast<uint64_t>(se) * GFX12::kAttributeRingPerSE;
        if (region + kNggSentinelBytesPerSe > nggRingSize) break;
        if (base >= vramSize || region >= vramSize - base) break;
        if (kNggSentinelBytesPerSe > vramSize - base - region) break;
        uint32_t* head = reinterpret_cast<uint32_t*>(
            const_cast<uint8_t*>(vram) + base + region);
        for (uint32_t i = 0; i < kNggSentinelBytesPerSe / 4u; i++) head[i] = kNggSentinel;
    }
    gmcHDPFlush(*this);
}

void RDNA4Driver::scanNggRing() {
    // The SPI attribute ring is the only place a vertex stage's exports land
    // that the driver can read back: hardware writes it, userspace never does.
    // A non-zero count therefore proves the ES/GS wave actually ran.
    if (!nggRingGpuAddr || !nggRingSize || !vram) return;
    if (nggRingGpuAddr < gmcContext.vramStart) return;
    const uint64_t offset = nggRingGpuAddr - gmcContext.vramStart;
    if (offset >= vramSize || nggRingSize > vramSize - offset) return;
    gmcHDPFlush(*this);
    const volatile uint32_t* ring = reinterpret_cast<const volatile uint32_t*>(
        const_cast<uint8_t*>(vram) + offset);
    // Scan the whole attribute-ring region, not a fixed window: one SE's region
    // is 0x160000 bytes, so a 256 KiB window never reached SE1..3.
    const uint32_t attributeDwords = GFX12::kAttributeRingTotal / 4u;
    const uint32_t dwords = attributeDwords < nggRingSize / 4u
        ? attributeDwords : nggRingSize / 4u;
    uint32_t touched = 0;
    char key[64];
    for (uint32_t se = 0; se < kNggSeCount; se++) {
        const uint32_t regionDwords = GFX12::kAttributeRingPerSE / 4u;
        if (static_cast<uint64_t>(se + 1u) * regionDwords > dwords) break;
        const uint32_t armed = kNggSentinelBytesPerSe / 4u;
        uint32_t seTouched = 0;
        for (uint32_t i = 0; i < armed; i++) {
            seTouched += (ring[se * regionDwords + i] != kNggSentinel);
        }
        touched += seTouched;
        snprintf(key, sizeof(key), "GfxDiag,NggRingTouchedSe%u", se);
        setProperty(key, static_cast<uint64_t>(seTouched), 32);
    }
    setProperty("GfxDiag,NggRingSentinelTouched", static_cast<uint64_t>(touched), 32);
    uint32_t nonZero = 0;
    uint32_t firstIndex = 0, firstValue = 0;
    bool haveFirst = false;
    for (uint32_t i = 0; i < dwords; i++) {
        const uint32_t value = ring[i];
        if (!value) continue;
        nonZero++;
        if (!haveFirst) { haveFirst = true; firstIndex = i; firstValue = value; }
    }
    // Read-only: this used to reprogram GB_ADDR_CONFIG with a Navi-era value on
    // every fence completion, i.e. rewrite the memory-interleave word between
    // submissions with a layout that was never derived for this chip.
    // GB_ADDR_CONFIG is broadcast: a read with an instance selected returns 0,
    // so a zero here says nothing about the front end either way.
    {
        const uint32_t gbAddrConfig = readGcRegister(kRegGbAddrConfig, 0);
        const uint32_t rbBackendDisable = readGcRegister(kRegCcRbBackendDisable, 0);
        setProperty("GfxDiag,GbAddrConfigValue", static_cast<uint64_t>(gbAddrConfig), 32);
        setProperty("GfxDiag,CcRbBackendDisable", static_cast<uint64_t>(rbBackendDisable), 32);
        // Live status: GB_BACKEND_MAP is the per-SE backend mapping the CB/DB
        // use, GRBM_STATUS says whether the front end is idle after the fence.
        setProperty("GfxDiag,GbBackendMap",
                    static_cast<uint64_t>(readGcRegister(0x13DFu, 0)), 32);
        setProperty("GfxDiag,GrbmStatus",
                    static_cast<uint64_t>(readGcRegister(0x0DA4u, 0)), 32);
        setProperty("GfxDiag,GrbmStatus2",
                    static_cast<uint64_t>(readGcRegister(0x0DA2u, 0)), 32);
    }
    setProperty("GfxDiag,NggRingNonZero", static_cast<uint64_t>(nonZero), 32);
    setProperty("GfxDiag,NggRingFirstIndex", static_cast<uint64_t>(haveFirst ? firstIndex : 0xFFFFFFFFu), 32);
    setProperty("GfxDiag,NggRingFirstValue", static_cast<uint64_t>(firstValue), 32);

    // Scan the WHOLE NGG allocation, not just the attribute-ring slice: if the
    // GE hands out a gs_attr_offset past the attribute region, the vertex
    // stage's parameter stores land in the position/primitive rings and the
    // attribute-only scan reports an untouched ring.
    {
        const uint32_t allDwords = nggRingSize / 4u;
        const volatile uint32_t* all = reinterpret_cast<const volatile uint32_t*>(
            const_cast<uint8_t*>(vram) + offset);
        const uint32_t armedDwords = kNggSentinelBytesPerSe / 4u;
        uint64_t allNonZero = 0;
        uint32_t allFirstIndex = 0xFFFFFFFFu, allFirstValue = 0;
        for (uint32_t i = 0; i < allDwords; i++) {
            const uint32_t v = all[i];
            if (!v) continue;
            // Skip the sentinels stamped at the head of every SE region.
            const uint32_t seRegion = GFX12::kAttributeRingPerSE / 4u;
            const bool isSentinel = (i % seRegion) < armedDwords && v == kNggSentinel;
            if (isSentinel) continue;
            allNonZero++;
            if (allFirstIndex == 0xFFFFFFFFu) { allFirstIndex = i; allFirstValue = v; }
        }
        setProperty("GfxDiag,NggAllNonZero", allNonZero, 32);
        setProperty("GfxDiag,NggAllFirstIndex", static_cast<uint64_t>(allFirstIndex), 32);
        setProperty("GfxDiag,NggAllFirstValue", static_cast<uint64_t>(allFirstValue), 32);
        LOG("GFX NGG whole-allocation scan: %llu dwords non-zero, first at %d = 0x%08x",
            allNonZero, allFirstIndex == 0xFFFFFFFFu ? -1 : static_cast<int>(allFirstIndex), allFirstValue);
    }

    // The position and primitive rings sit after the attribute ring in the same
    // allocation. The GE writes them as it consumes the vertex exports, so a
    // non-zero count there is the only direct evidence that primitives reached
    // the rasterizer's input.
    struct RingScan { const char* name; uint32_t offset; uint32_t length; };
    const RingScan rings[] = {
        { "Pos",  GFX12::kGePosRingOffset,  0x400000u },
        { "Prim", GFX12::kGePrimRingOffset, 0x100000u },
    };
    for (const RingScan& r : rings) {
        if (static_cast<uint64_t>(r.offset) + r.length > nggRingSize) continue;
        const volatile uint32_t* words = reinterpret_cast<const volatile uint32_t*>(
            const_cast<uint8_t*>(vram) + offset + r.offset);
        const uint32_t count = r.length / 4u;
        uint32_t firstIndexR = 0xFFFFFFFFu, firstValueR = 0;
        uint64_t nonzero = 0;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t v = words[i];
            if (!v) continue;
            nonzero++;
            if (firstIndexR == 0xFFFFFFFFu) { firstIndexR = i; firstValueR = v; }
        }
        snprintf(key, sizeof(key), "GfxDiag,Ngg%sNonZero", r.name);
        setProperty(key, nonzero, 32);
        snprintf(key, sizeof(key), "GfxDiag,Ngg%sFirstIndex", r.name);
        setProperty(key, static_cast<uint64_t>(firstIndexR), 32);
        snprintf(key, sizeof(key), "GfxDiag,Ngg%sFirstValue", r.name);
        setProperty(key, static_cast<uint64_t>(firstValueR), 32);
        // Head dump: the first 16 dwords show the ring's entry layout and, for
        // the position ring, the exported coordinates themselves.
        for (uint32_t i = 0; i < 16u && i < count; i++) {
            snprintf(key, sizeof(key), "GfxDiag,Ngg%sHead%02u", r.name, i);
            setProperty(key, static_cast<uint64_t>(words[i]), 32);
        }
        LOG("GFX NGG %s ring: %llu/%u dwords non-zero, first at %d = 0x%08x",
            r.name, nonzero, count,
            firstIndexR == 0xFFFFFFFFu ? -1 : static_cast<int>(firstIndexR), firstValueR);
    }
    LOG("GFX NGG ring scan: %u dwords touched across %u SE, %u/%u dwords non-zero, first at %d = 0x%08x",
        touched, kNggSeCount, nonZero, dwords,
        haveFirst ? static_cast<int>(firstIndex) : -1, firstValue);
}

void RDNA4Driver::logGraphicsStateReadback() {
    // MMIO readback of the state a draw depends on. Published (not logged once)
    // so every submission's actual context can be compared against what the
    // encoder believes it wrote.
    struct Reg { const char* name; uint32_t byteOffset; };
    static const Reg regs[] = {
        {"stagesEn", 0x28a98}, {"posFormat", 0x2864c}, {"cbColor0Base", 0x28c60},
        {"cbColor0Info", 0x28ec0}, {"paClVteCntl", 0x28814},
        {"primType", 0x30908}, {"geCntl", 0x3096c}, {"gsMaxVertOut", 0x28b38},
        {"pgmLoGs", 0x0b210}, {"rsrc1Gs", 0x0b228}, {"userDataGs0", 0x0b230},
        {"pgmLoEs", 0x0b224}, {"pgmHiEs", 0x0b218}, {"rsrc2Gs", 0x0b22c},
        {"rsrc3Gs", 0x0b21c}, {"rsrc4Gs", 0x0b220},
        {"pgmLoPs", 0x0b020}, {"rsrc1Ps", 0x0b028}, {"rsrc2Ps", 0x0b02c},
        {"numInstances", 0x30934}, {"gsInstCnt", 0x28b3c}, {"gsOutPrim", 0x30998},
        {"binner0", 0x28c44}, {"modeCntl0", 0x28a48}, {"modeCntl1", 0x28a4c},
        {"cbColorControl", 0x28858}, {"idxFormat", 0x28648}, {"colFormat", 0x28654},
        {"maxOutPerSg", 0x287fc}, {"nggSubgrp", 0x28b4c},
        {"psInputEna", 0x2865c}, {"psInputAddr", 0x28660}, {"gsOutCfgPs", 0x0b0c4},
        {"clipCntl", 0x28810}, {"scModeCntl", 0x2881c}, {"aaMaskLo", 0x28c38},
    };
    // Context registers are read through the currently selected GC instance;
    // without this the reads land on whatever block the last compute call
    // selected and come back as zero.
    grbmSelect(0, 0, 0, 0);
    char key[64];
    for (const Reg& reg : regs) {
        const uint32_t value = readGcRegister(reg.byteOffset >> 2, 0);
        snprintf(key, sizeof(key), "GfxDiag,Rd,%s", reg.name);
        setProperty(key, static_cast<uint64_t>(value), 32);
    }
}

uint64_t RDNA4Driver::graphicsFenceGpuAddr() {
    if (!graphicsRing) return 0;
    return gmcContext.vramStart + graphicsRing->getFenceVRAMOffset();
}
