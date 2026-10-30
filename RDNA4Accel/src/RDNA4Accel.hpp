#ifndef RDNA4Accel_hpp
#define RDNA4Accel_hpp

#include <IOKit/IOService.h>
#include <IOKit/graphics/IOAccelerator.h>

/**
 * RDNA4Accel - IOAccelerator property publisher.
 *
 * Attaches to the RDNA4Driver service and publishes the properties the Metal
 * bundle matches on (MetalPluginName / MetalPluginClassName / IODeviceID /
 * vram-size).
 *
 * It deliberately exposes no user client: buffer allocation, PM4 submission,
 * fences and the user window mapping all live on the RDNA4Driver service,
 * because a separate kext cannot call into RDNA4Driver's C++ methods.
 */
class RDNA4Accel : public IOAccelerator {
    OSDeclareDefaultStructors(RDNA4Accel)

public:
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    IOService* probe(IOService* provider, SInt32* score) override;

    IOReturn newUserClient(task_t owningTask,
                           void* securityID,
                           UInt32 type,
                           IOUserClient** handler) override;

private:
    IOService* mainDriver { nullptr };
    bool publishDeviceProperties();
};

#endif /* RDNA4Accel_hpp */
