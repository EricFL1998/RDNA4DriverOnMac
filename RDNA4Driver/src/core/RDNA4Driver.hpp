//
//  RDNA4Driver.hpp
//  Phase 1: 基础驱动框架
//
//  目标: 设备识别和基本 PCI 访问
//

#ifndef RDNA4Driver_hpp
#define RDNA4Driver_hpp

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

#include <IOKit/IOMemoryDescriptor.h>
#include "../hardware/IPDiscovery.hpp"
#include "../hardware/PSPLoader.hpp"
#include "../hardware/GMCContext.hpp"
#include "../hardware/ComputeRing.hpp"

// 版本信息
#define RDNA4_VERSION_MAJOR 0
#define RDNA4_VERSION_MINOR 2
#define RDNA4_VERSION_PATCH 0
#define RDNA4_VERSION_STRING "0.2.0"


// 日志宏
#define LOG(fmt, ...) IOLog("RDNA4: " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) IOLog("RDNA4 ERROR: " fmt "\n", ##__VA_ARGS__)
#define LOG_DEBUG(level, fmt, ...) \
    do { if (debugLevel >= level) IOLog("RDNA4 [%d]: " fmt "\n", level, ##__VA_ARGS__); } while(0)

// 支持的设备 ID
#define RDNA4_VENDOR_AMD    0x1002
// 0x7550 覆盖 Navi 48 的 RX 9070 / 9070 XT / 9070 GRE
#define RDNA4_DEVICE_NAVI48_9070XT  0x7550
// Navi 48 的 Radeon AI PRO R9700
#define RDNA4_DEVICE_NAVI48_R9700   0x7551
// Navi 44 的 RX 9050 / 9060 XT
#define RDNA4_DEVICE_NAVI44_9060XT  0x7590

//==============================================================================
// RDNA4Driver - 主驱动类
//==============================================================================

class RDNA4Driver : public IOService {
    OSDeclareDefaultStructors(RDNA4Driver)

public:
    //==========================================================================
    // IOService 生命周期
    //==========================================================================

    virtual IOService* probe(IOService* provider, SInt32* score) override;
    virtual bool start(IOService* provider) override;
    virtual void stop(IOService* provider) override;
    virtual void free() override;

    //==========================================================================
    // 公共接口
    //==========================================================================

    // 寄存器访问
    uint32_t readRegister(uint32_t offset);
    void writeRegister(uint32_t offset, uint32_t value);
    const IPDiscovery& discovery() const { return ipDiscovery; }
    void vramWindow(volatile uint8_t*& base, size_t& size) const {
        base = vram;
        size = vramSize;
    }
    bool copyFirmware(const char* name, const uint8_t*& bytes, size_t& size) const;

    // 设备信息
    uint16_t getVendorID() const { return vendorID; }
    uint16_t getDeviceID() const { return deviceID; }
    uint16_t getRevisionID() const { return revisionID; }
    const char* getDeviceName() const { return deviceName; }

private:
    // Phase 1.2: IP Discovery
    IPDiscovery ipDiscovery;
    uint8_t* onDieDisc { nullptr };
    bool performIPDiscovery();
    void performPSPInit();
    void performSMUInit();
    void performSMUSetup();
    void performNBIOInit();
    void performGFXInit();
    uint32_t vramRead32(uint64_t pos);
    void freeDiscovery();

    // Phase 1.6: GMC
    GMCContext gmcContext;
    bool performGMCInit();
    
    // Phase 2.3: Compute Ring
    ComputeRing* computeRing { nullptr };
    uint64_t computeHeapNext { 0 };
    uint64_t computeMqdGpuAddr { 0 };
    // MES maps the queue itself: the MQD must say active=0 so the map packet,
    // not the MQD contents, brings the HQD up.
    bool computeQueueViaMes { false };
    bool initComputeRing();
    
    // Phase 2.3: First Compute Kernel submission
    bool testVectorAdd();

    // Phase 2.4: CP/MEC compute queue (MQD + HQD programming)
    bool initComputeQueue(uint64_t ringGpuAddr, uint32_t ringSizeBytes,
                          uint64_t mqdGpuAddr, uint64_t eopGpuAddr,
                          uint64_t rptrGpuAddr, uint32_t doorbellIndex);
    bool initGfxRing(uint64_t ringGpuAddr, uint32_t ringSizeBytes,
                     uint64_t rptrGpuAddr, uint64_t wptrGpuAddr,
                     uint32_t doorbellIndex);
    bool enableCpGfx();

    // Phase 2.5: MES (mandatory scheduler on GFX12)
    bool mesInit();
    bool mesSubmitAndWait(uint32_t pipe, const uint32_t* pkt, uint32_t dwords,
                          uint64_t statusGpuAddr);
    bool mesMapLegacyQueue(uint32_t pipe, uint32_t queue,
                           uint32_t doorbellOffset, uint64_t mqdAddr,
                           uint64_t wptrAddr, uint64_t statusAddr);
    uint64_t mesRingGpuAddr[2] { 0, 0 };
    uint32_t mesRingDwords[2] { 0, 0 };
    uint32_t mesRingWrite[2] { 0, 0 };
    uint64_t mesMqdGpuAddr[2] { 0, 0 };
    uint64_t mesEopGpuAddr[2] { 0, 0 };
    uint64_t mesRptrGpuAddr[2] { 0, 0 };
    uint64_t mesWptrGpuAddr[2] { 0, 0 };
    uint32_t mesDoorbellIndex[2] { 0, 0 };
    uint64_t mesStatusGpuAddr { 0 };
    // MES ring/MQD/EOP/status live in system memory mapped through the GART
    // aperture, like every mes_v12_0_mqd_sw_init/amdgpu_ring_init allocation.
    // CPU access goes through these descriptors, never through the BAR0 window.
    IOBufferMemoryDescriptor* mesRingMd[2] { nullptr, nullptr };
    IOBufferMemoryDescriptor* mesMqdMd[2] { nullptr, nullptr };
    IOBufferMemoryDescriptor* mesEopMd[2] { nullptr, nullptr };
    IOBufferMemoryDescriptor* mesWpMd[2] { nullptr, nullptr };
    IOBufferMemoryDescriptor* mesStatusMd { nullptr };
    // MES buffers can be allocated in GART system memory (upstream) or in VRAM
    // (A/B switch rdna4-mes-gtt=0) to prove whether the GART page table is used.
    bool mesUseGtt { true };
    volatile uint8_t* mesBufferCpu(IOBufferMemoryDescriptor* md, uint64_t gpuAddr);
    // SET_HW_RESOURCES carries a scheduler context and a status fence that MES
    // writes into; both must be GART-mapped system memory.
    uint64_t mesSchCtxGpuAddr { 0 };
    uint64_t mesStatusFenceGpuAddr { 0 };
    IOBufferMemoryDescriptor* mesSchCtxMd { nullptr };
    IOBufferMemoryDescriptor* mesStatusFenceMd { nullptr };
    bool mesSetHwResources(uint32_t pipe);
    // sched_version >= 0x4b also requires SET_HW_RESOURCES_1, which hands MES
    // the cleaner-shader fence page and the KIQ unmap timeout.
    bool mesSetHwResources1(uint32_t pipe);
    uint64_t mesResource1GpuAddr { 0 };
    // MES event/interrupt history log (enable_mes_event_int_logging): the
    // firmware records every API it processes here, which is the only way to
    // see where ADD_QUEUE stalls without a serial console.
    uint64_t mesEventLogGpuAddr { 0 };
    static constexpr uint32_t kMesEventLogBytes = 0xC000 + 0x40000;
    // The queue write pointer MES polls must be a writeback slot, i.e. system
    // memory reached through the GART aperture (KFD uses wb slots for this).
    IOBufferMemoryDescriptor* computeWptrMd { nullptr };
    // MQD of the MES-mapped compute queue: MES reads it through the GART
    // aperture, so it must be a GART-mapped system buffer, not VRAM.
    IOBufferMemoryDescriptor* computeMqdMd { nullptr };
    // Distinct from the rptr report page: sharing one page makes the CP poll
    // and write back through the same address.
    uint64_t computeWptrPollGpuAddr { 0 };
    uint64_t mesUcStart { 0 };
    uint64_t computeWptrGpuAddr { 0 };
    bool activateComputeQueue();
    bool deactivateComputeQueue();
    bool enableMecRs64Pipes();
    bool initShaderMemoryConfig();
    bool initComputeVmid();
    bool initClearStateBuffer();
    uint64_t clearStateGpuAddr { 0 };
    uint32_t readGcRegister(uint32_t regDwordOffset, uint8_t segment = 0);
    bool writeGcRegister(uint32_t regDwordOffset, uint32_t value, uint8_t segment = 0);
    void grbmSelect(uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid);
    void dumpComputeQueueState();
    void programMecStartAddress();

    //==========================================================================
    // 初始化方法
    //==========================================================================

    // PCI 初始化
    bool initializePCI();
    bool enableBusMaster();

    // BAR 映射
    bool mapBARs();
    void unmapBARs();
    bool mapRegisters();    // BAR5
    void unmapRegisters();
    bool mapVRAM();         // BAR0
    void unmapVRAM();
    bool mapDoorbells();    // BAR2
    void unmapDoorbells();

    // 设备识别
    bool identifyDevice();
    const char* getDeviceNameForID(uint16_t devID);

    // 基础探测
    bool probeGPU();
    bool readBasicInfo();

    // 资源清理（stop/free/start 失败共用，必须幂等）
    void releaseResources();
    bool isPspAlive();

    //==========================================================================
    // 寄存器访问（内部）
    //==========================================================================

    uint32_t regRead32(uint32_t offset);
    void regWrite32(uint32_t offset, uint32_t value);
    bool checkRegisterAccess();

    //==========================================================================
    // Boot-args 解析
    //==========================================================================

    // parseBootArgs() 已移除 - boot-arg 检查现在在 probe() 中进行

    //==========================================================================
    // 成员变量
    //==========================================================================

    // PCI 设备
    IOPCIDevice*        pciDevice { nullptr };

    // 设备信息
    uint16_t            vendorID { 0 };
    uint16_t            deviceID { 0 };
    uint16_t            revisionID { 0 };
    uint16_t            subsysVendorID { 0 };
    uint16_t            subsysDeviceID { 0 };
    const char*         deviceName { nullptr };

    // BAR 映射
    IOMemoryMap*        rmmioMap { nullptr };       // BAR5 - 寄存器 MMIO
    volatile uint32_t*  rmmio { nullptr };
    size_t              rmmioSize { 0 };

    IOMemoryMap*        vramMap { nullptr };        // BAR0 - VRAM aperture
    volatile uint8_t*   vram { nullptr };
    size_t              vramSize { 0 };

    IOMemoryMap*        doorbellMap { nullptr };    // BAR2 - Doorbells
    volatile uint32_t*  doorbell { nullptr };
    size_t              doorbellSize { 0 };

    // 控制标志 (enabled已移除 - 由probe中的boot-arg控制)
    uint32_t            debugLevel { 0 };           // 调试级别 (0-5)

    // GFX 固件是否已由本次驱动实例成功提交（不能复用 PSP,Stage 判断）
    bool                gfxFirmwareLoaded { false };
    
    // Phase 2.3: Public API for buffer allocation and command submission
public:
    // Allocate buffer in VRAM (returns GPU address)
    uint64_t allocateVRAMBuffer(size_t size);
    
    // Free VRAM buffer
    void freeVRAMBuffer(uint64_t gpuAddress);
    
    // Submit PM4 commands to compute ring
    bool submitComputeCommands(const uint32_t* pm4Data, size_t dwordCount);
    
    // Wait for compute completion
    bool waitForComputeFence(uint64_t fenceValue, uint64_t timeoutMS);
    
private:

    // 统计信息
    uint64_t            startTime { 0 };            // 驱动启动时间
    uint32_t            regReadCount { 0 };         // 寄存器读取次数
    uint32_t            regWriteCount { 0 };        // 寄存器写入次数
};


#endif /* RDNA4Driver_hpp */
