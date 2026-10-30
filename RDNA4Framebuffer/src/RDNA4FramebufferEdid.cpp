//
//  RDNA4FramebufferEdid.cpp
//  DC_I2C EDID read for RDNA4Framebuffer.
//

#include "RDNA4FramebufferEdid.hpp"
#include <IOKit/IOLib.h>
#include <string.h>

namespace {

// DMU segment bases in dwords (dcn41_core.c): the I2C engine lives in
// segment 2, the microsecond time-base divider in segment 1 (DCCG).
constexpr uint32_t kSeg2 = 0x34C0u;
constexpr uint32_t kSeg1 = 0x00C0u;

constexpr uint32_t kI2cControl = 0x1E98u;
constexpr uint32_t kI2cArbitration = 0x1E99u;
constexpr uint32_t kI2cSwStatus = 0x1E9Bu;
constexpr uint32_t kI2cDdc1Speed = 0x1EA2u;
constexpr uint32_t kI2cDdc1Setup = 0x1EA3u;
constexpr uint32_t kI2cTransaction0 = 0x1EAEu;
constexpr uint32_t kI2cData = 0x1EB2u;
constexpr uint32_t kDioMemPwrStatus = 0x1EDDu;
constexpr uint32_t kDioMemPwrCtrl = 0x1EDEu;
constexpr uint32_t kMicrosecondTimeBaseDiv = 0x007Bu;

constexpr uint32_t kCtlGo = 1u << 0;
constexpr uint32_t kCtlSoftReset = 1u << 1;
constexpr uint32_t kCtlSwStatusReset = 1u << 3;
constexpr uint32_t kCtlDdcSelectShift = 8u;
constexpr uint32_t kCtlTxnCountShift = 20u;
constexpr uint32_t kArbRwStatusMask = 3u << 2;
constexpr uint32_t kArbRwStatusShift = 2u;
constexpr uint32_t kArbNoQueuedSwGo = 1u << 4;
constexpr uint32_t kArbSwUseReq = 1u << 20;
constexpr uint32_t kArbSwDoneUsing = 1u << 21;
constexpr uint32_t kStDone = 1u << 2;
constexpr uint32_t kStAborted = 1u << 4;
constexpr uint32_t kStTimeout = 1u << 5;
constexpr uint32_t kStOverflow = 1u << 7;
constexpr uint32_t kStNack = 1u << 8;
constexpr uint32_t kSetupClkEn = 1u << 3;
constexpr uint32_t kSetupEnable = 1u << 6;
constexpr uint32_t kSetupValue = kSetupEnable | kSetupClkEn | (1u << 2) | (3u << 24);
constexpr uint32_t kTxnRead = 1u << 0;
constexpr uint32_t kTxnStopOnNack = 1u << 8;
constexpr uint32_t kTxnStart = 1u << 12;
constexpr uint32_t kTxnStop = 1u << 13;
constexpr uint32_t kTxnCountShift = 16u;
constexpr uint32_t kDataRead = 1u << 0;
constexpr uint32_t kDataShift = 8u;
constexpr uint32_t kDataIndexShift = 16u;
constexpr uint32_t kDataIndexWrite = 1u << 31;

constexpr uint32_t kEdidBlock = 128u;
constexpr uint32_t kDdcAddrEdidW = 0xA0u;
constexpr uint32_t kDdcAddrEdidR = 0xA1u;
constexpr uint32_t kDdcAddrSegW = 0x60u;
constexpr uint32_t kDdcMaxTxn = 3u;
constexpr uint32_t kDdcPollStepUs = 10u;
constexpr uint32_t kDdcPollPerTxn = 2000u;
constexpr uint32_t kDdcFifoMax = 8u;

enum Status : uint32_t {
    kOk = 0,
    kBadArg,
    kEngineDead,
    kHwOwned,
    kSwBusy,
    kNotGranted,
    kTimeout,
    kAborted,
    kOverflow,
    kNack,
};

struct Txn {
    uint8_t addr8;
    uint8_t isRead;
    uint8_t wlen;
    uint8_t wdata;
    uint16_t rlen;
    uint8_t stop;
};

struct Plan {
    uint8_t ntxn;
    uint8_t offset;
    uint8_t fifoLen;
    uint8_t fifo[kDdcFifoMax];
    uint8_t readIndex;
    Txn t[kDdcMaxTxn];
};

struct Env {
    volatile uint32_t* mmio;
    uint64_t mmioBytes;
    bool writeFailed;
    bool canAccess(uint32_t baseIdx, uint32_t off) const {
        const uint64_t index = (baseIdx == 2u ? kSeg2 : kSeg1) + static_cast<uint64_t>(off);
        return mmio && index < mmioBytes / sizeof(uint32_t);
    }
    uint32_t rd(uint32_t baseIdx, uint32_t off) {
        if (!canAccess(baseIdx, off)) return 0xFFFFFFFFu;
        const uint32_t base = baseIdx == 2u ? kSeg2 : kSeg1;
        return mmio[base + off];
    }
    void wr2(uint32_t off, uint32_t v) {
        if (!canAccess(2u, off)) {
            writeFailed = true;
            return;
        }
        mmio[kSeg2 + off] = v;
    }
    void delayUs(uint32_t us) { IODelay(us); }
};

uint32_t arbEntry(uint32_t arb) {
    if (arb == 0xFFFFFFFFu) return kEngineDead;
    const uint32_t owner = (arb & kArbRwStatusMask) >> kArbRwStatusShift;
    if (owner == 2u) return kHwOwned;
    if (owner != 0u) return kSwBusy;
    return kOk;
}

bool arbGranted(uint32_t arb) {
    return arb != 0xFFFFFFFFu &&
           ((arb & kArbRwStatusMask) >> kArbRwStatusShift) == 1u;
}

int swStatusVerdict(uint32_t sts) {
    if (sts & kStNack) return kNack;
    if (sts & kStTimeout) return kTimeout;
    if (sts & kStAborted) return kAborted;
    if (sts & kStOverflow) return kOverflow;
    if (sts & kStDone) return 0;
    return -1;
}

uint32_t planDdc(uint32_t block, Plan* p) {
    memset(p, 0, sizeof(*p));
    if (block > 3u) return kBadArg;
    unsigned k = 0;
    p->offset = static_cast<uint8_t>((block * kEdidBlock) & 0xFFu);
    if (block >= 2u) {
        p->t[k].addr8 = kDdcAddrSegW;
        p->t[k].wlen = 1u;
        p->t[k].wdata = static_cast<uint8_t>(block / 2u);
        k++;
    }
    p->t[k].addr8 = kDdcAddrEdidW;
    p->t[k].wlen = 1u;
    p->t[k].wdata = p->offset;
    k++;
    p->t[k].addr8 = kDdcAddrEdidR;
    p->t[k].isRead = 1u;
    p->t[k].rlen = kEdidBlock;
    p->t[k].stop = 1u;
    k++;
    p->ntxn = static_cast<uint8_t>(k);
    for (unsigned i = 0; i < k; i++) {
        p->fifo[p->fifoLen++] = p->t[i].addr8;
        if (!p->t[i].isRead && p->t[i].wlen) p->fifo[p->fifoLen++] = p->t[i].wdata;
    }
    p->readIndex = p->fifoLen;
    return kOk;
}

uint32_t txnReg(const Txn* t) {
    uint32_t v = kTxnStopOnNack | kTxnStart;
    const uint32_t cnt = t->isRead ? t->rlen : t->wlen;
    if (t->isRead) v |= kTxnRead;
    if (t->stop) v |= kTxnStop;
    return v | (cnt << kTxnCountShift);
}

// One EDID block over the given DDC line.  The engine is acquired only if
// the arbitration word says it is free, and it is always released again.
uint32_t i2cXfer(Env& e, uint32_t line, const Plan& p, uint8_t out[kEdidBlock]) {
    const uint32_t rSetup = kI2cDdc1Setup + 2u * line;
    const uint32_t rSpeed = kI2cDdc1Speed + 2u * line;

    uint32_t arb = e.rd(2u, kI2cArbitration);
    uint32_t v = arbEntry(arb);
    if (v != kOk) return v;

    e.wr2(kI2cControl, 0u);                       // deassert soft reset
    const uint32_t memPwr = e.rd(2u, kDioMemPwrCtrl);
    if (memPwr != 0xFFFFFFFFu && (memPwr & 1u)) {
        e.wr2(kDioMemPwrCtrl, memPwr & ~1u);      // I2C RAM out of light sleep
        for (int i = 0; i < 10 && (e.rd(2u, kDioMemPwrStatus) & 1u); i++) e.delayUs(1u);
    }
    e.wr2(rSetup, e.rd(2u, rSetup) | kSetupClkEn);

    arb = e.rd(2u, kI2cArbitration);
    v = arbEntry(arb);
    if (v != kOk) return v;
    e.wr2(kI2cArbitration, arb | kArbSwUseReq);
    arb = e.rd(2u, kI2cArbitration);
    if (!arbGranted(arb)) {
        e.wr2(kI2cArbitration, e.rd(2u, kI2cArbitration) | kArbSwDoneUsing);
        return kNotGranted;
    }

    uint32_t status = kOk;
    do {
        e.wr2(rSetup, kSetupValue);
        e.wr2(kI2cControl, kCtlSwStatusReset | (line << kCtlDdcSelectShift));
        e.wr2(kI2cArbitration,
              (e.rd(2u, kI2cArbitration) | kArbSwUseReq) & ~kArbNoQueuedSwGo);

        const uint32_t mtb = e.rd(1u, kMicrosecondTimeBaseDiv);
        const uint32_t refBase = mtb & 0x7Fu;
        uint32_t xtalDiv = (mtb >> 8) & 0x7Fu;
        if (mtb != 0xFFFFFFFFu && refBase != 0u) {
            if (xtalDiv == 0u) xtalDiv = 2u;
            const uint32_t prescale = (refBase * 1000u / xtalDiv) / 100u;   // 100 kHz
            e.wr2(rSpeed, (prescale << 16) | (2u << 8) | 2u);
        }

        for (unsigned i = 0; i < p.ntxn; i++)
            e.wr2(kI2cTransaction0 + i, txnReg(&p.t[i]));
        for (unsigned i = 0; i < p.fifoLen; i++)
            e.wr2(kI2cData, (i == 0u ? kDataIndexWrite : 0u) |
                              (static_cast<uint32_t>(p.fifo[i]) << kDataShift));

        const uint32_t ctl = (line << kCtlDdcSelectShift) |
                             (static_cast<uint32_t>(p.ntxn - 1u) << kCtlTxnCountShift);
        e.wr2(kI2cControl, ctl);
        e.wr2(kI2cControl, ctl | kCtlGo);

        bool finished = false;
        for (uint32_t i = 0; i < p.ntxn * kDdcPollPerTxn; i++) {
            const int vd = swStatusVerdict(e.rd(2u, kI2cSwStatus));
            if (vd > 0) { status = static_cast<uint32_t>(vd); finished = true; break; }
            if (vd == 0) { finished = true; break; }
            e.delayUs(kDdcPollStepUs);
        }
        if (!finished) { status = kTimeout; break; }
        if (status != kOk) break;

        e.wr2(kI2cData, kDataIndexWrite | kDataRead |
                          (static_cast<uint32_t>(p.readIndex) << kDataIndexShift));
        for (unsigned i = 0; i < kEdidBlock; i++)
            out[i] = static_cast<uint8_t>((e.rd(2u, kI2cData) >> kDataShift) & 0xFFu);
    } while (0);

    e.wr2(kI2cControl, kCtlSoftReset | kCtlSwStatusReset);
    e.wr2(rSetup, 0u);
    e.wr2(kI2cArbitration, e.rd(2u, kI2cArbitration) | kArbSwDoneUsing);
    if (e.writeFailed && status == kOk) status = kEngineDead;
    return status;
}

bool edidHeaderOk(const uint8_t* b) {
    return b[0] == 0u && b[1] == 0xFFu && b[2] == 0xFFu && b[3] == 0xFFu &&
           b[4] == 0xFFu && b[5] == 0xFFu && b[6] == 0xFFu && b[7] == 0u;
}

uint32_t edidSum(const uint8_t* b) {
    uint32_t s = 0;
    for (unsigned i = 0; i < kEdidBlock; i++) s += b[i];
    return s & 0xFFu;
}

} // namespace

