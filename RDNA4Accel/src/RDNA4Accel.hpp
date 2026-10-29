#ifndef RDNA4Accel_hpp
#define RDNA4Accel_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/graphics/IOAccelerator.h>

/**
 * RDNA4Accel - Phase 2 IOAccelerator stub
 *
 * This service attaches to the RDNA4Driver service and publishes the
 * accelerator properties used by the Metal bundle (MetalPluginName /
 * MetalPluginClassName / IODeviceID).  It is intentionally a thin
 * IOService stub: it performs no buffer allocation, command submission
 * or fence management yet.
 */
class RDNA4Accel : public IOAccelerator {
    OSDeclareDefaultStructors(RDNA4Accel)

public:
    virtual bool start(IOService* provider) override;
    virtual void stop(IOService* provider) override;
    virtual IOService* probe(IOService* provider, SInt32* score) override;

    virtual IOReturn newUserClient(task_t owningTask,
                                   void* securityID,
                                   UInt32 type,
                                   IOUserClient** handler) override;

private:
    IOService* mainDriver { nullptr };
    bool publishDeviceProperties();
};

/**
 * RDNA4AccelUserClient - user-space method channel.
 *
 * Selectors:
 *   0 GetDeviceInfo    (2 scalar outputs)
 *   1 AllocateBuffer   (1 scalar input, 1 scalar output; stub)
 *   2 FreeBuffer       (1 scalar input; stub)
 *   3 SubmitCommands   (variable structure input; stub)
 *   4 WaitFence        (2 scalar inputs; stub)
 */
class RDNA4AccelUserClient : public IOUserClient {
    OSDeclareDefaultStructors(RDNA4AccelUserClient)

public:
    virtual bool start(IOService* provider) override;
    virtual void stop(IOService* provider) override;

    virtual IOReturn clientClose() override;
    virtual IOReturn clientDied() override;

    static const IOExternalMethodDispatch sMethods[16];

protected:
    virtual IOReturn externalMethod(uint32_t selector,
                                    IOExternalMethodArguments* arguments,
                                    IOExternalMethodDispatch* dispatch,
                                    OSObject* target,
                                    void* reference) override;

private:
    RDNA4Accel* owner { nullptr };

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
};

#endif /* RDNA4Accel_hpp */
