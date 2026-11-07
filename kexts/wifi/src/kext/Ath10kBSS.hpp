#pragma once
#include "Ath10kScanPlan.hpp"
#include "Ath10kInPlace.hpp"
#include "../shared/Ath10kSecurity.hpp"
namespace Ath10k {
static constexpr UInt32 kWMIBSSMax=64;
struct WMIBSS {
    bool htPresent=false,htOperation=false,vhtPresent=false,vhtOperation=false,wmmPresent=false;
    UInt8 ht[26]{},htOp[22]{},vht[12]{},vhtOp[5]{},wmm[16]{};
    UInt8 bssid[6]{}, ssid[32]{}, ssidLength=0, advertisedChannel=0;
    UInt8 rsn[257]{};UInt16 rsnLength=0;
    bool hidden=true, privacy=false, rsnPresent=false, wpaPresent=false;
    bool timPresent=false;UInt8 dtimCount=0,dtimPeriod=0;
    bool powerConstraintPresent=false;UInt8 powerConstraintDb=0;
    UInt8 country[254]{},countryLength=0,wmmCount=0;
    bool countryPresent=false,erpPresent=false,wmmVendor=true;UInt8 erp=0;
    UInt32 rxChannel=0, snr=0, seen=0;
    UInt16 capabilities=0,beaconInterval=0;
    UInt8 rates[32]{},rateCount=0;bool ratesValid=true;
};
// Country only restricts our configured domain. Extension triplets are not
// channels; max power is signed dBm (negative values forbid this host's TX).
inline bool validAPCountry(const UInt8 *v,UInt32 n){
    if(!v || n<6 || n>254 || (n&1))return false;
    const UInt32 remainder=(n-3)%3;
    return remainder==0 || (remainder==1 && v[n-1]==0);
}
inline int apCountryPower(const UInt8 *country,UInt32 length,UInt32 channel){
    if(!validAPCountry(country,length))return -1;
    int limit=-1;
    for(UInt32 i=3;i+3<=length;i+=3){
        const UInt32 first=country[i],count=country[i+1],step=channel<=14?1:4;
        if(!first || first>=201 || !count || (first<=14)!=(channel<=14) || channel<first)continue;
        if((channel-first)%step || (channel-first)/step>=count)continue;
        const int power=country[i+2]<128?int(country[i+2])*2:0;
        if(limit<0 || power<limit)limit=power;
    }
    return limit;
}
inline int apCountryPower(const WMIBSS &a,UInt32 channel){return a.countryPresent?apCountryPower(a.country,a.countryLength,channel):-1;}
inline bool parseAPEDCA(const UInt8 *v,UInt32 n,UInt8 *out,UInt8 &count){
    if(!v || !out || n!=18)return false;
    UInt32 seen=0;
    for(UInt32 i=0;i<4;++i){const UInt8 *ac=v+2+4*i;const UInt32 aci=(ac[0]>>5)&3;
        if((seen&(1u<<aci)) || (ac[0]&0x80) || !(ac[0]&15) || (ac[1]&15)>(ac[1]>>4))return false;
        seen|=1u<<aci;for(UInt32 j=0;j<4;++j)out[4*aci+j]=ac[j];}
    count=v[0]&15;return seen==15;
}
inline bool parseAPWMM(const UInt8 *v,UInt32 n,UInt8 *out,UInt8 &count){
    if(!v || n!=24 || v[0] || v[1]!=0x50 || v[2]!=0xf2 || v[3]!=2 || v[4]!=1 || v[5]!=1)return false;
    return parseAPEDCA(v+6,n-6,out,count);
}
enum BSSReject { BSSShort, BSSHeader, BSSAddress, BSSIEBounds, BSSSSID, BSSDS, BSSHT, BSSChannelConflict, BSSRejectCount };
inline const char *bssRejectName(UInt32 reason){
    const char *names[]={"short-frame","frame-header","bssid","ie-bounds","ssid","ds-parameter","ht-operation","ds-ht-conflict"};
    return reason<BSSRejectCount?names[reason]:"none";
}
struct WMIBSSCache {
    UInt32 rejects[BSSRejectCount]{},lastReject=BSSRejectCount;
    WMIBSS entries[kWMIBSSMax]{};
    UInt32 count=0, accepted=0, malformed=0, rxErrors=0, ignored=0, overflow=0;
    UInt32 beacons=0, probeResponses=0, lastStatus=0, lastChannel=0, lastBytes=0;
};
inline void rejectBSS(WMIBSSCache &c,BSSReject reason){++c.malformed;++c.rejects[reason];c.lastReject=reason;}
// Pure bounded 802.11 management decoder. RF input errors are counted and dropped;
// they do not invalidate an otherwise well-formed WMI/HTC credit transaction.
inline void recordWMIBSS(WMIBSSCache &cache, const UInt8 *f, UInt32 bytes,
                         UInt32 channel, UInt32 snr, UInt32 status) {
    cache.lastStatus=status;cache.lastChannel=channel;cache.lastBytes=bytes;
    if(status&0x39u){++cache.rxErrors;return;} // CRC/decrypt/MIC/key-cache errors
    if(!f || bytes<24){rejectBSS(cache,BSSShort);return;}
    const UInt16 fc=htcReadLE16(f);
    if((fc&0x00fcu)!=0x0080u && (fc&0x00fcu)!=0x0050u){++cache.ignored;return;}
    if((fc&0x4303u) || (fc&0x0400u) || (htcReadLE16(f+22)&15u) || bytes<36){rejectBSS(cache,BSSHeader);return;}
    if((fc&0xf0u)==0x80u)++cache.beacons;else ++cache.probeResponses;
    if(!wmiScanChannelAllowed(channel)){++cache.ignored;return;} // Accept only channels in this diagnostic plan.
    WMIBSS candidate{};candidate.rxChannel=channel;candidate.snr=snr;
    UInt8 nonzero=0;
    for(UInt32 i=0;i<6;++i){candidate.bssid[i]=f[16+i];nonzero|=f[16+i];}
    if(!nonzero || (candidate.bssid[0]&1)){rejectBSS(cache,BSSAddress);return;}
    candidate.capabilities=htcReadLE16(f+34);candidate.beaconInterval=htcReadLE16(f+32);
    candidate.privacy=(candidate.capabilities&0x10u)!=0;
    bool haveSSID=false,haveDS=false,haveHT=false,haveEDCA=false,haveWMM=false;
    UInt8 dsChannel=0,htChannel=0;
    for(UInt32 p=36;p<bytes;){
        if(bytes-p<2){rejectBSS(cache,BSSIEBounds);return;}
        const UInt32 id=f[p],len=f[p+1];p+=2;
        if(len>bytes-p){rejectBSS(cache,BSSIEBounds);return;}
        if(id==0){
            if(haveSSID || len>32){rejectBSS(cache,BSSSSID);return;}
            haveSSID=true;candidate.ssidLength=static_cast<UInt8>(len);
            for(UInt32 i=0;i<len;++i){candidate.ssid[i]=f[p+i];if(f[p+i])candidate.hidden=false;}
        } else if(id==5){
            if(candidate.timPresent || len<4 || !f[p+1] || f[p]>=f[p+1]){rejectBSS(cache,BSSIEBounds);return;}
            candidate.timPresent=true;candidate.dtimCount=f[p];candidate.dtimPeriod=f[p+1];
        } else if(id==32){
            if(len!=1 || candidate.powerConstraintPresent){rejectBSS(cache,BSSIEBounds);return;}
            candidate.powerConstraintPresent=true;candidate.powerConstraintDb=f[p];
        } else if(id==7){
            if(candidate.countryPresent || !validAPCountry(f+p,len)){rejectBSS(cache,BSSIEBounds);return;}
            candidate.countryPresent=true;candidate.countryLength=static_cast<UInt8>(len);
            for(UInt32 i=0;i<len;++i)candidate.country[i]=f[p+i];
        } else if(id==42){
            if(candidate.erpPresent || len!=1){rejectBSS(cache,BSSIEBounds);return;}
            candidate.erpPresent=true;candidate.erp=f[p];
        } else if(id==1 || id==50){
            if(!len || (id==1 && len>8) || len>32u-candidate.rateCount)candidate.ratesValid=false;
            else for(UInt32 i=0;i<len;++i)candidate.rates[candidate.rateCount++]=f[p+i];
        } else if(id==3){
            if(haveDS || len!=1 || !f[p] || f[p]>14){rejectBSS(cache,BSSDS);return;}
            haveDS=true;dsChannel=f[p];
        } else if(id==61){ // IEEE 802.11 HT Operation: primary channel + 21 bytes.
            if(haveHT || len!=22 || !f[p]){rejectBSS(cache,BSSHT);return;}
            haveHT=true;htChannel=f[p];candidate.htOperation=true;
            for(UInt32 i=0;i<22;++i)candidate.htOp[i]=f[p+i];
        } else if(id==45 || id==191 || id==192){
            bool &present=id==45?candidate.htPresent:(id==191?candidate.vhtPresent:candidate.vhtOperation);
            const UInt32 expected=id==45?26:(id==191?12:5);
            if(present || len!=expected){rejectBSS(cache,BSSIEBounds);return;}
            present=true;UInt8 *out=id==45?candidate.ht:(id==191?candidate.vht:candidate.vhtOp);
            for(UInt32 i=0;i<len;++i)out[i]=f[p+i];
        } else if(id==12){
            UInt8 ac[16]{},count=0;
            if(haveEDCA || !parseAPEDCA(f+p,len,ac,count)){rejectBSS(cache,BSSIEBounds);return;}
            haveEDCA=true;
            if(!haveWMM){candidate.wmmPresent=true;candidate.wmmVendor=false;candidate.wmmCount=count;
                for(UInt32 i=0;i<16;++i)candidate.wmm[i]=ac[i];}
        } else if(id==221 && len>=6 && f[p]==0 && f[p+1]==0x50 && f[p+2]==0xf2 && f[p+3]==2 && f[p+4]==1 && f[p+5]==1){
            if(haveWMM || !parseAPWMM(f+p,len,candidate.wmm,candidate.wmmCount)){rejectBSS(cache,BSSIEBounds);return;}
            haveWMM=true;candidate.wmmPresent=true;candidate.wmmVendor=true;
        } else if(id==48){
            if(candidate.rsnPresent){rejectBSS(cache,BSSIEBounds);return;}
            candidate.rsnPresent=true;candidate.rsnLength=static_cast<UInt16>(len+2);
            for(UInt32 j=0;j<len+2;++j)candidate.rsn[j]=f[p-2+j];
        } else if(id==221 && len>=4 && f[p]==0 && f[p+1]==0x50 && f[p+2]==0xf2 && f[p+3]==1){candidate.wpaPresent=true;}
        p+=len;
    }
    if(haveDS && haveHT && dsChannel!=htChannel){rejectBSS(cache,BSSChannelConflict);return;}
    candidate.advertisedChannel=haveHT?htChannel:dsChannel;
    if(!haveSSID){rejectBSS(cache,BSSSSID);return;}
    UInt32 slot=cache.count;
    for(UInt32 n=0;n<cache.count;++n){
        bool same=true;for(UInt32 i=0;i<6;++i)if(cache.entries[n].bssid[i]!=candidate.bssid[i])same=false;
        if(same){slot=n;break;}
    }
    if(slot==cache.count){
        if(cache.count==kWMIBSSMax){++cache.overflow;return;}
        ++cache.count;
    } else if(candidate.hidden && !cache.entries[slot].hidden){
        candidate.hidden=false;candidate.ssidLength=cache.entries[slot].ssidLength;
        for(UInt32 i=0;i<32;++i)candidate.ssid[i]=cache.entries[slot].ssid[i];
    }
    candidate.seen=cache.entries[slot].seen+1;
    cache.entries[slot]=candidate;++cache.accepted;
}
// SSIDs are arbitrary bytes, not assumed UTF-8 or NUL-terminated. Escape them
// for IORegistry/log display without truncating embedded NUL/control bytes.
inline void formatWMISSID(const WMIBSS &entry,char (&out)[129]) {
    const char hex[]="0123456789abcdef";UInt32 n=0;
    for(UInt32 i=0;i<entry.ssidLength && i<32;++i){
        const UInt8 c=entry.ssid[i];
        if(c>=32 && c<=126 && c!=92 && c!=34)out[n++]=static_cast<char>(c);
        else {out[n++]=92;out[n++]='x';out[n++]=hex[c>>4];out[n++]=hex[c&15];}
    }
    out[n]=0;
}
} // namespace Ath10k
