// Host checks for the Phase 1.3 PSP loader.
// Expectations are hand-read from psp_14_0_3_sos.bin, not derived by the loader.

#include "../src/hardware/IPDiscovery.cpp"
#include "../src/hardware/PSPLoader.hpp"
#include "../src/hardware/PSPLoader.cpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

namespace {

int gFailures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        gFailures++;
    }
}

std::vector<uint8_t> readFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
}

struct ScriptedHardware : PSPHardware {
    uint32_t boot = 0x80000000;
    int readyFor = 1000;
    int busyAfterCommand = 0;
    int busyReads = 0;
    int ringNotReadyFor = 0;
    int waitsBeforeKick = 0;
    int flushes = 0;
    const char* failure = nullptr;
    int waits = 0;
    uint32_t signOfLife = 0;
    uint32_t firmwareVersion = 0;
    uint32_t featureVersion = 0;
    uint32_t ringResponse = 0x80000000;
    uint32_t fbBase = 0x00000080;
    uint32_t fbTop = 0x000004ff;
    uint64_t vramBytes = 16ULL << 30;
    std::vector<uint8_t> vram;
    struct Write { uint32_t dword; uint32_t value; };
    std::vector<Write> writes;

    ScriptedHardware() : vram(64u << 20, 0xA5) {}

    uint32_t writePointer = 0;
    uint32_t fence = 0;

    uint32_t read(Block block, uint32_t dword) override {
        if (block == Block::PSP && dword == 0x0063) {
            if (busyReads > 0) {
                busyReads--;
                return boot & ~0x80000000u;
            }
            if (readyFor <= 0) return boot & ~0x80000000u;
            readyFor--;
            return boot;
        }
        if (block == Block::PSP && dword == 0x0091) return signOfLife;
        if (block == Block::PSP && dword == 0x007a) return firmwareVersion;
        if (block == Block::PSP && dword == 0x0083) return writePointer;
        if (block == Block::PSP && dword == 0x007b) return featureVersion;
        if (block == Block::PSP && dword == 0x0080) {
            if (ringNotReadyFor > 0) {
                ringNotReadyFor--;
                return 0;
            }
            return ringResponse;
        }
        if (block == Block::MemoryHub && dword == 0x0554) return fbBase;
        if (block == Block::MemoryHub && dword == 0x0555) return fbTop;
        return 0xFFFFFFFF;
    }

    bool write(Block block, uint32_t dword, uint32_t value) override {
        if (block != Block::PSP) return false;
        writes.push_back({dword, value});
        if (dword == 0x0083) {
            writePointer = value;
            if (value == 16) {
                const uint32_t done = 1;
                std::memcpy(vram.data() + (20ull << 20), &done, 4);
                fence = 1;
            }
        }
        if (dword == 0x0080 && value == 0x00020000 && (ringResponse & 0xffffu) == 0) {
            waitsBeforeKick = waits;
            ringResponse = 0x80000000;
        }
        if (dword == 0x0063 && busyAfterCommand > 0)
            busyReads = busyAfterCommand;
        if (dword == 0x0063 && value == 0x00020000) {
            signOfLife = 0x003a1214;
            firmwareVersion = 0x003a1214;
        }
        return true;
    }

    bool fill(uint64_t offset, uint8_t value, size_t length) override {
        if (offset > vram.size() || length > vram.size() - offset) {
            failure = "fill";
            return false;
        }
        std::memset(vram.data() + offset, value, length);
        return true;
    }

    bool flush(uint64_t registerBytes) override {
        if (registerBytes < 4) {
            failure = "flush";
            return false;
        }
        flushes++;
        return true;
    }

    void wait(uint32_t milliseconds) override {
        (void)milliseconds;
        waits++;
        if (ringNotReadyFor > 0) ringNotReadyFor--;
    }

    uint32_t pointer() override { return writePointer; }

    uint32_t readVRAM(uint64_t offset) override {
        uint32_t value = 0;
        std::memcpy(&value, vram.data() + offset, 4);
        return value;
    }

