#define ATH10K_AP_MAINTENANCE_TEST
#include "test_connection.cpp"
static void retries(){
    for(auto target:{LinkStage::Auth,LinkStage::Assoc})for(UInt32 status:{0u,1u}){
        auto p=std::make_unique<ConnectionDriver>();auto &d=*p;unsigned attempts=0;auto previous=d._regs.tx4Consume;
        d._regs.tx4Consume=[&](){auto &s=d._connection;
            if(s.stage!=target){previous();return;}
            ++d.packets;++attempts;
            if(attempts==3)d.deliver(2,managementResponse(s,target==LinkStage::Assoc));
            d.deliver(1,mgmtCompletion(s.txId,status));
        };
        for(unsigned now=1270;now<4700 && (!d.networkUp || !d._connection.maintenance.valid);now+=10)d.tick(now);
        assert(d.networkUp && d._wmiLive.active && attempts==3 && !d._connection.cancelRequested);
        d.releaseWMIRuntime();
    }
    {auto p=std::make_unique<ConnectionDriver>();auto &d=*p;unsigned attempts=0;auto previous=d._regs.tx4Consume;
     d._regs.tx4Consume=[&](){if(d._connection.stage!=LinkStage::Auth){previous();return;}++d.packets;++attempts;d.deliver(1,mgmtCompletion(d._connection.txId));};
     for(unsigned now=1270;now<1600;now+=10)d.tick(now);
     assert(attempts==1 && !d._connection.txOwned);d.deliver(2,managementResponse(d._connection,false));
     for(unsigned now=1600;now<2000 && !d.networkUp;now+=10)d.tick(now);
     assert(d.networkUp && attempts==1);d.releaseWMIRuntime();}
    for(auto target:{LinkStage::Auth,LinkStage::Assoc}){
        auto p=std::make_unique<ConnectionDriver>();auto &d=*p;unsigned attempts=0;auto previous=d._regs.tx4Consume;
        d._regs.tx4Consume=[&](){auto &s=d._connection;if(s.stage!=target){previous();return;}
            ++d.packets;++attempts;d.deliver(1,mgmtCompletion(s.txId));};
        bool cancelled=false;for(unsigned now=1270;now<4800;now+=10){d.tick(now);if(d._connection.cancelRequested){cancelled=true;break;}}
        assert(cancelled && attempts==3 && !d.networkUp && !strcmp(d._connection.error,"management-response-timeout"));
        d.releaseWMIRuntime();
    }
    {auto p=std::make_unique<ConnectionDriver>();auto &d=*p;unsigned attempts=0;auto previous=d._regs.tx4Consume;
     d._regs.tx4Consume=[&](){if(d._connection.stage!=LinkStage::Auth){previous();return;}++d.packets;++attempts;};
     for(unsigned now=1270;now<2500;now+=10)d.tick(now);
     assert(attempts==1 && d._connection.txOwned && d._wmiLive.active); // Completion uncertain: never retransmit DMA.
     for(unsigned now=2500;now<6700 && d._wmiLive.active;now+=10)d.tick(now);
     assert(attempts==1 && !d._wmiLive.active && !d.networkUp);d.releaseWMIRuntime();}
}
static std::vector<UInt8> spectrumRequest(const Ath10k::ConnectionState &s){
    std::vector<UInt8> f(27);f[0]=0xd0;memcpy(f.data()+4,s.local,6);memcpy(f.data()+10,s.bssid,6);memcpy(f.data()+16,s.bssid,6);f[26]=42;
    f.insert(f.end(),{38,14,7,0,0,6,0,0,0,0,0,0,0,0,10,0});return f;
}
static void spectrumTX(){
    for(UInt32 status:{0u,1u}){auto p=std::make_unique<ConnectionDriver>();auto &d=*p;d.connect();auto &s=d._connection;
        s.ap.capabilities|=0x100;auto request=spectrumRequest(s);unsigned reports=0;
        d._regs.tx4Consume=[&](){if(!s.spectrum.inFlight)return;
            ++reports;assert(s.lastTxSpectrum);
            const auto *f=static_cast<const UInt8 *>(d._packetBuffer.virtualAddress());assert(f[24]==0 && f[25]==1 && f[26]==42 && f[29]==7);
            d.deliver(1,mgmtCompletion(s.txId,status));};
        UInt8 data[18]{};memcpy(data,s.local,6);memcpy(data+6,s.bssid,6);data[12]=8;
        assert(!d.submitDataPacket(data,sizeof(data),1990) && s.dataPending==1);
        Ath10k::receiveStationManagement(s,request.data(),request.size(),0,6);assert(s.spectrum.pending);
        assert(!d.serviceConnection(2000) && !reports && s.dataPending==1);
        d.deliver(1,completion(s.dataTX[0].id));
        for(unsigned now=2010;now<2120;now+=10)d.tick(now);
        assert(reports==1 && !s.spectrum.pending && !s.spectrum.inFlight && d.networkUp && !s.cancelRequested);
        assert(s.spectrum.completed==(status?0u:1u) && s.spectrum.dropped==(status?1u:0u));d.releaseWMIRuntime();
    }
    {auto p=std::make_unique<ConnectionDriver>();auto &d=*p;d.connect();auto &s=d._connection;s.ap.capabilities|=0x100;
     auto request=spectrumRequest(s);Ath10k::receiveStationManagement(s,request.data(),request.size(),0,6);
     s.txPauseMask=1;assert(!d.serviceConnection(2000) && s.spectrum.pending);
     assert(!d.serviceConnection(7001) && !s.spectrum.pending && s.spectrum.dropped==1 && !s.cancelRequested && d.networkUp);d.releaseWMIRuntime();}
}
static void terminalCPU(){auto p=std::make_unique<ConnectionDriver>();auto &d=*p;d.connect();auto &s=d._connection;
    memset(s.temporalKey,1,16);memset(s.groupKey,2,16);memset(s.installedPair,3,16);memset(s.installedGroups,4,sizeof(s.installedGroups));
    memset(s.eapolFrames,5,sizeof(s.eapolFrames));s.pairInstalled=true;s.groupInstalled[1]=true;
    d.resetOK=false;d.endWMIRuntime("test-terminal");assert(d._dmaQuarantine && !s.portAuthorized && !s.pairInstalled && !s.groupInstalled[1]);
    auto zero=[](const void *p,size_t n){const auto *b=static_cast<const UInt8 *>(p);for(size_t i=0;i<n;++i)assert(!b[i]);};
    zero(s.installedPair,16);zero(s.installedGroups,sizeof(s.installedGroups));zero(s.eapolFrames,sizeof(s.eapolFrames));zero(s.temporalKey,16);zero(s.groupKey,16);
    assert(d._packetBuffer.allocated());d.releaseWMIRuntime();
}
int main(){retries();spectrumTX();terminalCPU();puts("0.0.86 runtime passed: Auth/Assoc response and no-ACK retries, bounded 3 attempts, no retry with DMA owned, Spectrum drain/completion/drop without disconnect, terminal CPU wipe with DMA quarantine.");}
