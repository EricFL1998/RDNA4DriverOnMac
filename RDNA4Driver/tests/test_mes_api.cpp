//
//  test_mes_api.cpp
//  Host-side checks for the MES API frames the driver hands to the scheduler
//  firmware. No hardware and no IOKit: the dword positions are the ones
//  mes_v12_api_def.h produces under `#pragma pack(push, 8)`, measured with
//  clang's offsetof on the upstream header.
//
//  build: clang++ -std=c++17 -I RDNA4Driver/src/hardware \
//             RDNA4Driver/tests/test_mes_api.cpp -o /tmp/test_mes_api
//

#include "MesApi.hpp"

#include <stdio.h>
#include <string.h>

static int failures;

static void checkEq(uint32_t got, uint32_t want, const char* what) {
    if (got == want) {
        printf("  ok   %-52s 0x%08x\n", what, got);
    } else {
        printf("  FAIL %-52s got 0x%08x want 0x%08x\n", what, got, want);
        failures++;
    }
}

static void testAddQueueLayout() {
    printf("\n[MESAPI__ADD_QUEUE legacy kernel queue]\n");
    uint32_t pkt[MesApi::kFrameDwords];
    memset(pkt, 0xA5, sizeof(pkt));

    MesApi::AddQueueInput in {};
    in.doorbellOffset = 0x40;
    in.mqdAddr = 0x0000001122334450ull;
    in.wptrAddr = 0x0000000899AABBC0ull;
    in.queueType = MesApi::kQueueTypeGfx;
    in.pipe = 0;
    in.queue = 0;
    in.fenceAddr = 0x00000000F0F0F000ull;
    in.fenceValue = 1;
    MesApi::buildAddQueueLegacy(pkt, in);

    checkEq(pkt[0], 1u | (2u << 4) | (64u << 12), "header: scheduler/ADD_QUEUE/64 dw");
    checkEq(pkt[18], 0x40u, "doorbell_offset @ dw18");
    checkEq(pkt[20], 0x22334450u, "mqd_addr lo @ dw20");
    checkEq(pkt[21], 0x00000011u, "mqd_addr hi @ dw21");
    checkEq(pkt[22], 0x99AABBC0u, "wptr_addr lo @ dw22");
    checkEq(pkt[23], 0x00000008u, "wptr_addr hi @ dw23");
    checkEq(pkt[28], 0u, "queue_type GFX @ dw28");
    checkEq(pkt[36], 0u, "vm_context_cntl @ dw36 is zero");
    checkEq(pkt[37], 1u << 13, "flags @ dw37 carries only map_legacy_kq");
    checkEq(pkt[38], 0xF0F0F000u, "api_completion_fence_addr lo @ dw38");
    checkEq(pkt[39], 0u, "api_completion_fence_addr hi @ dw39");
    checkEq(pkt[40], 1u, "api_completion_fence_value lo @ dw40");
    checkEq(pkt[41], 0u, "api_completion_fence_value hi @ dw41");
    checkEq(pkt[50], 0u, "pipe_id @ dw50");
    checkEq(pkt[51], 0u, "queue_id @ dw51");

    // mes_v12_0_map_legacy_queue() sets nothing else: process/gang context,
    // page table, h_context/h_queue, priorities all stay zero.
    uint32_t nonZeroElsewhere = 0;
    for (uint32_t i = 1; i < MesApi::kFrameDwords; i++) {
        const bool expected = i == 18 || i == 20 || i == 21 || i == 22 || i == 23 ||
                              i == 28 || i == 37 || i == 38 || i == 40;
        if (!expected && pkt[i] != 0) nonZeroElsewhere++;
    }
    checkEq(nonZeroElsewhere, 0, "every other dword is zero");
}

static void testAddQueueComputePipeQueue() {
    printf("\n[MESAPI__ADD_QUEUE compute on MEC pipe1/queue2]\n");
    uint32_t pkt[MesApi::kFrameDwords];
    MesApi::AddQueueInput in {};
    in.queueType = MesApi::kQueueTypeCompute;
    in.pipe = 1;
    in.queue = 2;
    in.fenceValue = 7;
    MesApi::buildAddQueueLegacy(pkt, in);
    checkEq(pkt[28], 1u, "queue_type COMPUTE @ dw28");
    checkEq(pkt[40], 7u, "fence value @ dw40");
    checkEq(pkt[50], 1u, "pipe_id @ dw50");
    checkEq(pkt[51], 2u, "queue_id @ dw51");
}

