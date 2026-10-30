#include "GMCContext.hpp"
#include "GARTUtils.hpp"
#include "../core/RDNA4Driver.hpp"
#include "VramLayout.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>

// GMC (Graphics Memory Controller) 初始化
// 目标：启用 MMHUB GART，让 GPU 能通过 GART aperture 访问系统内存。
//
// 页表必须位于 VRAM，并且 CONTEXT0_PAGE_TABLE_BASE_ADDR 接收的是
// GPU 可寻址地址（VRAM 相对偏移 | VALID），不是 CPU 物理地址。
// 参见 Linux amdgpu: amdgpu_gart_table_vram_alloc / amdgpu_gmc_pd_addr,
// 以及 Cobalt64 gmc_v12_0.cpp 的 hub_init_gart_aperture_regs。

namespace {

constexpr uint32_t GPU_PAGE_SIZE = 4096;
constexpr uint32_t GPU_PAGE_SHIFT = 12;

// GART aperture 与页表大小必须匹配：512MB aperture / 4KB pages = 131072 PTE。
constexpr uint64_t GART_SIZE = VramLayout::kGartSize;
constexpr uint64_t GART_PAGE_TABLE_SIZE = (GART_SIZE / GPU_PAGE_SIZE) * sizeof(uint64_t);
constexpr uint64_t GART_ALIGNMENT = 4ULL * 1024 * 1024 * 1024;  // 4GB

constexpr uint32_t mmMC_VM_FB_LOCATION_BASE = 0x0554;
constexpr uint32_t mmMC_VM_FB_LOCATION_TOP  = 0x0555;

constexpr uint32_t mmMC_VM_AGP_TOP  = 0x0556;
constexpr uint32_t mmMC_VM_AGP_BOT  = 0x0557;
constexpr uint32_t mmMC_VM_AGP_BASE = 0x0558;
constexpr uint32_t mmMC_VM_SYSTEM_APERTURE_LOW_ADDR  = 0x0559;
constexpr uint32_t mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR = 0x055a;
constexpr uint32_t mmMC_VM_CONTEXT0_CNTL = 0x0564;

constexpr uint32_t mmMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32  = 0x05cf;
constexpr uint32_t mmMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32  = 0x05d0;
constexpr uint32_t mmMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32 = 0x05ef;
constexpr uint32_t mmMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32 = 0x05f0;
constexpr uint32_t mmMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32   = 0x060f;
constexpr uint32_t mmMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32   = 0x0610;
constexpr uint32_t mmMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32 = 0x04f4;
constexpr uint32_t mmMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32 = 0x04f5;

constexpr uint32_t mmMMVM_INVALIDATE_ENG17_REQ = 0x0598;
constexpr uint32_t mmMMVM_INVALIDATE_ENG17_ACK = 0x05aa;

constexpr uint32_t gcGCVM_CONTEXT0_CNTL = 0x1624;
constexpr uint32_t gcGCVM_SYSTEM_APERTURE_LOW_ADDR  = 0x1619;
constexpr uint32_t gcGCVM_SYSTEM_APERTURE_HIGH_ADDR = 0x161a;
constexpr uint32_t gcGCVM_AGP_TOP  = 0x1616;
constexpr uint32_t gcGCVM_AGP_BOT  = 0x1617;
constexpr uint32_t gcGCVM_AGP_BASE = 0x1618;
constexpr uint32_t gcGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32  = 0x168f;
constexpr uint32_t gcGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32  = 0x1690;
constexpr uint32_t gcGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32 = 0x16af;
constexpr uint32_t gcGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32 = 0x16b0;
constexpr uint32_t gcGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32   = 0x16cf;
constexpr uint32_t gcGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32   = 0x16d0;
constexpr uint32_t gcGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32 = 0x15d4;
constexpr uint32_t gcGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32 = 0x15d5;

constexpr uint32_t gcGCVM_INVALIDATE_ENG17_REQ = 0x1658;
constexpr uint32_t gcGCVM_INVALIDATE_ENG17_ACK = 0x166a;

// MMHUB v4.1.0 TLB / L2 cache control (Linux mmhub_4_1_0)
constexpr uint32_t mmMMMC_VM_MX_L1_TLB_CNTL = 0x055b;
constexpr uint32_t mmMMVM_L2_CNTL  = 0x04e4;
constexpr uint32_t mmMMVM_L2_CNTL2 = 0x04e5;
constexpr uint32_t mmMMVM_L2_CNTL3 = 0x04e6;
constexpr uint32_t mmMMVM_L2_CNTL4 = 0x04fd;
constexpr uint32_t mmMMVM_L2_CNTL5 = 0x0503;

// GFXHUB v12_0 TLB / L2 cache control (Linux gfxhub_v12_0)
constexpr uint32_t gcGCMC_VM_MX_L1_TLB_CNTL = 0x161b;
constexpr uint32_t gcGCVM_L2_CNTL  = 0x15c4;
constexpr uint32_t gcGCVM_L2_CNTL2 = 0x15c5;
constexpr uint32_t gcGCVM_L2_CNTL3 = 0x15c6;
constexpr uint32_t gcGCVM_L2_CNTL4 = 0x15dd;
constexpr uint32_t gcGCVM_L2_CNTL5 = 0x15e3;

// GFX12 PTE 位定义（Linux amdgpu_vm.h / Cobalt64 PTEFlags）
constexpr uint64_t PTE_VALID      = 1ULL << 0;
constexpr uint64_t PTE_SYSTEM     = 1ULL << 1;
constexpr uint64_t PTE_SNOOPED    = 1ULL << 2;
constexpr uint64_t PTE_EXECUTABLE = 1ULL << 4;
constexpr uint64_t PTE_READABLE   = 1ULL << 5;
constexpr uint64_t PTE_WRITEABLE  = 1ULL << 6;
constexpr uint64_t PTE_MTYPE_UC   = 2ULL << 54;
constexpr uint64_t PTE_IS_PTE     = 1ULL << 63;

// Cobalt64 PTEFlags::SYSMEM_RW：GART 表项指向系统内存时使用。
constexpr uint64_t GART_PTE_FLAGS = PTE_VALID | PTE_SYSTEM | PTE_SNOOPED |
                                    PTE_EXECUTABLE | PTE_READABLE |
                                    PTE_WRITEABLE | PTE_MTYPE_UC | PTE_IS_PTE;

constexpr uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// GPU 直接 DMA 的系统内存必须 online 且物理连续：GART 表项保存的是物理
// 地址，硬件不会替我们做页交叉。IOBufferMemoryDescriptor::withCapacity 返回
// 的可分页内存两条都不保证，因此统一走 inTaskWithPhysicalMask。
IOBufferMemoryDescriptor* gmcAllocContiguous(uint64_t bytes) {
    if (bytes == 0) return nullptr;
    bytes = alignUp(bytes, GPU_PAGE_SIZE);
    // 48 位物理地址上限，并按页对齐掩码保证起始地址页对齐。
    const mach_vm_address_t mask =
        (0x0000FFFFFFFFFFFFULL) & ~static_cast<mach_vm_address_t>(GPU_PAGE_SIZE - 1);
    IOBufferMemoryDescriptor* md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
        bytes, mask);
    if (!md) return nullptr;
    if (md->prepare() != kIOReturnSuccess) {
        md->release();
        return nullptr;
    }
    return md;
}

