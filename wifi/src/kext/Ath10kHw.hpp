#pragma once

#include <IOKit/IOTypes.h>

namespace Ath10k {

static constexpr UInt16 kAtherosVendor = 0x168c;
static constexpr UInt16 kQCA6174Device = 0x003e;
static constexpr UInt16 kQCA9377Device = 0x0042;

// Target chip revisions used by Linux ath10k for the supported PCI IDs.
static constexpr UInt8 kQCA6174Hw21ChipRev = 4;
static constexpr UInt8 kQCA6174Hw22ChipRev = 5;
static constexpr UInt8 kQCA6174Hw30ChipRev = 8;
static constexpr UInt8 kQCA6174Hw31ChipRev = 9;
static constexpr UInt8 kQCA6174Hw32ChipRev = 10;
static constexpr UInt8 kQCA9377Hw10ChipRev = 0;
static constexpr UInt8 kQCA9377Hw11ChipRev = 1;

struct HwCandidate {
    UInt16 deviceId;
    UInt8 chipRevision;
    const char *chipName;
    const char *targetRevision;
    const char *firmwareDirectory;
    UInt32 calibrationDataLength;
    UInt32 targetCpuFrequencyHz;
    bool firmwareDiagCeDownload;
};

// Host-side subset of Linux ath10k hw_params that is useful before firmware
// loading. QCA6174 and QCA9377 share qca6174_regs/qca6174_values in Linux.
static constexpr HwCandidate kCandidates[] = {
    { kQCA6174Device, kQCA6174Hw21ChipRev, "Qualcomm Atheros QCA6174", "hw2.1", "QCA6174/hw2.1", 8124, 0, false },
    { kQCA6174Device, kQCA6174Hw22ChipRev, "Qualcomm Atheros QCA6174", "hw2.2", "QCA6174/hw2.1", 8124, 0, false },
    { kQCA6174Device, kQCA6174Hw30ChipRev, "Qualcomm Atheros QCA6174", "hw3.0", "QCA6174/hw3.0", 8124, 0, false },
    { kQCA6174Device, kQCA6174Hw31ChipRev, "Qualcomm Atheros QCA6174", "hw3.1", "QCA6174/hw3.0", 8124, 0, false },
    { kQCA6174Device, kQCA6174Hw32ChipRev, "Qualcomm Atheros QCA6174", "hw3.2", "QCA6174/hw3.0", 8124, 176000000, true },
    { kQCA9377Device, kQCA9377Hw10ChipRev, "Qualcomm Atheros QCA9377", "hw1.0", "QCA9377/hw1.0", 8124, 0, false },
    { kQCA9377Device, kQCA9377Hw11ChipRev, "Qualcomm Atheros QCA9377", "hw1.1", "QCA9377/hw1.0", 8124, 176000000, true },
};

inline const char *chipNameForDevice(UInt16 deviceId)
{
    switch (deviceId) {
        case kQCA6174Device: return "Qualcomm Atheros QCA6174";
        case kQCA9377Device: return "Qualcomm Atheros QCA9377";
        default: return "Qualcomm Atheros ath10k";
    }
}

inline bool isSupportedDevice(UInt16 vendorId, UInt16 deviceId)
{
    return vendorId == kAtherosVendor &&
           (deviceId == kQCA6174Device || deviceId == kQCA9377Device);
}

inline const HwCandidate *findCandidate(UInt16 deviceId, UInt8 chipRevision)
{
    for (const auto &candidate : kCandidates) {
        if (candidate.deviceId == deviceId &&
            candidate.chipRevision == chipRevision)
            return &candidate;
    }
    return nullptr;
}

// Mirrors ath10k_pci_get_num_banks() for the revisions this project matches.
// Zero means the revision is not known well enough for semantic Host Interest
// writes and the transactional init_config test must not proceed.
inline UInt32 targetIRAMBanks(UInt16 deviceId, UInt8 chipRevision)
{
    if (deviceId == kQCA6174Device) {
        switch (chipRevision) {
            case kQCA6174Hw21ChipRev:
            case kQCA6174Hw22ChipRev:
                return 3;
            case kQCA6174Hw30ChipRev:
            case kQCA6174Hw31ChipRev:
            case kQCA6174Hw32ChipRev:
                return 9;
            default:
                return 0;
        }
    }

    if (deviceId == kQCA9377Device) {
        switch (chipRevision) {
            case kQCA9377Hw10ChipRev:
            case kQCA9377Hw11ChipRev:
                return 9;
            default:
                return 0;
        }
    }

    return 0;
}

} // namespace Ath10k
