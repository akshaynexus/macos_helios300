#pragma once

#include <IOKit/IOTypes.h>

namespace Ath10k {

// Wire ABI from Linux v6.12 ath10k/htc.h. Decode bytes explicitly: DMA
// buffers and malicious/truncated packets need not have natural alignment.
static constexpr UInt32 kHTCHeaderBytes = 8;
static constexpr UInt32 kHTCReadyBufferBytes = 2048; // CE1 target src_sz_max
static constexpr UInt32 kHTCReadyTimeoutMs = 3000;
static constexpr UInt32 kHTCReadyPollMs = 1;
static constexpr UInt32 kHTCControlMaxBytes = 256;
static constexpr UInt32 kHTCServiceTimeoutMs = 2000;
static constexpr UInt32 kHTCServicePollMs = 1;
static constexpr UInt8 kHTCTrailerPresent = 0x02;
static constexpr UInt8 kHTCMoreBlock = 0x01; // Advisory for block-based buses.

// ath10k_htc_msg message identifiers from the firmware HTC ABI.
static constexpr UInt16 kHTCMessageReady = 1;
static constexpr UInt16 kHTCMessageConnectService = 2;
static constexpr UInt16 kHTCMessageConnectServiceResponse = 3;
static constexpr UInt16 kHTCMessageSetupComplete = 4;
static constexpr UInt16 kHTCMessageSetupCompleteEx = 5;

// Disable credit flow only for non-WMI services. WMI receives READY credits
// in connection flag bits 8..15, even while data transmission is deferred.
static constexpr UInt16 kHTCConnFlagDisableCreditFlow = 0x0008;
static constexpr UInt8 kHTCServiceStatusSuccess = 0;
static constexpr UInt32 kHTCSetupCompleteExFrameBytes = 20;
static constexpr UInt32 kHTCConnectServiceFrameBytes = 16;

struct HTCReadyInfo {
    UInt16 credits = 0;
    UInt16 creditSize = 0;
    UInt8 maxEndpoints = 0;
    bool extended = false;
    UInt8 version = 0;
    UInt8 maxBundleMessages = 0;
    UInt16 altDataCreditSize = 0;
    UInt16 payloadBytes = 0;
    UInt8 trailerBytes = 0;
};

struct HTCServiceInfo {
    UInt32 serviceId = 0;
    UInt8 endpoint = 0xff;
    UInt16 maxMessageSize = 0;
    UInt8 uplinkPipe = 0xff;
    UInt8 downlinkPipe = 0xff;
    UInt8 status = 0xff;
    bool connected = false;
    bool creditFlowEnabled = false;
};

inline UInt16 htcReadLE16(const UInt8 *p)
{
    return static_cast<UInt16>(p[0] | (static_cast<UInt16>(p[1]) << 8));
}

inline void htcWriteLE16(UInt8 *p, UInt16 value)
{
    p[0] = static_cast<UInt8>(value & 0xffu);
    p[1] = static_cast<UInt8>((value >> 8) & 0xffu);
}

inline void htcWriteLE32(UInt8 *p, UInt32 value)
{
    p[0] = static_cast<UInt8>(value & 0xffu);
    p[1] = static_cast<UInt8>((value >> 8) & 0xffu);
    p[2] = static_cast<UInt8>((value >> 16) & 0xffu);
    p[3] = static_cast<UInt8>((value >> 24) & 0xffu);
}

inline void htcZero(UInt8 *p, UInt32 bytes)
{
    for (UInt32 i = 0; i < bytes; ++i)
        p[i] = 0;
}

inline UInt32 buildHTCSetupCompleteExFrame(UInt8 *frame, UInt32 capacity,
                                            UInt8 maxBundleMessages = 0)
{
    if (!frame || capacity < kHTCSetupCompleteExFrameBytes)
        return 0;

    htcZero(frame, kHTCSetupCompleteExFrameBytes);
    frame[0] = 0; // reserved-control endpoint
    frame[1] = 0;
    htcWriteLE16(frame + 2, 12); // message id + 10-byte extended body
    htcWriteLE16(frame + kHTCHeaderBytes, kHTCMessageSetupCompleteEx);
    // bytes 10..11 are pad0/pad1; bytes 12..15 are flags (zero for PCIe).
    frame[16] = maxBundleMessages;
    // bytes 17..19 are pad2/pad3/pad4.
    return kHTCSetupCompleteExFrameBytes;
}

inline UInt32 buildHTCConnectServiceFrame(UInt8 *frame, UInt32 capacity,
                                           UInt16 serviceId, UInt16 flags)
{
    if (!frame || capacity < kHTCConnectServiceFrameBytes)
        return 0;

    htcZero(frame, kHTCConnectServiceFrameBytes);
    frame[0] = 0; // service setup is sent over reserved-control endpoint
    frame[1] = 0;
    htcWriteLE16(frame + 2, 8); // message id + connect-service body
    htcWriteLE16(frame + kHTCHeaderBytes, kHTCMessageConnectService);
    htcWriteLE16(frame + kHTCHeaderBytes + 2, serviceId);
    htcWriteLE16(frame + kHTCHeaderBytes + 4, flags);
    return kHTCConnectServiceFrameBytes;
}

// CONNECT_SERVICE_RESPONSE has an 8-byte minimum body. Firmware may append
// fields; HTC's declared payload includes any trailer. Accept extensions only
// inside the declared frame, never an unexplained extra word or second frame.
inline const char *parseHTCConnectServiceResponse(const UInt8 *frame,
                                                   UInt32 bytes,
                                                   UInt16 expectedServiceId,
                                                   UInt8 maxEndpoints,
                                                   HTCServiceInfo &out)
{
    out = HTCServiceInfo{};
    out.serviceId = expectedServiceId;
    if (!frame || bytes < kHTCHeaderBytes || bytes > kHTCControlMaxBytes)
        return "invalid-service-response-size";
    if (frame[0] != 0)
        return "unexpected-service-response-endpoint";
    if (frame[1] & ~(kHTCMoreBlock | kHTCTrailerPresent))
        return "unsupported-service-response-flags";

    const UInt32 payload = htcReadLE16(frame + 2);
    if (payload > bytes - kHTCHeaderBytes)
        return "truncated-frame";
    // CE may round the packet up to a word. Never accept a second frame.
    const UInt32 declared = kHTCHeaderBytes + payload;
    if (bytes != declared && bytes != ((declared + 3u) & ~3u))
        return "unexpected-frame-tail";
    const UInt32 trailer = (frame[1] & kHTCTrailerPresent) ? frame[4] : 0;
    if (trailer > payload || ((frame[1] & kHTCTrailerPresent) && trailer < 4))
        return "invalid-trailer-size";
    UInt32 pos = declared - trailer;
    while (pos < declared) {
        if (declared - pos < 4)
            return "truncated-trailer-header";
        const UInt32 length = frame[pos + 1];
        if (length > declared - pos - 4)
            return "truncated-trailer-record";
        // Credit entries are four bytes each; lookahead structures are 12.
        if (frame[pos] == 1 && (length == 0 || (length & 3u)))
            return "invalid-credit-record";
        if (frame[pos] == 2 && length < 12)
            return "invalid-lookahead-record";
        if (frame[pos] == 3 && (length == 0 || (length & 3u)))
            return "invalid-lookahead-bundle";
        pos += 4 + length;
    }
    const UInt32 body = payload - trailer;
    if (body < 8)
        return "invalid-service-response-size";
    // Trailer credits are structurally checked here, not added to a TX budget.
    // WMI/data transmission remains disabled until credit accounting exists.
    const UInt8 *msg = frame + kHTCHeaderBytes;
    if (htcReadLE16(msg) != kHTCMessageConnectServiceResponse)
        return "unexpected-service-response-id";
    const UInt16 serviceId = htcReadLE16(msg + 2);
    out.serviceId = serviceId;
    out.status = msg[4];
    out.endpoint = msg[5];
    out.maxMessageSize = htcReadLE16(msg + 6);
    if (serviceId != expectedServiceId)
        return "unexpected-service-id";
    if (out.status != kHTCServiceStatusSuccess)
        return "service-rejected";
    if (out.endpoint == 0 || out.endpoint >= maxEndpoints)
        return "invalid-service-endpoint";
    if (out.maxMessageSize == 0)
        return "invalid-service-message-size";
    out.connected = true;
    return nullptr;
}

// A diagnostic READY receiver only. No endpoints/credits are established or
// updated here. Reject bundles: none has been negotiated at this boot stage.
inline const char *parseHTCReady(const UInt8 *frame, UInt32 bytes, HTCReadyInfo &out)
{
    out = HTCReadyInfo{};
    if (!frame || bytes < kHTCHeaderBytes || bytes > kHTCReadyBufferBytes)
        return "invalid-frame-size";
    if (frame[0] != 0)
        return "unexpected-endpoint";
    if (frame[1] & ~(kHTCTrailerPresent | kHTCMoreBlock))
        return "unsupported-rx-flags";
    const UInt32 payload = htcReadLE16(frame + 2);
    if (payload > bytes - kHTCHeaderBytes)
        return "truncated-frame";
    // CE may round the packet up to a word. Never accept a second frame.
    const UInt32 declared = kHTCHeaderBytes + payload;
    if (bytes != declared && bytes != ((declared + 3u) & ~3u))
        return "unexpected-frame-tail";
    const UInt32 trailer = (frame[1] & kHTCTrailerPresent) ? frame[4] : 0;
    if (trailer > payload || ((frame[1] & kHTCTrailerPresent) && trailer < 4))
        return "invalid-trailer-size";
    UInt32 pos = declared - trailer;
    while (pos < declared) {
        if (declared - pos < 4)
            return "truncated-trailer-header";
        const UInt32 length = frame[pos + 1];
        if (length > declared - pos - 4)
            return "truncated-trailer-record";
        // Credit entries are four bytes each; lookahead structures are 12.
        if (frame[pos] == 1 && (length == 0 || (length & 3u)))
            return "invalid-credit-record";
        if (frame[pos] == 2 && length < 12)
            return "invalid-lookahead-record";
        if (frame[pos] == 3 && (length == 0 || (length & 3u)))
            return "invalid-lookahead-bundle";
        pos += 4 + length;
    }
    const UInt32 body = payload - trailer;
    if (body < 8 || body > kHTCControlMaxBytes || (body > 8 && body < 12))
        return "invalid-ready-size";
    const UInt8 *msg = frame + kHTCHeaderBytes;
    if (htcReadLE16(msg) != 1)
        return "unexpected-message-id";
    HTCReadyInfo info{};
    info.credits = htcReadLE16(msg + 2);
    info.creditSize = htcReadLE16(msg + 4);
    info.maxEndpoints = msg[6];
    if (!info.credits || !info.creditSize || !info.maxEndpoints)
        return "invalid-ready-resources";
    info.payloadBytes = static_cast<UInt16>(body);
    info.trailerBytes = static_cast<UInt8>(trailer);
    if (body >= 12) {
        info.extended = true;
        info.version = msg[8];
        if (info.version > 1)
            return "unsupported-htc-version";
        info.maxBundleMessages = msg[9];
        info.altDataCreditSize = htcReadLE16(msg + 10) & 0x0fff;
    }
    out = info;
    return nullptr;
}

} // namespace Ath10k