uint64_t gmcPhysicalAddress(IOBufferMemoryDescriptor* md) {
    if (!md) return 0;
    IOByteCount segment = 0;
    const addr64_t phys = md->getPhysicalSegment(0, &segment, kIOMemoryMapperNone);
    if (!phys || segment < md->getLength()) {
        // 分段说明分配器没能给出连续物理内存，调用者必须失败而不是把
        // 半个 buffer 的地址写进页表。
        return 0;
    }
    return static_cast<uint64_t>(phys);
}

void gmcReleaseContiguous(IOBufferMemoryDescriptor* md, bool prepared) {
    if (!md) return;
    if (prepared) md->complete();
    md->release();
}

} // namespace

void gmcFini(GMCContext& gmc) {
    if (gmc.dummyPage) {
        gmcReleaseContiguous(gmc.dummyPage, gmc.dummyPagePrepared);
        gmc.dummyPage = nullptr;
    }
    gmc.dummyPagePrepared = false;
    gmc.dummyPagePhys = 0;
    gmc.gartTableVramOffset = 0;
    gmc.gartTableSize = 0;
    gmc.gartStart = 0;
    gmc.gartEnd = 0;
    gmc.gartSize = 0;
    gmc.vramStart = 0;
    gmc.vramEnd = 0;
    gmc.vramSize = 0;
    gmc.inited = false;
    gmc.gartEnabled = false;
}

void gmcHDPFlush(RDNA4Driver& driver) {
    const IPDiscovery& disc = driver.discovery();

    // HDP flush 通过写 HDP_MEM_FLUSH_CNTL 触发
    uint32_t hdpReg;
    if (disc.regByteOffset(IPDiscovery::HwOsssys, 0, 0, 0x0014, hdpReg)) {
        driver.writeRegister(hdpReg, 1);
        // 读回确认 flush 完成
        driver.readRegister(hdpReg);
    }
}

