//
//  RDNA4Driver.cpp
//  RDNA4Driver —— GFX12 显卡驱动主体
//
//  基于 Cobalt64 的 Navi48Bringup
//

#include "RDNA4Driver.hpp"
#include "RDNA4DriverUserClient.hpp"
#include <IOKit/IOLib.h>
#include <string.h>
#include "../hardware/IPDiscovery.hpp"
#include "../hardware/VramLayout.hpp"
#include "../hardware/NBIODoorbell.hpp"
#include "psp_14_0_3_sos_fw.h"
#include "../hardware/SMUMailbox.hpp"
#include "../hardware/GFXFirmware.hpp"
#include "gc_12_0_1_imu_fw.h"
#include "gc_12_0_1_pfp_fw.h"
#include "gc_12_0_1_me_fw.h"
#include "gc_12_0_1_mec_fw.h"
#include "gc_12_0_1_uni_mes_fw.h"
#include "gc_12_0_1_rlc_fw.h"
#include "sdma_7_0_1_fw.h"
#include "smu_14_0_3_fw.h"

OSDefineMetaClassAndStructors(RDNA4Driver, IOService)

//==============================================================================
// IOService 生命周期
//==============================================================================

IOService* RDNA4Driver::probe(IOService* provider, SInt32* score) {
    IOLog("RDNA4: probe() ENTRY - provider=%p score=%d\n", provider, score ? *score : -1);

    // Boot-arg检查（临时禁用，方便测试）
    uint32_t bootArg = 0;
    bool hasBootArg = PE_parse_boot_argn("rdna4", &bootArg, sizeof(bootArg));
    if (hasBootArg && bootArg == 0) {
        // 明确设置为 0 才禁用
        IOLog("RDNA4: explicitly disabled (rdna4=0)\n");
        return nullptr;
    }
    IOLog("RDNA4: boot-arg check passed (rdna4=%u, present=%d)\n", bootArg, hasBootArg);

    // 调用父类probe
    if (!IOService::probe(provider, score)) {
        IOLog("RDNA4: super::probe() failed\n");
        return nullptr;
    }

    IOLog("RDNA4: super::probe() OK\n");

    // 获取 PCI 设备
    IOPCIDevice* pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        IOLog("RDNA4: provider is not IOPCIDevice\n");
        return nullptr;
    }

    // 读取设备 ID
    uint32_t vendorDevice = pci->configRead32(kIOPCIConfigVendorID);
    uint16_t vendor = vendorDevice & 0xFFFF;
    uint16_t device = (vendorDevice >> 16) & 0xFFFF;

    IOLog("RDNA4: found device 0x%08x\n", vendorDevice);

    // 检查是否是 AMD RDNA4
    if (vendor != RDNA4_VENDOR_AMD ||
        (device != RDNA4_DEVICE_NAVI48_9070XT &&
         device != RDNA4_DEVICE_NAVI48_R9700 &&
         device != RDNA4_DEVICE_NAVI44_9060XT)) {
        IOLog("RDNA4: probe: 0x%08x not RDNA4, ignoring\n", vendorDevice);
        return nullptr;
    }

    IOLog("RDNA4: probe: RDNA4 device 0x%04x - SUCCESS, returning this\n", device);
    *score += 10000;

    return this;
}

bool RDNA4Driver::start(IOService* provider) {
    LOG("start() - RDNA4 Driver version %s", RDNA4_VERSION_STRING);

    if (!IOService::start(provider)) {
        LOG_ERROR("IOService::start failed");
        return false;
    }

    // 获取并保留 PCI 设备
    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        LOG_ERROR("Provider is not an IOPCIDevice");
        return false;
    }
    pciDevice->retain();

    // 初始化成员变量
    rmmioMap = nullptr;
    rmmio = nullptr;
    rmmioSize = 0;
    vramMap = nullptr;
    vram = nullptr;
    vramSize = 0;
    doorbellMap = nullptr;
    doorbell = nullptr;
    doorbellSize = 0;
    regReadCount = 0;
    regWriteCount = 0;
    startTime = mach_absolute_time();

    // 解析 debug level (probe已经检查了rdna4=1)
    debugLevel = 1;
    PE_parse_boot_argn("rdna4-debug", &debugLevel, sizeof(debugLevel));

    LOG("Driver v%s starting initialization (built %s %s)",
        RDNA4_VERSION_STRING, __DATE__, __TIME__);
    setProperty("DriverVersion", RDNA4_VERSION_STRING);
    setProperty("BuildTimestamp", __DATE__ " " __TIME__);
    LOG_DEBUG(1, "Debug level: %u", debugLevel);

    // Serialises PM4 submission; user-space and kernel-side tests share the ring.
    if (!submitLock) submitLock = IOLockAlloc();

    // 读取设备信息
    if (!identifyDevice()) {
        LOG_ERROR("Device identification failed");
        releaseResources();
        return false;
    }

    // 初始化 PCI
    if (!initializePCI()) {
        LOG_ERROR("PCI initialization failed");
        releaseResources();
        return false;
    }

    // 映射 BARs
    if (!mapBARs()) {
        LOG_ERROR("BAR mapping failed");
        releaseResources();
        return false;
    }

    // 探测 GPU
    if (!probeGPU()) {
        LOG_ERROR("GPU probe failed");
        releaseResources();
        return false;
    }

    // IP Discovery: 读取硬件 IP 版本与寄存器基址
    if (!performIPDiscovery()) {
        LOG_ERROR("IP Discovery failed");
        releaseResources();
        return false;
    }

    // NBIO doorbell 初始化（须在 GMC 之前，对应参考实现的 IHInit）
    // 添加了完整的错误检查和日志，失败时安全降级
    performNBIOInit();

    // GMC 初始化（在 PSP 之前，与参考实现的顺序一致）
    if (!performGMCInit()) {
        LOG_ERROR("GMC initialization failed");
        releaseResources();
        return false;
    }

    performPSPInit();
    performSMUInit();
    performGFXInit();
    
    // 初始化 compute ring（GFX 已就绪之后）
    if (!initComputeRing()) {
        LOG_ERROR("Compute Ring initialization failed (non-fatal)");
        // ring 失败不阻断启动，后续仍可走无 ring 的调试路径
    }

    // 图形 (GFX) 队列。默认关闭：MES 的 ADD_QUEUE 用 pipe0/queue0，
    // 而 compute 队列已经占用同一个槽，映射 GFX 队列会把 compute 队列顶掉
    // （实测现象：compute 的 fence 到达但 WRITE_DATA 不再落盘）。
    // 用 rdna4-gfx-queue=1 单独开启，便于继续定位槽位方案。
    uint32_t gfxQueueEnable = 0;
    if (PE_parse_boot_argn("rdna4-gfx-queue", &gfxQueueEnable, sizeof(gfxQueueEnable)) &&
        gfxQueueEnable != 0) {
        if (!initGraphicsRing()) {
            LOG_ERROR("Graphics ring initialization failed (non-fatal)");
        }
    }

    // 可选的自检 compute 提交（只打印到 dmesg）
    uint32_t computeTest = 0;
    if (PE_parse_boot_argn("rdna4-compute-test", &computeTest, sizeof(computeTest)) &&
        computeTest != 0) {
        LOG("rdna4-compute-test=%u: running vectorAdd", computeTest);
        testVectorAdd();
    }

    LOG("RDNA4 Driver started successfully");

    LOG("Device: %s (%04x:%04x rev %02x)",
        deviceName, vendorID, deviceID, revisionID);

    registerService();
    return true;
}

IOReturn RDNA4Driver::newUserClient(task_t owningTask, void* securityID,
                                       UInt32 type, IOUserClient** handler) {
    if (!handler) return kIOReturnBadArgument;
    *handler = nullptr;
    (void)securityID;

    // Only the compute interface is exposed; type is reserved for future
    // splits (window mapping, profiling) and must be 0 today.
    if (type != 0) return kIOReturnUnsupported;
    if (!computeRing) {
        LOG_ERROR("newUserClient: compute ring not initialized");
        return kIOReturnNotReady;
    }

    RDNA4DriverUserClient* client = OSTypeAlloc(RDNA4DriverUserClient);
    if (!client) return kIOReturnNoMemory;

    if (!client->initWithTask(owningTask, securityID, type)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->attach(this)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }

    *handler = client;
    LOG("newUserClient: compute user client created");
    return kIOReturnSuccess;
}

void RDNA4Driver::releaseResources() {
    freeDiscovery();
    gmcFini(gmcContext);
    
    if (graphicsRing) {
        delete graphicsRing;
        graphicsRing = nullptr;
    }
    graphicsQueueMapped = false;

    // 释放 compute ring
    if (computeRing) {
        delete computeRing;
        computeRing = nullptr;
    }
    computeHeapNext = 0;
    userHeapNext = 0;
    vramBlocksReset();

    if (submitLock) {
        IOLockFree(submitLock);
        submitLock = nullptr;
    }
    clearStateGpuAddr = 0;
    
    unmapBARs();

    if (pciDevice) {
        pciDevice->release();
        pciDevice = nullptr;
    }

    gfxFirmwareLoaded = false;
}

void RDNA4Driver::stop(IOService* provider) {
    LOG("stop() called");
    releaseResources();
    IOService::stop(provider);
}

void RDNA4Driver::free() {
    LOG("free() called");
    releaseResources();
    IOService::free();
}

bool RDNA4Driver::isPspAlive() {
    OSObject* stageObj = copyProperty("PSP,Stage");
    OSString* stage = OSDynamicCast(OSString, stageObj);
    bool alive = stage && stage->isEqualTo("alive");
    OSSafeReleaseNULL(stageObj);
    return alive;
}

//==============================================================================
// 初始化方法
//==============================================================================

// parseBootArgs 已移除 - boot-arg 检查现在在 probe() 中进行

bool RDNA4Driver::identifyDevice() {
    uint32_t vendorDevice = pciDevice->configRead32(kIOPCIConfigVendorID);
    vendorID = vendorDevice & 0xFFFF;
    deviceID = (vendorDevice >> 16) & 0xFFFF;
    revisionID = pciDevice->configRead32(kIOPCIConfigRevisionID) & 0xFF;
    uint32_t subsys = pciDevice->configRead32(kIOPCIConfigSubSystemVendorID);
    subsysVendorID = subsys & 0xFFFF;
    subsysDeviceID = (subsys >> 16) & 0xFFFF;

    deviceName = getDeviceNameForID(deviceID);

    LOG_DEBUG(2, "Vendor ID: 0x%04x", vendorID);
    LOG_DEBUG(2, "Device ID: 0x%04x", deviceID);
    LOG_DEBUG(2, "Revision ID: 0x%02x", revisionID);
    LOG_DEBUG(2, "Subsystem: %04x:%04x", subsysVendorID, subsysDeviceID);

    return true;
}

const char* RDNA4Driver::getDeviceNameForID(uint16_t devID) {
    switch (devID) {
        case RDNA4_DEVICE_NAVI48_9070XT:
            return "AMD Radeon RX 9070 / 9070 XT / 9070 GRE";
        case RDNA4_DEVICE_NAVI48_R9700:
            return "AMD Radeon AI PRO R9700";
        case RDNA4_DEVICE_NAVI44_9060XT:
            return "AMD Radeon RX 9050 / 9060 XT";
        default:
            return "Unknown RDNA4 GPU";
    }
}

bool RDNA4Driver::initializePCI() {
    LOG_DEBUG(1, "Initializing PCI...");

    // 启用 Bus Master
    if (!enableBusMaster()) {
        LOG_ERROR("Failed to enable bus master");
        return false;
    }

    // 打印 BAR 信息
    for (int i = 0; i < 6; i++) {
        IODeviceMemory* mem = pciDevice->getDeviceMemoryWithRegister(
            static_cast<UInt8>(kIOPCIConfigBaseAddress0 + (i * 4))
        );
        if (mem) {
            uint64_t size = mem->getLength();
            LOG_DEBUG(2, "BAR%d: %llu bytes (%.1f MB)",
                     i, size, size / (1024.0 * 1024.0));
        }
    }

    return true;
}

