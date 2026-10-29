#include "PSPLoader.hpp"

namespace {

constexpr uint32_t HeaderSize = 32;
constexpr uint32_t BootloaderReady = 0x80000000u;
constexpr uint32_t TypeSOS = 1;
constexpr uint32_t TypeSys = 2;
constexpr uint32_t TypeKdb = 3;
constexpr uint32_t TypeSpl = 5;
constexpr uint32_t TypeSoc = 7;
constexpr uint32_t TypeIntf = 8;
constexpr uint32_t TypeDbg = 9;
constexpr uint32_t TypeRas = 10;
constexpr uint32_t TypeIpKey = 11;

constexpr uint32_t BootDword = 0x0063;
constexpr uint32_t AddressDword = 0x0064;
constexpr uint32_t SignOfLifeDword = 0x0091;
constexpr uint32_t FirmwareVersionDword = 0x007a;
constexpr uint32_t FeatureVersionDword = 0x007b;
constexpr uint32_t FramebufferBaseDword = 0x0554;
constexpr uint32_t FramebufferTopDword = 0x0555;
constexpr uint32_t FramebufferMask = 0x00ffffffu;
constexpr uint32_t FramebufferShift = 24;

// Timeouts follow Linux/Cobalt64: bootloader sub-images 10s, SOS alive 5s,
// ring creation 5s, ring response/fence 20s. The previous 1s values caused
// spurious failures on cold boot.
constexpr uint32_t BootloaderTimeoutMs = 10000;
constexpr uint32_t SosAliveTimeoutMs = 5000;
constexpr uint32_t RingReadyTimeoutMs = 5000;
constexpr uint32_t RingResponseTimeoutMs = 20000;
constexpr uint32_t FenceTimeoutMs = 20000;

struct Header {
    uint32_t sizeBytes;
    uint32_t headerSizeBytes;
    uint16_t headerMajor;
    uint16_t headerMinor;
    uint16_t ipMajor;
    uint16_t ipMinor;
    uint32_t ucodeVersion;
    uint32_t ucodeSizeBytes;
    uint32_t ucodeOffsetBytes;
    uint32_t crc32;
};

struct Descriptor {
    uint32_t type;
    uint32_t version;
    uint32_t offsetBytes;
    uint32_t sizeBytes;
};

}

bool PSPLoader::read32(size_t offset, uint32_t& value) const {
    if (!firmware_ || offset > firmwareSize_ || firmwareSize_ - offset < 4)
        return false;
    __builtin_memcpy(&value, firmware_ + offset, 4);
    return true;
}

PSPLoader::StoredImage* PSPLoader::find(const char* name) {
    for (StoredImage& image : images_)
        if (image.name && __builtin_strcmp(image.name, name) == 0)
            return &image;
    return nullptr;
}

const PSPLoader::StoredImage* PSPLoader::find(const char* name) const {
    for (const StoredImage& image : images_)
        if (image.name && __builtin_strcmp(image.name, name) == 0)
            return &image;
    return nullptr;
}

bool PSPLoader::parse(const uint8_t* firmware, size_t firmwareSize) {
    firmware_ = nullptr;
    firmwareSize_ = 0;
    for (StoredImage& image : images_)
        image = StoredImage{};
    if (!firmware || firmwareSize < sizeof(Header))
        return false;

    Header header;
    __builtin_memcpy(&header, firmware, sizeof(header));
    if (header.headerMajor != 2 || header.headerSizeBytes < HeaderSize ||
        header.sizeBytes > firmwareSize || header.ucodeOffsetBytes > firmwareSize)
        return false;

    const size_t countOffset = HeaderSize;
    const size_t descriptorOffset = countOffset + 4 + (header.headerMinor >= 1 ? 4 : 0);
    uint32_t count = 0;
    firmware_ = firmware;
    firmwareSize_ = firmwareSize;
    if (!read32(countOffset, count) || count == 0 || count > 32 ||
        descriptorOffset > firmwareSize ||
        firmwareSize - descriptorOffset < static_cast<size_t>(count) * sizeof(Descriptor)) {
        firmware_ = nullptr;
        firmwareSize_ = 0;
        return false;
    }

    images_[0] = {"KDB", TypeKdb, 0x00080000u, {}};
    images_[1] = {"SPL", TypeSpl, 0x10000000u, {}};
    images_[2] = {"SYS_DRV", TypeSys, 0x00010000u, {}};
    images_[3] = {"SOC_DRV", TypeSoc, 0x000B0000u, {}};
    images_[4] = {"INTF_DRV", TypeIntf, 0x000D0000u, {}};
    images_[5] = {"HAD_DRV", TypeDbg, 0x000C0000u, {}};
    images_[6] = {"RAS_DRV", TypeRas, 0x000E0000u, {}};
    images_[7] = {"IPKEYMGR", TypeIpKey, 0x000F0000u, {}};
    images_[8] = {"SOS", TypeSOS, 0x00020000u, {}};

    for (uint32_t index = 0; index < count; index++) {
        Descriptor descriptor;
        __builtin_memcpy(&descriptor, firmware + descriptorOffset + index * sizeof(descriptor),
                         sizeof(descriptor));
        const uint64_t end = static_cast<uint64_t>(header.ucodeOffsetBytes) +
                             descriptor.offsetBytes + descriptor.sizeBytes;
        if (descriptor.sizeBytes == 0 || end > firmwareSize) {
            firmware_ = nullptr;
            firmwareSize_ = 0;
            return false;
        }
        for (StoredImage& image : images_) {
            if (image.type == descriptor.type) {
                image.image.bytes = firmware + header.ucodeOffsetBytes + descriptor.offsetBytes;
                image.image.size = descriptor.sizeBytes;
                image.image.version = descriptor.version;
            }
        }
    }

    if (!image("SOS")) {
        firmware_ = nullptr;
        firmwareSize_ = 0;
        return false;
    }
    return true;
}