bool gmcReadVRAMInfo(RDNA4Driver& driver, GMCContext& gmc) {
    const IPDiscovery& disc = driver.discovery();

    uint32_t baseOffset = 0;
    if (!disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMC_VM_FB_LOCATION_BASE, baseOffset)) {
        LOG("GMC: MMHUB base not resolved");
        return false;
    }

    uint32_t fbBase = driver.readRegister(baseOffset);
    if (fbBase == 0xFFFFFFFF) {
        LOG("GMC: cannot read FB_LOCATION_BASE");
        return false;
    }

    uint32_t topOffset = 0;
    if (!disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMC_VM_FB_LOCATION_TOP, topOffset)) {
        return false;
    }

    uint32_t fbTop = driver.readRegister(topOffset);
    if (fbTop == 0xFFFFFFFF) {
        LOG("GMC: cannot read FB_LOCATION_TOP");
        return false;
    }

    // FB_LOCATION 寄存器的低 24 位是 16MB 对齐的地址
    gmc.vramStart = static_cast<uint64_t>(fbBase & 0x00FFFFFF) << 24;
    gmc.vramEnd = (static_cast<uint64_t>(fbTop & 0x00FFFFFF) << 24) | 0xFFFFFF;
    if (gmc.vramEnd <= gmc.vramStart) {
        LOG("GMC: invalid VRAM window [0x%llx, 0x%llx]",
            static_cast<unsigned long long>(gmc.vramStart),
            static_cast<unsigned long long>(gmc.vramEnd));
        return false;
    }
    gmc.vramSize = gmc.vramEnd - gmc.vramStart + 1;

    LOG("GMC: VRAM [0x%llx, 0x%llx) size %llu MB",
        static_cast<unsigned long long>(gmc.vramStart),
        static_cast<unsigned long long>(gmc.vramEnd + 1),
        static_cast<unsigned long long>(gmc.vramSize >> 20));

    return true;
}

bool gmcAllocateGART(RDNA4Driver& driver, GMCContext& gmc) {
    // Dummy page：所有 GART 表项和页错误默认地址都指向它。 它会被写进 GPU
    // 可访问的页表项，所以必须是线上（wired）、物理连续的页面：可分页内存
    // 一旦被换出，GPU 就会读写到别处。
    gmc.dummyPage = gmcAllocContiguous(GPU_PAGE_SIZE);
    if (!gmc.dummyPage) {
        LOG("GMC: failed to allocate dummy page");
        return false;
    }
    gmc.dummyPagePrepared = true;

    gmc.dummyPagePhys = gmcPhysicalAddress(gmc.dummyPage);
    if (!gmc.dummyPagePhys) {
        LOG("GMC: failed to get dummy page physical address");
        gmcFini(gmc);
        return false;
    }

    // 页表放在 VRAM（BAR0 可写），不能使用 CPU 物理地址。
    gmc.gartTableSize = static_cast<uint32_t>(GART_PAGE_TABLE_SIZE);
    gmc.gartTableVramOffset = VramLayout::kGartTable;

    volatile uint8_t* vram = nullptr;
    size_t vramSize = 0;
    driver.vramWindow(vram, vramSize);
    if (!vram || gmc.gartTableVramOffset > vramSize ||
        gmc.gartTableSize > vramSize - gmc.gartTableVramOffset) {
        LOG("GMC: VRAM window too small for GART page table "
            "(offset 0x%llx size %u, window %zu)",
            static_cast<unsigned long long>(gmc.gartTableVramOffset),
            gmc.gartTableSize, vramSize);
        gmcFini(gmc);
        return false;
    }

    // GFX12 PTEs carry a BYTE address, not a page frame number: the low 12
    // bits are the flag field (gmc_v12_0_set_pte_pde / Cobalt64's SYSMEM_RW).
    // Shifting the physical address right by 12 would point every mapping at a
    // bogus location.
    const uint64_t dummyPTE =
        (gmc.dummyPagePhys & ~0xFFFULL) | GART_PTE_FLAGS;
    volatile uint64_t* table = reinterpret_cast<volatile uint64_t*>(
        vram + gmc.gartTableVramOffset);
    const size_t entries = gmc.gartTableSize / sizeof(uint64_t);
    for (size_t i = 0; i < entries; i++) {
        table[i] = dummyPTE;
    }

    // 页表通过 BAR0 写入 VRAM，必须 HDP flush 后 GPU 才能看到。
    gmcHDPFlush(driver);

    gmc.gartStart = alignUp(gmc.vramEnd + 1, GART_ALIGNMENT);
    if (gmc.gartStart < gmc.vramEnd) {
        LOG("GMC: GART start overflow");
        gmcFini(gmc);
        return false;
    }
    gmc.gartSize = GART_SIZE;
    gmc.gartEnd = gmc.gartStart + gmc.gartSize - 1;
    if (gmc.gartEnd < gmc.gartStart) {
        LOG("GMC: GART end overflow");
        gmcFini(gmc);
        return false;
    }

    LOG("GMC: GART aperture [0x%llx, 0x%llx) size %llu MB",
        static_cast<unsigned long long>(gmc.gartStart),
        static_cast<unsigned long long>(gmc.gartEnd + 1),
        static_cast<unsigned long long>(gmc.gartSize >> 20));
    LOG("GMC: GART page table vram+0x%llx (%u bytes), dummy phys 0x%llx",
        static_cast<unsigned long long>(gmc.gartTableVramOffset),
        gmc.gartTableSize,
        static_cast<unsigned long long>(gmc.dummyPagePhys));

    return true;
}

