#include "../src/hardware/SMUMailbox.cpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <fstream>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

struct ScriptedSMU : SMUHardware {
    uint32_t response = 1;
    uint32_t parameter = 0;
    int busyReads = 0;
    std::vector<uint32_t> messages;

    uint32_t read(uint32_t dword) override {
        if (dword == 0x009a) {
            if (busyReads > 0) { busyReads--; return 0; }
            return response;
        }
        if (dword == 0x0092) return parameter;
        return 0xFFFFFFFF;
    }
    bool write(uint32_t dword, uint32_t value) override {
        if (dword == 0x009a && value == 0 && response == 1) response = 0;
        if (dword == 0x0092) parameter = value;
        if (dword == 0x0082) {
            messages.push_back(value);
            if (response == 0 || response == 1) response = 1;
            if (value == 1) parameter = parameter + 1;
            if (value == 2) parameter = 0x00685300;
            if (value == 3) parameter = 0x003e0000;
        }
        return true;
    }
};
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> firmware((std::istreambuf_iterator<char>(input)), {});
    SMUFirmware parsed;
    expect(SMUFirmware::parse(firmware.data(), firmware.size(), parsed), "smu firmware parses");
    expect(parsed.version == 0x00685000 && parsed.payloadSize == 327168, "smu header");
    expect(parsed.payload && parsed.payload[0] == 0x61 && parsed.payload[1] == 0x2b, "smu payload excludes header");
    expect(parsed.payload != firmware.data(), "smu payload is not the file start");

    ScriptedSMU hardware;
    hardware.busyReads = 2;
    uint32_t echo = 0;
    expect(SMUMailbox::test(hardware, 0xABCD0001, echo), "test message answers");
    expect(echo == 0xABCD0002, "test message echoes parameter plus one");
    expect(hardware.messages.size() == 1 && hardware.messages[0] == 1, "test message id is 1");

    uint32_t version = 0;
    expect(SMUMailbox::version(hardware, version), "version message answers");
    expect(version == 0x00685300, "pmfw version");
    uint32_t interfaceVersion = 0;
    expect(SMUMailbox::interfaceVersion(hardware, interfaceVersion), "interface version answers");
    expect(interfaceVersion == 0x003e0000, "interface version value");
    expect(hardware.messages.back() == 3, "interface query uses message 3");

    ScriptedSMU failed;
    failed.response = 0xFF;
    expect(!SMUMailbox::test(failed, 0xABCD0001, echo), "failed response is not success");
    expect(failed.messages.size() == 1, "failed response does not retry the message");
    if (failures) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::puts("smu checks passed");
    return 0;
}