    bool copy(uint64_t offset, const void* source, size_t length) override {
        if (offset == (20ull << 20) && length == 4) {
            uint32_t value = 0;
            std::memcpy(&value, source, 4);
            if (value == 1) fence = 1;
        }
        if (offset > vram.size() || length > vram.size() - offset) {
            failure = "copy";
            return false;
        }
        std::memcpy(vram.data() + offset, source, length);
        return true;
    }
};

void checkRealContainer(const std::vector<uint8_t>& firmware) {
    PSPLoader loader;
    expect(loader.parse(firmware.data(), firmware.size()), "real container parses");
    const PSPImage* images[] = {
        loader.image("KDB"), loader.image("SPL"), loader.image("SYS_DRV"),
        loader.image("SOC_DRV"), loader.image("INTF_DRV"), loader.image("HAD_DRV"),
        loader.image("RAS_DRV"), loader.image("IPKEYMGR"), loader.image("SOS"),
    };
    const uint32_t sizes[] = {6816, 832, 78272, 103792, 37280, 53344, 12704, 12848, 125472};
    const uint32_t versions[] = {
        0x703b0001, 0x01003a00, 0x003a1214, 0x003a1214, 0x003a1214,
        0x003a1214, 0x003a1214, 0x003a1214, 0x003a1214,
    };
    for (size_t i = 0; i < 9; i++) {
        expect(images[i] && images[i]->size == sizes[i], "image size");
        expect(images[i] && images[i]->version == versions[i], "image version");
    }
    expect(loader.image("SOS") && loader.image("SOS")->bytes &&
           loader.image("SOS")->bytes[0] == 0xf8 && loader.image("SOS")->bytes[1] == 0x24,
           "SOS payload bytes");
}

void checkRejectedContainers() {
    PSPLoader loader;
    uint8_t empty[32] = {};
    expect(!loader.parse(empty, sizeof(empty)), "zero header rejected");

    std::vector<uint8_t> truncated(64, 0);
    truncated[16] = 2;
    uint32_t count = 1;
    std::memcpy(truncated.data() + 32, &count, 4);
    expect(!loader.parse(truncated.data(), truncated.size()), "truncated descriptor rejected");

    std::vector<uint8_t> escaped = truncated;
    uint32_t descriptor[] = {1, 0, 1000, 16};
    std::memcpy(escaped.data() + 36, descriptor, sizeof(descriptor));
    uint32_t arrayOffset = 36;
    std::memcpy(escaped.data() + 24, &arrayOffset, 4);
    expect(!loader.parse(escaped.data(), escaped.size()), "escaped image rejected");
}