static void testGfxHqdRegisterSegments() {
    printf("\n[CP_GFX_HQD_* register block]\n");
    // gc_12_0_0_offset.h: every CP_GFX_HQD_* / CP_GFX_MQD_* register has
    // BASE_IDX 0. Reading them through segment 1 lands in reserved space.
    checkEq(MesApi::GfxHqd::kActive, 0x1E80u, "regCP_GFX_HQD_ACTIVE");
    checkEq(MesApi::GfxHqd::kBase, 0x1E86u, "regCP_GFX_HQD_BASE");
    checkEq(MesApi::GfxHqd::kWptr, 0x1E91u, "regCP_GFX_HQD_WPTR");
    checkEq(MesApi::GfxHqd::kMapped, 0x1E94u, "regCP_GFX_HQD_MAPPED");
    checkEq(MesApi::GfxHqd::kSegment, 0u, "BASE_IDX of the CP_GFX_HQD block");

    // gfx_v12_0_gfx_mqd_init starts with regCP_GFX_HQD_CNTL_DEFAULT and only
    // overrides RB_BUFSZ/RB_BLKSZ. KMD_QUEUE and RB_EXE stay clear on GFX12.
    checkEq(MesApi::GfxHqd::kCntlKmdQueue, 0x20000000u, "KMD_QUEUE is bit 29");
    checkEq(MesApi::GfxHqd::kCntlRbExe, 0x10000000u, "RB_EXE is bit 28");
    for (uint32_t bufsz = 8; bufsz <= 18; bufsz++) {
        const uint32_t v = MesApi::GfxHqd::cntlValue(bufsz);
        checkEq((v & 0x20000000u) == 0, 1, "GFX12 cntlValue leaves KMD_QUEUE clear");
        checkEq((v & 0x10000000u) == 0, 1, "GFX12 cntlValue leaves RB_EXE clear");
        checkEq(v & 0x3Fu, bufsz, "cntlValue keeps RB_BUFSZ");
        checkEq((v >> 8) & 0x3Fu, bufsz - 2, "cntlValue keeps RB_BLKSZ = bufsz-2");
    }
    checkEq(MesApi::GfxHqd::cntlValue(17), 0x00F00F11u, "GFX12 cntl for a 1 MiB ring");
}

static void testDoorbellWindows() {
    printf("\n[doorbell index map and CP range windows]\n");
    // soc24 / amdgpu_doorbell.h (Navi10 map): compute block first, then the
    // graphics block. CP_MEC_DOORBELL_RANGE and CP_RB_DOORBELL_RANGE must not
    // overlap, otherwise a GFX doorbell is treated as a compute one.
    checkEq(MesApi::Doorbell::kKiq, 0x000u, "kiq");
    checkEq(MesApi::Doorbell::kMecRing0, 0x003u, "mec_ring0");
    checkEq(MesApi::Doorbell::kMesRing0, 0x00Bu, "mes_ring0");
    checkEq(MesApi::Doorbell::kUserQueueEnd, 0x08Au, "userqueue_end");
    checkEq(MesApi::Doorbell::kGfxRing0, 0x08Bu, "gfx_ring0");
    checkEq(MesApi::Doorbell::kGfxUserQueueEnd, 0x0FFu, "gfx_userqueue_end");
    checkEq(MesApi::Doorbell::kUserQueueEnd < MesApi::Doorbell::kGfxRing0, 1,
            "compute window ends before the graphics window starts");

    // gfx_v12_0_cp_set_doorbell_range(): (index * 2) << 2.
    checkEq(MesApi::Doorbell::rangeValue(MesApi::Doorbell::kGfxRing0), (0x8Bu * 2u) << 2,
            "CP_RB_DOORBELL_RANGE_LOWER encoding");
    checkEq(MesApi::Doorbell::rangeValue(MesApi::Doorbell::kKiq), 0u,
            "CP_MEC_DOORBELL_RANGE_LOWER encoding");
    checkEq(MesApi::Doorbell::rangeValue(MesApi::Doorbell::kUserQueueEnd), (0x8Au * 2u) << 2,
            "CP_MEC_DOORBELL_RANGE_UPPER encoding");
    checkEq(MesApi::Doorbell::rangeValue(MesApi::Doorbell::kGfxUserQueueEnd), (0xFFu * 2u) << 2,
            "CP_RB_DOORBELL_RANGE_UPPER encoding");
}

