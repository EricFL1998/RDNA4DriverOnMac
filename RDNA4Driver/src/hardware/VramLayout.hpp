#ifndef VramLayout_hpp
#define VramLayout_hpp

#include <stdint.h>

// Fixed VRAM layout used by the bring-up driver. Every region is an offset
// from the start of the BAR0/FB aperture (VRAM-relative, not an MC address).
// Keep this table in sync with the staging offsets used in RDNA4Driver.cpp.
namespace VramLayout {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;

// PSP bootloader staging window: 64 MiB .. 68 MiB.
constexpr uint64_t kPspStagingBase = 64ULL * kMiB;
constexpr uint64_t kPspRing        = kPspStagingBase + 1ULL * kMiB;
constexpr uint64_t kPspCommand     = kPspStagingBase + 2ULL * kMiB;
constexpr uint64_t kPspFence       = kPspStagingBase + 3ULL * kMiB;

// SMU PMFW payload (LOAD_IP_FW type 18): 68 MiB .. 69 MiB.
constexpr uint64_t kSmuFirmware    = 68ULL * kMiB;

// GFX firmware staging: 71 MiB onward (CP/MES, SDMA, TOC, RLC, REG_LIST).
constexpr uint64_t kGfxStagingBase = 71ULL * kMiB;

// SMU driver table: a dedicated 64 KiB region, clear of the PSP/GFX staging
// and of the low 64 MiB reserved for display/console.
constexpr uint64_t kSmuDriverTable     = 96ULL * kMiB;
constexpr uint32_t kSmuDriverTableSize = 64U * 1024U;

// GART page table: 1 MiB for a 512 MiB aperture (131072 x 8-byte PTEs).
constexpr uint64_t kGartTable = 128ULL * kMiB;
constexpr uint64_t kGartSize  = 512ULL * kMiB;

// Compute ring (Phase 2.3). The ring and fence must stay clear of the PSP
// staging window (64..68 MiB), GFX staging (71..88 MiB), the SMU driver
// table (96 MiB) and the GART page table (128 MiB).
constexpr uint64_t kComputeRing     = 192ULL * kMiB;
constexpr uint64_t kComputeRingSize = 1ULL * kMiB;
constexpr uint64_t kComputeFence    = kComputeRing + kComputeRingSize;
constexpr uint64_t kComputeHeap     = 194ULL * kMiB;

} // namespace VramLayout

#endif
