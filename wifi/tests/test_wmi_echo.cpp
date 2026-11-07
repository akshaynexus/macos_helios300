#define ATH10K_WMI_RUNTIME_TEST
#include "test_wmi_init.cpp"
#include "wmi_runtime_methods.inc"

static std::vector<UInt8> echoPacket(UInt32 cookie,bool credit=true) {
    std::vector<UInt8> p(20);
    p[0]=2;p[2]=12;Ath10k::htcWriteLE32(p.data()+8,0x1d001);
    p[12]=4;p[14]=54;Ath10k::htcWriteLE32(p.data()+16,cookie);
    if(credit)addPkgCredit(p);
    return p;
}
class EchoDriver:public InitDriver {
public:
    unsigned echoes=0;
    enum Mode {Piggyback,ReplyOnly,CreditOnly,Silent,BadCookie};
    Mode mode=Piggyback;
    EchoDriver() {
        for (auto &radio:_wmiRadio) {radio.completed=true;radio.txConsumed=true;}
        _rxSetup.completed=true;
        _wmiScan.completed=true;
        _wmiScanChannels.completed=true;
        _wmiVdevCreate.completed=true; // Isolate ECHO; VDEV has its own integration tests.
        bootstrap();assert(runWMIInit());
        _regs.tx3Consume=[this](){
            if(_rxSetup.completed)_rxSetup.wmiNext=_wmiEcho.txNext;
            ++echoes;assert(_wmiEcho.txOwned && _wmi.credits.available==1 && _wmi.credits.outstanding==1);
            auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[3].virtualAddress());
            assert(ring[_wmiEcho.txSlot].nbytes==20 && ring[_wmiEcho.txSlot].flags==8);
            auto *p=static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress());
            assert(p[0]==2 && p[1]==1 && p[5]==1 && Ath10k::htcReadLE16(p+2)==12);
            assert(Ath10k::wmiReadLE32(p+8)==0x1d001 && Ath10k::htcReadLE16(p+14)==149);
            assert(Ath10k::wmiReadLE32(p+16)==_wmiEcho.cookie);
            if(mode==Piggyback || mode==ReplyOnly || mode==BadCookie)
                deliver(2,echoPacket(_wmiEcho.cookie+(mode==BadCookie),mode!=ReplyOnly));
            if(mode==CreditOnly)deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
        assert(prepareWMIRuntime());
        assert(_wmiWorkLoop->runAction(Ath10kPCIDevice::controlWMIRuntime,this)==0);
    }
    void tick(UInt64 now){mockNowMs=now;Ath10kPCIDevice::wmiTimerFired(this,_wmiTimer);}
};
#ifndef ATH10K_WMI_VDEV_TEST
int main() {
    // One real timer-driven exchange, then idle ticks must never resend.
    EchoDriver good;good.tick(1000);assert(good._wmiEcho.submitted && !good._wmiEcho.completed);
    good.tick(1010);assert(good._wmiEcho.completed && good._wmiEcho.elapsedMs==10);
    assert(good._wmiEcho.replyReceived && good._wmiEcho.txConsumed && good._wmiEcho.creditReturned);
    assert(good._wmiLive.active && good._wmi.credits.available==2);
    for(unsigned i=0;i<200;++i)good.tick(1020+10*i);
    assert(good.echoes==1 && good._wmiInit.completed);good.releaseWMIRuntime();assert(!good._wmiEcho.txOwned);
    EchoDriver beforeReply;beforeReply.mode=EchoDriver::CreditOnly;
    beforeReply.tick(1000);beforeReply.tick(1010);
    assert(beforeReply._wmiEcho.creditReturned && !beforeReply._wmiEcho.completed);
    beforeReply.deliver(2,diagPacket());beforeReply.tick(1020);
    beforeReply.deliver(2,echoPacket(beforeReply._wmiEcho.cookie,false));beforeReply.tick(1030);
    assert(beforeReply._wmiEcho.completed);beforeReply.releaseWMIRuntime();
    EchoDriver beforeCredit;beforeCredit.mode=EchoDriver::ReplyOnly;
    beforeCredit.tick(1000);beforeCredit.tick(1010);
    assert(beforeCredit._wmiEcho.replyReceived && !beforeCredit._wmiEcho.completed);
    beforeCredit.deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});beforeCredit.tick(1020);
    assert(beforeCredit._wmiEcho.completed);beforeCredit.releaseWMIRuntime();
    EchoDriver wrap;wrap._regs.tx3Read=wrap._regs.tx3Written=wrap._wmiInit.txNext=31;
    wrap.tick(1000);assert(wrap._wmiEcho.txSlot==31 && wrap._wmiEcho.txNext==0);
    wrap.tick(1010);assert(wrap._wmiEcho.completed);wrap.releaseWMIRuntime();
    EchoDriver wrong;wrong.mode=EchoDriver::BadCookie;wrong.tick(1000);wrong.tick(1010);
    assert(!wrong._wmiLive.active && !wrong._wmiEcho.completed && wrong.resets==1);
    assert(wrong._wmi.credits.outstanding==1 && !wrong._wmiEcho.replyReceived);wrong.releaseWMIRuntime();
    for(unsigned which=0;which<3;++which){
        EchoDriver timeout;timeout.mode=which==1?EchoDriver::ReplyOnly:EchoDriver::CreditOnly;
        if(which==0)timeout._regs.tx3Timeout=true;
        timeout.tick(1000);timeout.tick(1010);timeout.tick(5999);
        assert(timeout._wmiLive.active && !timeout._wmiEcho.completed);
        timeout.tick(6000);assert(!timeout._wmiLive.active && timeout.resets==1);
        const char *expected=which==0?"wmi-echo-tx-timeout":which==1?"wmi-echo-credit-timeout":"wmi-echo-reply-timeout";
        assert(!strcmp(timeout._wmiEcho.status,expected));timeout.releaseWMIRuntime();
    }
    EchoDriver doorbell;doorbell._regs.tx3Fail=true;doorbell.resetOK=false;doorbell.tick(1000);
    assert(doorbell._wmiEcho.attempted && doorbell._wmiEcho.txOwned && !doorbell._wmiEcho.submitted);
    assert(doorbell._dmaQuarantine && doorbell._wmi.credits.outstanding==1);
    auto write=doorbell._regs.tx3Written;assert(doorbell.serviceWMIEcho(2000));
    assert(doorbell._regs.tx3Written==write);doorbell.releaseWMIRuntime();
    EchoDriver noCredit;noCredit._wmi.credits.available=0;noCredit.tick(1000);
    assert(!noCredit._wmiEcho.submitted && !noCredit._wmiEcho.txOwned && noCredit.echoes==0);noCredit.releaseWMIRuntime();
    EchoDriver sync;sync._wmiTxBuffer.syncOK=false;sync.tick(1000);
    assert(!sync._wmiEcho.txOwned && sync._wmi.credits.outstanding==0);sync.releaseWMIRuntime();
    EchoDriver index;index._regs.tx3Read=2;index.tick(1000);
    assert(!index._wmiEcho.submitted && index.echoes==0);index.releaseWMIRuntime();
    EchoDriver stop;stop.mode=EchoDriver::Silent;stop.tick(1000);stop.setPowerState(0,&stop);
    assert(!stop._wmiEcho.completed && !stop._wmiEcho.txOwned && stop.resets==1);stop.releaseWMIRuntime();
    // Strict response parsing, with transactional credit and response-state updates.
    auto reject=[](std::vector<UInt8> p){
        Ath10k::HTCCreditState c;c.initialize(2,1792,2);c.reserve(20);
        Ath10k::WMIInitState out;Ath10k::WMIEchoState e;e.submitted=true;
        assert(Ath10k::parseWMIInitResponse(p.data(),p.size(),2,c,out,&e));
        assert(!e.replyReceived && c.outstanding==1 && c.available==1);
    };
    auto packet=echoPacket(0xa7100033);
    for(unsigned n=0;n<packet.size();++n)reject({packet.begin(),packet.begin()+n});
    packet=echoPacket(1);reject(packet);
    packet=echoPacket(0xa7100033);packet[14]=55;reject(packet);
    packet=echoPacket(0xa7100033);packet[12]=3;reject(packet);
    packet=echoPacket(0xa7100033);packet[25]=2;reject(packet); // Excess credit.
    packet=echoPacket(0xa7100033,false);packet.insert(packet.end(),{4,0,54,0,0x33,0,0x10,0xa7});packet[2]=20;reject(packet);
    packet=echoPacket(0xa7100033,false);packet.resize(12);packet[2]=4;reject(packet);
    packet=echoPacket(0xa7100033,false);
    Ath10k::HTCCreditState ledger;ledger.initialize(2,1792,2);ledger.reserve(20);
    Ath10k::WMIInitState out;Ath10k::WMIEchoState state;
    assert(Ath10k::parseWMIInitResponse(packet.data(),packet.size(),2,ledger,out));
    assert(Ath10k::parseWMIInitResponse(packet.data(),packet.size(),2,ledger,out,&state));
    state.submitted=true;
    assert(!Ath10k::parseWMIInitResponse(packet.data(),packet.size(),2,ledger,out,&state));
    assert(Ath10k::parseWMIInitResponse(packet.data(),packet.size(),2,ledger,out,&state));
    assert(state.replyReceived && !out.ready && ledger.outstanding==1);
    UInt8 frame[20];assert(!Ath10k::buildWMIEcho(frame,19,2,1));assert(!Ath10k::buildWMIEcho(frame,20,0,1));
}

#endif
