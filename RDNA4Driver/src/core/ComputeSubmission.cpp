#include "RDNA4Driver.hpp"
#include <IOKit/IOLib.h>
#include "../hardware/VramLayout.hpp"

//==============================================================================
// compute ring 与命令提交
//
// This file owns the driver-side compute ring orchestration. The low-level
// ring buffer lives in ComputeRing.cpp (src/hardware).
//==============================================================================

bool RDNA4Driver::initComputeRing() {
    LOG("initComputeRing() - initializing compute command ring");

    if (!vram || vramSize == 0) {
        LOG_ERROR("VRAM not mapped, cannot initialize compute ring");
        return false;
    }

    if (!doorbell || doorbellSize == 0) {
        LOG_ERROR("Doorbell not mapped, cannot initialize compute ring");
        return false;
    }

    // The ring is only valid once GFX firmware/autoload completed.
    OSObject* gfxStageObj = copyProperty("GFX,Stage");
    OSString* gfxStage = OSDynamicCast(OSString, gfxStageObj);
    bool gfxAlive = gfxStage && gfxStage->isEqualTo("gfx-alive");
    OSSafeReleaseNULL(gfxStageObj);

    if (!gfxAlive) {
        LOG_ERROR("GFX not alive, cannot initialize compute ring");
        return false;
    }

    if (computeRing) {
        delete computeRing;
        computeRing = nullptr;
    }

    // Path selection: GFX ring (ME/PFP, default) or MEC KCQ (rdna4-mec=1).
    uint32_t useMec = 0;
    const bool wantMec =
        PE_parse_boot_argn("rdna4-mec", &useMec, sizeof(useMec)) && useMec != 0;
    uint32_t useMes = 0;
    const bool wantMes =
        !PE_parse_boot_argn("rdna4-mes", &useMes, sizeof(useMes)) || useMes != 0;
    // MES SCHED uses dword 0x20. Sharing that doorbell sends later MES
    // write pointers to this compute queue too, executing stale ring data.
    const uint32_t doorbellIndex = wantMes ? MesApi::Doorbell::kMecRing0Dword
                                         : (wantMec ? 0u : 0x20u);

    computeRing = new ComputeRing();
    if (!computeRing) {
        LOG_ERROR("Failed to allocate ComputeRing");
        return false;
    }

    if (!computeRing->init(vram, vramSize, doorbell, doorbellIndex)) {
        LOG_ERROR("ComputeRing init failed");
        delete computeRing;
        computeRing = nullptr;
        return false;
    }

    // 用户可见窗口初始为空；首个用户
    // allocation seeds userHeapNext.
    userHeapNext = 0;

    computeHeapNext = VramLayout::kComputeHeap;

    // 把 ring 交给 CP/MEC 队列。MQD、EOP 与 rptr
    // writeback all live in the compute heap right above the ring.
    const uint64_t ringGpuAddr = gmcContext.vramStart + VramLayout::kComputeRing;
    const uint64_t mqdAddr = allocateVRAMBuffer(4096);
    const uint64_t eopAddr = allocateVRAMBuffer(4096);
    const uint64_t rptrAddr = allocateVRAMBuffer(4096);
    const uint64_t wptrAddr = allocateVRAMBuffer(4096);
    // GPU-written read pointer for refreshRingReadPtr().
    computeRptrGpuAddr = rptrAddr;
    computeWptrPollGpuAddr = wptrAddr;
    bool queueOk = false;

    if (wantMes) {
        // MES path (mandatory scheduler on GFX12): prepare the KCQ MQD, start
        // MES, then ask MES to map the queue with map_legacy_kq.
        setProperty("ComputeQueue,Path", "mes");
        const uint64_t statusAddr = allocateVRAMBuffer(4096);
        // MES reads the queue's MQD and polls its wptr through the GART
        // aperture (upstream KFD/MES convention), so both are allocated as
        // GART-mapped system buffers instead of VRAM.
        const uint64_t gttMqd = gmcAllocGartBuffer(*this, gmcContext, 4096,
                                                  &computeMqdMd);
        const uint64_t gttWptr = gmcAllocGartBuffer(*this, gmcContext, 4096,
                                                   &computeWptrMd);
        const uint64_t mqdGpu = gttMqd ? gttMqd : mqdAddr;
        computeWptrGpuAddr = gttWptr ? gttWptr : wptrAddr;
        computeWptrPollGpuAddr = computeWptrGpuAddr;
        setProperty("ComputeQueue,MqdGtt", static_cast<uint64_t>(gttMqd ? 1 : 0), 32);
        if (mqdAddr && eopAddr && rptrAddr && wptrAddr && statusAddr) {
            volatile uint32_t* wp = computeWptrMd
                ? static_cast<volatile uint32_t*>(computeWptrMd->getBytesNoCopy())
                : reinterpret_cast<volatile uint32_t*>(
                      const_cast<uint8_t*>(vram) + (wptrAddr - gmcContext.vramStart));
            *wp = 0;
            computeQueueViaMes = true;
            queueOk = initComputeQueue(ringGpuAddr,
                                       static_cast<uint32_t>(VramLayout::kComputeRingSize),
                                       mqdGpu, eopAddr, rptrAddr, doorbellIndex);
            if (queueOk) {
                // gfx_v12_0_hw_init() order: constants_init (SH_MEM_CONFIG for
                // every VMID, compute VMID trap enable, DB_RING_CONTROL), then
                // rlc_resume on the PSP path (clear-state buffer + RLC SRM),
                // then cp_resume: cp_gfx_enable(true) and cp_compute_enable(true)
                // before the KIQ/MES is brought up.
                initShaderMemoryConfig();
                initComputeVmid();
                setProperty("RLC,CsbOk", initClearStateBuffer());
                enableCpGfx();
                queueOk = enableMecRs64Pipes();
            }
            if (queueOk) {
                queueOk = mesInit();
            }
            if (queueOk) {
                // Same before/after snapshot as the GFX queue, so the two
                // MqdDiff properties show what the firmware writes back on a
                // queue that is known to run.
                const volatile uint32_t* cm = computeMqdMd
                    ? static_cast<const volatile uint32_t*>(computeMqdMd->getBytesNoCopy())
                    : reinterpret_cast<const volatile uint32_t*>(
                          const_cast<uint8_t*>(vram) + (mqdGpu - gmcContext.vramStart));
                uint32_t before[512];
                for (uint32_t i = 0; i < 512; i++) before[i] = cm[i];
                queueOk = mesMapLegacyQueue(0, 0, doorbellIndex, mqdGpu,
                                            computeWptrGpuAddr, statusAddr, 1);
                gmcHDPFlush(*this);
                exportMqdDiff("ComputeQueue", before, cm, 512);
            }
        } else {
            LOG_ERROR("initComputeRing: MES buffer allocation failed");
        }
    } else if (wantMec) {
        setProperty("ComputeQueue,Path", "mec");
        if (mqdAddr && eopAddr && rptrAddr) {
            queueOk = initComputeQueue(ringGpuAddr,
                                       static_cast<uint32_t>(VramLayout::kComputeRingSize),
                                       mqdAddr, eopAddr, rptrAddr, doorbellIndex);
            if (queueOk) {
                uint32_t enableQueue = 0;
                const bool requested =
                    PE_parse_boot_argn("rdna4-queue", &enableQueue,
                                       sizeof(enableQueue)) && enableQueue != 0;
                if (requested) {
                    queueOk = activateComputeQueue();
                } else {
                    LOG("initComputeRing: activation skipped (set rdna4-queue=1)");
                }
            }
        } else {
            LOG_ERROR("initComputeRing: MQD/EOP/rptr allocation failed");
        }
    } else {
        // Default path: the graphics ring (ME/PFP), which is the submission
        // path the bring-up reference uses for compute dispatch.
        setProperty("ComputeQueue,Path", "gfx-ring");
        // Upstream (amdgpu_ring_init) and Cobalt64 both keep the GFX ring in
        // GTT/GART system memory, and this is the one structural difference
        // left between their working rings and ours (which lives in VRAM).
        // rdna4-gfx-ring-vram=1 puts it back for A/B.
        uint32_t wantVramRing = 0;
        PE_parse_boot_argn("rdna4-gfx-ring-vram", &wantVramRing, sizeof(wantVramRing));
        const uint32_t ringDwords =
            static_cast<uint32_t>(VramLayout::kComputeRingSize / sizeof(uint32_t));
        uint64_t gfxRingGpu = ringGpuAddr;
        bool ringReady = false;
        if (wantVramRing == 0) {
            const uint64_t gartRing =
                gmcAllocGartBuffer(*this, gmcContext, VramLayout::kComputeRingSize,
                                   &gfxRingMd);
            if (gartRing && gfxRingMd) {
                volatile uint8_t* cpu =
                    static_cast<volatile uint8_t*>(gfxRingMd->getBytesNoCopy());
                for (size_t i = 0; i < VramLayout::kComputeRingSize; i++) cpu[i] = 0;
                volatile uint64_t* fenceCpu = reinterpret_cast<volatile uint64_t*>(
                    const_cast<uint8_t*>(vram) + VramLayout::kComputeFence);
                ringReady = computeRing->initAt(
                    reinterpret_cast<volatile uint32_t*>(cpu), ringDwords,
                    fenceCpu, doorbell, doorbellIndex);
                gfxRingGpu = gartRing;
                setProperty("ComputeQueue,RingGtt", static_cast<uint64_t>(1), 32);
                LOG("initComputeRing: GFX ring in GART at 0x%llx (%u dwords)",
                    static_cast<unsigned long long>(gartRing), ringDwords);
            } else {
                LOG_ERROR("initComputeRing: GART ring allocation failed, using VRAM");
            }
        }
        if (!ringReady && rptrAddr && wptrAddr) {
            queueOk = initGfxRing(ringGpuAddr,
                                  static_cast<uint32_t>(VramLayout::kComputeRingSize),
                                  rptrAddr, wptrAddr, doorbellIndex);
        } else if (ringReady) {
            queueOk = initGfxRing(gfxRingGpu,
                                  static_cast<uint32_t>(VramLayout::kComputeRingSize),
                                  rptrAddr, wptrAddr, doorbellIndex);
        } else {
            LOG_ERROR("initComputeRing: rptr/wptr allocation failed");
        }
    }

    setProperty("ComputeQueue,Initialized", static_cast<uint64_t>(queueOk ? 1 : 0), 32);
    if (!queueOk) {
        LOG_ERROR("initComputeRing: CP/MEC queue bring-up failed");
    }

    LOG("Compute Ring initialized successfully");
    setProperty("ComputeRing,Initialized", true);
    setProperty("ComputeRing,VRAMOffset", VramLayout::kComputeRing, 64);
    setProperty("ComputeRing,Size", VramLayout::kComputeRingSize, 64);
    setProperty("ComputeRing,HeapStart", computeHeapNext, 64);
    return true;
}

