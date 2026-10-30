#include "RDNA4FramebufferEdid.hpp"
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

class RDNA4Framebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(RDNA4Framebuffer)

public:
    IOService* probe(IOService* provider, SInt32* score) override;
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;

    IOReturn enableController() override;
    bool isConsoleDevice() override;
    IOItemCount getConnectionCount() override;
    IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                       uintptr_t* value) override;
    IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                 IOPixelAperture aperture,
                                 IOPixelInformation* pixelInfo) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID* displayMode, IOIndex* depth) override;
    IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
    IODeviceMemory* getVRAMRange() override;
    IODeviceMemory* getApertureRange(IOPixelAperture aperture) override;
    const char* getPixelFormats() override;
    IOItemCount getDisplayModeCount() override;
    IOReturn getDisplayModes(IODisplayModeID* allDisplayModes) override;
    IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
                                          IODisplayModeInformation* info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;

private:
    bool captureConsoleInfo();

    IOPhysicalAddress64 fbPhysBase { 0 };
    uint64_t fbLength { 0 };
    // Whole-VRAM size reported by getVRAMRange(); the console framebuffer is
    // only a small window inside it (2560x1440x4 bytes, which is what made
    // system_profiler show "14 MB").
    uint64_t vramBytes { 0 };
    uint32_t fbWidth { 0 };
    uint32_t fbHeight { 0 };
    uint32_t fbRowBytes { 0 };
    uint32_t fbDepth { 32 };
};

OSDefineMetaClassAndStructors(RDNA4Framebuffer, IOFramebuffer)

bool RDNA4Framebuffer::captureConsoleInfo() {
    IOPlatformExpert* platform = getPlatform();
    if (!platform) {
        IOLog("RDNA4Framebuffer: no platform expert\n");
        return false;
    }

    PE_Video video {};
    IOReturn ret = platform->getConsoleInfo(&video);
    if (ret != kIOReturnSuccess) {
        IOLog("RDNA4Framebuffer: getConsoleInfo failed 0x%x\n", ret);
        return false;
    }

    fbPhysBase = static_cast<IOPhysicalAddress64>(video.v_baseAddr) & ~0xFFFULL;
    fbWidth = static_cast<uint32_t>(video.v_width);
    fbHeight = static_cast<uint32_t>(video.v_height);
    fbRowBytes = static_cast<uint32_t>(video.v_rowBytes);
    fbDepth = static_cast<uint32_t>(video.v_depth);
    fbLength = video.v_length ? static_cast<uint64_t>(video.v_length)
                              : static_cast<uint64_t>(fbRowBytes) * fbHeight;

    if (!fbPhysBase || !fbWidth || !fbHeight || !fbRowBytes || !fbLength ||
        (fbDepth != 32 && fbDepth != 30 && fbDepth != 24)) {
        IOLog("RDNA4Framebuffer: invalid console framebuffer base=0x%llx %ux%u stride=%u depth=%u length=%llu\n",
              fbPhysBase, fbWidth, fbHeight, fbRowBytes, fbDepth, fbLength);
        return false;
    }
    fbDepth = 32;

    setProperty("Console,BaseAddress", fbPhysBase, 64);
    setProperty("Console,Width", static_cast<uint64_t>(fbWidth), 32);
    setProperty("Console,Height", static_cast<uint64_t>(fbHeight), 32);
    setProperty("Console,RowBytes", static_cast<uint64_t>(fbRowBytes), 32);
    setProperty("Console,Depth", static_cast<uint64_t>(fbDepth), 32);
    setProperty("Console,Length", fbLength, 64);

    IOLog("RDNA4Framebuffer: console framebuffer base=0x%llx %ux%u stride=%u depth=%u length=%llu\n",
          fbPhysBase, fbWidth, fbHeight, fbRowBytes, fbDepth, fbLength);
    return true;
}