const PSPImage* PSPLoader::image(const char* name) const {
    const StoredImage* stored = find(name);
    if (!stored || !stored->image.bytes || stored->image.size == 0)
        return nullptr;
    return &stored->image;
}

bool PSPLoader::setStagingOffset(uint64_t vramOffset) {
    return setStagingWindow(vramOffset, 0, 0);
}

bool PSPLoader::setStagingWindow(uint64_t offset, uint64_t reservedOffset, uint64_t reservedLength) {
    if (offset & (StagingSize - 1))
        return false;
    if (offset > UINT64_MAX - StagingSize)
        return false;
    const bool reserved = reservedLength != 0 && reservedOffset <= UINT64_MAX - reservedLength;
    const bool overlaps = reserved &&
                          offset < reservedOffset + reservedLength &&
                          offset + StagingSize > reservedOffset;
    if (overlaps)
        return false;
    stagingOffset_ = offset;
    stagingSet_ = true;
    return true;
}

bool PSPLoader::load(PSPHardware& hardware) {
    return load(hardware, UINT64_MAX, 8ull << 20);
}

bool PSPLoader::load(PSPHardware& hardware, uint64_t mappedBytes, uint64_t registerBytes) {
    if (!image("SOS") || !stagingSet_)
        return false;

    const uint32_t boot = hardware.read(PSPHardware::Block::PSP, BootDword);
    const uint32_t life = hardware.read(PSPHardware::Block::PSP, SignOfLifeDword);
    const uint32_t base = hardware.read(PSPHardware::Block::MemoryHub, FramebufferBaseDword);
    const uint32_t top = hardware.read(PSPHardware::Block::MemoryHub, FramebufferTopDword);
    if (boot == 0xFFFFFFFF || life == 0xFFFFFFFF || base == 0xFFFFFFFF || top == 0xFFFFFFFF)
        return false;
    if (life != 0)
        return true;
    if ((boot & BootloaderReady) == 0)
        return false;

    const uint64_t memoryBase = static_cast<uint64_t>(base & FramebufferMask) << FramebufferShift;
    const uint64_t memoryTop = top == base ? memoryBase :
        (static_cast<uint64_t>(top & FramebufferMask) << FramebufferShift) + (1ULL << FramebufferShift);
    const uint64_t staging = memoryBase + stagingOffset_;
    if (memoryBase == 0 || staging < memoryBase || staging + StagingSize > memoryTop)
        return false;
    if (stagingOffset_ > mappedBytes || StagingSize > mappedBytes - stagingOffset_)
        return false;

    for (const StoredImage& stored : images_) {
        if (!stored.name || stored.type == 0 || !stored.image.bytes)
            continue;
        if (stored.image.size > StagingSize)
            return false;
        if (!hardware.fill(stagingOffset_, 0, StagingSize) ||
            !hardware.copy(stagingOffset_, stored.image.bytes, stored.image.size) ||
            !hardware.flush(registerBytes))
            return false;
        uint32_t ready = 0;
        bool becameReady = false;
        for (uint32_t attempt = 0; attempt < BootloaderTimeoutMs; attempt++) {
            ready = hardware.read(PSPHardware::Block::PSP, BootDword);
            if (ready != 0xFFFFFFFF && (ready & BootloaderReady) != 0) {
                becameReady = true;
                break;
            }
            hardware.wait(1);
        }
        if (!becameReady)
            return false;
        if (!hardware.write(PSPHardware::Block::PSP, AddressDword,
                            static_cast<uint32_t>(staging >> 20)) ||
            !hardware.write(PSPHardware::Block::PSP, BootDword, stored.command))
            return false;
        
        // 硬件握手延迟（upstream Linux amdgpu / Cobalt64 标准要求）
        hardware.wait(20);

        bool accepted = false;
        const uint32_t acceptTimeout = stored.command == 0x00020000u
            ? SosAliveTimeoutMs : BootloaderTimeoutMs;
        for (uint32_t attempt = 0; attempt < acceptTimeout; attempt++) {
            const uint32_t status = hardware.read(PSPHardware::Block::PSP, BootDword);
            const uint32_t alive = hardware.read(PSPHardware::Block::PSP, SignOfLifeDword);
            if (stored.command == 0x00020000u) {
                if (alive != 0 && alive != 0xFFFFFFFF) {
                    accepted = true;
                    break;
                }
            } else if (status != 0xFFFFFFFF && (status & BootloaderReady) != 0) {
                accepted = true;
                break;
            }
            hardware.wait(1);
        }
        if (!accepted)
            return false;
    }
    return true;
}