void checkLoadOrder(const std::vector<uint8_t>& firmware) {
    ScriptedHardware hardware;
    PSPLoader loader;
    expect(loader.parse(firmware.data(), firmware.size()), "container ready");
    expect(!loader.setStagingOffset(0x100000 + 4096), "misaligned staging rejected");
    expect(loader.setStagingOffset(16ull << 20), "aligned staging accepted");
    expect(loader.load(hardware, 16ull << 30, 8ull << 20), "chain loads");

    const uint32_t commands[] = {
        0x00080000, 0x10000000, 0x00010000, 0x000B0000, 0x000D0000,
        0x000C0000, 0x000E0000, 0x000F0000, 0x00020000,
    };
    size_t seen = 0;
    uint32_t previous = 0;
    for (const auto& write : hardware.writes) {
        if (write.dword == 0x0064) {
            expect(write.value == 0x00000810, "staging MC address");
            previous = 0x0064;
        } else if (write.dword == 0x0063) {
            expect(previous == 0x0064, "address written before command");
            expect(seen < 9 && write.value == commands[seen], "command order");
            seen++;
            previous = 0x0063;
        }
    }
    expect(seen == 9, "nine load commands");
    expect(hardware.flushes == 9, "firmware writes are flushed before each PSP command");

    std::ifstream romFile("/tmp/RDNA4FB/firmware/Sapphire.RX9070XT.16384.241213.rom", std::ios::binary);
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(romFile)), {});
    IPDiscovery discovery;
    bool foundDiscovery = false;
    for (size_t offset = 0; offset + 8 < rom.size(); offset++) {
        if (discovery.init(rom.data() + offset, rom.size() - offset)) {
            foundDiscovery = true;
            break;
        }
    }
    uint32_t flushOffset = 0;
    expect(foundDiscovery && discovery.regByteOffset(IPDiscovery::HwNbif, 0, 2, 0x012d, flushOffset),
           "NBIF flush remap resolves");
    expect(flushOffset == 0x3934, "NBIF segment 2 dword 0x012d is byte 0x3934");
    std::ifstream driver("RDNA4Driver/src/core/RDNA4Driver.cpp");
    std::string driverText((std::istreambuf_iterator<char>(driver)), {});
    expect(driverText.find("0x012d") != std::string::npos, "driver flushes NBIF dword 0x012d");
    expect(driverText.find("0x00e6") == std::string::npos, "driver does not use dword 0x00e6");
    expect(driverText.find("0x0DE3 * 4") == std::string::npos, "flush barrier is not shifted again");
    expect(driverText.find("readRegister(0x378C)") != std::string::npos, "flush barrier reads byte 0x378C");
    expect(driverText.find("load(hardware, vramSize, rmmioSize)") != std::string::npos,
           "firmware load flushes against the mapped BAR5");
    expect(std::memcmp(hardware.vram.data() + (16ull << 20),
                       loader.image("SOS")->bytes, 125472) == 0,
           "SOS remains staged");

    PSPVersions versions;
    expect(loader.readVersions(hardware, versions), "running versions readable");
    expect(versions.signOfLife == 0x003a1214 && versions.firmware == 0x003a1214 &&
           versions.feature == 0x00000000, "version registers");
    expect(loader.command(hardware, 17ull << 20, 8ull << 20), "ring command is accepted");
    bool sawAddress = false, sawSize = false, sawKick = false;
    for (const auto& write : hardware.writes) {
        if (write.dword == 0x0085 && write.value == 0x81100000) sawAddress = true;
        if (write.dword == 0x0087 && write.value == 4096) sawSize = true;
        if (write.dword == 0x0080 && write.value == 0x00020000) sawKick = true;
    }
    expect(sawAddress && sawSize && sawKick, "ring address, size, then command");
    bool pointerReset = false;
    for (const auto& write : hardware.writes)
        if (write.dword == 0x0083 && write.value == 0) pointerReset = true;
    expect(pointerReset, "new ring pointer starts at zero");
    const bool cleared = hardware.vram[17ull << 20] == 0 &&
                         hardware.vram[(17ull << 20) + 4095] == 0;
    expect(cleared, "ring buffer is cleared");

    ScriptedHardware rejected;
    rejected.signOfLife = 0x003a1214;
    rejected.ringResponse = 0x80000115;
    expect(!loader.command(rejected, 17ull << 20, 8ull << 20), "PSP error status is not success");
    expect(rejected.waits < 100, "PSP error status returns without the full timeout");

    ScriptedHardware delayedRing;
    delayedRing.signOfLife = 0x003a1214;
    delayedRing.ringNotReadyFor = 3;
    expect(loader.command(delayedRing, 17ull << 20, 8ull << 20), "ring command waits until PSP is ready");
    expect(delayedRing.writePointer == 0, "successful ring leaves the pointer at zero");
    expect(delayedRing.waitsBeforeKick == 2, "ring command waits out the not-ready reads");

    hardware.writePointer = 0;
    const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44};
    expect(loader.loadFirmware(hardware, 18ull << 20, payload, sizeof(payload), 18,
                               17ull << 20, 19ull << 20, 20ull << 20, 8ull << 20),
           "PSP accepts an IP firmware frame");
    expect(std::memcmp(hardware.vram.data() + (18ull << 20), payload, sizeof(payload)) == 0,
           "IP firmware payload is staged");
    expect(hardware.readVRAM((19ull << 20) + 28) == 0x81200000, "command names the firmware MC address");
    const uint32_t commandLow = hardware.readVRAM(17ull << 20);
    const uint32_t commandHigh = hardware.readVRAM((17ull << 20) + 4);
    const uint32_t fenceLow = hardware.readVRAM((17ull << 20) + 12);
    const uint32_t fenceHigh = hardware.readVRAM((17ull << 20) + 16);
    expect(commandLow == 0x81300000 && commandHigh == 0 && fenceLow == 0x81400000 && fenceHigh == 0,
           "ring frame stores low addresses before their high halves");
    uint32_t commandId = 0;
    std::memcpy(&commandId, hardware.vram.data() + (19ull << 20) + 8, 4);
    expect(commandId == 6, "frame command is LOAD_IP_FW");
    uint32_t commandVersion = 0;
    std::memcpy(&commandVersion, hardware.vram.data() + (19ull << 20) + 4, 4);
    uint32_t commandSize = 0xFFFFFFFF;
    std::memcpy(&commandSize, hardware.vram.data() + (19ull << 20), 4);
    expect(commandSize == 0 && commandVersion == 0, "command header size and version stay zero");
    expect(hardware.readVRAM((19ull << 20) + 864) == 0, "PSP response status is zero");
    uint32_t firmwareType = 0;
    std::memcpy(&firmwareType, hardware.vram.data() + (19ull << 20) + 28 + 12, 4);
    expect(firmwareType == 18, "frame firmware type is SMU");
    expect(hardware.writePointer == 16, "ring write pointer advances one frame");
    expect(hardware.fence == 1, "PSP completion fence is observed");
}

