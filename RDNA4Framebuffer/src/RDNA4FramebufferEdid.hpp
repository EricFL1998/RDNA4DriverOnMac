//
//  RDNA4FramebufferEdid.hpp
//  Read the attached monitor's EDID over the display engine's DC_I2C
//  controller, so macOS can name the display instead of "unknown display".
//
//  The engine sequence is the one hardware-verified on this card by
//  Cobalt64 (dcn/navi48_dispread_flow.h, itself a port of RDNA4FB's
//  AMDRDNA4FB::readEDIDI2C).  No display-mode programming happens here:
//  only the I2C engine's own registers are touched, and the engine is
//  always handed back.
//

#ifndef RDNA4FramebufferEdid_hpp
#define RDNA4FramebufferEdid_hpp

#include <stdint.h>

// Returns true and fills `edid`/`length` (128 or 256 bytes) when a valid
// EDID base block was read.  `line` receives the DDC line that answered.
// `mmioBytes` is the length of the BAR5 mapping used for every register access.
bool rdna4fbReadEdid(volatile uint32_t* mmio, uint64_t mmioBytes,
                     uint8_t edid[256], uint32_t* length, uint32_t* line);

#endif /* RDNA4FramebufferEdid_hpp */