bool RDNA4Driver::enableBusMaster() {
    uint16_t cmd16 = pciDevice->configRead16(kIOPCIConfigCommand);
    LOG_DEBUG(3, "PCI command register: 0x%04x", cmd16);

    cmd16 |= (kIOPCICommandBusMaster | kIOPCICommandMemorySpace);
    pciDevice->configWrite16(kIOPCIConfigCommand, cmd16);

    // 验证
    cmd16 = pciDevice->configRead16(kIOPCIConfigCommand);
    LOG_DEBUG(3, "PCI command after: 0x%04x", cmd16);

    return (cmd16 & kIOPCICommandBusMaster) != 0;
}

bool RDNA4Driver::mapBARs() {
    LOG_DEBUG(1, "Mapping BARs...");

    bool success = true;

    // BAR5 - 寄存器 (必需)
    if (!mapRegisters()) {
        LOG_ERROR("Failed to map BAR5 (registers)");
        return false;
    }

    // BAR0 - VRAM（可选）
    if (!mapVRAM()) {
        LOG("Warning: Failed to map BAR0 (VRAM) - continuing");
    }

    // BAR2 - Doorbell（可选）
    if (!mapDoorbells()) {
        LOG("Warning: Failed to map BAR2 (doorbells) - continuing");
    }

    return success;
}

void RDNA4Driver::unmapBARs() {
    LOG_DEBUG(1, "Unmapping BARs...");
    unmapDoorbells();
    unmapVRAM();
    unmapRegisters();
}

bool RDNA4Driver::mapRegisters() {
    LOG_DEBUG(2, "Mapping BAR5 (registers)...");

    IODeviceMemory* bar = pciDevice->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress5
    );

    if (!bar) {
        LOG_ERROR("Failed to get BAR5");
        return false;
    }

    rmmioMap = bar->map();
    if (!rmmioMap) {
        LOG_ERROR("Failed to map BAR5");
        return false;
    }

    rmmio = (volatile uint32_t*)rmmioMap->getVirtualAddress();
    rmmioSize = rmmioMap->getLength();

    LOG("BAR5 mapped: %p, size: %zu bytes (%.1f KB)",
        rmmio, rmmioSize, rmmioSize / 1024.0);

    return checkRegisterAccess();
}

void RDNA4Driver::unmapRegisters() {
    if (rmmioMap) {
        rmmioMap->release();
        rmmioMap = nullptr;
        rmmio = nullptr;
        rmmioSize = 0;
    }
}

bool RDNA4Driver::mapVRAM() {
    LOG_DEBUG(2, "Mapping BAR0 (VRAM)...");

    IODeviceMemory* bar = pciDevice->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress0
    );

    if (!bar) {
        return false;
    }

    vramMap = bar->map();
    if (!vramMap) {
        return false;
    }

    vram = (volatile uint8_t*)vramMap->getVirtualAddress();
    vramSize = vramMap->getLength();

    LOG("BAR0 mapped: %p, size: %zu bytes (%.1f MB)",
        vram, vramSize, vramSize / (1024.0 * 1024.0));

    return true;
}

void RDNA4Driver::unmapVRAM() {
    if (vramMap) {
        vramMap->release();
        vramMap = nullptr;
        vram = nullptr;
        vramSize = 0;
    }
}

bool RDNA4Driver::mapDoorbells() {
    LOG_DEBUG(2, "Mapping BAR2 (doorbells)...");

    IODeviceMemory* bar = pciDevice->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress2
    );

    if (!bar) {
        return false;
    }

    doorbellMap = bar->map();
    if (!doorbellMap) {
        return false;
    }

    doorbell = (volatile uint32_t*)doorbellMap->getVirtualAddress();
    doorbellSize = doorbellMap->getLength();

    LOG("BAR2 mapped: %p, size: %zu bytes (%.1f MB)",
        doorbell, doorbellSize, doorbellSize / (1024.0 * 1024.0));

    return true;
}

void RDNA4Driver::unmapDoorbells() {
    if (doorbellMap) {
        doorbellMap->release();
        doorbellMap = nullptr;
        doorbell = nullptr;
        doorbellSize = 0;
    }
}

bool RDNA4Driver::probeGPU() {
    LOG_DEBUG(1, "Probing GPU...");

    if (!rmmio) {
        LOG_ERROR("Registers not mapped");
        return false;
    }

    // 读取一些基本寄存器验证访问
    uint32_t scratch = regRead32(0x1A18);
    LOG_DEBUG(2, "Scratch register (0x1A18): 0x%08x", scratch);

    uint32_t config = regRead32(0x0);
    LOG_DEBUG(2, "Config register (0x0): 0x%08x", config);

    LOG("GPU probe completed");
    LOG_DEBUG(1, "Register reads: %u, writes: %u", regReadCount, regWriteCount);

    return true;
}

bool RDNA4Driver::readBasicInfo() {
    // 基础信息读取
    // IP Discovery 在 start() 中完成
    return true;
}

bool RDNA4Driver::checkRegisterAccess() {
    if (!rmmio || rmmioSize == 0) {
        return false;
    }

    // 尝试读取一个安全的寄存器
    uint32_t value = regRead32(0x0);

    // 验证不是全 0xFF (映射失败的典型值)
    if (value == 0xFFFFFFFF) {
        LOG_ERROR("Register access check failed (read 0xFFFFFFFF)");
        return false;
    }

    LOG_DEBUG(3, "Register access check passed");
    return true;
}

//==============================================================================
// 寄存器访问
//==============================================================================

uint32_t RDNA4Driver::regRead32(uint32_t offset) {
    if (!rmmio || offset > rmmioSize || rmmioSize - offset < sizeof(uint32_t)) {
        LOG_ERROR("Invalid register read: offset=0x%x, size=%zu", offset, rmmioSize);
        return 0xFFFFFFFF;
    }

    uint32_t value = OSReadLittleInt32(rmmio, offset);
    regReadCount++;

    if (debugLevel >= 5) {
        LOG_DEBUG(5, "REG_R[0x%04x] = 0x%08x", offset, value);
    }

    return value;
}

void RDNA4Driver::regWrite32(uint32_t offset, uint32_t value) {
    if (!rmmio || offset > rmmioSize || rmmioSize - offset < sizeof(uint32_t)) {
        LOG_ERROR("Invalid register write: offset=0x%x, size=%zu", offset, rmmioSize);
        return;
    }

    if (debugLevel >= 5) {
        LOG_DEBUG(5, "REG_W[0x%04x] = 0x%08x", offset, value);
    }

    OSWriteLittleInt32(rmmio, offset, value);
    regWriteCount++;
}

//==============================================================================
// 公共接口
//==============================================================================

uint32_t RDNA4Driver::readRegister(uint32_t offset) {
    return regRead32(offset);
}

bool RDNA4Driver::copyFirmware(const char* name, const uint8_t*& bytes, size_t& size) const {
    if (!name || strcmp(name, "psp_14_0_3_sos.bin") != 0)
        return false;
    bytes = psp_14_0_3_sos;
    size = sizeof(psp_14_0_3_sos);
    return size != 0;
}

void RDNA4Driver::writeRegister(uint32_t offset, uint32_t value) {
    regWrite32(offset, value);
}

//==============================================================================
// IP Discovery
//==============================================================================

static constexpr uint32_t kDiscoveryTmrSize = 10 << 10;
static constexpr uint32_t kDiscoveryTmrOffset = 64 << 10;

void RDNA4Driver::freeDiscovery() {
    if (onDieDisc) {
        IOFree(onDieDisc, kDiscoveryTmrSize);
        onDieDisc = nullptr;
    }
    ipDiscovery = IPDiscovery();
}

uint32_t RDNA4Driver::vramRead32(uint64_t pos) {
    regWrite32(0x18, static_cast<uint32_t>(pos >> 31));
    regWrite32(0x00, (static_cast<uint32_t>(pos) & 0x7ffffffc) | 0x80000000u);
    return regRead32(0x04);
}

bool RDNA4Driver::performIPDiscovery() {
    LOG_DEBUG(1, "Performing IP Discovery...");
    freeDiscovery();

    if (!rmmio) {
        LOG_ERROR("IP Discovery requires register MMIO");
        return false;
    }

    uint32_t vramMB = regRead32(0x378c);
    if (vramMB == 0 || vramMB == 0xFFFFFFFF) {
        LOG_ERROR("IP Discovery cannot read VRAM size: 0x%08x", vramMB);
        return false;
    }

    onDieDisc = static_cast<uint8_t*>(IOMalloc(kDiscoveryTmrSize));
    if (!onDieDisc) {
        LOG_ERROR("Failed to allocate %u-byte IP Discovery TMR buffer", kDiscoveryTmrSize);
        return false;
    }

    uint64_t pos = (static_cast<uint64_t>(vramMB) << 20) - kDiscoveryTmrOffset;
    uint32_t* dwords = reinterpret_cast<uint32_t*>(onDieDisc);
    for (uint32_t i = 0; i < kDiscoveryTmrSize / 4; i++)
        dwords[i] = vramRead32(pos + 4ULL * i);

    LOG("Discovery: on-die TMR at vram+0x%llx, first dwords %08x %08x",
        pos, dwords[0], dwords[1]);
    if (!ipDiscovery.init(onDieDisc, kDiscoveryTmrSize)) {
        LOG_ERROR("IP Discovery on-die binary did not validate");
        freeDiscovery();
        return false;
    }

    uint16_t count = ipDiscovery.ipCount();
    IPDiscovery::IpEntry entry;
    if (!ipDiscovery.findIp(IPDiscovery::HwGc, 0, entry)) {
        LOG_ERROR("Graphics Compute (GC) not found - critical");
        freeDiscovery();
        return false;
    }

    setProperty("IPDiscoveryComplete", true);
    setProperty("Discovery,Source", "on-die TMR");
    setProperty("DiscoveredIPBlocks", static_cast<uint64_t>(count), 32);
    setProperty("HasGC", true);
    setProperty("GCVersion", static_cast<uint64_t>(entry.major), 8);

    if (ipDiscovery.findIp(IPDiscovery::HwMp0, 0, entry))
        setProperty("HasPSP", true);
    else
        LOG("Warning: PSP (MP0) not found");

    if (ipDiscovery.findIp(IPDiscovery::HwMp1, 0, entry))
        setProperty("HasSMU", true);
    else
        LOG("Warning: SMU (MP1) not found");

    if (ipDiscovery.findIp(IPDiscovery::HwDmu, 0, entry))
        setProperty("HasDCN", true);
    if (ipDiscovery.findIp(IPDiscovery::HwSdma0, 0, entry))
        setProperty("HasSDMA", true);

    LOG("IP Discovery completed successfully (%u IPs)", count);
    return true;
}

namespace {

class DriverPSPHardware : public PSPHardware {
public:
    explicit DriverPSPHardware(RDNA4Driver* driver) : driver_(driver) {}

    uint32_t read(Block block, uint32_t dword) override {
        uint32_t offset = 0;
        if (!offsetFor(block, dword, offset)) return 0xFFFFFFFF;
        return driver_->readRegister(offset);
    }

    bool write(Block block, uint32_t dword, uint32_t value) override {
        uint32_t offset = 0;
        if (!offsetFor(block, dword, offset)) return false;
        driver_->writeRegister(offset, value);
        return true;
    }

    bool fill(uint64_t offset, uint8_t value, size_t length) override {
        volatile uint8_t* vram = nullptr;
        size_t size = 0;
        driver_->vramWindow(vram, size);
        if (!vram || offset > size || length > size - offset) return false;
        volatile uint32_t* words = reinterpret_cast<volatile uint32_t*>(vram + offset);
        const uint32_t pattern = static_cast<uint32_t>(value) * 0x01010101u;
        for (size_t i = 0; i < length / 4; i++) words[i] = pattern;
        for (size_t i = length & ~3ull; i < length; i++) vram[offset + i] = value;
#if defined(__x86_64__)
        __asm__ __volatile__("sfence" ::: "memory");
#endif
        return true;
    }

