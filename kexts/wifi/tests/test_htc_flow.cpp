#define ATH10K_OTP_FLOW_TEST
#include "test_board_flow.cpp"
#include "htc_methods.inc"
class HTCDriver : public Ath10kPCIDevice {
public:
    unsigned requests = 0;
    bool duplicate = false, reject = false, noResponse = false;
    bool crash = false, badService = false;
    bool extended = false, trailer = false, malformed = false;
    HTCDriver() {
        _boot.readyValidated = true;
        _boot.ready.credits = 2;
        _boot.ready.maxEndpoints = 4;
        _regs.allowRX = true;
        _regs.read = _regs.written = 15;
        _runtimeRxSlot = _regs.rxRead = 511;
        _runtimeRxNext = _regs.rxWritten = 0;
        auto *rx = static_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[1].virtualAddress());
        rx[511].address = static_cast<UInt32>(_htcReadyBuffer.dmaAddress());
        _regs.consume = [this]() {
            auto *tx = static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[0].virtualAddress());
            const auto &desc = tx[_regs.read];
            assert(desc.flags == 0 && desc.nbytes == 16);
            auto *packet = static_cast<UInt8 *>(_htcControlBuffer.virtualAddress());
            assert(packet[0] == 0 && packet[5] == requests);
            assert(Ath10k::htcReadLE16(packet + 8) == 2);
            UInt16 service = requests == 0 ? Ath10k::kSvcHTTData : Ath10k::kSvcWMIControl;
            assert(Ath10k::htcReadLE16(packet + 10) == service);
            assert(Ath10k::htcReadLE16(packet + 12) == (requests == 0 ? 8 : 0x200));
            ++requests;
            if (crash) { _regs.fwIndicator = 3; return; }
            if (noResponse) return;
            auto *response = static_cast<UInt8 *>(_htcReadyBuffer.virtualAddress());
            bzero(response, 16);
            response[2] = 8;
            response[8] = 3;
            Ath10k::htcWriteLE16(response + 10, badService ? 0xffff : service);
            response[12] = reject ? 1 : 0;
            response[13] = duplicate ? 1 : requests;
            response[15] = 4;
            auto *rx = static_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[1].virtualAddress());
            if (extended || trailer) {
                response[2] = 12;
                if (trailer) {
                    response[1] = Ath10k::kHTCTrailerPresent;
                    response[4] = 4;
                    response[17] = malformed ? 1 : 0;
                } else {
                    response[16] = 0x7a;
                }
            }
            rx[_runtimeRxSlot].nbytes = (extended || trailer) ? 20 : 16;
            _regs.rxRead = _runtimeRxNext;
        };
    }
};
int main() {
    HTCDriver good;
    assert(good.runHTCServiceSetup());
    assert(good.requests == 2 && good._htcControlTxCount == 2 && good._htcControlRxCount == 2);
    assert(good._htcServiceSetupSucceeded && !good._htcSetupCompleteSent);
    assert(good._htcWMIControl.creditFlowEnabled && !good._htcHTTData.creditFlowEnabled);
    assert(good._htcWMIControl.endpoint == 2 && good._htcHTTData.endpoint == 1);
    assert(good._runtimeRxPostedCount == 3 && !good._htcControlExchangeUncertain);
    assert(!good.runHTCServiceSetup() && good.requests == 2);
    HTCDriver extension; extension.extended = true;
    assert(extension.runHTCServiceSetup() && extension.requests == 2);
    assert(extension._htcControlLastRxBytes == 20);
    HTCDriver trailing; trailing.trailer = true;
    assert(trailing.runHTCServiceSetup() && trailing.requests == 2);
    HTCDriver malformed; malformed.trailer = true; malformed.malformed = true;
    assert(!malformed.runHTCServiceSetup() && malformed.requests == 1);
    assert(!malformed._htcHTTData.connected && !malformed._htcSetupCompleteSent);
    HTCDriver timeout; timeout.noResponse = true;
    assert(!timeout.runHTCServiceSetup());
    assert(!strcmp(timeout._htcStatus, "control-response-timeout"));
    assert(timeout._htcControlLastWaitMs == Ath10k::kHTCServiceTimeoutMs);
    assert(timeout._htcControlExchangeUncertain);
    assert(!timeout.runHTCServiceSetup() && timeout._htcControlExchangeUncertain);
    HTCDriver crash; crash.crash = true;
    assert(!crash.runHTCServiceSetup());
    assert(!strcmp(crash._htcStatus, "firmware-crash-indicated"));
    assert(crash._boot.firmwareCrash && crash._htcControlExchangeUncertain);
    HTCDriver submit; submit._regs.doorbellFailure = true;
    assert(!submit.runHTCServiceSetup());
    assert(!strcmp(submit._htcStatus, "control-submit-failed") && submit._htcControlExchangeUncertain);
    HTCDriver stalled; stalled._regs.timeout = true;
    assert(!stalled.runHTCServiceSetup());
    assert(!strcmp(stalled._htcStatus, "control-tx-timeout"));
    HTCDriver dup; dup.duplicate = true;
    assert(!dup.runHTCServiceSetup());
    assert(!strcmp(dup._htcStatus, "duplicate-service-endpoint"));
    HTCDriver rejected; rejected.reject = true;
    assert(!rejected.runHTCServiceSetup() && rejected.requests == 1);
    HTCDriver mismatch; mismatch.badService = true;
    assert(!mismatch.runHTCServiceSetup() && mismatch.requests == 1);
    HTCDriver repost; repost._regs.rxDoorbellFails = true;
    assert(!repost.runHTCServiceSetup());
    assert(!strcmp(repost._htcStatus, "control-rx-repost-failed"));
    assert(repost._htcControlExchangeUncertain);
    HTCDriver credits; credits._boot.ready.credits = 256;
    assert(!credits.runHTCServiceSetup() && credits.requests == 0);
}
