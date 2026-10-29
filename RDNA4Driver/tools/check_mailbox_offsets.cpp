// Host check for the Phase 1.3 read-only mailbox map.
// Compiles the kernel IPDiscovery parser against a real Navi 48 ROM and
// asserts the PSP/SMU status registers resolve inside a real BAR5.

#include "../src/hardware/IPDiscovery.cpp"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <fstream>

static constexpr uint16_t kHwMp0 = IPDiscovery::HwMp0;
static constexpr uint16_t kHwMp1 = IPDiscovery::HwMp1;
static constexpr uint8_t kPspSegment = 0;
static constexpr uint32_t kPspBootDword = 0x0063;
static constexpr uint32_t kPspRingDword = 0x0080;
static constexpr uint32_t kPspSignOfLifeDword = 0x0091;
static constexpr uint8_t kSmuSegment = 1;
static constexpr uint32_t kSmuResponseDword = 0x009a;
static constexpr uint32_t kBar5Size = 0x800000;

static int fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 2)
        return fail("usage: check_mailbox_offsets <navi48.rom>");

    std::ifstream rom(argv[1], std::ios::binary);
    if (!rom)
        return fail("cannot open ROM");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(rom)),
                               std::istreambuf_iterator<char>());

    IPDiscovery discovery;
    bool found = false;
    for (size_t offset = 0; offset + 8 < bytes.size(); offset++) {
        if (discovery.init(bytes.data() + offset, bytes.size() - offset)) {
            found = true;
            break;
        }
    }
    if (!found)
        return fail("ROM contains no valid IP discovery binary");

    struct Expected {
        const char* name;
        uint16_t hwId;
        uint8_t segment;
        uint32_t dword;
    };
    const Expected registers[] = {
        {"PSP bootloader", kHwMp0, kPspSegment, kPspBootDword},
        {"PSP ring", kHwMp0, kPspSegment, kPspRingDword},
        {"PSP sign of life", kHwMp0, kPspSegment, kPspSignOfLifeDword},
        {"SMU response", kHwMp1, kSmuSegment, kSmuResponseDword},
    };

    for (const Expected& reg : registers) {
        uint32_t byteOffset = 0;
        if (!discovery.regByteOffset(reg.hwId, 0, reg.segment, reg.dword, byteOffset))
            return fail(reg.name);
        if (byteOffset > kBar5Size || kBar5Size - byteOffset < sizeof(uint32_t))
            return fail("register outside BAR5");
        std::printf("%s byte offset 0x%08x\n", reg.name, byteOffset);
    }
    return 0;
}
