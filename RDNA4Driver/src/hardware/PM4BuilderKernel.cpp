#include "PM4BuilderKernel.hpp"
#include <IOKit/IOLib.h>

PM4BuilderKernel::PM4BuilderKernel() : packetCount(0) {
}

PM4BuilderKernel::~PM4BuilderKernel() {
}

void PM4BuilderKernel::reset() {
    packetCount = 0;
}

uint32_t PM4BuilderKernel::makeType3Header(uint8_t opcode, uint32_t count,
                                           uint32_t flags) {
    return (PM4_TYPE3 << PM4_TYPE3_SHIFT) |
           ((count & 0x3FFFu) << 16) |
           ((uint32_t)opcode << 8) |
           (flags & 0xFFu);
}

void PM4BuilderKernel::append(uint32_t dword) {
    if (packetCount < MAX_PACKETS) {
        packets[packetCount++] = dword;
    } else {
        IOLog("PM4BuilderKernel: packet buffer overflow!\n");
    }
}

void PM4BuilderKernel::setComputeProgram(uint64_t shaderGpuAddr,
                                          uint32_t rsrc1,
                                          uint32_t rsrc2) {
    // COMPUTE_PGM_LO holds address bits [39:8], COMPUTE_PGM_HI bits [47:40].
    uint32_t lo = (uint32_t)(shaderGpuAddr >> 8);
    uint32_t hi = (uint32_t)((shaderGpuAddr >> 40) & 0xFFu);

    // PGM_LO/PGM_HI are adjacent, and so are RSRC1/RSRC2, but the two pairs
    // are not: 0x20e-0x211 belong to COMPUTE_DISPATCH_PKT_ADDR and
    // COMPUTE_DISPATCH_SCRATCH_BASE, so each pair needs its own packet.
    append(makeType3Header(PM4_OP_SET_SH_REG, 2, PM4_COMPUTE_FLAG));
    append(GFX12_COMPUTE_PGM_LO);
    append(lo);
    append(hi);

    append(makeType3Header(PM4_OP_SET_SH_REG, 2, PM4_COMPUTE_FLAG));
    append(GFX12_COMPUTE_PGM_RSRC1);
    append(rsrc1);
    append(rsrc2);

    // (c) RESOURCE_LIMITS = 0: no WAVES_PER_SH / TG_PER_CU caps.
    append(makeType3Header(PM4_OP_SET_SH_REG, 1, PM4_COMPUTE_FLAG));
    append(GFX12_COMPUTE_RESOURCE_LIMITS);
    append(0u);

    // (d) CU masks SE0/SE1.  gfx10+ writes these through SET_SH_REG_INDEX with
    // INDEX=3 (per-SE broadcast); a plain SET_SH_REG leaves them unchanged and
    // a dispatch with no CUs enabled launches almost nothing.
    append(makeType3Header(PM4_OP_SET_SH_REG_INDEX, 2, PM4_COMPUTE_FLAG));
    append(GFX12_COMPUTE_STATIC_THREAD_MGMT_SE0 | (3u << 28));
    append(0xFFFFFFFFu);
    append(0xFFFFFFFFu);

    // (e) TMPRING_SIZE = 0: no scratch ring.
    append(makeType3Header(PM4_OP_SET_SH_REG, 1, PM4_COMPUTE_FLAG));
    append(GFX12_COMPUTE_TMPRING_SIZE);
    append(0u);

    // (f) CU masks SE2/SE3, same INDEX=3 form.
    append(makeType3Header(PM4_OP_SET_SH_REG_INDEX, 2, PM4_COMPUTE_FLAG));
    append(GFX12_COMPUTE_STATIC_THREAD_MGMT_SE2 | (3u << 28));
    append(0xFFFFFFFFu);
    append(0xFFFFFFFFu);

    // (g) RSRC3: no shared VGPRs, no instruction prefetch beyond the blob.
    append(makeType3Header(PM4_OP_SET_SH_REG, 1, PM4_COMPUTE_FLAG));
    append(GFX12_COMPUTE_PGM_RSRC3);
    append(0u);
}

void PM4BuilderKernel::setComputeWorkgroupSize(uint32_t x, uint32_t y, uint32_t z) {
    append(makeType3Header(PM4_OP_SET_SH_REG, 3, PM4_COMPUTE_FLAG));
    append(GFX12_COMPUTE_NUM_THREAD_X);
    append(x);
    append(y);
    append(z);
}

