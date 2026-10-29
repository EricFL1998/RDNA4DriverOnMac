#ifndef PSPLoader_hpp
#define PSPLoader_hpp

#include <stdint.h>
#include <stddef.h>

struct PSPImage {
    const uint8_t* bytes;
    uint32_t size;
    uint32_t version;
};

struct PSPVersions {
    uint32_t signOfLife;
    uint32_t firmware;
    uint32_t feature;
};

struct PSPCommandResult {
    uint32_t status;
    uint32_t fence;
    uint32_t pointer;
    uint32_t finalPointer;
    uint32_t mailbox;
    uint32_t addressHigh;
    bool completed;
};

class PSPHardware {
public:
    enum class Block : uint8_t { PSP, MemoryHub };

    virtual uint32_t read(Block block, uint32_t dword) = 0;
    virtual bool write(Block block, uint32_t dword, uint32_t value) = 0;
    virtual bool fill(uint64_t offset, uint8_t value, size_t length) = 0;
    virtual bool copy(uint64_t offset, const void* source, size_t length) = 0;
    virtual void wait(uint32_t milliseconds) { (void)milliseconds; }
    virtual bool flush(uint64_t registerBytes) { (void)registerBytes; return false; }
    virtual uint32_t readVRAM(uint64_t offset) { (void)offset; return 0xFFFFFFFF; }
    virtual uint32_t pointer() { return 0; }

protected:
    ~PSPHardware() = default;
};

class PSPLoader {
public:
    static constexpr uint32_t StagingSize = 1024u * 1024u;

    bool parse(const uint8_t* firmware, size_t firmwareSize);
    const PSPImage* image(const char* name) const;
    bool setStagingOffset(uint64_t vramOffset);
    bool setStagingWindow(uint64_t offset, uint64_t reservedOffset, uint64_t reservedLength);
    bool load(PSPHardware& hardware);
    bool load(PSPHardware& hardware, uint64_t mappedBytes, uint64_t registerBytes);
    bool readVersions(PSPHardware& hardware, PSPVersions& versions) const;
    bool command(PSPHardware& hardware, uint64_t ringOffset, uint64_t registerBytes);
    bool loadFirmware(PSPHardware& hardware, uint64_t firmwareOffset, const uint8_t* payload,
                      uint32_t payloadSize, uint32_t firmwareType, uint64_t ringOffset,
                      uint64_t commandOffset, uint64_t fenceOffset, uint64_t registerBytes);
    bool submitPrepared(PSPHardware& hardware, uint64_t ringOffset, uint64_t commandOffset,
                        uint64_t fenceOffset, uint64_t registerBytes, PSPCommandResult& result);
    bool submitCommand(PSPHardware& hardware, uint32_t commandId, uint64_t ringOffset,
                       uint64_t commandOffset, uint64_t fenceOffset, uint64_t registerBytes,
                       PSPCommandResult& result);
    bool loadFirmwareWithResult(PSPHardware& hardware, uint64_t firmwareOffset, const uint8_t* payload,
                               uint32_t payloadSize, uint32_t firmwareType, uint64_t ringOffset,
                               uint64_t commandOffset, uint64_t fenceOffset, uint64_t registerBytes,
                               PSPCommandResult& result);

private:
    struct StoredImage {
        const char* name;
        uint32_t type;
        uint32_t command;
        PSPImage image;
    };

    bool read32(size_t offset, uint32_t& value) const;
    StoredImage* find(const char* name);
    const StoredImage* find(const char* name) const;

    const uint8_t* firmware_ { nullptr };
    size_t firmwareSize_ { 0 };
    uint64_t stagingOffset_ { 0 };
    bool stagingSet_ { false };
    StoredImage images_[12] {};
};

#endif
