#include <cstdio>
#include <cstdlib>
// The runner includes the three actual driver method bodies from the .cpp.
// This ROM/CE0 model is NOT a macOS kernel or physical hardware test.
#include "Ath10kBMI.hpp"
#include "Ath10kBoot.hpp"
#include "Ath10kWMI.hpp"
#include "Ath10kRxSetup.hpp"
#include "Ath10kRX.hpp"
#include "Ath10kPayload.hpp"
#include "Ath10kHw.hpp"
#include "Ath10kPerformance.hpp"
#include "Ath10kPower.hpp"
#include "Ath10kPLL.hpp"
#include <array>
#include <vector>
#include <cassert>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <strings.h>

using IOSimpleLock=int;
[[maybe_unused]] static void IOSimpleLockFree(IOSimpleLock *p){delete p;}
static void IOLog(const char *, ...) {}
[[maybe_unused]] static void IOFree(void *p,size_t){free(p);}
static void OSSynchronizeIO() {}
static void IOSleep(unsigned) {}
[[maybe_unused]] static void IODelay(unsigned) {}
namespace Ath10k {
constexpr UInt32 kCEHostIE = 0x2c, kCEMiscIE = 0x34,kCEHostIS=0x30,kCEMiscIS=0x38,kCEHostInterruptMask=0x1f;
constexpr UInt32 kCESrcWriteIndex = 0x3c, kCECurrentSrcRead = 0x44;
constexpr UInt32 kCEDstWriteIndex = 0x40, kCECurrentDstRead = 0x48;
constexpr UInt32 kCECtrl1SrcByteSwapMask=0x10000,kCECtrl1DstByteSwapMask=0x20000;
constexpr UInt32 kCECtrl1DmaxMask = 0xffff, kCEDescriptorRingAlign = 8;
struct CEDescriptor32 { UInt32 address; UInt16 nbytes, flags; };
struct CEHostConfig { UInt32 srcEntries, dstEntries, srcSizeMax=2048; };
constexpr UInt32 kCECount=8;
constexpr CEHostConfig kCEHostConfig[] = {{16, 0}, {0, 512}, {0, 128}, {32, 0}, {8192,0,256}, {0,0}, {0,0}, {2,2}};
struct CESnapshot { UInt32 srcWriteIndex, currentSrcRead, dstWriteIndex, currentDstRead, ctrl1;
    UInt32 srcBase=0,srcSize=0,dstBase=0,dstSize=0,hostIE=0,miscIE=0,srcWatermark=0,dstWatermark=0,command=0; };
UInt32 ceRingNext(UInt32 n, UInt32 count) { return (n + 1) % count; }
UInt32 ceBaseAddress(UInt32 n) { assert(n <= 4); return n * 0x400; }
}
struct Buffer {
    enum class Role { Ring, Payload };
    Role role=Role::Ring;
    std::vector<UInt64> data=std::vector<UInt64>(512);
    std::vector<UInt64> deviceData{};
    bool bounce=false;
    unsigned syncInCount=0,syncOutCount=0;
    bool syncOK = true, syncInOK = true, isAllocated = true, allocationFails = false;
    UInt64 size = 256, iovm = 0x100000;
    void release(){isAllocated=false;}
    bool allocated() const { return isAllocated; }
    bool allocate(UInt64 n, UInt64 alignment, void *,Role r=Role::Ring) {
        role=r;
        assert(alignment == 8 || alignment==16 || alignment==128);
        data.resize((n+7)/8);
        if (allocationFails) return false;
        size = n; isAllocated = true; return true;
    }
    void *virtualAddress() { return data.data(); }
    UInt64 length() const { return size; }
    UInt64 dmaAddress() const { return iovm; }
    bool syncForDevice() {
        if(!syncOK)return false;
        ++syncOutCount;if(bounce)deviceData=data;return true;
    }
    bool syncFromDevice() {
        if(!syncInOK)return false;
        ++syncInCount;if(bounce){assert(deviceData.size()==data.size());data=deviceData;}
        return true;
    }
};
struct Registers {
    template<class... T> void setAccessHook(T...){}
    UInt32 written = 0, read = 0;
    UInt32 tx3Written=0, tx3Read=0, tx4Written=0, tx4Read=0;
    bool tx4Fail=false,tx4Timeout=false;
    std::function<void()> tx4Consume;
    bool tx3Fail=false, tx3Timeout=false;
    std::function<void()> tx3Consume;
    UInt32 wmiWritten = 0, wmiRead = 0;
    bool wmiPostFail = false;
    std::function<void()> wmiPost;
    bool snapshotOK = true, timeout = false, doorbellFailure = false;
    unsigned posts = 0;
    unsigned failAtPost = 0;
    bool allowRX = false, rxDoorbellFails = false, rxLatchMismatch = false, mmioFails = false;
    UInt32 rxWritten = 0, rxRead = 0, rxCtrl1 = 2048, fwIndicator = 2;
    unsigned rxPosts = 0;
    std::function<void()> consume;
    std::function<void()> rxPoll;
    bool diagnosticReadFails=false;
    unsigned diagnosticReads=0;
    unsigned miscReads=0;UInt32 miscStatus=0;
    bool romSnapshot=false,romCE1Snapshot=false;
    UInt32 coreValue=0x8508;
    UInt32 wakeValue=1,rtcValue=3;
    bool read32(UInt32 address, UInt32 &value) {
        if(address==Ath10k::socCoreOffset(Ath10k::kQca6174RegisterLayout,Ath10k::kCoreCtrlAddress)){value=coreValue;return !mmioFails;}
        if(address==Ath10k::kCEHostIS || address==0x400+Ath10k::kCEHostIS){
            ++diagnosticReads;value=0;return !diagnosticReadFails;
        }
        if(address==Ath10k::kCEMiscIS || address==0x400+Ath10k::kCEMiscIS){
            ++miscReads;value=miscStatus;return !diagnosticReadFails;
        }
        if(address==Ath10k::socWakeOffset(Ath10k::kQca6174RegisterLayout)){value=wakeValue;return !mmioFails;}
        if(address==Ath10k::rtcStateOffset(Ath10k::kQca6174RegisterLayout)){value=rtcValue;return !mmioFails;}
        if(address==0x103c || address==0x1044){value=address==0x103c?tx4Written:tx4Read;return true;}
        if (address==0xc3c || address==0xc44) {value=address==0xc3c?tx3Written:tx3Read;return true;}
        if (address == 0x82c || address == 0x834) { value = 0; return true; }
        if (address == 0x840 || address == 0x848) {
            value = address == 0x840 ? wmiWritten : wmiRead; return !mmioFails;
        }
        if (allowRX && address == Ath10k::kQca6174RegisterLayout.fwIndicatorAddress) {
            value = fwIndicator; return !mmioFails;
        }
        if (allowRX && (address == 0x440 || address == 0x448)) {
            if (address == 0x448 && rxPoll) rxPoll();
            value = address == 0x440 ? rxWritten + (rxLatchMismatch ? 1 : 0) : rxRead;
            return !mmioFails;
        }
        assert(address == Ath10k::kCESrcWriteIndex || address == Ath10k::kCECurrentSrcRead);
        value = address == Ath10k::kCESrcWriteIndex ? written : read;
        return true;
    }
    bool write32(UInt32 address, UInt32 value) {
        if(address==0x103c){tx4Written=value;if(tx4Fail)return false;if(!tx4Timeout){if(tx4Consume)tx4Consume();tx4Read=value;}return true;}
        if (address==0xc3c) {tx3Written=value;if(tx3Fail)return false;if(!tx3Timeout){if(tx3Consume)tx3Consume();tx3Read=value;}return true;}
        if (address == 0x82c || address == 0x834) return value == 0;
        if (address == 0x840) { wmiWritten = value; if (wmiPostFail) return false; if (wmiPost) wmiPost(); return true; }
        if (allowRX && address == 0x440) {
            ++rxPosts; rxWritten = value; return !rxDoorbellFails;
        }
        // Board/OTP/WLAN tests do not allow CE1 posts.
        assert(address == Ath10k::kCESrcWriteIndex);
        ++posts;
        written = value;
        if (doorbellFailure || posts == failAtPost) return false;
        if (!timeout) { consume(); read = value; }
        return true;
    }
};
namespace Ath10k {
bool readCESnapshot(Registers &r, UInt32 ce, CESnapshot &s) {
    assert(ce <= 4);
    if (ce == 4) {s={r.tx4Written,r.tx4Read,0,0,256};return r.snapshotOK;}
    if (ce == 3) {s={r.tx3Written,r.tx3Read,0,0,2048};return r.snapshotOK;}
    if (ce == 2) { s = {0, 0, r.wmiWritten, r.wmiRead, 2048}; return r.snapshotOK; }
    s = {r.written, r.read, r.rxWritten, r.rxRead, r.rxCtrl1};
    if(ce==0 && r.romSnapshot){s.dstBase=0xa114ee48;s.dstSize=32;}
    if(ce==1 && r.romCE1Snapshot){s.srcBase=0xa114ec68;s.srcSize=32;s.srcWriteIndex=s.currentSrcRead=0;}
    return r.snapshotOK;
}
}
static constexpr UInt32 kIOPCIConfigCommand = 4;
struct FakePCI {
    UInt16 command = 6;
    bool disableWorks = true;
    void setBusMasterEnable(bool enabled) {
        assert(!enabled);
        if (disableWorks && command != 0xffff) command &= ~4u;
    }
    UInt16 configRead16(UInt32 address) { assert(address == 4); return command; }
};
#ifdef ATH10K_WMI_RUNTIME_TEST
#include "wmi_worker_mocks.hpp"
#endif
using Ath10kDMABuffer=Buffer;
class Ath10kPCIDevice {
public:
#ifndef ATH10K_WMI_RUNTIME_TEST
    bool powerIOBlocked(){return false;}
#endif
    const Ath10kPayload::Profile &payload() const {
        return Ath10kPayload::selectProfile(_vendorId, _deviceId, _subsystemVendorId,
            _subsystemId, _selectedHw ? _selectedHw->chipRevision : 0xff);
    }
    bool payloadMatchesTarget() const {
        return payload().matchesTarget(_bmiTargetVersion, _bmiTargetType, _bmiTargetInfoLength);
    }

public:
#ifdef ATH10K_WMI_RUNTIME_TEST
    using super=MockService;
    Ath10k::PowerState _power{};
    volatile UInt32 _powerIOBlock=0;
    void *_powerNotifier=nullptr;
    bool powerIOBlocked();
    bool powerRestartAllowed();
    void requestPowerEvent(Ath10k::PowerEvent);
    void publishPowerState();
    static IOReturn controlPowerState(OSObject *,void *,void *,void *,void *);
    const char *serviceRuntime();
    const char *startRuntimeWorkerIRQ(){return nullptr;}
    bool prepareRuntimePower(){return true;}
    bool finishRuntimePower(){return true;}
    void publishRuntimePower(){}
    const char *idleRuntimePower(bool,bool &asleep){asleep=false;return nullptr;}
    void stopRuntimeWorkerIRQ(){_runtimeIRQArmed=false;}
    void pauseRXReserve(){}
    bool maskRuntimeWorkerIRQ(){return true;}
    bool unmaskRuntimeWorkerIRQ(){return true;}
    IOSimpleLock *_runtimeIRQLock=nullptr;
    IOSimpleLock *_runtimeWakeLock=nullptr;
    IOFilterInterruptEventSource *_runtimeIRQ=nullptr;
    bool _runtimeIRQAdded=false,_runtimeIRQArmed=false,_runtimeIRQMaskFailed=false,_runtimeIRQDeferred=false,_systemSuspended=false;
    UInt64 _nextRxDoneRetryMs=0;
    UInt32 _lastMaintenanceRxPackets=0,_lastMaintenanceTxPackets=0;
    bool prepareWMIRuntime();
    void endWMIRuntime(const char *);
    void releaseWMIRuntime();
    void publishWMIRuntimeProperties();
    static void wmiTimerFired(OSObject *,IOTimerEventSource *);
    static IOReturn controlWMIRuntime(OSObject *,void *,void *,void *,void *);
    IOReturn setPowerState(unsigned long,IOService *);
    void systemWillShutdown(IOOptionBits);
    IOWorkLoop *_wmiWorkLoop=nullptr;
    IOTimerEventSource *_wmiTimer=nullptr;
    bool _wmiTimerAdded=false,_runtimeWakeAsserted=true;
    const char *_runtimeStatus="active";
    bool irqStopOK=true;
    unsigned irqStops=0,published=0;
    bool stopRuntimeCEIRQ(){++irqStops;_runtimeActive=false;_runtimeCE1Armed=false;return irqStopOK;}
    void restorePCIeBringupParity(){}
    void publishDiagnosticProperties(){++published;}
    template<class... T> void setProperty(T...){}
#endif
    bool htcRepostRuntimeReceive();
    bool htcSubmitControlFrame(const UInt8 *, UInt32, bool, UInt16, UInt8 *, UInt32 &);
    bool runHTCServiceSetup();
    bool runWMIBootstrap();
    bool repostWMIBuffer();
    bool runWMIInit();
    const char *serviceWMIEcho(UInt64 nowMs);
    const char *serviceWMIVdevCreate(UInt64 nowMs);
    Ath10k::WMIVdevCreateState _wmiVdevCreate{};
    const char *serviceWMIScanChannels(UInt64 nowMs);
    Ath10k::WMIScanChannelsState _wmiScanChannels{};
    Ath10k::RegulatoryPolicy _regulatory{};
    Ath10k::WMIScanChannelsState _wmiRadio[5]{};
    const char *serviceWMIRadio(UInt64 nowMs, UInt32 index);
    const char *serviceRxSetup(UInt64 nowMs);
    const char *sampleHTTRxPool();
    const char *serviceHTTRx();
    const char *serviceHTTRxOne(bool &);
    Ath10k::RxSetupState _rxSetup{};
    void *_controlOwner=nullptr;
    Ath10k::ConnectionState _connection{};
    Ath10k::WMIEchoState _connectionEcho{};
    Buffer _connectionCommand{},_packetBuffer{},_packetCommand{},_packetFragments{};
    bool _networkEnabled=false,networkUp=false,_dataTXStalled=false;
    bool _rxDeliveryBatch=false;
    Ath10k::PerformanceState _perf{};
    void flushNetworkRX(){}
    const char *serviceConnection(UInt64);
    const char *serviceDisconnect(UInt64);
    const char *serviceChannelSwitch(UInt64);
    const char *serviceRadioMaintenance(UInt64);
    const char *serviceAPMaintenance(UInt64);
    const char *serviceStatsMaintenance(UInt64);
    const char *serviceDataTX(UInt64);
    const char *submitDataPacket(const UInt8 *,UInt32,UInt64);
    struct DataBuffers {Buffer payload{},command{},fragments{};};
    DataBuffers _dataBuffers[Ath10k::kDataTxSlots]{};
    const char *submitNetworkCE(UInt32,Buffer &,UInt32,UInt32 &);
    const char *submitNetworkPacket(const UInt8 *,UInt32,bool,UInt64);
    void networkLink(bool up){networkUp=up;}
    void publishConnection(){}
    unsigned receivedFrames=0;
    Ath10k::RXBatch _rxBatch{};
    // Host scenarios create many driver doubles in one stack frame. Keep the
    // real RX state on the heap, as it is in the actual allocated IOService.
    std::unique_ptr<Ath10k::RXReorder> reorderStorage=std::make_unique<Ath10k::RXReorder>();
    std::unique_ptr<Ath10k::RXDefrag> defragStorage=std::make_unique<Ath10k::RXDefrag>();
    Ath10k::RXReorder &_rxReorder=*reorderStorage;
    Ath10k::RXDefrag &_rxDefrag=*defragStorage;
    UInt8 _rxAssembly[Ath10k::kRXAssemblyBytes]{};
    const UInt8 *_rxServiceData[256]{};
    void finishNetworkRX(){if(!_rxBatch.failed)receivedFrames+=_rxBatch.count;}
    void serviceRXReorder(UInt64){}
    void deliverNetworkRX(const UInt8 *d,UInt8 status,UInt16 peer,UInt16 length=0,UInt32 availableBytes=2048){
        Ath10k::appendRXBatch(_connection,_rxBatch,d,availableBytes,status,peer,length);
    }
    struct Queue {UInt32 wakes=0;void service(unsigned options){assert(options==1);++wakes;}} queue;
    bool queueEnabled=false;
    Queue *getOutputQueue(){return queueEnabled?&queue:nullptr;}
#ifdef ATH10K_WMI_RUNTIME_TEST
    IOWorkLoop *getWorkLoop(){return IOWorkLoop::workLoop();}
#endif

