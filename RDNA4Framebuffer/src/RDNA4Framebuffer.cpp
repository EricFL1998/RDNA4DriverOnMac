#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
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