void PM4BuilderKernel::setComputeUserData(uint32_t index, uint64_t gpuAddress) {
    // Each user data slot is a pair of SGPRs (64-bit address).
    // Index 0 = USER_DATA_0/1, index 1 = USER_DATA_2/3, and so on.
    uint32_t lo = (uint32_t)(gpuAddress & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(gpuAddress >> 32);
    uint32_t regOffset = GFX12_COMPUTE_USER_DATA_0 + (index * 2);

    append(makeType3Header(PM4_OP_SET_SH_REG, 2, PM4_COMPUTE_FLAG));
    append(regOffset);
    append(lo);
    append(hi);
}

void PM4BuilderKernel::emitDispatchDirect(uint32_t gridX, uint32_t gridY,
                                          uint32_t gridZ) {
    // DISPATCH_DIRECT: [dim_x][dim_y][dim_z][initiator], dims in workgroups.
    append(makeType3Header(PM4_OP_DISPATCH_DIRECT, 3, PM4_COMPUTE_FLAG));
    append(gridX);
    append(gridY);
    append(gridZ);
    append(DISPATCH_INITIATOR_DEFAULT);
}

void PM4BuilderKernel::emitCsPartialFlush() {
    // EVENT_WRITE with CS_PARTIAL_FLUSH; index 4 per soc15d.h.
    append(makeType3Header(PM4_OP_EVENT_WRITE, 0));
    append(PM4_EVENT_TYPE(EVENT_TYPE_CS_PARTIAL_FLUSH) |
           PM4_EVENT_INDEX(EVENT_INDEX_CS_PARTIAL_FLUSH));
}

void PM4BuilderKernel::emitFenceWrite(uint64_t fenceGpuAddr, uint64_t fenceValue) {
    // RELEASE_MEM with 7 payload dwords (count field 6), matching
    // gfx_v12_0_ring_emit_fence(). Writes a 64-bit value and flushes L2.
    uint32_t addrLo = (uint32_t)(fenceGpuAddr & 0xFFFFFFFFu);
    uint32_t addrHi = (uint32_t)(fenceGpuAddr >> 32);
    uint32_t dataLo = (uint32_t)(fenceValue & 0xFFFFFFFFu);
    uint32_t dataHi = (uint32_t)(fenceValue >> 32);

    append(makeType3Header(PM4_OP_RELEASE_MEM, 6));
    append(RELEASE_MEM_GCR_SEQ |
           RELEASE_MEM_GCR_GL2_WB |
           RELEASE_MEM_CACHE_POLICY(3) |
           RELEASE_MEM_EVENT_TYPE(RELEASE_MEM_CACHE_FLUSH_AND_INV_TS_EVENT) |
           RELEASE_MEM_EVENT_INDEX(5));
    append(RELEASE_MEM_DATA_SEL(2) | RELEASE_MEM_INT_SEL(2));
    append(addrLo & 0xFFFFFFFCu);
    append(addrHi);
    append(dataLo);
    append(dataHi);
    append(0);
}

void PM4BuilderKernel::emitWriteData(uint64_t dstGpuAddr, uint32_t value) {
    // PACKET3_WRITE_DATA, count 3 (4 payload dwords): control, addr lo/hi,
    // data.  ENGINE_SEL=1 (ME), DST_SEL=5 (memory), WR_CONFIRM.
    append(makeType3Header(PM4_OP_WRITE_DATA, 3));
    append((1u << 30) | (5u << 8) | (1u << 20));
    append(static_cast<uint32_t>(dstGpuAddr & 0xFFFFFFFCu));
    append(static_cast<uint32_t>(dstGpuAddr >> 32));
    append(value);
}

void PM4BuilderKernel::emitIndirectBuffer(uint64_t ibGpuAddr, uint32_t lengthDw,
                                          uint32_t vmid) {
    // gfx_v12_0_ring_emit_ib_compute: a NON compute-typed header, control
    // dword = INDIRECT_BUFFER_VALID | length | (vmid << 24).  Without VALID
    // the MEC takes the doorbell, clears PQ_EMPTY, and never runs the IB.
    append(makeType3Header(PM4_OP_INDIRECT_BUFFER, 2));
    append(static_cast<uint32_t>(ibGpuAddr & 0xFFFFFFFCu));
    append(static_cast<uint32_t>(ibGpuAddr >> 32));
    append((1u << 23) | (lengthDw & 0xFFFFFu) | ((vmid & 0xFu) << 24));
}
