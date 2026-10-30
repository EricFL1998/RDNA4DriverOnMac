#include "RDNA4FramebufferAux.hpp"
#include <IOKit/IOLib.h>

namespace Aux {

constexpr uint32_t kDmuSeg2 = 0x34C0u;
constexpr uint32_t kAuxControl = 0x16B2u;
constexpr uint32_t kAuxStride = 0x1Cu;
constexpr uint32_t kSwControl = 1u;
constexpr uint32_t kArbControl = 2u;
constexpr uint32_t kInterruptControl = 3u;
constexpr uint32_t kSwStatus = 4u;
constexpr uint32_t kSwData = 6u;
constexpr uint32_t kAuxEnable = 1u;
constexpr uint32_t kGo = 1u;
constexpr uint32_t kWrittenBytesMask = 0x1Fu << 16;
constexpr uint32_t kWrittenBytesShift = 16u;
constexpr uint32_t kOwnerMask = 3u << 2;
constexpr uint32_t kOwnerSoftware = 1u << 2;
constexpr uint32_t kUseRequest = 1u << 16;
constexpr uint32_t kDoneUsing = 1u << 17;
constexpr uint32_t kDoneAck = 1u << 1;
constexpr uint32_t kDone = 1u;
constexpr uint32_t kStatusError = (7u << 4) | (1u << 7) | (1u << 9);
constexpr uint32_t kReplyCountShift = 24u;
constexpr uint32_t kDataRead = 1u;
constexpr uint32_t kDataShift = 8u;
constexpr uint32_t kDataMask = 0xFFu << 8;
constexpr uint32_t kAutoincrementDisable = 1u << 31;
constexpr uint8_t kI2cWriteMot = 0x40u;
constexpr uint8_t kI2cRead = 0x10u;
constexpr uint8_t kI2cReadMot = 0x50u;
constexpr uint8_t kAck = 0u;
constexpr uint8_t kAuxDefer = 2u;
constexpr uint8_t kI2cDefer = 8u;
constexpr uint8_t kEdidAddress = 0x50u;
constexpr uint32_t kMaxChunk = 16u;
constexpr uint32_t kRetries = 7u;

struct AuxEngine {
    const RDNA4FBEdidAccess& access;
    uint32_t base;
    bool writeFailed { false };

    uint32_t rd(uint32_t reg) const {
        return access.read(access.context, static_cast<uint32_t>(kDmuSeg2) + base + reg);
    }