    void wait(uint32_t milliseconds) override { IOSleep(milliseconds); }

    bool flush(uint64_t registerBytes) override {
        (void)registerBytes;
        // 使用标准 HDP 刷新寄存器 (HwOsssys, dword 0x0014: regHDP_MEM_FLUSH_CNTL)
        uint32_t hdpReg = 0;
        if (driver_->discovery().regByteOffset(IPDiscovery::HwOsssys, 0, 0, 0x0014, hdpReg)) {
            driver_->writeRegister(hdpReg, 1);
            (void)driver_->readRegister(hdpReg);
            return true;
        }
        return false;
    }

    uint32_t readVRAM(uint64_t offset) override {
        volatile uint8_t* base = nullptr;
        size_t size = 0;
        driver_->vramWindow(base, size);
        if (!base || offset > size || size - offset < 4) return 0xFFFFFFFF;
        return *reinterpret_cast<volatile uint32_t*>(base + offset);
    }

    uint32_t pointer() override {
        uint32_t offset = 0;
        if (!driver_->discovery().regByteOffset(IPDiscovery::HwMp0, 0, 0, 0x0083, offset))
            return 0;
        return driver_->readRegister(offset);
    }

    bool copy(uint64_t offset, const void* source, size_t length) override {
        volatile uint8_t* vram = nullptr;
        size_t size = 0;
        driver_->vramWindow(vram, size);
        if (!vram || !source || offset > size || length > size - offset) return false;
        const uint8_t* bytes = static_cast<const uint8_t*>(source);
        volatile uint32_t* words = reinterpret_cast<volatile uint32_t*>(vram + offset);
        size_t i = 0;
        for (; i + 4 <= length; i += 4) {
            uint32_t value;
            memcpy(&value, bytes + i, 4);
            words[i / 4] = value;
        }
        for (; i < length; i++) vram[offset + i] = bytes[i];
#if defined(__x86_64__)
        __asm__ __volatile__("sfence" ::: "memory");
#endif
        return true;
    }

private:
    bool offsetFor(Block block, uint32_t dword, uint32_t& offset) const {
        const uint16_t hwId = block == Block::PSP ? IPDiscovery::HwMp0 : IPDiscovery::HwMmhub;
        return driver_->discovery().regByteOffset(hwId, 0, 0, dword, offset);
    }

    RDNA4Driver* driver_;
};

}

void RDNA4Driver::performNBIOInit() {
    // NBIO doorbell 路径初始化
    // 参考实现在 IHInit 阶段的 ensure_doorbell_path
    
    LOG("NBIO: starting doorbell path initialization...");
    
    // 获取 BAR2 物理地址
    if (!doorbellMap) {
        LOG("NBIO: BAR2 not mapped, skipping doorbell init");
        setProperty("NBIO,Stage", "skipped-no-bar2");
        return;
    }
    
    IOPhysicalAddress bar2Physical = doorbellMap->getPhysicalAddress();
    
    LOG("NBIO: BAR2 mapped at phys 0x%llx, size %llu KB",
        static_cast<unsigned long long>(bar2Physical),
        static_cast<unsigned long long>(doorbellSize >> 10));
    
    // 尝试初始化，失败时安全降级
    bool success = false;
    
    // 添加 try-catch 式的错误边界
    do {
        LOG("NBIO: attempting doorbell path init...");
        
        if (!NBIODoorbell::initDoorbellPath(*this, bar2Physical)) {
            LOG("NBIO: doorbell path initialization FAILED");
            LOG("NBIO: system will continue without doorbell init (may affect GC power-on)");
            setProperty("NBIO,Stage", "failed-safe-fallback");
            break;
        }
        
        LOG("NBIO: doorbell path initialized successfully!");
        setProperty("NBIO,Stage", "complete");
        setProperty("NBIO,BAR2Physical", static_cast<uint64_t>(bar2Physical), 64);
        success = true;
        
    } while (false);
    
    if (!success) {
        LOG("NBIO: continuing boot without doorbell init...");
    }
    
    // 无论成功失败，都继续启动（不阻塞系统）
}

void RDNA4Driver::performPSPInit() {
    // 需要 rdna4-psp=1 启用（用于逐步调试）
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("rdna4-psp", &enabled, sizeof(enabled)) || enabled == 0) {
        LOG("PSP init skipped (set rdna4-psp=1 to test)");
        setProperty("PSP,Stage", "disabled-by-bootarg");
        return;
    }
    
    LOG("PSP: starting initialization (rdna4-psp=%u)", enabled);
    
    if (!vram || vramSize < PSPLoader::StagingSize) {
        LOG_ERROR("PSP init requires the BAR0 VRAM window");
        setProperty("PSP,Stage", "no-vram");
        return;
    }

    const uint8_t* firmware = nullptr;
    size_t firmwareSize = 0;
    if (!copyFirmware("psp_14_0_3_sos.bin", firmware, firmwareSize)) {
        LOG_ERROR("PSP firmware psp_14_0_3_sos.bin is not embedded");
        setProperty("PSP,Stage", "no-firmware");
        return;
    }

    PSPLoader loader;
    if (!loader.parse(firmware, firmwareSize)) {
        LOG_ERROR("PSP firmware container is invalid");
        setProperty("PSP,Stage", "bad-firmware");
        return;
    }
    // 将 PSP 安全工作区基址设为 64 MiB（避开 VRAM 底部 0~64MB 的所有显示帧缓冲与扫描线通道，遵循 Cobalt64 标准规范）
    const uint64_t staging = VramLayout::kPspStagingBase;
    if (staging > vramSize || PSPLoader::StagingSize > vramSize - staging ||
        !loader.setStagingWindow(staging, 0, VramLayout::kPspStagingBase)) {
        LOG_ERROR("PSP staging window does not fit BAR0");
        setProperty("PSP,Stage", "no-staging");
        return;
    }

    LOG("PSP: Starting firmware load (9 images + SOS)...");
    DriverPSPHardware hardware(this);
    if (!loader.load(hardware, vramSize, rmmioSize)) {
        LOG_ERROR("PSP SOS load failed");
        setProperty("PSP,Stage", "load-failed");
        return;
    }
    LOG("PSP: Firmware load complete!");

    LOG("PSP: Creating ring command...");
    PSPVersions versions {};
    if (!loader.command(hardware, staging + PSPLoader::StagingSize, rmmioSize)) {
        LOG_ERROR("PSP ring command was not accepted");
        setProperty("PSP,Stage", "command-failed");
        return;
    }
    LOG("PSP: Ring command accepted!");
    
    LOG("PSP: Reading SOS versions...");
    if (!loader.readVersions(hardware, versions)) {
        LOG_ERROR("PSP SOS did not report a version");
        setProperty("PSP,Stage", "no-version");
        return;
    }
    setProperty("PSP,Stage", "alive");
    setProperty("PSP,SOSSignOfLife", static_cast<uint64_t>(versions.signOfLife), 32);
    setProperty("PSP,FirmwareVersion", static_cast<uint64_t>(versions.firmware), 32);
    setProperty("PSP,FeatureVersion", static_cast<uint64_t>(versions.feature), 32);
    LOG("PSP SOS alive: sign-of-life 0x%08x firmware 0x%08x feature 0x%08x",
        versions.signOfLife, versions.firmware, versions.feature);
}

namespace {

class DriverSMUHardware : public SMUHardware {
public:
    explicit DriverSMUHardware(RDNA4Driver* driver) : driver_(driver) {}
    uint32_t read(uint32_t dword) override {
        uint32_t offset = 0;
        if (!driver_->discovery().regByteOffset(IPDiscovery::HwMp1, 0, 1, dword, offset))
            return 0xFFFFFFFF;
        return driver_->readRegister(offset);
    }
    bool write(uint32_t dword, uint32_t value) override {
        uint32_t offset = 0;
        if (!driver_->discovery().regByteOffset(IPDiscovery::HwMp1, 0, 1, dword, offset))
            return false;
        driver_->writeRegister(offset, value);
        return true;
    }
    void wait(uint32_t milliseconds) override { IOSleep(milliseconds); }
private:
    RDNA4Driver* driver_;
};

}

void RDNA4Driver::performSMUInit() {
    // 需要 rdna4-smu=1 启用（用于逐步调试）
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("rdna4-smu", &enabled, sizeof(enabled)) || enabled == 0) {
        LOG("SMU init skipped (set rdna4-smu=1 to test)");
        setProperty("SMU,Stage", "disabled-by-bootarg");
        return;
    }
    if (!isPspAlive()) {
        LOG("SMU init skipped (PSP not alive; enable rdna4-psp=1)");
        setProperty("SMU,Stage", "psp-not-alive");
        return;
    }
    if (!vram) {
        LOG_ERROR("SMU init requires the BAR0 VRAM window");
        setProperty("SMU,Stage", "no-vram");
        return;
    }
    SMUFirmware firmware;
    if (!SMUFirmware::parse(smu_14_0_3, sizeof(smu_14_0_3), firmware)) {
        LOG_ERROR("SMU firmware header is invalid");
        setProperty("SMU,Stage", "bad-firmware");
        return;
    }
    if (VramLayout::kSmuFirmware > vramSize ||
        firmware.payloadSize > vramSize - VramLayout::kSmuFirmware) {
        LOG_ERROR("SMU firmware does not fit the BAR0 VRAM window");
        setProperty("SMU,Stage", "no-vram");
        return;
    }
    const uint64_t firmwareOffset = VramLayout::kSmuFirmware;
    PSPLoader psp;
    DriverPSPHardware pspHardware(this);
    PSPCommandResult firmwareResult;
    if (!psp.loadFirmwareWithResult(pspHardware, firmwareOffset, firmware.payload, firmware.payloadSize, 18,
                                    VramLayout::kPspRing,
                                    VramLayout::kPspCommand,
                                    VramLayout::kPspFence, rmmioSize,
                                    firmwareResult)) {
        setProperty("SMU,FirmwareStatus", static_cast<uint64_t>(firmwareResult.status), 32);
        setProperty("SMU,FirmwareFence", static_cast<uint64_t>(firmwareResult.fence), 32);
        setProperty("SMU,FirmwarePointer", static_cast<uint64_t>(firmwareResult.pointer), 32);
        setProperty("SMU,FirmwareFinalPointer", static_cast<uint64_t>(firmwareResult.finalPointer), 32);
        setProperty("SMU,FirmwareMailbox", static_cast<uint64_t>(firmwareResult.mailbox), 32);
        setProperty("SMU,FirmwareAddressHigh", static_cast<uint64_t>(firmwareResult.addressHigh), 32);
        LOG_ERROR("SMU firmware was not accepted by PSP");
        setProperty("SMU,Stage", "firmware-failed");
        return;
    }
    DriverSMUHardware hardware(this);
    uint32_t echo = 0;
    if (!SMUMailbox::test(hardware, 0xABCD0001, echo) || echo != 0xABCD0002) {
        LOG_ERROR("SMU TestMessage failed (echo 0x%08x)", echo);
        setProperty("SMU,Stage", "ping-failed");
        return;
    }
    uint32_t version = 0;
    if (!SMUMailbox::version(hardware, version)) {
        LOG_ERROR("SMU version query failed");
        setProperty("SMU,Stage", "version-failed");
        return;
    }
    setProperty("SMU,Stage", "alive");
    uint32_t interfaceVersion = 0;
    if (!SMUMailbox::interfaceVersion(hardware, interfaceVersion)) {
        LOG_ERROR("SMU interface version query failed");
        setProperty("SMU,Stage", "interface-failed");
        return;
    }
    setProperty("SMU,FirmwareVersion", static_cast<uint64_t>(version), 32);
    setProperty("SMU,InterfaceVersion", static_cast<uint64_t>(interfaceVersion), 32);
    LOG("SMU alive: firmware 0x%08x interface 0x%08x", version, interfaceVersion);
    
    // 完整的 SMU hw_setup 流程
    performSMUSetup();
}

