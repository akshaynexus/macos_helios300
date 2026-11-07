#define ATH10K_RX_SETUP_TEST
#include "test_wmi_radio.cpp"
class SetupDriver:public RadioDriver {
public:
    unsigned setupCommands=0,httCommands=0;
    UInt8 version=3;
    bool badBarrier=false,holdCredit=false;
    SetupDriver(){
        _rxSetup={};
        for(unsigned i=0;i<1023;++i){_httRecycled[i].isAllocated=false;_httRecycled[i].iovm=0x1000000+4096*i;}
        _htcHTTData.connected=true;_htcHTTData.endpoint=1;
        _htcHTTData.uplinkPipe=4;_htcHTTData.downlinkPipe=1;
        _htcHTTData.maxMessageSize=256;_htcHTTData.creditFlowEnabled=false;
        _ceSrcRings[4].allocate(8192*8,8,nullptr);
        _httTxBuffer.iovm=0x200000;_httRxIndex.iovm=0x300000;
        _httRxRing.iovm=0x400000;_httRxPool.iovm=0x500000;
        auto previous=_regs.tx3Consume;
        _regs.tx3Consume=[this,previous](){
            if(!_rxSetup.attempted || _rxSetup.stage>=3){previous();return;}
            const auto *p=static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress());
            unsigned stage=_rxSetup.stage;++setupCommands;
            const UInt32 ids[]={0x5001,0x5002,0x1d001};
            assert(Ath10k::wmiReadLE32(p+8)==ids[stage]);
            assert(p[5]==stage+2 && _rxSetup.txOwned && !_wmiVdevCreate.attempted);
            if(stage==1)assert(Ath10k::htcReadLE16(p+14)==87 && !Ath10k::wmiReadLE32(p+16));
            if(stage==2)deliver(2,echoPacket(Ath10k::kHTTSetupCookie+badBarrier));
            else if(stage==1){
                std::vector<UInt8> response={2,0,4,0,0,0,0,0,6,0x50,0,0};
                addPkgCredit(response);deliver(2,response);
            }
            else if(!holdCredit)deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
        _regs.tx4Consume=[this](){
            ++httCommands;const auto *p=static_cast<UInt8 *>(_httTxBuffer.virtualAddress());
            assert(p[0]==1 && !p[1] && p[5]==_rxSetup.stage-3 && _filterEcho.replyReceived);
            auto *desc=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[4].virtualAddress())+_rxSetup.txSlot;
            assert(desc->flags==4 && desc->address==0x200000);
            if(_rxSetup.stage==3){
                assert(desc->nbytes==12 && !Ath10k::wmiReadLE32(p+8));
                deliver(1,{1,0,4,0,0,0,0,0,0,1,version,0});
            }else if(_rxSetup.stage==4){
                assert(_rxSetup.versionReceived && _rxSetup.ringOwned && desc->nbytes==48);
                assert(p[8]==2 && p[9]==1 && !p[10] && !p[11]);
                assert(Ath10k::wmiReadLE32(p+12)==0x300000 && Ath10k::wmiReadLE32(p+16)==0x400000);
                assert(Ath10k::htcReadLE16(p+20)==2048 && Ath10k::htcReadLE16(p+22)==2048);
                assert(Ath10k::htcReadLE16(p+24)==0xffff && Ath10k::htcReadLE16(p+26)==1023);
                const UInt16 offsets[]={59,75,21,31,3,20,6,10,1,2};
                for(unsigned i=0;i<10;++i)assert(Ath10k::htcReadLE16(p+28+2*i)==offsets[i]);
                const auto *ring=static_cast<UInt8 *>(_httRxRing.virtualAddress());
                assert(Ath10k::wmiReadLE32(static_cast<UInt8 *>(_httRxIndex.virtualAddress()))==1023);
                for(unsigned i=0;i<2048;++i)assert(Ath10k::wmiReadLE32(ring+4*i)==(i<1023?0x500000+2048*i:0));
            }else {assert(_rxSetup.stage==5 && desc->nbytes==11 && p[8]==5 && p[9]==64 && p[10]==3);}
        };
    }
    void run(){for(unsigned t=1000;t<1250 && _wmiLive.active;t+=10)tick(t);}
};
#ifndef ATH10K_RX_REFILL_TEST
int main(){
    SetupDriver good;good.run();
    assert(good._wmiLive.active && good._rxSetup.completed && good._rxSetup.stage==6);
    assert(good.setupCommands==3 && good.httCommands==3 && good.creates==1 && good.scans==1);
    assert(good._rxSetup.major==3 && good._rxSetup.minor==1 && good._filterEcho.replyReceived);
    good.deliver(2,scanPacket(1));good.tick(1250);
    good.deliver(2,mgmtPacket());good.tick(1260);assert(good._wmiScan.mgmtCount==1);
    good.deliver(1,{1,0,4,0,0,0,0,0,8,0,0,0});good.tick(1270);
    assert(good._rxSetup.messages==2 && good._rxSetup.lastType==8);
    auto *pool=static_cast<UInt8 *>(good._httRxPool.virtualAddress());
    Ath10k::htcWriteLE32(pool+4,0x80000000);assert(!good.sampleHTTRxPool() && good._rxSetup.rxDone==1);
    for(unsigned i=1;i<1023;++i)Ath10k::htcWriteLE32(pool+i*2048+4,0x80000000);
    assert(!strcmp(good.sampleHTTRxPool(),"htt-diagnostic-pool-exhausted"));
    good._wmiLive.lastMaintenanceMs=280;good.tick(1280);assert(!good._wmiLive.active && good.resets==1 && !good._rxSetup.ringOwned);
    good.releaseWMIRuntime();
    SetupDriver unsupported;unsupported.version=1;unsupported.run();
    assert(!unsupported._wmiLive.active && unsupported.resets==1 && !unsupported.scans && unsupported.httCommands==1);unsupported.releaseWMIRuntime();
    SetupDriver barrier;barrier.badBarrier=true;barrier.run();
    assert(!barrier._wmiLive.active && !barrier.httCommands && !barrier.scans);barrier.releaseWMIRuntime();
    SetupDriver credit;credit.holdCredit=true;credit.run();assert(credit.setupCommands==1 && !credit.httCommands);
    credit.tick(7000);assert(!credit._wmiLive.active && !strcmp(credit._rxSetup.status,"rx-setup-credit-timeout"));credit.releaseWMIRuntime();
    SetupDriver stalled;stalled._regs.tx4Timeout=true;stalled.run();assert(!stalled.scans && !stalled.httCommands);
    stalled.tick(7000);assert(!stalled._wmiLive.active && !strcmp(stalled._rxSetup.status,"rx-setup-tx-timeout"));stalled.releaseWMIRuntime();
    SetupDriver failed;
    for(unsigned t=1000;failed._rxSetup.stage<4;t+=10){assert(t<1250);failed.tick(t);}
    failed._regs.tx4Fail=true;failed.resetOK=false;failed.tick(1250);
    assert(failed._dmaQuarantine && failed._rxSetup.ringOwned && failed._rxSetup.txOwned && !failed.scans);
    assert(failed.serviceRxSetup(1260));
    failed.releaseWMIRuntime();assert(failed._rxSetup.ringOwned);
    SetupDriver alloc;alloc._httRxPool.allocationFails=true;alloc.run();
    assert(!alloc.scans && !alloc._rxSetup.ringOwned && !strcmp(alloc._rxSetup.status,"htt-rx-allocation"));alloc.releaseWMIRuntime();
    SetupDriver span;span._httRxPool.iovm=0xfffff800ULL;span.run();
    assert(!span.scans && !strcmp(span._rxSetup.status,"htt-rx-address"));span.releaseWMIRuntime();
    UInt8 f[49],mac[]={2,1,2,3,4,5};memset(f,0xa5,sizeof(f));
    assert(Ath10k::buildRxSetup(f,48,4,2,1,mac,0x300000,0x400000)==48 && f[48]==0xa5);
    assert(!Ath10k::buildRxSetup(f,47,4,2,1,mac,0x300000,0x400000));
    assert(!Ath10k::buildRxSetup(f,48,4,2,1,mac,0x300000,0xfffff000ULL));
    assert(!Ath10k::buildRxSetup(f,48,4,2,1,mac,0x300001,0x400000));
    Ath10k::RxSetupState state;state.versionRequested=true;
    Ath10k::HTCCreditState credits=good._wmi.credits;
    UInt8 v[]={1,0,4,0,0,0,0,0,0,1,3,0};
    for(unsigned n=0;n<12;++n)assert(Ath10k::parseHTTSetup(v,n,1,credits,state));
    assert(!state.versionReceived && !state.messages);
    auto corrupt=std::vector<UInt8>(v,v+12);
    corrupt[1]=2;corrupt[2]=12;corrupt[4]=8;
    corrupt.insert(corrupt.end(),{1,4,0,0,2,255,0,0});
    const auto saved=credits;
    assert(Ath10k::parseHTTSetup(corrupt.data(),corrupt.size(),1,credits,state));
    assert(!state.versionReceived && !state.messages && credits.available==saved.available && credits.outstanding==saved.outstanding);

    assert(!Ath10k::parseHTTSetup(v,12,1,credits,state) && state.major==3);
    assert(Ath10k::parseHTTSetup(v,12,1,credits,state));
    state.ringOwned=true;
    UInt8 padded[]={1,0,1,0,0,0,0,0,8,0,0,0};
    assert(!Ath10k::parseHTTSetup(padded,12,1,credits,state));
    padded[1]=0x80;assert(Ath10k::parseHTTSetup(padded,12,1,credits,state));
}

#endif
