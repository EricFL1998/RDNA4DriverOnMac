#include "../src/hardware/GFXFirmware.cpp"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <fstream>

namespace {
int failures = 0;
void expect(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); failures++; }
}
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 9) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    GFXFirmware firmware;
    expect(GFXFirmware::parseIMU(bytes.data(), bytes.size(), firmware), "IMU parses");
    expect(firmware.count == 2, "IMU has instruction and data halves");
    expect(firmware.parts[0].type == 68 && firmware.parts[0].size == 66048, "IMU instruction half");
    expect(firmware.parts[1].type == 69 && firmware.parts[1].size == 66048, "IMU data half");
    expect(firmware.parts[0].bytes && firmware.parts[0].bytes[0] == 0x87, "IMU payload excludes header");
    expect(firmware.parts[1].bytes - firmware.parts[0].bytes == 66048, "IMU data follows the instruction half");
    if (argc == 3) {
        std::ifstream rlcInput(argv[2], std::ios::binary);
        std::vector<uint8_t> rlc((std::istreambuf_iterator<char>(rlcInput)), {});
        GFXFirmware rlcFirmware;
        expect(GFXFirmware::parseRLC(rlc.data(), rlc.size(), rlcFirmware), "RLC parses");
        expect(rlcFirmware.count == 5, "RLC v2.2 ends with RLC_G");
        const uint32_t types[] = {20, 21, 26, 48, 8};
        const uint32_t sizes[] = {2560, 23616, 66048, 33280, 25088};
        for (uint32_t index = 0; index < 5; index++) {
            expect(rlcFirmware.parts[index].type == types[index], "RLC part type order");
            expect(rlcFirmware.parts[index].size == sizes[index], "RLC part size");
            expect(rlcFirmware.parts[index].size <= 1024u * 1024u, "RLC part fits one staging window");
        }
        expect(rlcFirmware.parts[4].type == 8, "RLC_G is last");
    }
    if (argc == 7) {
        const char* names[] = {"pfp", "me", "mec", "mes"};
        const uint32_t expected[][4] = {{87, 90}, {88, 92}, {89, 94, 95}, {33, 34, 81, 82}};
        const uint32_t counts[] = {2, 2, 3, 4};
        for (int file = 0; file < 4; file++) {
            std::ifstream input(argv[3 + file], std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
            GFXFirmware firmware;
            bool parsed = false;
            if (file < 3) {
                const uint32_t stacks[] = {expected[file][1], file == 2 ? expected[file][2] : 0};
                parsed = GFXFirmware::parseCP(bytes.data(), bytes.size(), expected[file][0], stacks,
                                              counts[file] - 1, firmware);
            } else {
                parsed = GFXFirmware::parseMES(bytes.data(), bytes.size(), firmware);
            }
            expect(parsed && firmware.count == counts[file], names[file]);
            for (uint32_t index = 0; index < firmware.count; index++) {
                expect(firmware.parts[index].type == expected[file][index], "CP/MES type");
                expect(firmware.parts[index].size > 0 && firmware.parts[index].size <= 1024u * 1024u,
                       "CP/MES size fits staging");
            }
        }
    }
    if (argc == 8) {
        std::ifstream sdmaInput(argv[7], std::ios::binary);
        std::vector<uint8_t> sdma((std::istreambuf_iterator<char>(sdmaInput)), {});
        GFXFirmware sdmaFirmware;
        expect(GFXFirmware::parseSingle(sdma.data(), sdma.size(), 71, sdmaFirmware), "SDMA parses");
        expect(sdmaFirmware.count == 1 && sdmaFirmware.parts[0].type == 71, "SDMA type");
        expect(sdmaFirmware.parts[0].size == 46848, "SDMA payload excludes header");
    }
    if (argc == 9) {
        std::ifstream sosInput(argv[8], std::ios::binary);
        std::vector<uint8_t> sos((std::istreambuf_iterator<char>(sosInput)), {});
        GFXFirmware toc;
        expect(GFXFirmware::parseSOSImage(sos.data(), sos.size(), 4, toc), "TOC parses");
        expect(toc.count == 1 && toc.parts[0].size == 2304, "TOC is the 2304-byte sub-image");
    }
    if (failures) return 1;
    std::puts("gfx checks passed");
    return 0;
}
