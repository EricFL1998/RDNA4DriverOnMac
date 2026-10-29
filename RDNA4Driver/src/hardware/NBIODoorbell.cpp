#include "NBIODoorbell.hpp"
#include "../core/RDNA4Driver.hpp"
#include "IPDiscovery.hpp"

#define NBIO_LOG(fmt, ...) IOLog("RDNA4: NBIO: " fmt "\n", ##__VA_ARGS__)

// 辅助宏：位域操作
#define REG_SET_FIELD(orig, field_shift, field_mask, val) \
    (((orig) & ~(field_mask)) | (((val) << (field_shift)) & (field_mask)))

#define REG_GET_FIELD(value, field_shift, field_mask) \
    (((value) & (field_mask)) >> (field_shift))

// 完整的 doorbell 路径初始化
bool NBIODoorbell::initDoorbellPath(RDNA4Driver& driver, uint64_t bar2Physical) {
    NBIO_LOG("doorbell_path_init: bar2Phys=0x%llx", 
             static_cast<unsigned long long>(bar2Physical));
    
    if (bar2Physical == 0) {
        NBIO_LOG("doorbell_path_init: BAR2 not mapped, skipping");
        return false;
    }
    
    // Step 1: 验证 NBIO 寄存器可访问性（先读不写）
    uint32_t testRead = readNBIORegister(driver, NBIORegs::RCC_DOORBELL_APER_EN, 
                                         NBIORegs::RCC_DOORBELL_APER_EN_BASE_IDX);
    if (testRead == 0xFFFFFFFF) {
        NBIO_LOG("doorbell_path_init: CRITICAL - cannot read NBIO registers, ABORTING");
        return false;
    }
    NBIO_LOG("doorbell_path_init: NBIO registers accessible (test read = 0x%08x)", testRead);
    
    // Step 2: init_registers (非关键，失败继续)
    if (!initRegisters(driver)) {
        NBIO_LOG("doorbell_path_init: init_registers failed (non-fatal)");
    }
    
    // Step 3: remap_hdp_registers (非关键，失败继续)
    if (!remapHDPRegisters(driver)) {
        NBIO_LOG("doorbell_path_init: remap_hdp_registers failed (non-fatal)");
    }
    
    // Step 4: enable_doorbell_aperture (关键，但添加安全检查)
    uint32_t aperBefore = readNBIORegister(driver, NBIORegs::RCC_DOORBELL_APER_EN,
                                           NBIORegs::RCC_DOORBELL_APER_EN_BASE_IDX);
    NBIO_LOG("doorbell_path_init: doorbell aperture current state = 0x%08x", aperBefore);
    
    // 如果已经启用，跳过写入
    if (aperBefore & 0x1) {
        NBIO_LOG("doorbell_path_init: doorbell aperture already enabled, skipping write");
    } else {
        if (!enableDoorbellAperture(driver, true)) {
            NBIO_LOG("doorbell_path_init: enable_doorbell_aperture FAILED - ABORTING");
            return false;
        }
    }
    
    // Step 5: enable_selfring_aperture (关键)
    if (!enableSelfringAperture(driver, bar2Physical)) {
        NBIO_LOG("doorbell_path_init: enable_selfring_aperture FAILED - ABORTING");
        return false;
    }
    
    // Step 6: gc_doorbell_init (关键)
    if (!initGCDoorbell(driver)) {
        NBIO_LOG("doorbell_path_init: gc_doorbell_init FAILED - ABORTING");
        return false;
    }
    
    NBIO_LOG("doorbell_path_init: complete");
    return true;
}

// 解析 NBIO 寄存器地址
uint32_t NBIODoorbell::resolveNBIORegister(RDNA4Driver& driver, uint32_t offset, uint32_t baseIdx) {
    uint32_t byteOffset = 0;
    if (!driver.discovery().regByteOffset(IPDiscovery::HwNbif, 0, static_cast<uint8_t>(baseIdx), offset, byteOffset)) {
        return 0xFFFFFFFF;
    }
    return byteOffset;
}

// 读取 NBIO 寄存器
uint32_t NBIODoorbell::readNBIORegister(RDNA4Driver& driver, uint32_t offset, uint32_t baseIdx) {
    uint32_t byteOffset = resolveNBIORegister(driver, offset, baseIdx);
    if (byteOffset == 0xFFFFFFFF) {
        return 0xFFFFFFFF;
    }
    return driver.readRegister(byteOffset);
}

