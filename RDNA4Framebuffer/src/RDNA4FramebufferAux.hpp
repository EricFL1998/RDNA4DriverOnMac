#ifndef RDNA4FramebufferAux_hpp
#define RDNA4FramebufferAux_hpp

#include "RDNA4FramebufferEdid.hpp"

bool rdna4fbAuxReadBlockWithAccess(const RDNA4FBEdidAccess& access,
                                   uint32_t instance, uint8_t start, uint8_t out[128]);

#endif
