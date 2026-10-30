//
//  RDNA4FramebufferEdid.hpp
//  Read the attached monitor's EDID over DisplayPort AUX or the HDMI
//  display engine's DC_I2C controller, following RDNA4FB's hardware-verified
//  sequences. No display-mode or link programming happens here, and the
//  engines are handed back after use.
//

#ifndef RDNA4FramebufferEdid_hpp
#define RDNA4FramebufferEdid_hpp

#include <stdint.h>

// Returns true and fills `edid`/`length` (128 or 256 bytes) when a valid
// EDID base block was read. `line` receives AUX instance 0/1 or DDC line 2/3.
// `mmioBytes` is the length of the BAR5 mapping used for every register access.
bool rdna4fbReadEdid(volatile uint32_t* mmio, uint64_t mmioBytes,
                     uint8_t edid[256], uint32_t* length, uint32_t* line);

struct RDNA4FBEdidAccess {
    void* context;
    uint32_t (*read)(void* context, uint32_t dword);
    void (*write)(void* context, uint32_t dword, uint32_t value);
};

bool rdna4fbReadEdidWithAccess(const RDNA4FBEdidAccess& access,
                               uint8_t edid[256], uint32_t* length, uint32_t* line);

bool rdna4fbReadEdidBlock(const uint8_t* edid, uint32_t edidLength,
                          uint32_t blockNumber, uint8_t* data, uint64_t* length);

#endif /* RDNA4FramebufferEdid_hpp */