uint64_t RDNA4Driver::allocateVRAMBuffer(size_t size) {
    if (!computeRing || computeHeapNext == 0) {
        LOG_ERROR("allocateVRAMBuffer: compute ring not initialized");
        return 0;
    }
    if (!vram || vramSize == 0 || size == 0) {
        LOG_ERROR("allocateVRAMBuffer: VRAM not available or zero size");
        return 0;
    }

    constexpr uint64_t alignment = 0x1000;
    if (size > UINT32_MAX) {
        LOG_ERROR("allocateVRAMBuffer: request %zu exceeds the 4 GiB heap limit", size);
        return 0;
    }
    uint64_t allocSize = (static_cast<uint64_t>(size) + alignment - 1) &
                         ~(alignment - 1);

    // The kernel heap must stop at the user window: a user mapping of that
    // range would otherwise expose kernel-owned PSP/GFX buffers.
    const uint64_t heapCeiling = VramLayout::kUserHeap < vramSize
        ? VramLayout::kUserHeap : vramSize;
    if (allocSize < size || computeHeapNext > heapCeiling ||
        allocSize > heapCeiling - computeHeapNext) {
        LOG_ERROR("allocateVRAMBuffer: out of kernel heap (need %llu, available %llu)",
                  static_cast<unsigned long long>(allocSize),
                  static_cast<unsigned long long>(heapCeiling - computeHeapNext));
        return 0;
    }

    const uint64_t vramOffset = computeHeapNext;
    computeHeapNext += allocSize;
    const uint64_t gpuAddress = gmcContext.vramStart + vramOffset;

    if (vramLiveBlockCount < kMaxVramBlocks) {
        vramLiveBlocks[vramLiveBlockCount].gpuAddr = gpuAddress;
        vramLiveBlocks[vramLiveBlockCount].size = allocSize;
        vramLiveBlockCount++;
    } else {
        LOG_ERROR("allocateVRAMBuffer: live-block table full (%u)", kMaxVramBlocks);
    }

    LOG("allocateVRAMBuffer: allocated %llu bytes at GPU 0x%llx (VRAM+0x%llx)",
        static_cast<unsigned long long>(allocSize),
        static_cast<unsigned long long>(gpuAddress),
        static_cast<unsigned long long>(vramOffset));
    return gpuAddress;
}