    Ath10k::WMIEchoState _filterEcho{};
    Buffer _httRecycled[Ath10k::kHTTRxPosted]{};
    Buffer _httTxBuffer{},_httRxIndex{},_httRxRing{},_httRxPool{};
    const char *serviceWMIScan(UInt64 nowMs);
    Ath10k::WMIScanState _wmiScan{};
    // IOKit allocates the production device on the heap. Keep its large
    // persistent RF cache off the host test's main() stack as well.
    std::unique_ptr<Ath10k::WMIBSSCache> _bssStorage=std::make_unique<Ath10k::WMIBSSCache>();
    Ath10k::WMIBSSCache &_wmiBSS=*_bssStorage;
    Ath10k::WMIEchoState _wmiEcho{};
    const char *drainWMIRuntime();
    Ath10k::WMIRuntimeState _wmiLive{};
    Ath10k::WMIInitState _wmiEvents{};
    Buffer _wmiTxBuffer;
    Ath10k::WMIInitState _wmiInit{};
    Buffer _wmiHostMemory[Ath10k::kWMIHostMemoryMax];
    Buffer _wmiRxBuffer;
    Ath10k::WMIBootstrapState _wmi{};
    Buffer _htcControlBuffer;
    bool _runtimeActive = true, _runtimeCE1Armed = true;
    UInt32 _runtimeRxSlot = 0, _runtimeRxNext = 1;
    UInt32 _runtimeLastCE1Read = 0, _runtimeLastCE1Write = 1;
    UInt32 _runtimeRxPostedCount = 1, _runtimeRxReplenishedCount = 1;
    UInt32 _runtimeRxCompletedCount = 0, _runtimeLastRxBytes = 0;
    bool _htcAttempted = false;
    bool _htcSetupCompleteSent = false;
    bool _htcReservedControlConnected = false;
    bool _htcServiceSetupSucceeded = false;
    bool _htcCreditFlowDeferred = true;
    UInt32 _htcControlTxCount = 0;
    UInt32 _htcControlRxCount = 0;
    UInt32 _htcControlLastTxBytes = 0;
    UInt32 _htcControlLastRxBytes = 0;
    UInt32 _htcControlLastTxSlot = 0xffffffffu;
    UInt32 _htcControlLastTxNext = 0xffffffffu;
    UInt32 _htcControlLastRxSlot = 0xffffffffu;
    UInt32 _htcControlLastRxNext = 0xffffffffu;
    UInt32 _htcControlLastWaitMs = 0;
    const char *_htcStatus = "not-run";
    Ath10k::HTCServiceInfo _htcReservedControl{};
    Ath10k::HTCServiceInfo _htcWMIControl{};
    Ath10k::HTCServiceInfo _htcHTTData{};
    bool bmiSendOnly(const void *, UInt32);
    bool bmiWriteMemory(UInt32, const void *, UInt32);
    bool bmiReadSOCRegister(UInt32,UInt32 &);
    bool bmiWriteSOCRegister(UInt32,UInt32);
    bool bmiWritePLLWord(UInt32,UInt32);
    bool configureTargetPLL();
    Ath10k::PLLState _pll{};
    bool runBMIBoardUploadTest();
    bool bmiLZStreamStart(UInt32);
    bool bmiLZData(const UInt8 *, UInt32);
    bool runBMIOtpTest();
    bool bmiFirmwareLZStart(UInt32);
    bool bmiFirmwareLZData(const UInt8 *, UInt32);
    bool verifyFirmwareSegments();
    bool runBMIFirmwareDownloadTest();
    bool writeBootHIWord(UInt32, UInt32);
    bool verifyBootHIWord(UInt32, UInt32);
    bool verifyBootHIOptionsAfterDownload();
    bool configureBootHostInterest();
    bool configureBootFirmwareDownload();
    bool finishBootHostInterest();
    bool prepareHTCReadyReceive();
    bool waitForHTCReady();
    bool runFirmwareBootTest();
    void captureBMIROMEvidence(bool beforeSubmit=false);
    std::function<bool(UInt32,void *,UInt32)> diagReadHook;
    bool ce7DiagnosticRead(UInt32 a,void *p,UInt32 n){return diagReadHook && diagReadHook(a,p,n);}
    bool quiesceTargetAfterBMI();
    bool stopDMAForRelease();
    bool resetOK = true, wakeOK = true, deinitOK = true;
    unsigned resets = 0;
    bool pulseColdResetForQuiesce() { ++resets; return resetOK; }
    bool wakeTarget(bool &asserted) { asserted = wakeOK; return wakeOK; }
    bool deinitCopyEnginePipes(const char *) { if (deinitOK) _cePipesProgrammed = false; return deinitOK; }
    void releaseTargetWake(bool) {}
    FakePCI pci;
    FakePCI *_pci = &pci;
    void *_dmaMapper = nullptr;
    std::function<bool(const void *, UInt32, void *, UInt32 &)> exchange;
    bool bmiExchange(const void *req, UInt32 n, void *resp, UInt32 &rn) {
        assert(exchange); return exchange(req, n, resp, rn);
    }
    bool bmiTransportExchange(const void *,UInt32,void *,UInt32 &);
    bool _bmiPipeValidationAttempted=false,_bmiPipeStrictValidationMismatch=false,_bmiPipeStructuralValidationSucceeded=false;
    UInt32 _bmiCE1DstWrite=0,_bmiCE1DstRead=0,_bmiCompletedBytes=0;
    // Retained across quiesce/reset; overwritten only by a new GET_TARGET_INFO attempt.
    UInt32 _bmiROMStatus=0, _bmiROMCore=0xffffffffu, _bmiROMFW=0xffffffffu;
    bool _bmiROMCoreRead=false, _bmiROMFWRead=false, _bmiROMStable=false;
    UInt32 _bmiROMDescriptorCPU=0, _bmiROMBufferCPU=0, _bmiROMWord=0xffffffffu;
    UInt32 _bmiROMDescriptorAddress=0, _bmiROMDescriptorBytes=0, _bmiROMDescriptorFlags=0;
    UInt32 _bmiROMSlot=0, _bmiROMReadCount=0;
    UInt32 _bmiROMVersion=0xffffffffu,_bmiROMType=0xffffffffu;
    UInt32 _bmiROMCE1DescriptorCPU=0,_bmiROMCE1Address=0,_bmiROMCE1Bytes=0,_bmiROMCE1Flags=0;
    bool _bmiROMResponseCandidate=false,_bmiROMCE1Stable=false;
    bool _bmiEvidenceEntered=false, _bmiEvidenceCondition=false;
    UInt32 _bmiEvidenceRequestLength=0, _bmiEvidenceCommand=0xffffffffu;
    bool _bmiEvidenceCaptured[2]={false,false};
    UInt32 _bmiEvidenceDescriptor[2][6]{};
    UInt32 _bmiEvidenceCE[2][2][18]{};
    UInt32 _bmiMiscStatus[2][2]{};bool _bmiMiscRead[2][2]{};
    UInt32 _bmiROMPreStatus=0,_bmiROMPreDescriptorCPU=0,_bmiROMPreBufferCPU=0,_bmiROMPreWords[3]{};
    bool _bmiROMPreStable=false;
    UInt32 _bmiWakeAtTimeout=0xffffffffu,_bmiRTCAtTimeout=0xffffffffu;
    bool strictOK=true;
    bool verifyCopyEnginePipe(UInt32,const Ath10k::CESnapshot &) const {return strictOK;}
    bool structureOK = true;
    bool validateBMIPipeStructure(UInt32 ce, const Ath10k::CESnapshot &) { assert(ce <= 4); return structureOK; }
    std::function<bool(UInt32, void *, UInt32)> readHook;
    bool bmiReadMemory(UInt32 addr, void *out, UInt32 count) {
        if (readHook) return readHook(addr, out, count);
        if (readFailure) return false;
        auto *bytes = static_cast<UInt8 *>(out);
        for (UInt32 i = 0; i < count; ++i) bytes[i] = memory[addr+i];
        if (corruptReadback && addr == _bmiBoardDataAddress) bytes[0] ^= 1;
        return true;
    }
    Ath10kPCIDevice() {
        _regs.consume = [this]() {
            auto *ring = static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[0].virtualAddress());
            const auto &desc = ring[_regs.read];
            assert(desc.nbytes >= 16 && desc.nbytes <= 256);
            auto *r = static_cast<Ath10k::BMIWriteMemoryRequest *>(_bmiRequestBuffer.virtualAddress());
            assert(r->id == 3 && r->length + 12 == desc.nbytes && r->length % 4 == 0);
            for (UInt32 i = 0; i < r->length; ++i) memory[r->address+i] = r->payload[i];
        };
    }
    Registers _regs;
    Buffer _bmiRequestBuffer, _bmiResponseBuffer, _ceSrcRings[5], _ceDstRings[3], _htcReadyBuffer;
    Ath10k::BootState _boot{};
    bool _bmiQuiesceAttempted = false, _bmiQuiesceSucceeded = false;
    std::map<UInt32, UInt8> memory;
    bool readFailure = false, corruptReadback = false;
    bool _bmiGetTargetInfoAttempted = false;
    bool _bmiAttempted=false,_bmiSucceeded=false;
    UInt32 _bmiRequestLength=0,_bmiResponseLength=0;
    bool runBMIGetTargetInfoTest();
    bool _bmiGetTargetInfoSucceeded = true, _bmiBoardPrereqSucceeded = true;
    bool _cePipesProgrammed = true, _wakeTargetCPUSucceeded = true;
    bool _bmiTransferUncertain = false, _htcControlExchangeUncertain = false,
         _dmaQuarantine = false, _bmiTimedOut = false;
    UInt32 _bmiCE0SrcWrite = 0, _bmiCE0SrcRead = 0, _bmiWaitUs = 0;
    bool _bmiBoardUploadAttempted = false, _bmiBoardUploadSucceeded = false;
    bool _bmiBoardReadbackSucceeded = false, _bmiBoardInitVerified = false;
    UInt32 _bmiWriteMemoryTransactionCount = 0, _bmiBoardBytesWritten = 0, _bmiBoardBytesVerified = 0;
    const char *_bmiBoardUploadStatus = "not-run";
    bool _otpAttempted = false, _otpStreamStarted = false, _otpStreamClosed = false;
    bool _otpDownloadSucceeded = false, _otpExecuteAttempted = false, _otpExecuteSucceeded = false;
    bool _otpResultAccepted = false, _otpResultIgnored = false;
    UInt32 _otpResult = 0xffffffffu, _otpWireBytesSent = 0, _otpLZDataCommands = 0, _otpExecuteResponseBytes = 0;
    const char *_otpStatus = "not-run";
    bool _fwAttempted = false, _fwStreamStarted = false, _fwStreamClosed = false;
    bool _fwDownloadSucceeded = false, _fwPostBMIProbeSucceeded = false;
    bool _fwReadbackAttempted = false, _fwReadbackSucceeded = false, _fwReadbackInProgress = false;
    UInt32 _fwWireBytesSent = 0, _fwLZDataCommands = 0, _fwReadbackBytes = 0, _fwVerifiedSegments = 0;
    UInt32 _fwFailedSegment = 0xffffffffu, _fwMismatchAddress = 0xffffffffu;
    UInt32 _fwPostTargetVersion = 0xffffffffu, _fwPostTargetType = 0xffffffffu;
    const char *_fwStatus = "not-run";
    bool _ceDiagTransferUncertain = false;
    UInt16 _vendorId = 0x168c, _deviceId = 0x003e, _subsystemVendorId = 0x1028, _subsystemId = 0x0310;
    const Ath10k::HwCandidate *_selectedHw = Ath10k::findCandidate(0x003e, 10);
    UInt32 _bmiTargetVersion = 0x05030000, _bmiTargetType = 8, _bmiTargetInfoLength = 12;
    UInt32 _bmiBoardDataAddress = 0x401fc0, _bmiBoardDataInitialized = 0;
    UInt32 _bmiBoardExtDataAddress = 0, _bmiBoardExtDataConfig = 0, _bmiCalDataAddress = 0;
};
#include "driver_methods.inc"