bool gmcFlushTLB(RDNA4Driver& driver, IPDiscovery::HwId hub, uint32_t reqReg, uint32_t ackReg) {
    const IPDiscovery& disc = driver.discovery();

    uint32_t reqOffset = 0, ackOffset = 0;
    if (!disc.regByteOffset(hub, 0, 0, reqReg, reqOffset) ||
        !disc.regByteOffset(hub, 0, 0, ackReg, ackOffset)) {
        LOG("GMC: failed to resolve TLB flush registers for hub %u (req=0x%x, ack=0x%x)",
            static_cast<uint32_t>(hub), reqReg, ackReg);
        return false;
    }

    // 标准 VMID 0 Invalidate 请求字 (Linux amdgpu / Cobalt64)
    // PER_VMID (bit 0) | INVALIDATE_L2_PTES (bit 19) | L2_PDE0-2 (bits 20-22) | L1_PTES (bit 23)
    const uint32_t reqValue = 0x00F80001;
    driver.writeRegister(reqOffset, reqValue);

    // 等待 ACK（轮询直到读到 bit 0，最多等待 100ms）
    // 注意：硬件 ACK 寄存器为只读状态位，不可写入清除
    for (uint32_t i = 0; i < 2000; i++) {
        uint32_t ack = driver.readRegister(ackOffset);
        if (ack & 1) {
            return true;
        }
        IODelay(50);
    }

    LOG("GMC: TLB flush timeout on hub %u (reqOff=0x%x, ackOff=0x%x)",
        static_cast<uint32_t>(hub), reqOffset, ackOffset);
    return false;
}

namespace {

// L1 TLB control: ENABLE_L1_TLB=1, SYSTEM_ACCESS_MODE=3,
// ENABLE_ADVANCED_DRIVER_MODEL=1, SYSTEM_APERTURE_UNMAPPED_ACCESS=0,
// ECO_BITS=0, MTYPE=UC(2). Same fields as mmhub_v4_1_0_init_tlb_regs.
void gmcInitL1TLB(RDNA4Driver& driver, IPDiscovery::HwId hub, uint32_t reg) {
    uint32_t offset = 0;
    if (!driver.discovery().regByteOffset(hub, 0, 0, reg, offset)) {
        LOG("GMC: failed to resolve L1 TLB control (hub %u)",
            static_cast<uint32_t>(hub));
        return;
    }
    uint32_t value = driver.readRegister(offset);
    if (value == 0xFFFFFFFF)
        return;
    value &= ~0x1FF9u;
    value |= 0x1059u;
    driver.writeRegister(offset, value);
}

// L2 cache control. The CNTL3/4/5 values mirror the per-IP DEFAULT seeds
// used by Linux/Cobalt64, with the same fields cleared.
void gmcInitL2(RDNA4Driver& driver, IPDiscovery::HwId hub,
               uint32_t cntl, uint32_t cntl2, uint32_t cntl3,
               uint32_t cntl4, uint32_t cntl5, uint32_t cntl3Default) {
    const IPDiscovery& disc = driver.discovery();
    uint32_t offset = 0;

    if (disc.regByteOffset(hub, 0, 0, cntl, offset)) {
        uint32_t value = driver.readRegister(offset);
        if (value != 0xFFFFFFFF) {
            value &= ~0x3FC0903u;
            value |= 0x00080801u;
            driver.writeRegister(offset, value);
        }
    }
    if (disc.regByteOffset(hub, 0, 0, cntl2, offset)) {
        uint32_t value = driver.readRegister(offset);
        if (value != 0xFFFFFFFF)
            driver.writeRegister(offset, value | 0x3u);
    }
    if (disc.regByteOffset(hub, 0, 0, cntl3, offset)) {
        // BANK_SELECT=9, L2_CACHE_BIGK_FRAGMENT_SIZE=6
        driver.writeRegister(offset, (cntl3Default & ~0xF803Fu) | 0x30009u);
    }
    if (disc.regByteOffset(hub, 0, 0, cntl4, offset)) {
        // 0x000000C1 with VMC_TAP_PDE/PTE_REQUEST_PHYSICAL cleared.
        driver.writeRegister(offset, 0x00000001u);
    }
    if (disc.regByteOffset(hub, 0, 0, cntl5, offset)) {
        // 0x00003FE0 with L2_CACHE_SMALLK_FRAGMENT_SIZE cleared.
        driver.writeRegister(offset, 0x00003FC0u);
    }
}

} // namespace

