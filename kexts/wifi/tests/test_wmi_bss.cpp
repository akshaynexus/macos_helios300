#define ATH10K_WMI_BSS_TEST
#include "test_wmi_scan.cpp"
static std::vector<UInt8> beacon(std::vector<UInt8> ssid={'T','e','s','t'},UInt8 id=1) {
    std::vector<UInt8> f(36);f[0]=0x80;f[16]=2;f[21]=id;f[34]=0x10;
    f.push_back(0);f.push_back(ssid.size());f.insert(f.end(),ssid.begin(),ssid.end());
    f.insert(f.end(),{3,1,1,48,2,1,0,221,4,0,0x50,0xf2,1});return f;
}
static std::vector<UInt8> wrapMgmt(const std::vector<UInt8> &f,UInt32 status=0,bool credit=false) {
    std::vector<UInt8> p(60+f.size());p[0]=2;Ath10k::htcWriteLE16(p.data()+2,p.size()-8);
    Ath10k::htcWriteLE32(p.data()+8,0x7001);p[12]=40;p[14]=44;
    Ath10k::htcWriteLE32(p.data()+16,1);Ath10k::htcWriteLE32(p.data()+20,42);
    Ath10k::htcWriteLE32(p.data()+32,f.size());Ath10k::htcWriteLE32(p.data()+36,status);
    Ath10k::htcWriteLE16(p.data()+56,f.size());p[58]=17;
    std::copy(f.begin(),f.end(),p.begin()+60);if(credit)addPkgCredit(p);return p;
}
int main(){
    ScanDriver d;d.launch();d.deliver(2,scanPacket(1,7));d.tick(1040);
    d.deliver(2,wrapMgmt(beacon()));d.tick(1050);
    assert(d._wmiBSS.count==1 && d._wmiBSS.accepted==1 && d._wmiBSS.beacons==1);
    const auto &e=d._wmiBSS.entries[0];
    assert(e.ssidLength==4 && !memcmp(e.ssid,"Test",4) && e.bssid[5]==1);
    assert(e.snr==42 && e.rxChannel==1 && e.advertisedChannel==1 && e.privacy && e.rsnPresent && e.wpaPresent);
    d.deliver(2,wrapMgmt(beacon()));d.tick(1060);assert(d._wmiBSS.count==1 && e.seen==2);
    d.deliver(2,scanPacket(2));d.tick(1600);assert(d._wmiScan.completed && d._wmiLive.active);
    d.releaseWMIRuntime();
    Ath10k::WMIBSSCache c;
    auto hidden=beacon({});Ath10k::recordWMIBSS(c,hidden.data(),hidden.size(),1,20,0);
    assert(c.count==1 && c.entries[0].hidden);
    auto probe=beacon({'N','a','m','e'});probe[0]=0x50;
    Ath10k::recordWMIBSS(c,probe.data(),probe.size(),1,30,0);
    assert(!c.entries[0].hidden && c.entries[0].ssidLength==4 && c.probeResponses==1);
    Ath10k::recordWMIBSS(c,hidden.data(),hidden.size(),1,31,0);
    assert(!c.entries[0].hidden && !memcmp(c.entries[0].ssid,"Name",4));
    for(unsigned n=2;n<=65;++n){auto f=beacon({'X'},n);Ath10k::recordWMIBSS(c,f.data(),f.size(),1,1,0);}
    assert(c.count==64 && c.overflow==1);
    for(unsigned size=0;size<38;++size){
        auto f=beacon();Ath10k::WMIBSSCache bad;Ath10k::recordWMIBSS(bad,f.data(),size,1,1,0);
        assert(!bad.count && bad.malformed==1);
        UInt32 total=0;for(auto count:bad.rejects)total+=count;assert(total==bad.malformed);
    }
    for(unsigned mode=0;mode<7;++mode){
        auto f=beacon();
        if(mode==0)f[37]=33; // oversized SSID
        if(mode==1)f.insert(f.end(),{0,1,'X'}); // duplicate SSID
        if(mode==2)f.push_back(3); // truncated IE header
        if(mode==3)f[16]=1; // multicast BSSID
        if(mode==4)f[1]=0x40; // protected management body
        if(mode==5)f[22]=1; // fragment
        if(mode==6)f[36]=99; // no SSID
        Ath10k::WMIBSSCache bad;Ath10k::recordWMIBSS(bad,f.data(),f.size(),1,1,0);
        assert(!bad.count && bad.malformed==1);
        UInt32 total=0;for(auto count:bad.rejects)total+=count;assert(total==bad.malformed);
    }
    auto f=beacon();Ath10k::WMIBSSCache dropped;
    for(unsigned status:{1u,8u,16u,32u})Ath10k::recordWMIBSS(dropped,f.data(),f.size(),1,1,status);
    assert(dropped.rxErrors==4 && !dropped.count);
    Ath10k::recordWMIBSS(dropped,f.data(),f.size(),14,1,0);assert(dropped.ignored==1);
    f[0]=0x40;Ath10k::recordWMIBSS(dropped,f.data(),f.size(),1,1,0);assert(dropped.ignored==2);
    for(unsigned channel=1;channel<=13;++channel){
        Ath10k::WMIBSSCache perChannel;auto frame=beacon();
        Ath10k::recordWMIBSS(perChannel,frame.data(),frame.size(),channel,42,0);
        assert(perChannel.count==1 && perChannel.entries[0].rxChannel==channel);
    }
    // 5 GHz beacons with HT Operation and no DS Parameter Set.
    for(UInt8 channel:{36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165}){
        auto frame=beacon({'F','i','v','e'},channel);
        frame.erase(frame.begin()+42,frame.begin()+45);
        frame.push_back(61);frame.push_back(22);frame.push_back(channel);
        frame.insert(frame.end(),21,0);
        ScanDriver five;five.launch();auto packet=wrapMgmt(frame);
        Ath10k::htcWriteLE32(packet.data()+16,channel);
        five.deliver(2,packet);five.tick(1040);
        assert(five._wmiLive.active && five._wmiBSS.count==1);
        assert(five._wmiBSS.entries[0].rxChannel==channel && five._wmiBSS.entries[0].advertisedChannel==channel);
        five.releaseWMIRuntime();
        Ath10k::WMIBSSCache bad;
        frame.push_back(61);frame.push_back(1);frame.push_back(channel);
        Ath10k::recordWMIBSS(bad,frame.data(),frame.size(),channel,42,0);
        assert(!bad.count && bad.malformed==1);
        UInt32 total=0;for(auto count:bad.rejects)total+=count;assert(total==bad.malformed);
    }
    for(UInt32 channel:{14u,35u,37u,169u}){
        Ath10k::WMIBSSCache outside;auto frame=beacon();
        Ath10k::recordWMIBSS(outside,frame.data(),frame.size(),channel,42,0);
        assert(!outside.count && outside.ignored==1);
    }
    auto existing=beacon({'U','p','d','a','t','e','d'});
    Ath10k::recordWMIBSS(c,existing.data(),existing.size(),1,42,0);
    assert(c.count==64 && c.overflow==1 && c.entries[0].ssidLength==7);
    // Binary SSIDs and max length have deterministic, bounded display escaping.
    Ath10k::WMIBSSCache binary;auto b=beacon({'A',0,10,34,92,0xff});
    Ath10k::recordWMIBSS(binary,b.data(),b.size(),1,1,0);char display[129]{};
    Ath10k::formatWMISSID(binary.entries[0],display);
    assert(!strcmp(display,"A\\x00\\x0a\\x22\\x5c\\xff"));
    auto max=beacon(std::vector<UInt8>(32,0xff),2);
    Ath10k::recordWMIBSS(binary,max.data(),max.size(),1,1,0);
    Ath10k::formatWMISSID(binary.entries[1],display);assert(strlen(display)==128);
    // A malformed RF frame still returns valid transport credits, but no network.
    Ath10k::HTCCreditState credits;credits.initialize(2,1792,2);credits.reserve(184);
    Ath10k::WMIInitState events;Ath10k::WMIScanState scan;scan.submitted=true;
    Ath10k::WMIBSSCache cache;auto shortFrame=wrapMgmt({0x80,0},0,true);
    assert(!Ath10k::parseWMIInitResponse(shortFrame.data(),shortFrame.size(),2,credits,events,nullptr,&scan,&cache));
    assert(cache.malformed==1 && !cache.count && credits.outstanding==0);
    auto packet=wrapMgmt(beacon(),0,true);credits.reserve(184);packet.back()=0;packet[packet.size()-3]=3;
    assert(Ath10k::parseWMIInitResponse(packet.data(),packet.size(),2,credits,events,nullptr,&scan,&cache));
    assert(cache.count==0 && credits.outstanding==1);
    packet[packet.size()-3]=1;
    assert(!Ath10k::parseWMIInitResponse(packet.data(),packet.size(),2,credits,events,nullptr,&scan,&cache));
    assert(cache.count==1 && credits.outstanding==0);
    // Older 24-byte test fixture is a short beacon; receiver remains alive.
    ScanDriver shortBeacon;shortBeacon.launch();shortBeacon.deliver(2,mgmtPacket());shortBeacon.tick(1040);
    assert(shortBeacon._wmiLive.active && !shortBeacon._wmiBSS.count);shortBeacon.releaseWMIRuntime();
}
