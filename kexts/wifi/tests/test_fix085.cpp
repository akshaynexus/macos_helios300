#include <cstdio>
#include <cassert>
#include <cstring>
#include <vector>
#include <memory>
#include "Ath10kRX.hpp"
#include "Ath10kWMI.hpp"
using namespace Ath10k;
static void base(ConnectionState &s){
    resetInPlace(s);s.stage=LinkStage::Connected;s.portAuthorized=true;s.peerId=7;s.peerCount=1;s.peerIDs[0]=1u<<7;
    const UInt8 local[]={2,1,2,3,4,5},bssid[]={2,6,7,8,9,10};memcpy(s.local,local,6);memcpy(s.bssid,bssid,6);
    s.operatingChannel=36;s.rfChains=2;s.firmwareHT=1|2|0x800;s.firmwareVHT=1u<<5;
    s.regulatory.country=regulatoryCountry("BR");s.regulatory.indoor=true;
    auto &a=s.ap;a.htPresent=a.htOperation=a.wmmPresent=a.vhtPresent=a.vhtOperation=true;
    htcWriteLE16(a.ht,0x6e);a.ht[3]=a.ht[4]=255;a.htOp[0]=36;a.htOp[1]=5;
    htcWriteLE16(a.vht+4,0xfffa);htcWriteLE16(a.vht+8,0xfffa);htcWriteLE32(a.vht,1u<<5);
    a.vhtOp[0]=1;a.vhtOp[1]=42;htcWriteLE16(a.vhtOp+3,0xfffc);
    const UInt8 ac[]={3,0xa4,0,0,0x27,0xa4,0,0,0x42,0x43,94,0,0x62,0x32,47,0};memcpy(a.wmm,ac,16);
    negotiateHighThroughput(s);rememberNegotiatedRates(s);s.wmmConfigured=true;
    assert(s.width==80 && s.nss==2);
}
static std::vector<UInt8> assoc(ConnectionState &s,bool reduce,bool mode){
    std::vector<UInt8> f(30);f[0]=0x10;memcpy(f.data()+4,s.local,6);memcpy(f.data()+10,s.bssid,6);memcpy(f.data()+16,s.bssid,6);htcWriteLE16(f.data()+28,0xc007);
    auto add=[&](UInt8 id,const UInt8 *p,UInt32 n){f.push_back(id);f.push_back(n);f.insert(f.end(),p,p+n);};
    UInt8 ht[26];memcpy(ht,s.ap.ht,26);if(reduce)ht[4]=0;add(45,ht,26);
    UInt8 vht[12];memcpy(vht,s.ap.vht,12);if(reduce){htcWriteLE16(vht+4,0xfffe);htcWriteLE16(vht+8,0xfffe);}add(191,vht,12);
    UInt8 wmm[24]={0,0x50,0xf2,2,1,1,0,0};memcpy(wmm+8,s.ap.wmm,16);add(221,wmm,24);
    if(mode){UInt8 m=2;add(199,&m,1);}return f;
}
static void associationAndWidth(ConnectionState &s){
    UInt8 cmd[256]{};base(s);s.stage=LinkStage::Assoc;s.submitted=true;s.portAuthorized=false;
    auto f=assoc(s,true,false);receiveStationManagement(s,f.data(),f.size(),0,36);
    assert(s.assocReply && s.nss==1 && s.negotiatedNSS==1);
    s.maintenance.target=desiredAPConfig(s);s.maintenance.step=7;
    assert(buildAPMaintenance(cmd,256,2,s) && connLE32(cmd+32)==1);
    base(s);s.stage=LinkStage::Assoc;s.submitted=true;s.portAuthorized=false;f=assoc(s,false,true);
    receiveStationManagement(s,f.data(),f.size(),0,36);
    assert(s.assocReply && s.nss==1 && s.negotiatedNSS==2 && s.peerOperatingNSS==1 && s.peerOperatingWidth==80);
    base(s);s.stage=LinkStage::Assoc;s.submitted=true;s.portAuthorized=false;f=assoc(s,false,true);
    f.insert(f.end(),{199,1,0});receiveStationManagement(s,f.data(),f.size(),0,36);assert(!s.assocReply);
    base(s);s.width=40;s.centerChannel=38;s.ap.vhtOp[0]=0;s.ap.vhtOp[1]=0;s.peerOperatingWidth=40;
    receiveAPOperatingMode(s,0x12);
    assert(s.csa.phase==ChannelSwitchPhase::Idle && s.width==40 && peerBandwidth(s)==40 && s.peerOperatingWidth==80);
    receiveAPOperatingMode(s,0);assert(s.width==40 && peerBandwidth(s)==20 && s.csa.phase==ChannelSwitchPhase::Idle);
    const auto before=s.peerOperatingWidth;receiveAPOperatingMode(s,0x88);assert(s.peerOperatingWidth==before);
    base(s);const UInt8 both[]={37,3,1,44,2,62,1,1,194,3,1,42,0,199,1,0};
    assert(channelSwitchIEs(s,both,sizeof(both),false,36));
    assert(s.csa.width==80 && s.peerOperatingWidth==20 && s.peerOperatingNSS==1 && peerBandwidth(s)==20);
    s.csa.phase=ChannelSwitchPhase::PeerWidth;assert(buildChannelSwitchWMI(cmd,256,2,s));assert(!connLE32(cmd+32));
    s.csa.phase=ChannelSwitchPhase::PeerPhy;assert(buildChannelSwitchWMI(cmd,256,2,s) && connLE32(cmd+32)==8);
}
static void countryWrapper(ConnectionState &s){
    base(s);const UInt8 current[]={'B','R',' ',36,4,10};memcpy(s.ap.country,current,6);s.ap.countryPresent=true;s.ap.countryLength=6;
    const UInt8 wrapped[]={37,3,1,44,2,196,13,7,6,'B','R',' ',36,4,5,194,3,1,42,0};
    assert(channelSwitchIEs(s,wrapped,sizeof(wrapped),false,36));
    assert(s.csa.phase==ChannelSwitchPhase::Announced && s.csa.countryPresent && s.csa.countryLength==6 && s.csa.power==10);
    assert(s.ap.country[5]==10 && stationPowerLimit(s)==20);
    assert(stationPowerLimitFor(s,44,42,80,s.csa.country,6)==10);
    for(unsigned size=6;size<sizeof(wrapped);++size){base(s);assert(!channelSwitchIEs(s,wrapped,size,false,36) || !s.csa.countryPresent);}
    base(s);auto bad=std::vector<UInt8>(wrapped,wrapped+sizeof(wrapped));bad.insert(bad.end(),{196,0});
    assert(!channelSwitchIEs(s,bad.data(),bad.size(),false,36) && s.csa.phase==ChannelSwitchPhase::Idle);
    base(s);const UInt8 duplicate[]={37,3,1,44,2,196,16,7,6,'B','R',' ',36,4,5,7,6,'B','R',' ',36,4,5};
    assert(!channelSwitchIEs(s,duplicate,sizeof(duplicate),false,36));
    base(s);auto generous=std::vector<UInt8>(wrapped,wrapped+sizeof(wrapped));generous[14]=127;
    assert(channelSwitchIEs(s,generous.data(),generous.size(),false,36));assert(s.csa.power<=regulatoryPower(s.regulatory,44,42,80));
}
static void fragmentRetries(ConnectionState &s){
    const UInt8 body[]={0xaa,0xaa,3,0,0,0,8,0,0x45,0x11,0x22,0x33};
    auto defrag=std::make_unique<RXDefrag>();UInt8 frag[2048]{},out[kRXAssemblyBytes]{};UInt32 bytes=0;
    for(bool secure:{false,true})for(unsigned layout:{0u,1u,2u,3u}){
        base(s);s.secure=secure;s.securityPhase=secure?3:0;s.pairPN[3]=5;s.pairInstalled=secure;defrag->reset();
        const bool raw=layout==0 || layout==3,order=layout==3;const UInt32 header=layout==2?24:26;
        auto fill=[&](UInt8 number,bool more,bool retry,UInt32 off,UInt32 n,UInt64 pn=0){
            memset(frag,0,sizeof(frag));htcWriteLE32(frag+4,0x80000000u);htcWriteLE32(frag+32,raw?0:1u<<8);htcWriteLE32(frag+56,0xc000);
            const UInt32 skip=header+(order?4:0)+(raw && secure?8:0),trim=raw?(secure?12:4):0;htcWriteLE32(frag+24,skip+n+trim);
            auto *h=frag+300;htcWriteLE16(h,UInt16(0x0288|(order?0x8000:0)|(secure?0x4000:0)|(more?0x400:0)|(retry?0x800:0)));
            memcpy(h+4,s.local,6);memcpy(h+10,s.bssid,6);memcpy(h+16,s.bssid,6);htcWriteLE16(h+22,UInt16((88<<4)|number));
            if(header==26)h[24]=3;
            if(order)htcWriteLE32(h+26,0x11111111+number);
            memcpy(h+skip,body+off,n);memcpy(frag+236,h,24);frag[260]=3;
            if(secure){htcWriteLE32(frag+12,(6u<<28)|(1u<<13));frag[order?271:267]=0x20;htcWriteLE32(frag+16,UInt32(pn));}
        };
        fill(0,true,false,0,8,6);assert(defrag->receive(s,frag,sizeof(frag),7,0,10,out,bytes)==0);
        fill(1,true,false,8,2,7);assert(defrag->receive(s,frag,sizeof(frag),7,0,11,out,bytes)==0);
        const UInt64 deadline=defrag->entries[3].since;const UInt32 used=defrag->entries[3].used;
        fill(1,true,true,8,2,7);assert(defrag->receive(s,frag,sizeof(frag),7,0,12,out,bytes)==0);
        assert(defrag->entries[3].active && defrag->entries[3].used==used && defrag->entries[3].since==deadline && s.pairPN[3]==5);
        fill(0,true,true,0,8,6);assert(defrag->receive(s,frag,sizeof(frag),7,0,13,out,bytes)==0);
        assert(defrag->entries[3].next==2);
        fill(2,false,false,10,2,8);assert(defrag->receive(s,frag,sizeof(frag),7,0,14,out,bytes)==1);
        assert(bytes==300+header+sizeof(body) && !memcmp(out+300+header,body,sizeof(body)) && s.pairPN[3]==5);
        // Completed secure MSDU goes through the normal replay validator once.
        if(secure){s.pairPN[3]=8;assert(defrag->receive(s,frag,sizeof(frag),7,0,15,out,bytes)==-1);s.pairPN[3]=5;}
        for(unsigned invalid=0;invalid<5;++invalid){
            defrag->reset();fill(0,true,false,0,8,6);assert(defrag->receive(s,frag,sizeof(frag),7,0,20,out,bytes)==0);
            fill(1,true,false,8,2,7);assert(defrag->receive(s,frag,sizeof(frag),7,0,21,out,bytes)==0);
            fill(1,true,true,8,2,7);
            if(invalid==0)frag[300+header+(order?4:0)+(raw && secure?8:0)]^=1;
            if(invalid==1)frag[310]^=2;
            if(invalid==2)frag[260]=4;
            if(invalid==3)++s.securityGeneration;
            if(invalid==4){if(secure)htcWriteLE32(frag+16,8);else frag[301]&=~8u;}
            assert(defrag->receive(s,frag,sizeof(frag),7,0,22,out,bytes)==-1);
        }
        defrag->reset();fill(0,true,false,0,8,6);assert(defrag->receive(s,frag,sizeof(frag),7,0,30,out,bytes)==0);
        fill(0,true,true,0,8,6);assert(defrag->receive(s,frag,sizeof(frag),7,0,1029,out,bytes)==0);
        defrag->poll(1030);assert(!defrag->entries[3].active);
    }
}
int main(){auto s=std::make_unique<ConnectionState>();associationAndWidth(*s);countryWrapper(*s);fragmentRetries(*s);
    puts("0.0.85 S1-S4 regressions passed: final NSS/OM, peer/BSS bandwidth, CSA Country, raw/native CCMP Retry identity/replay/deadline.");}
