// Host-only IOKit dispatch/lifecycle surface. Tests invoke stop explicitly;
// this shim does not pretend that terminate synchronously detaches a provider.
#pragma once
#include "IOTypes.h"

using UInt32 = uint32_t;
using IOOptionBits = uint32_t;
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnBadArgument = -1;
constexpr IOReturn kIOReturnNotAttached = -2;
constexpr IOReturn kIOReturnUnsupported = -3;
constexpr IOReturn kIOReturnNotReady = -4;
constexpr IOReturn kIOReturnNoMemory = -5;
constexpr IOReturn kIOReturnNoResources = -6;
constexpr IOReturn kIOReturnTimeout = -7;
constexpr uint32_t kIOUCVariableStructureSize = UINT32_MAX;

class OSObject { public: virtual ~OSObject() = default; };
class IOMemoryDescriptor;
class IOService : public OSObject {};
struct IOExternalMethodArguments {
    uint32_t scalarInputCount = 0;
    const uint64_t* scalarInput = nullptr;
    uint32_t scalarOutputCount = 0;
    uint64_t* scalarOutput = nullptr;
    const void* structureInput = nullptr;
    size_t structureInputSize = 0;
};
struct IOExternalMethodDispatch {
    IOReturn (*function)(OSObject*, void*, IOExternalMethodArguments*);
    uint32_t inputCount, inputSize, outputCount, outputSize;
};
class IOUserClient : public IOService {
    bool inactive = false;
public:
    virtual bool start(IOService*) { return true; }
    virtual void stop(IOService*) {}
    virtual void free() {}
    virtual IOReturn clientClose() { return kIOReturnUnsupported; }
    virtual IOReturn clientDied() { return clientClose(); }
    virtual IOReturn clientMemoryForType(UInt32, IOOptionBits*, IOMemoryDescriptor**) {
        return kIOReturnUnsupported;
    }
    virtual IOReturn externalMethod(uint32_t, IOExternalMethodArguments* arguments,
                                    IOExternalMethodDispatch* dispatch,
                                    OSObject* target, void* reference) {
        return dispatch->function(target, reference, arguments);
    }
    bool isInactive() const { return inactive; }
    void terminate() { inactive = true; }
};
#define OSDeclareDefaultStructors(T) public: T() = default; ~T() override = default;
#define OSDefineMetaClassAndStructors(T, B)
#define OSDynamicCast(T, object) dynamic_cast<T*>(object)
