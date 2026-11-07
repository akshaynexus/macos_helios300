#define ATH10K_AP_MAINTENANCE_TEST
#include "test_connection.cpp"

static std::vector<UInt8> statsPacket(const UInt8 *bssid,UInt32 headerBytes=40){
    constexpr UInt32 records=180+164+20;
    std::vector<UInt8> f(20+headerBytes+records);f[0]=2;
    Ath10k::htcWriteLE16(f.data()+2,f.size()-8);Ath10k::htcWriteLE32(f.data()+8,0x1d004);
    Ath10k::connTLV(f.data()+12,70,headerBytes);UInt8 *h=f.data()+16;
    Ath10k::htcWriteLE32(h,13);for(unsigned i=1;i<=3;++i)Ath10k::htcWriteLE32(h+4*i,1);
    Ath10k::connTLV(h+headerBytes,17,records);UInt8 *d=h+headerBytes+4;
    Ath10k::htcWriteLE32(d,static_cast<UInt32>(-101));
    UInt8 *v=d+180;for(unsigned i=0;i<4;++i){Ath10k::htcWriteLE32(v+12+4*i,100+i);
        Ath10k::htcWriteLE32(v+32+4*i,10+i);Ath10k::htcWriteLE32(v+48+4*i,1+i);}
    Ath10k::htcWriteLE32(v+80,9);
    UInt8 *p=v+164;memcpy(p,bssid,6);Ath10k::htcWriteLE32(p+12,433300);Ath10k::htcWriteLE32(p+16,390000);
    return f;
}

struct StatsDriver:ConnectionDriver {
    std::vector<std::vector<UInt8>> wire;bool holdCredit=false,holdEcho=false,emitStats=true;
    StatsDriver(){connect();auto previous=_regs.tx3Consume;
        _regs.tx3Consume=[this,previous](){
            auto &s=_connection;if(!s.stats.step){previous();return;}
            const auto *f=static_cast<const UInt8 *>(_connectionCommand.virtualAddress());
            wire.emplace_back(f,f+8+Ath10k::htcReadLE16(f+2));assert(s.stats.submitted && s.wmiOwned);++commands;
            if(s.stats.step==1 && emitStats)deliver(2,statsPacket(s.bssid));
            if(s.stats.step==2 && !holdEcho){deliver(2,echoPacket(Ath10k::statsCookie(s),!holdCredit));return;}
            if(!holdCredit)deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
    }
    void pump(UInt64 first=12000,UInt64 end=12300){for(UInt64 t=first;t<end && _wmiLive.active;t+=10)tick(t);}
};

static void parserTests(){
    Ath10k::ConnectionState s;s.stage=LinkStage::Connected;s.portAuthorized=true;s.bssid[0]=2;
    Ath10k::HTCCreditState credits;credits.initialize(2,256,2);assert(credits.reserve(32));
    Ath10k::WMIInitState events;Ath10k::WMIScanState scan;scan.completed=true;
    auto parse=[&](const std::vector<UInt8> &f){return Ath10k::parseWMIInitResponse(f.data(),f.size(),2,credits,events,nullptr,&scan,nullptr,false,&s);};
    auto f=statsPacket(s.bssid);assert(!parse(f));
    assert(s.fwStats.noiseValid && s.fwStats.rateValid && s.fwStats.vdevValid && s.fwStats.noiseDbm==-101);
    assert(s.fwStats.txKbps==433300 && s.fwStats.rxKbps==390000 && s.fwStats.tx[3]==103 && s.fwStats.notAcked==9);
    assert(credits.outstanding==1 && !s.securityAck && events.statsDecoded==1);
    const auto before=s.fwStats;
    for(unsigned n=0;n<f.size();++n){auto shortFrame=std::vector<UInt8>(f.begin(),f.begin()+n);
        assert(parse(shortFrame));assert(s.fwStats.serial==before.serial);}
    f=statsPacket(s.bssid);f[1]=2;f[4]=8;f.insert(f.end(),{1,4,0,0,2,2,0,0});Ath10k::htcWriteLE16(f.data()+2,f.size()-8);
    assert(parse(f) && s.fwStats.serial==before.serial && credits.outstanding==1);
    f=statsPacket(s.bssid,52);assert(!parse(f) && events.statsUnsupported==1 && s.fwStats.serial==before.serial);
    f=statsPacket(s.bssid);Ath10k::htcWriteLE32(f.data()+20,0xffffffffu);
    assert(!parse(f) && events.statsUnsupported==2 && s.fwStats.serial==before.serial);
    // Unrelated peers and noise-only records cannot refresh the last rate sample.
    f=statsPacket(s.bssid);f[f.size()-20]=4;assert(!parse(f));
    assert(s.fwStats.noiseSerial==before.noiseSerial+1 && s.fwStats.rateSerial==before.rateSerial);
    f=statsPacket(s.bssid);scan.completed=false;assert(!parse(f) && s.fwStats.rateSerial==before.rateSerial);
    scan.completed=true;s.csa.phase=Ath10k::ChannelSwitchPhase::Drain;assert(!parse(f) && s.fwStats.rateSerial==before.rateSerial);
}

int main(){
    parserTests();
    {StatsDriver d;auto &s=d._connection;assert(s.stats.nextMs && !s.stats.step);d.pump();
     assert(d.wire.size()==2 && s.stats.completed==1 && !s.wmiOwned && !s.stats.step && d.networkUp);
     const auto &f=d.wire[0];assert(f.size()==36 && Ath10k::wmiReadLE32(f.data()+8)==0x16001);
     assert(Ath10k::htcReadLE16(f.data()+14)==143 && Ath10k::wmiReadLE32(f.data()+16)==13);
     assert(Ath10k::sameMAC(f.data()+24,s.bssid));
     assert(s.fwStats.noiseObservedMs && s.fwStats.rateObservedMs);
     d.pump(12400,12500);assert(d.wire.size()==2);d.releaseWMIRuntime();}
    {StatsDriver d;d.emitStats=false;d.pump();assert(d._connection.stats.completed==1 && !d._connection.fwStats.serial && d.networkUp);d.releaseWMIRuntime();}
    {StatsDriver d;d.holdEcho=true;d.pump();auto &s=d._connection;
     assert(s.stats.step==2 && !s.wmiOwned && !s.stats.completed && d.networkUp);
     s.cancelRequested=true;d.tick(12310);assert(s.stats.step==2 && s.disconnect==Ath10k::DisconnectStage::None);
     d.holdEcho=false;d.deliver(2,echoPacket(Ath10k::statsCookie(s),false));d.tick(12320);
     assert(!s.stats.step);d.releaseWMIRuntime();}
    {StatsDriver d;d.holdCredit=true;d.pump();auto &s=d._connection;
     assert(s.stats.step==1 && s.wmiOwned && !s.stats.completed);d.tick(17100);assert(!d._wmiLive.active && d.resets==1);d.releaseWMIRuntime();}
    {StatsDriver d;auto &s=d._connection;s.maintenance.blocked=true;
     assert(!d.serviceStatsMaintenance(12000) && !s.stats.step && d.wire.empty());d.releaseWMIRuntime();}
    {StatsDriver d;auto &s=d._connection;s.dataPending=1;
     assert(!d.serviceStatsMaintenance(12000) && s.stats.step==1 && s.wmiOwned); // HTT data does not serialize an informational WMI query.
     s.dataPending=0;d.pump(12010,12300);assert(s.stats.completed==1 && d.networkUp);d.releaseWMIRuntime();}
    puts("Firmware stats passed: exact Linux records, atomic credits, per-field freshness, request/ECHO ownership and unavailable statistics without false acknowledgement.");
}
