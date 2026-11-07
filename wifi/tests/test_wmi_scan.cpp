#define ATH10K_WMI_SCAN_TEST
#include "test_wmi_channels.cpp"
static std::vector<UInt8> scanPacket(UInt32 type,UInt32 reason=0,bool credit=false) {
    std::vector<UInt8> p(40);p[0]=2;p[2]=32;
    Ath10k::htcWriteLE32(p.data()+8,0x3001);p[12]=24;p[14]=36;
    Ath10k::htcWriteLE32(p.data()+16,type);Ath10k::htcWriteLE32(p.data()+20,reason);
    Ath10k::htcWriteLE32(p.data()+24,2412);
    Ath10k::htcWriteLE32(p.data()+28,0xa001);Ath10k::htcWriteLE32(p.data()+32,0xa036);
    if(credit)addPkgCredit(p);
    return p;
}
static std::vector<UInt8> mgmtPacket() {
    std::vector<UInt8> p(84);p[0]=2;p[2]=76;
    Ath10k::htcWriteLE32(p.data()+8,0x7001);p[12]=40;p[14]=44;
    Ath10k::htcWriteLE32(p.data()+32,24);p[56]=24;p[58]=17;p[60]=0x80;
    return p;
}
class ScanDriver:public ChannelsDriver {
public:
    unsigned scans=0;
    bool scanCredit=true;
    ScanDriver() {
        _wmiScan={};auto prior=_regs.tx3Consume;
        _regs.tx3Consume=[this,prior](){
            auto *p=static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress());
            if(Ath10k::wmiReadLE32(p+8)!=0x3001){prior();return;}
            ++scans;assert(_wmiScanChannels.completed && _wmiScan.txOwned && _wmi.credits.outstanding==1);
            assert(p[0]==2 && p[1]==1 && p[5]==9 && Ath10k::htcReadLE16(p+2)==284);
            assert(Ath10k::htcReadLE16(p+12)==100 && Ath10k::htcReadLE16(p+14)==77);
            const UInt32 fixed[25]={0xa036,0xa001,0,1,0x1ff,50,500,50,500,0,0,0,30000,5,0x31,0,38,1,0,0,0,0,0,0,0};
            for(unsigned i=0;i<25;++i)assert(Ath10k::wmiReadLE32(p+16+4*i)==fixed[i]);
            assert(Ath10k::htcReadLE16(p+116)==152 && p[118]==16);
            for(unsigned i=0;i<38;++i)assert(Ath10k::wmiReadLE32(p+120+4*i)==expectedScanFrequencies[i]);
            const UInt8 tail[20]={0,0,19,0,8,0,19,0,255,255,255,255,255,255,0,0,0,0,17,0};
            assert(!memcmp(p+272,tail,20));
            auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[3].virtualAddress());
            assert(ring[_wmiScan.txSlot].nbytes==292 && ring[_wmiScan.txSlot].flags==8);
            if(scanCredit)deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
    }
    void launch(){tick(1000);tick(1010);tick(1020);assert(!_wmiScan.attempted);tick(1030);assert(_wmiScan.submitted);}
};
[[maybe_unused]] static void reusableScanTests(){
    ScanDriver d;d.launch();d.deliver(2,scanPacket(1));d.tick(1040);d.deliver(2,scanPacket(2));d.tick(1050);
    assert(d._wmiScan.completed);
    d._wmiScan={};d._wmiScan.generation=1;d._wmiScan.id=0xa037;
    d._connection.stage=Ath10k::LinkStage::Connected;d._connection.portAuthorized=true;
    unsigned starts=0,stops=0;
    d._regs.tx3Consume=[&](){
        const auto *f=static_cast<const UInt8 *>(d._wmiTxBuffer.virtualAddress());
        const UInt32 cmd=Ath10k::wmiReadLE32(f+8);
        if(cmd==0x3001){
            ++starts;assert(Ath10k::wmiReadLE32(f+16)==0xa037);
            assert(Ath10k::wmiReadLE32(f+40)==100 && Ath10k::wmiReadLE32(f+44)==150);
        }else{
            ++stops;assert(cmd==0x3002 && f[14]==78 && Ath10k::htcReadLE16(f+2)==24);
            assert(Ath10k::wmiReadLE32(f+16)==0xa001 && Ath10k::wmiReadLE32(f+20)==0xa037);
            assert(!Ath10k::wmiReadLE32(f+24) && !Ath10k::wmiReadLE32(f+28));
        }
        d.deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
    };
    // A delayed previous COMPLETED arriving before the new START submission
    // must not kill runtime or complete the next generation.
    d.deliver(2,scanPacket(2));d.tick(1060);
    assert(d._wmiLive.active && d._wmiScan.submitted && !d._wmiScan.terminal && !d._wmiScan.eventCount);
    auto start=scanPacket(1);Ath10k::htcWriteLE32(start.data()+32,0xa037);d.deliver(2,start);d.tick(1070);
    assert(d._wmiScan.started && !d._wmiScan.txOwned && starts==1);
    d._wmiScan.cancelRequested=true;d.tick(1080);assert(d._wmiScan.stopSubmitted && d._wmiScan.txOwned && stops==1);
    auto canceled=scanPacket(2,1);Ath10k::htcWriteLE32(canceled.data()+32,0xa037);d.deliver(2,canceled);d.tick(1090);
    assert(d._wmiScan.completed && d._wmiScan.stopConsumed && !d._wmiScan.txOwned && !d._wmiScan.failed && d._wmiLive.active);
    d.releaseWMIRuntime();
}

