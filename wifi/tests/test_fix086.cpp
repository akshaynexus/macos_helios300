#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main fix085_reference_main
#include "test_fix085.cpp"
#undef main
#pragma GCC diagnostic pop
static void finishAssoc(ConnectionState &s,bool reduced=false){
    s.stage=LinkStage::Assoc;s.submitted=true;s.portAuthorized=false;
    auto f=assoc(s,reduced,false);receiveStationManagement(s,f.data(),f.size(),0,36);
    assert(s.assocReply);s.stage=LinkStage::Connected;s.portAuthorized=true;
}
static void capabilityRecovery(ConnectionState &s){
    base(s);s.ap.htOp[1]=0;s.ap.vhtOp[0]=s.ap.vhtOp[1]=0;
    negotiateHighThroughput(s);rememberNegotiatedRates(s);finishAssoc(s);
    assert(s.width==20 && s.negotiatedWidth==80 && s.negotiatedNSS==2);
    UInt8 frame[256];s.stage=LinkStage::Assoc;const auto n=buildStationManagement(frame,sizeof(frame),s);assert(n);
    bool ht40=false,om80=false;for(UInt32 i=28;i<n;i+=2+frame[i+1]){
        if(frame[i]==45)ht40=(htcReadLE16(frame+i+2)&2)!=0;
        if(frame[i]==199)om80=(frame[i+2]&3)==2;
    }assert(ht40 && om80);s.stage=LinkStage::Connected;
    const UInt8 up[]={37,3,1,44,0,62,1,1,194,3,1,42,0};
    assert(channelSwitchIEs(s,up,sizeof(up),true) && !s.cancelRequested && s.csa.width==80);
    auto rate=peerRateSet(s,2,20);assert(rate.rx==0xfff5 && rate.tx==0xfff5);
    rate=peerRateSet(s,2,80);assert(rate.rx==0xfffa && rate.count==16);
    base(s);htcWriteLE16(s.ap.ht,0x62);negotiateHighThroughput(s);rememberNegotiatedRates(s);finishAssoc(s);
    assert(s.nss==1 && s.negotiatedNSS==2 && desiredAPConfig(s).nss==1);
    UInt8 action[27]{};action[0]=0xd0;memcpy(action+4,s.local,6);memcpy(action+10,s.bssid,6);memcpy(action+16,s.bssid,6);action[24]=7;action[25]=1;
    receiveStationManagement(s,action,sizeof(action),0,36);assert(desiredAPConfig(s).nss==2);
    applyPeerRates(s,2,80);assert(s.htRateCount==16 && s.vhtTxMap==0xfffa);
    base(s);finishAssoc(s,true);assert(s.negotiatedNSS==1 && peerRateSet(s,2,80).nss==1);
    base(s);s.rfChains=1;negotiateHighThroughput(s);rememberNegotiatedRates(s);finishAssoc(s);
    assert(s.negotiatedNSS==1 && peerRateSet(s,2,80).count==8);
}
static RXBatch rx86(ConnectionState &s,UInt16 seq,bool group=false,UInt64 pn=0){
    UInt8 d[2048]{};htcWriteLE32(d+4,0x80000000);htcWriteLE32(d+24,18);htcWriteLE32(d+32,2<<8);htcWriteLE32(d+56,0xc000);
    memcpy(d+300,s.local,6);memcpy(d+306,s.bssid,6);d[312]=8;d[314]=0x45;
    UInt8 *h=d+236;htcWriteLE16(h,s.secure?0x4288:0x0288);memcpy(h+4,s.local,6);memcpy(h+10,s.bssid,6);htcWriteLE16(h+22,seq<<4);
    if(group){h[4]=1;d[300]=1;}
    if(pn){htcWriteLE32(d+12,(6u<<28)|(1u<<13));htcWriteLE32(d+16,UInt32(pn));d[267]=group?0x60:0x20;}
    RXBatch b;assert(appendRXBatch(s,b,d,sizeof(d),1,7));return b;
}
static void baEpochAndGroup(ConnectionState &s){
    base(s);s.firmwareReorder=false;s.ba[0].peer=7;s.ba[0].tid=0;s.ba[0].window=64;s.ba[0].startValid=true;s.ba[0].startSeq=100;
    auto q=std::make_unique<RXReorder>();std::vector<UInt16> delivered;
    auto clone=[](const RXBatch &b){return new RXBatch(b);};auto destroy=[](RXBatch *p){delete p;};auto release=[&](RXBatch &b){delivered.push_back(b.sequence);};
    auto b=rx86(s,101);q->receive(s,b,1,clone,release,destroy);assert(q->held==1);
    b=rx86(s,1000,true);q->receive(s,b,2,clone,release,destroy);
    assert(q->sessions[0].head==100 && delivered==std::vector<UInt16>{1000} && q->held==1);
    b=rx86(s,100);q->receive(s,b,3,clone,release,destroy);
    assert(delivered==std::vector<UInt16>({1000,100,101}) && !q->late && !q->held);
    q->reset(destroy);delivered.clear();base(s);s.secure=true;s.securityPhase=5;s.groupCipher=AthSecurity::cipherCCMP;s.groupIndex=1;s.groupInstalled[1]=true;s.pairInstalled=true;
    s.ba[0].peer=7;s.ba[0].tid=0;s.ba[0].window=64;s.ba[0].startValid=true;s.ba[0].startSeq=100;
    auto secureRelease=[&](RXBatch &batch){if(acceptSecureRX(s,batch.descriptor,batch.data,batch.lengths[0]))delivered.push_back(batch.sequence);};
    b=rx86(s,101,false,11);q->receive(s,b,1,clone,secureRelease,destroy);
    b=rx86(s,1000,true,20);q->receive(s,b,2,clone,secureRelease,destroy);
    q->receive(s,b,3,clone,secureRelease,destroy);assert(s.groupPN[0]==20 && !s.pairPN[0] && q->sessions[0].head==100 && q->held==1 && s.rxLastReason==ConnectionState::Replay);
    b=rx86(s,100,false,10);q->receive(s,b,4,clone,secureRelease,destroy);
    assert(delivered==std::vector<UInt16>({1000,100,101}) && s.pairPN[0]==11 && !q->held);
    // The original group receiver decides BA membership even if an inner A-MSDU destination is unicast.
    b=rx86(s,1001,true,21);b.data[0]=s.local[0];q->receive(s,b,5,clone,secureRelease,destroy);assert(q->sessions[0].head==102 && s.groupPN[0]==21);
    q->reset(destroy);delivered.clear();base(s);s.firmwareReorder=false;s.baSSNValid[0]=true;s.baSSN[0]=100;
    UInt8 message[12]{};message[8]=5;message[9]=64;htcWriteLE16(message+10,7<<4);commitConnectionHTT(s,message);
    assert(s.ba[0].startValid && !s.baSSNValid[0]);message[8]=6;commitConnectionHTT(s,message);
    assert(!s.ba[0].window && !s.baSSNValid[0]);message[8]=5;commitConnectionHTT(s,message);assert(!s.ba[0].startValid);
    b=rx86(s,3000);q->receive(s,b,4,clone,release,destroy);assert(delivered==std::vector<UInt16>{3000} && !q->held);
    s.baSSNValid[4]=true;clearPeerBA(s,7);assert(!s.baSSNValid[4]);q->reset(destroy);
}
static void vlanAndConstraints(ConnectionState &s){
    base(s);UInt8 cmd[256],out[kEthernetFrameMax];
    for(unsigned tags=0;tags<=2;++tags){
        std::vector<UInt8> packet(1514+4*tags);memcpy(packet.data(),s.local,6);memcpy(packet.data()+6,s.bssid,6);
        for(unsigned t=0;t<tags;++t){packet[12+4*t]=t?0x81:0x88;packet[13+4*t]=t?0:0xa8;packet[14+4*t]=0xc0;}
        packet[12+4*tags]=8;assert(ethernetFrameAllowed(packet.data(),packet.size()));
        assert(buildHTTPacket(cmd,sizeof(cmd),packet.data(),packet.size(),0x10000,0x20000,3,s,false));
        if(tags)assert(ethernetPriority(packet.data(),packet.size())==6);
        UInt8 d[2048]{};htcWriteLE32(d+4,0x80000000);htcWriteLE32(d+24,packet.size());htcWriteLE32(d+32,2<<8);
        memcpy(d+300,packet.data(),packet.size());assert(decodeEthernetRX(s,d,sizeof(d),1,7,out,sizeof(out))==packet.size());
        auto *p=d+300;memset(p,0,1748);htcWriteLE32(d+32,1<<8);htcWriteLE32(d+24,24+8+packet.size()-14);
        htcWriteLE16(p,0x0208);memcpy(p+4,s.local,6);memcpy(p+10,s.bssid,6);memcpy(p+16,s.bssid,6);
        p[24]=p[25]=0xaa;p[26]=3;memcpy(p+30,packet.data()+12,packet.size()-12);
        assert(decodeEthernetRX(s,d,sizeof(d),1,7,out,sizeof(out))==packet.size());
        std::vector<UInt8> sub(14+8+packet.size()-14);memcpy(sub.data(),packet.data(),12);const unsigned body=sub.size()-14;
        sub[12]=body>>8;sub[13]=body;sub[14]=sub[15]=0xaa;sub[16]=3;memcpy(sub.data()+20,packet.data()+12,packet.size()-12);
        assert(decodeAMSDUSubframe(sub.data(),sub.size(),0,out,sizeof(out),nullptr)==packet.size());
        packet.push_back(0);assert(!ethernetFrameAllowed(packet.data(),packet.size()));
        assert(!buildHTTPacket(cmd,sizeof(cmd),packet.data(),packet.size(),0x10000,0x20000,3,s,false));
    }
    UInt8 shortTag[16]={};shortTag[12]=0x81;assert(!ethernetFrameAllowed(shortTag,sizeof(shortTag)));
    base(s);s.radio.vdevPower=s.radio.appliedPower=s.txPowerHalfDbm=20;s.ap.capabilities=0x100;
    const UInt8 reduced[]={7,6,'B','R',' ',36,4,5,32,1,1};assert(channelSwitchIEs(s,reduced,sizeof(reduced),false,36));
    const auto before=stationOperationalPowerLimit(s);assert(before==8);
    const UInt8 absent[]={3,1,36};assert(channelSwitchIEs(s,absent,sizeof(absent),false,36));
    assert(s.ap.countryPresent && s.powerConstraintDb==1 && stationOperationalPowerLimit(s)==before);
    const UInt8 zero[]={32,1,0};assert(channelSwitchIEs(s,zero,sizeof(zero),false,36) && stationOperationalPowerLimit(s)==10);
    const UInt8 invalid[]={7,6,'B','R',' ',36,4};assert(!channelSwitchIEs(s,invalid,sizeof(invalid),false,36) && stationOperationalPowerLimit(s)==10);
}
static void terminalSecrets(ConnectionState &s){
    base(s);memset(s.temporalKey,1,16);memset(s.groupKey,2,16);memset(s.installedPair,3,16);memset(s.installedGroups,4,sizeof(s.installedGroups));
    memset(s.eapolFrames,5,sizeof(s.eapolFrames));memset(s.eapolLength,6,sizeof(s.eapolLength));memset(s.groupRSC,7,6);
    s.pairInstalled=true;for(auto &v:s.groupInstalled)v=true;s.eapolCount=4;
    wipeConnectionSecrets(s);auto zero=[](const void *p,size_t n){const auto *b=static_cast<const UInt8 *>(p);for(size_t i=0;i<n;++i)assert(!b[i]);};
    zero(s.temporalKey,16);zero(s.groupKey,16);zero(s.installedPair,16);zero(s.installedGroups,sizeof(s.installedGroups));
    zero(s.eapolFrames,sizeof(s.eapolFrames));zero(s.eapolLength,sizeof(s.eapolLength));zero(s.groupRSC,6);
    assert(!s.portAuthorized && !s.pairInstalled && !s.eapolCount);for(auto v:s.groupInstalled)assert(!v);
}
static void extendedClasses(ConnectionState &s){
    const std::vector<std::vector<UInt8>> good={
        {60,4,1,128,44,0},{60,4,1,129,44,0},
        {60,4,1,129,44,0,194,3,2,50,0},
        {60,4,1,129,44,0,194,3,1,42,50},
        {60,4,1,130,44,0,194,3,3,42,106},
        {60,4,1,130,44,0,194,3,1,42,106}};
    for(const auto &ies:good){base(s);assert(channelSwitchIEs(s,ies.data(),ies.size(),true));
        assert(!s.cancelRequested && s.csa.width==80 && s.csa.center==42 && s.csa.primary==44);
        base(s);s.negotiatedWidth=40;assert(channelSwitchIEs(s,ies.data(),ies.size(),true));assert(!s.cancelRequested && s.csa.width==40 && s.csa.center==46);
    }
    base(s);const UInt8 unknownSecond[]={60,4,1,130,44,0};assert(channelSwitchIEs(s,unknownSecond,sizeof(unknownSecond),true) && s.csa.width==20 && !s.cancelRequested);
    for(const std::vector<UInt8> &ies:std::vector<std::vector<UInt8>>{{60,4,1,129,44,0,194,3,0,0,0},{60,4,1,129,44,0,194,3,1,42,0},{60,4,1,128,44,0,194,3,2,50,0},{60,4,1,116,40,0},{60,4,1,131,44,0}}){
        base(s);assert(channelSwitchIEs(s,ies.data(),ies.size(),true) && s.cancelRequested && s.csa.rejected==1);
    }
    base(s);s.regulatory.stationDFS=false;const UInt8 dfs[]={60,4,1,128,52,0};assert(channelSwitchIEs(s,dfs,sizeof(dfs),true) && s.cancelRequested);
}
static void spectrumParsing(ConnectionState &s){
    base(s);s.ap.capabilities|=0x100;s.radio.appliedPower=18;
    std::vector<UInt8> f(27);f[0]=0xd0;memcpy(f.data()+4,s.local,6);memcpy(f.data()+10,s.bssid,6);memcpy(f.data()+16,s.bssid,6);f[26]=42;
    f.insert(f.end(),{38,14,7,0,0,36,0,0,0,0,0,0,0,0,10,0});f.insert(f.end(),{38,3,8,0,99});
    receiveStationManagement(s,f.data(),f.size(),0,36);assert(s.spectrum.pending && s.spectrum.length==37);
    const UInt8 expected[]={0,1,42,39,3,7,4,0,39,3,8,2,99};assert(!memcmp(s.spectrum.frame+24,expected,sizeof(expected)));
    receiveStationManagement(s,f.data(),f.size(),0,36);assert(s.spectrum.dropped==1);
    s.spectrum={};f.pop_back();receiveStationManagement(s,f.data(),f.size(),0,36);assert(!s.spectrum.pending);
    f.resize(27);f[25]=2;f.insert(f.end(),{34,0});receiveStationManagement(s,f.data(),f.size(),0,36);
    const UInt8 tpc[]={0,3,42,35,2,9,0};assert(s.spectrum.pending && !memcmp(s.spectrum.frame+24,tpc,sizeof(tpc)));
    s.spectrum={};f[10]^=2;receiveStationManagement(s,f.data(),f.size(),0,36);assert(!s.spectrum.pending);
}
int main(){auto p=std::make_unique<ConnectionState>();auto &s=*p;capabilityRecovery(s);baEpochAndGroup(s);vlanAndConstraints(s);terminalSecrets(s);extendedClasses(s);spectrumParsing(s);
    puts("0.0.86 A1-A7/ECSA/Spectrum passed: initial20/staticSMPS recovery, association/1SS ceilings, group BA isolation, fresh BA epoch, VLAN MTU RX/TX/native/A-MSDU, retained power, key copies, wider AP classes and atomic negative reports.");}
