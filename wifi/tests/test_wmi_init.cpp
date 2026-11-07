#define ATH10K_WMI_INIT_TEST
#include "test_wmi_bootstrap.cpp"

static std::vector<UInt8> servicePacket() {
    auto p=event();
    for (unsigned i=0;i<6;++i) Ath10k::htcWriteLE32(p.data()+20+4*i,Ath10k::kWMIHostABI[i]);
    Ath10k::htcWriteLE32(p.data()+88,0);
    p.resize(256);
    Ath10k::htcWriteLE16(p.data()+120,128);p[122]=16;
    p[254]=18; // Empty memory requests array.
    Ath10k::htcWriteLE16(p.data()+2,p.size()-8);
    return p;
}
static std::vector<UInt8> readyPacket(bool credit) {
    std::vector<UInt8> p(credit?60:52);
    p[0]=2;p[2]=p.size()-8;p[8]=2;p[12]=36;p[14]=35;
    for(unsigned i=0;i<6;++i) Ath10k::htcWriteLE32(p.data()+16+4*i,Ath10k::kWMIHostABI[i]);
    p[40]=2;p[45]=1; // Locally administered synthetic MAC.
    if(credit){p[1]=2;p[4]=8;p[52]=1;p[53]=4;p[56]=2;p[57]=1;}
    return p;
}
// Same event/TLV lengths as hardware log; diagnostic contents are synthetic.
[[maybe_unused]] static std::vector<UInt8> diagPacket() {
    std::vector<UInt8> p(376);
    p[0]=2; Ath10k::htcWriteLE16(p.data()+2,368);
    Ath10k::htcWriteLE32(p.data()+8,0x1d011);
    Ath10k::htcWriteLE16(p.data()+12,360);p[14]=17;
    return p;
}
// Exact 20-byte PKGID packet captured on QCA6174 in the 0.0.29 log.
[[maybe_unused]] static std::vector<UInt8> pkgPacket() {
    return {2,0,12,0,0,7,0,0,0x19,0xd0,1,0,4,0,0x5d,2,0,0,0,0};
}
[[maybe_unused]] static void addPkgCredit(std::vector<UInt8> &p, UInt8 count=1) {
    p[1]=2;p[4]=8;p.insert(p.end(),{1,4,0,0,2,count,0,0});
    Ath10k::htcWriteLE16(p.data()+2,p.size()-8);
}
// Captured 64-byte prefix and 160-byte framing; remaining entries are synthetic.
[[maybe_unused]] static std::vector<UInt8> supportedPacket() {
    std::vector<UInt8> p(160);
    p[0]=2;p[2]=152;p[5]=8;
    Ath10k::htcWriteLE32(p.data()+8,0x16006);
    p[12]=4;p[14]=0xa2;p[15]=1;p[16]=34;
    p[20]=136;p[22]=16;
    const UInt32 first[]={0x11857,0x1189a,0x118f1,0x118f2,0x67b,0x67c,0x67d,0x67e,0x9ca,0x9cb};
    for(unsigned i=0;i<34;++i)Ath10k::htcWriteLE32(p.data()+24+4*i,i<10?first[i]:i);
    return p;
}
class InitDriver:public WMIDriver {
public:
    bool split=false, noCredit=false, noReady=false, rejected=false;
    unsigned initSends=0;
    InitDriver() {
        _htcWMIControl.uplinkPipe=3;_htcWMIControl.maxMessageSize=1784;
        _wmiTxBuffer.isAllocated=false;
        _wmiRxBuffer.isAllocated=false; // Exercise the real 2048-byte RX allocation.
        _regs.tx3Read=_regs.tx3Written=31;
        _regs.wmiPost=nullptr;
        _regs.consume=[this](){++sent;deliver(2,servicePacket());};
        _regs.tx3Consume=[this](){
            ++initSends;
            assert(_wmi.credits.available==1 && _wmi.credits.outstanding==1);
            auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[3].virtualAddress());
            assert(ring[31].flags==8 && ring[31].nbytes==228+20*_wmi.hostMemoryCount);
            auto *p=static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress());
            assert(p[0]==2 && p[1]==1 && p[5]==0 && Ath10k::wmiReadLE32(p+8)==1);
            assert(Ath10k::htcReadLE16(p+14)==74 && Ath10k::htcReadLE16(p+46)==75);
            assert(Ath10k::wmiReadLE32(p+76)==((1u<<_wmi.rfChains)-1));
            assert(Ath10k::wmiReadLE32(p+80)==Ath10k::wmiReadLE32(p+76));
            assert(_wmiInit.requestedChainMask==Ath10k::wmiReadLE32(p+76));
            assert(Ath10k::wmiReadLE32(p+40)==_wmi.hostMemoryCount && Ath10k::htcReadLE16(p+226)==18);
            if(!noReady){auto ready=readyPacket(!split&&!noCredit);if(rejected)ready[48]=1;deliver(2,ready);}
            if(split&&!noCredit) deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
    }
    void deliver(unsigned ce,const std::vector<UInt8> &p) {
        auto &buf=ce==1?_htcReadyBuffer:_wmiRxBuffer;
        memcpy(buf.virtualAddress(),p.data(),p.size());
        auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[ce].virtualAddress());
        unsigned slot=ce==1?_runtimeRxSlot:_wmi.slot;
        ring[slot].nbytes=p.size();
        if(ce==2)_regs.wmiRead=_wmi.next;
    }
    void bootstrap(){assert(runWMIBootstrap());assert(_wmi.memoryRequests==0 && _wmi.serviceBitmapPresent);}
};
#ifndef ATH10K_WMI_RUNTIME_TEST
int main(){
    InitDriver good;good.bootstrap();assert(good.runWMIInit());
    assert(good._wmiInit.completed && !good._htcCreditFlowDeferred);
    assert(!strcmp(good._wmi.status,"service-ready-before-reset"));
    assert(good._wmiInit.ready && good._wmiInit.creditReturned && good._wmiInit.txConsumed);
    assert(good._wmi.credits.available==2 && good._wmi.credits.outstanding==0);
    assert(!good.runWMIInit() && good.initSends==1);
    assert(good.quiesceTargetAfterBMI() && !good._wmiInit.txOwned);
    InitDriver split;split.split=true;split.bootstrap();assert(split.runWMIInit());assert(split._wmiInit.rxCount==2);
    InitDriver credit;credit.noCredit=true;credit.bootstrap();assert(!credit.runWMIInit());
    assert(credit._wmiInit.ready && !strcmp(credit._wmiInit.status,"wmi-credit-return-timeout"));
    InitDriver ready;ready.noReady=ready.split=true;ready.bootstrap();assert(!ready.runWMIInit());
    assert(ready._wmiInit.creditReturned && !strcmp(ready._wmiInit.status,"wmi-ready-timeout"));
    InitDriver stalled;stalled._regs.tx3Timeout=true;stalled.bootstrap();assert(!stalled.runWMIInit());
    assert(!strcmp(stalled._wmiInit.status,"wmi-init-tx-timeout"));
    InitDriver fail;fail._regs.tx3Fail=true;fail.bootstrap();assert(!fail.runWMIInit() && fail._wmiInit.txOwned);
    fail.resetOK=false;assert(!fail.quiesceTargetAfterBMI() && fail._wmiInit.txOwned);
    InitDriver bad;bad.rejected=true;bad.bootstrap();assert(!bad.runWMIInit());assert(bad._wmiInit.statusCode==1);
    InitDriver abi;abi.bootstrap();abi._wmi.abi[2]^=1;assert(!abi.runWMIInit() && abi.initSends==0);
    InitDriver mem;mem.bootstrap();mem._wmi.memoryRequests=1;assert(!mem.runWMIInit() && mem.initSends==0);
    InitDriver empty;empty.bootstrap();empty._wmi.credits.available=0;assert(!empty.runWMIInit() && empty.initSends==0);
    InitDriver diagnostics;diagnostics.noCredit=true;unsigned reposts=0;
    diagnostics._regs.wmiPost=[&](){
        if(!diagnostics.initSends)return;
        if(++reposts==1) diagnostics.deliver(2,diagPacket());
        else diagnostics.deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
    };
    diagnostics.bootstrap();assert(diagnostics.runWMIInit());
    assert(diagnostics._wmiInit.diagCount==1 && diagnostics._wmiInit.diagBytes==360);
    assert(diagnostics._wmiInit.ready && diagnostics._wmiInit.statusCode==0);
    assert(diagnostics._wmiInit.rxCount==3 && diagnostics._wmi.credits.available==2);
    InitDriver diagTimeout;diagTimeout.noCredit=true;unsigned timeoutPosts=0;
    diagTimeout._regs.wmiPost=[&](){
        if(diagTimeout.initSends && ++timeoutPosts==1)diagTimeout.deliver(2,diagPacket());
    };
    diagTimeout.bootstrap();assert(!diagTimeout.runWMIInit());
    assert(diagTimeout._wmiInit.waitMs==5000 && diagTimeout._wmiInit.ready);
    assert(!strcmp(diagTimeout._wmiInit.status,"wmi-credit-return-timeout"));
    InitDriver flood;flood.noCredit=true;
    flood._regs.wmiPost=[&](){if(flood.initSends)flood.deliver(2,diagPacket());};
    flood.bootstrap();assert(!flood.runWMIInit());
    assert(!strcmp(flood._wmiInit.status,"wmi-init-event-limit") && !flood._wmiInit.completed);
    auto diag=diagPacket();
    for(unsigned n=0;n<diag.size();++n){
        Ath10k::HTCCreditState ledger;ledger.initialize(2,1792,2);ledger.reserve(228);
        Ath10k::WMIInitState state;state.ready=true;state.statusCode=0;
        assert(Ath10k::parseWMIInitResponse(diag.data(),n,2,ledger,state));
        assert(state.ready && state.diagCount==0 && ledger.outstanding==1);
    }
    Ath10k::HTCCreditState ledger;ledger.initialize(2,1792,2);ledger.reserve(228);
    Ath10k::WMIInitState state;
    assert(!Ath10k::parseWMIInitResponse(diag.data(),diag.size(),2,ledger,state));
    assert(!state.ready && !state.creditReturned && ledger.outstanding==1);
    diag[14]=18;assert(Ath10k::parseWMIInitResponse(diag.data(),diag.size(),2,ledger,state));
    diag=diagPacket();diag[8]=0x12;
    assert(!Ath10k::parseWMIInitResponse(diag.data(),diag.size(),2,ledger,state) && state.unknownCount==1);
    diag=diagPacket();diag.resize(384);diag[1]=2;diag[4]=8;
    Ath10k::htcWriteLE16(diag.data()+2,376);
    diag[376]=1;diag[377]=4;diag[380]=2;diag[381]=1;
    assert(!Ath10k::parseWMIInitResponse(diag.data(),diag.size(),2,ledger,state));
    assert(ledger.available==2 && state.creditReturned && !state.ready);
    // READY -> DIAG -> captured PKGID -> credit on CE1 or CE2.
    for (bool piggyback : {false,true}) {
        InitDriver pkg;pkg.noCredit=true;unsigned posts=0;
        pkg._regs.wmiPost=[&](){
            if(!pkg.initSends)return;
            ++posts;
            if(posts==1)pkg.deliver(2,diagPacket());
            else if(posts==2){auto packet=pkgPacket();if(piggyback)addPkgCredit(packet);pkg.deliver(2,packet);}
            else pkg.deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
        pkg.bootstrap();assert(pkg.runWMIInit());
        assert(pkg._wmiInit.pkgIdCount==1 && pkg._wmiInit.pkgId==0);
        assert(pkg._wmiInit.diagCount==1 && pkg._wmiInit.ready && pkg._wmiInit.statusCode==0);
        assert(pkg._wmiInit.rxCount==(piggyback?3u:4u) && pkg._wmi.credits.available==2);
    }
    InitDriver pkgTimeout;pkgTimeout.noCredit=true;unsigned pkgPosts=0;
    pkgTimeout._regs.wmiPost=[&](){if(pkgTimeout.initSends && ++pkgPosts==1)pkgTimeout.deliver(2,pkgPacket());};
    pkgTimeout.bootstrap();assert(!pkgTimeout.runWMIInit());
    assert(pkgTimeout._wmiInit.pkgIdCount==1 && pkgTimeout._wmiInit.ready);
    assert(!strcmp(pkgTimeout._wmiInit.status,"wmi-credit-return-timeout"));
    InitDriver pkgFlood;pkgFlood.noCredit=true;
    pkgFlood._regs.wmiPost=[&](){if(pkgFlood.initSends)pkgFlood.deliver(2,pkgPacket());};
    pkgFlood.bootstrap();assert(!pkgFlood.runWMIInit());
    assert(!strcmp(pkgFlood._wmiInit.status,"wmi-init-event-limit") && !pkgFlood._wmiInit.completed);
    auto rejectPkg=[](const std::vector<UInt8> &packet) {
        Ath10k::HTCCreditState c;c.initialize(2,1792,2);c.reserve(228);
        Ath10k::WMIInitState st;st.ready=true;st.statusCode=0;st.pkgId=42;
        assert(Ath10k::parseWMIInitResponse(packet.data(),packet.size(),2,c,st));
        assert(c.outstanding==1 && c.available==1 && st.ready && st.statusCode==0);
        assert(st.pkgIdCount==0 && st.pkgId==42 && !st.creditReturned);
    };
    auto package=pkgPacket();
    for(unsigned n=0;n<package.size();++n)rejectPkg({package.begin(),package.begin()+n});
    package=pkgPacket();package[14]=0x5c;rejectPkg(package);
    package=pkgPacket();package[12]=3;rejectPkg(package);
    package=pkgPacket();package.resize(12);package[2]=4;rejectPkg(package);
    package=pkgPacket();package.insert(package.end(),{4,0,0x5d,2,0,0,0,0});package[2]=20;rejectPkg(package);
    package=pkgPacket();package[0]=0;rejectPkg(package);
    package=pkgPacket();package[8]=0x1a;
    {Ath10k::HTCCreditState c;c.initialize(2,1792,2);c.reserve(228);Ath10k::WMIInitState st;
     assert(!Ath10k::parseWMIInitResponse(package.data(),package.size(),2,c,st) && st.unknownCount==1 && c.outstanding==1);}
    package=pkgPacket();addPkgCredit(package,2);rejectPkg(package); // No partial commit on excess credit.
    package=pkgPacket();package[16]=0x80;addPkgCredit(package);
    Ath10k::HTCCreditState pkgLedger;pkgLedger.initialize(2,1792,2);pkgLedger.reserve(228);
    Ath10k::WMIInitState pkgState;
    assert(!Ath10k::parseWMIInitResponse(package.data(),package.size(),2,pkgLedger,pkgState));
    assert(pkgState.pkgId==0x80 && pkgState.pkgIdCount==1 && pkgState.creditReturned && !pkgState.ready);
    auto laterReady=readyPacket(false);
    assert(!Ath10k::parseWMIInitResponse(laterReady.data(),laterReady.size(),2,pkgLedger,pkgState));
    assert(pkgState.ready && pkgState.pkgId==0x80 && pkgState.pkgIdCount==1);
    InitDriver support;support.noCredit=true;unsigned supportPosts=0;
    support._regs.wmiPost=[&](){
        if(!support.initSends)return;
        if(++supportPosts==1)support.deliver(2,supportedPacket());
        else support.deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
    };
    support.bootstrap();assert(support.runWMIInit());
    assert(support._wmiInit.diagSupportedCount==1 && support._wmiInit.diagSupportedEntries==34);
    assert(support._wmiInit.ready && support._wmi.credits.available==2);
    auto rejectSupported=[](const std::vector<UInt8> &packet) {
        Ath10k::HTCCreditState c;c.initialize(2,1792,2);c.reserve(228);
        Ath10k::WMIInitState st;st.ready=true;st.statusCode=0;st.diagSupportedEntries=7;
        assert(Ath10k::parseWMIInitResponse(packet.data(),packet.size(),2,c,st));
        assert(c.outstanding==1 && c.available==1 && st.ready && st.statusCode==0);
        assert(!st.diagSupportedCount && st.diagSupportedEntries==7);
    };
    auto supp=supportedPacket();
    for(unsigned n=0;n<supp.size();++n)rejectSupported({supp.begin(),supp.begin()+n});
    supp=supportedPacket();supp[16]=35;rejectSupported(supp);
    supp=supportedPacket();Ath10k::htcWriteLE32(supp.data()+16,0xffffffffu);rejectSupported(supp);
    supp=supportedPacket();supp[20]=135;rejectSupported(supp);
    supp=supportedPacket();supp[14]=0xa1;rejectSupported(supp);
    supp=supportedPacket();supp[22]=17;rejectSupported(supp);
    supp=supportedPacket();supp[12]=8;rejectSupported(supp);
    supp=supportedPacket();supp.resize(20);supp[2]=12;rejectSupported(supp);
    supp=supportedPacket();supp.erase(supp.begin()+12,supp.begin()+20);supp[2]=supp.size()-8;rejectSupported(supp);
    supp=supportedPacket();supp.insert(supp.end(),{4,0,0xa2,1,34,0,0,0});supp[2]=supp.size()-8;rejectSupported(supp);
    supp=supportedPacket();supp.insert(supp.end(),{0,0,16,0});supp[2]=supp.size()-8;rejectSupported(supp);
    supp=supportedPacket();addPkgCredit(supp,2);rejectSupported(supp);
    supp=supportedPacket();addPkgCredit(supp);
    Ath10k::HTCCreditState supportedLedger;supportedLedger.initialize(2,1792,2);supportedLedger.reserve(228);
    Ath10k::WMIInitState supportedState;
    assert(!Ath10k::parseWMIInitResponse(supp.data(),supp.size(),2,supportedLedger,supportedState));
    assert(!supportedState.ready && supportedState.creditReturned && supportedState.diagSupportedEntries==34);
    supp=supportedPacket();supp.resize(24);supp[2]=16;supp[16]=0;supp[20]=0;
    assert(!Ath10k::parseWMIInitResponse(supp.data(),supp.size(),2,supportedLedger,supportedState));
    assert(supportedState.diagSupportedCount==2 && supportedState.diagSupportedEntries==0);
    UInt8 frame[228];assert(!Ath10k::buildWMIInit(frame,sizeof(frame),good._wmi,2));
    // INIT masks follow reported hardware count; all other bytes are invariant.
    UInt8 baseline[228];memcpy(baseline,frame,sizeof(frame));
    for(UInt32 chains:{1u,2u,3u}){
        auto boot=good._wmi;boot.rfChains=chains;
        assert(!Ath10k::buildWMIInit(frame,sizeof(frame),boot,2));
        assert(Ath10k::wmiReadLE32(frame+76)==((1u<<chains)-1));
        assert(Ath10k::wmiReadLE32(frame+80)==((1u<<chains)-1));
        for(unsigned i=0;i<sizeof(frame);++i)if(i<76 || i>=84)assert(frame[i]==baseline[i]);
    }
    for(UInt32 chains:{0u,4u,32u,0xffffffffu}){
        auto boot=good._wmi;boot.rfChains=chains;memset(frame,0xaa,sizeof(frame));
        assert(Ath10k::buildWMIInit(frame,sizeof(frame),boot,2));
        for(auto byte:frame)assert(byte==0xaa); // reject before write/shift
    }

    memcpy(frame,baseline,sizeof(frame));
    assert(Ath10k::wmiReadLE32(frame+48)==4 && Ath10k::wmiReadLE32(frame+52)==33);
    auto r=readyPacket(true);
    for(unsigned n=0;n<r.size();++n){Ath10k::HTCCreditState c;c.initialize(2,1792,2);c.reserve(228);Ath10k::WMIInitState st;
        assert(Ath10k::parseWMIInitResponse(r.data(),n,2,c,st));assert(c.outstanding==1&&!st.ready);}
    Ath10k::HTCCreditState c;c.initialize(2,1792,2);c.reserve(228);Ath10k::WMIInitState st;
    assert(!Ath10k::parseWMIInitResponse(r.data(),r.size(),2,c,st));
    assert(Ath10k::parseWMIInitResponse(r.data(),r.size(),2,c,st)); // Duplicate return rejected.
}

#endif
