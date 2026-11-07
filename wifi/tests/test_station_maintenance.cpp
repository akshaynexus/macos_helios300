#define ATH10K_CHANNEL_SWITCH_TEST
#include "test_connection.cpp"
class MaintenanceDriver:public ConnectionDriver {
public:
    std::vector<UInt32> wire,parameters,values;bool holdEcho=false,holdCredit=false,respondProbe=false;unsigned probes=0;
    MaintenanceDriver(){connect();_connection.ap.timPresent=true;_connection.ap.dtimPeriod=1;_connection.radio.appliedDTIM=1;
        auto oldTX=_regs.tx4Consume;
        _regs.tx4Consume=[this,oldTX](){
            if(!_connection.lastTxProbe){oldTX();return;}
            ++probes;auto *f=static_cast<const UInt8 *>(_packetBuffer.virtualAddress());
            assert(Ath10k::htcReadLE16(f)==0x40 && Ath10k::sameMAC(f+4,_connection.bssid));
            assert(f[24]==0 && f[25]==_connection.ssidLength);
            assert(Ath10k::wmiReadLE32(static_cast<const UInt8 *>(_packetCommand.virtualAddress())+16)==_packetBuffer.dmaAddress());
            deliver(1,mgmtCompletion(_connection.txId,respondProbe?0:2));
            if(respondProbe){UInt8 response[36]{};response[0]=0x50;
                for(unsigned i=0;i<6;++i){response[4+i]=_connection.local[i];response[10+i]=response[16+i]=_connection.bssid[i];}
                Ath10k::receiveStationManagement(_connection,response,sizeof(response),0,_connection.operatingChannel);}
        };
        auto previous=_regs.tx3Consume;
        _regs.tx3Consume=[this,previous](){
            auto &r=_connection.radio;if(!r.step){previous();return;}
            ++commands;assert(r.submitted && r.blocked && _connection.wmiOwned);
            auto *f=static_cast<const UInt8 *>(_connectionCommand.virtualAddress());
            wire.push_back(Ath10k::wmiReadLE32(f+8));parameters.push_back(Ath10k::wmiReadLE32(f+20));values.push_back(Ath10k::wmiReadLE32(f+24));
            if(r.step==8){if(!holdEcho)deliver(2,echoPacket(Ath10k::radioCookie(_connection),!holdCredit));}
            else if(!holdCredit)deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
    }
};
static void modelTests(){
    Ath10k::ConnectionState s;s.stage=LinkStage::Connected;s.portAuthorized=true;
    assert(!Ath10k::stationWatchdog(s,1000,false));
    assert(!Ath10k::stationWatchdog(s,30999,false));assert(Ath10k::stationWatchdog(s,31000,false) && s.beaconMisses==1 && s.probe.active && !s.cancelRequested);
    s.cancelRequested=false;s.beaconSerial++;assert(!Ath10k::stationWatchdog(s,31001,false));
    s.beaconWarning=true;assert(!Ath10k::stationWatchdog(s,32000,false));
    assert(!Ath10k::stationWatchdog(s,36999,false));assert(Ath10k::stationWatchdog(s,37000,false));
    s.cancelRequested=false;s.beaconWarning=false;assert(!Ath10k::stationWatchdog(s,100000,true));
    s.rxPackets++;assert(!Ath10k::stationWatchdog(s,100001,false));
    s.operatingChannel=s.centerChannel=6;s.regulatory.country=Ath10k::regulatoryCountry("US");
    s.powerConstraintDb=25;assert(Ath10k::stationPowerLimit(s)==10);s.powerConstraintDb=30;assert(!Ath10k::stationPowerLimit(s));
    Ath10k::MulticastFilter filter;const UInt8 multicast[]={1,0,0x5e,0,0,1},other[]={0x33,0x33,0,0,0,1},broadcast[]={255,255,255,255,255,255},unicast[]={2,1,2,3,4,5};
    assert(filter.configure(multicast,1));assert(filter.accepts(multicast) && !filter.accepts(other) && filter.accepts(broadcast) && filter.accepts(unicast));
    assert(!filter.configure(nullptr,1) && !filter.configure(unicast,1));filter.enabled=false;assert(!filter.accepts(multicast) && filter.accepts(broadcast));
    UInt8 many[65*6]{};for(unsigned i=0;i<65;++i)many[i*6]=1;assert(filter.configure(many,65) && filter.all);filter.enabled=true;assert(filter.accepts(other));
    s.multicast=filter;Ath10k::resetDisconnected(s);assert(s.multicast.all && s.beaconMisses==2);
    Ath10k::RegulatoryPolicy p;p.country=Ath10k::regulatoryCountry("US");p.indoor=true;p.stationDFS=true;
    assert(Ath10k::channelDefinitionAllowed(52,58,80,true) && Ath10k::regulatoryPower(p,52,58,80));
    assert(!Ath10k::channelDefinitionAllowed(52,58,80) && !Ath10k::channelDefinitionAllowed(52,62,80,true));
    assert(!Ath10k::countryPower(p.country,58,80,true,true,true,true)); // DFS never permits initiating a probe scan.
    Ath10k::resetDisconnected(s);s.regulatory=p;s.ssidLength=1;s.ssid[0]='X';s.bssid[0]=2;
    Ath10k::WMIBSSCache cache;cache.count=1;auto &ap=cache.entries[0];ap.hidden=false;ap.ssidLength=1;ap.ssid[0]='X';ap.bssid[0]=2;
    ap.rxChannel=ap.advertisedChannel=52;ap.capabilities=0x101;ap.beaconInterval=100;ap.rates[0]=0x8c;ap.rateCount=1;
    assert(!Ath10k::selectOpenBSS(s,cache) && s.operatingChannel==52);s.stage=LinkStage::Assoc;UInt8 frame[256]{};
    const UInt32 n=Ath10k::buildStationManagement(frame,sizeof(frame),s);assert(n && (Ath10k::htcReadLE16(frame+24)&0x100));
    bool powerIE=false,channelsIE=false;for(UInt32 i=28;i<n;i+=2+frame[i+1]){assert(i+2u+frame[i+1]<=n);powerIE|=frame[i]==33;channelsIE|=frame[i]==36;}
    assert(powerIE && channelsIE);s.regulatory.stationDFS=false;assert(Ath10k::selectOpenBSS(s,cache));
}
int main(){
    modelTests();
    {MaintenanceDriver d;auto &s=d._connection;
     std::vector<UInt8> event(28);event[0]=2;event[2]=20;event[8]=1;event[9]=0xb0;event[12]=12;event[14]=48;event[20]=2;
     d.deliver(2,event);d.tick(2000);assert(s.beaconWarning && !s.cancelRequested && d._wmiLive.active);
     ++s.beaconSerial;d.tick(2010);assert(!s.beaconWarning);
     auto credits=d._wmi.credits;event[16]=1;
     assert(Ath10k::parseWMIInitResponse(event.data(),event.size(),2,credits,d._wmiInit,nullptr,nullptr,nullptr,false,&s) && !s.beaconWarning);
     d.releaseWMIRuntime();}
    {MaintenanceDriver d;UInt8 f[Ath10k::kWMIInitFrameBytes]{};d._wmi.rfChains=2;
     assert(!Ath10k::buildWMIInit(f,sizeof(f),d._wmi,2,1));assert(Ath10k::wmiReadLE32(f+76)==1 && Ath10k::wmiReadLE32(f+80)==1);
     assert(!Ath10k::buildWMIInit(f,sizeof(f),d._wmi,2,2));assert(Ath10k::wmiReadLE32(f+76)==3);
     d.releaseWMIRuntime();}
    {MaintenanceDriver d;auto &s=d._connection;s.radio.requestedPS=true;
     for(unsigned t=2000;t<2400;t+=10){d.tick(t);}
     assert(s.radio.appliedPS && s.radio.configuredPS && !s.radio.step && !s.radio.blocked && s.radio.completed==1);
     assert(d.wire==std::vector<UInt32>({0x9002,0x9002,0x9002,0x9002,0x9002,0x9001,0x4003,0x1d001}));
     assert(d.parameters[0]==0 && d.values[0]==0 && d.parameters[1]==1 && d.values[1]==1 && d.values[3]==102 && d.parameters[5]==1);
     s.eapolPause=true;for(unsigned t=2400;t<2600;t+=10){d.tick(t);}assert(!s.radio.appliedPS && s.radio.completed==2 && d.wire.size()==11);
     d.releaseWMIRuntime();}
    {MaintenanceDriver d;d.holdEcho=true;d._connection.radio.requestedPS=true;
     for(unsigned t=2000;t<2300;t+=10){d.tick(t);}
     assert(d._connection.radio.step==8 && !d._connection.radio.appliedPS && d._connection.radio.blocked);
     d.tick(8000);assert(!d._wmiLive.active && d.resets==1);d.releaseWMIRuntime();}
    {MaintenanceDriver d;auto &s=d._connection;s.radio.requestedPS=true;
     for(unsigned t=2000;t<2400;t+=10){d.tick(t);}
     assert(s.radio.appliedPS && Ath10k::requestDisconnect(s,2400));
     for(unsigned t=2400;t<2900;t+=10){d.tick(t);}
     assert(s.stage==LinkStage::Disabled && !s.radio.appliedPS && s.radio.requestedPS && d._wmiLive.active);
     assert(d.wire[8]==0x9001 && !d.parameters[8]);d.releaseWMIRuntime();}
    {MaintenanceDriver d;d.holdCredit=true;d._connection.radio.requestedPS=true;
     for(unsigned t=2000;t<2200;t+=10){d.tick(t);}
     assert(d._connection.wmiOwned && d._connection.radio.step==1);
     d.tick(8000);assert(!d._wmiLive.active && d.resets==1);d.releaseWMIRuntime();}
    {MaintenanceDriver d;d._connection.radio.requestedPS=true;d._connection.dataPending=1;
     assert(!d.serviceRadioMaintenance(2000) && d._connection.radio.blocked && !d._connection.radio.step && d.wire.empty());
     d._connection.dataPending=0;assert(!d.serviceRadioMaintenance(2010) && d._connection.radio.step==1);d.releaseWMIRuntime();}
    {MaintenanceDriver d;auto &s=d._connection;s.radio.requestedPS=true;s.ap.timPresent=false;
     for(unsigned t=2000;t<2200;t+=10){d.tick(t);}assert(!s.radio.appliedPS && d.wire.empty());
     UInt8 tim[]={5,4,0,3,0,0};assert(Ath10k::channelSwitchIEs(s,tim,sizeof(tim),false,6));
     s.ap.beaconInterval=200;for(unsigned t=2200;t<2700;t+=10){d.tick(t);}
     assert(s.radio.appliedPS && s.radio.appliedDTIM==3 && s.radio.appliedBeacon==200);
     assert(d.wire[0]==0x5008 && d.parameters[0]==13 && d.values[0]==3);
     assert(d.wire[1]==0x5008 && d.parameters[1]==3 && d.values[1]==200);
     assert(d.values[5]==204); // PS inactivity, 200 TU in milliseconds.
     const auto n=d.wire.size();s.ap.dtimPeriod=4;for(unsigned t=2700;t<3200;t+=10){d.tick(t);}
     assert(d.wire[n]==0x9001 && d.parameters[n]==0 && s.radio.appliedDTIM==4 && s.radio.appliedPS);
     UInt8 invalid[]={5,4,4,4,0,0};assert(!Ath10k::channelSwitchIEs(s,invalid,sizeof(invalid),false,6) && s.ap.dtimPeriod==4);
     UInt8 duplicate[]={5,4,0,1,0,0,5,4,0,2,0,0};assert(!Ath10k::channelSwitchIEs(s,duplicate,sizeof(duplicate),false,6) && s.ap.dtimPeriod==4);
     d.releaseWMIRuntime();}
    {MaintenanceDriver d;auto &s=d._connection;s.radio.requestedPS=true;
     for(unsigned t=2000;t<2400;t+=10){d.tick(t);}assert(s.radio.appliedPS);
     d.respondProbe=true;s.lastBeaconMs=1000;d.tick(31000);
     for(unsigned t=31010;t<31400;t+=10){d.tick(t);}
     assert(d.probes==1 && !s.probe.active && !s.cancelRequested && s.portAuthorized && s.radio.appliedPS);d.releaseWMIRuntime();}
    {MaintenanceDriver d;auto &s=d._connection;s.lastBeaconMs=1000;
     for(unsigned t=31000;t<34600;t+=10){d.tick(t);}
     assert(d.probes==3 && s.stage==LinkStage::Disabled && d._wmiLive.active);d.releaseWMIRuntime();}
    {MaintenanceDriver d;auto &s=d._connection;s.lastBeaconMs=1000;s.operatingChannel=s.centerChannel=52;s.regulatory.stationDFS=true;
     s.regulatory.country=Ath10k::regulatoryCountry("US");
     for(unsigned t=31000;t<34600;t+=10){d.tick(t);}
     assert(d.probes==0 && s.stage==LinkStage::Disabled && d._wmiLive.active);d.releaseWMIRuntime();}
    {MaintenanceDriver d;auto &s=d._connection;Ath10k::WMIBSSCache cache;
     UInt8 frame[46]{};frame[0]=0x80;for(unsigned i=0;i<6;++i)frame[10+i]=frame[16+i]=s.bssid[i];
     frame[32]=100;frame[34]=1;frame[36]=0;frame[37]=0;frame[38]=5;frame[39]=4;frame[40]=1;frame[41]=3;
     Ath10k::recordWMIBSS(cache,frame,44,6,30,0);assert(cache.count==1 && cache.entries[0].timPresent && cache.entries[0].dtimPeriod==3);
     frame[41]=0;Ath10k::recordWMIBSS(cache,frame,44,6,30,0);assert(cache.malformed==1 && cache.entries[0].dtimPeriod==3);
     s.ap.timPresent=true;s.ap.dtimPeriod=3;s.stage=LinkStage::Start;UInt8 cmd[256]{};
     assert(Ath10k::buildConnectionWMI(cmd,sizeof(cmd),2,s) && Ath10k::wmiReadLE32(cmd+28)==3);d.releaseWMIRuntime();}
    {MaintenanceDriver d;auto &s=d._connection;s.radio.requestedPS=true;d.holdEcho=true;
     for(unsigned t=2000;t<2250;t+=10){d.tick(t);}assert(s.radio.step==8);
     s.powerConstraintDb=255;d.deliver(2,echoPacket(Ath10k::radioCookie(s)));d.holdEcho=false;
     for(unsigned t=2250;t<2800;t+=10){d.tick(t);}
     assert(s.stage==LinkStage::Disabled && d._wmiLive.active && !s.radio.appliedPS);d.releaseWMIRuntime();}
    {MaintenanceDriver d;auto &s=d._connection;s.probe.active=true;s.powerConstraintDb=25;
     UInt8 response[36]{};response[0]=0x50;for(unsigned i=0;i<6;++i){response[4+i]=s.local[i];response[10+i]=response[16+i]=s.bssid[i];}
     Ath10k::receiveStationManagement(s,response,sizeof(response),0,6);assert(s.beaconSerial==1 && s.powerConstraintDb==25);d.releaseWMIRuntime();}
    {MaintenanceDriver d;auto &s=d._connection;s.radio.appliedPS=s.radio.requestedPS=true;
     s.probe.active=true;s.probe.deadlineMs=2000;s.dataPending=1;
     assert(!d.serviceRadioMaintenance(2000) && s.radio.blocked && !s.radio.step);
     assert(!Ath10k::stationWatchdog(s,2010,false) && s.probe.active);
     s.dataPending=0;assert(!d.serviceRadioMaintenance(2020) && s.radio.step==6 && !s.radio.targetPS);d.releaseWMIRuntime();}
    {MaintenanceDriver d;auto &s=d._connection;s.radio.requestedPS=true;s.dataPending=1;
     assert(!d.serviceRadioMaintenance(2000) && s.radio.blocked);s.radio.requestedPS=false;
     assert(!d.serviceRadioMaintenance(2010) && !s.radio.blocked && !s.radio.step && s.txWakePending);s.dataPending=0;d.releaseWMIRuntime();}
    {ConnectionDriver d;auto &s=d._connection;auto original=d._regs.tx3Consume;
     d._regs.tx3Consume=[&](){
        if(s.radio.step){++d.commands;if(s.radio.step==8)d.deliver(2,echoPacket(Ath10k::radioCookie(s)));
            else {d.deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});}return;}
        const bool start=s.stage==LinkStage::Start;
        if(start)assert(Ath10k::wmiReadLE32(static_cast<const UInt8 *>(d._connectionCommand.virtualAddress())+28)==1);
        original();if(start){s.ap.timPresent=true;s.ap.dtimPeriod=3;s.ap.beaconInterval=200;}
     };
     // Inspect the START snapshot before the subsequent AP/radio maintenance.
     for(unsigned t=1270;t<1800 && !d.networkUp;t+=10)d.tick(t);
     assert(s.stage==LinkStage::Connected && !s.radio.appliedDTIM && s.radio.appliedBeacon==100);
     s.radio.requestedPS=true;for(unsigned t=2000;t<2500;t+=10){d.tick(t);}
     assert(s.radio.appliedPS && s.radio.appliedDTIM==3 && s.radio.appliedBeacon==200);d.releaseWMIRuntime();}
    puts("Station maintenance passed: actual serialized PS/power/ECHO, credit/timeout/drain failures, AP watchdog, firmware warning, DFS geometry and multicast subscriptions.");
}
