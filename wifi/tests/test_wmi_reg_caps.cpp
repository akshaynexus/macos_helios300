#define ATH10K_WMI_RUNTIME_TEST
#include "test_wmi_init.cpp"
#include "wmi_runtime_methods.inc"

// The 40-byte variant matches the captured 324-byte frame's first 32 bytes.
// Remaining fields (including regulatory values) are synthetic. The capture
// does not include the actual regulatory TLV body or its header.
static std::vector<UInt8> regulatoryPacket(unsigned regBytes) {
    std::vector<UInt8> p(12+4+128+4+regBytes+4+128+4);
    p[0]=2;p[5]=4;p[8]=1;
    Ath10k::htcWriteLE16(p.data()+2,p.size()-8);
    Ath10k::htcWriteLE16(p.data()+12,128);
    Ath10k::htcWriteLE16(p.data()+14,32);
    Ath10k::htcWriteLE32(p.data()+16,0x441a8135);
    const UInt32 abi[]={0x01000000,0x3c8,0x5f414351,0x4c4d,0,0};
    for(unsigned i=0;i<6;++i)Ath10k::htcWriteLE32(p.data()+20+4*i,abi[i]);
    Ath10k::htcWriteLE32(p.data()+52,2);
    Ath10k::htcWriteLE32(p.data()+56,2139);
    Ath10k::htcWriteLE32(p.data()+60,864027058);
    Ath10k::htcWriteLE32(p.data()+64,65530);
    Ath10k::htcWriteLE32(p.data()+72,20);
    Ath10k::htcWriteLE16(p.data()+144,regBytes);
    Ath10k::htcWriteLE16(p.data()+146,33);
    const UInt32 caps[]={0x64,1,2,3,4,2412,2472,5180,5825};
    for(unsigned i=0;i<9 && 4*i+4<=regBytes;++i)
        Ath10k::htcWriteLE32(p.data()+148+4*i,caps[i]);
    for(unsigned i=36;i<regBytes;++i)p[148+i]=0xa5;
    const unsigned bitmap=148+regBytes;
    Ath10k::htcWriteLE16(p.data()+bitmap,128);
    Ath10k::htcWriteLE16(p.data()+bitmap+2,16);
    Ath10k::htcWriteLE32(p.data()+bitmap+4,0x13579bdf);
    Ath10k::htcWriteLE16(p.data()+bitmap+132+2,18);
    return p;
}

static void acceptedCaps(const Ath10k::WMIBootstrapState &s,unsigned regBytes) {
    assert(s.serviceReady && s.regCapsPresent);
    assert(s.regCapsBytes==regBytes && s.regCapsOffset==144 && s.regCapsCount==1);
    assert(s.eepromRD==0x64 && s.eepromRDExt==1 && s.regCap1==2 && s.regCap2==3);
    assert(s.wirelessModes==4 && s.low2==2412 && s.high2==2472 && s.low5==5180 && s.high5==5825);
    assert(s.rfChains==2 && s.hwMaxPower==20 && s.htCapability==2139);
    assert(s.serviceBitmapPresent && s.serviceBitmap[0]==0x13579bdf && s.memoryArrayEmpty);
    assert(Ath10k::wmiABICompatible(s.abi));
}

