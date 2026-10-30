#ifndef RDNA4DriverUserClient_hpp
#define RDNA4DriverUserClient_hpp

#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>

class RDNA4Driver;

// User-space interface to the GPU.
//
// It lives in the RDNA4Driver kext rather than in RDNA4Accel on purpose: the
// two kexts are separate binaries, and a client kext cannot call into another
// kext's C++ methods without an exported symbol table. RDNA4Accel only
// publishes the Metal properties.
//
// Selectors:
//   0 GetDeviceInfo   0 in, 4 out   { vendorID, deviceID, userHeapGPU, userHeapSize }
//   1 AllocateBuffer  1 in, 1 out   { size } -> GPU address inside the user window
//   2 FreeBuffer      1 in, 0 out   { GPU address }
//   3 SubmitCommands  struct in, 1 out  PM4 dwords -> fence value
//   4 WaitFence       2 in, 0 out   { fence value, timeout ms }
//
// Memory type 0 (IOConnectMapMemory64) maps the user window; that is the only
// VRAM a client may touch.
class RDNA4DriverUserClient : public IOUserClient {
    OSDeclareDefaultStructors(RDNA4DriverUserClient)

public:
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;

    IOReturn clientMemoryForType(UInt32 type, IOOptionBits* options,
                                 IOMemoryDescriptor** memory) override;

    static const IOExternalMethodDispatch sMethods[8];

protected:
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments* arguments,
                            IOExternalMethodDispatch* dispatch, OSObject* target,
                            void* reference) override;

private:
    RDNA4Driver* driver { nullptr };

    static IOReturn sGetDeviceInfo(OSObject* target, void* reference,
                                   IOExternalMethodArguments* arguments);
    static IOReturn sAllocateBuffer(OSObject* target, void* reference,
                                    IOExternalMethodArguments* arguments);
    static IOReturn sFreeBuffer(OSObject* target, void* reference,
                                IOExternalMethodArguments* arguments);
    static IOReturn sSubmitCommands(OSObject* target, void* reference,
                                    IOExternalMethodArguments* arguments);
    static IOReturn sWaitFence(OSObject* target, void* reference,
                               IOExternalMethodArguments* arguments);
    static IOReturn sSubmitGraphics(OSObject* target, void* reference,
                                    IOExternalMethodArguments* arguments);
    static IOReturn sWaitGraphicsFence(OSObject* target, void* reference,
                                       IOExternalMethodArguments* arguments);
    static IOReturn sGetGraphicsInfo(OSObject* target, void* reference,
                                     IOExternalMethodArguments* arguments);
};

#endif /* RDNA4DriverUserClient_hpp */