bool gmcEnableMMHUBGART(RDNA4Driver& driver, GMCContext& gmc) {
    const IPDiscovery& disc = driver.discovery();

    if (!gmc.gartTableVramOffset || !gmc.gartTableSize) {
        LOG("GMC: GART page table is not allocated");
        return false;
    }

    // AGP aperture 在 Linux 上当 agp_start=0xFFFFFFFFFFFF / agp_end=0 时关闭。
    uint32_t agpBaseReg, agpBotReg, agpTopReg;
    if (disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMC_VM_AGP_BASE, agpBaseReg) &&
        disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMC_VM_AGP_BOT, agpBotReg) &&
        disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMC_VM_AGP_TOP, agpTopReg)) {
        driver.writeRegister(agpBaseReg, 0);
        driver.writeRegister(agpBotReg, 0xFFFFFFFFu);
        driver.writeRegister(agpTopReg, 0);
    } else {
        LOG("GMC: AGP registers not resolved (continuing)");
    }

    // System aperture：Linux 使用 min(fb_start, agp_start) / max(fb_end, agp_end)。
    // AGP 关闭时即 FB 范围；GART 由 CONTEXT0 的 START/END 单独覆盖。
    uint32_t apLowReg, apHighReg;
    if (!disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMC_VM_SYSTEM_APERTURE_LOW_ADDR, apLowReg) ||
        !disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR, apHighReg)) {
        LOG("GMC: failed to resolve aperture registers");
        return false;
    }
    driver.writeRegister(apLowReg, static_cast<uint32_t>(gmc.vramStart >> 18));
    driver.writeRegister(apHighReg, static_cast<uint32_t>(gmc.vramEnd >> 18));

    // CONTEXT0 页表基址 = VRAM 相对偏移 | VALID。
    // GFX12 的 gmc_v12_0_get_vm_pde 对 VRAM 常驻页目录写的是
    // vram_base_offset + addr - vram_start（即 VRAM 相对偏移），不是 MC 地址；
    // 写 MC 地址会让 walker 去取一个不存在的 VRAM 偏移，每次 GART 访问都
    // 报 WALKER_ERROR/MAPPING_ERROR。
    const uint64_t ptBase = gmc.gartTableVramOffset | PTE_VALID;
    uint32_t ptBaseLoReg, ptBaseHiReg;
    if (!disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32, ptBaseLoReg) ||
        !disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32, ptBaseHiReg)) {
        LOG("GMC: failed to resolve CONTEXT0 page table base registers");
        return false;
    }
    driver.writeRegister(ptBaseLoReg, static_cast<uint32_t>(ptBase));
    driver.writeRegister(ptBaseHiReg, static_cast<uint32_t>(ptBase >> 32));

    uint32_t ptStartLoReg, ptStartHiReg, ptEndLoReg, ptEndHiReg;
    if (!disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32, ptStartLoReg) ||
        !disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32, ptStartHiReg) ||
        !disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32, ptEndLoReg) ||
        !disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32, ptEndHiReg)) {
        LOG("GMC: failed to resolve CONTEXT0 range registers");
        return false;
    }
    driver.writeRegister(ptStartLoReg, static_cast<uint32_t>(gmc.gartStart >> 12));
    driver.writeRegister(ptStartHiReg, static_cast<uint32_t>(gmc.gartStart >> 44));
    driver.writeRegister(ptEndLoReg, static_cast<uint32_t>(gmc.gartEnd >> 12));
    driver.writeRegister(ptEndHiReg, static_cast<uint32_t>(gmc.gartEnd >> 44));

    // L1 TLB / L2 cache：与 mmhub_v4_1_0_gart_enable 的默认路径一致。
    gmcInitL1TLB(driver, IPDiscovery::HwMmhub, mmMMMC_VM_MX_L1_TLB_CNTL);
    gmcInitL2(driver, IPDiscovery::HwMmhub,
              mmMMVM_L2_CNTL, mmMMVM_L2_CNTL2, mmMMVM_L2_CNTL3,
              mmMMVM_L2_CNTL4, mmMMVM_L2_CNTL5, 0x80100007u);

    // CONTEXT0_CNTL：启用 context，页表深度 0，不重试页错误。
    uint32_t ctx0CntlReg;
    if (!disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMC_VM_CONTEXT0_CNTL, ctx0CntlReg)) {
        LOG("GMC: failed to resolve CONTEXT0_CNTL");
        return false;
    }

    uint32_t ctx0Cntl = driver.readRegister(ctx0CntlReg);
    ctx0Cntl |= (1u << 0);   // ENABLE_CONTEXT
    ctx0Cntl &= ~(3u << 1);  // PAGE_TABLE_DEPTH = 0
    ctx0Cntl &= ~(1u << 7);  // RETRY_PERMISSION_OR_INVALID_PAGE_FAULT = 0
    driver.writeRegister(ctx0CntlReg, ctx0Cntl);

    // 页错误默认地址指向 dummy page，避免 fault 落到 VRAM offset 0。
    uint32_t pfLoReg, pfHiReg;
    if (disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32, pfLoReg) &&
        disc.regByteOffset(IPDiscovery::HwMmhub, 0, 0, mmMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32, pfHiReg)) {
        const uint64_t dummy = gmc.dummyPagePhys >> 12;
        driver.writeRegister(pfLoReg, static_cast<uint32_t>(dummy));
        driver.writeRegister(pfHiReg, static_cast<uint32_t>(dummy >> 32));
    }

    LOG("GMC: MMHUB GART enabled (CONTEXT0=%#x, PT=0x%llx)",
        ctx0Cntl, static_cast<unsigned long long>(ptBase));
    gmc.gartEnabled = true;
    return true;
}