IOService* RDNA4Framebuffer::probe(IOService* provider, SInt32* score) {
    uint32_t off = 0;
    if (PE_parse_boot_argn("rdna4fb-off", &off, sizeof(off)) && off != 0) {
        IOLog("RDNA4Framebuffer: disabled by rdna4fb-off boot-arg\n");
        return nullptr;
    }

    if (!IOFramebuffer::probe(provider, score))
        return nullptr;

    IOPCIDevice* pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return nullptr;

    uint32_t vendorDevice = pci->configRead32(kIOPCIConfigVendorID);
    uint16_t vendor = vendorDevice & 0xFFFF;
    uint16_t device = (vendorDevice >> 16) & 0xFFFF;
    if (vendor != 0x1002 || device != 0x7550)
        return nullptr;

    IOLog("RDNA4Framebuffer: matched RX 9070 XT\n");
    if (score)
        *score = 25000;
    return this;
}

bool RDNA4Framebuffer::start(IOService* provider) {
    IOPCIDevice* pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        IOLog("RDNA4Framebuffer: start without PCI provider\n");
        return false;
    }

    if (!captureConsoleInfo())
        return false;

    pci->setMemoryEnable(true);
    if (!IOFramebuffer::start(provider)) {
        IOLog("RDNA4Framebuffer: super::start failed\n");
        return false;
    }

    // The driver publishes hardware properties on this PCI provider, so reading
    // them does not depend on which service completes registration first.
    const char* deviceModel = nullptr;
    char fallbackModel[48];
    if (OSNumber* size = OSDynamicCast(OSNumber, pci->getProperty("GMC,VRAMSize"))) {
        vramBytes = size->unsigned64BitValue();
    }
    if (OSString* name = OSDynamicCast(OSString, pci->getProperty("Device,Name"))) {
        deviceModel = name->getCStringNoCopy();
    }
    if (!deviceModel) {
        const uint32_t vd = pci->configRead32(kIOPCIConfigVendorID);
        snprintf(fallbackModel, sizeof(fallbackModel), "Radeon (0x%04x:0x%04x)",
                 vd & 0xFFFFu, (vd >> 16) & 0xFFFFu);
        deviceModel = fallbackModel;
    }

    if (vramBytes) {
        setProperty("VRAM,TotalMB", vramBytes >> 20, 64);
        // About This Mac reads these off the GPU's IOPCIDevice, not off the
        // framebuffer: "VRAM,totalMB" (32-bit, MiB) and "VRAM,totalsize" (an
        // 8-byte OSData holding the size in bytes).
        pci->setProperty("VRAM,totalMB", vramBytes >> 20, 32);
        const uint64_t bytes = vramBytes;
        if (OSData* size = OSData::withBytes(&bytes, sizeof(bytes))) {
            pci->setProperty("VRAM,totalsize", size);
            size->release();
        }
        IOLog("RDNA4Framebuffer: VRAM = %llu bytes (%llu MiB)\n",
              vramBytes, vramBytes >> 20);
    }

    // system_profiler's "Chipset Model" is the `model` property of the
    // IOPCIDevice; without it the GPU shows up only as vendor/device IDs.
    pci->setProperty("model", deviceModel);
    setProperty("model", deviceModel);

    // The display's own name comes from its EDID, read over the display
    // engine's I2C bus.  A failure here is non-fatal: the console framebuffer
    // keeps working, macOS just shows a generic display name.
    if (IOMemoryMap* bar5 = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5)) {
        volatile uint32_t* mmio =
            reinterpret_cast<volatile uint32_t*>(bar5->getVirtualAddress());
        const uint64_t mmioBytes = bar5->getLength();
        uint8_t edid[256];
        uint32_t edidLen = 0, ddcLine = 0;
        if (mmio && mmioBytes && rdna4fbReadEdid(mmio, mmioBytes, edid, &edidLen, &ddcLine)) {
            if (OSData* d = OSData::withBytes(edid, edidLen)) {
                setProperty("IODisplayEDID", d);
                d->release();
            }
            if (OSData* d = OSData::withBytes(edid, 128)) {
                char key[24];
                snprintf(key, sizeof(key), "EDID,DDC%u", ddcLine);
                setProperty(key, d);
                d->release();
            }
        }
        bar5->release();
    } else {
        IOLog("RDNA4Framebuffer: BAR5 not mappable, skipping EDID\n");
    }

    IOLog("RDNA4Framebuffer: started\n");
    return true;
}

