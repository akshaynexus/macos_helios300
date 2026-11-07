// Compile the production boot/TX/quiesce method bodies against simulated
// ROM RAM, CE registers and DMA. This does not run on macOS or execute firmware.
#define ATH10K_OTP_FLOW_TEST
#include "test_board_flow.cpp"
#include "boot_methods.inc"
#include "quiesce_method.inc"
#include "release_method.inc"
#include <vector>

class BootDriver : public Ath10kPCIDevice {
public:
    std::vector<UInt8> packet{0, 0, 12, 0, 0, 0, 0, 0,
                            1, 0, 40, 0, 0, 1, 9, 0, 1, 8, 0, 0};
    unsigned doneCommands = 0, rxPolls = 0, completeAfter = 1;
    bool suppressReady = false, indexLags = false, indexFirst = false;
    bool badDescriptorAddress = false, crashAfterDone = false;
    bool failDoneDoorbell = false, doneTimeout = false;
    bool corruptFinalHI = false;
    bool corruptInitialOptionRead = false;
    UInt32 initialOptions = 0;
    UInt32 completionBytes = 0;
    UInt32 badReadAddress = 0xffffffffu;
    std::vector<UInt32> hiAddresses;

    void word(UInt32 addr, UInt32 value) {
        for (unsigned i = 0; i < 4; ++i) memory[addr + i] = (value >> (8 * i)) & 255;
    }
    explicit BootDriver(UInt32 options = 0x40000041, bool qca9377=false) : initialOptions(options) {
        if(qca9377){_deviceId=0x0042;_subsystemVendorId=0x1028;_subsystemId=0x1810;
            _selectedHw=Ath10k::findCandidate(0x0042,1);_bmiTargetVersion=0x05020001;}

        _regs.allowRX = true;
        // Nonzero slots, including CE1 wraparound, exercise handoff after BMI.
        _regs.written = _regs.read = 15;
        _regs.rxWritten = _regs.rxRead = 511;
        _htcReadyBuffer.isAllocated = false;
        _htcReadyBuffer.iovm = 0x200000;
        _ceDstRings[1].size = 4096;
        word(0x400810, options); // Preserve existing ROM option bits.
        word(0x400854, 0x401fc0);
        readHook = [this](UInt32 address, void *out, UInt32 length) {
            assert(!_boot.bmiClosed && !_boot.rxOwned);
            if (address == badReadAddress) return false;
            auto *bytes = static_cast<UInt8 *>(out);
            for (UInt32 i = 0; i < length; ++i) bytes[i] = memory[address + i];
            if (corruptFinalHI && _boot.attempted && address == 0x400904) bytes[0] ^= 1;
            if (corruptInitialOptionRead && !_boot.attempted &&
                _boot.optionApplied != 0xffffffffu && address == 0x400810) bytes[0] ^= 2;
            return true;
        };
        _regs.consume = [this]() {
            const auto *r = static_cast<const UInt32 *>(_bmiRequestBuffer.virtualAddress());
            const auto *ring = static_cast<const Ath10k::CEDescriptor32 *>(_ceSrcRings[0].virtualAddress());
            const auto &d = ring[_regs.read];
            if (r[0] == 1) {
                ++doneCommands;
                assert(d.nbytes == 4 && _boot.bmiClosed && _boot.doneAttempted);
                assert(_boot.rxOwned && _boot.rxPosted && _regs.rxPosts == 1);
                assert(_boot.preDoneHIVerified && _boot.uartDisabled);
                if (crashAfterDone) _regs.fwIndicator |= 1;
                return;
            }
            assert(r[0] == 3 && !_boot.bmiClosed && !_boot.rxOwned);
            assert(d.nbytes == r[2] + 12);
            if (Ath10k::isBootHIWord(r[1])) {
                assert(r[2] == 4);
                hiAddresses.push_back(r[1]);
            }
            for (UInt32 i = 0; i < r[2]; ++i)
                memory[r[1] + i] = static_cast<const UInt8 *>(_bmiRequestBuffer.virtualAddress())[12 + i];
            // Inject only after the UART write: all earlier setup remains valid.
            if (_boot.attempted && r[1] == 0x400814) {
                if (doneTimeout) _regs.timeout = true;
                if (failDoneDoorbell) _regs.failAtPost = _regs.posts + 1;
            }
        };
        _regs.rxPoll = [this]() {
            assert(_boot.doneSent && _boot.bmiClosed);
            ++rxPolls;
            if (indexFirst) _regs.rxRead = _boot.rxNext;
            if (suppressReady || rxPolls < completeAfter) return;
            auto *ring = static_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[1].virtualAddress());
            auto &d = ring[_boot.rxSlot];
            assert(packet.size() <= _htcReadyBuffer.length());
            std::memcpy(_htcReadyBuffer.virtualAddress(), packet.data(), packet.size());
            d.nbytes = completionBytes ? completionBytes : packet.size();
            if (badDescriptorAddress) d.address ^= 8;
            if (!indexLags) _regs.rxRead = _boot.rxNext;
        };
    }
    void load() {
        assert(configureBootHostInterest());
        assert(_boot.optionApplied == (initialOptions | 0x2208) && _boot.hiWrites == 5);
        assert(hiAddresses == std::vector<UInt32>({0x400800,0x400810,0x400844,0x400904,0x4008bc}));
        assert(runBMIBoardUploadTest());
        // OTP and all six WLAN segments have their own production-method
        // transport tests. Stand in for their validated results at this boundary.
        _otpDownloadSucceeded = _otpStreamClosed = _otpExecuteSucceeded = _otpResultAccepted = true;
        _otpResult = 3; _otpResultIgnored = true;
        assert(configureBootFirmwareDownload());
        _fwAttempted = _fwDownloadSucceeded = _fwStreamClosed = _fwPostBMIProbeSucceeded = _fwReadbackSucceeded = true;
        _fwVerifiedSegments = payload().kFirmwareSegmentCount; _fwReadbackBytes = payload().kFirmwareSegmentDataBytes;
        _regs.posts = 0;
    }
};