bool gmcEnableGFXHUBGART(RDNA4Driver& driver, GMCContext& gmc) {
    const IPDiscovery& disc = driver.discovery();

    if (!gmc.gartTableVramOffset || !gmc.gartTableSize) {
        LOG("GMC: GART page table is not allocated");
        return false;
    }

    // AGP aperture（与 MMHUB 一致：关闭）
    uint32_t gcAgpBaseReg, gcAgpBotReg, gcAgpTopReg;
    if (disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_AGP_BASE, gcAgpBaseReg) &&
        disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_AGP_BOT, gcAgpBotReg) &&
        disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_AGP_TOP, gcAgpTopReg)) {
        driver.writeRegister(gcAgpBaseReg, 0);
        driver.writeRegister(gcAgpBotReg, 0xFFFFFFFFu);
        driver.writeRegister(gcAgpTopReg, 0);
    } else {
        LOG("GMC: GFXHUB AGP registers not resolved (continuing)");
    }

    // System aperture
    uint32_t gcApLowReg, gcApHighReg;
    if (!disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_SYSTEM_APERTURE_LOW_ADDR, gcApLowReg) ||
        !disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_SYSTEM_APERTURE_HIGH_ADDR, gcApHighReg)) {
        LOG("GMC: failed to resolve GFXHUB aperture registers");
        return false;
    }
    driver.writeRegister(gcApLowReg, static_cast<uint32_t>(gmc.vramStart >> 18));
    driver.writeRegister(gcApHighReg, static_cast<uint32_t>(gmc.vramEnd >> 18));

    // CONTEXT0 页表基址 / 范围（gfxhub_v12_0_init_gart_aperture_regs）
    // VRAM 常驻页表按 VRAM 相对偏移编码，与 MMHUB 一致。
    const uint64_t ptBase = gmc.gartTableVramOffset | PTE_VALID;
    uint32_t gcPtBaseLoReg, gcPtBaseHiReg;
    if (!disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32, gcPtBaseLoReg) ||
        !disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32, gcPtBaseHiReg)) {
        LOG("GMC: failed to resolve GFXHUB CONTEXT0 page table base registers");
        return false;
    }
    driver.writeRegister(gcPtBaseLoReg, static_cast<uint32_t>(ptBase));
    driver.writeRegister(gcPtBaseHiReg, static_cast<uint32_t>(ptBase >> 32));

    uint32_t gcPtStartLoReg, gcPtStartHiReg, gcPtEndLoReg, gcPtEndHiReg;
    if (!disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32, gcPtStartLoReg) ||
        !disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32, gcPtStartHiReg) ||
        !disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32, gcPtEndLoReg) ||
        !disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32, gcPtEndHiReg)) {
        LOG("GMC: failed to resolve GFXHUB CONTEXT0 range registers");
        return false;
    }
    driver.writeRegister(gcPtStartLoReg, static_cast<uint32_t>(gmc.gartStart >> 12));
    driver.writeRegister(gcPtStartHiReg, static_cast<uint32_t>(gmc.gartStart >> 44));
    driver.writeRegister(gcPtEndLoReg, static_cast<uint32_t>(gmc.gartEnd >> 12));
    driver.writeRegister(gcPtEndHiReg, static_cast<uint32_t>(gmc.gartEnd >> 44));

    // L1 TLB / L2 cache：与 gfxhub_v12_0_gart_enable 的默认路径一致。
    gmcInitL1TLB(driver, IPDiscovery::HwGc, gcGCMC_VM_MX_L1_TLB_CNTL);
    gmcInitL2(driver, IPDiscovery::HwGc,
              gcGCVM_L2_CNTL, gcGCVM_L2_CNTL2, gcGCVM_L2_CNTL3,
              gcGCVM_L2_CNTL4, gcGCVM_L2_CNTL5, 0x80120007u);

    // CONTEXT0_CNTL
    uint32_t gcCtx0CntlReg;
    if (!disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_CONTEXT0_CNTL, gcCtx0CntlReg)) {
        LOG("GMC: failed to resolve GFXHUB CONTEXT0_CNTL");
        return false;
    }

    uint32_t gcCtx0Cntl = driver.readRegister(gcCtx0CntlReg);
    gcCtx0Cntl |= (1u << 0);   // ENABLE_CONTEXT
    gcCtx0Cntl &= ~(3u << 1);  // PAGE_TABLE_DEPTH = 0
    gcCtx0Cntl &= ~(1u << 7);  // RETRY_PERMISSION_OR_INVALID_PAGE_FAULT = 0
    driver.writeRegister(gcCtx0CntlReg, gcCtx0Cntl);

    // 页错误默认地址指向 dummy page
    uint32_t gcPfLoReg, gcPfHiReg;
    if (disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32, gcPfLoReg) &&
        disc.regByteOffset(IPDiscovery::HwGc, 0, 0, gcGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32, gcPfHiReg)) {
        const uint64_t dummy = gmc.dummyPagePhys >> 12;
        driver.writeRegister(gcPfLoReg, static_cast<uint32_t>(dummy));
        driver.writeRegister(gcPfHiReg, static_cast<uint32_t>(dummy >> 32));
    }

    // 刷新 GFXHUB TLB
    gmcFlushTLB(driver, IPDiscovery::HwGc,
                gcGCVM_INVALIDATE_ENG17_REQ,
                gcGCVM_INVALIDATE_ENG17_ACK);

    LOG("GMC: GFXHUB GART enabled (CONTEXT0=%#x, PT=0x%llx)",
        gcCtx0Cntl, static_cast<unsigned long long>(ptBase));

    // Readback so the page-table programming can be audited from ioreg without
    // a serial console: if these do not echo what we wrote, the walker is not
    // using our table and every GART access fails silently.
    driver.setProperty("GMC,VramStart", gmc.vramStart, 64);
    driver.setProperty("GMC,GartStart", gmc.gartStart, 64);
    driver.setProperty("GMC,GfxPtBase", ptBase, 64);
    driver.setProperty("GMC,GfxPtBaseRb",
                       (static_cast<uint64_t>(driver.readRegister(gcPtBaseHiReg)) << 32) |
                           driver.readRegister(gcPtBaseLoReg), 64);
    driver.setProperty("GMC,GfxPtStartRb",
                       (static_cast<uint64_t>(driver.readRegister(gcPtStartHiReg)) << 32) |
                           driver.readRegister(gcPtStartLoReg), 64);
    driver.setProperty("GMC,GfxPtEndRb",
                       (static_cast<uint64_t>(driver.readRegister(gcPtEndHiReg)) << 32) |
                           driver.readRegister(gcPtEndLoReg), 64);
    driver.setProperty("GMC,GfxCtx0CntlRb",
                       static_cast<uint64_t>(driver.readRegister(gcCtx0CntlReg)), 32);
    return true;
}

