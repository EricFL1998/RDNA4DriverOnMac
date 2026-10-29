#include "SMUMailbox.hpp"

namespace {
constexpr uint32_t Response = 0x009a;
constexpr uint32_t Parameter = 0x0092;
constexpr uint32_t Message = 0x0082;
constexpr uint32_t TestMessage = 1;
constexpr uint32_t GetVersion = 2;
constexpr uint32_t GetInterfaceVersion = 3;
constexpr uint32_t SetAllowedFeaturesMaskLow = 4;
constexpr uint32_t SetAllowedFeaturesMaskHigh = 5;
constexpr uint32_t EnableAllSmuFeatures = 6;
// Linux smu_v14_0_2_ppsmc.h: PPSMC_MSG_SetDriverDramAddrHigh/Low = 0x0E/0x0F.
// 0x0A/0x0B are not defined for this PMFW.
constexpr uint32_t SetDriverDramAddrHigh = 0x0E;
constexpr uint32_t SetDriverDramAddrLow = 0x0F;
constexpr uint32_t UseDefaultPPTable = 0x14;
constexpr uint32_t NotifyPowerSource = 0x35;
constexpr uint32_t RunDcBtc = 0x36;

struct Header {
    uint32_t sizeBytes;
    uint32_t headerSize;
    uint16_t major;
    uint16_t minor;
    uint16_t ipMajor;
    uint16_t ipMinor;
    uint32_t ucodeVersion;
    uint32_t ucodeSize;
    uint32_t ucodeOffset;
    uint32_t crc32;
};
}

bool SMUFirmware::parse(const uint8_t* bytes, size_t size, SMUFirmware& out) {
    out = {};
    if (!bytes || size < sizeof(Header)) return false;
    Header header;
    __builtin_memcpy(&header, bytes, sizeof(header));
    if (header.major != 2 || header.ipMajor != 14 || header.ucodeOffset > size ||
        header.ucodeSize > size - header.ucodeOffset || header.sizeBytes > size)
        return false;
    out.payload = bytes + header.ucodeOffset;
    out.payloadSize = header.ucodeSize;
    out.version = header.ucodeVersion;
    return out.payloadSize != 0;
}

bool SMUMailbox::send(SMUHardware& hardware, uint32_t message, uint32_t parameter, uint32_t& result) {
    if (!hardware.write(Response, 0) || !hardware.write(Parameter, parameter) ||
        !hardware.write(Message, message))
        return false;
    for (uint32_t attempt = 0; attempt < 1000; attempt++) {
        const uint32_t response = hardware.read(Response);
        if (response == 0xFFFFFFFF) return false;
        if (response != 0) {
            result = hardware.read(Parameter);
            return response == 1;
        }
        hardware.wait(1);
    }
    return false;
}

bool SMUMailbox::test(SMUHardware& hardware, uint32_t parameter, uint32_t& echo) {
    return send(hardware, TestMessage, parameter, echo);
}

bool SMUMailbox::version(SMUHardware& hardware, uint32_t& version) {
    return send(hardware, GetVersion, 0, version);
}

bool SMUMailbox::interfaceVersion(SMUHardware& hardware, uint32_t& version) {
    return send(hardware, GetInterfaceVersion, 0, version);
}

bool SMUMailbox::enableAllFeatures(SMUHardware& hardware) {
    uint32_t result = 0;
    return send(hardware, EnableAllSmuFeatures, 0, result);
}

bool SMUMailbox::setDriverTableAddress(SMUHardware& hardware, uint64_t address) {
    uint32_t addrHigh = static_cast<uint32_t>(address >> 32);
    uint32_t addrLow = static_cast<uint32_t>(address & 0xFFFFFFFF);
    uint32_t result = 0;
    if (!send(hardware, SetDriverDramAddrHigh, addrHigh, result)) {
        return false;
    }
    return send(hardware, SetDriverDramAddrLow, addrLow, result);
}

bool SMUMailbox::setFeatureMask(SMUHardware& hardware, uint32_t maskLow, uint32_t maskHigh) {
    uint32_t result = 0;
    if (!send(hardware, SetAllowedFeaturesMaskLow, maskLow, result)) {
        return false;
    }
    return send(hardware, SetAllowedFeaturesMaskHigh, maskHigh, result);
}

bool SMUMailbox::useDefaultPowerTable(SMUHardware& hardware) {
    uint32_t result = 0;
    return send(hardware, UseDefaultPPTable, 0, result);
}

bool SMUMailbox::runDcBtc(SMUHardware& hardware) {
    uint32_t result = 0;
    return send(hardware, RunDcBtc, 0, result);
}

bool SMUMailbox::notifyPowerSource(SMUHardware& hardware, uint32_t source) {
    uint32_t result = 0;
    return send(hardware, NotifyPowerSource, source, result);
}