bool rdna4fbReadEdid(volatile uint32_t* mmio, uint64_t mmioBytes,
                     uint8_t edid[256], uint32_t* length, uint32_t* line) {
    constexpr uint64_t requiredDwords = kSeg2 + kDioMemPwrCtrl + 1u;
    if (!mmio || !edid || !length || !line ||
        mmioBytes < requiredDwords * sizeof(uint32_t)) return false;
    Env env { mmio, mmioBytes, false };
    for (uint32_t ln = 2u; ln <= 3u; ln++) {
        Plan plan;
        if (planDdc(0, &plan) != kOk) return false;
        const uint32_t st = i2cXfer(env, ln, plan, edid);
        if (st != kOk) {
            IOLog("RDNA4Framebuffer: EDID read on DDC%u failed (status %u)\n", ln, st);
            continue;
        }
        if (!edidHeaderOk(edid) || edidSum(edid) != 0u) {
            IOLog("RDNA4Framebuffer: DDC%u returned data that is not a valid EDID base block\n", ln);
            continue;
        }
        *line = ln;
        *length = kEdidBlock;
        // An extension block, if the monitor offers one, carries the extra
        // timing and the monitor name in many panels.
        if (edid[126] != 0u) {
            Plan ext;
            if (planDdc(1, &ext) == kOk) {
                uint8_t block1[kEdidBlock];
                if (i2cXfer(env, ln, ext, block1) == kOk && edidSum(block1) == 0u) {
                    memcpy(edid + kEdidBlock, block1, kEdidBlock);
                    *length = 2u * kEdidBlock;
                }
            }
        }
        IOLog("RDNA4Framebuffer: EDID read on DDC%u (%u bytes)\n", ln, *length);
        return true;
    }
    return false;
}