bool gmcInit(RDNA4Driver& driver, GMCContext& gmc) {
    if (!gmcReadVRAMInfo(driver, gmc)) {
        gmcFini(gmc);
        return false;
    }

    if (!gmcAllocateGART(driver, gmc)) {
        LOG("GMC: GART allocation failed");
        gmcFini(gmc);
        return false;
    }

    if (!gmcEnableMMHUBGART(driver, gmc)) {
        LOG("GMC: MMHUB GART enable failed");
        gmcFini(gmc);
        return false;
    }

    // 注意：GFXHUB GART 在此处严格禁止配置！
    // 依据 Linux amdgpu (gmc_v12_0.c) 与 Cobalt64 规范，
    // GFXHUB 属于 GC IP 域，必须在 PSP 校验固件且 SMU 完成 GC 通电后（GFXInit 阶段）配置。
    // 在此时强行写入 GFXHUB 或触发 GFXHUB TLB Flush 会导致未通电的核心挂起 PCIe 总线，
    // 并且会破坏 PSP 对 GFX 固件的验证环境。

    // HDP flush 确保 GART 页表写入对 GPU 可见
    gmcHDPFlush(driver);

    // 仅刷新 MMHUB 的 TLB（使用专用 GART 刷新引擎 Engine 17）
    bool mmhubTLB = gmcFlushTLB(driver, IPDiscovery::HwMmhub,
                                 mmMMVM_INVALIDATE_ENG17_REQ,
                                 mmMMVM_INVALIDATE_ENG17_ACK);
    if (!mmhubTLB) {
        LOG("GMC: MMHUB TLB flush timeout (continuing safely without blocking)");
    }

    gmc.inited = true;
    LOG("GMC: initialized with MMHUB GART enabled");
    return true;
}