bool PSPLoader::command(PSPHardware& hardware, uint64_t ringOffset, uint64_t registerBytes) {
    const uint32_t life = hardware.read(PSPHardware::Block::PSP, SignOfLifeDword);
    if (life == 0 || life == 0xFFFFFFFF || (ringOffset & 0xfff) != 0)
        return false;
    const uint32_t baseRaw = hardware.read(PSPHardware::Block::MemoryHub, FramebufferBaseDword);
    if (baseRaw == 0xFFFFFFFF)
        return false;
    const uint64_t base = static_cast<uint64_t>(baseRaw & FramebufferMask) << FramebufferShift;
    const uint64_t address = base + ringOffset;
    if (!hardware.fill(ringOffset, 0, 4096) || !hardware.flush(registerBytes))
        return false;
    bool ready = false;
    for (uint32_t attempt = 0; attempt < RingReadyTimeoutMs; attempt++) {
        const uint32_t response = hardware.read(PSPHardware::Block::PSP, 0x0080);
        if (response != 0xFFFFFFFF && (response & 0x80000000u) != 0) {
            ready = true;
            break;
        }
        hardware.wait(1);
    }
    if (!ready)
        return false;
    if (!hardware.write(PSPHardware::Block::PSP, 0x0085, static_cast<uint32_t>(address)) ||
        !hardware.write(PSPHardware::Block::PSP, 0x0086, static_cast<uint32_t>(address >> 32)) ||
        !hardware.write(PSPHardware::Block::PSP, 0x0087, 4096) ||
        !hardware.write(PSPHardware::Block::PSP, 0x0080, 0x00020000))
        return false;
    
    // 握手延迟
    hardware.wait(20);

    for (uint32_t attempt = 0; attempt < RingResponseTimeoutMs; attempt++) {
        const uint32_t response = hardware.read(PSPHardware::Block::PSP, 0x0080);
        if (response != 0xFFFFFFFF && (response & 0x80000000u) != 0)
            return (response & 0xffffu) == 0 && hardware.write(PSPHardware::Block::PSP, 0x0083, 0);
        hardware.wait(1);
    }
    return false;
}

bool PSPLoader::loadFirmware(PSPHardware& hardware, uint64_t firmwareOffset, const uint8_t* payload,
                            uint32_t payloadSize, uint32_t firmwareType, uint64_t ringOffset,
                            uint64_t commandOffset, uint64_t fenceOffset, uint64_t registerBytes) {
    PSPCommandResult result;
    return loadFirmwareWithResult(hardware, firmwareOffset, payload, payloadSize, firmwareType,
                                  ringOffset, commandOffset, fenceOffset, registerBytes, result);
}

bool PSPLoader::loadFirmwareWithResult(PSPHardware& hardware, uint64_t firmwareOffset, const uint8_t* payload,
                                      uint32_t payloadSize, uint32_t firmwareType, uint64_t ringOffset,
                                      uint64_t commandOffset, uint64_t fenceOffset, uint64_t registerBytes,
                                      PSPCommandResult& result) {
    if (!payload || payloadSize == 0 || (firmwareOffset & 0xfff)) return false;
    const uint32_t baseRaw = hardware.read(PSPHardware::Block::MemoryHub, FramebufferBaseDword);
    if (baseRaw == 0xFFFFFFFF)
        return false;
    const uint64_t memoryBase =
        static_cast<uint64_t>(baseRaw & FramebufferMask) << FramebufferShift;
    if (!hardware.fill(firmwareOffset, 0, payloadSize) ||
        !hardware.copy(firmwareOffset, payload, payloadSize))
        return false;
    uint8_t command[1024] = {};
    const uint32_t commandId = 6;
    const uint32_t addressLow = static_cast<uint32_t>(memoryBase + firmwareOffset);
    const uint32_t addressHigh = static_cast<uint32_t>((memoryBase + firmwareOffset) >> 32);
    __builtin_memcpy(command + 8, &commandId, 4);
    __builtin_memcpy(command + 28, &addressLow, 4);
    __builtin_memcpy(command + 32, &addressHigh, 4);
    __builtin_memcpy(command + 36, &payloadSize, 4);
    __builtin_memcpy(command + 40, &firmwareType, 4);
    if (!hardware.fill(commandOffset, 0, sizeof(command)) ||
        !hardware.copy(commandOffset, command, sizeof(command)))
        return false;
    return submitCommand(hardware, commandId, ringOffset, commandOffset, fenceOffset,
                         registerBytes, result);
}

