#define ATH10K_FIRMWARE_FLOW_TEST
#include "test_otp_flow.cpp"
#include "firmware_methods.inc"

class FirmwareDriver : public OtpDriver {
public:
    std::vector<UInt8> wlanStream;
    std::vector<UInt32> wlanStarts;
    struct ModelSegment { UInt32 address, size, offset; };
    std::vector<ModelSegment> targetSegments;
    UInt32 corruptAddress = 0xffffffffu, failReadAddress = 0xffffffffu;
    UInt32 postVersion = 0x05030000, postReplyBytes = 12;
    bool postExchangeOK = true;
    unsigned postQueries = 0, reads = 0;

    UInt32 word(size_t pos) const {
        assert(pos + 4 <= wlanStream.size());
        return static_cast<UInt32>(wlanStream[pos]) |
            (static_cast<UInt32>(wlanStream[pos + 1]) << 8) |
            (static_cast<UInt32>(wlanStream[pos + 2]) << 16) |
            (static_cast<UInt32>(wlanStream[pos + 3]) << 24);
    }
    void acceptImage() {
        assert(wlanStream.size() == ((payload().kFirmwareSize+3u)&~3u) && word(0) == 0x544d4753 && word(4) == 0);
        assert(std::memcmp(wlanStream.data(), payload().kFirmware, payload().kFirmwareSize) == 0);
        for (size_t i = payload().kFirmwareSize; i < wlanStream.size(); ++i) assert(wlanStream[i] == 0);
        // Model the uncompressed ROM loader using the received SGMT bytes,
        // independently of the generated readback table used by the driver.
        size_t pos = 8;
        UInt32 entry = 0;
        for (;;) {
            UInt32 address = word(pos), length = word(pos + 4);
            pos += 8;
            if (length == 0xffffffff) { assert(pos == payload().kFirmwareSize); break; }
            if (length == 0xfffffffd) { entry = address; continue; }
            assert(length > 0 && length < payload().kFirmwareSize && pos + length <= payload().kFirmwareSize);
            targetSegments.push_back({address, length, static_cast<UInt32>(pos)});
            pos += length;
        }
        assert(targetSegments.size() == 6 && entry == 0xa0400);
    }
    explicit FirmwareDriver(bool qca9377=false) : OtpDriver(qca9377) {
        postVersion=_bmiTargetVersion;
        rawResult = 3; // Preserve the exact OTP result reported on hardware.
        assert(runBMIOtpTest() && _otpResult == 3 && _otpResultIgnored);
        _regs.posts = 0;
        _regs.consume = [this]() {
            auto *ring = static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[0].virtualAddress());
            const auto &desc = ring[_regs.read];
            const auto *w = static_cast<const UInt32 *>(_bmiRequestBuffer.virtualAddress());
            if (w[0] == 13) {
                assert(desc.nbytes == 8);
                if (wlanStarts.empty()) assert(w[1] == 0x1234);
                else { assert(wlanStarts.size() == 1 && w[1] == 0); acceptImage(); }
                wlanStarts.push_back(w[1]);
            } else {
                assert(w[0] == 14 && wlanStarts.size() == 1); // No DONE or EXECUTE.
                auto *r = static_cast<const Ath10k::BMILZDataRequest *>(_bmiRequestBuffer.virtualAddress());
                assert(r->length && r->length <= 248 && r->length % 4 == 0);
                assert(desc.nbytes == 8 + r->length);
                wlanStream.insert(wlanStream.end(), r->payload, r->payload + r->length);
            }
        };
        exchange = [this](const void *p, UInt32 n, void *out, UInt32 &length) {
            ++postQueries;
            assert(n == 4 && *static_cast<const UInt32 *>(p) == 8);
            assert(length == 12 && wlanStarts == std::vector<UInt32>({0x1234, 0}));
            assert(_regs.posts == _fwLZDataCommands+2 && _fwDownloadSucceeded);
            if (!postExchangeOK) { _bmiTransferUncertain = true; return false; }
            Ath10k::BMIGetTargetInfoResponse response{12, postVersion, 8};
            std::memcpy(out, &response, sizeof(response));
            length = postReplyBytes;
            return true;
        };
        readHook = [this](UInt32 address, void *out, UInt32 size) {
            ++reads;
            assert(_fwPostBMIProbeSucceeded && _fwReadbackInProgress);
            assert(size && size <= 256 && size % 4 == 0 && address % 4 == 0);
            if (address <= failReadAddress && failReadAddress - address < size) return false;
            for (const auto &s : targetSegments) {
                if (address < s.address || address >= s.address + s.size) continue;
                UInt32 offset = address - s.address;
                UInt32 actual = s.size - offset;
                if (actual > size) actual = size;
                assert(offset + size <= ((s.size + 3u) & ~3u));
                std::memset(out, 0xcc, size); // RAM padding is NOT stream padding.
                std::memcpy(out, wlanStream.data() + s.offset + offset, actual);
                if (address <= corruptAddress && corruptAddress - address < actual)
                    static_cast<UInt8 *>(out)[corruptAddress - address] ^= 1;
                return true;
            }
            assert(false); // Driver attempted a read outside an uploaded segment.
            return false;
        };
    }
};

