#include "ComputeRing.hpp"
#include "VramLayout.hpp"

#include <stdio.h>
#include <stdint.h>

static int failures;

static void checkEq(uint64_t got, uint64_t want, const char* what) {
    if (got == want) {
        printf("  ok   %-48s 0x%llx\n", what, static_cast<unsigned long long>(got));
    } else {
        printf("  FAIL %-48s got 0x%llx want 0x%llx\n", what,
               static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
        failures++;
    }
}

int main() {
    printf("test_compute_ring\n");
    uint32_t ring[1024] = {};
    uint32_t doorbell[16] = {};
    uint64_t fence = ~0ull;

    ComputeRing computeRing;
    checkEq(computeRing.initAt(ring, 1024, &fence, doorbell, 6), 1,
            "initAt accepts caller-provided GART ring and VRAM fence");
    checkEq(fence, 0, "initAt clears the fence");
    checkEq(computeRing.getFenceVRAMOffset(), VramLayout::kComputeFence,
            "initAt preserves the VRAM fence offset");

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
