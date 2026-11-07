#include <cassert>
#include <cstring>
#include "Ath10kRXRate.hpp"
using namespace Ath10k;
int main(){
    UInt8 d[300]{};htcWriteLE32(d+4,1);htcWriteLE32(d+56,0x4000);
    RXRateState s;
    for(UInt32 mcs=0;mcs<16;++mcs)for(UInt32 bw=0;bw<2;++bw)for(UInt32 gi=0;gi<2;++gi){
        htcWriteLE32(d+104,8u<<24);htcWriteLE32(d+108,mcs|(bw<<7));htcWriteLE32(d+112,gi<<7);
        recordRXRate(s,d,sizeof(d));assert(s.valid && s.mcs==mcs && s.nss==1+(mcs>>3));
        assert(s.width==(bw?40:20) && s.shortGI==bool(gi));
    }
    for(UInt32 mcs=0;mcs<10;++mcs)for(UInt32 nss=1;nss<=2;++nss)for(UInt32 bw=0;bw<3;++bw){
        htcWriteLE32(d+104,12u<<24);htcWriteLE32(d+108,bw|((nss-1)<<10)|(63u<<4));htcWriteLE32(d+112,(mcs<<4)|1);
        recordRXRate(s,d,sizeof(d));assert(s.valid && s.mcs==mcs && s.nss==nss && s.width==(20u<<bw) && s.shortGI);
    }
    const UInt32 samples=s.samples;htcWriteLE32(d+4,0);recordRXRate(s,d,sizeof(d));assert(s.samples==samples);
    htcWriteLE32(d+4,1);htcWriteLE32(d+56,0x8000);recordRXRate(s,d,sizeof(d));assert(s.samples==samples);
    htcWriteLE32(d+56,0x4000);recordRXRate(s,d,123);assert(s.samples==samples);
    htcWriteLE32(d+108,2|(1u<<4));recordRXRate(s,d,sizeof(d));assert(!s.valid && s.mu==1 && s.mcs==255 && s.nss==0 && s.width==80);
    htcWriteLE32(d+108,0);htcWriteLE32(d+112,10u<<4);recordRXRate(s,d,sizeof(d));assert(!s.valid && s.invalid==1);
    htcWriteLE32(d+104,4u<<24);recordRXRate(s,d,sizeof(d));assert(!s.valid && s.legacy==1 && s.mcs==255);
}
