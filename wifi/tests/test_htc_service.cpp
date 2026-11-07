#include "Ath10kHTC.hpp"
#include "Ath10kTarget.hpp"
#include <cassert>

int main()
{
    UInt8 frame[Ath10k::kHTCControlMaxBytes]{};
    UInt32 bytes = Ath10k::buildHTCSetupCompleteExFrame(frame, sizeof(frame), 8);
    assert(bytes == Ath10k::kHTCSetupCompleteExFrameBytes);
    assert(frame[0] == 0 && frame[1] == 0 && Ath10k::htcReadLE16(frame + 2) == 12);
    assert(Ath10k::htcReadLE16(frame + 8) == Ath10k::kHTCMessageSetupCompleteEx);
    assert(frame[16] == 8);
    assert(Ath10k::buildHTCSetupCompleteExFrame(frame, 19, 0) == 0);

    bytes = Ath10k::buildHTCConnectServiceFrame(
        frame, sizeof(frame), 0x0100, Ath10k::kHTCConnFlagDisableCreditFlow);
    assert(bytes == Ath10k::kHTCConnectServiceFrameBytes);
    assert(Ath10k::htcReadLE16(frame + 2) == 8);
    assert(Ath10k::htcReadLE16(frame + 8) == Ath10k::kHTCMessageConnectService);
    assert(Ath10k::htcReadLE16(frame + 10) == 0x0100);
    assert(Ath10k::htcReadLE16(frame + 12) == Ath10k::kHTCConnFlagDisableCreditFlow);

    const UInt8 response[] = {
        0, 0, 8, 0, 0, 0, 0, 0,
        3, 0, 0, 1, 0, 2, 0x00, 0x04,
    };
    Ath10k::HTCServiceInfo info{};
    assert(!Ath10k::parseHTCConnectServiceResponse(
        response, sizeof(response), 0x0100, 4, info));
    assert(info.serviceId == 0x0100 && info.status == 0 && info.endpoint == 2);
    assert(info.maxMessageSize == 0x0400 && info.connected);

    Ath10k::HTCServiceInfo rejected{};
    UInt8 bad[sizeof(response)];
    for (UInt32 i = 0; i < sizeof(response); ++i)
        bad[i] = response[i];
    bad[12] = 1;
    assert(Ath10k::parseHTCConnectServiceResponse(
        bad, sizeof(bad), 0x0100, 4, rejected));
    assert(rejected.status == 1 && !rejected.connected);

    for (UInt32 i = 0; i < sizeof(response); ++i)
        bad[i] = response[i];
    bad[13] = 4; // maxEndpoints is exclusive; endpoint 4 is invalid.
    assert(Ath10k::parseHTCConnectServiceResponse(
        bad, sizeof(bad), 0x0100, 4, rejected));
    for (UInt32 i = 0; i < sizeof(response); ++i)
        bad[i] = response[i];
    bad[10] = 1; // service id no longer matches the requested service.
    assert(Ath10k::parseHTCConnectServiceResponse(
        bad, sizeof(bad), 0x0100, 4, rejected));
    assert(Ath10k::parseHTCConnectServiceResponse(
        response, sizeof(response) - 1, 0x0100, 4, rejected));

    // Synthetic 20-byte variants: the hardware log gave a length, not raw bytes.
    // Exercise both a declared extension and a trailer, without assuming which
    // one the QCA6174 actually returned.
    UInt8 extended[20]{};
    for (UInt32 i = 0; i < sizeof(response); ++i) extended[i] = response[i];
    extended[2] = 12;
    extended[16] = 0xab; // Unknown extension fields need not be zero.
    assert(!Ath10k::parseHTCConnectServiceResponse(extended, 20, 0x100, 4, info));
    assert(info.connected && info.endpoint == 2);
    for (UInt32 n = 0; n < 20; ++n)
        assert(Ath10k::parseHTCConnectServiceResponse(extended, n, 0x100, 4, info));
    extended[2] = 8; // Extra undeclared word is NOT a permitted extension.
    assert(Ath10k::parseHTCConnectServiceResponse(extended, 20, 0x100, 4, info));
    extended[2] = 9; // Three bytes of CE alignment padding.
    assert(!Ath10k::parseHTCConnectServiceResponse(extended, 20, 0x100, 4, info));
    extended[2] = 12;
    extended[1] = Ath10k::kHTCTrailerPresent;
    extended[4] = 4;
    extended[16] = 0; // Null record, header only.
    assert(!Ath10k::parseHTCConnectServiceResponse(extended, 20, 0x100, 4, info));
    extended[17] = 1; // Record body overruns trailer.
    assert(Ath10k::parseHTCConnectServiceResponse(extended, 20, 0x100, 4, info));
    extended[17] = 0;
    extended[4] = 13; // Trailer exceeds declared payload.
    assert(Ath10k::parseHTCConnectServiceResponse(extended, 20, 0x100, 4, info));
    extended[4] = 3; // Trailer shorter than record header.
    assert(Ath10k::parseHTCConnectServiceResponse(extended, 20, 0x100, 4, info));
    extended[4] = 8; // Trailer overlaps the mandatory response body.
    extended[12] = 0; extended[13] = 0; extended[14] = 0; extended[15] = 0;
    assert(Ath10k::parseHTCConnectServiceResponse(extended, 20, 0x100, 4, info));
    UInt8 credits[24]{};
    for (UInt32 i = 0; i < sizeof(response); ++i) credits[i] = response[i];
    credits[1] = Ath10k::kHTCTrailerPresent; credits[2] = 16; credits[4] = 8;
    credits[16] = 1; credits[17] = 4; credits[20] = 2; credits[21] = 1;
    assert(!Ath10k::parseHTCConnectServiceResponse(credits, 24, 0x100, 4, info));
    credits[17] = 3;
    assert(Ath10k::parseHTCConnectServiceResponse(credits, 24, 0x100, 4, info));
    credits[1] = 0x80;
    assert(Ath10k::parseHTCConnectServiceResponse(credits, 24, 0x100, 4, info));
    assert(Ath10k::parseHTCConnectServiceResponse(nullptr, 20, 0x100, 4, info));

    UInt8 uplink = 0xff, downlink = 0xff;
    assert(Ath10k::mapQCA6174ServiceToPipes(Ath10k::kSvcWMIControl,
                                             uplink, downlink));
    assert(uplink == 3 && downlink == 2);
    assert(Ath10k::mapQCA6174ServiceToPipes(Ath10k::kSvcHTTData,
                                             uplink, downlink));
    assert(uplink == 4 && downlink == 1);
    return 0;
}
