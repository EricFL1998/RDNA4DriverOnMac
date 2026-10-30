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

    // What the firmware wrote back into the MQD is the proof it consumed it.
    gmcHDPFlush(*this);
    const volatile uint32_t* m = static_cast<const volatile uint32_t*>(
        graphicsMqdMd->getBytesNoCopy());
    setProperty("GraphicsQueue,MqdActive", static_cast<uint64_t>(m[kGfxHqdActive]), 32);
    setProperty("GraphicsQueue,MqdMapped", static_cast<uint64_t>(m[kGfxHqdMapped]), 32);
    exportMqdDiff("GraphicsQueue", mqdBefore, m, 512);

    // gfx_v12_0_cp_gfx_start() runs right after the MES maps the queue: the CP
    // is already running on the async path, but it still needs its context
    // count and device id before it will fetch from a mapped HQD.
    writeGcRegister(kCpMaxContext, 8u - 1u, 0);
    writeGcRegister(kCpDeviceId, 1u, 0);
    gmcHDPFlush(*this);
    // The firmware programs the GFX HQD block directly; reading it back is how
    // we tell "the MES applied the queue" from "the MES only acked the API".
    // The GFX HQD lives on me0/pipe0/queue0 (me1 is the PFP, which has no HQD
    // of its own - reading there returns zeros and looks like "not loaded").
    // The whole CP_GFX_HQD_* block is BASE_IDX 0 (gc_12_0_0_offset.h).
    grbmSelect(0, 0, 0, 0);
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
    };
    char key[64];
    auto publish = [&](const char* name, uint64_t value) {
        snprintf(key, sizeof(key), "GfxDiag,%s,%s", stage, name);
        setProperty(key, value, 64);
    };
    for (const Reg& reg : regs) {
        publish(reg.name, readGcRegister(reg.offset, reg.segment));
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

bool RDNA4Driver::submitGraphicsCommands(const uint32_t* pm4Data, size_t dwordCount,
                                          uint64_t* fenceOut) {
    if (!graphicsRing || !graphicsQueueMapped) {
        LOG_ERROR("submitGraphicsCommands: GFX queue is not up");
        return false;
    }
    if (!pm4Data || dwordCount == 0 || !fenceOut || !graphicsLock) return false;

    IOLockLock(graphicsLock);
    const uint64_t fence = graphicsRing->allocateFence();
    const uint64_t fenceAddr = graphicsFenceGpuAddr();
    if (!fenceAddr) {
        IOLockUnlock(graphicsLock);
        return false;
    }

    PM4BuilderKernel fencePacket;
    fencePacket.emitFenceWrite(fenceAddr, fence);
    const size_t fenceDwords = fencePacket.getPacketCount();
    const size_t totalDwords = dwordCount + fenceDwords;
    uint32_t* stream = static_cast<uint32_t*>(IOMalloc(totalDwords * sizeof(uint32_t)));
    if (!stream) {
        IOLockUnlock(graphicsLock);
        return false;
    }
    for (size_t i = 0; i < dwordCount; i++) stream[i] = pm4Data[i];
    for (size_t i = 0; i < fenceDwords; i++) {
        stream[dwordCount + i] = fencePacket.getPackets()[i];
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

    bool ok = graphicsRing->writePM4(stream, totalDwords);
    if (ok) {
        // The CP reads the ring through the BAR, so the PM4 bytes have to be
        // pushed out before the doorbell tells it to look.
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
    IOFree(stream, totalDwords * sizeof(uint32_t));
    IOLockUnlock(graphicsLock);
    if (ok) *fenceOut = fence;
    if (!ok) LOG_ERROR("submitGraphicsCommands: ring refused %zu dwords", totalDwords);
    return ok;
}

bool RDNA4Driver::waitForGraphicsFence(uint64_t fenceValue, uint64_t timeoutMS) {
    if (!graphicsRing) return false;
    const bool reached = graphicsRing->waitForFence(fenceValue, timeoutMS);
    if (graphicsLock) IOLockLock(graphicsLock);
    snapshotGraphicsQueue(reached ? "Complete" : "Timeout");
    if (graphicsLock) IOLockUnlock(graphicsLock);
    return reached;
}

uint64_t RDNA4Driver::graphicsFenceGpuAddr() {
    if (!graphicsRing) return 0;
    return gmcContext.vramStart + graphicsRing->getFenceVRAMOffset();
}