#ifndef ATH10K_PROFILE_FLOW_TEST
int main()
{
    { FirmwareDriver d;
      assert(d.runBMIFirmwareDownloadTest());
      assert(d._fwWireBytesSent == 681824 && d._fwLZDataCommands == 2751);
      assert(d._fwReadbackBytes == 681749 && d._fwVerifiedSegments == 6);
      assert(d._fwReadbackSucceeded && d._fwPostBMIProbeSucceeded && !d._fwReadbackInProgress);
      assert(d._fwFailedSegment == 0xffffffffu && d.executions == 1 && d._otpResult == 3);
      assert(!d.runBMIFirmwareDownloadTest() && d.postQueries == 1); }
    { FirmwareDriver d; d.corruptAddress = 0xa0400 + 25732; // Odd-sized final byte.
      assert(!d.runBMIFirmwareDownloadTest() && !d._fwReadbackSucceeded);
      assert(d._fwFailedSegment == 5 && d._fwMismatchAddress == d.corruptAddress);
      assert(!d._fwReadbackInProgress && d._fwVerifiedSegments == 5); }
    { FirmwareDriver d; d.failReadAddress = 0x416d90;
      assert(!d.runBMIFirmwareDownloadTest() && d._fwVerifiedSegments == 2);
      assert(!d._fwReadbackInProgress); }
    for (unsigned fail : {1u, 2u, 1000u, 2752u, 2753u}) {
        FirmwareDriver d; d._regs.failAtPost = fail;
        assert(!d.runBMIFirmwareDownloadTest() && d._regs.posts == fail);
        assert(d.postQueries == 0 && !d._fwDownloadSucceeded && d._bmiTransferUncertain);
    }
    { FirmwareDriver d; d.postVersion = 0xdeadbeef;
      assert(!d.runBMIFirmwareDownloadTest() && !d._fwPostBMIProbeSucceeded && d.reads == 0); }
    { FirmwareDriver d; d.postReplyBytes = 8;
      assert(!d.runBMIFirmwareDownloadTest() && d.reads == 0); }
    { FirmwareDriver d; d.postExchangeOK = false;
      assert(!d.runBMIFirmwareDownloadTest() && d._bmiTransferUncertain && d.reads == 0); }
    { FirmwareDriver d; d._regs.timeout = true;
      assert(!d.runBMIFirmwareDownloadTest() && d._bmiTimedOut && d._regs.posts == 1); }
    { FirmwareDriver d; d._otpResultAccepted = false;
      assert(!d.runBMIFirmwareDownloadTest() && !d._fwAttempted && d._regs.posts == 0); }
    { FirmwareDriver d; d._subsystemId = 0xffff;
      assert(!d.runBMIFirmwareDownloadTest() && d._regs.posts == 0); }
    { FirmwareDriver d;
      assert(!d.bmiFirmwareLZStart(0) && !d.bmiFirmwareLZStart(0x1234));
      assert(!d.bmiFirmwareLZData(Ath10kPayload::kFirmware, 4) && d._regs.posts == 0); }
}

#endif
