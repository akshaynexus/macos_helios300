#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include "Ath10kConnection.hpp"
using namespace Ath10k;
static void prepare(ConnectionState &s,UInt32 channel){
    resetInPlace(s);s.operatingChannel=channel;s.rfChains=2;s.firmwareHT=1|2|0x800;s.firmwareVHT=1u<<5;
    s.regulatory.country=regulatoryCountry("US");s.regulatory.indoor=true;
    auto &a=s.ap;a.htPresent=a.htOperation=a.wmmPresent=true;a.vhtPresent=a.vhtOperation=channel>14;
    htcWriteLE16(a.ht,0x6e);a.ht[3]=a.ht[4]=255;a.htOp[0]=UInt8(channel);a.htOp[1]=0;
    htcWriteLE16(a.vht+4,0xfffa);htcWriteLE16(a.vht+8,0xfffa);htcWriteLE16(a.vhtOp+3,0xfffc);
    negotiateHighThroughput(s);rememberNegotiatedRates(s);
}
int main(){
    auto p=std::make_unique<ConnectionState>();auto &s=*p;
    const UInt32 channels[]={6,36};for(UInt32 channel:channels){
        prepare(s,channel);assert(s.width==20);
        const UInt8 ceiling=!ATH10K_HT_VHT?20:(channel<=14 && ATH10K_MAX_WIDTH==80?40:ATH10K_MAX_WIDTH);
        assert(s.negotiatedWidth==ceiling && s.negotiatedNSS==(!ATH10K_HT_VHT?1:ATH10K_MAX_NSS));
        s.stage=LinkStage::Assoc;UInt8 frame[256];const UInt32 n=buildStationManagement(frame,sizeof(frame),s);assert(n);
        bool found=false;for(UInt32 i=28;i<n;i+=2+frame[i+1])if(frame[i]==45){found=true;assert(bool(htcReadLE16(frame+i+2)&2)==(ATH10K_MAX_WIDTH>=40));
            assert(frame[i+5]==255 && frame[i+6]==(ATH10K_MAX_NSS==2?255:0));}
        assert(found==bool(ATH10K_HT_VHT));
        if(s.vhtEnabled){const UInt16 map=ATH10K_MAX_NSS==2?0xfff5:0xfffd;assert(peerRateSet(s,2,20).rx==map);}
        s.rfChains=1;negotiateHighThroughput(s);rememberNegotiatedRates(s);assert(s.negotiatedNSS==1);
        s.firmwareHT=0;negotiateHighThroughput(s);rememberNegotiatedRates(s);assert(!s.htEnabled && !s.vhtEnabled && s.negotiatedWidth==20);
        prepare(s,channel);htcWriteLE16(s.ap.ht,12);negotiateHighThroughput(s);rememberNegotiatedRates(s);assert(s.negotiatedWidth==20);
    }
    printf("Capability limits passed: HT=%u max-width=%u max-NSS=%u; 2.4/5GHz, BSS20, 1SS hardware and AP/firmware ceilings.\n",ATH10K_HT_VHT,ATH10K_MAX_WIDTH,ATH10K_MAX_NSS);
}
