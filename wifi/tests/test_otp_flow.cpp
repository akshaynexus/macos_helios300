#define ATH10K_OTP_FLOW_TEST
#include "test_board_flow.cpp"
#include "otp_methods.inc"
#include <vector>

// This model validates command framing/order and the exact compressed stream.
// It does not decompress/execute the binary or model physical calibration.
class OtpDriver : public Ath10kPCIDevice {
public:
    std::vector<UInt8> stream;
    std::vector<UInt32> starts;
    UInt32 rawResult = 0, replyBytes = 4;
    bool exchangeOK = true;
    unsigned executions = 0;
    explicit OtpDriver(bool qca9377=false) {
        if(qca9377){_deviceId=0x0042;_subsystemVendorId=0x1028;_subsystemId=0x1810;
            _selectedHw=Ath10k::findCandidate(0x0042,1);_bmiTargetVersion=0x05020001;
            _bmiBoardDataAddress=0x401100;}

        assert(runBMIBoardUploadTest());
        _regs.posts = 0;
        _regs.consume = [this]() {
            auto *ring = static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[0].virtualAddress());
            const auto &desc = ring[_regs.read];
            const auto *words = static_cast<const UInt32 *>(_bmiRequestBuffer.virtualAddress());
            assert(desc.nbytes <= 256);
            if (words[0] == 13) {
                assert(desc.nbytes == 8);
                if (starts.empty()) assert(words[1] == 0x1234);
                else {
                    assert(starts.size() == 1 && words[1] == 0);
                    assert(stream.size() == ((payload().kOTPSize+3u)&~3u));
                }
                starts.push_back(words[1]);
            } else {
                assert(words[0] == 14 && starts.size() == 1);
                const auto *data = static_cast<const Ath10k::BMILZDataRequest *>(_bmiRequestBuffer.virtualAddress());
                assert(data->length > 0 && data->length <= 248 && data->length % 4 == 0);
                assert(desc.nbytes == data->length + 8);
                stream.insert(stream.end(), data->payload, data->payload + data->length);
            }
        };
        exchange = [this](const void *p, UInt32 bytes, void *out, UInt32 &length) {
            ++executions;
            const auto *r = static_cast<const Ath10k::BMIExecuteRequest *>(p);
            assert(bytes == 12 && r->id == 4 && r->address == 0x1234 && r->parameter == 0);
            assert(starts == std::vector<UInt32>({0x1234, 0}));
            assert(_otpDownloadSucceeded && _regs.posts == _otpLZDataCommands+2 && length == 4);
            assert(stream.size() == ((payload().kOTPSize+3u)&~3u));
            assert(std::memcmp(stream.data(), payload().kOTP, payload().kOTPSize) == 0);
            for (size_t i = payload().kOTPSize; i < stream.size(); ++i) assert(stream[i] == 0);
            if (!exchangeOK) { _bmiTransferUncertain = true; return false; }
            std::memcpy(out, &rawResult, sizeof(rawResult));
            length = replyBytes;
            return true;
        };
    }
};

#ifndef ATH10K_FIRMWARE_FLOW_TEST
int main()
{
    { OtpDriver d;
      assert(d.runBMIOtpTest());
      assert(d._otpDownloadSucceeded && d._otpExecuteSucceeded && d._otpResultAccepted);
      assert(d._otpResult == 0 && !d._otpResultIgnored && d._otpWireBytesSent == 24432);
      assert(d._otpLZDataCommands == 100 && d.executions == 1);
      assert(!d.runBMIOtpTest() && d.executions == 1); }
    { OtpDriver d; d.rawResult = 0x123;
      assert(d.runBMIOtpTest());
      assert(d._otpResultIgnored && d._otpResult == 0x123); }
    { OtpDriver d; d.exchangeOK = false;
      assert(!d.runBMIOtpTest() && d._bmiTransferUncertain);
      assert(d._otpDownloadSucceeded && !d._otpExecuteSucceeded && !d._otpResultAccepted); }
    { OtpDriver d; d.replyBytes = 2;
      assert(!d.runBMIOtpTest() && !d._otpExecuteSucceeded);
      assert(d._otpExecuteResponseBytes == 2 && !d._otpResultAccepted); }
    for (unsigned fail : {1u, 2u, 51u, 101u, 102u}) {
        OtpDriver d; d._regs.failAtPost = fail;
        assert(!d.runBMIOtpTest());
        assert(d._regs.posts == fail && d.executions == 0 && d._bmiTransferUncertain);
        assert(!d._otpDownloadSucceeded && !d._otpExecuteAttempted);
    }
    { OtpDriver d; d._regs.timeout = true;
      assert(!d.runBMIOtpTest() && d._bmiTimedOut && d._bmiTransferUncertain);
      assert(d.executions == 0 && d._regs.posts == 1); }
    { OtpDriver d; d._bmiBoardReadbackSucceeded = false;
      assert(!d.runBMIOtpTest() && d._regs.posts == 0 && !d._otpAttempted); }
    { OtpDriver d; d._deviceId = 0x0042;
      assert(!d.runBMIOtpTest() && d._regs.posts == 0); }
    { OtpDriver d;
      assert(!d.bmiLZStreamStart(0) && !d.bmiLZStreamStart(0x1234));
      assert(!d.bmiLZData(d.payload().kOTP, 4) && d._regs.posts == 0); }
    assert(Ath10k::otpResultAccepted(0, false));
    assert(!Ath10k::otpResultAccepted(1, false));
    assert(Ath10k::otpResultAccepted(1, true));
}
#endif