#ifndef ATH10K_OTP_FLOW_TEST
int main()
{
    { Ath10kPCIDevice d;
      assert(d.runBMIBoardUploadTest());
      assert(d._bmiBoardBytesWritten == 8124 && d._bmiBoardBytesVerified == 8124);
      assert(d._bmiWriteMemoryTransactionCount == 35 && d._regs.posts == 35);
      assert(d._bmiBoardInitVerified && !d._bmiTransferUncertain);
      assert(d.memory[0x400858] == 1);
    }
    { Ath10kPCIDevice d; d._subsystemId = 0x0311;
      assert(!d.runBMIBoardUploadTest() && d._regs.posts == 0); }
    { Ath10kPCIDevice d; d._bmiBoardDataAddress += 4;
      assert(!d.runBMIBoardUploadTest() && d._regs.posts == 0); }
    { Ath10kPCIDevice d; d._bmiBoardPrereqSucceeded = false;
      assert(!d.runBMIBoardUploadTest() && d._regs.posts == 0); }
    { Ath10kPCIDevice d; d.corruptReadback = true;
      assert(!d.runBMIBoardUploadTest() && !d._bmiBoardInitVerified);
      assert(d._regs.posts == 34 && d.memory.count(0x400858) == 0); }
    { Ath10kPCIDevice d; d.readFailure = true;
      assert(!d.runBMIBoardUploadTest() && d._regs.posts == 34); }
    { Ath10kPCIDevice d; d._regs.timeout = true;
      assert(!d.runBMIBoardUploadTest() && d._bmiTransferUncertain && d._bmiTimedOut);
      assert(d._regs.posts == 1 && d._bmiBoardBytesWritten == 0); }
    { Ath10kPCIDevice d; d._regs.doorbellFailure = true;
      assert(!d.runBMIBoardUploadTest() && d._bmiTransferUncertain && d._regs.posts == 1); }
    { Ath10kPCIDevice d; d._ceSrcRings[0].syncOK = false;
      assert(!d.runBMIBoardUploadTest() && !d._bmiTransferUncertain && d._regs.posts == 0); }
    { Ath10kPCIDevice d; d._regs.read = 1;
      assert(!d.runBMIBoardUploadTest() && d._regs.posts == 0); }
    { Ath10kPCIDevice d; d._bmiBoardUploadAttempted = true; UInt8 bytes[4]{};
      assert(!d.bmiWriteMemory(0x400000, bytes, 4));
      assert(!d.bmiWriteMemory(d._bmiBoardDataAddress + 8124, bytes, 4));
      assert(d._regs.posts == 0); }
}
#endif