uint64_t RDNA4Driver::userHeapGpuAddr() const {
    return gmcContext.vramStart + VramLayout::kUserHeap;
}

uint64_t RDNA4Driver::userHeapSize() const {
    // Never advertise more window than BAR0 actually maps.
    if (vramSize <= VramLayout::kUserHeap) return 0;
    const uint64_t available = vramSize - VramLayout::kUserHeap;
    return available < VramLayout::kUserHeapSize ? available : VramLayout::kUserHeapSize;
}

uint64_t RDNA4Driver::allocateUserVRAMBuffer(size_t size) {
    const uint64_t window = userHeapSize();
    if (!vram || window == 0 || size == 0) {
        LOG_ERROR("allocateUserVRAMBuffer: no user window (size=%zu)", size);
        return 0;
    }
    constexpr uint64_t alignment = 0x1000;
    const uint64_t allocSize = (static_cast<uint64_t>(size) + alignment - 1) &
                               ~(alignment - 1);
    if (allocSize < size) return 0;

    // Reuse a released range before bumping: the window is only 32 MiB, so a
    // bump-only allocator runs dry after a few frames of alloc/free. vramFreeTake
    // records the range in the live table, which is what freeVRAMBuffer needs.
    const uint64_t reused = vramFreeTake(allocSize, true);
    if (reused) {
        LOG("allocateUserVRAMBuffer: %llu bytes reused at GPU 0x%llx",
            static_cast<unsigned long long>(allocSize),
            static_cast<unsigned long long>(reused));
        return reused;
    }

    if (userHeapNext == 0) userHeapNext = VramLayout::kUserHeap;
    if (userHeapNext < VramLayout::kUserHeap ||
        userHeapNext - VramLayout::kUserHeap > window ||
        allocSize > window - (userHeapNext - VramLayout::kUserHeap)) {
        LOG_ERROR("allocateUserVRAMBuffer: user window exhausted (need %llu of %llu)",
                  static_cast<unsigned long long>(allocSize),
                  static_cast<unsigned long long>(window));
        return 0;
    }

    const uint64_t gpuAddress = gmcContext.vramStart + userHeapNext;
    userHeapNext += allocSize;

    if (vramLiveBlockCount < kMaxVramBlocks) {
        vramLiveBlocks[vramLiveBlockCount].gpuAddr = gpuAddress;
        vramLiveBlocks[vramLiveBlockCount].size = allocSize;
        vramLiveBlockCount++;
    } else {
        LOG_ERROR("allocateUserVRAMBuffer: live-block table full, 0x%llx cannot be freed",
                  static_cast<unsigned long long>(gpuAddress));
    }

    LOG("allocateUserVRAMBuffer: %llu bytes at GPU 0x%llx (VRAM+0x%llx)",
        static_cast<unsigned long long>(allocSize),
        static_cast<unsigned long long>(gpuAddress),
        static_cast<unsigned long long>(userHeapNext - allocSize));
    return gpuAddress;
}

