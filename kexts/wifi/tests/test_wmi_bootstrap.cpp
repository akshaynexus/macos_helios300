#define ATH10K_OTP_FLOW_TEST
#include "test_board_flow.cpp"
#include "htc_methods.inc"
#include "quiesce_method.inc"
#include "release_method.inc"
#include <vector>

static std::vector<UInt8> event() {
    std::vector<UInt8> p(120);
    p[0] = 2;
    Ath10k::htcWriteLE16(p.data()+2, 112);
    p[8] = 1;
    Ath10k::htcWriteLE16(p.data()+12, 104);
    Ath10k::htcWriteLE16(p.data()+14, 32);
    Ath10k::htcWriteLE32(p.data()+16, 0x12345678);
    Ath10k::htcWriteLE32(p.data()+20, 0x01000000);
    Ath10k::htcWriteLE32(p.data()+52, 2);
    Ath10k::htcWriteLE32(p.data()+88, 0);
    return p;
}
// Synthetic final word: hardware log captured only the first 32 of 36 bytes.
static std::vector<UInt8> availableEvent() {
    std::vector<UInt8> p(36);
    p[0] = 2; p[2] = 28; p[5] = 3; p[8] = 3;
    p[12] = 20; p[14] = 0x2f; p[15] = 2; p[16] = 128;
    p[21] = 0x10; p[23] = 8;
    return p;
}
class WMIDriver : public Ath10kPCIDevice {
public:
    bool absent = false, badAddress = false, crash = false, invalidEvent = false;
    bool availableFirst = false, availableOnly = false, flood = false;
    bool failRepost = false, malformedAvailable = false;
    unsigned sent = 0;
    void emit(bool available) {
        auto p = available ? availableEvent() : event();
        if (invalidEvent) p[8] = 2;
        if (available && malformedAvailable) p[12] = 21;
        memcpy(_wmiRxBuffer.virtualAddress(), p.data(), p.size());
        auto *rx = static_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[2].virtualAddress());
        rx[_wmi.slot].nbytes = p.size();
        if (badAddress) rx[_wmi.slot].address ^= 8;
        _regs.wmiRead = _wmi.next;
    }
    WMIDriver() {
        _boot.readyValidated = true; _boot.rxOwned = true;
        _boot.ready.credits = 2; _boot.ready.creditSize = 1792;
        _htcServiceSetupSucceeded = true; _htcControlTxCount = 2;
        _htcWMIControl.connected = true; _htcWMIControl.creditFlowEnabled = true;
        _htcWMIControl.endpoint = 2;
        _regs.allowRX = true; _regs.rxWritten = 1;
        _regs.wmiRead = _regs.wmiWritten = 127;
        auto *rx1 = static_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[1].virtualAddress());
        rx1[0].address = static_cast<UInt32>(_htcReadyBuffer.dmaAddress());
        _regs.wmiPost = [this]() {
            if (sent && !availableOnly) emit(flood);
        };
        _regs.consume = [this]() {
            ++sent;
            assert(_wmi.rxOwned && _wmi.rxPosted && _regs.wmiWritten == 0);
            auto *tx = static_cast<UInt8 *>(_htcControlBuffer.virtualAddress());
            assert(Ath10k::htcReadLE16(tx+8) == 5 && tx[5] == 2);
            if (crash) { _regs.fwIndicator = 3; return; }
            if (absent) return;
            emit(availableFirst);
            if (failRepost) _regs.wmiPostFail = true;
        };
    }
};
#ifndef ATH10K_WMI_INIT_TEST
int main() {
    Ath10k::HTCCreditState credits;
    assert(!credits.reserve(12));
    assert(credits.initialize(2, 1792, 2));
    assert(credits.reserve(1792) && credits.available == 1);
    assert(!credits.reserve(1793) && credits.available == 1);
    assert(!credits.giveBack(1,1) && !credits.giveBack(2,2));
    assert(credits.giveBack(2,1) && credits.available == 2);
    assert(credits.reserve(1793) && credits.available == 0);
    assert(credits.giveBack(2,2) && !credits.giveBack(2,1));
    assert(!credits.reserve(0xffffffffu));
    auto p = event(); Ath10k::WMIBootstrapState state;
    assert(!Ath10k::parseWMIBootstrapEvent(p.data(),p.size(),2,state));
    assert(state.serviceReady && state.rfChains == 2 && state.memoryRequests == 0);
    Ath10k::htcWriteLE32(p.data()+56,2139);Ath10k::htcWriteLE32(p.data()+60,864027058);Ath10k::htcWriteLE32(p.data()+64,65530);
    assert(!Ath10k::parseWMIBootstrapEvent(p.data(),p.size(),2,state));
    assert(state.htCapability==2139 && state.vhtCapability==864027058 && state.vhtMCS==65530);
    for (unsigned n = 0; n < p.size(); ++n) {
        Ath10k::WMIBootstrapState truncated;
        assert(Ath10k::parseWMIBootstrapEvent(p.data(),n,2,truncated));
        assert(!truncated.serviceReady);
    }
    p[12] = 105;
    assert(Ath10k::parseWMIBootstrapEvent(p.data(),p.size(),2,state));
    p = event(); p[14] = 33;
    assert(Ath10k::parseWMIBootstrapEvent(p.data(),p.size(),2,state));
    p = event(); p.resize(128); p[1] = 2; p[4] = 8; p[2] = 120;
    p[120] = 1; p[121] = 4; p[124] = 2; p[125] = 1;
    state.credits.initialize(2,1792,2); state.credits.reserve(100);
    assert(!Ath10k::parseWMIBootstrapEvent(p.data(),p.size(),2,state));
    assert(state.credits.available == 2);
    assert(Ath10k::parseWMIBootstrapEvent(p.data(),p.size(),2,state)); // duplicate return
    assert(state.credits.available == 2);
    WMIDriver good;
    assert(good.runWMIBootstrap());
    assert(good._htcSetupCompleteSent && good._wmi.serviceReady && good._wmi.rxOwned);
    assert(!good.runWMIBootstrap() && good.sent == 1);
    assert(good.quiesceTargetAfterBMI() && !good._wmi.rxOwned);
    auto avail = availableEvent();
    Ath10k::WMIBootstrapState avstate;
    assert(!Ath10k::parseWMIBootstrapEvent(avail.data(),avail.size(),2,avstate));
    assert(!avstate.serviceReady && avstate.availableCount == 1 && avstate.availableBits == 128);
    for (unsigned n = 0; n < avail.size(); ++n) {
        Ath10k::WMIBootstrapState broken;
        assert(Ath10k::parseWMIBootstrapEvent(avail.data(),n,2,broken));
        assert(broken.availableCount == 0);
    }
    avail[16] = 129;
    assert(Ath10k::parseWMIBootstrapEvent(avail.data(),avail.size(),2,avstate));
    WMIDriver sequence; sequence.availableFirst = true;
    assert(sequence.runWMIBootstrap());
    assert(sequence.sent == 1 && sequence._wmi.availableCount == 1);
    assert(sequence._wmi.rxCount == 2 && sequence._wmi.repostCount == 1);
    assert(sequence._wmi.slot == 0 && sequence._wmi.next == 1 && sequence._wmi.serviceReady);
    WMIDriver only; only.availableFirst = only.availableOnly = true;
    assert(!only.runWMIBootstrap() && only._wmi.waitMs == 5000);
    assert(only._wmi.availableCount == 1 && !only._wmi.serviceReady);
    WMIDriver flooding; flooding.availableFirst = flooding.flood = true;
    assert(!flooding.runWMIBootstrap());
    assert(!strcmp(flooding._wmi.status,"wmi-event-limit") && flooding._wmi.availableCount == 8);
    WMIDriver repost; repost.availableFirst = repost.failRepost = true;
    assert(!repost.runWMIBootstrap() && repost._wmi.rxOwned);
    assert(!strcmp(repost._wmi.status,"wmi-repost-doorbell-failed"));
    WMIDriver malformed; malformed.availableFirst = malformed.malformedAvailable = true;
    assert(!malformed.runWMIBootstrap() && malformed._wmi.repostCount == 0);
    WMIDriver timeout; timeout.absent = true;
    assert(!timeout.runWMIBootstrap() && timeout._wmi.waitMs == 5000);
    assert(!strcmp(timeout._wmi.status,"wmi-service-ready-timeout"));
    WMIDriver bad; bad.badAddress = true;
    assert(!bad.runWMIBootstrap() && !bad._wmi.serviceReady);
    WMIDriver crash; crash.crash = true;
    assert(!crash.runWMIBootstrap() && crash._boot.firmwareCrash);
    WMIDriver doorbell; doorbell._regs.wmiPostFail = true;
    assert(!doorbell.runWMIBootstrap() && doorbell._wmi.rxOwned && doorbell.sent == 0);
    doorbell.resetOK = false;
    assert(!doorbell.quiesceTargetAfterBMI() && doorbell._wmi.rxOwned && doorbell._dmaQuarantine);
    WMIDriver alloc; alloc._wmiRxBuffer.isAllocated = false; alloc._wmiRxBuffer.allocationFails = true;
    assert(!alloc.runWMIBootstrap() && !alloc._wmi.rxOwned && alloc.sent == 0);
    WMIDriver other; other.invalidEvent = true;
    assert(!other.runWMIBootstrap() && !strcmp(other._wmi.status,"wmi-unexpected-event"));
}

#endif
