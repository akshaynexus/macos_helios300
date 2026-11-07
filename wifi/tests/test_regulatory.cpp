#define ATH10K_WMI_BSS_TEST
#include "test_wmi_scan.cpp"
struct RegulatoryDriver:ScanDriver {
    unsigned radioCommands=0,hold=999;
    RegulatoryDriver(){
        for(auto &r:_wmiRadio)r={};
        _wmi.regCapsPresent=true;_wmi.eepromRD=0x64;_wmi.low2=2412;_wmi.high2=2472;_wmi.low5=5180;_wmi.high5=5825;
        auto previous=_regs.tx3Consume;
        _regs.tx3Consume=[this,previous](){
            auto *f=static_cast<const UInt8 *>(_wmiTxBuffer.virtualAddress());const UInt32 cmd=Ath10k::wmiReadLE32(f+8);
            if(cmd!=0x4003 && cmd!=0x4001){previous();return;}
            unsigned i=radioCommands++;assert(i<5 && !_wmiScanChannels.attempted && _wmiRadio[i].txOwned);
            if(i)assert(_wmiRadio[i-1].completed && _wmiRadio[i-1].txConsumed && _wmiRadio[i-1].creditReturned);
            assert(_wmi.credits.outstanding==1 && f[5]==i+6 && f[0]==2);
            if(i==2){assert(cmd==0x4001 && Ath10k::htcReadLE16(f+2)==32 && f[14]==81);
                for(unsigned j=0;j<3;++j)assert(Ath10k::wmiReadLE32(f+20+j*4)==0x64);
                assert(Ath10k::wmiReadLE32(f+32)==0xff && Ath10k::wmiReadLE32(f+36)==0xff);
            }else {assert(cmd==0x4003 && Ath10k::htcReadLE16(f+2)==20 && f[14]==82);
                assert(Ath10k::wmiReadLE32(f+20)==(i<2?i+1:i));assert(Ath10k::wmiReadLE32(f+24)==(i<2?3:20));}
            if(i!=hold)deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
    }
};
static void rules(){
    using namespace Ath10k;RegulatoryPolicy p;
    assert(regulatoryCountry("BR") && !regulatoryCountry("ZZ") && !regulatoryCountry("br") && !regulatoryCountry("BRA"));
    p.country=regulatoryCountry("BR");assert(!configureRegulatory(p,0x64,2412,2472,5180,5825));
    assert(p.firmwareCapsValid && p.domain!=0x64 && p.ctl2!=0xff);
    assert(!regulatoryPower(p,36,42,80));p.indoor=true;assert(regulatoryPower(p,36,42,80)==20);
    assert(!regulatoryPower(p,52,58,80) && regulatoryPower(p,52,52,20,false)==20);
    assert(regulatoryPower(p,149,155,80)==20 && regulatoryPower(p,13,13,20)==20);
    p.ceilingHalfDbm=6;assert(regulatoryPower(p,149,155,80)==6);
    assert(!configureRegulatory(p,0x8000,0,0,0,0) && p.low5==4800);
    assert(configureRegulatory(p,0xdead,2412,2472,5180,5825));
    assert(configureRegulatory(p,0x64,0,2472,5180,5825));
    p.ceilingHalfDbm=0;assert(configureRegulatory(p,0x64,2412,2472,5180,5825));p.ceilingHalfDbm=20;
    p.country=regulatoryCountry("US");assert(!configureRegulatory(p,0x8000|392,2412,2472,5180,5825));
    assert(p.boardCountry && p.boardCountry->alpha2[0]=='J'); // Fixed EEPROM country intersection.
    assert(!regulatoryPower(p,13,13,20));
    p.country=regulatoryCountry("BR");assert(!configureRegulatory(p,0x64,2412,2472,5745,5805));
    UInt8 list[kWMIScanChannelsFrameBytes+1],scan[kWMIScanFrameBytes+1];memset(list,0xab,sizeof(list));memset(scan,0xcd,sizeof(scan));
    assert(buildWMIScanChannels(list,sizeof(list)-1,2,&p));UInt32 count=wmiReadLE32(list+16),n=htcReadLE16(list+2)+8;
    assert(count==17 && n==24+28*count && list[n]==0xab); // 13 2.4GHz + four permitted 5GHz centers.
    assert(buildWMIScan(scan,sizeof(scan)-1,2,3,false,&p) && wmiReadLE32(scan+80)==count);
    assert(scan[htcReadLE16(scan+2)+8]==0xcd);
    for(unsigned i=0;i<count;++i){UInt32 freq=wmiReadLE32(list+28+i*28);assert(freq==wmiReadLE32(scan+120+i*4));assert(freq<=2472 || (freq>=5745 && freq<=5805));}
    p.low2=p.high2=2400;p.low5=p.high5=5000;assert(!buildWMIScanChannels(list,sizeof(list),2,&p));
}
static void serviceReady(){
    auto p=event();p.resize(p.size()+40);Ath10k::htcWriteLE16(p.data()+2,p.size()-8);
    unsigned o=p.size()-40;Ath10k::htcWriteLE16(p.data()+o,36);Ath10k::htcWriteLE16(p.data()+o+2,33);
    UInt32 values[9]={0x64,1,2,3,4,2412,2472,5180,5825};
    for(unsigned i=0;i<9;++i)Ath10k::htcWriteLE32(p.data()+o+4+i*4,values[i]);
    Ath10k::WMIBootstrapState state;assert(!Ath10k::parseWMIBootstrapEvent(p.data(),p.size(),2,state));
    assert(state.regCapsPresent && state.eepromRD==0x64 && state.high5==5825);
    auto bad=p;bad[o]=32;assert(Ath10k::parseWMIBootstrapEvent(bad.data(),bad.size(),2,state));assert(state.eepromRD==0x64);
    bad=p;bad.insert(bad.end(),p.begin()+o,p.end());Ath10k::htcWriteLE16(bad.data()+2,bad.size()-8);
    assert(Ath10k::parseWMIBootstrapEvent(bad.data(),bad.size(),2,state));
}
int main(){
    rules();serviceReady();
    {RegulatoryDriver d;for(unsigned t=1000;t<1250;t+=10)d.tick(t);
     assert(d._wmiLive.active && d.radioCommands==5 && d.lists==1 && d.scans==1 && d._regulatory.firmwareCapsValid);
     for(auto &r:d._wmiRadio)assert(r.completed && r.creditReturned && r.txConsumed);
     d.releaseWMIRuntime();}
    for(unsigned hold:{2u,3u,4u}){RegulatoryDriver d;d.hold=hold;for(unsigned t=1000;t<1250;t+=10)d.tick(t);
        assert(d.radioCommands==hold+1 && !d.lists && !d.scans);d.tick(6500);assert(!d._wmiLive.active && d.resets==1);d.releaseWMIRuntime();}
    {RegulatoryDriver d;d._wmi.regCapsPresent=false;for(unsigned t=1000;t<1250;t+=10)d.tick(t);assert(d.radioCommands==2 && !d.lists && !d.scans && !d._wmiLive.active);d.releaseWMIRuntime();}
    puts("Regulatory passed: country/EEPROM/environment/band intersection, variable scan lengths, bounded SERVICE_READY parser, serialized domain/power commands and credit failure gates.");
}