IOMemoryDescriptor* RDNA4Driver::copyUserHeapDescriptor() {
    if (!pciDevice) return nullptr;
    const uint64_t window = userHeapSize();
    if (window == 0) {
        LOG_ERROR("copyUserHeapDescriptor: user window is empty");
        return nullptr;
    }
    IODeviceMemory* bar = pciDevice->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress0);
    if (!bar) {
        LOG_ERROR("copyUserHeapDescriptor: BAR0 not available");
        return nullptr;
    }
    IODeviceMemory* window_mem = IODeviceMemory::withSubRange(
        bar, VramLayout::kUserHeap, static_cast<IOByteCount>(window));
    if (!window_mem) {
        LOG_ERROR("copyUserHeapDescriptor: cannot describe the user window");
        return nullptr;
    }
    return window_mem;
}

void RDNA4Driver::refreshRingReadPtr() {
    if (!computeRing || !computeRptrGpuAddr || !vram) return;
    if (computeRptrGpuAddr < gmcContext.vramStart) return;
    const uint64_t offset = computeRptrGpuAddr - gmcContext.vramStart;
    if (offset + sizeof(uint32_t) > vramSize) return;
    const uint32_t rptr = *reinterpret_cast<volatile uint32_t*>(
        const_cast<uint8_t*>(vram) + offset);
    computeRing->setReadPointer(rptr);
}

