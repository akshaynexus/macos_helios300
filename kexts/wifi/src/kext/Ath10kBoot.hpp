#pragma once

#include "Ath10kTarget.hpp"
#include "Ath10kHTC.hpp"

namespace Ath10k {

// targaddrs.h host_interest offsets; wmi.h HTC_PROTOCOL_VERSION.
static constexpr UInt32 kBootHIApp = kHostInterestBaseAddress + 0x00;
static constexpr UInt32 kBootHIOption = kHostInterestBaseAddress + 0x10;
static constexpr UInt32 kBootHISerial = kHostInterestBaseAddress + 0x14;
static constexpr UInt32 kBootHISkipClock = kHostInterestBaseAddress + 0x20;
static constexpr UInt32 kBootHIHostBE = kHostInterestBaseAddress + 0x44;
static constexpr UInt32 kBootHIPCIId = kHostInterestBaseAddress + 0xbc;
static constexpr UInt32 kBootHIFWSwap = kHostInterestBaseAddress + 0x104;
static constexpr UInt32 kBootHTCProtocolVersion = 2;
// Match ath10k_init_configure_target(): num_device=1, FW_MODE_AP=2,
// MAC_ADDR_METHOD=1, preserving the other ROM bits. This does not create an AP.
static constexpr UInt32 kBootHIOptionBits = (1u << 9) | (2u << 12) | (1u << 3);
// Linux targaddrs.h HI_OPTION_BMI_CRED_LIMIT. Hardware testing of 0.0.20
// observed this flag become set after OTP/WLAN loading (0x2208 -> 0x220a).
// Permit that addition only at the final check; preserve the target's value.
static constexpr UInt32 kBootHIOptionBMICreditLimit = 0x00000002;

inline bool bootHIOptionMatchesAfterDownload(UInt32 applied, UInt32 observed)
{
    return applied != 0xffffffffu && observed != 0xffffffffu &&
        (observed == applied || observed == (applied | kBootHIOptionBMICreditLimit));
}

inline bool isBootHIWord(UInt32 address)
{
    return address == kBootHIApp || address == kBootHIOption ||
        address == kBootHISerial || address == kBootHISkipClock ||
        address == kBootHIHostBE || address == kBootHIPCIId || address == kBootHIFWSwap;
}

struct BootState {
    bool configAttempted = false;
    bool configSucceeded = false;
    bool downloadConfigAttempted = false;
    bool downloadConfigSucceeded = false;
    bool preDoneHIVerified = false;
    bool uartDisabled = false;
    bool attempted = false;
    bool bmiClosed = false; // Set before the DONE doorbell, including failure.
    bool doneAttempted = false;
    bool doneSent = false; // CE0 consumption, not proof of firmware startup.
    bool rxPosted = false;
    bool rxOwned = false; // Retained until verified reset, including RX failure.
    bool rxCompleted = false;
    bool readyValidated = false;
    bool timedOut = false;
    bool firmwareCrash = false;
    UInt32 optionBefore = 0xffffffffu;
    UInt32 optionApplied = 0xffffffffu;
    bool optionFinalRead = false;
    bool optionFinalVerified = false;
    bool optionBMICreditLimitAdded = false;
    UInt32 optionAfterDownload = 0xffffffffu;
    UInt32 optionChangedBits = 0;
    UInt32 hiWrites = 0;
    UInt32 hiVerified = 0;
    UInt32 lastHIAddress = 0xffffffffu;
    UInt32 lastHIExpected = 0xffffffffu;
    UInt32 lastHIObserved = 0xffffffffu;
    UInt32 rxSlot = 0xffffffffu;
    UInt32 rxNext = 0xffffffffu;
    UInt32 rxRead = 0xffffffffu;
    UInt32 rxWrite = 0xffffffffu;
    UInt32 rxBytes = 0;
    UInt32 waitMs = 0;
    UInt32 fwIndicator = 0xffffffffu;
    UInt8 prefix[32]{};
    UInt32 prefixBytes = 0;
    HTCReadyInfo ready{};
    const char *status = "not-run";
    const char *parseError = "none";
};

} // namespace Ath10k