#ifndef ATH10K_WMI_BSS_TEST
int main() {
    reusableScanTests();
    ScanDriver d;d.launch();d.deliver(2,scanPacket(1));d.tick(1040);
    assert(d._wmiScan.started && d._wmiScan.creditReturned && !d._wmiScan.completed);
    d.deliver(2,scanPacket(8));d.tick(1050);
    d.deliver(2,mgmtPacket());d.tick(1060);assert(d._wmiScan.mgmtCount==1 && d._wmiScan.mgmtBytes==24);
    d.deliver(2,scanPacket(256));d.tick(1070);
    d.deliver(2,scanPacket(2));d.tick(1080);
    assert(d._wmiScan.completed && d._wmiLive.active && d._wmiScan.eventCount==4);
    for(unsigned i=0;i<380;++i)d.tick(1090+i*10);
    assert(d.scans==1 && d._wmi.credits.available==2);d.releaseWMIRuntime();assert(!d._wmiScan.txOwned);
    ScanDriver late;late.scanCredit=false;late.launch();late.deliver(2,scanPacket(1));late.tick(1040);
    late.deliver(2,scanPacket(2));late.tick(1050);assert(late._wmiScan.terminal && !late._wmiScan.completed);
    late.deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});late.tick(1060);
    assert(late._wmiScan.completed);late.releaseWMIRuntime();
    for(unsigned mode=0;mode<4;++mode){
        ScanDriver t;t.scanCredit=mode!=1;t.tick(1000);t.tick(1010);t.tick(1020);
        t._regs.tx3Timeout=mode==0;t.tick(1030);
        if(mode==3){t.deliver(2,scanPacket(1));t.tick(1040);}
        t.tick(36029);assert(t._wmiLive.active);t.tick(36030);
        const char *errors[]={"scan-tx-timeout","scan-credit-timeout","scan-start-timeout","scan-completion-timeout"};
        assert(!t._wmiLive.active && !strcmp(t._wmiScan.status,errors[mode]) && t.resets==1);t.releaseWMIRuntime();
    }
    for(unsigned type:{2u,16u,32u,64u}){
        ScanDriver f;f.launch();f.deliver(2,scanPacket(type,4));f.tick(1040);
        assert(f._wmiScan.failed && !f._wmiScan.completed && !f._wmiLive.active && f.resets==1);f.releaseWMIRuntime();
    }
    ScanDriver failed;failed.tick(1000);failed.tick(1010);failed.tick(1020);
    failed._regs.tx3Fail=true;failed.resetOK=false;failed.tick(1030);
    assert(failed._wmiScan.txOwned && !failed._wmiScan.submitted && failed._dmaQuarantine);
    assert(failed.serviceWMIScan(2000) && failed.scans==0);failed.releaseWMIRuntime();
    ScanDriver stop;stop.launch();stop.setPowerState(0,&stop);
    assert(!stop._wmiScan.txOwned && !stop._wmiScan.completed && stop.resets==1);stop.releaseWMIRuntime();
    ScanDriver wrap;wrap._wmiScan.completed=true;wrap.tick(1000);wrap.tick(1010);wrap.tick(1020);wrap.tick(1030);
    wrap._wmiScan={};wrap._regs.tx3Written=wrap._regs.tx3Read=wrap._wmiScanChannels.txNext=31;
    assert(!wrap.serviceWMIScan(1040) && wrap._wmiScan.txNext==0);wrap.releaseWMIRuntime();
    // Parser rejects malformed/cross-request events without committing credits or state.
    auto valid=scanPacket(1,0,true);
    for(unsigned n=0;n<valid.size();++n){
        Ath10k::HTCCreditState c;c.initialize(2,1792,2);c.reserve(200);
        Ath10k::WMIInitState o;Ath10k::WMIScanState s;s.submitted=true;
        assert(Ath10k::parseWMIInitResponse(valid.data(),n,2,c,o,nullptr,&s));assert(!s.started && c.outstanding==1);
    }
    for(unsigned off:{14u,28u,32u,36u,16u}){
        auto p=valid;p[off]=0xff;
        Ath10k::HTCCreditState c;c.initialize(2,1792,2);c.reserve(200);
        Ath10k::WMIInitState o;Ath10k::WMIScanState s;s.submitted=true;
        assert(Ath10k::parseWMIInitResponse(p.data(),p.size(),2,c,o,nullptr,&s));assert(!s.started && c.outstanding==1);
    }
    Ath10k::HTCCreditState c;c.initialize(2,1792,2);c.reserve(200);
    Ath10k::WMIInitState o;Ath10k::WMIScanState s;s.submitted=true;
    auto badCredit=valid;badCredit[45]=3;
    assert(Ath10k::parseWMIInitResponse(badCredit.data(),badCredit.size(),2,c,o,nullptr,&s));assert(!s.started && c.outstanding==1);
    assert(!Ath10k::parseWMIInitResponse(valid.data(),valid.size(),2,c,o,nullptr,&s));assert(s.started && c.available==2);
    auto dup=scanPacket(1);assert(Ath10k::parseWMIInitResponse(dup.data(),dup.size(),2,c,o,nullptr,&s));
    auto mgmt=mgmtPacket();mgmt[32]=25;
    assert(Ath10k::parseWMIInitResponse(mgmt.data(),mgmt.size(),2,c,o,nullptr,&s));assert(!s.mgmtCount);
    // Exact complete frame from hardware log 2026-09-21 00:18:15.887.
    // Extended fixed TLV, STARTED, reason=7, same request/scan/VDEV IDs.
    const std::vector<UInt8> captured={
        0x02,0x00,0x24,0x00,0x00,0x0e,0x00,0x00,0x01,0x30,0x00,0x00,
        0x1c,0x00,0x24,0x00,0x01,0x00,0x00,0x00,0x07,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x01,0xa0,0x00,0x00,0x36,0xa0,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    ScanDriver hardware;hardware.launch();hardware.deliver(2,captured);hardware.tick(1040);
    assert(hardware._wmiScan.started && hardware._wmiScan.reason==7 && !hardware._wmiScan.failed);
    assert(hardware._wmiLive.active && !hardware._wmiScan.completed);
    auto extendedEnd=captured;extendedEnd[16]=2;extendedEnd[20]=0;extendedEnd[40]=0xab;
    hardware.deliver(2,extendedEnd);hardware.tick(1200);
    assert(hardware._wmiScan.completed && hardware._wmiScan.reason==0 && hardware._wmiScan.eventCount==2);
    hardware.releaseWMIRuntime();
    for(UInt32 reason:{5u,6u,7u,255u,0xffffffffu}) {
        ScanDriver rejected;rejected.launch();rejected.deliver(2,captured);rejected.tick(1040);
        auto end=extendedEnd;Ath10k::htcWriteLE32(end.data()+20,reason);
        rejected.deliver(2,end);rejected.tick(1050);
        assert(rejected._wmiScan.failed && rejected._wmiScan.terminal && !rejected._wmiScan.completed);
        assert(rejected._wmiScan.reason==reason && !rejected._wmiLive.active && rejected.resets==1);
        rejected.releaseWMIRuntime();
    }
    // Both known layouts accept opaque nonterminal reason values.
    for(bool extended:{false,true}) {
        auto start=extended?captured:scanPacket(1,7);
        Ath10k::HTCCreditState credits;credits.initialize(2,1792,2);
        Ath10k::WMIInitState events;Ath10k::WMIScanState state;state.submitted=true;
        assert(!Ath10k::parseWMIInitResponse(start.data(),start.size(),2,credits,events,nullptr,&state));
        auto channel=scanPacket(8,255);
        assert(!Ath10k::parseWMIInitResponse(channel.data(),channel.size(),2,credits,events,nullptr,&state));
        assert(state.started && !state.failed && !state.terminal && state.reason==255);
    }
    // Reject truncated extended frames and unknown fixed lengths, without state commits.
    for(unsigned n=0;n<captured.size();++n) {
        Ath10k::HTCCreditState credits;credits.initialize(2,1792,2);
        Ath10k::WMIInitState events;Ath10k::WMIScanState state;state.submitted=true;
        assert(Ath10k::parseWMIInitResponse(captured.data(),n,2,credits,events,nullptr,&state));
        assert(!state.started && !state.eventCount);
    }
    for(unsigned len:{20u,25u,27u,29u,32u}) {
        auto bad=captured;bad.resize(16+len);bad[12]=len;bad[2]=8+len;
        Ath10k::HTCCreditState credits;credits.initialize(2,1792,2);
        Ath10k::WMIInitState events;Ath10k::WMIScanState state;state.submitted=true;
        assert(Ath10k::parseWMIInitResponse(bad.data(),bad.size(),2,credits,events,nullptr,&state));
        assert(!state.started && !state.eventCount);
    }
    auto extendedCredit=captured;addPkgCredit(extendedCredit);extendedCredit[49]=3;
    Ath10k::HTCCreditState ec;ec.initialize(2,1792,2);ec.reserve(200);
    Ath10k::WMIInitState eo;Ath10k::WMIScanState es;es.submitted=true;
    assert(Ath10k::parseWMIInitResponse(extendedCredit.data(),extendedCredit.size(),2,ec,eo,nullptr,&es));
    assert(!es.started && !es.eventCount && ec.outstanding==1);
    extendedCredit[49]=1;
    assert(!Ath10k::parseWMIInitResponse(extendedCredit.data(),extendedCredit.size(),2,ec,eo,nullptr,&es));
    assert(es.started && ec.outstanding==0);
    // Full 38-channel dwell exceeds the old 5-second host deadline.
    ScanDriver multi;multi.launch();multi.deliver(2,scanPacket(1,7));multi.tick(1040);
    for(unsigned i=0;i<38;++i){
        auto event=scanPacket(8,7);Ath10k::htcWriteLE32(event.data()+24,expectedScanFrequencies[i]);
        multi.deliver(2,event);multi.tick(1050+500*i);
        assert(multi._wmiLive.active && multi._wmiScan.visitedMask==((1ULL<<(i+1))-1));
    }
    multi.deliver(2,scanPacket(2));multi.tick(20100);
    assert(multi._wmiScan.completed && multi._wmiScan.elapsedMs>19000 && multi._wmiScan.visitedMask==((1ULL<<38)-1));
    multi.releaseWMIRuntime();
    for(unsigned freq:{2407u,2413u,2484u,5181u,5746u,5845u}){
        Ath10k::HTCCreditState cr;cr.initialize(2,1792,2);
        Ath10k::WMIInitState ev;Ath10k::WMIScanState st;st.submitted=true;st.started=true;
        auto event=scanPacket(8);Ath10k::htcWriteLE32(event.data()+24,freq);
        assert(Ath10k::parseWMIInitResponse(event.data(),event.size(),2,cr,ev,nullptr,&st));
        assert(!st.visitedMask);
    }
    UInt8 frame[293];memset(frame,0xaa,sizeof(frame));
    assert(Ath10k::buildWMIScan(frame,292,2) && frame[292]==0xaa);
    assert(!Ath10k::buildWMIScan(frame,291,2));assert(!Ath10k::buildWMIScan(frame,292,0));
    assert(!Ath10k::buildWMIScan(frame,292,255));assert(!Ath10k::buildWMIScan(nullptr,292,2));
}

#endif