static void noRetry(BootDriver &d)
{
    const auto posts = d._regs.posts, rxPosts = d._regs.rxPosts;
    assert(!d.runFirmwareBootTest());
    const UInt32 done = 1, other = 8;
    assert(!d.bmiSendOnly(&done, 4));
    assert(!d.bmiSendOnly(&other, 4));
    assert(!d.writeBootHIWord(0x400800, 2));
    assert(d._regs.posts == posts && d._regs.rxPosts == rxPosts);
}

int main()
{
    { BootDriver d(0x40000041,true); d.load(); assert(d.memory[0x4008bc]==0x42);
      assert(d.runFirmwareBootTest() && d._boot.readyValidated && d.doneCommands==1); }

    // Replay the exact 0.0.20 hardware failure: host wrote 0x2208, the final
    // target read returned 0x220a. This must reach DONE/READY without rewriting
    // the target's credit flag or adding any extra HI transactions.
    { BootDriver d(0); d.load(); d.word(0x400810, 0x220a);
      assert(d._boot.optionApplied == 0x2208);
      assert(d.runFirmwareBootTest() && d.doneCommands == 1 && d._boot.readyValidated);
      assert(d._boot.optionFinalRead && d._boot.optionFinalVerified);
      assert(d._boot.optionAfterDownload == 0x220a && d._boot.optionChangedBits == 2);
      assert(d._boot.optionBMICreditLimitAdded && d.memory[0x400810] == 0x0a);
      assert(d._boot.hiWrites == 6 && d._boot.hiVerified == 11 && d.hiAddresses.size() == 6);
      assert(d.quiesceTargetAfterBMI()); }
    // Every other bit still blocks the transition, even when credit limit
    // becomes set at the same time. No DONE or HTC DMA may be submitted.
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (bit == 1) continue;
        BootDriver d(0); d.load();
        d.word(0x400810, (0x2208u ^ (1u << bit)) | 2u);
        assert(!d.runFirmwareBootTest() && !d._boot.optionFinalVerified);
        assert(!d._boot.doneAttempted && d._regs.rxPosts == 0);
    }
    // Only adding the observed flag is permitted; clearing a previously set
    // credit-limit bit is not covered by the hardware evidence.
    { BootDriver d(2); d.load(); d.word(0x400810, 0x2208);
      assert(!d.runFirmwareBootTest() && !d._boot.doneAttempted); }
    { BootDriver d(2); d.load();
      assert(d.runFirmwareBootTest() && !d._boot.optionBMICreditLimitAdded);
      assert(d._boot.optionChangedBits == 0 && d._boot.optionFinalVerified); }
    { BootDriver d(0); d.load(); d.badReadAddress = 0x400810;
      assert(!d.runFirmwareBootTest() && !d._boot.optionFinalRead && !d._boot.doneAttempted); }
    { BootDriver d(0); d.load(); d.word(0x400810, 0xffffffffu);
      assert(!d.runFirmwareBootTest() && !d._boot.optionFinalVerified && !d._boot.doneAttempted); }
    { BootDriver d(0); d.corruptInitialOptionRead = true;
      assert(!d.configureBootHostInterest() && !d._boot.configSucceeded);
      assert(d._boot.lastHIExpected == 0x2208 && d._boot.lastHIObserved == 0x220a); }
    // The bit exception must not affect any other Host Interest word.
    for (const auto &bad : std::vector<std::pair<UInt32, UInt32>>{
            {0x400800, 0}, {0x400844, 2}, {0x400904, 2}, {0x4008bc, 0x3c}}) {
        BootDriver d; d.load(); d.word(bad.first, bad.second);
        assert(!d.runFirmwareBootTest() && !d._boot.doneAttempted && d._regs.rxPosts == 0);
    }
    { BootDriver d; d.load();
      assert(d.runFirmwareBootTest());
      assert(d.doneCommands == 1 && d._regs.rxPosts == 1 && d._boot.rxSlot == 511 && d._boot.rxNext == 0);
      assert(d._boot.readyValidated && d._boot.doneSent && d._boot.ready.credits == 40);
      assert(d._boot.optionFinalVerified && d._boot.optionChangedBits == 0);
      assert(d._boot.hiWrites == 6 && d._boot.hiVerified == 11 && d._bmiWriteMemoryTransactionCount == 41);
      assert(d._boot.rxBytes == 20 && d._boot.prefixBytes == 20 && d._boot.rxOwned);
      assert(d._otpResult == 3 && !std::strcmp(d._boot.status, "htc-ready-before-reset"));
      noRetry(d);
      assert(d.quiesceTargetAfterBMI() && d.resets == 1 && !d._boot.rxOwned);
      assert(d._boot.readyValidated && d._boot.bmiClosed && d.stopDMAForRelease()); }
    { BootDriver d; d.load(); d.packet.resize(16); d.packet[2] = 8;
      assert(d.runFirmwareBootTest() && !d._boot.ready.extended); }
    { BootDriver d; d.load(); d.indexLags = true;
      assert(d.runFirmwareBootTest() && d._boot.rxRead == 511); }
    { BootDriver d; d.load(); d.indexFirst = true; d.completeAfter = 4;
      assert(d.runFirmwareBootTest() && d._boot.waitMs == 3); }
    { BootDriver d; d.load(); d.suppressReady = true;
      assert(!d.runFirmwareBootTest() && d._boot.timedOut && !d._boot.readyValidated);
      assert(d._boot.waitMs == 3000 && d._boot.doneSent && d._boot.rxOwned);
      noRetry(d); assert(d.quiesceTargetAfterBMI() && !d._boot.rxOwned); }
    { BootDriver d; d.load(); d.doneTimeout = true;
      assert(!d.runFirmwareBootTest() && d._bmiTimedOut && d._bmiTransferUncertain);
      assert(d._boot.doneAttempted && !d._boot.doneSent && d.rxPolls == 0);
      noRetry(d); assert(d.quiesceTargetAfterBMI() && !d._bmiTransferUncertain); }
    { BootDriver d; d.load(); d.failDoneDoorbell = true;
      assert(!d.runFirmwareBootTest() && d._boot.bmiClosed && d._bmiTransferUncertain);
      assert(!d._boot.doneSent); noRetry(d); }
    { BootDriver d; d.load(); d.packet[8] = 3;
      assert(!d.runFirmwareBootTest() && d._boot.rxCompleted && !d._boot.timedOut);
      assert(!std::strcmp(d._boot.parseError, "unexpected-message-id")); noRetry(d); }
    { BootDriver d; d.load(); d.packet.resize(7);
      assert(!d.runFirmwareBootTest() && !d._boot.readyValidated && d._boot.prefixBytes == 7); }
    { BootDriver d; d.load(); d.completionBytes = 2049;
      assert(!d.runFirmwareBootTest() && d._boot.rxCompleted && d._boot.prefixBytes == 0); }
    { BootDriver d; d.load(); d.badDescriptorAddress = true;
      assert(!d.runFirmwareBootTest() && !d._boot.readyValidated); }
    { BootDriver d; d.load(); d.crashAfterDone = true;
      assert(!d.runFirmwareBootTest() && d._boot.firmwareCrash); }
    { BootDriver d; d.load(); d._regs.fwIndicator = 0xffffffffu;
      assert(!d.runFirmwareBootTest() && !d._boot.doneAttempted && d._regs.rxPosts == 0); }
    { BootDriver d; d.load(); d._regs.rxWritten = 510;
      assert(!d.runFirmwareBootTest() && !d._boot.doneAttempted && !d._boot.rxOwned); }
    { BootDriver d; d.load(); d._regs.rxCtrl1 = 4096;
      assert(!d.runFirmwareBootTest() && d._regs.rxPosts == 0); }
    { BootDriver d; d.load(); d.corruptFinalHI = true;
      assert(!d.runFirmwareBootTest() && !d._boot.preDoneHIVerified && d._regs.rxPosts == 0); }
    { BootDriver d; d.load(); d.word(0x400858, 0);
      assert(!d.runFirmwareBootTest() && !d._boot.doneAttempted); }
    { BootDriver d; d.load(); d.word(0x400854, 0x400000);
      assert(!d.runFirmwareBootTest() && !d._boot.doneAttempted); }
    { BootDriver d; d.load(); d._regs.rxDoorbellFails = true;
      assert(!d.runFirmwareBootTest() && d._boot.rxOwned && !d._boot.rxPosted && !d._boot.doneAttempted);
      assert(d.quiesceTargetAfterBMI()); }
    { BootDriver d; d.load(); d._regs.rxLatchMismatch = true;
      assert(!d.runFirmwareBootTest() && d._boot.rxOwned && !d._boot.doneAttempted); }
    { BootDriver d; d.load(); d._htcReadyBuffer.allocationFails = true;
      assert(!d.runFirmwareBootTest() && !d._boot.rxOwned && d._regs.rxPosts == 0); }
    for (unsigned failure = 0; failure < 4; ++failure) {
        BootDriver d; d.load();
        if (failure == 0) d._htcReadyBuffer.syncOK = false;
        if (failure == 1) d._ceDstRings[1].syncOK = false;
        if (failure == 2) d._ceDstRings[1].syncInOK = false;
        if (failure == 3) d._htcReadyBuffer.syncInOK = false;
        assert(!d.runFirmwareBootTest() && !d._boot.readyValidated);
        assert(d._boot.doneSent == (failure >= 2));
        assert(d.quiesceTargetAfterBMI());
    }
    for (unsigned failure = 0; failure < 5; ++failure) {
        BootDriver d; d.load(); assert(d.runFirmwareBootTest());
        if (failure == 0) d.resetOK = false;
        if (failure == 1) d.wakeOK = false;
        if (failure == 2) d.deinitOK = false;
        if (failure == 3) d.pci.disableWorks = false;
        if (failure == 4) d.pci.command = 0xffff;
        assert(!d.quiesceTargetAfterBMI() && d._dmaQuarantine && d._boot.rxOwned);
        assert(d._htcReadyBuffer.allocated());
        assert(d.stopDMAForRelease() == (failure < 3));
        d._pci = nullptr;
        assert(!d.stopDMAForRelease()); // Must not unmap on unverifiable shutdown.
    }
    for (unsigned failure = 0; failure < 5; ++failure) {
        BootDriver d; d.load();
        if (failure == 0) d._fwReadbackSucceeded = false;
        if (failure == 1) d._otpResultAccepted = false;
        if (failure == 2) d._fwVerifiedSegments = 5;
        if (failure == 3) d._boot.downloadConfigSucceeded = false;
        if (failure == 4) d._bmiTransferUncertain = true;
        assert(!d.runFirmwareBootTest() && !d._boot.attempted && d._regs.rxPosts == 0);
    }
    for (unsigned failure = 0; failure < 5; ++failure) {
        BootDriver d;
        if (failure == 0) d._subsystemId = 0xffff;
        if (failure == 1) d._bmiTargetVersion = 0;
        if (failure == 2) d._bmiBoardDataAddress = 0x400000;
        if (failure == 3) d._bmiBoardPrereqSucceeded = false;
        if (failure == 4) d._bmiBoardUploadAttempted = true;
        assert(!d.configureBootHostInterest() && d._regs.posts == 0);
    }
    for (unsigned failure = 1; failure <= 5; ++failure) {
        BootDriver d; d._regs.failAtPost = failure;
        assert(!d.configureBootHostInterest() && d._regs.posts == failure);
        assert(!d.configureBootHostInterest() && d._regs.posts == failure);
        assert(!d._boot.configSucceeded);
    }
    { BootDriver d; d.badReadAddress = 0x400810;
      assert(!d.configureBootHostInterest() && d._boot.hiWrites == 1); }
    { BootDriver d;
      assert(!d.writeBootHIWord(0x400800, 2));
      d._boot.configAttempted = true;
      assert(!d.writeBootHIWord(0x401000, 2) && d._regs.posts == 0); }
    { BootDriver d; UInt32 done = 1;
      assert(!d.bmiSendOnly(&done, sizeof(done)) && d._regs.posts == 0); }
}
