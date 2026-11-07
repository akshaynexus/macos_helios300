#define ATH10K_CHANNEL_SWITCH_TEST
#include "test_connection.cpp"
#include <cstdio>
using D=Ath10k::DisconnectStage;
static void credit(ConnectionDriver &d){d.deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});}
static std::vector<UInt8> started(){
    std::vector<UInt8> p(32);p[0]=2;p[2]=24;p[8]=1;p[9]=0x50;p[12]=16;p[14]=40;return p;
}
static std::vector<UInt8> deleted(const Ath10k::ConnectionState &s){
    std::vector<UInt8> p(28);p[0]=2;p[2]=20;p[8]=7;p[9]=0x60;p[12]=12;
    Ath10k::htcWriteLE16(p.data()+14,451);memcpy(p.data()+20,s.bssid,6);return p;
}
static std::vector<UInt8> stopped090(){
    return {2,0,12,0,0,0,0,0,2,0x50,0,0,4,0,41,0,0,0,0,0};
}
static void reconnect(ConnectionDriver &d,unsigned now){
    auto &s=d._connection;const auto &b=d._wmiBSS.entries[0];
    memcpy(s.bssid,b.bssid,6);memcpy(s.ssid,b.ssid,b.ssidLength);
    s.ssidLength=b.ssidLength;s.stage=LinkStage::Scan;
    for(unsigned t=now;t<now+800 && !d.networkUp;t+=10)d.tick(t);
    assert(d.networkUp && d._wmiLive.active && s.stage==LinkStage::Connected);
}
static void missingReply(LinkStage target,unsigned hold,bool syncDelete,bool lateReply){
    ConnectionDriver d;auto &s=d._connection;auto normal=d._regs.tx3Consume;
    bool fault=false;unsigned deletes=0,stops=0;D pending=D::None;
    if(syncDelete)d._wmi.serviceBitmap[92/4]|=1u<<(92%4);
    d._regs.tx3Consume=[&](){
        if(s.disconnect==D::None && s.stage==target){
            fault=true;++d.commands;if(hold!=1)credit(d);return; // CE consumed, semantic reply absent
        }
        if(s.disconnect==D::DeletePeer){
            ++deletes;
            if(syncDelete && hold!=2)d.deliver(2,deleted(s));
            if(hold==2)pending=D::DeletePeer;
            // A MAP delayed until deletion must be tracked even in Failed.
            if(lateReply && target==LinkStage::Peer){
                std::vector<UInt8> p(20);p[0]=1;p[2]=12;p[8]=3;p[10]=7;memcpy(p.data()+12,s.bssid,6);
                d.deliver(1,p);assert(!d.drainWMIRuntime() && s.peerCount==1);
            }
            d.deliver(1,{1,0,4,0,0,0,0,0,4,0,7,0});credit(d);return;
        }
        if(s.disconnect==D::Stop){
            ++stops;
            if(lateReply && target==LinkStage::Start){d.deliver(2,started());assert(!d.drainWMIRuntime() && !s.rollback.startPending);}
            if(hold==3){pending=D::Stop;credit(d);}else {auto p=stopped090();addPkgCredit(p);d.deliver(2,p);}
            return;
        }
        if(s.disconnect==D::Barrier && hold==4){pending=D::Barrier;credit(d);return;}
        normal();
    };
    for(unsigned t=1270;t<1800;t+=10)d.tick(t);
    assert(fault && d._wmiLive.active && s.stage==target && !d.networkUp);
    // Secrets/replay state may be cleared only after DELETE/STOP/ECHO.
    s.temporalKey[0]=0x55;s.pairPN[0]=42;
    if(hold==1){
        s.cancelRequested=true;d.tick(2000);
        assert(s.wmiOwned && !deletes && !stops && d._wmiLive.active && s.temporalKey[0]==0x55);
        credit(d);
    }else {d.tick(6500);assert(s.cancelRequested && d._wmiLive.active);}
    const unsigned first=hold==1?2010:6510;
    for(unsigned t=first;t<first+800 && s.stage!=LinkStage::Disabled && pending==D::None;t+=10)d.tick(t);
    if(hold>=2){
        for(unsigned t=first+810;t<first+910;t+=10)d.tick(t);
        assert(s.disconnect==pending && d._wmiLive.active && s.temporalKey[0]==0x55 && s.pairPN[0]==42);
        assert(!s.wmiOwned && !d._wmi.credits.outstanding);
        if(hold==2)d.deliver(2,deleted(s));
        if(hold==3)d.deliver(2,stopped090());
        if(hold==4){
            if(lateReply && target==LinkStage::Barrier){
                const auto cookie=d._connectionEcho.cookie;
                d.deliver(2,echoPacket(0xa7100047,false));assert(!d.drainWMIRuntime());
                assert(!d._connectionEcho.replyReceived && d._connectionEcho.cookie==cookie);
                assert(s.temporalKey[0]==0x55 && s.rollback.active);
            }
            d.deliver(2,echoPacket(d._connectionEcho.cookie,false));
        }
        for(unsigned t=first+920;t<first+1400 && s.stage!=LinkStage::Disabled;t+=10)d.tick(t);
    }
    assert(s.stage==LinkStage::Disabled && d._wmiLive.active && !d._dmaQuarantine && !d.resets);
    assert(s.disconnectCount==1 && s.diagnostics.rollbacks==1 && !s.temporalKey[0] && !s.pairPN[0]);
    assert(!s.wmiOwned && !s.txOwned && !s.peerCount && !s.peerCreateIssued && !s.vdevStartIssued);
    assert(stops==1 && deletes==(target==LinkStage::Start?0u:1u));
    d._regs.tx3Consume=normal;d._wmi.serviceBitmap[92/4]&=~(1u<<(92%4));
    reconnect(d,9500);d.releaseWMIRuntime();
}
static void missingCleanupStillResets(){
    for(D blocked:{D::DeletePeer,D::Stop,D::Barrier}){
        ConnectionDriver d;d.connect();auto &s=d._connection;auto normal=d._regs.tx3Consume;
        d._wmi.serviceBitmap[92/4]|=1u<<(92%4);
        d._regs.tx3Consume=[&](){
            if(s.disconnect==blocked){credit(d);return;}
            if(s.disconnect==D::DeletePeer){d.deliver(2,deleted(s));assert(!d.drainWMIRuntime());}
            normal();
        };
        assert(Ath10k::requestDisconnect(s,2000));d.networkLink(false);
        for(unsigned t=2010;t<2800;t+=10)d.tick(t);
        if(s.disconnect!=blocked || !d._wmiLive.active)fprintf(stderr,"blocked=%u actual=%u stage=%u active=%u error=%s status=%s reply=%u peers=%u\n",unsigned(blocked),unsigned(s.disconnect),unsigned(s.stage),d._wmiLive.active,s.error,d._wmiLive.status,s.deleteReply,s.peerCount);
        assert(s.disconnect==blocked && d._wmiLive.active);
        d.tick(9000);assert(!d._wmiLive.active && d.resets==1);d.releaseWMIRuntime();
    }
}
static void firstCauseAndMetadata(){
    Ath10k::ConnectionState s{};s.stage=LinkStage::Connected;s.peerCreateIssued=true;
    const UInt8 ap[]={2,6,7,8,9,10};memcpy(s.bssid,ap,6);s.local[0]=2;
    UInt8 f[20]{};f[8]=3;f[10]=71;memcpy(f+12,ap,6);Ath10k::commitConnectionHTT(s,f);
    assert(s.peerCount==1 && s.diagnostics.maps==1 && s.diagnostics.peerAfter==1);
    UInt8 deauth[26]{};deauth[0]=0xc0;memcpy(deauth+4,s.local,6);memcpy(deauth+10,ap,6);memcpy(deauth+16,ap,6);
    Ath10k::htcWriteLE16(deauth+24,15);Ath10k::receiveStationManagement(s,deauth,sizeof(deauth),0,11);
    f[8]=4;Ath10k::commitConnectionHTT(s,f);
    assert(!strcmp(s.error,"ap-disconnected") && s.diagnostics.mlmeReason==15 && s.stage==LinkStage::Failed);
    assert(s.diagnostics.peerBefore==1 && s.diagnostics.peerAfter==0 && s.diagnostics.unmaps==1);
    f[10]=99;Ath10k::commitConnectionHTT(s,f);assert(s.diagnostics.unknownUnmaps==1 && !s.peerCount);
    Ath10k::resetDisconnected(s);assert(s.diagnostics.mlmeReason==15 && s.diagnostics.unmaps==2);
}
static void keyLossThenUnmapKeepsRuntime(){
    ConnectionDriver d;d.connect();auto &s=d._connection;s.secure=true;s.securityPhase=5;
    std::vector<UInt8> none(36);none[0]=1;none[2]=28;none[8]=11;none[9]=0x80;none[10]=7;
    d.deliver(1,none);assert(!d.drainWMIRuntime() && s.cancelRequested && !s.portAuthorized);
    d.deliver(1,{1,0,4,0,0,0,0,0,4,0,7,0});assert(!d.drainWMIRuntime());
    assert(s.stage==LinkStage::Failed && !strcmp(s.error,"firmware-key-cleared"));
    for(unsigned t=2000;t<2600;t+=10)d.tick(t);
    assert(d._wmiLive.active && s.stage==LinkStage::Disabled && !strcmp(s.diagnostics.lastDisconnectReason,"firmware-key-cleared"));
    d.releaseWMIRuntime();
}
static void lateEchoIsTransactional(){
    Ath10k::ConnectionState s{};s.rollback.active=s.rollback.echoPending=true;s.rollback.echoCookie=0xa7100047;
    Ath10k::WMIEchoState echo{};echo.submitted=true;echo.cookie=0xa7590001;
    Ath10k::HTCCreditState c{};c.initialize(2,256,2);assert(c.reserve(20));const auto saved=c;
    Ath10k::WMIInitState out{};auto p=echoPacket(s.rollback.echoCookie,false);
    addPkgCredit(p);p[p.size()-3]=255;
    assert(Ath10k::parseWMIInitResponse(p.data(),p.size(),2,c,out,&echo,nullptr,nullptr,false,&s));
    assert(s.rollback.echoPending && !echo.replyReceived && c.outstanding==saved.outstanding && c.available==saved.available);
    p=echoPacket(0xa7100048,false);
    assert(Ath10k::parseWMIInitResponse(p.data(),p.size(),2,c,out,&echo,nullptr,nullptr,false,&s));
    assert(s.rollback.echoPending && !echo.replyReceived);
    p=echoPacket(s.rollback.echoCookie,false);
    assert(!Ath10k::parseWMIInitResponse(p.data(),p.size(),2,c,out,&echo,nullptr,nullptr,false,&s));
    assert(!s.rollback.echoPending && !echo.replyReceived && c.outstanding==saved.outstanding);
}
static void initialHTRatesAreNotReassociated(){
  for(bool secure:{false,true}){
    ConnectionDriver d;auto &b=d._wmiBSS.entries[0];b.htPresent=b.htOperation=b.wmmPresent=true;
    b.wmmVendor=true;b.ht[0]=12;b.ht[2]=3;b.ht[3]=b.ht[4]=255;b.htOp[0]=6;
    const UInt8 edca[]={3,0xa4,0,0,0x27,0xa4,0,0,0x42,0x43,94,0,0x62,0x32,47,0};memcpy(b.wmm,edca,16);
    auto &s=d._connection;s.secure=secure;
    if(secure){b.privacy=b.rsnPresent=true;b.capabilities|=0x10;
        b.rsnLength=sizeof(AthSecurity::stationRSN);memcpy(b.rsn,AthSecurity::stationRSN,b.rsnLength);}
    d._wmiInit.requestedChainMask=3;auto normal=d._regs.tx3Consume;unsigned peerAssocs=0,latePeerAssocs=0;
    d._regs.tx3Consume=[&](){
        const auto *p=static_cast<const UInt8 *>(d._connectionCommand.virtualAddress());
        if(Ath10k::connLE32(p+8)==0x6005){++peerAssocs;if(d._connection.portAuthorized)++latePeerAssocs;}
        if(secure && s.stage==LinkStage::Connected && !s.maintenance.step){
            if(s.securityPhase<=2){std::vector<UInt8> key(36);key[0]=1;key[2]=28;key[8]=11;
                key[9]=s.securityPhase==1?0x86:6;key[10]=7;d.deliver(1,key);}
            if(s.securityPhase==4)d.deliver(2,echoPacket(0xa7100057));else credit(d);
            return;
        }
        normal();
    };
    d.connect();
    if(secure){
        assert(!d.networkUp && s.stage==LinkStage::Connected);s.securityPhase=1;s.groupIndex=1;s.submitted=false;
        for(unsigned t=1800;t<1900;t+=10)d.tick(t);
        assert(s.securityPhase==3 && !s.portAuthorized);s.authorizeRequested=true;
        for(unsigned t=1900;t<2500;t+=10)d.tick(t);
        assert(s.securityPhase==5 && s.portAuthorized);
    }
    assert(d.networkUp && d._connection.htEnabled && d._connection.nss==2);
    assert(peerAssocs==1 && !latePeerAssocs && d._connection.maintenance.valid);
    const unsigned count=d.commands;for(unsigned t=2500;t<2700;t+=10)d.tick(t);
    assert(d.commands==count);d.releaseWMIRuntime();
  }
}
int main(){
    for(LinkStage target:{LinkStage::Start,LinkStage::Peer,LinkStage::Barrier}){
        missingReply(target,0,false,false);missingReply(target,1,false,false);
        missingReply(target,3,false,true);missingReply(target,4,false,true);
        if(target!=LinkStage::Start)missingReply(target,2,true,true);
    }
    missingCleanupStillResets();firstCauseAndMetadata();keyLossThenUnmapKeepsRuntime();lateEchoIsTransactional();initialHTRatesAreNotReassociated();
    puts("0.0.90 reconnect passed: missing replies roll back; create/delete tracked separately from HTT IDs; credits, SYNC_DELETE, STOP/ECHO and late replies; first MLME reason retained; initial HT rates not reassociated.");
}
