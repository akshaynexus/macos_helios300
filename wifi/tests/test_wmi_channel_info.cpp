#define ATH10K_WMI_BSS_TEST
#include "test_wmi_scan.cpp"
static std::vector<UInt8> channelPacket(unsigned length=52,unsigned freq=2412,unsigned error=0){
    std::vector<UInt8> p(16+length);
    p[0]=2;Ath10k::htcWriteLE16(p.data()+2,p.size()-8);
    Ath10k::htcWriteLE32(p.data()+8,0x4002);
    Ath10k::htcWriteLE16(p.data()+12,length);p[14]=38;
    const UInt32 fields[]={error,freq,1,0xffffffa0,123,456,0,0,7,0,0,0,88};
    for(unsigned i=0;i<length/4 && i<13;++i)Ath10k::htcWriteLE32(p.data()+16+4*i,fields[i]);
    return p;
}
int main(){
    ScanDriver d;d.launch();d.deliver(2,scanPacket(1));d.tick(1040);
    for(unsigned i=0;i<38;++i){d.deliver(2,channelPacket(52,expectedScanFrequencies[i]));d.tick(1050+10*i);}
    assert(d._wmiScan.channelInfoMask==((1ULL<<38)-1) && d._wmiScan.channelInfoCount==38);
    assert(d._wmiScan.mgmtCount==0 && d._wmiBSS.count==0);
    d.deliver(2,mgmtPacket());d.tick(1490);assert(d._wmiScan.mgmtCount==1);
    d.deliver(2,scanPacket(2));d.tick(1500);assert(d._wmiScan.completed);
    auto last=channelPacket(24,5825,9);
    Ath10k::htcWriteLE32(last.data()+24,2); // flags differ from first snapshot.
    Ath10k::htcWriteLE32(last.data()+32,999); // clear counter.
    d.deliver(2,last);d.tick(1510);
    assert(d._wmiLive.active && d._wmiScan.channelInfoCount==39);
    assert(!d._wmiScan.channels[37].rxFramesPresent);
    const auto &first=d._wmiScan.firstChannels[37];const auto &latest=d._wmiScan.channels[37];
    assert(first.count==1 && first.flags==1 && first.clear==123 && first.rxFramesPresent && first.rxFrames==7);
    assert(first.errorCode==0 && latest.errorCode==9 && latest.count==2 && latest.clear==999 && latest.flags==2);
    d.releaseWMIRuntime();
    for(unsigned length:{24u,28u,32u,36u,52u,56u}){
        Ath10k::WMIScanState scan{};scan.submitted=true;
        Ath10k::HTCCreditState credits{};credits.initialize(2,1792,2);
        Ath10k::WMIInitState ev{};auto p=channelPacket(length);
        assert(!Ath10k::parseWMIInitResponse(p.data(),p.size(),2,credits,ev,nullptr,&scan));
        const auto &s=scan.channels[0];assert(s.count==1 && s.noise==0xffffffa0 && s.clear==123 && s.cycles==456);
        assert(s.rxFramesPresent==(length>=36));assert(s.rxFrames==(length>=36?7u:0u));
    }
    for(unsigned mode=0;mode<6;++mode){
        Ath10k::WMIScanState scan{};scan.submitted=true;
        Ath10k::HTCCreditState credits{};credits.initialize(2,1792,2);credits.reserve(184);
        Ath10k::WMIInitState ev{};auto p=channelPacket();
        if(mode==0)p=channelPacket(20);
        if(mode==1)p[14]=39;
        if(mode==2)p=channelPacket(52,2484);
        if(mode==3){auto copy=p;p.insert(p.end(),copy.begin()+12,copy.end());Ath10k::htcWriteLE16(p.data()+2,p.size()-8);}
        if(mode==4){p=channelPacket(25);}
        addPkgCredit(p);
        if(mode==5)p[p.size()-3]=3; // Invalid credit endpoint after a valid channel TLV.
        assert(Ath10k::parseWMIInitResponse(p.data(),p.size(),2,credits,ev,nullptr,&scan));
        assert(!scan.channelInfoCount && !scan.channelInfoMask && !scan.firstChannels[0].count && credits.outstanding==1);
    }
    Ath10k::WMIScanState scan{};scan.submitted=true;
    Ath10k::HTCCreditState credits{};credits.initialize(2,1792,2);
    Ath10k::WMIInitState ev{};auto p=channelPacket(52,2412,5);
    assert(!Ath10k::parseWMIInitResponse(p.data(),p.size(),2,credits,ev,nullptr,&scan));
    assert(scan.channelInfoErrors==1 && scan.channels[0].errors==1);
}
