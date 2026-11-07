#define ATH10K_WMI_RUNTIME_TEST
#include "test_wmi_init.cpp"
#include "wmi_runtime_methods.inc"

static void live(InitDriver &d) {
    d.bootstrap();assert(d.runWMIInit());
    for(auto &radio:d._wmiRadio){radio.completed=true;}d._rxSetup.completed=true;d._wmiVdevCreate.completed=true;d._wmiScanChannels.completed=true;d._wmiScan.completed=true;
    d._wmiEcho.completed=true; // Isolate receiver tests; dedicated ECHO tests exercise TX.
    // Success must leave both rings rearmed, not replay the terminal credit.
    for(unsigned ce=1;ce<=2;++ce){
        auto *ring=static_cast<Ath10k::CEDescriptor32 *>(d._ceDstRings[ce].virtualAddress());
        assert(ring[ce==1?d._runtimeRxSlot:d._wmi.slot].nbytes==0);
    }
    assert(d.prepareWMIRuntime());
    assert(!d._wmiTimer->armed && d._wmiLive.active);
    assert(d._wmiWorkLoop->runAction(Ath10kPCIDevice::controlWMIRuntime,&d)==0);
    assert(d._wmiTimer->armed);
}
int main() {
    // Exact ordering from the hardware: READY, DIAG, PKGID, then CE1 credit
    // and CE2 supported-list pending together. INIT must leave CE2 untouched.
    for(bool malformed : {false,true}) {
        InitDriver handoff;handoff.noCredit=true;unsigned posts=0;
        handoff._regs.wmiPost=[&](){
            if(!handoff.initSends)return;
            if(++posts==1)handoff.deliver(2,diagPacket());
            else if(posts==2)handoff.deliver(2,pkgPacket());
            else if(posts==3){
                handoff.deliver(1,{0,2,8,0,8,9,0,0,1,4,0,1,2,1,0xf8,6});
                auto packet=supportedPacket();if(malformed)packet[16]=35;
                handoff.deliver(2,packet);
            }
        };
        handoff.bootstrap();assert(handoff.runWMIInit());
        assert(handoff._wmiInit.rxCount==4 && handoff._wmiInit.completed);
        assert(handoff._wmiInit.diagSupportedCount==0 && handoff._wmi.credits.outstanding==0);
        auto *ring=static_cast<Ath10k::CEDescriptor32 *>(handoff._ceDstRings[2].virtualAddress());
        assert(ring[handoff._wmi.slot].nbytes==160);
        for(auto &radio:handoff._wmiRadio){radio.completed=true;}handoff._rxSetup.completed=true;handoff._wmiVdevCreate.completed=true;handoff._wmiScanChannels.completed=true;handoff._wmiScan.completed=true;
        handoff._wmiEcho.completed=true; // Receiver-only lifecycle regression.
        assert(handoff.prepareWMIRuntime());
        assert(handoff._wmiWorkLoop->runAction(Ath10kPCIDevice::controlWMIRuntime,&handoff)==0);
        Ath10kPCIDevice::wmiTimerFired(&handoff,handoff._wmiTimer);
        assert(handoff._wmiInit.completed && handoff._wmiInit.rxCount==4);
        if(malformed){assert(!handoff._wmiLive.active && handoff.resets==1);}
        else {
            assert(handoff._wmiLive.active && handoff._wmiLive.rxCount==1);
            assert(handoff._wmiEvents.diagSupportedCount==1 && handoff._wmiEvents.diagSupportedEntries==34);
            for(unsigned i=0;i<200;++i)assert(!handoff.drainWMIRuntime());
            assert(handoff._wmiLive.rxCount==1 && handoff._wmi.credits.available==2);
        }
        handoff.releaseWMIRuntime();
    }
    InitDriver d;live(d);
    for(unsigned i=0;i<200;++i)assert(!d.drainWMIRuntime());
    assert(d._wmiLive.polls==200 && d._wmiLive.rxCount==0);
    d.deliver(2,pkgPacket());assert(!d.drainWMIRuntime());
    assert(d._wmiEvents.pkgIdCount==1 && d._wmiLive.rxCount==1);
    assert(d._wmiInit.rxCount==1 && d._wmiInit.completed); // INIT history frozen.
    // Ring wraparound and sustained traffic; stop after one callback's budget.
    d._regs.wmiPost=[&](){d.deliver(2,diagPacket());};
    d.deliver(2,diagPacket());
    for(unsigned i=0;i<5;++i)assert(!d.drainWMIRuntime());
    assert(d._wmiLive.budgetHits==5 && d._wmiEvents.diagCount==160);
    d._regs.wmiPost=nullptr;
    assert(!d.drainWMIRuntime());
    d.releaseWMIRuntime();
    assert(!d._wmiLive.active && !d._wmiWorkLoop && !d._wmiTimer && d.resets==1);
    assert(!d._wmi.rxOwned && !d._wmiInit.txOwned);
    d.releaseWMIRuntime();assert(d.resets==1);
    // A late/stale callback after stop must do no register access.
    d._regs.mmioFails=true;Ath10kPCIDevice::wmiTimerFired(&d,nullptr);assert(d.resets==1);
    InitDriver separate;separate.split=true;live(separate);
    assert(!separate.drainWMIRuntime() && separate._wmiLive.rxCount==0);
    // Duplicate returned credit after INIT is a protocol error, never extra balance.
    separate.deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
    Ath10kPCIDevice::wmiTimerFired(&separate,separate._wmiTimer);
    assert(!separate._wmiLive.active && separate._wmi.credits.available==2);
    separate.releaseWMIRuntime();
    InitDriver unknownDriver;live(unknownDriver);auto unknown=pkgPacket();unknown[8]=0x1a;unknownDriver.deliver(2,unknown);
    Ath10kPCIDevice::wmiTimerFired(&unknownDriver,unknownDriver._wmiTimer);
    assert(unknownDriver._wmiLive.active && !unknownDriver.resets && unknownDriver._wmiLive.lastEventId==0x1d01a);
    assert(unknownDriver._wmiEvents.unknownCount==1 && unknownDriver._wmiTimer->armed);unknownDriver.releaseWMIRuntime();
    InitDriver dup;live(dup);dup.deliver(2,readyPacket(false));
    assert(!strcmp(dup.drainWMIRuntime(),"runtime-unexpected-ready"));dup.releaseWMIRuntime();
    InitDriver crash;live(crash);crash._regs.fwIndicator=Ath10k::kFwIndicatorEventPending;
    Ath10kPCIDevice::wmiTimerFired(&crash,crash._wmiTimer);
    assert(crash._boot.firmwareCrash && !crash._wmiLive.active && crash.resets==1);crash.releaseWMIRuntime();
    InitDriver mmio;live(mmio);mmio._regs.mmioFails=true;
    assert(!strcmp(mmio.drainWMIRuntime(),"runtime-mmio-failed"));mmio._regs.mmioFails=false;mmio.releaseWMIRuntime();
    InitDriver repost;live(repost);repost.deliver(2,pkgPacket());repost._regs.wmiPostFail=true;
    Ath10kPCIDevice::wmiTimerFired(&repost,repost._wmiTimer);
    assert(!repost._wmiLive.active && repost.resets==1);repost.releaseWMIRuntime();
    InitDriver dma;live(dma);dma.deliver(2,pkgPacket());dma._wmiRxBuffer.syncInOK=false;
    assert(!strcmp(dma.drainWMIRuntime(),"runtime-rx-descriptor"));dma.releaseWMIRuntime();
    // Root notifier blocks new timer service immediately, before its gated
    // teardown; simulate inaccessible MMIO during that callback window.
    InitDriver blocked;live(blocked);blocked._powerIOBlock=1;blocked._regs.mmioFails=true;
    const unsigned blockedPolls=blocked._wmiLive.polls;
    Ath10kPCIDevice::wmiTimerFired(&blocked,blocked._wmiTimer);
    assert(blocked._wmiLive.active && !blocked.resets && blocked._wmiLive.polls==blockedPolls);
    blocked._regs.mmioFails=false;blocked.releaseWMIRuntime();
    InitDriver power;live(power);assert(power.setPowerState(0,&power)==0);
    assert(!power._wmiLive.active && power.resets==1 && !power._wmiTimer->armed);
    power.setPowerState(1,&power);assert(!power._wmiLive.active);power.releaseWMIRuntime();
    InitDriver shutdown;live(shutdown);shutdown.systemWillShutdown(0);
    assert(shutdown.resets==1 && !shutdown._wmiLive.active);shutdown.releaseWMIRuntime();
    InitDriver timer;live(timer);IOTimerEventSource::scheduleFail=true;
    Ath10kPCIDevice::wmiTimerFired(&timer,timer._wmiTimer);
    assert(!timer._wmiLive.active && timer.resets==1);IOTimerEventSource::scheduleFail=false;timer.releaseWMIRuntime();
    InitDriver reset;live(reset);reset.resetOK=false;reset.releaseWMIRuntime();
    assert(reset._dmaQuarantine && reset._wmi.rxOwned && reset._wmiInit.txOwned);
    InitDriver mask;live(mask);mask.irqStopOK=false;mask.releaseWMIRuntime();
    assert(mask._dmaQuarantine && mask.resets==1); // Still attempt reset after mask failure.
    for(unsigned failure=0;failure<3;++failure){
        InitDriver alloc;alloc.bootstrap();assert(alloc.runWMIInit());
        IOWorkLoop::allocFail=failure==0;IOTimerEventSource::allocFail=failure==1;IOWorkLoop::addFail=failure==2;
        assert(!alloc.prepareWMIRuntime());alloc.releaseWMIRuntime();
        assert(!alloc._wmiWorkLoop && !alloc._wmiTimer);
        IOWorkLoop::allocFail=IOTimerEventSource::allocFail=IOWorkLoop::addFail=false;
    }
}
