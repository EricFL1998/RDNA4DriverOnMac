#include "RDNA4Driver.hpp"
#include <IOKit/IOLib.h>
#include "../hardware/VramLayout.hpp"

//==============================================================================
// Phase 2.3: Compute ring and command submission
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
    // AMDGPU_DOORBELL_GFX_RING0 is 0x20; doorbell 0 belongs to the KIQ.
    const uint32_t doorbellIndex = wantMec ? 0u : 0x20u;

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

    computeHeapNext = VramLayout::kComputeHeap;

    // Phase 2.4: give the ring to the CP/MEC queue. MQD, EOP and the rptr
    // writeback all live in the compute heap right above the ring.
    const uint64_t ringGpuAddr = gmcContext.vramStart + VramLayout::kComputeRing;
    const uint64_t mqdAddr = allocateVRAMBuffer(4096);
    const uint64_t eopAddr = allocateVRAMBuffer(4096);
    const uint64_t rptrAddr = allocateVRAMBuffer(4096);
    const uint64_t wptrAddr = allocateVRAMBuffer(4096);
    computeWptrPollGpuAddr = wptrAddr;
    bool queueOk = false;

    uint32_t useMes = 0;
    const bool wantMes =
        !PE_parse_boot_argn("rdna4-mes", &useMes, sizeof(useMes)) || useMes != 0;

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
                queueOk = mesMapLegacyQueue(0, 0, doorbellIndex, mqdGpu,
                                            computeWptrGpuAddr, statusAddr);
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
        if (rptrAddr && wptrAddr) {
            queueOk = initGfxRing(ringGpuAddr,
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
    uint64_t allocSize = (static_cast<uint64_t>(size) + alignment - 1) &
                         ~(alignment - 1);
    if (allocSize < size || computeHeapNext > vramSize ||
        allocSize > vramSize - computeHeapNext) {
        LOG_ERROR("allocateVRAMBuffer: out of VRAM (need %llu, available %llu)",
                  static_cast<unsigned long long>(allocSize),
                  static_cast<unsigned long long>(vramSize - computeHeapNext));
        return 0;
    }

    const uint64_t vramOffset = computeHeapNext;
    computeHeapNext += allocSize;
    const uint64_t gpuAddress = gmcContext.vramStart + vramOffset;

    LOG("allocateVRAMBuffer: allocated %llu bytes at GPU 0x%llx (VRAM+0x%llx)",
        static_cast<unsigned long long>(allocSize),
        static_cast<unsigned long long>(gpuAddress),
        static_cast<unsigned long long>(vramOffset));

    return gpuAddress;
}

void RDNA4Driver::freeVRAMBuffer(uint64_t gpuAddress) {
    // Stub: a real allocator must track and coalesce these ranges.
    LOG("freeVRAMBuffer: GPU address 0x%llx (stub)",
        static_cast<unsigned long long>(gpuAddress));
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

    LOG("submitComputeCommands: submitting %zu dwords", dwordCount);

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