bool PSPLoader::submitCommand(PSPHardware& hardware, uint32_t commandId, uint64_t ringOffset,
                             uint64_t commandOffset, uint64_t fenceOffset, uint64_t registerBytes,
                             PSPCommandResult& result) {
    result = {};
    if (commandId != 6) {
        uint8_t command[1024] = {};
        __builtin_memcpy(command + 8, &commandId, 4);
        if (!hardware.fill(commandOffset, 0, sizeof(command)) ||
            !hardware.copy(commandOffset, command, sizeof(command)))
            return false;
    }
    if (!hardware.fill(fenceOffset, 0, 4))
        return false;
    return submitPrepared(hardware, ringOffset, commandOffset, fenceOffset, registerBytes, result);
}

bool PSPLoader::submitPrepared(PSPHardware& hardware, uint64_t ringOffset, uint64_t commandOffset,
                               uint64_t fenceOffset, uint64_t registerBytes, PSPCommandResult& result) {
    if (!hardware.fill(fenceOffset, 0, 4))
        return false;
    const uint32_t baseRaw = hardware.read(PSPHardware::Block::MemoryHub, FramebufferBaseDword);
    if (baseRaw == 0xFFFFFFFF)
        return false;
    const uint64_t memoryBase =
        static_cast<uint64_t>(baseRaw & FramebufferMask) << FramebufferShift;
    uint8_t frame[64] = {};
    const uint32_t fenceValue = 1;
    const uint64_t commandAddress = memoryBase + commandOffset;
    const uint64_t fenceAddress = memoryBase + fenceOffset;
    const uint32_t commandLow = static_cast<uint32_t>(commandAddress);
    const uint32_t commandHigh = static_cast<uint32_t>(commandAddress >> 32);
    const uint32_t fenceLow = static_cast<uint32_t>(fenceAddress);
    const uint32_t fenceHigh = static_cast<uint32_t>(fenceAddress >> 32);
    result.addressHigh = commandHigh;
    __builtin_memcpy(frame + 0, &commandLow, 4);
    __builtin_memcpy(frame + 4, &commandHigh, 4);
    __builtin_memcpy(frame + 12, &fenceLow, 4);
    __builtin_memcpy(frame + 16, &fenceHigh, 4);
    __builtin_memcpy(frame + 20, &fenceValue, 4);
    const uint32_t pointer = hardware.pointer();
    result.pointer = pointer;
    if (pointer % 16) return false;
    const uint32_t slotIndex = pointer / 16;
    if (slotIndex >= (4096 / sizeof(frame))) return false;
    const uint64_t slot = ringOffset + static_cast<uint64_t>(slotIndex) * sizeof(frame);
    if (!hardware.copy(slot, frame, sizeof(frame)) || !hardware.flush(registerBytes) ||
        !hardware.write(PSPHardware::Block::PSP, 0x0083, (pointer + 16) % 1024))
        return false;
    for (uint32_t attempt = 0; attempt < FenceTimeoutMs; attempt++) {
        result.fence = hardware.readVRAM(fenceOffset);
        result.status = hardware.readVRAM(commandOffset + 864);
        if (result.fence == fenceValue) {
            result.completed = true;
            break;
        }
        hardware.wait(1);
    }
    result.finalPointer = hardware.pointer();
    result.mailbox = hardware.read(PSPHardware::Block::PSP, 0x0080);
    return result.completed && result.status == 0;
}

bool PSPLoader::readVersions(PSPHardware& hardware, PSPVersions& versions) const {
    versions.signOfLife = hardware.read(PSPHardware::Block::PSP, SignOfLifeDword);
    versions.firmware = hardware.read(PSPHardware::Block::PSP, FirmwareVersionDword);
    versions.feature = hardware.read(PSPHardware::Block::PSP, FeatureVersionDword);
    return versions.signOfLife != 0 && versions.signOfLife != 0xFFFFFFFF &&
           versions.firmware != 0xFFFFFFFF && versions.feature != 0xFFFFFFFF;
}
