#ifndef SMUMailbox_hpp
#define SMUMailbox_hpp

#include <stdint.h>
#include <stddef.h>

struct SMUFirmware {
    const uint8_t* payload;
    uint32_t payloadSize;
    uint32_t version;
    static bool parse(const uint8_t* bytes, size_t size, SMUFirmware& out);
};

class SMUHardware {
public:
    virtual uint32_t read(uint32_t dword) = 0;
    virtual bool write(uint32_t dword, uint32_t value) = 0;
    virtual void wait(uint32_t milliseconds) { (void)milliseconds; }
protected:
    ~SMUHardware() = default;
};

class SMUMailbox {
public:
    static bool test(SMUHardware& hardware, uint32_t parameter, uint32_t& echo);
    static bool version(SMUHardware& hardware, uint32_t& version);
    static bool interfaceVersion(SMUHardware& hardware, uint32_t& version);
    static bool enableAllFeatures(SMUHardware& hardware);
    static bool setDriverTableAddress(SMUHardware& hardware, uint64_t address);
    static bool setFeatureMask(SMUHardware& hardware, uint32_t maskLow, uint32_t maskHigh);
    static bool useDefaultPowerTable(SMUHardware& hardware);
    static bool runDcBtc(SMUHardware& hardware);
    static bool notifyPowerSource(SMUHardware& hardware, uint32_t source);

private:
    static bool send(SMUHardware& hardware, uint32_t message, uint32_t parameter, uint32_t& result);
};

#endif
