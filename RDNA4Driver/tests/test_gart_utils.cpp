#include "GARTUtils.hpp"

#include <stdio.h>
#include <stdint.h>
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

class TestDescriptor final : public IOBufferMemoryDescriptor {
public:
    int completes = 0;
    int releases = 0;
    void complete() override { completes++; }
    void release() override { releases++; }
};

static void testResetGartTable() {
    printf("\n[GART table reset]\n");
    alignas(8) uint8_t storage[4104 * sizeof(uint64_t)];
    GMCContext gmc;
    gmc.gartTableVramOffset = 16;
    gmc.gartTableSize = 4100 * sizeof(uint64_t);
    gmc.gartNextPage = 0x1002;
    volatile uint64_t* table = reinterpret_cast<volatile uint64_t*>(storage + 16);
    for (uint32_t i = 0; i < 4100; i++) table[i] = 0xAAAAAAAAAAAAAAAAull;
    constexpr uint64_t dummy = 0x123456789ab00000ull;

    checkEq(gmcResetGartTable(storage, sizeof(storage), gmc, dummy), 1,
            "reset accepts the allocated GART range");
    checkEq(table[0], 0xAAAAAAAAAAAAAAAAull, "reserved first PTE stays untouched");
    checkEq(table[0x0fff], 0xAAAAAAAAAAAAAAAAull, "reserved last PTE stays untouched");
    checkEq(table[0x1000], dummy, "first allocated PTE becomes dummy");
    checkEq(table[0x1001], dummy, "last allocated PTE becomes dummy");
    checkEq(table[0x1002], 0xAAAAAAAAAAAAAAAAull, "first unallocated PTE stays untouched");
    checkEq(gmc.gartNextPage, 0x1000, "allocator rewinds to the first free page");

    GMCContext invalid;
    invalid.gartTableSize = sizeof(uint64_t);
    uint64_t one = 0x55;
    checkEq(gmcResetGartTable(reinterpret_cast<volatile uint8_t*>(&one),
                              sizeof(one), invalid, dummy), 0,
            "invalid table bounds are rejected");
    checkEq(one, 0x55, "invalid table is not modified");
}

static void testDescriptorRelease() {
    printf("\n[GART descriptor release]\n");
    TestDescriptor descriptor;
    IOBufferMemoryDescriptor* md = &descriptor;
    gmcReleaseGartBuffer(&md);
    checkEq(descriptor.completes, 1, "prepared descriptor is completed once");
    checkEq(descriptor.releases, 1, "descriptor is released once");
    checkEq(md == nullptr, 1, "caller pointer is cleared");
    gmcReleaseGartBuffer(&md);
    checkEq(descriptor.completes, 1, "null release does not complete again");
    checkEq(descriptor.releases, 1, "null release does not release again");
}

int main() {
    printf("test_gart_utils\n");
    testResetGartTable();
    testDescriptorRelease();
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