void RDNA4Driver::performSMUSetup() {
    DriverSMUHardware hardware(this);

    // Step 1: 专用 64KB driver table 区域（避开低 64MB 显示区和 VRAM 顶部保留区）
    const uint64_t driverTableOffset = VramLayout::kSmuDriverTable;
    const uint64_t driverTableSize = VramLayout::kSmuDriverTableSize;

    if (!vram || driverTableOffset > vramSize ||
        driverTableSize > vramSize - driverTableOffset) {
        LOG_ERROR("SMU: driver table does not fit the BAR0 VRAM window");
        setProperty("SMU,Stage", "no-driver-table");
        return;
    }

    volatile uint32_t* table = reinterpret_cast<volatile uint32_t*>(
        vram + driverTableOffset);
    for (size_t i = 0; i < driverTableSize / sizeof(uint32_t); i++) {
        table[i] = 0;
    }

    // Step 2: 告知 SMU driver table 地址
    // 读取 VRAM MC base address (MMHUB MC_VM_FB_LOCATION_BASE)
    uint32_t fbBaseReg = 0;
    if (!discovery().regByteOffset(IPDiscovery::HwMmhub, 0, 0, 0x0554, fbBaseReg)) {
        LOG_ERROR("SMU: failed to resolve FB base register");
        setProperty("SMU,Stage", "no-fb-base");
        return;
    }
    const uint32_t fbBaseRaw = readRegister(fbBaseReg);
    if (fbBaseRaw == 0xFFFFFFFF) {
        LOG_ERROR("SMU: cannot read FB_LOCATION_BASE");
        setProperty("SMU,Stage", "no-fb-base");
        return;
    }
    const uint64_t memoryBase = static_cast<uint64_t>(fbBaseRaw & 0x00ffffffu) << 24;
    const uint64_t driverTableAddr = memoryBase + driverTableOffset;

    if (!SMUMailbox::setDriverTableAddress(hardware, driverTableAddr)) {
        LOG_ERROR("SMU: SetDriverTableAddress failed");
        setProperty("SMU,Stage", "driver-table-failed");
        return;
    }
    LOG("SMU: driver table at 0x%llx (vram+0x%llx, %llu bytes)",
        static_cast<unsigned long long>(driverTableAddr),
        static_cast<unsigned long long>(driverTableOffset),
        static_cast<unsigned long long>(driverTableSize));
    setProperty("SMU,DriverTableAddress", driverTableAddr, 64);
    
    // Step 3: 设置 feature mask（允许所有 features）
    if (!SMUMailbox::setFeatureMask(hardware, 0xFFFFFFFF, 0xFFFFFFFF)) {
        LOG("SMU: SetFeatureMask failed (non-fatal)");
    } else {
        LOG("SMU: feature mask set to 0xFFFFFFFF:0xFFFFFFFF");
    }
    
    // Step 4: 使用默认 power table
    if (!SMUMailbox::useDefaultPowerTable(hardware)) {
        LOG("SMU: UseDefaultPPTable failed (non-fatal)");
    } else {
        LOG("SMU: default power table loaded");
    }
    
    // Step 5: 运行 DcBtc（Display Configuration BTC）
    if (!SMUMailbox::runDcBtc(hardware)) {
        LOG("SMU: RunDcBtc failed (non-fatal)");
    } else {
        LOG("SMU: DcBtc completed");
    }
    
    // Step 6: 通知电源类型（0 = AC power）
    if (!SMUMailbox::notifyPowerSource(hardware, 0)) {
        LOG("SMU: NotifyPowerSource failed (non-fatal)");
    } else {
        LOG("SMU: power source = AC");
    }
    
    // Step 7: 启用所有 SMU features（关键步骤）
    if (!SMUMailbox::enableAllFeatures(hardware)) {
        LOG("SMU: EnableAllSmuFeatures failed - PMFW may auto-enable features");
        setProperty("SMU,SetupStage", "auto-features");
    } else {
        LOG("SMU: all features enabled explicitly");
        setProperty("SMU,SetupStage", "complete");
    }
    
    // Step 8: 等待 GC 启动（SMU Setup 可能触发了 GC 通电，但需要时间）
    LOG("SMU: setup complete, waiting for GC to power on...");
    IOSleep(100);  // 给 GC 100ms 启动时间
    
    // Step 9: 轮询 RLC BOOTLOAD_STATUS 确认 GC 已启动
    uint32_t bootloadByte = 0;
    bool bootloadOk = discovery().regByteOffset(IPDiscovery::HwGc, 0, 1, 0x4E7C, bootloadByte);
    
    if (bootloadOk) {
        for (uint32_t attempt = 0; attempt < 100; attempt++) {
            uint32_t status = readRegister(bootloadByte);
            if (status != 0) {
                LOG("SMU: GC responding! BOOTLOAD_STATUS = 0x%08x after %u ms", status, attempt * 10);
                if (status & 0x80000000u) {
                    LOG("SMU: RLC autoload COMPLETE");
                    setProperty("SMU,GCPowerState", "active");
                } else {
                    LOG("SMU: GC active but RLC autoload pending");
                    setProperty("SMU,GCPowerState", "partial");
                }
                return;
            }
            IOSleep(10);
        }
        LOG("SMU: GC still not responding after 1000ms (BOOTLOAD_STATUS = 0)");
        setProperty("SMU,GCPowerState", "off");
    } else {
        LOG("SMU: cannot resolve GC BOOTLOAD_STATUS register");
        setProperty("SMU,GCPowerState", "unknown");
    }
}

namespace {
uint32_t mecHeaderU32(const uint8_t* p, size_t offset) {
    return static_cast<uint32_t>(p[offset]) |
           (static_cast<uint32_t>(p[offset + 1]) << 8) |
           (static_cast<uint32_t>(p[offset + 2]) << 16) |
           (static_cast<uint32_t>(p[offset + 3]) << 24);
}
} // namespace

void RDNA4Driver::programMecStartAddress() {
    // gfx_v12_0_config_gfx_rs64(): on the PSP load path Linux calls this
    // unconditionally in hw_init.  All three RS64 cores (PFP, ME, MEC) get
    // their program counter from the firmware header and a reset pulse so the
    // new PC takes effect.  gfx_firmware_header_v2_0: ucode_start_addr_lo at
    // 0x34, _hi at 0x38.  Only the MEC half used to be done here; a PFP/ME
    // left at PC 0 keeps CP_STAT busy for ever and MES never gets a handshake.
    auto startOf = [](const uint8_t* fw, uint32_t& lo32, uint32_t& hi32) {
        const uint32_t lo = mecHeaderU32(fw, 0x34);
        const uint32_t hi = mecHeaderU32(fw, 0x38);
        lo32 = (lo >> 2) | (hi << 30);
        hi32 = hi >> 2;
        return !(lo == 0 && hi == 0);
    };

    uint32_t pfpLo = 0, pfpHi = 0, meLo = 0, meHi = 0, mecLo = 0, mecHi = 0;
    const bool havePfp = startOf(gc_12_0_1_pfp, pfpLo, pfpHi);
    const bool haveMe = startOf(gc_12_0_1_me, meLo, meHi);
    const bool haveMec = startOf(gc_12_0_1_mec, mecLo, mecHi);
    if (!haveMec) {
        LOG_ERROR("MEC: firmware header has no start address");
        return;
    }

    // --- PFP: program both pipes, then pulse PFP_PIPE0/1_RESET ---
    if (havePfp) {
        for (uint32_t pipe = 0; pipe < 2; pipe++) {
            grbmSelect(0, pipe, 0, 0);
            writeGcRegister(0x1E44, pfpLo, 0);   // CP_PFP_PRGRM_CNTR_START
            writeGcRegister(0x1E59, pfpHi, 0);   // CP_PFP_PRGRM_CNTR_START_HI
        }
        grbmSelect(0, 0, 0, 0);
        uint32_t me = readGcRegister(0x0803, 1); // CP_ME_CNTL
        writeGcRegister(0x0803, me | (1u << 18) | (1u << 19), 1);
        writeGcRegister(0x0803, me & ~((1u << 18) | (1u << 19)), 1);
    }

    // --- ME: program both pipes, then pulse ME_PIPE0/1_RESET ---
    if (haveMe) {
        for (uint32_t pipe = 0; pipe < 2; pipe++) {
            grbmSelect(0, pipe, 0, 0);
            writeGcRegister(0x1E45, meLo, 0);    // CP_ME_PRGRM_CNTR_START
            writeGcRegister(0x1E79, meHi, 0);    // CP_ME_PRGRM_CNTR_START_HI
        }
        grbmSelect(0, 0, 0, 0);
        uint32_t me = readGcRegister(0x0803, 1);
        writeGcRegister(0x0803, me | (1u << 20) | (1u << 21), 1);
        writeGcRegister(0x0803, me & ~((1u << 20) | (1u << 21)), 1);
    }

    // --- MEC: all four pipes, then pulse MEC_PIPE0..3_RESET ---
    const uint32_t before = readGcRegister(0x2900, 1);
    uint32_t after = 0;
    for (uint32_t pipe = 0; pipe < 4; pipe++) {
        grbmSelect(1, pipe, 0, 0);
        writeGcRegister(0x2900, mecLo, 1);
        writeGcRegister(0x2938, mecHi, 1);
        if (pipe == 0) after = readGcRegister(0x2900, 1);
    }
    grbmSelect(0, 0, 0, 0);
    {
        uint32_t rs = readGcRegister(0x2904, 1);  // CP_MEC_RS64_CNTL
        writeGcRegister(0x2904, rs | (0xFu << 16), 1);
        writeGcRegister(0x2904, rs & ~(0xFu << 16), 1);
    }

    LOG("RS64: pfp=0x%08x:%08x me=0x%08x:%08x mec=0x%08x:%08x (mec cntr 0x%08x -> 0x%08x)",
        pfpHi, pfpLo, meHi, meLo, mecHi, mecLo, before, after);
    setProperty("GFX,PfpStartLo", static_cast<uint64_t>(pfpLo), 32);
    setProperty("GFX,MeStartLo", static_cast<uint64_t>(meLo), 32);
    setProperty("GFX,MecStartLo", static_cast<uint64_t>(mecLo), 32);
    setProperty("GFX,MecStartHi", static_cast<uint64_t>(mecHi), 32);
    setProperty("GFX,MecStartRegBefore", static_cast<uint64_t>(before), 32);
    setProperty("GFX,MecStartRegAfter", static_cast<uint64_t>(after), 32);
}

