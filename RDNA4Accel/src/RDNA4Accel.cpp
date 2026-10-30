#include "RDNA4Accel.hpp"
#include <IOKit/IOLib.h>

#define super IOAccelerator
OSDefineMetaClassAndStructors(RDNA4Accel, IOAccelerator)

//==============================================================================
// IOService Lifecycle
//==============================================================================

IOService* RDNA4Accel::probe(IOService* provider, SInt32* score) {
    IOLog("RDNA4Accel: probe() called\n");

    if (!provider) {
        return nullptr;
    }
    if (!super::probe(provider, score)) {
        return nullptr;
    }

    IOLog("RDNA4Accel: probe() succeeded\n");
    return this;
}

bool RDNA4Accel::start(IOService* provider) {
    IOLog("RDNA4Accel: start() ENTRY\n");

    if (!super::start(provider)) {
        IOLog("RDNA4Accel: super::start() failed\n");
        return false;
    }

    // Retain the provider: publishDeviceProperties() dereferences it and the
    // pointer must stay valid for as long as our properties can be re-read.
    if (provider) provider->retain();
    mainDriver = provider;

    if (!publishDeviceProperties()) {
        IOLog("RDNA4Accel: failed to publish device properties\n");
        OSSafeReleaseNULL(mainDriver);
        super::stop(provider);
        return false;
    }

    registerService();

    IOLog("RDNA4Accel: start() SUCCESS - accelerator stub registered\n");
    return true;
}

void RDNA4Accel::stop(IOService* provider) {
    IOLog("RDNA4Accel: stop() called\n");

    OSSafeReleaseNULL(mainDriver);
    super::stop(provider);
}

//==============================================================================
// Device Properties
//==============================================================================

bool RDNA4Accel::publishDeviceProperties() {
    // The hardware identifies as Navi 48 (RX 9070 / 9070 XT / 9070 GRE).
    setProperty("IOAcceleratorType", "GPU");
    setProperty("vendor-id", 0x1002, 32);
    setProperty("device-id", 0x7550, 32);
    setProperty("revision-id", 0xC0, 32);

    // IOPropertyMatch keys consumed by the RDNA4Metal.bundle Info.plist.
    setProperty("IOVendorID", 0x1002, 32);
    setProperty("IODeviceID", 0x7550, 32);

    setProperty("model", "AMD Radeon RX 9070 XT");
    setProperty("name", "RDNA4");

    // Prefer the live VRAM size published by RDNA4Driver; fall back to the
    // known 16320 MiB aperture if the property is not available.
    uint64_t vramSize = 16320ULL * 1024 * 1024;
    if (mainDriver) {
        OSNumber* vram = OSDynamicCast(OSNumber, mainDriver->copyProperty("GMC,VRAMSize"));
        if (vram) {
            vramSize = vram->unsigned64BitValue();
            vram->release();
        }
    }
    setProperty("vram-size", vramSize, 64);

    OSArray* capabilities = OSArray::withCapacity(3);
    if (capabilities) {
        OSString* capMetal = OSString::withCString("Metal");
        OSString* capCompute = OSString::withCString("Compute");
        OSString* capGraphics = OSString::withCString("Graphics");
        if (capMetal) capabilities->setObject(capMetal);
        if (capCompute) capabilities->setObject(capCompute);
        if (capGraphics) capabilities->setObject(capGraphics);
        setProperty("capabilities", capabilities);
        OSSafeReleaseNULL(capMetal);
        OSSafeReleaseNULL(capCompute);
        OSSafeReleaseNULL(capGraphics);
        OSSafeReleaseNULL(capabilities);
    }

    // Must match navi48-native/metal-bundle/Info.plist.
    setProperty("MetalPluginClassName", "RDNA4Device");
    setProperty("MetalPluginName", "RDNA4");
    setProperty("MetalPluginType", "Device");
    setProperty("MetalStatisticsName", "RDNA4");

    setProperty("GPUFamily", 2, 32);
    setProperty("MaxThreadsPerThreadgroup", 1024, 32);

    IOLog("RDNA4Accel: published device properties to IORegistry (VRAM %llu MB)\n",
          static_cast<unsigned long long>(vramSize >> 20));
    return true;
}

//==============================================================================
// IOUserClient Creation
//==============================================================================

IOReturn RDNA4Accel::newUserClient(task_t owningTask,
                                   void* securityID,
                                   UInt32 type,
                                   IOUserClient** handler) {
    IOLog("RDNA4Accel: newUserClient() called (type=%u)\n", type);

    if (!handler) {
        return kIOReturnBadArgument;
    }
    *handler = nullptr;

    // The hardware interface (buffer allocation, PM4 submission, fences, window
    // mapping) lives on the RDNA4Driver service: RDNA4Accel is a separate kext
    // and cannot call into RDNA4Driver's C++ methods. Clients must open
    // "RDNA4Driver" for that; this service only publishes the Metal properties.
    IOLog("RDNA4Accel: no user client here — open the RDNA4Driver service\n");
    (void)owningTask;
    (void)securityID;
    return kIOReturnUnsupported;
}