void RDNA4Driver::freeVRAMBuffer(uint64_t gpuAddress) {
    if (!computeRing || !vram || gpuAddress < gmcContext.vramStart) {
        LOG_ERROR("freeVRAMBuffer: 0x%llx is not a heap address",
                  static_cast<unsigned long long>(gpuAddress));
        return;
    }

    for (uint32_t i = 0; i < vramLiveBlockCount; i++) {
        if (vramLiveBlocks[i].gpuAddr != gpuAddress) continue;
        const uint64_t size = vramLiveBlocks[i].size;
        vramLiveBlocks[i] = vramLiveBlocks[--vramLiveBlockCount];
        if (!vramFreeInsert(gpuAddress, size)) {
            LOG_ERROR("freeVRAMBuffer: free list full, %llu bytes at 0x%llx leaked",
                      static_cast<unsigned long long>(size),
                      static_cast<unsigned long long>(gpuAddress));
            return;
        }
        LOG("freeVRAMBuffer: released %llu bytes at GPU 0x%llx",
            static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(gpuAddress));
        return;
    }

    // Buffers handed out by gmcAllocGartBuffer (GART aperture) or the fixed
    // VRAM layout are not part of this heap and must not enter the free list.
    LOG_ERROR("freeVRAMBuffer: 0x%llx was not allocated from the compute heap",
              static_cast<unsigned long long>(gpuAddress));
}

// Insert a released range, merging it with any neighbour it touches.
bool RDNA4Driver::vramFreeInsert(uint64_t gpuAddr, uint64_t size) {
    if (size == 0) return true;

    for (uint32_t i = 0; i < vramFreeBlockCount; i++) {
        VramBlock& b = vramFreeBlocks[i];
        if (b.gpuAddr + b.size == gpuAddr) {           // grows up
            b.size += size;
            for (uint32_t j = 0; j < vramFreeBlockCount; j++) {
                VramBlock& n = vramFreeBlocks[j];
                if (j != i && n.gpuAddr == b.gpuAddr + b.size) {
                    b.size += n.size;
                    vramFreeBlocks[j] = vramFreeBlocks[--vramFreeBlockCount];
                    break;
                }
            }
            return true;
        }
        if (gpuAddr + size == b.gpuAddr) {              // grows down
            b.gpuAddr = gpuAddr;
            b.size += size;
            for (uint32_t j = 0; j < vramFreeBlockCount; j++) {
                VramBlock& n = vramFreeBlocks[j];
                if (j != i && n.gpuAddr + n.size == b.gpuAddr) {
                    b.gpuAddr = n.gpuAddr;
                    b.size += n.size;
                    vramFreeBlocks[j] = vramFreeBlocks[--vramFreeBlockCount];
                    break;
                }
            }
            return true;
        }
    }

    if (vramFreeBlockCount >= kMaxVramBlocks) return false;
    vramFreeBlocks[vramFreeBlockCount].gpuAddr = gpuAddr;
    vramFreeBlocks[vramFreeBlockCount].size = size;
    vramFreeBlockCount++;
    return true;
}

// First fit; a block larger than the request keeps its tail free.
uint64_t RDNA4Driver::vramFreeTake(uint64_t size, bool userRegion) {
    for (uint32_t i = 0; i < vramFreeBlockCount; i++) {
        VramBlock& b = vramFreeBlocks[i];
        if (b.size < size) continue;
        // Kernel heap and user window are disjoint: handing a kernel allocation
        // out of the mapped window would expose it to user space.
        const uint64_t windowStart = gmcContext.vramStart + VramLayout::kUserHeap;
        if (userRegion ? (b.gpuAddr < windowStart) : (b.gpuAddr >= windowStart)) continue;
        const uint64_t addr = b.gpuAddr;
        if (b.size == size) {
            vramFreeBlocks[i] = vramFreeBlocks[--vramFreeBlockCount];
        } else {
            b.gpuAddr += size;
            b.size -= size;
        }
        if (vramLiveBlockCount < kMaxVramBlocks) {
            vramLiveBlocks[vramLiveBlockCount].gpuAddr = addr;
            vramLiveBlocks[vramLiveBlockCount].size = size;
            vramLiveBlockCount++;
        }
        return addr;
    }
    return 0;
}

