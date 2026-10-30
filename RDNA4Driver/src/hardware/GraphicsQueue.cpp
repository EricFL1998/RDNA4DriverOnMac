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
#include "../hardware/GraphicsPM4Builder.hpp"
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
constexpr uint32_t kDefaultGfxHqdCntl    = 0x00f00000u;
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
        !graphicsRing->init(vram, vramSize,
                            reinterpret_cast<volatile uint64_t*>(doorbell),
                            graphicsDoorbellIndex,
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

    // CP_GFX_HQD_BASE holds the ring address >> 8.
    mqd[kGfxHqdBase] = static_cast<uint32_t>((ringGpu >> 8) & 0xFFFFFFFFu);
    mqd[kGfxHqdBaseHi] = static_cast<uint32_t>(ringGpu >> 40);

    mqd[kGfxHqdRptrAddr] = static_cast<uint32_t>(graphicsRptrGpuAddr & 0xFFFFFFFCu);
    mqd[kGfxHqdRptrAddrHi] = static_cast<uint32_t>(graphicsRptrGpuAddr >> 32) & 0xFFFFu;
    mqd[kRbWptrPollAddrLo] = static_cast<uint32_t>(wptrGpu & 0xFFFFFFFCu);
    mqd[kRbWptrPollAddrHi] = static_cast<uint32_t>(wptrGpu >> 32) & 0xFFFFu;

    graphicsRbBufsz = ringBufszFor(kGfxRingBytes / 4u);
    mqd[kGfxHqdCntl] = kDefaultGfxHqdCntl |
                       (graphicsRbBufsz & 0x3Fu) |
                       (((graphicsRbBufsz - 2u) & 0x3Fu) << 8);

    mqd[kRbDoorbellControl] = ((graphicsDoorbellIndex & 0x3FFFu) << 2) | (1u << 30);
    mqd[kGfxHqdOffset] = 0;
    mqd[kGfxHqdRptr] = 0;
    // The MES, not the MQD contents, brings an HQD up: the compute queue on
    // this card only maps with active=0, and a pre-set ACTIVE bit is the one
    // difference that stopped the firmware from activating this queue.
    mqd[kGfxHqdActive] = 0;

    gmcHDPFlush(*this);

    graphicsQueueMapped = mesMapLegacyQueue(0, 0, graphicsDoorbellIndex, mqdGpu,
                                            wptrGpu, statusGpu, 0 /* GFX */);
    setProperty("GraphicsQueue,Mapped", static_cast<uint64_t>(graphicsQueueMapped ? 1 : 0), 32);
    setProperty("GraphicsQueue,RingGpu", ringGpu, 64);
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
    grbmSelect(0, 0, 0, 0);
    graphicsHqdActive = readGcRegister(0x1E80u, 1);
    setProperty("GraphicsQueue,HqdActiveReg", static_cast<uint64_t>(graphicsHqdActive), 32);
    setProperty("GraphicsQueue,HqdMappedReg", static_cast<uint64_t>(readGcRegister(0x1E94u, 1)), 32);
    setProperty("GraphicsQueue,HqdBaseReg", static_cast<uint64_t>(readGcRegister(0x1E86u, 1)), 32);
    setProperty("GraphicsQueue,HqdWptrReg", static_cast<uint64_t>(readGcRegister(0x1E91u, 1)), 32);
    setProperty("GraphicsQueue,MaxContext", static_cast<uint64_t>(readGcRegister(kCpMaxContext, 0)), 32);
    setProperty("GraphicsQueue,DeviceId", static_cast<uint64_t>(readGcRegister(kCpDeviceId, 0)), 32);

    // CP_RB0_RPTR is the ring-0 read pointer: once the firmware has taken the
    // queue it tracks the ring instead of sitting at its reset value.
    setProperty("GraphicsQueue,Rptr", static_cast<uint64_t>(readGcRegister(kCpRb0Rptr, 0)), 32);
    return true;
}

bool RDNA4Driver::submitGraphicsCommands(const uint32_t* pm4Data, size_t dwordCount) {
    if (!graphicsRing || !graphicsQueueMapped) {
        LOG_ERROR("submitGraphicsCommands: GFX queue is not up");
        return false;
    }
    if (!pm4Data || dwordCount == 0) return false;

    if (graphicsLock) IOLockLock(graphicsLock);

    // Reclaim whatever the CP already consumed before checking for space.
    if (graphicsRptrGpuAddr >= gmcContext.vramStart) {
        const uint64_t off = graphicsRptrGpuAddr - gmcContext.vramStart;
        if (off + sizeof(uint32_t) <= vramSize) {
            const uint32_t rptr = *reinterpret_cast<volatile uint32_t*>(
                const_cast<uint8_t*>(vram) + off);
            graphicsRing->setReadPointer(rptr);
        }
    }

    bool ok = graphicsRing->writePM4(pm4Data, dwordCount);
    if (ok) {
        // The CP reads the ring through the BAR, so the PM4 bytes have to be
        // pushed out before the doorbell tells it to look.
        gmcHDPFlush(*this);
        const uint32_t wptr = graphicsRing->getWritePointer();
        if (graphicsWptrMd) {
            *static_cast<volatile uint32_t*>(graphicsWptrMd->getBytesNoCopy()) = wptr;
            gmcHDPFlush(*this);
        }
        graphicsRing->submit();
        // A MES-owned queue is supposed to be kicked by the doorbell alone. If
        // the firmware left the HQD active but did not wire the doorbell to it,
        // the ring-0 write pointer is the only other handle we have.
        if (graphicsHqdActive) {
            writeGcRegister(0x1DF4u, wptr, 0);
            writeGcRegister(0x1DF5u, 0, 0);
        }
    }

    if (ok && graphicsMqdMd) {
        // If the queue is running, the firmware advances these; a stuck pair is
        // how we tell "the doorbell did nothing" from "the CP is not there".
        const volatile uint32_t* m = static_cast<const volatile uint32_t*>(
            graphicsMqdMd->getBytesNoCopy());
        setProperty("GraphicsQueue,MqdRptr", static_cast<uint64_t>(m[kGfxHqdRptr]), 32);
        setProperty("GraphicsQueue,MqdWptr", static_cast<uint64_t>(m[kGfxHqdWptr]), 32);
    }
    if (graphicsLock) IOLockUnlock(graphicsLock);
    if (!ok) LOG_ERROR("submitGraphicsCommands: ring refused %zu dwords", dwordCount);
    return ok;
}

bool RDNA4Driver::waitForGraphicsFence(uint64_t fenceValue, uint64_t timeoutMS) {
    if (!graphicsRing) return false;
    return graphicsRing->waitForFence(fenceValue, timeoutMS);
}

uint64_t RDNA4Driver::allocGraphicsFence() {
    return graphicsRing ? graphicsRing->allocateFence() : 0;
}

uint64_t RDNA4Driver::graphicsFenceGpuAddr() {
    if (!graphicsRing) return 0;
    return gmcContext.vramStart + graphicsRing->getFenceVRAMOffset();
}