uint64_t gmcAllocGartBuffer(RDNA4Driver& driver, GMCContext& gmc, size_t size,
                            IOBufferMemoryDescriptor** outMd) {
    if (!outMd)
        return 0;
    *outMd = nullptr;
    if (!gmc.inited || !gmc.gartEnabled || !gmc.gartTableSize || size == 0)
        return 0;

    const uint32_t pages =
        static_cast<uint32_t>((size + GPU_PAGE_SIZE - 1) / GPU_PAGE_SIZE);
    const uint32_t totalPages = gmc.gartTableSize / sizeof(uint64_t);
    if (pages == 0 || gmc.gartNextPage >= totalPages ||
        pages > totalPages - gmc.gartNextPage) {
        LOG("GMC: GART aperture exhausted (%u pages requested)", pages);
        return 0;
    }

    IOBufferMemoryDescriptor* md = gmcAllocContiguous(
        static_cast<uint64_t>(pages) * GPU_PAGE_SIZE);
    if (!md)
        return 0;

    const uint64_t phys = gmcPhysicalAddress(md);
    if (!phys) {
        gmcReleaseContiguous(md, true);
        return 0;
    }

    volatile uint8_t* vram = nullptr;
    size_t vramSize = 0;
    driver.vramWindow(vram, vramSize);
    if (!vram || gmc.gartTableVramOffset > vramSize ||
        gmc.gartTableSize > vramSize - gmc.gartTableVramOffset) {
        gmcReleaseContiguous(md, true);
        return 0;
    }

    volatile uint64_t* table = reinterpret_cast<volatile uint64_t*>(
        vram + gmc.gartTableVramOffset);
    for (uint32_t i = 0; i < pages; i++) {
        const uint64_t pagePhys = phys + static_cast<uint64_t>(i) * GPU_PAGE_SIZE;
        // PTE holds a byte address, flags live in the low 12 bits.
        table[gmc.gartNextPage + i] = (pagePhys & ~0xFFFULL) | GART_PTE_FLAGS;
    }

    const uint64_t gpuAddr =
        gmc.gartStart + static_cast<uint64_t>(gmc.gartNextPage) * GPU_PAGE_SIZE;
    gmc.gartNextPage += pages;

    *outMd = md;
    LOG("GMC: GART buffer %u pages -> GPU 0x%llx (phys 0x%llx)",
        pages, static_cast<unsigned long long>(gpuAddr),
        static_cast<unsigned long long>(phys));
    return gpuAddr;
}

void gmcUnmapGartBuffers(RDNA4Driver& driver, GMCContext& gmc) {
    if (gmc.gartNextPage == 0x1000) return;
    volatile uint8_t* vram = nullptr;
    size_t vramSize = 0;
    driver.vramWindow(vram, vramSize);
    const uint64_t dummyPte = (gmc.dummyPagePhys & ~0xFFFULL) | GART_PTE_FLAGS;
    if (gmcResetGartTable(vram, vramSize, gmc, dummyPte)) {
        gmcInvalidateTlb(driver);
    } else {
        LOG_ERROR("GMC: failed to reset allocated GART table entries");
    }
}

void gmcInvalidateTlb(RDNA4Driver& driver) {
    gmcHDPFlush(driver);
    gmcFlushTLB(driver, IPDiscovery::HwMmhub,
                mmMMVM_INVALIDATE_ENG17_REQ, mmMMVM_INVALIDATE_ENG17_ACK);
    gmcFlushTLB(driver, IPDiscovery::HwGc,
                gcGCVM_INVALIDATE_ENG17_REQ, gcGCVM_INVALIDATE_ENG17_ACK);
}