    void wr(uint32_t reg, uint32_t value) {
        access.write(access.context, static_cast<uint32_t>(kDmuSeg2) + base + reg, value);
    }
};

int auxTransaction(AuxEngine& e, uint8_t action, uint32_t address,
                   const uint8_t* data, uint8_t len,
                   uint8_t* reply, uint8_t replyCap, uint8_t* replyBytes) {
    if (replyBytes) *replyBytes = 0;
    const uint32_t initialArb = e.rd(kArbControl);
    if (initialArb == 0xFFFFFFFFu || (initialArb & kOwnerMask) >= (2u << 2)) {
        IOLog("RDNA4Framebuffer: AUX unavailable at 0x%04x, arb=0x%08x\n",
              e.base, initialArb);
        return -1;
    }

    uint32_t control = e.rd(kAuxControl - kAuxControl);
    if (control == 0xFFFFFFFFu) return -1;
    if (!(control & kAuxEnable)) e.wr(0u, control | kAuxEnable);
    e.wr(kArbControl, e.rd(kArbControl) | kUseRequest);
    const uint32_t arb = e.rd(kArbControl);
    if ((arb & kOwnerMask) != kOwnerSoftware) {
        e.wr(kArbControl, (e.rd(kArbControl) & ~kUseRequest) | kDoneUsing);
        IOLog("RDNA4Framebuffer: AUX 0x%04x not granted, arb=0x%08x\n", e.base, arb);
        return -1;
    }

    int result = -1;
    do {
        e.wr(kInterruptControl, kDoneAck);
        bool idle = false;
        for (uint32_t i = 0; i < 100u; ++i) {
            if (!(e.rd(kSwStatus) & kDone)) {
                idle = true;
                break;
            }
            IODelay(10);
        }
        if (!idle) break;

        const bool isWrite = (action & 0x10u) == 0u;
        uint32_t length = len ? 4u : 3u;
        if (isWrite) length += len;
        e.wr(kSwControl, (e.rd(kSwControl) & ~kWrittenBytesMask) |
                            (length << kWrittenBytesShift));
        const uint8_t request[5] = {
            static_cast<uint8_t>(action | ((address >> 16) & 0x0Fu)),
            static_cast<uint8_t>(address >> 8),
            static_cast<uint8_t>(address),
            static_cast<uint8_t>(len ? len - 1u : 0u),
            static_cast<uint8_t>(isWrite && len ? data[0] : 0u)
        };
        const uint32_t requestBytes = isWrite ? length : (len ? 4u : 3u);
        for (uint32_t i = 0; i < requestBytes; ++i)
            e.wr(kSwData, (i == 0u ? kAutoincrementDisable : 0u) |
                           (static_cast<uint32_t>(request[i]) << kDataShift));
        if (e.writeFailed) break;
        e.wr(kSwControl, e.rd(kSwControl) | kGo);

        uint32_t status = 0;
        bool done = false;
        for (uint32_t i = 0; i < 2000u; ++i) {
            status = e.rd(kSwStatus);
            if (status & kDone) {
                done = true;
                break;
            }
            IODelay(10);
        }
        if (!done || (status & kStatusError) || status == 0xFFFFFFFFu) {
            IOLog("RDNA4Framebuffer: AUX 0x%04x action=0x%02x status=0x%08x done=%u\n",
                  e.base, action, status, done ? 1u : 0u);
            break;
        }

        const uint32_t nbytes = (status >> kReplyCountShift) & 0x1Fu;
        if (!nbytes || nbytes - 1u > kMaxChunk) break;
        e.wr(kSwData, kAutoincrementDisable | kDataRead);
        const uint32_t header = (e.rd(kSwData) & kDataMask) >> kDataShift;
        const uint32_t dataBytes = nbytes - 1u;
        for (uint32_t i = 0; i < dataBytes; ++i) {
            const uint32_t byte = (e.rd(kSwData) & kDataMask) >> kDataShift;
            if (reply && i < replyCap) reply[i] = static_cast<uint8_t>(byte);
        }
        if (replyBytes) *replyBytes = static_cast<uint8_t>(dataBytes);
        result = static_cast<int>((header >> 4) & 0x0Fu);
    } while (false);

    e.wr(kArbControl, (e.rd(kArbControl) & ~kUseRequest) | kDoneUsing);
    return result;
}

bool auxReadChunk(AuxEngine& e, uint8_t action, uint32_t address,
                  const uint8_t* data, uint8_t len,
                  uint8_t* reply, uint8_t* replyBytes) {
    for (uint32_t attempt = 0; attempt < kRetries; ++attempt) {
        const int rc = auxTransaction(e, action, address, data, len,
                                      reply, len, replyBytes);
        if (rc == kAck) return true;
        if (rc < 0) return false;
        if (rc != kAuxDefer && rc != kI2cDefer) return false;
        IODelay(500);
    }
    return false;
}

} // namespace

bool rdna4fbAuxReadBlockWithAccess(const RDNA4FBEdidAccess& access,
                                   uint32_t instance, uint8_t start, uint8_t out[128]) {
    if (!access.read || !access.write || !out || instance > 1u) return false;
    using namespace Aux;
    AuxEngine e { access, kAuxControl + instance * kAuxStride };
    uint8_t received = 0;
    if (!auxReadChunk(e, kI2cWriteMot, kEdidAddress, &start, 1, nullptr, &received))
        return false;

    for (uint32_t pos = 0; pos < 128u;) {
        const uint8_t len = static_cast<uint8_t>(128u - pos > kMaxChunk ? kMaxChunk : 128u - pos);
        const uint8_t action = pos + len == 128u ? kI2cRead : kI2cReadMot;
        if (!auxReadChunk(e, action, kEdidAddress, nullptr, len, out + pos, &received) ||
            received != len) {
            IOLog("RDNA4Framebuffer: AUX%u EDID block failed at byte %u (received %u of %u)\n",
                  instance, pos, received, len);
            return false;
        }
        pos += received;
    }
    return true;
}