void RDNA4Driver::vramBlocksReset() {
    for (uint32_t i = 0; i < vramLiveBlockCount; i++) vramLiveBlocks[i] = VramBlock{};
    for (uint32_t i = 0; i < vramFreeBlockCount; i++) vramFreeBlocks[i] = VramBlock{};
    vramLiveBlockCount = 0;
    vramFreeBlockCount = 0;
}

bool RDNA4Driver::submitComputeCommands(const uint32_t* pm4Data, size_t dwordCount) {
    if (!computeRing) {
        LOG_ERROR("submitComputeCommands: compute ring not initialized");
        return false;
    }
    if (!pm4Data || dwordCount == 0) {
        LOG_ERROR("submitComputeCommands: empty command buffer");
        return false;
    }

    if (submitLock) IOLockLock(submitLock);
    const bool ok = submitComputeCommandsLocked(pm4Data, dwordCount);
    if (submitLock) IOLockUnlock(submitLock);
    return ok;
}

bool RDNA4Driver::submitComputeCommandsLocked(const uint32_t* pm4Data,
                                              size_t dwordCount) {
    LOG("submitComputeCommands: submitting %zu dwords", dwordCount);

    // Reclaim whatever the CP has already consumed before checking for space,
    // otherwise the ring looks full after ~1 MiB of lifetime submissions.
    refreshRingReadPtr();

    if (!computeRing->writePM4(pm4Data, dwordCount)) {
        LOG_ERROR("submitComputeCommands: failed to write PM4 to ring");
        return false;
    }

    // The ring lives in VRAM; flush HDP before ringing the doorbell so the
    // GPU sees the PM4 bytes, not stale cache lines.
    gmcHDPFlush(*this);

    // MES polls the queue write pointer from memory (map_legacy_queue's
    // wptr_addr). Update it before the doorbell / register kicks.
    const uint32_t wptr = computeRing->getWritePointer();
    if (computeWptrGpuAddr) {
        volatile uint32_t* wp = computeWptrMd
            ? static_cast<volatile uint32_t*>(computeWptrMd->getBytesNoCopy())
            : reinterpret_cast<volatile uint32_t*>(
                  const_cast<uint8_t*>(vram) + (computeWptrGpuAddr - gmcContext.vramStart));
        *wp = wptr;
        gmcHDPFlush(*this);
    }

    computeRing->submit();

    if (!computeQueueViaMes) {
        // Legacy paths (MEC KCQ / GFX ring): also push the write pointer through
        // the HQD register window and CP_RB0_WPTR, which do not depend on the
        // doorbell aperture.
        grbmSelect(1, 0, 0, 0);
        writeGcRegister(0x1FDF, wptr, 0);
        writeGcRegister(0x1FE0, 0, 0);
        grbmSelect(0, 0, 0, 0);
        writeGcRegister(0x1DF4, wptr, 0);
        writeGcRegister(0x1DF5, 0, 0);
    }
    // A MES-mapped queue is owned by the firmware: the HQD registers are its
    // state, so the only legal kick is the doorbell (plus the wptr page it
    // polls).  Writing CP_HQD_PQ_WPTR behind its back desynchronises the queue.
    setProperty("ComputeTest,HqdWptrWritten", static_cast<uint64_t>(wptr), 32);

    LOG("submitComputeCommands: commands submitted successfully");
    return true;
}

uint64_t RDNA4Driver::allocFenceValue() {
    return computeRing ? computeRing->allocateFence() : 0;
}

uint64_t RDNA4Driver::computeFenceGpuAddr() {
    if (!computeRing) return 0;
    return gmcContext.vramStart + computeRing->getFenceVRAMOffset();
}

bool RDNA4Driver::waitForComputeFence(uint64_t fenceValue, uint64_t timeoutMS) {
    if (!computeRing) {
        LOG_ERROR("waitForComputeFence: compute ring not initialized");
        return false;
    }

    LOG("waitForComputeFence: waiting for fence %llu (timeout %llu ms)",
        static_cast<unsigned long long>(fenceValue),
        static_cast<unsigned long long>(timeoutMS));

    bool result = computeRing->waitForFence(fenceValue, timeoutMS);

    if (result) {
        LOG("waitForComputeFence: fence %llu reached",
            static_cast<unsigned long long>(fenceValue));
    } else {
        LOG_ERROR("waitForComputeFence: timeout waiting for fence %llu",
                  static_cast<unsigned long long>(fenceValue));
    }

    return result;
}