void RDNA4Framebuffer::stop(IOService* provider) {
    IOFramebuffer::stop(provider);
}

IOReturn RDNA4Framebuffer::enableController() {
    if (!fbPhysBase && !captureConsoleInfo())
        return kIOReturnNoResources;
    return kIOReturnSuccess;
}

bool RDNA4Framebuffer::isConsoleDevice() {
    return true;
}

IODeviceMemory* RDNA4Framebuffer::getApertureRange(IOPixelAperture aperture) {
    if (aperture != kIOFBSystemAperture || !fbPhysBase || !fbLength)
        return nullptr;
    return IODeviceMemory::withRange(fbPhysBase, fbLength);
}

IODeviceMemory* RDNA4Framebuffer::getVRAMRange() {
    // This is the scanout descriptor (IOFramebufferSharedUserClient maps it),
    // so it must stay the console framebuffer window, not the whole VRAM
    // aperture.  The card's VRAM size is published on the IOPCIDevice instead.
    if (!fbPhysBase || !fbLength)
        return nullptr;
    return IODeviceMemory::withRange(fbPhysBase, fbLength);
}

const char* RDNA4Framebuffer::getPixelFormats() {
    static const char formats[] = IO32BitDirectPixels "\0";
    return formats;
}

IOItemCount RDNA4Framebuffer::getDisplayModeCount() {
    return 1;
}

IOReturn RDNA4Framebuffer::getDisplayModes(IODisplayModeID* allDisplayModes) {
    if (!allDisplayModes)
        return kIOReturnBadArgument;
    allDisplayModes[0] = 1;
    return kIOReturnSuccess;
}

IOReturn RDNA4Framebuffer::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                       IODisplayModeInformation* info) {
    if (displayMode != 1 || !info)
        return kIOReturnBadArgument;

    bzero(info, sizeof(*info));
    info->nominalWidth = fbWidth;
    info->nominalHeight = fbHeight;
    info->refreshRate = 60 << 16;
    info->maxDepthIndex = 0;
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}

UInt64 RDNA4Framebuffer::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) {
    return 0;
}

IOReturn RDNA4Framebuffer::getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                              IOPixelAperture aperture,
                                              IOPixelInformation* pixelInfo) {
    if (displayMode != 1 || depth != 0 || aperture != kIOFBSystemAperture || !pixelInfo)
        return kIOReturnBadArgument;

    bzero(pixelInfo, sizeof(*pixelInfo));
    pixelInfo->bytesPerRow = fbRowBytes;
    pixelInfo->bitsPerPixel = 32;
    pixelInfo->pixelType = kIORGBDirectPixels;
    pixelInfo->componentCount = 3;
    pixelInfo->bitsPerComponent = 8;
    pixelInfo->componentMasks[0] = 0x00FF0000;
    pixelInfo->componentMasks[1] = 0x0000FF00;
    pixelInfo->componentMasks[2] = 0x000000FF;

    strlcpy(pixelInfo->pixelFormat, IO32BitDirectPixels, sizeof(pixelInfo->pixelFormat));
    pixelInfo->activeWidth = fbWidth;
    pixelInfo->activeHeight = fbHeight;
    return kIOReturnSuccess;
}

IOReturn RDNA4Framebuffer::getCurrentDisplayMode(IODisplayModeID* displayMode, IOIndex* depth) {
    if (displayMode) *displayMode = 1;
    if (depth) *depth = 0;
    return kIOReturnSuccess;
}

IOReturn RDNA4Framebuffer::setDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
    return displayMode == 1 && depth == 0 ? kIOReturnSuccess : kIOReturnUnsupported;
}

IOItemCount RDNA4Framebuffer::getConnectionCount() {
    return 1;
}

IOReturn RDNA4Framebuffer::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                                    uintptr_t* value) {
    switch (attribute) {
        case kConnectionEnable:
        case kConnectionCheckEnable:
            if (value) *value = 1;
            return kIOReturnSuccess;
        case kConnectionFlags:
            if (value) *value = 0;
            return kIOReturnSuccess;
        default:
            return IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);
    }
}
