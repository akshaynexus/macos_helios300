#pragma once
#include "Ath10kHTC.hpp"
namespace Ath10k {
// htt_rx_desc_v1 ppdu_start starts at word 21. Only FIRST_MPDU/FIRST_MSDU
// contains valid PPDU rate metadata. These are received rates, not negotiated
// capabilities or a measurement of the local transmitter's rate/retries.
struct RXRateState {
    UInt32 samples=0,invalid=0,legacy=0,ht=0,vht=0,mu=0;
    UInt8 preamble=0,mcs=255,nss=0;UInt16 width=0;
    bool valid=false,shortGI=false;
};
inline UInt32 rateLE32(const UInt8 *p){return UInt32(htcReadLE16(p))|(UInt32(htcReadLE16(p+2))<<16);}
inline void recordRXRate(RXRateState &s,const UInt8 *d,UInt32 bytes)
{
    if(!d || bytes<124 || !(rateLE32(d+4)&1) || !(rateLE32(d+56)&0x4000))return;
    const UInt32 a=rateLE32(d+104),b=rateLE32(d+108),c=rateLE32(d+112);
    const UInt8 preamble=static_cast<UInt8>(a>>24);
    ++s.samples;s.valid=false;s.preamble=preamble;s.mcs=255;s.nss=0;s.width=0;s.shortGI=false;
    if(preamble==4){++s.legacy;s.width=20;return;}
    if(preamble==8 || preamble==9){
        const UInt8 mcs=static_cast<UInt8>(b&31);
        ++s.ht;s.mcs=mcs;s.nss=static_cast<UInt8>((mcs>>3)+1);
        s.width=(b&0x80)?40:20;s.shortGI=(c&0x80)!=0;s.valid=true;return;
    }
    if(preamble==12 || preamble==13){
        ++s.vht;s.width=static_cast<UInt16>(20u<<(b&3));s.shortGI=(c&1)!=0;
        const UInt32 group=(b>>4)&63;
        if(group!=0 && group!=63){++s.mu;return;} // MU SIG-B/user position unavailable
        const UInt8 mcs=static_cast<UInt8>((c>>4)&15),nsts=static_cast<UInt8>((b>>10)&7);
        if(mcs>9){++s.invalid;return;}
        s.mcs=mcs;s.nss=static_cast<UInt8>((b&8)?(nsts>>2)+1:nsts+1);s.valid=true;return;
    }
    ++s.invalid;
}
}
