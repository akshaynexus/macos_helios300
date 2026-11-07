#pragma once

#include <IOKit/IOTypes.h>
#include "Ath10kRegisterIO.hpp"
#include "Ath10kTarget.hpp"

namespace Ath10k {

// qca6174_values.ce_count from Linux ath10k. QCA9377 uses the same
// qca6174 register/value tables on PCIe.
static constexpr UInt32 kCECount = 8;
static constexpr UInt32 kCEBaseStride = 0x00000400;
static constexpr UInt32 kCEDescriptorSize = 8;
static constexpr UInt32 kCEDescriptorRingAlign = 8;
static constexpr UInt32 kCEHTTH2TMsgSrcNEntries = 8192;
// Linux ath10k pci.h DIAG_TRANSFER_LIMIT. This is the CE7 host
// src_sz_max and the per-transfer CE diagnostic chunk size. Do not confuse it
// with the separate ATH10K_DIAG_TRANSFER_LIMIT (0x5000) in pci.c.
static constexpr UInt32 kCEDiagTransferLimit = 2048;

// Linux ath10k pci.h diagnostic CE polling constants.
static constexpr UInt32 kCEDiagAccessTimeoutUs = 10000;
static constexpr UInt32 kCEDiagAccessWaitUs    = 50;

// QCA6174/QCA9377 use the legacy 8-byte CE descriptor format.
struct CEDescriptor32 {
    UInt32 address;
    UInt16 nbytes;
    UInt16 flags;
};
static_assert(sizeof(CEDescriptor32) == kCEDescriptorSize,
              "QCA6174 CE descriptor must remain 8 bytes");

inline UInt32 ceRingNext(UInt32 index, UInt32 entries)
{
    return (index + 1u) & (entries - 1u);
}

// qcax_ce_regs offsets (relative to each CE base).
static constexpr UInt32 kCESrcRingBaseLo    = 0x00000000;
static constexpr UInt32 kCESrcRingSize      = 0x00000004;
static constexpr UInt32 kCEDstRingBaseLo    = 0x00000008;
static constexpr UInt32 kCEDstRingSize      = 0x0000000c;
static constexpr UInt32 kCECtrl1            = 0x00000010;
static constexpr UInt32 kCECommand          = 0x00000018;
static constexpr UInt32 kCEHostIE           = 0x0000002c;
static constexpr UInt32 kCEHostIS           = 0x00000030; // qcax W1C copy/watermark status
static constexpr UInt32 kCEMiscIE           = 0x00000034;
static constexpr UInt32 kCEMiscIS           = 0x00000038; // qcax W1C error status; diagnostic reads never clear it
static constexpr UInt32 kCESrcWriteIndex    = 0x0000003c;
static constexpr UInt32 kCEDstWriteIndex    = 0x00000040;
static constexpr UInt32 kCECurrentSrcRead   = 0x00000044;
static constexpr UInt32 kCECurrentDstRead   = 0x00000048;
static constexpr UInt32 kCESrcWatermark     = 0x0000004c;
static constexpr UInt32 kCEDstWatermark     = 0x00000050;

// qcax_ctrl1 fields.
static constexpr UInt32 kCECtrl1DmaxMask        = 0x0000ffff;
static constexpr UInt32 kCECtrl1SrcByteSwapMask = 0x00010000;
static constexpr UInt32 kCECtrl1DstByteSwapMask = 0x00020000;
static constexpr UInt32 kCECtrl1SoftwareMask    = 0x0007ffff;

// qcax interrupt-enable fields. Linux ath10k leaves CE7 alone because it is
// polling-only; for the other CEs irq_disable clears these masks.
static constexpr UInt32 kCEHostInterruptMask = 0x0000001f;
static constexpr UInt32 kCEMiscErrorMask     = 0x000007e0;
// On qcax the copy-complete bit is bit 0; bits 1..4 are watermark sources.
// Linux enables only copy-complete for active send/receive callbacks and
// keeps watermark interrupts disabled.
static constexpr UInt32 kCEHostCopyCompleteMask = 0x00000001;
static constexpr UInt32 kCEHostWatermarkMask    = 0x0000001e;

// qcax src/dst ring watermark fields: high mark in [15:0], low mark in [31:16].
static constexpr UInt32 kCEWatermarkHighMask = 0x0000ffff;
static constexpr UInt32 kCEWatermarkLowMask  = 0xffff0000;

struct CEHostConfig {
    UInt32 srcEntries;
    UInt32 srcSizeMax;
    UInt32 dstEntries;
    bool disableInterrupt;
    bool poll;
    const char *role;
};

// QCA6174/QCA9377 effective host configuration. Linux starts from
// pci_host_ce_config_wlan[] and overrides CE5 to unused for both chips.
static constexpr CEHostConfig kCEHostConfig[kCECount] = {
    {16,                         256, 0,   false, false, "HTC control H2T"},
    {0,                         2048, 512, false, false, "HTT+HTC T2H"},
    {0,                         2048, 128, false, false, "WMI T2H"},
    {32,                        2048, 0,   false, false, "WMI H2T"},
    {kCEHTTH2TMsgSrcNEntries,    256, 0,   true,  false, "HTT H2T"},
    {0,                            0, 0,   false, false, "unused on QCA6174/QCA9377"},
    {0,                            0, 0,   false, false, "target autonomous"},
    {2,          kCEDiagTransferLimit, 2, false, true,  "diagnostic window"},
};

struct CESnapshot {
    UInt32 id = 0;
    UInt32 base = 0;
    UInt32 srcBase = 0xffffffffu;
    UInt32 srcSize = 0xffffffffu;
    UInt32 dstBase = 0xffffffffu;
    UInt32 dstSize = 0xffffffffu;
    UInt32 ctrl1 = 0xffffffffu;
    UInt32 command = 0xffffffffu;
    UInt32 hostIE = 0xffffffffu;
    UInt32 miscIE = 0xffffffffu;
    UInt32 srcWriteIndex = 0xffffffffu;
    UInt32 dstWriteIndex = 0xffffffffu;
    UInt32 currentSrcRead = 0xffffffffu;
    UInt32 currentDstRead = 0xffffffffu;
    UInt32 srcWatermark = 0xffffffffu;
    UInt32 dstWatermark = 0xffffffffu;
    bool valid = false;
};

inline UInt32 ceBaseAddress(UInt32 ceId)
{
    return kQca6174RegisterLayout.ce0BaseAddress + kCEBaseStride * ceId;
}

inline bool readCESnapshot(const Ath10kRegisterIO &regs,
                           UInt32 ceId,
                           CESnapshot &out)
{
    if (ceId >= kCECount)
        return false;

    out = CESnapshot{};
    out.id = ceId;
    out.base = ceBaseAddress(ceId);

    bool ok = true;
    ok &= regs.read32(out.base + kCESrcRingBaseLo, out.srcBase);
    ok &= regs.read32(out.base + kCESrcRingSize, out.srcSize);
    ok &= regs.read32(out.base + kCEDstRingBaseLo, out.dstBase);
    ok &= regs.read32(out.base + kCEDstRingSize, out.dstSize);
    ok &= regs.read32(out.base + kCECtrl1, out.ctrl1);
    ok &= regs.read32(out.base + kCECommand, out.command);
    ok &= regs.read32(out.base + kCEHostIE, out.hostIE);
    ok &= regs.read32(out.base + kCEMiscIE, out.miscIE);
    ok &= regs.read32(out.base + kCESrcWriteIndex, out.srcWriteIndex);
    ok &= regs.read32(out.base + kCEDstWriteIndex, out.dstWriteIndex);
    ok &= regs.read32(out.base + kCECurrentSrcRead, out.currentSrcRead);
    ok &= regs.read32(out.base + kCECurrentDstRead, out.currentDstRead);
    ok &= regs.read32(out.base + kCESrcWatermark, out.srcWatermark);
    ok &= regs.read32(out.base + kCEDstWatermark, out.dstWatermark);

    out.valid = ok;
    return ok;
}

inline bool isPowerOfTwo(UInt32 value)
{
    return value != 0 && (value & (value - 1u)) == 0;
}

inline UInt32 watermarkValue(UInt32 low, UInt32 high)
{
    return ((low & 0xffffu) << 16) | (high & 0xffffu);
}

} // namespace Ath10k
