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
constexpr uint32_t DB_Z_INFO                 = 0x28018u;
constexpr uint32_t DB_STENCIL_INFO           = 0x2801cu;
constexpr uint32_t DB_DEPTH_CONTROL          = 0x28070u;

constexpr uint32_t PA_SC_WINDOW_SCISSOR_TL   = 0x28204u;
constexpr uint32_t PA_SC_WINDOW_SCISSOR_BR   = 0x28208u;
constexpr uint32_t PA_SC_CLIPRECT_RULE       = 0x2820cu;
constexpr uint32_t PA_SC_GENERIC_SCISSOR_TL  = 0x28240u;
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
constexpr uint32_t PA_CL_VPORT_STRIDE        = 0x10u;

constexpr uint32_t SPI_PS_IN_CONTROL         = 0x28640u;
constexpr uint32_t SPI_INTERP_CONTROL_0      = 0x28644u;
constexpr uint32_t SPI_SHADER_POS_FORMAT     = 0x2864cu;
constexpr uint32_t SPI_BARYC_CNTL            = 0x28658u;
constexpr uint32_t SPI_PS_INPUT_CNTL_0       = 0x28664u;
constexpr uint32_t SPI_PS_INPUT_CNTL_1       = 0x28668u;

constexpr uint32_t CB_BLEND0_CONTROL         = 0x28780u;
constexpr uint32_t CB_BLEND_STRIDE            = 0x04u;
constexpr uint32_t PA_CL_CLIP_CNTL           = 0x28810u;
constexpr uint32_t PA_CL_VTE_CNTL            = 0x28814u;
constexpr uint32_t PA_SU_SC_MODE_CNTL        = 0x2881cu;
constexpr uint32_t CB_TARGET_MASK            = 0x28850u;
constexpr uint32_t CB_SHADER_MASK            = 0x28854u;

constexpr uint32_t PA_SC_MODE_CNTL_0         = 0x28a48u;
constexpr uint32_t PA_SC_MODE_CNTL_1         = 0x28a4cu;
constexpr uint32_t VGT_SHADER_STAGES_EN      = 0x28a98u;
constexpr uint32_t VGT_DRAW_PAYLOAD_CNTL     = 0x28aa0u;
constexpr uint32_t VGT_TF_PARAM              = 0x28aa4u;
constexpr uint32_t VGT_LS_HS_CONFIG          = 0x28b58u;
constexpr uint32_t PA_SC_LINE_CNTL           = 0x28bdcu;
constexpr uint32_t PA_SC_AA_CONFIG           = 0x28be0u;
constexpr uint32_t PA_SC_CENTROID_PRIORITY_0 = 0x28bf0u;

// Color target registers. COLOR0..COLOR7 are a fixed 0x24 apart; INFO is
// contiguous per target. PITCH and SLICE no longer exist on gfx12 - the size
// lives in CB_COLORn_ATTRIB2 and the hardware derives the pitch from FORMAT.
constexpr uint32_t CB_COLOR0_BASE            = 0x28c60u;
constexpr uint32_t CB_COLOR0_VIEW            = 0x28c64u;
constexpr uint32_t CB_COLOR0_ATTRIB          = 0x28c6cu;
constexpr uint32_t CB_COLOR0_ATTRIB2         = 0x28c78u;
constexpr uint32_t CB_COLOR_TARGET_STRIDE    = 0x24u;
constexpr uint32_t CB_COLOR0_INFO            = 0x28ec0u;
constexpr uint32_t CB_COLOR_INFO_STRIDE      = 0x04u;

// ---------------------------------------------------------------------------
// Shader registers
// ---------------------------------------------------------------------------
constexpr uint32_t SPI_SHADER_PGM_RSRC3_PS   = 0x0b018u;
constexpr uint32_t SPI_SHADER_PGM_LO_PS      = 0x0b020u;
constexpr uint32_t SPI_SHADER_PGM_HI_PS      = 0x0b024u;
constexpr uint32_t SPI_SHADER_PGM_RSRC1_PS   = 0x0b028u;
constexpr uint32_t SPI_SHADER_PGM_RSRC2_PS   = 0x0b02cu;
constexpr uint32_t SPI_SHADER_USER_DATA_PS_0 = 0x0b030u;

constexpr uint32_t SPI_SHADER_PGM_LO_GS      = 0x0b210u;
constexpr uint32_t SPI_SHADER_PGM_HI_GS      = 0x0b214u;
constexpr uint32_t SPI_SHADER_PGM_HI_ES      = 0x0b218u;
constexpr uint32_t SPI_SHADER_PGM_LO_ES      = 0x0b224u;
constexpr uint32_t SPI_SHADER_PGM_RSRC1_GS   = 0x0b228u;
constexpr uint32_t SPI_SHADER_PGM_RSRC2_GS   = 0x0b22cu;

constexpr uint32_t SPI_SHADER_PGM_LO_LS      = 0x0b424u;
constexpr uint32_t SPI_SHADER_USER_DATA_HS_0 = 0x0b430u;

// ---------------------------------------------------------------------------
// Uconfig registers (primitive and geometry state, gfx11+ moved these here)
// ---------------------------------------------------------------------------
constexpr uint32_t VGT_PRIMITIVE_TYPE        = 0x30908u;
constexpr uint32_t VGT_INDEX_TYPE            = 0x3090cu;
constexpr uint32_t GE_CNTL                   = 0x3096cu;
constexpr uint32_t GE_USER_VGPR_EN           = 0x30980u;
constexpr uint32_t VGT_PRIMITIVEID_EN        = 0x30988u;
constexpr uint32_t VGT_GS_OUT_PRIM_TYPE      = 0x30998u;

// ---------------------------------------------------------------------------
// Field values
// ---------------------------------------------------------------------------

// VGT_DRAW_INITIATOR.SOURCE_SELECT
constexpr uint32_t DI_SRC_SEL_DMA            = 0u;
constexpr uint32_t DI_SRC_SEL_IMMEDIATE      = 1u;
constexpr uint32_t DI_SRC_SEL_AUTO_INDEX     = 2u;

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
constexpr uint32_t PKT3_DRAW_INDEX_AUTO      = 0x2du;
constexpr uint32_t PKT3_DRAW_INDEX_2         = 0x27u;
constexpr uint32_t PKT3_EVENT_WRITE          = 0x46u;
constexpr uint32_t PKT3_RELEASE_MEM          = 0x49u;

// EVENT_WRITE event type/index for "flush and invalidate caches".
constexpr uint32_t EVENT_TYPE_CACHE_FLUSH_AND_INV_TS = 0x14u;
constexpr uint32_t EVENT_INDEX_CACHE_FLUSH_AND_INV_TS = 0x05u;

} // namespace GFX12

#endif /* GraphicsRegs_hpp */
