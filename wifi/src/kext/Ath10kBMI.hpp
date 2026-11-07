#pragma once

#include <IOKit/IOTypes.h>

namespace Ath10k {

// Linux ath10k BMI transport constants for PCI/QCA6174.
static constexpr UInt32 kBMICENumToTarget = 0;
static constexpr UInt32 kBMICENumToHost   = 1;
static constexpr UInt32 kBMIDone          = 1;
static constexpr UInt32 kBMIReadMemory    = 2;
static constexpr UInt32 kBMIWriteMemory   = 3;
static constexpr UInt32 kBMIExecute       = 4;
static constexpr UInt32 kBMIReadSOCRegister=6,kBMIWriteSOCRegister=7;
struct BMIReadSOCRequest {UInt32 id,address;};
struct BMIWriteSOCRequest {UInt32 id,address,value;};
static_assert(sizeof(BMIReadSOCRequest)==8 && sizeof(BMIWriteSOCRequest)==12,"BMI SOC ABI");
static constexpr UInt32 kBMILZStreamStart = 13;
static constexpr UInt32 kBMILZData        = 14;
// Linux hw.h QCA6174_HW_3_0_PATCH_LOAD_ADDR, also used for hw3.2.
// Use with LZ/EXECUTE, not as a guessed address for WRITE_MEMORY.
static constexpr UInt32 kQCA6174OTPLoadAddress = 0x1234;
static constexpr UInt32 kQCA6174OTPExecuteParam = 0;
static constexpr UInt32 kQCA6174FirmwareLoadAddress = 0x1234;
static constexpr UInt32 kBMIGetTargetInfo = 8;
static constexpr UInt32 kBMIMaxDataSize   = 256;
static constexpr UInt32 kBMIResponseBufferSize = 256;
static constexpr UInt32 kBMICommunicationTimeoutUs = 3000000;
static constexpr UInt32 kBMIPollIntervalUs = 1000;

// ath10k_ce_send(... transfer_id = -1, flags = 0) encodes metadata 0xffff
// through QCA6174's metadata field (mask 0xfffc, lsb 2), yielding 0xfffc.
static constexpr UInt16 kQCA6174CEDescriptorMetadataAnonymous = 0xfffcu;

struct BMIReadMemoryRequest {
    UInt32 id;
    UInt32 address;
    UInt32 length;
};

static_assert(sizeof(BMIReadMemoryRequest) == 12,
              "BMI_READ_MEMORY request ABI size");

// Linux limits the entire WRITE_MEMORY command to 256 bytes: 12 header
// bytes and at most 244 payload bytes. The final word is zero padded.
static constexpr UInt32 kBMIWritePayloadSize = kBMIMaxDataSize - 12;
struct BMIWriteMemoryRequest {
    UInt32 id;
    UInt32 address;
    UInt32 length;
    UInt8 payload[kBMIWritePayloadSize];
};
static_assert(sizeof(BMIWriteMemoryRequest) == 256, "BMI write command ABI");

struct BMILZStreamStartRequest { UInt32 id; UInt32 address; };
struct BMIExecuteRequest { UInt32 id; UInt32 address; UInt32 parameter; };
static constexpr UInt32 kBMILZPayloadSize = kBMIMaxDataSize - 8;
struct BMILZDataRequest { UInt32 id; UInt32 length; UInt8 payload[kBMILZPayloadSize]; };
static_assert(sizeof(BMILZStreamStartRequest) == 8, "BMI LZ start ABI");
static_assert(sizeof(BMIExecuteRequest) == 12, "BMI execute ABI");
static_assert(sizeof(BMILZDataRequest) == 256, "BMI LZ data ABI");

inline UInt32 buildBMILZDataRequest(BMILZDataRequest &request,
                                  const UInt8 *data, UInt32 remaining,
                                  UInt32 &consumed)
{
    consumed = 0;
    if (!data || remaining == 0 || (remaining & 3u))
        return 0;
    const UInt32 size = remaining < kBMILZPayloadSize ? remaining : kBMILZPayloadSize;
    request.id = kBMILZData;
    request.length = size;
    for (UInt32 i = 0; i < kBMILZPayloadSize; ++i)
        request.payload[i] = i < size ? data[i] : 0;
    consumed = size;
    return 8 + size;
}

inline bool otpResultAccepted(UInt32 result, bool ignoreResultFeature)
{
    return result == 0 || ignoreResultFeature;
}

inline bool bmiMemoryRangeValid(UInt32 address, UInt32 length)
{
    return length != 0 && static_cast<UInt64>(address) + length <= 0x100000000ull;
}

inline UInt32 buildBMIWriteRequest(BMIWriteMemoryRequest &request,
                                  UInt32 address, const UInt8 *data,
                                  UInt32 remaining, UInt32 &consumed)
{
    consumed = 0;
    if (!data || (address & 3u) || !bmiMemoryRangeValid(address, remaining))
        return 0;
    const UInt32 size = remaining < kBMIWritePayloadSize ? remaining : kBMIWritePayloadSize;
    const UInt32 padded = (size + 3u) & ~3u;
    if (!bmiMemoryRangeValid(address, padded))
        return 0;
    request.id = kBMIWriteMemory;
    request.address = address;
    request.length = padded;
    for (UInt32 i = 0; i < kBMIWritePayloadSize; ++i)
        request.payload[i] = i < size ? data[i] : 0;
    consumed = size;
    return 12u + padded;
}

struct BMIGetTargetInfoResponse {
    UInt32 length;
    UInt32 version;
    UInt32 type;
};

static_assert(sizeof(BMIGetTargetInfoResponse) == 12,
              "BMI_GET_TARGET_INFO response ABI size");

} // namespace Ath10k
