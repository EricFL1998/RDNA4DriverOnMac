//
//  test_graphics_ring.cpp
//  Host-side check for GraphicsRing: the doorbell write must land at BAR2
//  byte offset `doorbellIndex * 4`, the DWORD-indexed aperture the HQD's
//  CP_RB_DOORBELL_CONTROL.DOORBELL_OFFSET refers to (amdgpu_mm_wdoorbell64).
//
//  build: clang++ -std=c++17 -I RDNA4Driver/tests/host-shim \
//             -I RDNA4Driver/src/hardware RDNA4Driver/tests/test_graphics_ring.cpp \
//             RDNA4Driver/src/hardware/GraphicsRing.cpp -o /tmp/test_graphics_ring
//

#include "GraphicsRing.hpp"

#include <stdio.h>
#include <string.h>

static int failures;

static void checkEq(uint64_t got, uint64_t want, const char* what) {
    if (got == want) {
        printf("  ok   %-52s 0x%llx\n", what, static_cast<unsigned long long>(got));
    } else {
        printf("  FAIL %-52s got 0x%llx want 0x%llx\n", what,
               static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
        failures++;
    }
}

static void testDoorbellLandsAtDwordIndex() {
    printf("\n[doorbell aperture]\n");
    alignas(4096) static uint8_t vram[64 * 1024];
    alignas(8) static uint32_t bar2[1024];   // 4 KiB of the doorbell BAR
    memset(vram, 0, sizeof(vram));
    // Pre-fill so a write of zero is still visible.
    memset(bar2, 0xAA, sizeof(bar2));

    const uint32_t dbIndex = 0x116;          // gfx_ring0 dword index
    GraphicsRing ring;
    const bool ok = ring.init(vram, sizeof(vram), bar2, dbIndex,
                              0, 16 * 1024, 32 * 1024);
    checkEq(ok, 1, "init accepted");

    const uint32_t pm4[4] = { 0xC0001000u, 1, 2, 3 };
    checkEq(ring.writePM4(pm4, 4), 1, "writePM4");
    ring.submit();

    // gfx_v12_0_ring_set_wptr_gfx(): WDOORBELL64(ring->doorbell_index, wptr).
    // amdgpu_mm_wdoorbell64 indexes a u32* and stores 64 bits, so both the
    // doorbell dword and the one above it are written.  Cobalt64 documents
    // that a half-write is "not guaranteed to register at all" - the CP
    // samples the pair as one 64-bit wptr.
    checkEq(bar2[dbIndex], ring.getWritePointer(), "low dword = dword write pointer");
    checkEq(bar2[dbIndex + 1], 0, "high dword = 0 (written, not left alone)");

    uint32_t untouched = 0;
    for (uint32_t i = 0; i < 1024; i++) {
        const bool ours = (i == dbIndex || i == dbIndex + 1);
        if (!ours && bar2[i] != 0xAAAAAAAAu) untouched++;
    }
    checkEq(untouched, 0, "no other doorbell dword touched");
}

int main() {
    printf("test_graphics_ring\n");
    testDoorbellLandsAtDwordIndex();
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