void checkUnsafeStates(const std::vector<uint8_t>& firmware) {
    PSPLoader loader;
    expect(loader.parse(firmware.data(), firmware.size()), "container ready");
    expect(loader.setStagingOffset(16ull << 20), "staging ready");

    ScriptedHardware notReady;
    notReady.boot = 0;
    expect(!loader.load(notReady, 16ull << 30, 8ull << 20), "bootloader not ready refuses load");
    expect(notReady.writes.empty(), "not-ready load writes nothing");

    ScriptedHardware outside;
    outside.fbTop = outside.fbBase;
    expect(!loader.load(outside, 16ull << 30, 8ull << 20), "staging outside VRAM refuses load");
    expect(outside.writes.empty(), "outside-VRAM load writes nothing");

    ScriptedHardware dropped;
    dropped.readyFor = 2;
    expect(!loader.load(dropped, 16ull << 30, 8ull << 20), "bootloader dropping ready stops the chain");
    expect(dropped.writes.size() == 2, "only the first component is submitted");

    ScriptedHardware delayed;
    delayed.busyAfterCommand = 2;
    expect(loader.load(delayed, 16ull << 30, 8ull << 20), "bootloader recovering after a command still loads");
    expect(delayed.waits == 16, "each pre-SOS component waits through its busy window");

    ScriptedHardware smallMap;
    expect(loader.setStagingOffset(512ull << 20), "high staging is aligned");
    expect(!loader.load(smallMap, 256ull << 20, 8ull << 20), "staging past the mapped BAR0 is refused");
    expect(smallMap.writes.empty(), "unmapped staging writes nothing");

    const uint64_t staging = 16ull << 20;
    expect(loader.setStagingOffset(staging), "staging after framebuffer");
    expect(loader.setStagingWindow(8u << 20, 0, 8u << 20), "staging after framebuffer accepted");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: check_psp_loader <psp_sos.bin>\n");
        return 2;
    }
    const std::vector<uint8_t> firmware = readFile(argv[1]);
    expect(firmware.size() == 435632, "fixture size");
    checkRealContainer(firmware);
    checkRejectedContainers();
    checkLoadOrder(firmware);
    checkUnsafeStates(firmware);
    if (gFailures) {
        std::fprintf(stderr, "%d checks failed\n", gFailures);
        return 1;
    }
    std::puts("psp loader checks passed");
    return 0;
}