void RDNA4Driver::performGFXInit() {
    // 需要 rdna4-gfx=1 启用（用于逐步调试）
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("rdna4-gfx", &enabled, sizeof(enabled)) || enabled == 0) {
        LOG("GFX init skipped (set rdna4-gfx=1 to test)");
        setProperty("GFX,Stage", "disabled-by-bootarg");
        return;
    }
    
    LOG("GFX: starting initialization (rdna4-gfx=%u)", enabled);

    if (!isPspAlive()) {
        LOG("GFX init skipped (PSP not alive; enable rdna4-psp=1)");
        setProperty("GFX,Stage", "psp-not-alive");
        return;
    }

    // 只有本驱动实例成功提交过 GFX 固件才跳过；PSP,Stage 仅代表 SOS 存活。
    if (gfxFirmwareLoaded) {
        LOG("GFX: firmware already loaded, skipping reload");
        // 直接读取 GC 寄存器验证状态
        uint32_t bootloadByte = 0;
        uint32_t bootload = 0xFFFFFFFF;
        for (uint32_t attempt = 0; attempt < 10000; attempt++) {
            if (!discovery().regByteOffset(IPDiscovery::HwGc, 0, 1, 0x4E7C, bootloadByte)) break;
            bootload = readRegister(bootloadByte);
            if ((bootload & 0x80000000u) != 0) break;
            IOSleep(1);
        }
        uint32_t alternateByte = 0;
        const uint32_t alternate = discovery().regByteOffset(IPDiscovery::HwGc, 0, 0, 0x4E7C, alternateByte)
            ? readRegister(alternateByte) : 0xFFFFFFFF;
        uint32_t grbmByte = 0;
        const bool grbmOk = discovery().regByteOffset(IPDiscovery::HwGc, 0, 0, 0x0DA4, grbmByte);
        const uint32_t grbm = grbmOk ? readRegister(grbmByte) : 0xFFFFFFFF;
        IPDiscovery::IpEntry gc;
        const bool gcFound = discovery().findIp(IPDiscovery::HwGc, 0, gc);
        setProperty("GFX,GCBaseCount", static_cast<uint64_t>(gcFound ? gc.numBases : 0), 32);
        setProperty("GFX,GCBase0", static_cast<uint64_t>(gcFound && gc.numBases > 0 ? gc.bases[0] : 0xFFFFFFFF), 32);
        setProperty("GFX,GCBase1", static_cast<uint64_t>(gcFound && gc.numBases > 1 ? gc.bases[1] : 0xFFFFFFFF), 32);
        setProperty("GFX,GRBMByteOffset", static_cast<uint64_t>(grbmOk ? grbmByte : 0), 32);
        setProperty("GFX,GRBMStatus", static_cast<uint64_t>(grbm), 32);
        setProperty("GFX,BootloadStatus", static_cast<uint64_t>(bootload), 32);
        setProperty("GFX,BootloadStatusGC0", static_cast<uint64_t>(alternate), 32);
        setProperty("GFX,Stage", (bootload & 0x80000000u) ? "gfx-alive" : "autoload-pending");
        LOG("GFX autoload requested; bootload status 0x%08x", bootload);
        if (bootload & 0x80000000u) {
            programMecStartAddress();
            const uint32_t mesLo = mecHeaderU32(gc_12_0_1_uni_mes, 0x38);
            const uint32_t mesHi = mecHeaderU32(gc_12_0_1_uni_mes, 0x3C);
            mesUcStart = (static_cast<uint64_t>(mesHi) << 32) | mesLo;
            setProperty("GFX,MesUcStart", mesUcStart, 64);
            gmcEnableGFXHUBGART(*this, gmcContext);
        }
        return;
    }
    
    // 固件未加载，执行完整的加载流程
    GFXFirmware sdma;
    if (!GFXFirmware::parseSingle(sdma_7_0_1, sizeof(sdma_7_0_1), 71, sdma)) {
        LOG_ERROR("SDMA firmware is invalid");
        setProperty("GFX,Stage", "bad-sdma");
        return;
    }
    GFXFirmware firmware;
    if (!GFXFirmware::parseIMU(gc_12_0_1_imu, sizeof(gc_12_0_1_imu), firmware)) {
        LOG_ERROR("GFX IMU firmware is invalid");
        setProperty("GFX,Stage", "bad-firmware");
        return;
    }
    const uint64_t kPspSafeVramBase = VramLayout::kPspStagingBase;
    PSPLoader psp;
    DriverPSPHardware hardware(this);
    GFXFirmware tocFirmware;
    if (!GFXFirmware::parseSOSImage(psp_14_0_3_sos, sizeof(psp_14_0_3_sos), 4, tocFirmware)) {
        LOG_ERROR("PSP TOC is missing");
        setProperty("GFX,Stage", "bad-toc");
        return;
    }
    const GFXFirmwarePart& toc = tocFirmware.parts[0];
    const uint64_t tocOffset = kPspSafeVramBase + 18ull * PSPLoader::StagingSize;
    uint8_t tocCommand[1024] = {};
    const uint32_t tocCommandId = 0x20;
    const uint64_t memoryBase = static_cast<uint64_t>(
        hardware.read(PSPHardware::Block::MemoryHub, 0x0554) & 0x00ffffffu) << 24;
    const uint32_t tocLow = static_cast<uint32_t>(memoryBase + tocOffset);
    const uint32_t tocHigh = static_cast<uint32_t>((memoryBase + tocOffset) >> 32);
    const uint32_t tocSize = toc.size;
    __builtin_memcpy(tocCommand + 8, &tocCommandId, 4);
    __builtin_memcpy(tocCommand + 28, &tocLow, 4);
    __builtin_memcpy(tocCommand + 32, &tocHigh, 4);
    __builtin_memcpy(tocCommand + 36, &tocSize, 4);
    PSPCommandResult tocResult;
    if (!hardware.fill(tocOffset, 0, PSPLoader::StagingSize) ||
        !hardware.copy(tocOffset, toc.bytes, toc.size) ||
        !hardware.fill(kPspSafeVramBase + 2ull * PSPLoader::StagingSize, 0, sizeof(tocCommand)) ||
        !hardware.copy(kPspSafeVramBase + 2ull * PSPLoader::StagingSize, tocCommand, sizeof(tocCommand)) ||
        !psp.submitPrepared(hardware, kPspSafeVramBase + PSPLoader::StagingSize,
                            kPspSafeVramBase + 2ull * PSPLoader::StagingSize,
                            kPspSafeVramBase + 3ull * PSPLoader::StagingSize, rmmioSize, tocResult)) {
        setProperty("GFX,Stage", "toc-failed");
        setProperty("GFX,FirmwareStatus", static_cast<uint64_t>(tocResult.status), 32);
        setProperty("GFX,FirmwareFence", static_cast<uint64_t>(tocResult.fence), 32);
        LOG_ERROR("PSP LOAD_TOC was not accepted");
        return;
    }
    for (uint32_t index = 0; index < sdma.count; index++) {
        const GFXFirmwarePart& part = sdma.parts[index];
        const uint64_t firmwareOffset = kPspSafeVramBase + (19ull + index) * PSPLoader::StagingSize;
        PSPCommandResult result;
        if (!psp.loadFirmwareWithResult(hardware, firmwareOffset, part.bytes, part.size, part.type,
                                        kPspSafeVramBase + PSPLoader::StagingSize,
                                        kPspSafeVramBase + 2ull * PSPLoader::StagingSize,
                                        kPspSafeVramBase + 3ull * PSPLoader::StagingSize,
                                        rmmioSize, result)) {
            setProperty("GFX,Stage", "sdma-failed");
            setProperty("GFX,FirmwareType", static_cast<uint64_t>(part.type), 32);
            setProperty("GFX,FirmwareStatus", static_cast<uint64_t>(result.status), 32);
            setProperty("GFX,FirmwareFence", static_cast<uint64_t>(result.fence), 32);
            LOG_ERROR("SDMA firmware type %u was not accepted", part.type);
            return;
        }
    }
    for (uint32_t index = 0; index < firmware.count; index++) {
        const GFXFirmwarePart& part = firmware.parts[index];
        const uint64_t firmwareOffset = kPspSafeVramBase + (5ull + index) * PSPLoader::StagingSize;
        PSPCommandResult result;
        if (!psp.loadFirmwareWithResult(hardware, firmwareOffset, part.bytes, part.size, part.type,
                                        kPspSafeVramBase + PSPLoader::StagingSize,
                                        kPspSafeVramBase + 2ull * PSPLoader::StagingSize,
                                        kPspSafeVramBase + 3ull * PSPLoader::StagingSize,
                                        rmmioSize, result)) {
            setProperty("GFX,Stage", "firmware-failed");
            setProperty("GFX,FirmwareType", static_cast<uint64_t>(part.type), 32);
            setProperty("GFX,FirmwareStatus", static_cast<uint64_t>(result.status), 32);
            setProperty("GFX,FirmwareFence", static_cast<uint64_t>(result.fence), 32);
            LOG_ERROR("GFX firmware type %u was not accepted", part.type);
            return;
        }
    }
    GFXFirmware sequence;
    const uint32_t pfpStack[] = {90};
    const uint32_t meStack[] = {92};
    const uint32_t mecStack[] = {94, 95};
    GFXFirmware pfp, me, mec, mes, rlc;
    if (!GFXFirmware::parseCP(gc_12_0_1_pfp, sizeof(gc_12_0_1_pfp), 87, pfpStack, 1, pfp) ||
        !GFXFirmware::parseCP(gc_12_0_1_me, sizeof(gc_12_0_1_me), 88, meStack, 1, me) ||
        !GFXFirmware::parseCP(gc_12_0_1_mec, sizeof(gc_12_0_1_mec), 89, mecStack, 2, mec) ||
        !GFXFirmware::parseMES(gc_12_0_1_uni_mes, sizeof(gc_12_0_1_uni_mes), mes)) {
        LOG_ERROR("GFX CP or MES firmware is invalid");
        setProperty("GFX,Stage", "bad-cp");
        return;
    }
    GFXFirmware* groups[] = {&pfp, &me, &mec, &mes};
    for (GFXFirmware* group : groups) {
        for (uint32_t index = 0; index < group->count; index++) {
            if (sequence.count >= GFXFirmware::MaxParts) {
                setProperty("GFX,Stage", "too-many-parts");
                return;
            }
            sequence.parts[sequence.count++] = group->parts[index];
        }
    }
    for (uint32_t index = 0; index < sequence.count; index++) {
        const GFXFirmwarePart& part = sequence.parts[index];
        const uint64_t firmwareOffset = kPspSafeVramBase + (7ull + index) * PSPLoader::StagingSize;
        PSPCommandResult result;
        if (!psp.loadFirmwareWithResult(hardware, firmwareOffset, part.bytes, part.size, part.type,
                                        kPspSafeVramBase + PSPLoader::StagingSize,
                                        kPspSafeVramBase + 2ull * PSPLoader::StagingSize,
                                        kPspSafeVramBase + 3ull * PSPLoader::StagingSize,
                                        rmmioSize, result)) {
            setProperty("GFX,Stage", "cp-failed");
            setProperty("GFX,FirmwareType", static_cast<uint64_t>(part.type), 32);
            setProperty("GFX,FirmwareStatus", static_cast<uint64_t>(result.status), 32);
            setProperty("GFX,FirmwareFence", static_cast<uint64_t>(result.fence), 32);
            LOG_ERROR("GFX firmware type %u was not accepted", part.type);
            return;
        }
    }
    if (!GFXFirmware::parseRLC(gc_12_0_1_rlc, sizeof(gc_12_0_1_rlc), rlc)) {
        LOG_ERROR("GFX RLC firmware is invalid");
        setProperty("GFX,Stage", "bad-rlc");
        return;
    }
    for (uint32_t index = 0; index < rlc.count; index++) {
        const GFXFirmwarePart& part = rlc.parts[index];
        const uint64_t firmwareOffset = kPspSafeVramBase + (7ull + sequence.count + index) * PSPLoader::StagingSize;
        PSPCommandResult result;
        if (!psp.loadFirmwareWithResult(hardware, firmwareOffset, part.bytes, part.size, part.type,
                                        kPspSafeVramBase + PSPLoader::StagingSize,
                                        kPspSafeVramBase + 2ull * PSPLoader::StagingSize,
                                        kPspSafeVramBase + 3ull * PSPLoader::StagingSize,
                                        rmmioSize, result)) {
            setProperty("GFX,Stage", "rlc-failed");
            setProperty("GFX,FirmwareType", static_cast<uint64_t>(part.type), 32);
            setProperty("GFX,FirmwareStatus", static_cast<uint64_t>(result.status), 32);
            setProperty("GFX,FirmwareFence", static_cast<uint64_t>(result.fence), 32);
            LOG_ERROR("GFX RLC type %u was not accepted", part.type);
            return;
        }
    }
    GFXFirmware registerList;
    if (!GFXFirmware::parseSOSImage(psp_14_0_3_sos, sizeof(psp_14_0_3_sos), 6, registerList)) {
        setProperty("GFX,Stage", "bad-register-list");
        return;
    }
    uint8_t autoloadCommand[1024] = {};
    const uint32_t autoloadCommandId = 0x21;
    __builtin_memcpy(autoloadCommand + 8, &autoloadCommandId, 4);
    PSPCommandResult autoloadResult;
    if (!hardware.fill(kPspSafeVramBase + 2ull * PSPLoader::StagingSize, 0, sizeof(autoloadCommand)) ||
        !hardware.copy(kPspSafeVramBase + 2ull * PSPLoader::StagingSize, autoloadCommand, sizeof(autoloadCommand)) ||
        !psp.submitPrepared(hardware, kPspSafeVramBase + PSPLoader::StagingSize,
                            kPspSafeVramBase + 2ull * PSPLoader::StagingSize,
                            kPspSafeVramBase + 3ull * PSPLoader::StagingSize, rmmioSize, autoloadResult)) {
        setProperty("GFX,Stage", "autoload-failed");
        setProperty("GFX,FirmwareStatus", static_cast<uint64_t>(autoloadResult.status), 32);
        setProperty("GFX,FirmwareFence", static_cast<uint64_t>(autoloadResult.fence), 32);
        return;
    }
    const GFXFirmwarePart& registers = registerList.parts[0];
    const uint64_t registerOffset = kPspSafeVramBase + 23ull * PSPLoader::StagingSize;
    PSPCommandResult registerResult;
    if (!psp.loadFirmwareWithResult(hardware, registerOffset, registers.bytes, registers.size, 67,
                                    kPspSafeVramBase + PSPLoader::StagingSize,
                                    kPspSafeVramBase + 2ull * PSPLoader::StagingSize,
                                    kPspSafeVramBase + 3ull * PSPLoader::StagingSize,
                                    rmmioSize, registerResult)) {
        setProperty("GFX,Stage", "register-list-failed");
        setProperty("GFX,FirmwareStatus", static_cast<uint64_t>(registerResult.status), 32);
        setProperty("GFX,FirmwareFence", static_cast<uint64_t>(registerResult.fence), 32);
        return;
    }
    gfxFirmwareLoaded = true;
    uint32_t bootloadByte = 0;
    uint32_t bootload = 0xFFFFFFFF;
    for (uint32_t attempt = 0; attempt < 10000; attempt++) {
        if (!discovery().regByteOffset(IPDiscovery::HwGc, 0, 1, 0x4E7C, bootloadByte)) break;
        bootload = readRegister(bootloadByte);
        if ((bootload & 0x80000000u) != 0) break;
        IOSleep(1);
    }
    uint32_t alternateByte = 0;
    const uint32_t alternate = discovery().regByteOffset(IPDiscovery::HwGc, 0, 0, 0x4E7C, alternateByte)
        ? readRegister(alternateByte) : 0xFFFFFFFF;
    uint32_t grbmByte = 0;
    const bool grbmOk = discovery().regByteOffset(IPDiscovery::HwGc, 0, 0, 0x0DA4, grbmByte);
    const uint32_t grbm = grbmOk ? readRegister(grbmByte) : 0xFFFFFFFF;
    IPDiscovery::IpEntry gc;
    const bool gcFound = discovery().findIp(IPDiscovery::HwGc, 0, gc);
    setProperty("GFX,GCBaseCount", static_cast<uint64_t>(gcFound ? gc.numBases : 0), 32);
    setProperty("GFX,GCBase0", static_cast<uint64_t>(gcFound && gc.numBases > 0 ? gc.bases[0] : 0xFFFFFFFF), 32);
    setProperty("GFX,GCBase1", static_cast<uint64_t>(gcFound && gc.numBases > 1 ? gc.bases[1] : 0xFFFFFFFF), 32);
    setProperty("GFX,GRBMByteOffset", static_cast<uint64_t>(grbmOk ? grbmByte : 0), 32);
    setProperty("GFX,GRBMStatus", static_cast<uint64_t>(grbm), 32);
    setProperty("GFX,BootloadStatus", static_cast<uint64_t>(bootload), 32);
    setProperty("GFX,BootloadStatusGC0", static_cast<uint64_t>(alternate), 32);
    setProperty("GFX,Stage", (bootload & 0x80000000u) ? "gfx-alive" : "autoload-pending");
    LOG("GFX autoload requested; bootload status 0x%08x", bootload);
    if (bootload & 0x80000000u) {
        programMecStartAddress();
        const uint32_t mesLo = mecHeaderU32(gc_12_0_1_uni_mes, 0x38);
        const uint32_t mesHi = mecHeaderU32(gc_12_0_1_uni_mes, 0x3C);
        mesUcStart = (static_cast<uint64_t>(mesHi) << 32) | mesLo;
        setProperty("GFX,MesUcStart", mesUcStart, 64);
        gmcEnableGFXHUBGART(*this, gmcContext);
    }
}


