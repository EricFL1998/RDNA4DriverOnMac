#ifndef NBIODoorbell_hpp
#define NBIODoorbell_hpp

#include <stdint.h>

class RDNA4Driver;

// NBIO v6.3.1 寄存器定义
namespace NBIORegs {
    // Doorbell aperture enable
    constexpr uint32_t RCC_DOORBELL_APER_EN = 0x00c0;
    constexpr uint32_t RCC_DOORBELL_APER_EN_BASE_IDX = 2;
    
    // Self-ring doorbell aperture
    constexpr uint32_t DOORBELL_SELFRING_GPA_APER_BASE_HIGH = 0x00f3;
    constexpr uint32_t DOORBELL_SELFRING_GPA_APER_BASE_LOW = 0x00f4;
    constexpr uint32_t DOORBELL_SELFRING_GPA_APER_CNTL = 0x00f5;
    constexpr uint32_t SELFRING_BASE_IDX = 2;
    
    // HDP flush remap
    constexpr uint32_t REMAP_HDP_MEM_FLUSH_CNTL = 0x012d;
    constexpr uint32_t REMAP_HDP_REG_FLUSH_CNTL = 0x012e;
    constexpr uint32_t HDP_BASE_IDX = 2;
    
    // GDC S2A doorbell entries
    constexpr uint32_t S2A_DOORBELL_ENTRY_0_CTRL = 0x01cb;  // GC entry 0
    constexpr uint32_t S2A_DOORBELL_ENTRY_3_CTRL = 0x01ce;  // GC entry 3
    constexpr uint32_t S2A_BASE_IDX = 2;
    
    // Strap register
    constexpr uint32_t RCC_DEV0_EPF2_STRAP2 = 0xd102;
    constexpr uint32_t STRAP2_BASE_IDX = 5;
}

class NBIODoorbell {
public:
    // 完整的 doorbell 路径初始化
    static bool initDoorbellPath(RDNA4Driver& driver, uint64_t bar2Physical);
    
private:
    // 子步骤
    static bool initRegisters(RDNA4Driver& driver);
    static bool remapHDPRegisters(RDNA4Driver& driver);
    static bool enableDoorbellAperture(RDNA4Driver& driver, bool enable);
    static bool enableSelfringAperture(RDNA4Driver& driver, uint64_t bar2Physical);
    static bool initGCDoorbell(RDNA4Driver& driver);
    
    // 辅助函数
    static uint32_t resolveNBIORegister(RDNA4Driver& driver, uint32_t offset, uint32_t baseIdx);
    static bool writeNBIORegister(RDNA4Driver& driver, uint32_t offset, uint32_t baseIdx, 
                                  uint32_t value, uint32_t checkMask, const char* name);
    static uint32_t readNBIORegister(RDNA4Driver& driver, uint32_t offset, uint32_t baseIdx);
};

#endif /* NBIODoorbell_hpp */
