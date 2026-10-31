//
//  GraphicsRegs.hpp
//  RDNA4Driver - GFX12 (Navi 48) register addresses used by the graphics path
//
//  These are MMIO byte addresses, not packet offsets. GraphicsPM4Builder turns
//  them into SET_CONTEXT_REG / SET_SH_REG / SET_UCONFIG_REG offsets by taking
//  the block base into account, so callers never hand-assemble an offset.
//
//  Values were taken from navi48/Cobalt64-main/src/xlat12/xlat12_tables.h, the
//  gfx12 (Navi 48) address table generated from mesa's kgd register database.
//  Registers that gfx12 removed (CB_COLOR0_PITCH/SLICE, VGT_GS_MODE) or that
//  only exist as legacy-VS (SPI_SHADER_PGM_*_VS) are deliberately absent here:
//  gfx12 runs the vertex stage as an NGG ES/GS pair.
//

#ifndef GraphicsRegs_hpp
#define GraphicsRegs_hpp

#include <stdint.h>

namespace GFX12 {

// ---------------------------------------------------------------------------
// Register address blocks. A PM4 SET packet carries an offset inside one of
// these spaces, and the space itself is selected by the packet opcode.
// ---------------------------------------------------------------------------
constexpr uint32_t kContextBase = 0x28000u;   // SET_CONTEXT_REG, dword base 0xa000
constexpr uint32_t kContextEnd  = 0x2c000u;
constexpr uint32_t kShBase      = 0x0b000u;   // SET_SH_REG,      dword base 0x2c00
constexpr uint32_t kShEnd       = 0x0c000u;
constexpr uint32_t kUconfigBase = 0x30000u;   // SET_UCONFIG_REG, dword base 0xc000
constexpr uint32_t kUconfigEnd  = 0x40000u;

// ---------------------------------------------------------------------------
// Context registers
// ---------------------------------------------------------------------------
constexpr uint32_t DB_RENDER_CONTROL         = 0x28000u;
constexpr uint32_t DB_RENDER_OVERRIDE        = 0x2800cu;
constexpr uint32_t DB_RENDER_OVERRIDE2       = 0x28010u;
constexpr uint32_t DB_Z_INFO                 = 0x28018u;
constexpr uint32_t DB_STENCIL_INFO           = 0x2801cu;
constexpr uint32_t DB_DEPTH_CONTROL          = 0x28070u;
constexpr uint32_t DB_EQAA                   = 0x28078u;
constexpr uint32_t DB_ALPHA_TO_MASK          = 0x2807cu;
constexpr uint32_t DB_STENCIL_OPVAL          = 0x2808cu;

constexpr uint32_t PA_SC_SCREEN_SCISSOR_TL   = 0x28180u;
constexpr uint32_t PA_SC_SCREEN_SCISSOR_BR   = 0x28184u;
constexpr uint32_t PA_SC_WINDOW_OFFSET       = 0x28200u;
constexpr uint32_t PA_SC_WINDOW_SCISSOR_TL   = 0x28204u;
constexpr uint32_t PA_SC_WINDOW_SCISSOR_BR   = 0x28208u;
constexpr uint32_t PA_SC_CLIPRECT_RULE       = 0x2820cu;
// PA_SC_EDGERULE: 0xaa959a6a = ER_TRI(0xA) | ER_POINT(0x6) | ER_RECT(0xA) | ...
// Defines the top-left fill rule for rasterizing primitives (Mesa si_state.c:1010).
// Without it, ER_TRI=0 rejects all triangle edges and produces 0 fragments!
constexpr uint32_t PA_SC_EDGERULE            = 0x28230u;
constexpr uint32_t PA_SU_HARDWARE_SCREEN_OFFSET = 0x28234u;
constexpr uint32_t PA_SC_GENERIC_SCISSOR_TL  = 0x28240u;
constexpr uint32_t PA_SC_GENERIC_SCISSOR_BR  = 0x28244u;
constexpr uint32_t PA_SC_VPORT_SCISSOR_0_TL  = 0x28250u;
constexpr uint32_t PA_SC_VPORT_SCISSOR_0_BR  = 0x28254u;
constexpr uint32_t PA_SC_VPORT_SCISSOR_1_TL  = 0x28258u;
constexpr uint32_t PA_SC_VPORT_SCISSOR_1_BR  = 0x2825cu;
constexpr uint32_t PA_SC_VPORT_SCISSOR_STRIDE = 0x08u;

constexpr uint32_t PA_CL_VPORT_XSCALE        = 0x2843cu;
constexpr uint32_t PA_CL_VPORT_XOFFSET       = 0x28440u;
constexpr uint32_t PA_CL_VPORT_YSCALE        = 0x28444u;
constexpr uint32_t PA_CL_VPORT_YOFFSET       = 0x28448u;
constexpr uint32_t PA_CL_VPORT_ZSCALE        = 0x2844cu;
constexpr uint32_t PA_CL_VPORT_ZOFFSET       = 0x28450u;
constexpr uint32_t PA_SC_VPORT_ZMIN_0        = 0x28454u;
constexpr uint32_t PA_SC_VPORT_ZMAX_0        = 0x28458u;
constexpr uint32_t PA_CL_VPORT_STRIDE        = 0x10u;

constexpr uint32_t SPI_PS_IN_CONTROL         = 0x28640u;
constexpr uint32_t SPI_INTERP_CONTROL_0      = 0x28644u;
constexpr uint32_t SPI_SHADER_IDX_FORMAT     = 0x28648u;
constexpr uint32_t SPI_SHADER_POS_FORMAT     = 0x2864cu;
constexpr uint32_t SPI_SHADER_Z_FORMAT       = 0x28650u;
constexpr uint32_t SPI_SHADER_COL_FORMAT     = 0x28654u;
constexpr uint32_t SPI_SHADER_POS_NONE       = 0x0u;
constexpr uint32_t SPI_SHADER_4COMP          = 0x4u;   // POS format enum
// SPI_SHADER_COL_FORMAT uses its own enum (gfx12.json SPI_SHADER_COL_FORMAT):
// 4 is FP16_ABGR, 9 is 32_ABGR - the 4x32-bit float export a fragment shader
// emits with `exp mrt0`. The POS enum value 4 is NOT valid here.
constexpr uint32_t SPI_SHADER_COL_32_ABGR    = 0x9u;
constexpr uint32_t SPI_BARYC_CNTL            = 0x28658u;
constexpr uint32_t SPI_PS_INPUT_ENA          = 0x2865cu;
constexpr uint32_t SPI_PS_INPUT_ADDR         = 0x28660u;
constexpr uint32_t SPI_PS_INPUT_CNTL_0       = 0x28664u;
constexpr uint32_t SPI_PS_INPUT_CNTL_1       = 0x28668u;
constexpr uint32_t SPI_BARYC_SSAA_CNTL       = 0x286e4u;
constexpr uint32_t SPI_TMPRING_SIZE          = 0x286e8u;

// CB_COLOR_CONTROL.MODE: gfx12 has no "enable" bit, the mode enum carries it.
constexpr uint32_t CB_COLOR_CONTROL          = 0x28858u;
constexpr uint32_t CB_MODE_DISABLE           = 0x0u;
constexpr uint32_t CB_MODE_NORMAL            = 0x1u;

constexpr uint32_t DB_SHADER_CONTROL         = 0x2806cu;
// SX_PS_DOWNCONVERT_CONTROL on GFX12: bit k = MRTk_FMT_MAPPING_DISABLE.
// Setting 0xFF disables format downconversion for MRT0..7 (Mesa ac_cmdbuf.c:743).
// 0x28754 is SX_PS_DOWNCONVERT (the downconversion target table); writing 0xFF
// there left the control word at 0 and active downconversion corrupted MRT0.
constexpr uint32_t SX_PS_DOWNCONVERT         = 0x28750u;
constexpr uint32_t PA_SU_PRIM_FILTER_CNTL    = 0x2882cu;
constexpr uint32_t PA_SC_AA_MASK_X0Y0_X1Y0   = 0x28c38u;
constexpr uint32_t PA_SC_AA_MASK_X0Y1_X1Y1   = 0x28c3cu;
constexpr uint32_t PA_SC_BINNER_CNTL_0       = 0x28c44u;
constexpr uint32_t PA_SC_BINNER_CNTL_1       = 0x28c48u;
constexpr uint32_t PA_SC_BINNER_CNTL_2       = 0x28c4cu;
constexpr uint32_t PA_SC_NGG_MODE_CNTL       = 0x28c50u;
constexpr uint32_t PA_SC_CONSERVATIVE_RASTERIZATION_CNTL = 0x28c54u;
constexpr uint32_t PA_SC_VRS_OVERRIDE_CNTL   = 0x283d0u;
constexpr uint32_t PA_SC_VRS_INFO            = 0x283e0u;
constexpr uint32_t PA_SC_HIZ_INFO            = 0x28b94u;
constexpr uint32_t PA_SC_HIS_INFO            = 0x28b98u;
constexpr uint32_t PA_SC_HISZ_CONTROL        = 0x28bbcu;
constexpr uint32_t PA_SC_HISZ_RENDER_OVERRIDE = 0x28bc0u;
constexpr uint32_t CB_MEM0_INFO              = 0x28f00u;

constexpr uint32_t CB_BLEND0_CONTROL         = 0x28780u;
constexpr uint32_t CB_BLEND_STRIDE            = 0x04u;
constexpr uint32_t PA_CL_CLIP_CNTL           = 0x28810u;
constexpr uint32_t PA_CL_VTE_CNTL            = 0x28814u;
constexpr uint32_t PA_CL_VS_OUT_CNTL         = 0x28818u;
constexpr uint32_t PA_SU_SC_MODE_CNTL        = 0x2881cu;
constexpr uint32_t CB_TARGET_MASK            = 0x28850u;
constexpr uint32_t CB_SHADER_MASK            = 0x28854u;

constexpr uint32_t GE_MAX_OUTPUT_PER_SUBGROUP = 0x287fcu;
constexpr uint32_t VGT_GS_MAX_VERT_OUT       = 0x28b38u;
constexpr uint32_t GE_NGG_SUBGRP_CNTL        = 0x28b4cu;
constexpr uint32_t VGT_GS_INSTANCE_CNT       = 0x28b3cu;
constexpr uint32_t PA_SC_MODE_CNTL_0         = 0x28a48u;
constexpr uint32_t PA_SC_MODE_CNTL_1         = 0x28a4cu;
constexpr uint32_t VGT_SHADER_STAGES_EN      = 0x28a98u;
constexpr uint32_t VGT_DRAW_PAYLOAD_CNTL     = 0x28aa0u;
constexpr uint32_t VGT_TF_PARAM              = 0x28aa4u;
constexpr uint32_t VGT_LS_HS_CONFIG          = 0x28b58u;
constexpr uint32_t PA_SC_LINE_CNTL           = 0x28bdcu;
constexpr uint32_t PA_SC_AA_CONFIG           = 0x28be0u;
constexpr uint32_t PA_SU_VTX_CNTL            = 0x28be4u;
constexpr uint32_t PA_SC_CENTROID_PRIORITY_0 = 0x28bf0u;
constexpr uint32_t VGT_REUSE_OFF             = 0x28a9cu;

// Color target registers. COLOR0..COLOR7 are a fixed 0x24 apart; INFO and
// BASE_EXT are contiguous per target. PITCH and SLICE no longer exist on
// gfx12 - the size lives in CB_COLORn_ATTRIB2, the swizzle mode in ATTRIB3
// and the hardware derives the pitch from FORMAT. BASE holds address bits
// [39:8]; BASE_EXT holds bits [47:40].
constexpr uint32_t CB_COLOR0_BASE            = 0x28c60u;
constexpr uint32_t CB_COLOR0_VIEW            = 0x28c64u;
constexpr uint32_t CB_COLOR0_VIEW2           = 0x28c68u;
constexpr uint32_t CB_COLOR0_ATTRIB          = 0x28c6cu;
constexpr uint32_t CB_COLOR0_FDCC_CONTROL    = 0x28c70u;
constexpr uint32_t CB_COLOR0_ATTRIB2         = 0x28c78u;
constexpr uint32_t CB_COLOR0_ATTRIB3         = 0x28c7cu;
constexpr uint32_t CB_COLOR_TARGET_STRIDE    = 0x24u;
constexpr uint32_t CB_COLOR0_BASE_EXT        = 0x28e40u;
constexpr uint32_t CB_COLOR0_INFO            = 0x28ec0u;
constexpr uint32_t CB_COLOR_INFO_STRIDE      = 0x04u;

// CB_COLORn_ATTRIB3 fields (gfx12.json): MIP0_DEPTH [13:0], COLOR_SW_MODE
// [17:15] (ADDR3_LINEAR = 0), MAX_MIP [23:19], RESOURCE_TYPE [25:24].
constexpr uint32_t CB_ATTRIB3_RESOURCE_2D    = 1u << 24;

// ---------------------------------------------------------------------------
// Shader registers
// ---------------------------------------------------------------------------
constexpr uint32_t SPI_SHADER_PGM_RSRC3_PS   = 0x0b018u;
constexpr uint32_t SPI_SHADER_PGM_LO_PS      = 0x0b020u;
constexpr uint32_t SPI_SHADER_PGM_HI_PS      = 0x0b024u;
constexpr uint32_t SPI_SHADER_PGM_RSRC1_PS   = 0x0b028u;
constexpr uint32_t SPI_SHADER_PGM_RSRC2_PS   = 0x0b02cu;
constexpr uint32_t SPI_SHADER_USER_DATA_PS_0 = 0x0b030u;
constexpr uint32_t SPI_SHADER_REQ_CTRL_PS    = 0x0b0c0u;

constexpr uint32_t SPI_SHADER_PGM_LO_GS      = 0x0b210u;
constexpr uint32_t SPI_SHADER_PGM_HI_GS      = 0x0b214u;
constexpr uint32_t SPI_SHADER_PGM_HI_ES      = 0x0b218u;
constexpr uint32_t SPI_SHADER_PGM_RSRC3_GS   = 0x0b21cu;
constexpr uint32_t SPI_SHADER_PGM_LO_ES      = 0x0b224u;
constexpr uint32_t SPI_SHADER_PGM_RSRC1_GS   = 0x0b228u;
constexpr uint32_t SPI_SHADER_PGM_RSRC2_GS   = 0x0b22cu;
constexpr uint32_t SPI_SHADER_PGM_RSRC4_GS   = 0x0b220u;
constexpr uint32_t SPI_SHADER_PGM_RSRC4_PS   = 0x0b01cu;
constexpr uint32_t SPI_SHADER_GS_OUT_CONFIG_PS = 0x0b0c4u;
constexpr uint32_t SPI_SHADER_USER_DATA_GS_0 = 0x0b230u;

constexpr uint32_t SPI_SHADER_PGM_LO_LS      = 0x0b424u;
constexpr uint32_t SPI_SHADER_PGM_RSRC3_HS   = 0x0b41cu;
constexpr uint32_t SPI_SHADER_USER_DATA_HS_0 = 0x0b430u;

// ---------------------------------------------------------------------------
// Uconfig registers (primitive and geometry state, gfx11+ moved these here)
// ---------------------------------------------------------------------------
constexpr uint32_t VGT_PRIMITIVE_TYPE        = 0x30908u;
constexpr uint32_t VGT_INDEX_TYPE            = 0x3090cu;
constexpr uint32_t GE_MIN_VTX_INDX           = 0x30924u;
constexpr uint32_t GE_MULTI_PRIM_IB_RESET_EN = 0x3092cu;
constexpr uint32_t VGT_NUM_INSTANCES         = 0x30934u;
constexpr uint32_t GE_GS_THROTTLE            = 0x30950u;
constexpr uint32_t GE_MAX_VTX_INDX           = 0x30964u;
constexpr uint32_t GE_CNTL                   = 0x3096cu;
constexpr uint32_t GE_USER_VGPR_EN           = 0x30980u;
constexpr uint32_t VGT_PRIMITIVEID_EN        = 0x30988u;
constexpr uint32_t VGT_GS_OUT_PRIM_TYPE      = 0x30998u;
constexpr uint32_t SPI_GRP_LAUNCH_GUARANTEE_ENABLE = 0x31128u;
constexpr uint32_t SPI_GRP_LAUNCH_GUARANTEE_CTRL   = 0x3112cu;

// ---------------------------------------------------------------------------
// Field values
// ---------------------------------------------------------------------------

// VGT_DRAW_INITIATOR.SOURCE_SELECT
constexpr uint32_t DI_SRC_SEL_DMA            = 0u;
constexpr uint32_t DI_SRC_SEL_IMMEDIATE      = 1u;
constexpr uint32_t DI_SRC_SEL_AUTO_INDEX     = 2u;

// NGG ring regions. Mesa's Navi48 sizes (ac_gpu_info.c, 4 SEs): the attribute
// ring is 0x160000 per SE -> 0x580000, the position ring sits at +0x580000 and
// the primitive ring at +0x980000, 0xA80000 bytes in one mapping. The per-SE
// attribute size - not the total - is what SPI_ATTRIBUTE_RING_SIZE encodes.
constexpr uint32_t kAttributeRingPerSE   = 0x160000u;
constexpr uint32_t kAttributeRingTotal   = 0x580000u;
constexpr uint32_t kGePosRingOffset      = 0x580000u;
constexpr uint32_t kGePrimRingOffset     = 0x980000u;
constexpr uint32_t kNggRingTotal         = 0xA80000u;

// SPI_SHADER_PGM_RSRC3_*CU_EN masks. Navi48 reports one harvested CU per SA,
// which is why the GS/ES mask is not simply 0xffff; both values are the ones
// Mesa programs for gfx12 (ac_cmdbuf.c gfx12_init_graphics_preamble_state).
constexpr uint32_t CU_EN_GS_ALL              = 0xfffffdfdu;
constexpr uint32_t CU_EN_PS_ALL              = 0xffffu;

// VGT_DRAW_INITIATOR bits (gfx10.3+ layout)
constexpr uint32_t DI_FLAG_NOT_EOP           = 1u << 5;
constexpr uint32_t DI_FLAG_USE_OPAQUE        = 1u << 6;

// VGT_INDEX_TYPE
constexpr uint32_t VGT_INDEX_16              = 0u;
constexpr uint32_t VGT_INDEX_32              = 1u;

// VGT_DI_PRIM_TYPE (VGT_PRIMITIVE_TYPE)
constexpr uint32_t DI_PT_POINTLIST           = 1u;
constexpr uint32_t DI_PT_LINELIST            = 2u;
constexpr uint32_t DI_PT_LINESTRIP           = 3u;
constexpr uint32_t DI_PT_TRILIST             = 4u;
constexpr uint32_t DI_PT_TRISTRIP            = 5u;
constexpr uint32_t DI_PT_TRIFAN              = 6u;

// PA_SU_SC_MODE_CNTL
constexpr uint32_t SC_CULL_FRONT             = 1u << 0;
constexpr uint32_t SC_CULL_BACK              = 1u << 1;
constexpr uint32_t SC_FACE_CW                = 1u << 2;

// CB_COLORn_INFO.FORMAT values (gfx12 ColorFormat)
constexpr uint32_t CB_FORMAT_8_UNORM         = 0u;
constexpr uint32_t CB_FORMAT_8_8_UNORM       = 1u;
constexpr uint32_t CB_FORMAT_8_8_8_8_UNORM   = 10u;
constexpr uint32_t CB_FORMAT_32_FLOAT        = 15u;
constexpr uint32_t CB_FORMAT_16_FLOAT        = 14u;

// CB_BLENDn_CONTROL field shifts
constexpr uint32_t BLEND_COLOR_SRC_SHIFT     = 0u;
constexpr uint32_t BLEND_COLOR_FCN_SHIFT     = 5u;
constexpr uint32_t BLEND_COLOR_DST_SHIFT     = 8u;
constexpr uint32_t BLEND_ALPHA_SRC_SHIFT     = 16u;
constexpr uint32_t BLEND_ALPHA_FCN_SHIFT     = 21u;
constexpr uint32_t BLEND_ALPHA_DST_SHIFT     = 24u;

constexpr uint32_t BLEND_ZERO                = 0u;
constexpr uint32_t BLEND_ONE                 = 1u;
constexpr uint32_t BLEND_SRC_COLOR           = 2u;
constexpr uint32_t BLEND_ONE_MINUS_SRC_COLOR = 3u;
constexpr uint32_t BLEND_SRC_ALPHA           = 4u;
constexpr uint32_t BLEND_ONE_MINUS_SRC_ALPHA = 5u;
constexpr uint32_t BLEND_DST_ALPHA           = 6u;
constexpr uint32_t BLEND_ONE_MINUS_DST_ALPHA = 7u;
constexpr uint32_t BLEND_DST_COLOR           = 8u;
constexpr uint32_t BLEND_ONE_MINUS_DST_COLOR = 9u;

constexpr uint32_t COMB_DISABLE              = 0u;
constexpr uint32_t COMB_ADD                  = 1u;
constexpr uint32_t COMB_SUBTRACT             = 2u;
constexpr uint32_t COMB_MIN                  = 3u;
constexpr uint32_t COMB_MAX                  = 4u;

// ---------------------------------------------------------------------------
// PM4 opcodes used by the graphics path
// ---------------------------------------------------------------------------
constexpr uint32_t PKT3_SET_CONTEXT_REG      = 0x69u;
constexpr uint32_t PKT3_SET_SH_REG           = 0x76u;
constexpr uint32_t PKT3_SET_UCONFIG_REG      = 0x79u;
constexpr uint32_t PKT3_ACQUIRE_MEM          = 0x58u;
constexpr uint32_t PKT3_CONTEXT_CONTROL      = 0x28u;

// ACQUIRE_MEM's final GCR_CNTL dword (GFX10+, including GFX12).
// The older CP_COHER_CNTL action bits do not encode cache operations here.
constexpr uint32_t GCR_GLI_INV_ALL = 1u;
constexpr uint32_t GCR_GLK_INV     = 1u << 7;
constexpr uint32_t GCR_GLV_INV     = 1u << 8;
constexpr uint32_t GCR_GL2_INV     = 1u << 14;
constexpr uint32_t GCR_GL2_WB      = 1u << 15;
constexpr uint32_t PKT3_DRAW_INDEX_AUTO      = 0x2du;
// Graphics work reaches the PFP through an indirect buffer; the ring itself
// only carries this packet plus the fence.
constexpr uint32_t PKT3_INDIRECT_BUFFER      = 0x3fu;
// Every gfx10+ driver sets the instance count with this packet rather than a
// register write; the CP keeps its own copy and the draw reads that.
// 0x2F is PKT3_NUM_INSTANCES (Mesa sid.h, amdgpu soc15d.h, Cobalt64
// xlat12_ib.c). 0x2A is PKT3_INDEX_TYPE and only exists on gfx6-8, so using it
// here emitted an index-type packet - or nothing at all on gfx12 - instead of
// the instance count.
constexpr uint32_t PKT3_NUM_INSTANCES        = 0x2fu;
constexpr uint32_t PKT3_DRAW_INDEX_2         = 0x27u;
constexpr uint32_t PKT3_EVENT_WRITE          = 0x46u;
constexpr uint32_t PKT3_RELEASE_MEM          = 0x49u;

// EVENT_WRITE event type/index for "flush and invalidate caches".
constexpr uint32_t EVENT_TYPE_CACHE_FLUSH_AND_INV_TS = 0x14u;
constexpr uint32_t EVENT_INDEX_CACHE_FLUSH_AND_INV_TS = 0x05u;

} // namespace GFX12

#endif /* GraphicsRegs_hpp */