//==============================================================================
// GMC 初始化
//==============================================================================

extern bool gmcInit(RDNA4Driver& driver, GMCContext& gmc);
extern bool gmcEnableGFXHUBGART(RDNA4Driver& driver, GMCContext& gmc);

bool RDNA4Driver::performGMCInit() {
    LOG_DEBUG(2, "Starting GMC initialization...");
    
    if (!gmcInit(*this, gmcContext)) {
        LOG_ERROR("GMC initialization failed");
        return false;
    }
    
    setProperty("GMC,VRAMStart", gmcContext.vramStart, 64);
    setProperty("GMC,VRAMSize", gmcContext.vramSize, 64);
    
    return true;
}
#include "../hardware/PM4BuilderKernel.hpp"
#include <IOKit/IOLib.h>

#include "../shaders/compiled/vectoradd_gfx12.h"

bool RDNA4Driver::testVectorAdd() {
    LOG("======================================");
    LOG("  Vector Add Test (FULL)");
    LOG("======================================");

    if (!computeRing) {
        LOG_ERROR("testVectorAdd: compute ring not initialized");
        return false;
    }

    // Step 0: queue liveness probe.  One WRITE_DATA + fence through the ring,
    // no shader involved.  If the magic never lands the queue is not draining
    // and the dispatch below cannot be blamed on the kernel or its ABI.
    {
        const uint64_t probe = allocateVRAMBuffer(4096);
        if (probe) {
            volatile uint32_t* probeCpu = reinterpret_cast<volatile uint32_t*>(
                const_cast<uint8_t*>(vram) + (probe - gmcContext.vramStart));
            probeCpu[0] = 0xDEADBEEFu;
            gmcHDPFlush(*this);

            PM4BuilderKernel pm4;
            pm4.emitWriteData(probe, 0x4B435121u);   // 'KCQ!'
            const uint64_t fenceValue = computeRing->allocateFence();
            const uint64_t fenceGpuAddr =
                gmcContext.vramStart + computeRing->getFenceVRAMOffset();
            pm4.emitFenceWrite(fenceGpuAddr, fenceValue);

            bool landed = false;
            bool fenced = false;
            if (submitComputeCommands(pm4.getPackets(), pm4.getPacketCount())) {
                fenced = waitForComputeFence(fenceValue, 2000);
                landed = (probeCpu[0] == 0x4B435121u);
            }
            LOG("  Queue probe: WRITE_DATA %s, fence %s (probe=0x%08x)",
                landed ? "landed" : "did NOT land",
                fenced ? "reached" : "timed out", probeCpu[0]);
            setProperty("ComputeTest,ProbeLanded", static_cast<uint64_t>(landed ? 1 : 0), 32);
            setProperty("ComputeTest,ProbeFenced", static_cast<uint64_t>(fenced ? 1 : 0), 32);
            setProperty("ComputeTest,ProbeValue", static_cast<uint64_t>(probeCpu[0]), 32);
            if (!landed) {
                setProperty("ComputeTest,Stage", "probe-failed");
                dumpComputeQueueState();
                return false;
            }

            // Step 0a: the same WRITE_DATA, but inside an indirect buffer
            // (gfx_v12_0_ring_test_ib).  The dispatch below lives in an IB;
            // if this magic never lands the CP is not executing IBs at all
            // and no amount of shader work can be blamed.
            const uint64_t ibProbe = allocateVRAMBuffer(4096);
            if (ibProbe) {
                probeCpu[1] = 0xDEADBEEFu;
                PM4BuilderKernel ib;
                ib.emitWriteData(probe + 4, 0x1B7E57EDu);   // "IB tested"
                volatile uint32_t* ibCpu = reinterpret_cast<volatile uint32_t*>(
                    const_cast<uint8_t*>(vram) + (ibProbe - gmcContext.vramStart));
                for (size_t i = 0; i < ib.getPacketCount(); i++) ibCpu[i] = ib.getPackets()[i];
                gmcHDPFlush(*this);

                PM4BuilderKernel ring;
                ring.emitIndirectBuffer(ibProbe, static_cast<uint32_t>(ib.getPacketCount()), 0);
                const uint64_t fv = computeRing->allocateFence();
                ring.emitFenceWrite(fenceGpuAddr, fv);
                bool ibFenced = false;
                if (submitComputeCommands(ring.getPackets(), ring.getPacketCount())) {
                    ibFenced = waitForComputeFence(fv, 2000);
                }
                const bool ibLanded = (probeCpu[1] == 0x1B7E57EDu);
                LOG("  IB probe: WRITE_DATA-in-IB %s, fence %s (value=0x%08x)",
                    ibLanded ? "landed" : "did NOT land",
                    ibFenced ? "reached" : "timed out", probeCpu[1]);
                setProperty("ComputeTest,IbProbeLanded", static_cast<uint64_t>(ibLanded ? 1 : 0), 32);
                setProperty("ComputeTest,IbProbeFenced", static_cast<uint64_t>(ibFenced ? 1 : 0), 32);
                setProperty("ComputeTest,IbProbeValue", static_cast<uint64_t>(probeCpu[1]), 32);
            }
        }
    }
    
    // Step 0b (rdna4-compute-test>=2): shader control.  Cobalt64's store_magic
    // (hardware-verified on this exact GPU): one wave32, lane i stores
    // 0xCA11ED47+i at s[0:1]+4*i, scope:SCOPE_SYS + global_wb so the readback
    // does not depend on GCR bits.  RSRC1=0x600F0000 RSRC2=0x84, workgroup
    // 32/1/1, grid 1/1/1.  Runs behind a step-wise probe that submits each
    // SET_SH_REG packet in its own IB with its own fence, naming the first
    // packet the MEC cannot execute.  If the control lands and vectorAdd does
    // not, the dispatch path is fine and the LLVM blob / kernarg ABI is what
    // to look at.
    uint32_t computeTestLevel = 0;
    PE_parse_boot_argn("rdna4-compute-test", &computeTestLevel, sizeof(computeTestLevel));
    if (computeTestLevel >= 2) {
        static const uint32_t kStoreMagic[] = {
            0x360000ffu, 0x000003ffu, 0x30020082u, 0x4a0400ffu, 0xca11ed47u,
            0xee068000u, 0x010c0000u, 0x00000001u, 0xbfc10000u, 0xee0b007cu,
            0x000c0000u, 0x00000000u, 0xbfc10000u, 0xbfb00000u,
        };
        const uint64_t code = allocateVRAMBuffer(4096);
        const uint64_t res = allocateVRAMBuffer(4096);
        const uint64_t ibb = allocateVRAMBuffer(4096);
        if (code && res && ibb) {
            volatile uint32_t* codeCpu = reinterpret_cast<volatile uint32_t*>(
                const_cast<uint8_t*>(vram) + (code - gmcContext.vramStart));
            volatile uint32_t* resCpu = reinterpret_cast<volatile uint32_t*>(
                const_cast<uint8_t*>(vram) + (res - gmcContext.vramStart));
            for (uint32_t i = 0; i < 1024; i++) codeCpu[i] = 0xBFB00000u; // s_endpgm fill
            for (uint32_t i = 0; i < sizeof(kStoreMagic) / 4; i++) codeCpu[i] = kStoreMagic[i];
            for (uint32_t i = 0; i < 64; i++) resCpu[i] = 0xDEADBEEFu;
            gmcHDPFlush(*this);

            // Step-wise probe: every packet of the dispatch IB submitted on its
            // own, in its own IB, behind its own fence.  The first fence that
            // does not land names the packet the MEC cannot execute.
            {
                const uint64_t stepIb = allocateVRAMBuffer(4096);
                uint32_t stuckAt = 0xFFFFFFFFu;
                uint32_t stepsOk = 0;
                if (stepIb) {
                    volatile uint32_t* stepCpu = reinterpret_cast<volatile uint32_t*>(
                        const_cast<uint8_t*>(vram) + (stepIb - gmcContext.vramStart));
                    PM4BuilderKernel full;
                    full.setComputeProgram(code, 0x600F0000u, 0x00000084u);
                    full.setComputeWorkgroupSize(32, 1, 1);
                    full.setComputeUserData(0, res);
                    const uint32_t* p = full.getPackets();
                    const size_t n = full.getPacketCount();
                    size_t off = 0;
                    uint32_t step = 0;
                    while (off < n) {
                        const uint32_t hdr = p[off];
                        const uint32_t len = ((hdr >> 16) & 0x3FFFu) + 2u;
                        for (uint32_t i = 0; i < len; i++) stepCpu[i] = p[off + i];
                        gmcHDPFlush(*this);
                        PM4BuilderKernel r;
                        r.emitIndirectBuffer(stepIb, len, 0);
                        const uint64_t sfv = computeRing->allocateFence();
                        r.emitFenceWrite(gmcContext.vramStart + computeRing->getFenceVRAMOffset(), sfv);
                        bool ok = false;
                        if (submitComputeCommands(r.getPackets(), r.getPacketCount())) {
                            ok = waitForComputeFence(sfv, 500);
                        }
                        LOG("  step %u: hdr=0x%08x reg=0x%04x len=%u -> %s", step, hdr,
                            p[off + 1] & 0xFFFFu, len, ok ? "fenced" : "STUCK");
                        if (!ok) {
                            stuckAt = (step << 16) | (p[off + 1] & 0xFFFFu);
                            break;
                        }
                        stepsOk++;
                        off += len;
                        step++;
                    }
                }
                setProperty("ComputeTest,StepStuckAt", static_cast<uint64_t>(stuckAt), 32);
                setProperty("ComputeTest,StepsOk", static_cast<uint64_t>(stepsOk), 32);
                if (stuckAt != 0xFFFFFFFFu) {
                    // The queue is wedged; the control dispatch below cannot run.
                    dumpComputeQueueState();
                    setProperty("ComputeTest,Stage", "step-stuck");
                    setProperty("ComputeTest,CtrlLanes", static_cast<uint64_t>(0), 32);
                    return false;
                }
            }

            PM4BuilderKernel cib;
            cib.setComputeProgram(code, 0x600F0000u, 0x00000084u);
            cib.setComputeWorkgroupSize(32, 1, 1);
            cib.setComputeUserData(0, res);
            cib.emitDispatchDirect(1, 1, 1);
            cib.emitCsPartialFlush();
            volatile uint32_t* ibCpu = reinterpret_cast<volatile uint32_t*>(
                const_cast<uint8_t*>(vram) + (ibb - gmcContext.vramStart));
            for (size_t i = 0; i < cib.getPacketCount(); i++) ibCpu[i] = cib.getPackets()[i];
            gmcHDPFlush(*this);

            PM4BuilderKernel ring;
            ring.emitIndirectBuffer(ibb, static_cast<uint32_t>(cib.getPacketCount()), 0);
            const uint64_t fv = computeRing->allocateFence();
            ring.emitFenceWrite(gmcContext.vramStart + computeRing->getFenceVRAMOffset(), fv);

            // gfx_v12_0_select_se_sh(0xffffffff x3) leaves GRBM_GFX_INDEX in
            // SE/SA/INSTANCE broadcast.  Nothing in our bring-up has written it
            // yet; a non-broadcast index makes SH/SPI register traffic from the
            // CP land on one (possibly harvested) instance.
            constexpr uint32_t kRegGrbmGfxIndex = 0x2200;  // BASE_IDX 1
            constexpr uint32_t kGrbmBroadcastAll = 0xE0000000u;
            const uint32_t gfxIndexBefore = readGcRegister(kRegGrbmGfxIndex, 1);
            if (gfxIndexBefore != 0xFFFFFFFFu && gfxIndexBefore != kGrbmBroadcastAll) {
                writeGcRegister(kRegGrbmGfxIndex, kGrbmBroadcastAll, 1);
            }
            setProperty("ComputeTest,GrbmGfxIndexBefore", static_cast<uint64_t>(gfxIndexBefore), 32);
            setProperty("ComputeTest,GrbmGfxIndex",
                        static_cast<uint64_t>(readGcRegister(kRegGrbmGfxIndex, 1)), 32);

            bool fenced = false;
            uint32_t wfPeak = 0, cpcPeak = 0, grbmPeak = 0, fenceUs = 0;
            if (submitComputeCommands(ring.getPackets(), ring.getPacketCount())) {
                // Sample SPI/CPC from the kick until the fence lands (or 2 s):
                // SPI_CSQ_WF_ACTIVE_STATUS is non-zero only while compute waves
                // are resident.  If it never rises the dispatch never reached
                // the shader engines.
                const volatile uint64_t* fenceSlot = reinterpret_cast<const volatile uint64_t*>(
                    const_cast<uint8_t*>(vram) + computeRing->getFenceVRAMOffset());
                for (uint32_t i = 0; i < 2000000; i++) {
                    const uint32_t wf = readGcRegister(0x127B, 0);
                    const uint32_t cpc = readGcRegister(0x0E25, 0);
                    const uint32_t g = readGcRegister(0x0DA4, 0);
                    if (wf != 0xFFFFFFFFu && wf > wfPeak) wfPeak = wf;
                    if (cpc != 0xFFFFFFFFu && cpc > cpcPeak) cpcPeak = cpc;
                    if (g != 0xFFFFFFFFu && (g & 0x00400000u)) grbmPeak = g;
                    if (*fenceSlot >= fv) {
                        fenced = true;
                        fenceUs = i;
                        break;
                    }
                    IODelay(1);
                }
            }
            setProperty("ComputeTest,CtrlFenceUs", static_cast<uint64_t>(fenceUs), 32);
            setProperty("ComputeTest,CtrlWfActivePeak", static_cast<uint64_t>(wfPeak), 32);
            setProperty("ComputeTest,CtrlCpcBusyPeak", static_cast<uint64_t>(cpcPeak), 32);
            setProperty("ComputeTest,CtrlGrbmSpiBusy", static_cast<uint64_t>(grbmPeak), 32);
            {
                const uint32_t stalled = readGcRegister(0x0E26, 0);
                const uint32_t cpcStat = readGcRegister(0x0E24, 0);
                setProperty("ComputeTest,CtrlCpcStalled", static_cast<uint64_t>(stalled), 32);
                setProperty("ComputeTest,CtrlCpcStatus", static_cast<uint64_t>(cpcStat), 32);
            }
            // SH register readback (Cobalt64 dump_sh_readback).  COMPUTE_* SH
            // registers are shadowed per ME/pipe; select the MEC pipe that owns
            // the queue first, otherwise this reads the untouched ME0 copy.
            {
                grbmSelect(1, 0, 0, 0);
                const uint32_t pgmLo = readGcRegister(0x1BAC, 0);
                const uint32_t pgmHi = readGcRegister(0x1BAD, 0);
                const uint32_t rsrc1 = readGcRegister(0x1BB2, 0);
                const uint32_t rsrc2 = readGcRegister(0x1BB3, 0);
                const uint32_t se0 = readGcRegister(0x1BB6, 0);
                const uint32_t ntx = readGcRegister(0x1BA7, 0);
                const uint32_t ud0 = readGcRegister(0x1BE0, 0);
                const uint32_t init = readGcRegister(0x1BA0, 0);
                grbmSelect(0, 0, 0, 0);
                LOG("  SH readback: PGM_LO=0x%08x PGM_HI=0x%08x RSRC1=0x%08x RSRC2=0x%08x "
                    "SE0=0x%08x NUM_THREAD_X=%u USER_DATA_0=0x%08x INITIATOR=0x%08x "
                    "(want PGM_LO=0x%08x RSRC1=0x600F0000 UD0=0x%08x)",
                    pgmLo, pgmHi, rsrc1, rsrc2, se0, ntx, ud0, init,
                    static_cast<uint32_t>(code >> 8), static_cast<uint32_t>(res));
                setProperty("ComputeTest,ShPgmLo", static_cast<uint64_t>(pgmLo), 32);
                setProperty("ComputeTest,ShPgmLoWant", static_cast<uint64_t>(code >> 8), 32);
                setProperty("ComputeTest,ShRsrc1", static_cast<uint64_t>(rsrc1), 32);
                setProperty("ComputeTest,ShRsrc2", static_cast<uint64_t>(rsrc2), 32);
                setProperty("ComputeTest,ShSe0", static_cast<uint64_t>(se0), 32);
                setProperty("ComputeTest,ShNumThreadX", static_cast<uint64_t>(ntx), 32);
                setProperty("ComputeTest,ShUserData0", static_cast<uint64_t>(ud0), 32);
                setProperty("ComputeTest,ShInitiator", static_cast<uint64_t>(init), 32);
            }
            uint32_t good = 0;
            for (uint32_t i = 0; i < 32; i++) {
                if (resCpu[i] == 0xCA11ED47u + i) good++;
            }
            LOG("  Control shader: fence %s, %u/32 lanes correct, r[0]=0x%08x r[1]=0x%08x",
                fenced ? "reached" : "timed out", good, resCpu[0], resCpu[1]);
            setProperty("ComputeTest,CtrlFenced", static_cast<uint64_t>(fenced ? 1 : 0), 32);
            setProperty("ComputeTest,CtrlLanes", static_cast<uint64_t>(good), 32);
            setProperty("ComputeTest,CtrlR0", static_cast<uint64_t>(resCpu[0]), 32);
            setProperty("ComputeTest,CtrlR1", static_cast<uint64_t>(resCpu[1]), 32);
        }
    }

    // Test parameters
    const size_t numElements = 1024;
    const size_t bufferSize = numElements * sizeof(float);
    const size_t workgroupSize = 256;  // Threads per workgroup
    const size_t numWorkgroups = (numElements + workgroupSize - 1) / workgroupSize;  // 4
    
    LOG("Configuration:");
    LOG("  Elements: %zu", numElements);
    LOG("  Buffer size: %zu bytes", bufferSize);
    LOG("  Workgroup size: %zu threads", workgroupSize);
    LOG("  Num workgroups: %zu", numWorkgroups);
    
    // Step 1: Allocate buffers
    LOG("");
    LOG("Step 1: Allocating VRAM buffers...");
    uint64_t bufferA = allocateVRAMBuffer(bufferSize);
    uint64_t bufferB = allocateVRAMBuffer(bufferSize);
    uint64_t bufferC = allocateVRAMBuffer(bufferSize);
    constexpr size_t kShaderBufferBytes = 16 * 1024;
    uint64_t shaderBuffer = allocateVRAMBuffer(kShaderBufferBytes);
    uint64_t kernargBuffer = allocateVRAMBuffer(4096);

    if (!bufferA || !bufferB || !bufferC || !shaderBuffer || !kernargBuffer) {
        LOG_ERROR("testVectorAdd: buffer allocation failed");
        return false;
    }
    if (g_vectorAddShaderSize > kShaderBufferBytes) {
        // 拷贝循环没有边界保护，shader 一旦变大就会写穿到相邻的 VRAM 分配。
        LOG_ERROR("testVectorAdd: shader blob %u bytes exceeds the %zu byte buffer",
                  g_vectorAddShaderSize, kShaderBufferBytes);
        return false;
    }
    
    LOG("  Buffer A (input):  GPU 0x%llx", bufferA);
    LOG("  Buffer B (input):  GPU 0x%llx", bufferB);
    LOG("  Buffer C (output): GPU 0x%llx", bufferC);
    LOG("  Shader buffer:     GPU 0x%llx", shaderBuffer);
    LOG("  Kernarg buffer:    GPU 0x%llx", kernargBuffer);
    
    // Step 2: Initialize input data
    LOG("");
    LOG("Step 2: Initializing input data...");
    
    uint64_t vramOffsetA = bufferA - gmcContext.vramStart;
    uint64_t vramOffsetB = bufferB - gmcContext.vramStart;
    uint64_t vramOffsetC = bufferC - gmcContext.vramStart;
    
    volatile float* vramA = (volatile float*)((uint8_t*)vram + vramOffsetA);
    volatile float* vramB = (volatile float*)((uint8_t*)vram + vramOffsetB);
    volatile float* vramC = (volatile float*)((uint8_t*)vram + vramOffsetC);
    
    for (size_t i = 0; i < numElements; i++) {
        vramA[i] = (float)i;
        vramB[i] = (float)(i * 2);
        vramC[i] = 0.0f;  // Clear output
    }
    
    LOG("  Input A: [0.0, 1.0, 2.0, ..., %f]", (float)(numElements-1));
    LOG("  Input B: [0.0, 2.0, 4.0, ..., %f]", (float)(numElements-1)*2);
    LOG("  Output C: [cleared to 0.0]");
    
    // Step 3: Upload shader to VRAM
    LOG("");
    LOG("Step 3: Uploading shader to VRAM...");
    
    uint64_t shaderVramOffset = shaderBuffer - gmcContext.vramStart;
    volatile uint8_t* shaderVram = (volatile uint8_t*)vram + shaderVramOffset;
    
    // Copy shader binary (byte view of the dword array)
    const uint8_t* shaderBytes = reinterpret_cast<const uint8_t*>(g_vectorAddShader);
    for (size_t i = 0; i < g_vectorAddShaderSize; i++) {
        shaderVram[i] = shaderBytes[i];
    }
    
    LOG("  Shader size: %u bytes", g_vectorAddShaderSize);
    LOG("  Shader at: GPU 0x%llx (VRAM+0x%llx)", shaderBuffer, shaderVramOffset);

    // Step 3.5: Build the kernarg segment. The kernel loads its three buffer
    // pointers from here (USER_SGPR = s[0:1] = this buffer's MC address).
    uint64_t kernargVramOffset = kernargBuffer - gmcContext.vramStart;
    volatile uint64_t* kernarg =
        reinterpret_cast<volatile uint64_t*>((uint8_t*)vram + kernargVramOffset);
    kernarg[0] = bufferA;
    kernarg[1] = bufferB;
    kernarg[2] = bufferC;
    gmcHDPFlush(*this);
    LOG("  Kernarg: A=0x%llx B=0x%llx C=0x%llx", kernarg[0], kernarg[1], kernarg[2]);
    
    // Step 4: Build the dispatch as an INDIRECT BUFFER.  Cobalt64's stage-17
    // finding: a dispatch written straight into the ring came up under the
    // wrong VMID and faulted on its own code page; the VMID a wave runs under
    // is carried by INDIRECT_BUFFER (0 = kernel, system aperture pass-through),
    // so the SET_SH_REG/DISPATCH stream goes into its own buffer and the ring
    // only carries INDIRECT_BUFFER + fence.
    LOG("");
    LOG("Step 4: Building PM4 dispatch IB...");

    PM4BuilderKernel ib;
    ib.setComputeProgram(shaderBuffer, g_vectorAddRSRC1, g_vectorAddRSRC2);
    LOG("  ✅ Set compute program (RSRC1=0x%08x, RSRC2=0x%08x)",
        g_vectorAddRSRC1, g_vectorAddRSRC2);
    ib.setComputeWorkgroupSize(workgroupSize, 1, 1);
    LOG("  ✅ Set workgroup size (%zu, 1, 1)", workgroupSize);
    ib.setComputeUserData(0, kernargBuffer);
    LOG("  ✅ Set kernarg pointer (s[0:1] = 0x%llx)", kernargBuffer);
    ib.emitDispatchDirect(numWorkgroups, 1, 1);
    LOG("  ✅ Emit DISPATCH_DIRECT (%zu, 1, 1)", numWorkgroups);
    ib.emitCsPartialFlush();
    LOG("  ✅ Emit CS_PARTIAL_FLUSH");

    const uint64_t ibBuffer = allocateVRAMBuffer(4096);
    if (!ibBuffer) {
        LOG_ERROR("testVectorAdd: IB allocation failed");
        return false;
    }
    {
        volatile uint32_t* ibVram = reinterpret_cast<volatile uint32_t*>(
            const_cast<uint8_t*>(vram) + (ibBuffer - gmcContext.vramStart));
        const uint32_t* src = ib.getPackets();
        for (size_t i = 0; i < ib.getPacketCount(); i++) ibVram[i] = src[i];
        gmcHDPFlush(*this);
    }
    LOG("  IB: %zu dwords at GPU 0x%llx", ib.getPacketCount(), ibBuffer);

    PM4BuilderKernel pm4;
    pm4.emitIndirectBuffer(ibBuffer, static_cast<uint32_t>(ib.getPacketCount()), 0);
    LOG("  ✅ Emit INDIRECT_BUFFER (vmid 0)");

    uint64_t fenceValue = computeRing->allocateFence();
    uint64_t fenceGpuAddr = gmcContext.vramStart + computeRing->getFenceVRAMOffset();
    pm4.emitFenceWrite(fenceGpuAddr, fenceValue);
    LOG("  ✅ Emit fence write (value=%llu, addr=0x%llx)", fenceValue, fenceGpuAddr);
    
    LOG("  Total ring packet size: %zu dwords (%zu bytes)", 
        pm4.getPacketCount(), pm4.getPacketCount() * 4);
    
    // Step 5: Submit commands to GPU
    LOG("");
    LOG("Step 5: Submitting PM4 commands to GPU...");
    
    if (!submitComputeCommands(pm4.getPackets(), pm4.getPacketCount())) {
        LOG_ERROR("testVectorAdd: command submission failed");
        return false;
    }
    
    LOG("  ✅ Commands submitted to Compute Ring");
    setProperty("ComputeTest,Stage", "submitted");
    
    // Step 6: Wait for completion
    LOG("");
    LOG("Step 6: Waiting for GPU completion...");
    
    if (!waitForComputeFence(fenceValue, 5000)) {  // 5 second timeout
        LOG_ERROR("testVectorAdd: fence timeout (GPU may not have executed)");
        // The fence packet may be the only thing that failed. Sample the
        // output buffer anyway so "not dispatched" and "dispatched but no
        // fence" are distinguishable.
        union { float f; uint32_t u; } t0, t1, t1023;
        t0.f = vramC[0];
        t1.f = vramC[1];
        t1023.f = vramC[1023];
        setProperty("ComputeTest,C0Bits", static_cast<uint64_t>(t0.u), 32);
        setProperty("ComputeTest,C1Bits", static_cast<uint64_t>(t1.u), 32);
        setProperty("ComputeTest,C1023Bits", static_cast<uint64_t>(t1023.u), 32);
        setProperty("ComputeTest,Stage", "fence-timeout");
        dumpComputeQueueState();
        return false;
    }
    
    LOG("  ✅ Fence reached (GPU completed)");
    
    // Step 7: Verify results
    LOG("");
    LOG("Step 7: Verifying results...");
    
    bool pass = true;
    size_t errorCount = 0;
    const size_t maxErrors = 10;
    
    for (size_t i = 0; i < numElements; i++) {
        float expected = (float)i + (float)(i * 2);  // a[i] + b[i] = i + 2i = 3i
        float actual = vramC[i];
        
        if (actual != expected) {
            if (errorCount < maxErrors) {
                LOG_ERROR("  Mismatch at [%zu]: got %f, expected %f", i, actual, expected);
            }
            errorCount++;
            pass = false;
        }
    }
    
    if (errorCount > maxErrors) {
        LOG_ERROR("  ... and %zu more errors", errorCount - maxErrors);
    }
    
    // Print sample results
    LOG("");
    LOG("Sample results:");
    LOG("  C[0] = %f (expected 0.0)", vramC[0]);
    LOG("  C[1] = %f (expected 3.0)", vramC[1]);
    LOG("  C[10] = %f (expected 30.0)", vramC[10]);
    LOG("  C[100] = %f (expected 300.0)", vramC[100]);
    LOG("  C[1023] = %f (expected 3069.0)", vramC[1023]);

    union { float f; uint32_t u; } bits0, bits1, bits1023;
    bits0.f = vramC[0];
    bits1.f = vramC[1];
    bits1023.f = vramC[1023];
    setProperty("ComputeTest,Stage", pass ? "passed" : "failed");
    setProperty("ComputeTest,Errors", static_cast<uint64_t>(errorCount), 32);
    setProperty("ComputeTest,C0Bits", static_cast<uint64_t>(bits0.u), 32);
    setProperty("ComputeTest,C1Bits", static_cast<uint64_t>(bits1.u), 32);
    setProperty("ComputeTest,C1023Bits", static_cast<uint64_t>(bits1023.u), 32);

    if (!pass) {
        // Did the shader write anything at all?  Count non-zero output words
        // and record the first few, so "no waves launched" and "waves ran but
        // stored to the wrong place" are distinguishable.
        uint32_t nonZero = 0;
        uint32_t firstNz = 0xFFFFFFFFu;
        for (size_t i = 0; i < numElements; i++) {
            union { float f; uint32_t u; } t;
            t.f = vramC[i];
            if (t.u) {
                nonZero++;
                if (firstNz == 0xFFFFFFFFu) firstNz = static_cast<uint32_t>(i);
            }
        }
        setProperty("ComputeTest,NonZeroOut", static_cast<uint64_t>(nonZero), 32);
        setProperty("ComputeTest,FirstNonZero", static_cast<uint64_t>(firstNz), 32);

        // GFXHUB aperture + fault state right after the dispatch.
        const IPDiscovery& disc = discovery();
        uint32_t off = 0;
        auto rd = [&](uint32_t reg) -> uint32_t {
            return disc.regByteOffset(IPDiscovery::HwGc, 0, 0, reg, off)
                ? readRegister(off) : 0xFFFFFFFFu;
        };
        setProperty("ComputeTest,GcApLow", static_cast<uint64_t>(rd(0x1619)), 32);
        setProperty("ComputeTest,GcApHigh", static_cast<uint64_t>(rd(0x161A)), 32);
        setProperty("ComputeTest,GcCtx0Cntl", static_cast<uint64_t>(rd(0x1624)), 32);
        setProperty("ComputeTest,GcL1Tlb", static_cast<uint64_t>(rd(0x161B)), 32);
        setProperty("ComputeTest,GcFaultLo", static_cast<uint64_t>(rd(0x15D0)), 32);
        setProperty("ComputeTest,GcFaultAddrLo", static_cast<uint64_t>(rd(0x15D2)), 32);
        setProperty("ComputeTest,GcFaultAddrHi", static_cast<uint64_t>(rd(0x15D3)), 32);
        LOG_ERROR("  nonZero=%u firstNz=%u GcAp=[0x%x,0x%x] ctx0=0x%x l1tlb=0x%x fault=0x%x",
                  nonZero, firstNz, rd(0x1619), rd(0x161A), rd(0x1624), rd(0x161B),
                  rd(0x15D0));
    }
    
    LOG("");
    LOG("======================================");
    if (pass) {
        LOG("  ✅ Vector Add Test: PASSED!");
        LOG("======================================");
        LOG("  All %zu elements computed correctly", numElements);
        LOG("  GPU compute execution successful!");
    } else {
        LOG("  ❌ Vector Add Test: FAILED!");
        LOG("======================================");
        LOG("  %zu errors found (out of %zu elements)", errorCount, numElements);
        LOG("");
        LOG("Possible causes:");
        LOG("  1. Stub shader used (need real shader from compile_shader.sh)");
        LOG("  2. PM4 packet format incorrect");
        LOG("  3. GFX engine not properly initialized");
        LOG("  4. Buffer addresses misconfigured");
    }
    LOG("======================================");
    
    return pass;
}
