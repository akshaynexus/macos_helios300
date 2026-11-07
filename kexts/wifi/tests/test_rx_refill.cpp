#define ATH10K_RX_REFILL_TEST
#include "test_rx_setup.cpp"
static std::vector<UInt8> indication(UInt8 count=1){
    std::vector<UInt8> p(60);p[0]=1;p[2]=52;p[8]=1;p[15]=1;p[56]=count;p[57]=8;
    return p;
}
static void completeBuffer(SetupDriver &d,UInt32 id,bool last=true){
    auto *data=d._httRecycled[id].allocated()?static_cast<UInt8 *>(d._httRecycled[id].virtualAddress()):
        static_cast<UInt8 *>(d._httRxPool.virtualAddress())+2048*id;
    Ath10k::htcWriteLE32(data+4,0x80000000);Ath10k::htcWriteLE32(data+56,last?0x8000:0);
}
int main(){
    SetupDriver d;d.run();
    d.deliver(2,scanPacket(1));d.tick(1250);
    d.deliver(2,scanPacket(2));d.tick(1260);
    d.deliver(2,mgmtPacket());d.tick(1270);
    // MSDU_DONE alone must never authorize refill.
    completeBuffer(d,0);assert(!d.serviceHTTRx() && !d._rxSetup.recycled);
    // Multiple cycles test both ring wrap and buffer reuse, not only initial allocation.
    for(unsigned i=0;i<2200;++i){
        UInt32 id=d._rxSetup.bufferIds[d._rxSetup.readIndex];completeBuffer(d,id);
        d.deliver(1,indication());d.tick(1300+10*i);
        assert(d._wmiLive.active && d._rxSetup.recycled==i+1 && !d._rxSetup.pendingMPDUs);
        assert(!Ath10k::wmiReadLE32(static_cast<UInt8 *>(d._httRecycled[id].virtualAddress())+4));
        assert(Ath10k::wmiReadLE32(static_cast<UInt8 *>(d._httRxIndex.virtualAddress()))==((1024+i)&2047));
    }
    // More than one batch: 64+64+2, no starvation/unbounded drain.
    for(unsigned i=0;i<130;++i)completeBuffer(d,d._rxSetup.bufferIds[(d._rxSetup.readIndex+i)&2047]);
    d.deliver(1,indication(130));d.tick(24900);
    assert(d._rxSetup.recycled==2264 && d._rxSetup.pendingMPDUs==66 && d._rxSetup.batchMax==64);
    assert(d._rxSetup.budgetHits==1 && d._wmiLive.pollIntervalMs==1);
    d.tick(24901);assert(d._rxSetup.recycled==2328 && d._rxSetup.pendingMPDUs==2);
    d.tick(24902);assert(d._rxSetup.recycled==2330 && !d._rxSetup.pendingMPDUs);
    // Two MSDUs belonging to one MPDU; both buffers returned only after LAST.
    UInt32 id=d._rxSetup.bufferIds[d._rxSetup.readIndex];completeBuffer(d,id,false);
    UInt32 second=d._rxSetup.bufferIds[(d._rxSetup.readIndex+1)&2047];completeBuffer(d,second);
    d.deliver(1,indication());d.tick(25000);
    assert(d._wmiLive.active && d._rxSetup.recycled==2332 && !d._rxSetup.pendingMPDUs);
    d.releaseWMIRuntime();
    SetupDriver premature;premature.run();premature.deliver(1,indication());premature.tick(1300);
    assert(premature._wmiLive.active && !premature.resets && !premature._rxSetup.recycled);
    assert(premature._rxSetup.pendingMPDUs==1 && premature._rxSetup.rxDoneConsecutiveWaits==1);
    completeBuffer(premature,0);premature.tick(1301);
    assert(premature._wmiLive.active && premature._rxSetup.recycled==1 &&
           !premature._rxSetup.pendingMPDUs && !premature._rxSetup.rxDoneConsecutiveWaits);
    premature.releaseWMIRuntime();
    // Partial ordered MPDU: no delivery, refill or index changes until every
    // descriptor through LAST is ready. It is retried transactionally.
    SetupDriver partial;partial.run();completeBuffer(partial,0,false);
    partial.deliver(1,indication());partial.tick(1300);
    assert(partial._wmiLive.active && !partial._rxSetup.recycled && !partial._rxSetup.readIndex);
    assert(partial._rxSetup.pendingMPDUs==1 && partial._rxSetup.rxDoneLastBuffer==1);
    completeBuffer(partial,1);partial.tick(1301);
    assert(partial._wmiLive.active && partial._rxSetup.recycled==2 && !partial._rxSetup.pendingMPDUs);
    partial.releaseWMIRuntime();
    // Bounded stall: never recycle DMA without DONE, even after many polls.
    SetupDriver stalled;stalled.run();stalled.deliver(1,indication());stalled.tick(1300);
    for(unsigned n=1;n<Ath10k::kHTTRxDoneRetryLimit;++n)stalled.tick(1300+n);
    assert(!stalled._wmiLive.active && stalled.resets==1 && !stalled._rxSetup.recycled);
    assert(stalled._rxSetup.rxDoneTimeouts==1 &&
           !strcmp(stalled._wmiLive.status,"htt-rx-completion-done-timeout"));
    stalled.releaseWMIRuntime();
    SetupDriver failure;failure.run();completeBuffer(failure,0);failure._httRxIndex.syncOK=false;failure.resetOK=false;
    failure.deliver(1,indication());failure.tick(1300);
    assert(failure._dmaQuarantine && failure._httRecycled[0].allocated() && !failure._rxSetup.recycled);failure.releaseWMIRuntime();
    // Malformed indications must not commit credits or ownership counters.
    for(unsigned mode=0;mode<4;++mode){
        Ath10k::RxSetupState s;s.ringOwned=true;Ath10k::HTCCreditState c;c.initialize(2,1792,2);c.reserve(20);
        auto p=indication();
        if(mode==0)p[52]=255;
        if(mode==1)p[15]=2;
        if(mode==2)p[8]=0x12;
        addPkgCredit(p);if(mode==3)p[p.size()-3]=3;
        assert(Ath10k::parseHTTSetup(p.data(),p.size(),1,c,s));
        assert(!s.pendingMPDUs && !s.rxIndications && c.outstanding==1);
    }
}
