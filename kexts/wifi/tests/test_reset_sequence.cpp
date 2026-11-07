#define ATH10K_OTP_FLOW_TEST
#include "test_board_flow.cpp"
#include <string>
#include <algorithm>
static constexpr UInt32 kMSIMessageControlOffset=2;
// Actual warmResetTarget/probeTarget orchestration, with primitive MMIO/ROM,
// DMA setup and firmware operations mocked. This is not a hardware boot test.
struct ResetDriver:Ath10kPCIDevice {
    std::vector<std::string> events;std::string fail;
    unsigned irqMasks=0,waits=0,bmiQueries=0;bool romOverrides=false,chipChanged=false;
    struct ResetRegisters {
        ResetDriver *owner;
        bool read32(UInt32 address,UInt32 &value){
            value=address==Ath10k::chipIdOffset(Ath10k::kQca6174RegisterLayout)?
                UInt32(owner->_selectedHw->chipRevision+(owner->chipChanged?1:0))<<8:0;
            return true;
        }
    } _regs{this};
    Ath10k::CESnapshot _ceSnapshots[Ath10k::kCECount]{};
    bool _targetProbeAttempted=false,_targetWakeSucceeded=false,_postResetWakeSucceeded=false;
    bool _coldResetSucceeded=false,_coldTargetInitSucceeded=false,_hasMSI=false,_initConfigTargetDirty=false,_pcieParityRestored=false;
    bool _warmResetAttempted=false,_warmResetSucceeded=false,_warmResetIRQMasked=false,_warmResetSI0Succeeded=false;
    bool _warmResetCPU1Succeeded=false,_warmResetCPU2Succeeded=false,_warmResetCESucceeded=false,_warmResetLFCleared=false;
    bool _warmResetFirstTargetInitSucceeded=false,_warmResetSecondTargetInitSucceeded=false;
    bool _wakeTargetCPUAttempted=false,_ceDiagWriteSucceeded=false,_bmiReadMemorySucceeded=false;
    UInt32 _cePipeInitPassCount=0,_cePipeLastVerifiedCount=0,_msiCapOffset=0,_msiControlAfterWait=0,_msiControlPostReset=0;
    UInt32 _warmResetControlBefore=0,_warmResetControlAfter=0,_warmResetFirstTargetInitMs=0,_warmResetSecondTargetInitMs=0;
    UInt32 _warmResetFirstFWIndicator=0,_warmResetSecondFWIndicator=0,_postWarmChipId=0,_postWarmChipRevision=0;
    UInt32 _targetInitWaitMs=0,_fwIndicatorAfter=0,_socGlobalResetAfter=0,_coldTargetInitMs=0,_coldFWIndicator=0;
    UInt32 _ceDiagTransactionCount=0,_ceDiagWriteTransactionCount=0,_bmiReadMemoryTransactionCount=0;
    const char *_runtimeStatus="mock";bool _runtimeWakeAsserted=false;
    explicit ResetDriver(UInt16 device=Ath10k::kQCA9377Device){
        _deviceId=device;_selectedHw=Ath10k::findCandidate(device,device==Ath10k::kQCA9377Device?1:10);
    }
    bool event(const char *stage){events.emplace_back(stage);return fail!=stage;}
    void clearTargetProbeState(){}
    bool preparePCIeBringupParity(){return event("pci-parity");}
    void restorePCIeBringupParity(){event("restore-parity");_pcieParityRestored=true;}
    bool wakeTarget(bool &asserted){asserted=true;return event("wake");}
    void releaseTargetWake(bool){event("release-wake");}
    bool inspectCopyEngines(){event("inspect");return !romOverrides && fail!="inspect";}
    bool coldResetTarget(){_coldResetSucceeded=event("cold-reset");return _coldResetSucceeded;}
    void logMSIState(const char *){}
    bool waitForTargetInit(){
        ++waits;_targetInitWaitMs=10;_fwIndicatorAfter=2;
        const char *stage=waits==1?"cold-wait":(waits==2?"warm-wait1":"warm-wait2");
        // Firmware init overwrites host rings: only a post-reset init repairs it.
        if(waits==3)romOverrides=true;
        return event(stage);
    }
    bool readPostResetChipId(){return event("chip-id");}
    bool maskTargetIRQsForWarmReset(){++irqMasks;_warmResetIRQMasked=true;return event("irq-mask");}
    bool warmResetSI0(){_warmResetSI0Succeeded=event("si0");return _warmResetSI0Succeeded;}
    bool warmResetCPU(const char *pass){return event(pass);}
    bool warmResetClearLF(){_warmResetLFCleared=event("lf-clear");return _warmResetLFCleared;}
    bool warmResetCE(){_warmResetCESucceeded=event("ce-reset");return _warmResetCESucceeded;}
    bool initCopyEnginePipes(const char *stage){if(!event(stage))return false;++_cePipeInitPassCount;romOverrides=false;_cePipesProgrammed=true;return true;}
    bool deinitCopyEnginePipes(const char *){event("ce-deinit");_cePipesProgrammed=false;return true;}
    bool verifyCopyEnginePipe(UInt32,const Ath10k::CESnapshot &) const{return !romOverrides;}
    bool runCE7DiagnosticReadTest(){return event("diag");}
    bool runCE7InitConfigPersistentTest(){return event("config");}
    bool wakeTargetCPUForBMI(){_wakeTargetCPUAttempted=true;return event("cpu-wake");}
    bool runBMIGetTargetInfoTest(){++bmiQueries;return event("bmi");}
    bool runBMIBoardPrerequisiteProbe(){return event("prereq");}
    bool configureTargetPLL(){return event("pll");}
    bool configureBootHostInterest(){return event("boot-config");}
    bool runBMIBoardUploadTest(){return event("board");}
    bool runBMIOtpTest(){return event("otp");}
    bool configureBootFirmwareDownload(){return event("download-config");}
    bool runBMIFirmwareDownloadTest(){return event("firmware");}
    bool runFirmwareBootTest(){return event("boot");}
    bool startRuntimeCEIRQ(){return event("runtime");}
    bool stopRuntimeCEIRQ(){return event("stop-runtime");}
    bool runHTCServiceSetup(){return event("htc");}
    bool runWMIBootstrap(){return event("wmi");}
    bool runWMIInit(){return event("wmi-init");}
    bool prepareWMIRuntime(){return event("receiver");}
    bool quiesceTargetAfterBMI(){return event("quiesce");}
    bool warmResetTarget();bool probeTarget();
    size_t at(const char *stage){auto i=std::find(events.begin(),events.end(),stage);assert(i!=events.end());return size_t(i-events.begin());}
    bool seen(const char *stage){return std::find(events.begin(),events.end(),stage)!=events.end();}
};
#include "reset_sequence_methods.inc"
int main(){
    for(UInt16 device:{Ath10k::kQCA9377Device,Ath10k::kQCA6174Device}){
        auto p=std::make_unique<ResetDriver>(device);auto &d=*p;assert(d.probeTarget());
        assert(d.irqMasks==1 && d.waits==3 && d._cePipeInitPassCount==3 && d.bmiQueries==1 && !d.romOverrides);
        assert(d.at("warm-wait2")<d.at("post-chip-reset") && d.at("post-chip-reset")<d.at("diag"));
        assert(d.at("diag")<d.at("config") && d.at("config")<d.at("cpu-wake") && d.at("cpu-wake")<d.at("bmi"));
        assert(d.at("bmi")<d.at("prereq") && d.at("prereq")<d.at("pll") && d.at("pll")<d.at("boot-config"));
    }
    for(const char *stage:{"irq-mask","si0","pass1","warm-pass1","lf-clear","ce-reset","pass2","warm-pass2","warm-wait2","post-chip-reset","diag","config","cpu-wake"}){
        auto p=std::make_unique<ResetDriver>();auto &d=*p;d.fail=stage;assert(!d.probeTarget());
        assert(!d.bmiQueries && d.irqMasks==1);
        if(d.seen("post-chip-reset") && d.fail=="post-chip-reset")assert(!d.seen("diag") && !d.seen("cpu-wake"));
    }
    {auto d=std::make_unique<ResetDriver>();d->fail="warm-wait1";assert(d->probeTarget() && d->irqMasks==1);}
    {auto d=std::make_unique<ResetDriver>();d->fail="bmi";assert(!d->probeTarget() && d->bmiQueries==1 && d->seen("quiesce") && !d->seen("board"));}
    {auto d=std::make_unique<ResetDriver>();d->fail="pll";assert(!d->probeTarget() && d->seen("quiesce") && !d->seen("boot-config") && !d->seen("board"));}
    {auto d=std::make_unique<ResetDriver>();d->chipChanged=true;assert(!d->probeTarget() && !d->bmiQueries);}
    puts("Reset sequence passed: actual warm reset/probe, one IRQ mask, three CE passes, ROM overwrite recovery, CE/config/CPU/BMI order and failure stops (6174/9377 mocks).");
}