// 写入 NBIO 寄存器（带验证）
bool NBIODoorbell::writeNBIORegister(RDNA4Driver& driver, uint32_t offset, uint32_t baseIdx,
                                     uint32_t value, uint32_t checkMask, const char* name) {
    uint32_t byteOffset = resolveNBIORegister(driver, offset, baseIdx);
    if (byteOffset == 0xFFFFFFFF) {
        NBIO_LOG("%s: CRITICAL - register offset 0x%x base %u unresolved", name, offset, baseIdx);
        return false;
    }
    
    // 读取当前值
    uint32_t before = driver.readRegister(byteOffset);
    if (before == 0xFFFFFFFF) {
        NBIO_LOG("%s: WARNING - pre-read returned 0xFFFFFFFF (may be unresponsive)", name);
    }
    
    NBIO_LOG("%s: offset=0x%x base=%u byteOff=0x%x before=0x%08x writing=0x%08x",
             name, offset, baseIdx, byteOffset, before, value);
    
    // 写入新值
    driver.writeRegister(byteOffset, value);
    
    // 短暂延迟让硬件响应
    IOSleep(1);
    
    // 读回验证
    uint32_t after = driver.readRegister(byteOffset);
    
    bool ok = ((after & checkMask) == (value & checkMask));
    if (!ok) {
        NBIO_LOG("%s: VERIFICATION FAILED - wrote 0x%08x, read back 0x%08x, mask 0x%08x",
                 name, value, after, checkMask);
        NBIO_LOG("%s: expected masked=0x%08x got masked=0x%08x",
                 name, value & checkMask, after & checkMask);
        return false;
    }
    
    NBIO_LOG("%s: SUCCESS - verified 0x%08x", name, after);
    return ok;
}

// Step 1: init_registers - 清除 strap register
bool NBIODoorbell::initRegisters(RDNA4Driver& /* driver */) {
    // 清除 RCC_DEV0_EPF2_STRAP2 bit 7
    // 这个寄存器在 BASE_IDX 5（SMN 访问，需要特殊处理）
    // 简化实现：跳过这个步骤，它不是关键路径
    NBIO_LOG("init_registers: skipping STRAP2 clear (SMN access required)");
    return true;
}

// Step 2: remap_hdp_registers - HDP flush 重映射
bool NBIODoorbell::remapHDPRegisters(RDNA4Driver& driver) {
    using namespace NBIORegs;
    
    NBIO_LOG("remap_hdp_registers: starting HDP flush remap");
    
    // 检查寄存器是否可访问
    uint32_t testMem = readNBIORegister(driver, REMAP_HDP_MEM_FLUSH_CNTL, HDP_BASE_IDX);
    uint32_t testReg = readNBIORegister(driver, REMAP_HDP_REG_FLUSH_CNTL, HDP_BASE_IDX);
    
    if (testMem == 0xFFFFFFFF || testReg == 0xFFFFFFFF) {
        NBIO_LOG("remap_hdp_registers: HDP registers not accessible, skipping");
        return false;
    }
    
    NBIO_LOG("remap_hdp_registers: current MEM=0x%08x REG=0x%08x", testMem, testReg);
    
    // 设置 HDP memory flush 和 register flush 控制
    // 值来自参考实现
    const uint32_t memFlushValue = 0x00010000;  // REMAP bit
    const uint32_t regFlushValue = 0x00010000;
    
    if (!writeNBIORegister(driver, REMAP_HDP_MEM_FLUSH_CNTL, HDP_BASE_IDX,
                           memFlushValue, 0x0001FFFF, "REMAP_HDP_MEM_FLUSH_CNTL")) {
        NBIO_LOG("remap_hdp_registers: MEM_FLUSH write failed");
        return false;
    }
    
    if (!writeNBIORegister(driver, REMAP_HDP_REG_FLUSH_CNTL, HDP_BASE_IDX,
                           regFlushValue, 0x0001FFFF, "REMAP_HDP_REG_FLUSH_CNTL")) {
        NBIO_LOG("remap_hdp_registers: REG_FLUSH write failed");
        return false;
    }
    
    NBIO_LOG("remap_hdp_registers: complete");
    return true;
}

