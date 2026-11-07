#define ATH10K_CHANNEL_SWITCH_TEST
#include "test_connection.cpp"
using P=Ath10k::ChannelSwitchPhase;
static std::vector<UInt8> beacon(const Ath10k::ConnectionState &s,UInt8 channel,UInt8 count=0,UInt8 mode=1,bool announce=true){
    std::vector<UInt8> f(36,0);f[0]=0x80;memset(f.data()+4,255,6);
    memcpy(f.data()+10,s.bssid,6);memcpy(f.data()+16,s.bssid,6);
    Ath10k::htcWriteLE16(f.data()+32,s.ap.beaconInterval);Ath10k::htcWriteLE16(f.data()+34,s.ap.capabilities);
    if(announce)f.insert(f.end(),{37,3,mode,channel,count});
    else f.insert(f.end(),{3,1,channel});
    return f;
}
static void deliverBeacon(ConnectionDriver &d,UInt8 channel,UInt8 count=0,UInt8 mode=1,bool announce=true,UInt32 rxChannel=0){
    auto f=beacon(d._connection,channel,count,mode,announce);
    Ath10k::receiveStationManagement(d._connection,f.data(),f.size(),0,rxChannel?rxChannel:d._connection.operatingChannel);
}
struct SwitchDriver:ConnectionDriver {
    unsigned switches=0;bool holdReply=false,rejectReply=false,holdCredit=false;
    std::vector<UInt32> wire;
    SwitchDriver(){connect();assert(networkUp);
        auto previous=_regs.tx3Consume;
        _regs.tx3Consume=[this,previous](){
            auto &c=_connection.csa;
            if(c.phase<P::Down || c.phase>P::Barrier){previous();return;}
            ++switches;++commands;
            assert(c.blocked && c.submitted && _connection.wmiOwned);
            auto *f=static_cast<const UInt8 *>(_connectionCommand.virtualAddress());
            UInt32 cmd=Ath10k::wmiReadLE32(f+8);wire.push_back(cmd);
            if(c.phase==P::Restart){
                assert(cmd==0x5004 && Ath10k::wmiReadLE32(f+92)==2462 && Ath10k::wmiReadLE32(f+96)==2462);
                assert(_connection.operatingChannel==6); // Build must not mutate live channel.
                if(!holdReply){std::vector<UInt8> p(32);p[0]=2;p[2]=24;p[8]=1;p[9]=0x50;p[12]=16;p[14]=40;
                    Ath10k::htcWriteLE32(p.data()+24,1);Ath10k::htcWriteLE32(p.data()+28,rejectReply?1:0);
                    if(!holdCredit)addPkgCredit(p);
                    deliver(2,p);return;}
            }else if(c.phase==P::Barrier){
                assert(cmd==0x1d001);if(!holdReply){deliver(2,echoPacket(Ath10k::channelSwitchCookie(_connection),!holdCredit));return;}
            }
            if(!holdCredit)deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
    }
};
static void parserTests(){
    ConnectionDriver d;d.connect();auto &s=d._connection;
    auto valid=beacon(s,11,3,1);
    for(unsigned n=0;n<valid.size();++n){Ath10k::receiveStationManagement(s,valid.data(),n,0,6);assert(s.csa.phase==P::Idle);}
    auto f=valid;f[10]^=2;Ath10k::receiveStationManagement(s,f.data(),f.size(),0,6);assert(s.csa.phase==P::Idle);
    f=valid;f.insert(f.end(),{37,3,1,1,3});Ath10k::receiveStationManagement(s,f.data(),f.size(),0,6);assert(s.csa.phase==P::Idle);
    for(unsigned mode:{2u,255u}){f=valid;f[38]=mode;Ath10k::receiveStationManagement(s,f.data(),f.size(),0,6);assert(s.csa.phase==P::Idle);}
    f=valid;f.insert(f.end(),{196,4,194,3,1,42});Ath10k::receiveStationManagement(s,f.data(),f.size(),0,6);assert(s.csa.phase==P::Idle);
    deliverBeacon(d,11,3);assert(s.csa.phase==P::Announced && s.csa.blocked);
    d.releaseWMIRuntime();
    for(UInt8 channel:{UInt8(52),UInt8(36),UInt8(165),UInt8(0),UInt8(255)}){
        ConnectionDriver bad;bad.connect();deliverBeacon(bad,channel);assert(bad._connection.cancelRequested && bad._connection.csa.blocked && bad._connection.csa.rejected==1);bad.releaseWMIRuntime();
    }
    // Wide bandwidth change requires peer bandwidth update, so fail closed.
    ConnectionDriver width;width.connect();f=beacon(width._connection,11);f.insert(f.end(),{62,1,3});
    Ath10k::receiveStationManagement(width._connection,f.data(),f.size(),0,6);assert(width._connection.cancelRequested);width.releaseWMIRuntime();
    ConnectionDriver ecsa;ecsa.connect();f=beacon(ecsa._connection,11);f.resize(36);f.insert(f.end(),{60,4,1,81,11,1});
    Ath10k::receiveStationManagement(ecsa._connection,f.data(),f.size(),0,6);assert(ecsa._connection.csa.primary==11);ecsa.releaseWMIRuntime();
}
static void widthChanges(){
    ConnectionDriver d;d.connect();auto &s=d._connection;
    s.operatingChannel=36;s.centerChannel=42;s.width=s.negotiatedWidth=80;s.htEnabled=s.vhtEnabled=true;s.nss=2;
    s.wmmConfigured=true;
    s.localHT=s.peerHT=0x6e;s.ap.ht[0]=14;s.localVHT=s.peerVHT=1u<<5;s.vhtRxMap=s.vhtTxMap=0xfffa;
    s.regulatory.country=Ath10k::regulatoryCountry("US");s.regulatory.indoor=true;s.ap.vhtPresent=s.ap.vhtOperation=true;
    Ath10k::htcWriteLE16(s.ap.vht+4,0xfffa);Ath10k::htcWriteLE16(s.ap.vht+8,0xfffa);
    s.htRateCount=16;for(UInt8 i=0;i<16;++i)s.htRates[i]=i;
    Ath10k::rememberNegotiatedRates(s);
    s.pairPN[0]=99;s.groupPN[3]=123;s.installedPair[0]=42;s.pairInstalled=true;
    const UInt8 oldRates[]={12,24,48,108};memcpy(s.ap.rates,oldRates,4);
    std::vector<UInt32> commands;
    d._regs.tx3Consume=[&](){auto *f=static_cast<const UInt8 *>(d._connectionCommand.virtualAddress());
        ++d.commands;commands.push_back(Ath10k::wmiReadLE32(f+8));
        if(s.csa.phase==P::Restart){assert(Ath10k::wmiReadLE32(f+104)==(8u|(1u<<11)|(1u<<12)));
            std::vector<UInt8> p(32);p[0]=2;p[2]=24;p[8]=1;p[9]=0x50;p[12]=16;p[14]=40;Ath10k::htcWriteLE32(p.data()+24,1);addPkgCredit(p);d.deliver(2,p);return;}
        if(s.csa.phase==P::PeerPhy)assert(Ath10k::wmiReadLE32(f+28)==0xd && Ath10k::wmiReadLE32(f+32)==8);
        if(s.csa.phase==P::PeerWidth)assert(Ath10k::wmiReadLE32(f+28)==4 && !Ath10k::wmiReadLE32(f+32));
        if(s.csa.phase==P::PeerRates)assert(!Ath10k::wmiReadLE32(f+28) && s.vhtRxMap==0xfff5);
        if(s.csa.phase==P::Barrier){d.deliver(2,echoPacket(Ath10k::channelSwitchCookie(s)));return;}
        d.deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
    };
    auto f=beacon(s,36);f.insert(f.end(),{62,1,0});Ath10k::receiveStationManagement(s,f.data(),f.size(),0,36);
    assert(s.csa.phase==P::Announced && s.csa.width==20 && !s.cancelRequested);
    for(unsigned t=2000;t<2350;t+=10)d.tick(t);
    assert(commands==std::vector<UInt32>({0x5007,0x5004,0x6004,0x6004,0x6005,0x5005,0x1d001}));
    deliverBeacon(d,36,0,0,false,36);d.tick(2350);assert(s.csa.phase==P::Confirm);
    f=beacon(s,36,0,0,false);f.insert(f.end(),{61,22,36,0});f.resize(f.size()+20,0);
    Ath10k::receiveStationManagement(s,f.data(),f.size(),0,36);d.tick(2360);
    assert(s.width==20 && s.csa.phase==P::Idle && !s.csa.blocked && s.pairPN[0]==99 && s.groupPN[3]==123 && s.installedPair[0]==42 && s.pairInstalled);
    // A later HT/VHT beacon restores the original negotiated width without CSA.
    f=beacon(s,36,0,0,false);f.insert(f.end(),{61,22,36,5});f.resize(f.size()+20,0);f.insert(f.end(),{192,5,1,42,0,255,255});
    Ath10k::receiveStationManagement(s,f.data(),f.size(),0,36);assert(s.csa.phase==P::Announced && s.csa.width==80);
    d.releaseWMIRuntime();
}
int main(){
    widthChanges();
    parserTests();
    {SwitchDriver d;auto &s=d._connection;s.secure=true;s.pairPN[3]=123;s.groupPN[5]=456;s.installedPair[0]=0x73;s.pairInstalled=true;
     deliverBeacon(d,11);for(unsigned t=2000;t<2250;t+=10)d.tick(t);
     assert(d.wire==std::vector<UInt32>({0x5007,0x5004,0x5005,0x1d001}));
     assert(s.csa.phase==P::Confirm && s.csa.blocked && s.operatingChannel==11 && d.networkUp && s.portAuthorized);
     deliverBeacon(d,11,0,0,false,6);d.tick(2250);assert(s.csa.phase==P::Confirm); // Wrong RX channel cannot confirm.
     deliverBeacon(d,11,0,0,false,11);d.tick(2260);
     assert(s.csa.phase==P::Idle && !s.csa.blocked && s.csa.completed==1 && s.pairPN[3]==123 && s.groupPN[5]==456 && s.installedPair[0]==0x73 && s.pairInstalled);
     d.releaseWMIRuntime();}
    {SwitchDriver d;deliverBeacon(d,11,2,0);d.tick(2000);assert(d._connection.csa.dueMs==2204 && !d._connection.csa.blocked && !d.switches);
     deliverBeacon(d,11,3,0);d.tick(2050);assert(d._connection.csa.dueMs==2204);
     deliverBeacon(d,11,1,0);d.tick(2060);assert(d._connection.csa.dueMs==2162);
     d.tick(2161);assert(!d.switches);d.tick(2162);assert(d.switches==1);d.releaseWMIRuntime();}
    {SwitchDriver d;deliverBeacon(d,11);d._wmi.credits.outstanding=1;
     assert(!d.serviceChannelSwitch(2000) && d._connection.csa.phase==P::Drain && !d.switches);
     d._wmi.credits.outstanding=0;assert(!d.serviceChannelSwitch(2010) && d.switches==1);d.releaseWMIRuntime();}
    {SwitchDriver d;d.holdReply=true;deliverBeacon(d,11);for(unsigned t=2000;t<2100;t+=10)d.tick(t);
     assert(d._connection.csa.phase==P::Restart && !d._connection.csa.restartReply);
     d.tick(7200);assert(d._connection.cancelRequested && d._connection.csa.blocked && d._connection.operatingChannel==6);d.releaseWMIRuntime();}
    {SwitchDriver d;d.rejectReply=true;deliverBeacon(d,11);for(unsigned t=2000;t<2080;t+=10)d.tick(t);
     assert(d._connection.operatingChannel==6 && !d.networkUp && d._connection.csa.blocked);d.releaseWMIRuntime();}
    {SwitchDriver d;d.holdCredit=true;deliverBeacon(d,11);for(unsigned t=2000;t<2070;t+=10)d.tick(t);
     assert(d.switches==1 && d._connection.wmiOwned);d.tick(7050);assert(!d._wmiLive.active && d.resets==1);d.releaseWMIRuntime();}
    puts("CSA passed: AP identity/IE validation, deadline, DMA/credit drain, DOWN/RESTART/UP, response/barrier/beacon gates, preserved WPA2 keys/PN, forbidden moves and failures.");
}