// The map above is in 64-bit doorbell units.  The ring's doorbell_index
// (MQD CP_RB_DOORBELL_CONTROL.DOORBELL_OFFSET, the BAR2 dword index the
// driver writes, and amdgpu_mm_wdoorbell64's index) is the DWORD index, i.e.
// the map entry shifted left by one.  x86_64 0.8.3 shipped the map entry
// itself and rang its doorbell at byte 0x22c while CP_RB_DOORBELL_RANGE_LOWER
// said 0x458 - the CP dropped every kick.
static void testDoorbellIndexUnits() {
    printf("\n[doorbell index units: 64-bit map entry vs dword index]\n");
    checkEq(MesApi::Doorbell::kGfxRingDword, 0x116u, "gfx_ring0 dword index");
    checkEq(MesApi::Doorbell::kGfxRingDword, 0x8Bu << 1, "dword index == map entry << 1");
    checkEq(MesApi::Doorbell::byteOffset(MesApi::Doorbell::kGfxRingDword), 0x458u,
            "dword index -> byte offset (== RB_RANGE_LOWER)");
    checkEq(MesApi::Doorbell::byteOffset(MesApi::Doorbell::kGfxRingDword),
            MesApi::Doorbell::rangeValue(MesApi::Doorbell::kGfxRing0),
            "doorbell byte offset lands exactly on CP_RB_DOORBELL_RANGE_LOWER");
    checkEq(MesApi::Doorbell::byteOffset(MesApi::Doorbell::kMesRing0Dword), 0x58u,
            "mes_ring0 byte offset");
    checkEq(MesApi::Doorbell::byteOffset(MesApi::Doorbell::kMecRing0Dword), 0x18u,
            "mec_ring0 byte offset");
    checkEq(MesApi::Doorbell::byteOffset(MesApi::Doorbell::kMesRing0Dword) <
                MesApi::Doorbell::rangeValue(MesApi::Doorbell::kUserQueueEnd), 1,
            "MES rings stay inside the MEC window");
}

static void testCompletionStatus() {
    printf("\n[MES API completion status]\n");
    checkEq(MesApi::completionSucceeded(1, 1), 1, "matching fence succeeds");
    checkEq(MesApi::completionSucceeded(0, 1), 0, "cleared status stays pending");
    checkEq(MesApi::completionSucceeded(2, 1), 0, "a different fence is not success");
    checkEq(MesApi::completionSucceeded(0x80000001ull, 1), 0,
            "error status never succeeds");
    checkEq(MesApi::completionFailed(0x80000001ull), 1,
            "bit 31 reports an API error");
    checkEq(MesApi::completionFailed(1), 0, "ordinary completion is not an error");

    checkEq(MesApi::nextCompletionFence(0), 1, "first completion sequence is nonzero");
    checkEq(MesApi::nextCompletionFence(1), 2, "completion sequence increments");
    checkEq(MesApi::nextCompletionFence(0x7ffffffeu), 0x7fffffffu,
            "completion sequence reaches the 31-bit maximum");
    checkEq(MesApi::nextCompletionFence(0x7fffffffu), 1,
            "completion sequence wraps without zero or bit 31");
    checkEq(MesApi::completionStatusDword(MesApi::kOpSetHwResources), 50,
            "SET_HW_RESOURCES status dword");
    checkEq(MesApi::completionStatusDword(MesApi::kOpAddQueue), 38,
            "ADD_QUEUE status dword");
    checkEq(MesApi::completionStatusDword(MesApi::kOpQuerySchedulerStatus), 2,
            "QUERY status dword");
    checkEq(MesApi::completionStatusDword(MesApi::kOpSetHwResources1), 2,
            "SET_HW_RESOURCES_1 status dword");
    checkEq(MesApi::completionStatusDword(0xff), 0, "unknown opcode has no status layout");
}

int main() {
    printf("test_mes_api\n");
    testAddQueueLayout();
    testAddQueueComputePipeQueue();
    testGfxHqdRegisterSegments();
    testDoorbellWindows();
    testDoorbellIndexUnits();
    testCompletionStatus();
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
