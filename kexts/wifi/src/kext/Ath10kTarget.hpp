#pragma once

#include <IOKit/IOTypes.h>

namespace Ath10k {

struct TargetRegisterLayout {
    UInt32 rtcSocBaseAddress;
    UInt32 socCoreBaseAddress;
    UInt32 ceWrapperBaseAddress;
    UInt32 ce0BaseAddress;
    UInt32 ce1BaseAddress;
    UInt32 ce2BaseAddress;
    UInt32 ce3BaseAddress;
    UInt32 ce4BaseAddress;
    UInt32 ce5BaseAddress;
    UInt32 ce6BaseAddress;
    UInt32 ce7BaseAddress;
    UInt32 socChipIdAddress;
    UInt32 fwIndicatorAddress;
    UInt32 pcieLocalBaseAddress;
    UInt32 rtcStateValueOn;
};

static constexpr TargetRegisterLayout kQca6174RegisterLayout = {
    0x00000800, // RTC_SOC_BASE_ADDRESS
    0x0003a000, // SOC_CORE_BASE_ADDRESS
    0x00034000, // CE_WRAPPER_BASE_ADDRESS
    0x00034400, // CE0_BASE_ADDRESS
    0x00034800, // CE1_BASE_ADDRESS
    0x00034c00, // CE2_BASE_ADDRESS
    0x00035000, // CE3_BASE_ADDRESS
    0x00035400, // CE4_BASE_ADDRESS
    0x00035800, // CE5_BASE_ADDRESS
    0x00035c00, // CE6_BASE_ADDRESS
    0x00036000, // CE7_BASE_ADDRESS
    0x000000f0, // SOC_CHIP_ID_ADDRESS within RTC_SOC
    0x0003a028, // FW_INDICATOR_ADDRESS
    0x00080000, // PCIE_LOCAL_BASE_ADDRESS
    3           // RTC_STATE_V_ON
};

static constexpr UInt32 kRtcStateAddress       = 0x0000;
static constexpr UInt32 kRtcStateMask          = 0x00000007;
static constexpr UInt32 kPcieSocWakeAddress    = 0x0004;
static constexpr UInt32 kPcieSocWakeMask       = 0x00000001;
static constexpr UInt32 kPcieSocWakeReset      = 0x00000000;
static constexpr UInt32 kSocGlobalResetAddress = 0x0008;
static constexpr UInt32 kSocGlobalResetMask    = 0x00000001;

// RTC_SOC reset/warm-reset registers used by qca6174_regs.
static constexpr UInt32 kSocResetControlAddress      = 0x00000000;
static constexpr UInt32 kSocResetControlSI0ResetMask = 0x00000000; // QCA6174/QCA9377
static constexpr UInt32 kSocResetControlCEResetMask  = 0x00000001;
static constexpr UInt32 kSocResetControlCPUWarmMask  = 0x00000040;
static constexpr UInt32 kSocLFTimerControl0Address   = 0x00000050;
static constexpr UInt32 kSocLFTimerEnableMask        = 0x00000004;

// SOC_CORE interrupt control, matching the qca6174 register table.
static constexpr UInt32 kCoreCtrlAddress          = 0x00000000;
static constexpr UInt32 kCoreCtrlPCIeReg31Mask    = 0x00000800;
static constexpr UInt32 kCoreCtrlCPUIntrMask       = 0x00002000;
static constexpr UInt32 kPCIeIntrEnableAddress    = 0x00000008;
static constexpr UInt32 kPCIeIntrCauseAddress     = 0x0000000c;
static constexpr UInt32 kPCIeIntrClearAddress     = 0x00000014;
static constexpr UInt32 kPCIeIntrFirmwareMask     = 0x00000400;
static constexpr UInt32 kPCIeIntrCEMaskAll        = 0x0007f800;

static constexpr UInt32 kFwIndicatorEventPending = 0x00000001;
static constexpr UInt32 kFwIndicatorInitialized  = 0x00000002;

static constexpr UInt32 kTargetInitTimeoutMs = 3000;
static constexpr UInt32 kTargetInitPollMs    = 10;
static constexpr UInt32 kColdResetDelayMs    = 20;
static constexpr UInt32 kWarmResetDelayMs    = 10;

// Target Host Interest is shared with the firmware and is explicitly intended
// for BMI/Diagnostic Window access. Linux ath10k uses the same base for QCA6174.
// hi_interconnect_state is at offset 0xf8 in struct host_interest.
static constexpr UInt32 kHostInterestBaseAddress             = 0x00400800;
static constexpr UInt32 kHostInterestDesiredCPUSpeedOffset   = 0x00000050;
static constexpr UInt32 kHostInterestBoardDataOffset         = 0x00000054;
static constexpr UInt32 kHostInterestBoardDataInitOffset     = 0x00000058;
static constexpr UInt32 kHostInterestBoardExtDataOffset      = 0x000000ac;
static constexpr UInt32 kHostInterestBoardExtConfigOffset    = 0x000000b0;
static constexpr UInt32 kHostInterestOptionFlag2Offset       = 0x000000cc;
static constexpr UInt32 kHostInterestCalDataOffset           = 0x000000e4;
static constexpr UInt32 kHostInterestInterconnectOffset      = 0x000000f8;
static constexpr UInt32 kHostInterestEarlyAllocOffset        = 0x00000100;

static constexpr UInt32 kHostInterestDesiredCPUSpeedAddress =
    kHostInterestBaseAddress + kHostInterestDesiredCPUSpeedOffset;
static constexpr UInt32 kHostInterestBoardDataAddress =
    kHostInterestBaseAddress + kHostInterestBoardDataOffset;
static constexpr UInt32 kHostInterestBoardDataInitAddress =
    kHostInterestBaseAddress + kHostInterestBoardDataInitOffset;
static constexpr UInt32 kHostInterestBoardExtDataAddress =
    kHostInterestBaseAddress + kHostInterestBoardExtDataOffset;
static constexpr UInt32 kHostInterestBoardExtConfigAddress =
    kHostInterestBaseAddress + kHostInterestBoardExtConfigOffset;
static constexpr UInt32 kHostInterestOptionFlag2Address =
    kHostInterestBaseAddress + kHostInterestOptionFlag2Offset;
static constexpr UInt32 kHostInterestCalDataAddress =
    kHostInterestBaseAddress + kHostInterestCalDataOffset;
static constexpr UInt32 kHostInterestInterconnectAddress =
    kHostInterestBaseAddress + kHostInterestInterconnectOffset;
static constexpr UInt32 kHostInterestEarlyAllocAddress =
    kHostInterestBaseAddress + kHostInterestEarlyAllocOffset;

// hi_board_ext_data_config packs the extension size in bits 31:16 and a
// valid bit in bit 0. These are discovery-only in 0.0.17.
static constexpr UInt32 kHostInterestBoardExtDataValidMask = 0x00000001;
static constexpr UInt32 kHostInterestBoardExtDataSizeMask  = 0xffff0000;
static constexpr UInt32 kHostInterestBoardExtDataSizeShift = 16;

// Read-only pcie_state fields discovered by the first part of
// ath10k_pci_init_config(). All members are 32-bit quantities.
static constexpr UInt32 kPCIeStatePipeCfgOffset      = 0x00000000;
static constexpr UInt32 kPCIeStateServiceMapOffset   = 0x00000004;
static constexpr UInt32 kPCIeStateConfigFlagsOffset  = 0x00000020;
static constexpr UInt32 kPCIeConfigFlagEnableL1      = 0x00000001;

// Linux ath10k Host Interest early-configuration values. QCA6174 hw3.x uses
// nine RAM banks. The 0.0.17 milestone keeps these applied through the read-only
// BMI prerequisite probe, then quiesces the Target with reset before DMA cleanup.
static constexpr UInt32 kHIEarlyAllocMagic          = 0x00006d8a;
static constexpr UInt32 kHIEarlyAllocMagicMask      = 0xffff0000;
static constexpr UInt32 kHIEarlyAllocMagicShift     = 16;
static constexpr UInt32 kHIEarlyAllocIRAMBanksMask  = 0x0000000f;
static constexpr UInt32 kHIEarlyAllocIRAMBanksShift = 0;
static constexpr UInt32 kHIOptionEarlyCfgDone       = 0x00000010;

// Structures shared with target firmware. QCA6174 is little-endian on this
// path and the Intel macOS host is little-endian, so UInt32 fields are laid
// out exactly as Linux ce_pipe_config / ce_service_to_pipe.
struct TargetCEPipeConfig {
    UInt32 pipeNum;
    UInt32 pipeDir;
    UInt32 nEntries;
    UInt32 nBytesMax;
    UInt32 flags;
    UInt32 reserved;
};

struct TargetServiceToPipe {
    UInt32 serviceId;
    UInt32 pipeDir;
    UInt32 pipeNum;
};

static constexpr UInt32 kPipeDirNone  = 0;
static constexpr UInt32 kPipeDirIn    = 1;
static constexpr UInt32 kPipeDirOut   = 2;
static constexpr UInt32 kPipeDirInOut = 3;

static constexpr UInt32 kSvcRsvdCtrl   = 0x0001;
static constexpr UInt32 kSvcWMIControl = 0x0100;
static constexpr UInt32 kSvcWMIDataBE  = 0x0101;
static constexpr UInt32 kSvcWMIDataBK  = 0x0102;
static constexpr UInt32 kSvcWMIDataVI  = 0x0103;
static constexpr UInt32 kSvcWMIDataVO  = 0x0104;
static constexpr UInt32 kSvcHTTData    = 0x0300;
static constexpr UInt32 kSvcTestRaw    = 0xfe00;

static constexpr UInt32 kQCA6174TargetCEConfigCount = 7;
static constexpr TargetCEPipeConfig kQCA6174TargetCEConfig[kQCA6174TargetCEConfigCount] = {
    {0, kPipeDirOut,   32,  256, 0, 0},
    {1, kPipeDirIn,    32, 2048, 0, 0},
    {2, kPipeDirIn,    64, 2048, 0, 0},
    {3, kPipeDirOut,   32, 2048, 0, 0},
    {4, kPipeDirOut,  256,  256, 0, 0},
    // QCA6174/QCA9377 override: CE5 is repurposed host->target.
    {5, kPipeDirOut,   32, 2048, 0, 0},
    {6, kPipeDirInOut, 32, 4096, 0, 0},
};

static constexpr UInt32 kQCA6174ServiceMapCount = 17;
static constexpr TargetServiceToPipe kQCA6174ServiceMap[kQCA6174ServiceMapCount] = {
    {kSvcWMIDataVO,  kPipeDirOut, 3},
    {kSvcWMIDataVO,  kPipeDirIn,  2},
    {kSvcWMIDataBK,  kPipeDirOut, 3},
    {kSvcWMIDataBK,  kPipeDirIn,  2},
    {kSvcWMIDataBE,  kPipeDirOut, 3},
    {kSvcWMIDataBE,  kPipeDirIn,  2},
    {kSvcWMIDataVI,  kPipeDirOut, 3},
    {kSvcWMIDataVI,  kPipeDirIn,  2},
    {kSvcWMIControl, kPipeDirOut, 3},
    {kSvcWMIControl, kPipeDirIn,  2},
    {kSvcRsvdCtrl,   kPipeDirOut, 0},
    {kSvcRsvdCtrl,   kPipeDirIn,  1},
    {kSvcTestRaw,    kPipeDirOut, 0},
    {kSvcTestRaw,    kPipeDirIn,  1},
    {kSvcHTTData,    kPipeDirOut, 4},
    // QCA6174/QCA9377 override: HTT target->host uses CE1.
    {kSvcHTTData,    kPipeDirIn,  1},
    {0,               kPipeDirNone, 0},
};

static constexpr UInt32 kQCA6174TargetCEConfigBytes =
    sizeof(kQCA6174TargetCEConfig);
static constexpr UInt32 kQCA6174ServiceMapBytes =
    sizeof(kQCA6174ServiceMap);

inline bool mapQCA6174ServiceToPipes(UInt32 serviceId, UInt8 &uplinkPipe,
                                     UInt8 &downlinkPipe)
{
    uplinkPipe = 0xff;
    downlinkPipe = 0xff;
    for (UInt32 i = 0; i < kQCA6174ServiceMapCount; ++i) {
        const auto &entry = kQCA6174ServiceMap[i];
        if (entry.serviceId != serviceId)
            continue;
        if (entry.pipeDir == kPipeDirOut)
            uplinkPipe = static_cast<UInt8>(entry.pipeNum);
        else if (entry.pipeDir == kPipeDirIn)
            downlinkPipe = static_cast<UInt8>(entry.pipeNum);
    }
    return uplinkPipe != 0xff && downlinkPipe != 0xff;
}

static_assert(sizeof(TargetCEPipeConfig) == 24, "target CE config ABI size");
static_assert(sizeof(TargetServiceToPipe) == 12, "service map ABI size");

static constexpr UInt32 kChipIdRevisionMask   = 0x00000f00;
static constexpr UInt32 kChipIdRevisionShift  = 8;

static constexpr UInt32 kWakeTimeoutUs = 30000;
static constexpr UInt32 kWakeLateUs    = 10000;

inline UInt32 rtcStateValue(UInt32 value) { return value & kRtcStateMask; }
inline UInt8 chipRevision(UInt32 chipId) {
    return static_cast<UInt8>((chipId & kChipIdRevisionMask) >> kChipIdRevisionShift);
}
inline UInt32 rtcStateOffset(const TargetRegisterLayout &layout) {
    return layout.pcieLocalBaseAddress + kRtcStateAddress;
}
inline UInt32 socWakeOffset(const TargetRegisterLayout &layout) {
    return layout.pcieLocalBaseAddress + kPcieSocWakeAddress;
}
inline UInt32 chipIdOffset(const TargetRegisterLayout &layout) {
    return layout.rtcSocBaseAddress + layout.socChipIdAddress;
}
inline UInt32 globalResetOffset(const TargetRegisterLayout &layout) {
    return layout.pcieLocalBaseAddress + kSocGlobalResetAddress;
}
inline UInt32 socResetControlOffset(const TargetRegisterLayout &layout) {
    return layout.rtcSocBaseAddress + kSocResetControlAddress;
}
inline UInt32 socLFTimerControl0Offset(const TargetRegisterLayout &layout) {
    return layout.rtcSocBaseAddress + kSocLFTimerControl0Address;
}
inline UInt32 socCoreOffset(const TargetRegisterLayout &layout, UInt32 relative) {
    return layout.socCoreBaseAddress + relative;
}

} // namespace Ath10k