int main() {
    assert(Ath10k::kWMIRegCapabilitiesMinBytes==36);
    for(unsigned len:{36u,37u,40u,44u,128u,1764u}) {
        auto p=regulatoryPacket(len);
        Ath10k::WMIBootstrapState s;
        assert(!Ath10k::parseWMIBootstrapEvent(p.data(),p.size(),2,s));
        acceptedCaps(s,len);
        assert(s.servicePacketBytes==p.size());
    }
    auto extended=regulatoryPacket(40);
    const UInt8 capturedPrefix[]={
        0x02,0x00,0x3c,0x01,0x00,0x04,0x00,0x00,
        0x01,0x00,0x00,0x00,0x80,0x00,0x20,0x00,
        0x35,0x81,0x1a,0x44,0x00,0x00,0x00,0x01,
        0xc8,0x03,0x00,0x00,0x51,0x43,0x41,0x5f};
    assert(extended.size()==324 && !memcmp(extended.data(),capturedPrefix,sizeof(capturedPrefix)));
    for(unsigned len=0;len<36;++len) {
        auto p=regulatoryPacket(len);
        Ath10k::WMIBootstrapState s;s.eepromRD=0x777;
        const char *e=Ath10k::parseWMIBootstrapEvent(p.data(),p.size(),2,s);
        assert(e && !strcmp(e,"wmi-reg-capabilities-size"));
        assert(!s.serviceReady && !s.regCapsPresent && s.eepromRD==0x777);
        assert(s.regCapsBytes==len && s.regCapsCount==1);
    }
    for(unsigned bytes=0;bytes<extended.size();++bytes) {
        Ath10k::WMIBootstrapState s;s.eepromRD=0x777;
        assert(Ath10k::parseWMIBootstrapEvent(extended.data(),bytes,2,s));
        assert(!s.serviceReady && !s.regCapsPresent && s.eepromRD==0x777);
    }
    auto bad=extended;
    Ath10k::htcWriteLE16(bad.data()+144,0xffff);
    Ath10k::WMIBootstrapState kept;
    kept.eepromRD=0x777;kept.credits.initialize(2,1792,2);kept.credits.reserve(228);
    assert(!strcmp(Ath10k::parseWMIBootstrapEvent(bad.data(),bad.size(),2,kept),"wmi-truncated-tlv"));
    assert(kept.eepromRD==0x777 && kept.credits.available==1 && kept.credits.outstanding==1);
    bad=extended;
    bad.insert(bad.end(),extended.begin()+144,extended.begin()+188);
    Ath10k::htcWriteLE16(bad.data()+2,bad.size()-8);
    assert(!strcmp(Ath10k::parseWMIBootstrapEvent(bad.data(),bad.size(),2,kept),"wmi-reg-capabilities-duplicate"));
    assert(kept.regCapsCount==2 && kept.eepromRD==0x777 && kept.credits.outstanding==1);
    auto withCredit=extended;
    withCredit[1]=2;withCredit[4]=8;
    withCredit.insert(withCredit.end(),{1,4,0,0,2,1,0,0});
    Ath10k::htcWriteLE16(withCredit.data()+2,withCredit.size()-8);
    assert(!Ath10k::parseWMIBootstrapEvent(withCredit.data(),withCredit.size(),2,kept));
    acceptedCaps(kept,40);
    assert(kept.credits.available==2 && kept.credits.outstanding==0);
    bad=withCredit;
    Ath10k::htcWriteLE16(bad.data()+144,35);
    kept.credits.reserve(228);kept.eepromRD=0x888;
    assert(Ath10k::parseWMIBootstrapEvent(bad.data(),bad.size(),2,kept));
    assert(kept.eepromRD==0x888 && kept.credits.available==1 && kept.credits.outstanding==1);
    for(unsigned len:{36u,40u,44u,128u}) {
        InitDriver d;auto packet=regulatoryPacket(len);
        d._regs.consume=[&](){++d.sent;d.deliver(2,packet);};
        d.bootstrap();acceptedCaps(d._wmi,len);
        assert(d.runWMIInit() && d.initSends==1 && d._wmiInit.completed);
        assert(d._wmi.credits.available==2 && d._wmi.credits.outstanding==0);
        assert(d.quiesceTargetAfterBMI());
    }
    InitDriver shortCaps;auto packet=regulatoryPacket(32);
    shortCaps._regs.consume=[&](){++shortCaps.sent;shortCaps.deliver(2,packet);};
    assert(!shortCaps.runWMIBootstrap() && !shortCaps._wmi.serviceReady && !shortCaps.initSends);
    assert(shortCaps._wmi.rxOwned && shortCaps.quiesceTargetAfterBMI());
    puts("WMI regulatory TLV integration passed: 36-byte prefix and bounded extensions, captured envelope/prefix with synthetic body, short/truncated/duplicate rejection, transactional caps/credits, actual CE2 bootstrap and CE3 INIT.");
}
