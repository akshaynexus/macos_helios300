#include <cstdio>
#include <cstring>
#include <cassert>
#include <initializer_list>
#include "Ath10kRX.hpp"
#include "Ath10kWMI.hpp"
using namespace Ath10k;
static ConnectionState connected(){
    ConnectionState s;s.stage=LinkStage::Connected;s.peerId=7;s.peerCount=1;s.peerIDs[0]=1u<<7;s.portAuthorized=true;
    const UInt8 local[]={2,1,2,3,4,5},ap[]={2,6,7,8,9,10};
    memcpy(s.local,local,6);memcpy(s.bssid,ap,6);return s;
}
static void native(UInt8 *d,const ConnectionState &s,UInt16 number,bool more,const UInt8 *body,UInt32 n,bool retainedQoS){
    memset(d,0,2048);htcWriteLE32(d+4,0x80000000);htcWriteLE32(d+32,1u<<8);htcWriteLE32(d+56,0xc000);
    const UInt32 header=retainedQoS?26:24;
    htcWriteLE32(d+24,header+n);UInt8 *p=d+300;p[0]=0x88;p[1]=more?6:2;
    memcpy(p+4,s.local,6);memcpy(p+10,s.bssid,6);memcpy(p+16,s.bssid,6);htcWriteLE16(p+22,0x80|number);
    memcpy(p+header,body,n);memcpy(d+236,p,24);d[260]=0;
}
static void qosTests(){
    ConnectionState s=connected();s.ap.wmmPresent=true;
    const UInt8 ac[]={3,0xa4,0,0,0x27,0xa4,0,0,0x42,0x43,94,0,0x62,0x32,47,0};
    memcpy(s.ap.wmm,ac,16);UInt8 p[128]{},f[256]{};p[12]=8;p[14]=0x45;
    for(UInt8 up=0;up<8;++up){p[15]=static_cast<UInt8>((up<<5)|3);
        assert(ethernetPriority(p,sizeof(p))==up && txPriorityTID(s,p,sizeof(p))==up);
        assert(buildHTTPacket(f,sizeof(f),p,sizeof(p),0x10000,0x20000,1,s,false));
        assert(((htcReadLE16(f+10)>>6)&31)==up);}
    p[15]=(46<<2)|3;assert(txPriorityTID(s,p,sizeof(p))==6); // EF + ECN
    p[12]=0x86;p[13]=0xdd;p[14]=0x60|11;p[15]=0x80;assert(txPriorityTID(s,p,sizeof(p))==6);
    p[12]=0x81;p[13]=0;p[14]=7<<5;p[16]=8;p[17]=0;assert(txPriorityTID(s,p,sizeof(p))==7);
    for(UInt8 aci:{UInt8(3),UInt8(2),UInt8(0),UInt8(1)}){s.ap.wmm[4*aci]|=16;
        const UInt8 expected=aci==3?5:(aci==2?0:(aci==0?1:255));assert(txPriorityTID(s,p,sizeof(p))==expected);}
    assert(!buildHTTPacket(f,sizeof(f),p,sizeof(p),0x10000,0x20000,1,s,false));
    p[12]=0x88;p[13]=0x8e;assert(txPriorityTID(s,p,sizeof(p))==16); // Control-port traffic remains usable when every data AC requires admission.
    assert(buildHTTPacket(f,sizeof(f),p,sizeof(p),0x10000,0x20000,1,s,false));
    assert(buildHTTPacket(f,sizeof(f),p,sizeof(p),0x10000,0x20000,1,s,true));
    assert(((htcReadLE16(f+10)>>6)&31)==17);
    s.portAuthorized=false;assert(txPriorityTID(s,p,sizeof(p))==16);
    for(UInt32 n=0;n<60;++n)(void)ethernetPriority(p,n);
}
static void vhtAndCountryTests(){
    UInt8 center=0;assert(vhtPrimary80(36,1,42,50,center) && center==42);
    assert(vhtPrimary80(36,2,50,0,center) && center==42);
    assert(vhtPrimary80(60,2,50,0,center) && center==58);
    assert(vhtPrimary80(36,3,42,106,center) && center==42);
    assert(!vhtPrimary80(36,1,42,58,center));assert(!vhtPrimary80(36,1,42,43,center));
    assert(!vhtPrimary80(36,3,42,50,center));
    auto s=connected();s.operatingChannel=s.centerChannel=36;s.width=20;
    s.regulatory.country=regulatoryCountry("BR");s.regulatory.indoor=true;
    const UInt8 ie[]={7,6,'B','R',' ',36,4,10,32,1,3};
    assert(channelSwitchIEs(s,ie,sizeof(ie),false,36));assert(stationPowerLimit(s)==14);
    s.regulatory.ceilingHalfDbm=6;assert(stationPowerLimit(s)==6); // AP can never relax host limit.
    s.regulatory.ceilingHalfDbm=20;s.ap.country[5]=255;assert(!stationPowerLimit(s));
    s.ap.country[5]=10;s.powerConstraintDb=11;assert(!stationPowerLimit(s));
    const UInt8 invalid[]={7,7,'B','R',' ',36,4,10,0};const auto before=s.ap.countryLength;
    assert(!channelSwitchIEs(s,invalid,sizeof(invalid),false,36) && s.ap.countryLength==before);
    UInt8 country[]={ 'B','R',' ',201,1,0,36,4,8,0};
    memcpy(s.ap.country,country,sizeof(country));s.ap.countryLength=sizeof(country);s.ap.countryPresent=true;
    assert(apCountryPower(s.ap,44)==16 && apCountryPower(s.ap,48)==16 && apCountryPower(s.ap,52)==-1);
}
int main(){qosTests();vhtAndCountryTests();
    auto s=connected();UInt8 d[2048]{},out[kRXAssemblyBytes]{};UInt32 outBytes=0;
    const UInt8 body[]={0xaa,0xaa,3,0,0,0,8,0,0x45,0x11,0x22};
    // Retained QoS is accepted by the ordinary native-WiFi decoder.
    native(d,s,0,false,body,sizeof(body),true);UInt8 eth[1514]{};
    auto baseline=decodeEthernetRX(s,d,2048,1,7,eth,sizeof(eth));assert(baseline==17);
    RXDefrag f;native(d,s,0,true,body,8,true);assert(f.receive(s,d,2048,7,0,10,out,outBytes)==0);
    native(d,s,1,false,body+8,3,true);assert(f.receive(s,d,2048,7,0,11,out,outBytes)==1);
    auto fragmented=decodeEthernetRX(s,out,outBytes,1,7,eth,sizeof(eth));
    bool equal=fragmented==17 && !memcmp(eth+14,body+8,3);
    assert(equal && fragmented==baseline); // No continuation QoS bytes in the payload.
    std::printf("native QoS retained: unfragmented=%u fragmented=%u expected=17 payload_equal=%u\n",baseline,fragmented,equal);
    f.reset();native(d,s,0,true,body,8,false);assert(f.receive(s,d,2048,7,0,20,out,outBytes)==0);
    native(d,s,1,false,body+8,3,false);assert(f.receive(s,d,2048,7,0,21,out,outBytes)==1);
    auto stripped=decodeEthernetRX(s,out,outBytes,1,7,eth,sizeof(eth));assert(stripped==17 && !memcmp(eth+14,body+8,3));
    std::printf("native QoS stripped: fragmented=%u expected=17 payload_equal=1\n",stripped);
    // No BA session: Retry with same sequence passes the current open RX path.
    RXBatch b;RXReorder q;unsigned delivered=0;
    auto release=[&](RXBatch &){++delivered;};auto clone=[](const RXBatch &x){return new RXBatch(x);};auto destroy=[](RXBatch *x){delete x;};
    native(d,s,0,false,body,sizeof(body),false);b.reset();assert(appendRXBatch(s,b,d,2048,1,7));q.receive(s,b,30,clone,release,destroy);
    d[237]|=8;d[301]|=8;b.reset();assert(appendRXBatch(s,b,d,2048,1,7));q.receive(s,b,31,clone,release,destroy);
    assert(delivered==1 && q.duplicates==1);std::printf("open RX no BA: original + same-sequence Retry delivered=%u\n",delivered);
    // Valid envelope + bounded unknown event. Policy rejects before credit parsing.
    UInt8 frame[12]{};frame[0]=2;htcWriteLE16(frame+2,4);htcWriteLE32(frame+8,0x123456);
    HTCCreditState credits;WMIInitState init;const char *error=parseWMIInitResponse(frame,sizeof(frame),2,credits,init);
    assert(!error && init.unknownCount==1);std::printf("bounded unknown WMI event: accepted, no acknowledgement\n");
    // 802.11h uses the AP Country triplet as the base for Power Constraint.
    s=connected();s.operatingChannel=s.centerChannel=36;s.width=20;
    s.regulatory.country=regulatoryCountry("BR");s.regulatory.indoor=true;s.ap.capabilities=0x100;
    const UInt8 countryAndConstraint[]={7,6,'B','R',' ',36,4,10,32,1,3};
    assert(channelSwitchIEs(s,countryAndConstraint,sizeof(countryAndConstraint),false,36));
    const auto apPower=stationPowerLimit(s);
    assert(apPower==14);std::printf("AP Country max=10dBm constraint=3dB: host_limit=%u half-dBm Linux_AP_limit=14 half-dBm\n",apPower);
    // A modern 160-MHz VHT Operation IE advertises the primary 80-MHz segment
    // (42) plus the 160-MHz center (50). Association accepts the primary 80.
    s=connected();s.operatingChannel=36;s.rfChains=2;s.firmwareHT=1|2|0x800;s.firmwareVHT=1<<5;
    auto &a=s.ap;a.htPresent=a.htOperation=a.wmmPresent=a.vhtPresent=a.vhtOperation=true;
    htcWriteLE16(a.ht,0x6e);a.ht[3]=a.ht[4]=255;a.htOp[0]=36;a.htOp[1]=5;
    htcWriteLE16(a.vht+4,0xfffa);htcWriteLE16(a.vht+8,0xfffa);htcWriteLE32(a.vht,1<<5);
    a.vhtOp[0]=1;a.vhtOp[1]=42;a.vhtOp[2]=50;htcWriteLE16(a.vhtOp+3,0xfffc);
    negotiateHighThroughput(s);assert(s.vhtEnabled && s.width==80 && s.centerChannel==42);s.negotiatedWidth=s.width;
    UInt8 ies[37]{};ies[0]=61;ies[1]=22;memcpy(ies+2,a.htOp,22);
    ies[24]=192;ies[25]=5;memcpy(ies+26,a.vhtOp,5);
    const UInt8 tim[]={5,4,0,3,0,0};memcpy(ies+31,tim,6);
    const bool accepted=channelSwitchIEs(s,ies,sizeof(ies),false,36);
    assert(accepted && s.ap.timPresent && s.ap.dtimPeriod==3);
    std::printf("AP160 primary80: association_width=%u live_beacon_accepted=%u TIM_updated=%u\n",s.width,accepted,s.ap.timPresent);
}