// Step 3: enable_doorbell_aperture - 启用 doorbell aperture
bool NBIODoorbell::enableDoorbellAperture(RDNA4Driver& driver, bool enable) {
    using namespace NBIORegs;
    
    uint32_t before = readNBIORegister(driver, RCC_DOORBELL_APER_EN, RCC_DOORBELL_APER_EN_BASE_IDX);
    if (before == 0xFFFFFFFF) {
        NBIO_LOG("enable_doorbell_aperture: register read failed");
        return false;
    }
    
    // BIF_DOORBELL_APER_EN 是 bit 0
    uint32_t value = enable ? (before | 0x1u) : (before & ~0x1u);
    
    if (!writeNBIORegister(driver, RCC_DOORBELL_APER_EN, RCC_DOORBELL_APER_EN_BASE_IDX,
                           value, 0x1, "RCC_DOORBELL_APER_EN")) {
        return false;
    }
    
    NBIO_LOG("enable_doorbell_aperture: %s", enable ? "enabled" : "disabled");
    return true;
}

// Step 4: enable_selfring_aperture - 配置 self-ring doorbell
bool NBIODoorbell::enableSelfringAperture(RDNA4Driver& driver, uint64_t bar2Physical) {
    using namespace NBIORegs;
    
    // 配置 doorbell self-ring GPA aperture
    // 地址范围：bar2Physical, size = BAR2 size
    
    uint32_t addrLow = static_cast<uint32_t>(bar2Physical & 0xFFFFFFFF);
    uint32_t addrHigh = static_cast<uint32_t>((bar2Physical >> 32) & 0xFFFF);
    
    // BASE_LOW: bits [31:18] of physical address
    uint32_t baseLow = (addrLow >> 18) & 0x3FFF;
    
    if (!writeNBIORegister(driver, DOORBELL_SELFRING_GPA_APER_BASE_LOW, SELFRING_BASE_IDX,
                           baseLow, 0x3FFF, "DOORBELL_SELFRING_GPA_APER_BASE_LOW")) {
        return false;
    }
    
    if (!writeNBIORegister(driver, DOORBELL_SELFRING_GPA_APER_BASE_HIGH, SELFRING_BASE_IDX,
                           addrHigh, 0x0000FFFF, "DOORBELL_SELFRING_GPA_APER_BASE_HIGH")) {
        return false;
    }
    
    // CNTL: enable aperture
    uint32_t cntl = 0x1;  // DOORBELL_SELFRING_GPA_APER_EN = 1
    if (!writeNBIORegister(driver, DOORBELL_SELFRING_GPA_APER_CNTL, SELFRING_BASE_IDX,
                           cntl, 0x1, "DOORBELL_SELFRING_GPA_APER_CNTL")) {
        return false;
    }
    
    NBIO_LOG("enable_selfring_aperture: configured at 0x%llx", 
             static_cast<unsigned long long>(bar2Physical));
    return true;
}

// Step 5: init_gc_doorbell - 初始化 GC doorbell entries
bool NBIODoorbell::initGCDoorbell(RDNA4Driver& driver) {
    using namespace NBIORegs;

    // nbif_v6_3_1_gc_doorbell_init: entries 0 and 3 route the doorbell aperture
    // into the GC.  AWADDR_31_28 = 3 selects the address region the fabric
    // forwards; without it the NBIF drops every doorbell write and the CP never
    // sees a kick (the HQD shows ACTIVE with PQ_RPTR stuck at 0).
    if (!writeNBIORegister(driver, S2A_DOORBELL_ENTRY_0_CTRL, S2A_BASE_IDX,
                           0x30000007u, 0x003FFFFF, "S2A_DOORBELL_ENTRY_0_CTRL")) {
        return false;
    }
    if (!writeNBIORegister(driver, S2A_DOORBELL_ENTRY_3_CTRL, S2A_BASE_IDX,
                           0x3000000du, 0x003FFFFF, "S2A_DOORBELL_ENTRY_3_CTRL")) {
        return false;
    }

    NBIO_LOG("init_gc_doorbell: GC entries 0 and 3 configured");
    return true;
}
