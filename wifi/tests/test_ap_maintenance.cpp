#define ATH10K_AP_MAINTENANCE_TEST
#include "test_connection.cpp"
struct APDriver:ConnectionDriver {
    std::vector<std::vector<UInt8>> wire;bool holdEcho=false,holdCredit=false;
    APDriver(){connect();assert(networkUp && _connection.maintenance.valid);
        auto previous=_regs.tx3Consume;
        _regs.tx3Consume=[this,previous](){
            const auto &m=_connection.maintenance;if(!m.step){previous();return;}
            const auto *f=static_cast<const UInt8 *>(_connectionCommand.virtualAddress());
            wire.emplace_back(f,f+8+Ath10k::htcReadLE16(f+2));
            assert(m.submitted && m.blocked && _connection.wmiOwned);++commands;
            if(m.step==8 && !holdEcho){deliver(2,echoPacket(Ath10k::apMaintenanceCookie(_connection),!holdCredit));return;}
            if(!holdCredit)deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
    }
    void pump(unsigned first=2000,unsigned last=2400){for(unsigned t=first;t<last && _wmiLive.active;t+=10)tick(t);}
    void announce(UInt8 count,UInt8 aifsn=3,bool standard=false){
        auto &s=_connection;std::vector<UInt8> f(36);f[0]=0x80;memset(f.data()+4,255,6);
        memcpy(f.data()+10,s.bssid,6);memcpy(f.data()+16,s.bssid,6);
        Ath10k::htcWriteLE16(f.data()+32,s.ap.beaconInterval);Ath10k::htcWriteLE16(f.data()+34,s.ap.capabilities);
        if(standard)f.insert(f.end(),{12,18,count,0});
        else f.insert(f.end(),{221,24,0,0x50,0xf2,2,1,1,count,0});
        const UInt8 params[]={aifsn,0xa4,0,0,0x27,0xa4,0,0,0x42,0x43,94,0,0x62,0x32,47,0};
        f.insert(f.end(),params,params+16);
        Ath10k::receiveStationManagement(s,f.data(),f.size(),0,s.operatingChannel);
    }
    void enableQoS(bool standard=false){_connection.ap.wmmPresent=true;_connection.ap.wmmVendor=!standard;announce(14,3,standard);pump();wire.clear();}
};
static void wireEDCA(const std::vector<UInt8> &f,UInt32 aifsn){
    assert(f.size()==136 && Ath10k::wmiReadLE32(f.data()+8)==0x4006);
    for(unsigned ac=0;ac<4;++ac){const UInt8 *v=f.data()+28+28*ac;
        assert(Ath10k::htcReadLE16(v-4)==24 && Ath10k::htcReadLE16(v-2)==84);
        assert(Ath10k::wmiReadLE32(v)==(ac>=2?(ac==2?7:3):15));
        assert(Ath10k::wmiReadLE32(v+8)==(ac==0?aifsn:(ac==1?7:2)));
        assert(Ath10k::wmiReadLE32(v+12)==(ac==2?94u*32:(ac==3?47u*32:0)));
    }
}
static void actions(){
    APDriver d;auto &s=d._connection;d.enableQoS();
    s.htEnabled=s.vhtEnabled=true;s.nss=s.negotiatedNSS=2;s.operatingChannel=36;s.centerChannel=42;s.width=s.negotiatedWidth=80;
    s.regulatory.country=Ath10k::regulatoryCountry("US");s.regulatory.indoor=true;s.ap.htOp[1]=5;s.ap.ht[0]=14;
    s.localHT=s.peerHT=0x6e;s.wmmConfigured=true;s.vhtRxMap=s.vhtTxMap=0xfffa;s.htRateCount=16;
    for(UInt8 i=0;i<16;++i)s.htRates[i]=i;
    Ath10k::rememberNegotiatedRates(s);
    s.maintenance.applied=Ath10k::desiredAPConfig(s);s.maintenance.blocked=false;
    UInt8 f[27]{};f[0]=0xd0;memcpy(f+4,s.local,6);memcpy(f+10,s.bssid,6);memcpy(f+16,s.bssid,6);
    f[24]=7;f[25]=1;f[26]=1;Ath10k::receiveStationManagement(s,f,sizeof(f),0,36);
    assert(s.peerSMPS==1 && s.maintenance.blocked);d.pump(2500,2700);assert(s.maintenance.applied.smps==1 && s.nss==1);
    f[26]=2;Ath10k::receiveStationManagement(s,f,sizeof(f),0,36);assert(s.peerSMPS==1);
    f[26]=0;Ath10k::receiveStationManagement(s,f,sizeof(f),0,36);d.pump(2700,2900);assert(s.nss==2);
    f[24]=21;f[25]=2;f[26]=2;Ath10k::receiveStationManagement(s,f,sizeof(f),0,36);
    assert(s.peerOperatingNSS==1 && s.csa.phase==Ath10k::ChannelSwitchPhase::Idle);d.pump(2900,3100);assert(s.nss==1);
    f[26]=0x92;Ath10k::receiveStationManagement(s,f,sizeof(f),0,36);assert(s.peerOperatingNSS==1);
    s.ap.htOp[1]=0;f[26]=0x11;Ath10k::receiveStationManagement(s,f,sizeof(f),0,36);
    assert(s.peerOperatingNSS==2 && s.peerOperatingWidth==40 && s.csa.phase==Ath10k::ChannelSwitchPhase::Idle);
    s.ap.htOp[1]=5;f[26]=0;Ath10k::receiveStationManagement(s,f,sizeof(f),0,36);
    assert(s.csa.phase==Ath10k::ChannelSwitchPhase::Idle && s.width==80 && s.peerOperatingWidth==20 && s.maintenance.blocked);
    s.csa.phase=Ath10k::ChannelSwitchPhase::Idle;s.peerOperatingNSS=2;
    const UInt8 both[]={37,3,1,40,2,62,1,0,194,3,0,0,0,199,1,0};
    assert(Ath10k::channelSwitchIEs(s,both,sizeof(both),false,36));
    assert(s.csa.primary==40 && s.csa.width==20 && s.peerOperatingNSS==1 && s.maintenance.blocked);
    d.releaseWMIRuntime();
}
int main(){
    for(bool standard:{false,true}){
        APDriver d;d.enableQoS(standard);auto &s=d._connection;const UInt32 old=s.maintenance.completed;
        d.announce(15,4,standard);assert(s.maintenance.blocked);d.pump(2500,2800);
        assert(s.maintenance.applied.wmmCount==15 && !s.maintenance.blocked && s.maintenance.completed==old+1);
        assert(d.wire.size()==2);wireEDCA(d.wire[0],4);d.wire.clear();
        d.announce(0,5,standard);d.pump(2800,3100);assert(s.maintenance.applied.wmmCount==0);wireEDCA(d.wire[0],5);
        d.wire.clear();d.announce(0,5,standard);d.pump(3100,3150);assert(d.wire.empty()); // unchanged
        d.announce(0,6,standard);d.pump(3150,3300);assert(s.maintenance.applied.wmm[0]==6); // same counter, changed content
        d.releaseWMIRuntime();
    }
    {APDriver d;d.enableQoS();auto &s=d._connection;d.holdEcho=true;d.announce(15,4);d.pump(2500,2600);
     assert(s.maintenance.step==8 && !s.wmiOwned && s.maintenance.applied.wmmCount==14);
     const UInt32 generation=s.maintenance.generation;d.announce(0,5);assert(s.maintenance.target.wmmCount==15);
     d.holdEcho=false;d.deliver(2,echoPacket(Ath10k::apMaintenanceCookie(s),false));d.tick(2600);
     assert(s.maintenance.blocked && s.maintenance.applied.wmmCount==15);d.pump(2610,2900);
     assert(s.maintenance.generation==generation+1 && s.maintenance.applied.wmmCount==0 && !s.maintenance.blocked);
     d.releaseWMIRuntime();}
    {APDriver d;d.enableQoS();auto &s=d._connection;d.holdCredit=true;d.announce(15,4);d.pump(2500,2570);
     assert(s.wmiOwned && s.maintenance.step==1 && s.maintenance.applied.wmmCount==14);
     d.tick(7600);assert(!d._wmiLive.active && d.resets==1);d.releaseWMIRuntime();}
    {APDriver d;d.enableQoS();auto &s=d._connection;d.announce(15,4);s.eapolPause=true;
     assert(!d.serviceAPMaintenance(2500) && !s.maintenance.step && d.wire.empty());s.eapolPause=false;
     s.dataPending=1;assert(!d.serviceAPMaintenance(2510) && s.maintenance.draining && !s.maintenance.step);
     s.dataPending=0;d.pump(2520,2800);assert(!s.maintenance.blocked);d.releaseWMIRuntime();}
    {APDriver d;d.enableQoS();auto &s=d._connection;
     const UInt8 invalid[]={221,24,0,0x50,0xf2,2,1,1,1,0,3,0xa4,0,0,3,0xa4,0,0,0x42,0x43,0,0,0x62,0x32,0,0};
     const auto before=s.ap;assert(!Ath10k::channelSwitchIEs(s,invalid,sizeof(invalid),false,6));
     assert(!memcmp(before.wmm,s.ap.wmm,16) && before.wmmCount==s.ap.wmmCount);
     d.announce(15,4);s.ap.erpPresent=true;s.ap.erp=6;s.ap.capabilities&=~0x420;
     d.pump(2500,2900);assert(s.maintenance.applied.protection==1 && s.maintenance.applied.slot==1 && s.maintenance.applied.preamble==1);
     bool rts=false;for(auto &f:d.wire)if(Ath10k::wmiReadLE32(f.data()+8)==0x5008 && Ath10k::wmiReadLE32(f.data()+20)==44){assert(Ath10k::wmiReadLE32(f.data()+24)==0x21);rts=true;}assert(rts);
     d.releaseWMIRuntime();}
    actions();puts("AP maintenance passed: WMM/EDCA, counter wrap, snapshots/ECHO/credits, ERP/slot/preamble/RTS, SMPS/NSS and transaction drain.");
}
