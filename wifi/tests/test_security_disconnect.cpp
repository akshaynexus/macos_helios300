#define ATH10K_CHANNEL_SWITCH_TEST
#include "test_connection.cpp"
#include <cstdio>
using D=Ath10k::DisconnectStage;
// All 36 bytes captured on QCA6174 0.0.88, not a fabricated SEC_IND payload.
static std::vector<UInt8> capturedClear(){
    return {1,0,28,0,0,59,0,0,11,128,71,0,0,0,0,0,0,0,0,0,136,54,67,0,4,0,0,1,0,0,192,0,1,0,4,0};
}
static std::vector<UInt8> sec(UInt16 peer,UInt8 flags){
    auto p=capturedClear();p[9]=flags;Ath10k::htcWriteLE16(p.data()+10,peer);return p;
}
static std::vector<UInt8> unmap(UInt16 peer){
    std::vector<UInt8> p={1,0,4,0,0,0,0,0,4,0,0,0};Ath10k::htcWriteLE16(p.data()+10,peer);return p;
}
static std::vector<UInt8> map(const Ath10k::ConnectionState &s,UInt16 peer,bool own=true){
    std::vector<UInt8> p(20);p[0]=1;p[2]=12;p[8]=3;Ath10k::htcWriteLE16(p.data()+10,peer);
    memcpy(p.data()+12,s.bssid,6);if(!own)p[12]^=2;return p;
}
static const char *parse(Ath10k::ConnectionState &s,const std::vector<UInt8> &p){
    Ath10k::HTCCreditState c{};c.initialize(2,256,2);Ath10k::RxSetupState htt{};
    htt.ringOwned=true;
    return Ath10k::parseHTTSetup(p.data(),p.size(),1,c,htt,&s);
}
static void parserCases(){
    for(D stage:{D::ClearPair,D::ClearGroup}){
        Ath10k::ConnectionState s{};s.stage=LinkStage::Connected;s.secure=true;s.securityPhase=5;
        s.disconnect=stage;s.submitted=true;s.peerCount=2;s.peerIDs[2]=(1u<<7)|(1u<<8);
        assert(!parse(s,unmap(71)) && s.peerCount==1 && Ath10k::retiredPeer(s,71));
        const UInt8 expected=stage==D::ClearPair?0x80:0;
        assert(!parse(s,sec(71,expected^0x80)) && !s.securityAck);
        assert(!parse(s,sec(999,expected)) && !s.securityAck);
        assert(!parse(s,sec(71,expected)) && s.securityAck && s.securityInd.lateClears==1);
        assert(s.stage==LinkStage::Connected && !s.portAuthorized && s.securityPhase==5);
        s.securityAck=false;assert(!parse(s,unmap(72)) && !s.peerCount);
        assert(!parse(s,capturedClear()));
        assert(s.stage==LinkStage::Connected); // teardown unmaps do not fail a new association
        Ath10k::resetDisconnected(s);
        assert(!Ath10k::retiredPeer(s,71) && !s.securityAck && s.securityInd.received==4);
    }
    // Reused ID cannot acknowledge a clear for our old AP mapping.
    Ath10k::ConnectionState reused{};reused.stage=LinkStage::Connected;reused.secure=true;
    reused.disconnect=D::ClearPair;reused.submitted=true;reused.peerIDs[2]=1u<<7;reused.peerCount=1;
    assert(!parse(reused,unmap(71)));assert(!parse(reused,map(reused,71,false)));
    assert(!Ath10k::retiredPeer(reused,71));assert(!parse(reused,capturedClear()) && !reused.securityAck);
    // NONE may not acknowledge install/rekey, open the port, or reset PN/key material.
    for(UInt32 phase:{0u,1u,2u,3u,4u,5u}){
        Ath10k::ConnectionState s{};s.stage=LinkStage::Connected;s.secure=true;s.securityPhase=phase;
        s.submitted=true;s.peerIDs[2]=1u<<7;s.peerCount=1;s.pairPN[0]=42;s.temporalKey[0]=0x55;
        assert(!parse(s,capturedClear()));
        assert(!s.securityAck && !s.portAuthorized && s.securityPhase==phase && s.pairPN[0]==42 && s.temporalKey[0]==0x55);
    }
    Ath10k::ConnectionState idle{};assert(!parse(idle,capturedClear()) && !idle.securityAck);
    // A cipher loss of an authorized mapped peer closes the port before next TX/RX.
    Ath10k::ConnectionState live{};live.stage=LinkStage::Connected;live.secure=true;live.portAuthorized=true;
    live.peerCount=1;live.peerIDs[2]=1u<<7;live.securityPhase=5;live.pairPN[0]=42;
    assert(!parse(live,capturedClear()));
    assert(!live.portAuthorized && live.cancelRequested && live.pairPN[0]==42 && !live.securityAck);
    assert(!strcmp(live.error,"firmware-key-cleared"));
    // Malformed layout/fields/credits are still rejected transactionally.
    for(unsigned n=0;n<36;++n){
        Ath10k::ConnectionState s{};auto p=capturedClear();p.resize(n);
        assert(parse(s,p) && !s.securityInd.received && !s.securityAck);
    }
    auto p=capturedClear();p.push_back(0);p[2]=29;Ath10k::ConnectionState s{};assert(parse(s,p));
    assert(parse(s,sec(2048,0x80)));assert(parse(s,sec(71,0x88)));
    p=capturedClear();addPkgCredit(p);p[p.size()-3]=255;
    Ath10k::HTCCreditState c{};c.initialize(2,256,2);assert(c.reserve(20));const auto saved=c;
    Ath10k::RxSetupState htt{};htt.ringOwned=true;s.secure=true;s.disconnect=D::ClearPair;s.submitted=true;s.peerIDs[2]=1u<<7;
    assert(Ath10k::parseHTTSetup(p.data(),p.size(),1,c,htt,&s));
    assert(!s.securityInd.received && !s.securityAck && c.available==saved.available && c.outstanding==saved.outstanding);
}
static void receive(ConnectionDriver &d,const std::vector<UInt8> &p){
    d.deliver(1,p);assert(!d.drainWMIRuntime());
}
static std::vector<UInt8> stopped(bool credit){
    std::vector<UInt8> p={2,0,12,0,0,0,0,0,2,0x50,0,0,4,0,41,0,0,0,0,0};
    if(credit)addPkgCredit(p);
    return p;
}
static void runtimeCases(){
    // Only SEC_IND is captured verbatim. MAP/UNMAP, credits and WMI replies
    // below model Linux wire layouts; logs did not capture their payloads.
    for(D target:{D::ClearPair,D::ClearGroup})for(unsigned hold=0;hold<4;++hold){
        ConnectionDriver d;d.connect();assert(d.networkUp && d._wmiLive.active);
        auto association=d._regs.tx3Consume;auto &s=d._connection;
        s.secure=true;s.securityPhase=5;s.pairInstalled=true;s.groupIndex=2;s.groupInstalled[2]=true;
        s.temporalKey[0]=0x55;s.groupKey[0]=0x66;s.pairPN[0]=42;
        receive(d,map(s,55));receive(d,map(s,71));assert(s.peerCount==3);
        assert(Ath10k::requestDisconnect(s,2000));d.networkLink(false);
        bool injected=false;unsigned clearCommands=0,deleteCommands=0;D pending=D::None;
        auto credit=[&](){d.deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});};
        d._regs.tx3Consume=[&](){
            ++d.commands;
            if(s.disconnect==D::ClearPair || s.disconnect==D::ClearGroup){
                ++clearCommands;
                if(s.disconnect==target){
                    injected=true;receive(d,unmap(7));receive(d,unmap(55));receive(d,unmap(71));
                    receive(d,capturedClear());assert(!s.peerCount && d._wmiLive.active);
                    if(hold==1){pending=s.disconnect;return;}
                }else receive(d,sec(71,0x80));
            }else if(s.disconnect==D::DeletePeer){++deleteCommands;assert(!s.peerCount && s.peerCreateIssued);}
            else if(s.disconnect==D::Stop){
                if(hold==2){pending=D::Stop;credit();}else d.deliver(2,stopped(true));return;
            }else if(s.disconnect==D::Barrier){
                if(hold==3){pending=D::Barrier;credit();}
                else d.deliver(2,echoPacket(0xa7590001));
                return;
            }else assert(s.disconnect==D::Down);
            credit();
        };
        for(unsigned i=0;i<60 && s.disconnect!=D::None && pending==D::None;++i)d.tick(2010+i*10);
        assert(injected && d._wmiLive.active && !d._dmaQuarantine && !d.networkUp);
        if(hold){
            for(unsigned i=0;i<10;++i)d.tick(2700+i*10);
            assert(s.disconnect==pending && s.stage!=LinkStage::Disabled && d._wmiLive.active);
            assert(s.temporalKey[0]==0x55 && s.pairPN[0]==42); // no premature cleanup
            if(hold==1){assert(s.wmiOwned && d._wmi.credits.outstanding);credit();}
            else if(hold==2){assert(!s.stopReply);d.deliver(2,stopped(false));}
            else {assert(!d._connectionEcho.replyReceived);d.deliver(2,echoPacket(0xa7590001,false));}
            for(unsigned i=0;i<60 && s.disconnect!=D::None;++i)d.tick(2900+i*10);
        }
        assert(s.stage==LinkStage::Disabled && s.disconnect==D::None && s.disconnectCount==1);
        assert(d._wmiLive.active && d._rxSetup.ringOwned && !s.temporalKey[0] && !s.groupKey[0] && !s.pairPN[0]);
        assert(!s.wmiOwned && !s.txOwned && !s.peerCount && deleteCommands==1);
        assert(clearCommands==(target==D::ClearPair?1u:2u));
        assert(!Ath10k::retiredPeer(s,71) && s.securityInd.received);
        // The same live driver re-associates after cleanup, without rematching.
        s.secure=false;const auto &b=d._wmiBSS.entries[0];memcpy(s.bssid,b.bssid,6);
        memcpy(s.ssid,b.ssid,b.ssidLength);s.ssidLength=b.ssidLength;s.stage=LinkStage::Scan;
        d._regs.tx3Consume=association;
        for(unsigned i=0;i<60 && !d.networkUp;++i)d.tick(4000+i*10);
        assert(d.networkUp && d._wmiLive.active && s.stage==LinkStage::Connected);d.releaseWMIRuntime();
    }
}
int main(){
    parserCases();runtimeCases();
    puts("SECURITY_IND teardown regression passed: captured 36 bytes, early unmaps, partial/reused peers, NONE/install isolation, credit/STOP/ECHO gates and reconnect without runtime restart.");
}
