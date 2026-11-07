#pragma once
#include "Ath10kWMI.hpp"
namespace Ath10k {
#ifndef ATH10K_AMPDU_LIMIT
#define ATH10K_AMPDU_LIMIT 64
#endif
#ifndef ATH10K_AMSDU_LIMIT
#define ATH10K_AMSDU_LIMIT 3
#endif
static_assert(ATH10K_AMPDU_LIMIT>=1 && ATH10K_AMPDU_LIMIT<=64,"AMPDU limit must be 1..64");
static_assert(ATH10K_AMSDU_LIMIT>=1 && ATH10K_AMSDU_LIMIT<=31,"AMSDU limit must be 1..31");
static constexpr UInt8 kAMPDULimit=ATH10K_AMPDU_LIMIT,kAMSDULimit=ATH10K_AMSDU_LIMIT;

// Linux v6.12 QCA6174 PCI htt_rx_desc_v1 / ring32 ABI.
static constexpr UInt32 kHTTRingEntries=2048, kHTTRxPosted=1023, kHTTRxBufferBytes=2048;
static constexpr UInt32 kHTTRxBudget=64;
static constexpr UInt32 kHTTRxDoneRetryLimit=100;
static constexpr UInt32 kHTTSetupCookie=0xa7100044;
struct RxSetupState {
    UInt32 stage=0, txSlot=0, txNext=0, wmiNext=0, httNext=0;
    UInt32 major=0, minor=0, messages=0, lastType=0, rxDone=0;
    UInt32 pendingMPDUs=0,rxIndications=0,recycled=0,discardedMPDUs=0;
    UInt32 rxMode=0; // 1: ordered RX_IND, 2: explicit paddr ownership.
    UInt32 addresses[kHTTRxPosted]{};
    bool addressPending[kHTTRxPosted]{};
    UInt16 completionIds[kHTTRingEntries]{},completionLengths[kHTTRingEntries]{};
    bool completionFragment[kHTTRingEntries]{};
    UInt32 fragIndications=0,pnIndications=0,rxChainWaits=0;
    UInt32 paddrIndications=0,batchCalls=0,batchMax=0,budgetHits=0,pendingHigh=0,rxChainDrops=0;
    // Missing DONE may mean a stale CPU mapping or an ordering/ownership
    // problem. Retry without reclaiming DMA, with a bounded stall guard.
    UInt32 rxCompletionNotDone=0,rxCompletionWaits=0;
    UInt32 rxDoneConsecutiveWaits=0,rxDoneTimeouts=0;
    UInt32 rxDoneLastBuffer=0xffffffffu,rxDoneLastAttention=0;
    UInt32 readIndex=0,writeIndex=kHTTRxPosted;
    UInt16 bufferIds[kHTTRingEntries]{};
    UInt8 mpduStatus[kHTTRingEntries]{};UInt16 mpduPeer[kHTTRingEntries]{};UInt32 statusRead=0,statusWrite=0;
    UInt64 startedMs=0;
    bool attempted=false, submitted=false, txOwned=false, ringOwned=false;
    bool versionRequested=false, versionReceived=false, completed=false;
    const char *status="not-run";
};
inline bool dmaSpan32(UInt64 address,UInt64 length) {
    return address && length && address<=0xffffffffULL && length-1<=0xffffffffULL-address;
}
inline UInt32 buildRxSetup(UInt8 *f, UInt32 capacity, UInt32 stage,
                           UInt8 wmiEndpoint,UInt8 httEndpoint,const UInt8 *mac,
                           UInt64 indexAddress=0,UInt64 ringAddress=0) {
    const UInt32 lengths[]={36,20,20,12,48,11};
    if(stage>=6 || !f || capacity<lengths[stage] || !wmiEndpoint || wmiEndpoint==255 ||
       !httEndpoint || httEndpoint==255 || wmiEndpoint==httEndpoint) return 0;
    if(stage==0) {
        if(!buildWMIVdevCreate(f,capacity,wmiEndpoint,mac))return 0;
        f[5]=2;return 36;
    }
    if(stage==4 && (!dmaSpan32(indexAddress,4) || !dmaSpan32(ringAddress,kHTTRingEntries*4) ||
                   (indexAddress&3) || (ringAddress&3)))return 0;
    const UInt32 bytes=lengths[stage];htcZero(f,bytes);
    f[0]=stage<3?wmiEndpoint:httEndpoint;
    f[1]=stage<3?1:0; // HTT service has credit flow disabled.
    f[5]=static_cast<UInt8>(stage<3?stage+2:stage-3);
    htcWriteLE16(f+2,static_cast<UInt16>(bytes-8));
    if(stage==1){htcWriteLE32(f+8,0x5002);htcWriteLE16(f+12,4);htcWriteLE16(f+14,87);}
    if(stage==2){htcWriteLE32(f+8,0x1d001);htcWriteLE16(f+12,4);htcWriteLE16(f+14,149);htcWriteLE32(f+16,kHTTSetupCookie);}
    if(stage==4){
        f[8]=2;f[9]=1; // HTT RX_RING_CFG, one ring.
        htcWriteLE32(f+12,static_cast<UInt32>(indexAddress));
        htcWriteLE32(f+16,static_cast<UInt32>(ringAddress));
        htcWriteLE16(f+20,kHTTRingEntries);htcWriteLE16(f+22,kHTTRxBufferBytes);
        htcWriteLE16(f+24,0xffff);htcWriteLE16(f+26,kHTTRxPosted);
        // Offsets in words, computed from Linux htt_rx_desc_v1 and rx_desc.h.
        const UInt16 offsets[]={59,75,21,31,3,20,6,10,1,2};
        for(UInt32 i=0;i<10;++i)htcWriteLE16(f+28+2*i,offsets[i]);
    }
    if(stage==5){f[8]=5;f[9]=kAMPDULimit;f[10]=kAMSDULimit;}
    return bytes;
}
// Validate VERSION_CONF and ordered RX_IND transactionally with HTC credits.
// Other control notifications do not authorize reclaiming any RX memory.
inline const char *parseHTTSetup(const UInt8 *f,UInt32 bytes,UInt8 endpoint,
                                  HTCCreditState &credits,RxSetupState &state,ConnectionState *connection=nullptr) {
    if(!f || bytes<8 || f[0]!=endpoint || !endpoint || endpoint==255)return "htt-envelope";
    if(f[1]&~(kHTCTrailerPresent|kHTCMoreBlock))return "htt-flags";
    UInt32 payload=htcReadLE16(f+2),declared=8+payload,end=declared;
    if(declared>bytes || (bytes!=declared && bytes!=((declared+3u)&~3u)))return "htt-length";
    UInt32 trailer=(f[1]&kHTCTrailerPresent)?f[4]:0;
    if(trailer>payload || ((f[1]&kHTCTrailerPresent)&&trailer<4))return "htt-trailer";
    end-=trailer;
    if(end<9)return "htt-empty";
    const UInt32 type=f[8];
    if(type==0){
        if(end!=12 || !state.versionRequested || state.versionReceived)return "htt-version-unexpected";
        if(f[10]!=2 && f[10]!=3)return "htt-version-unsupported";
    } else if(!state.ringOwned)return "htt-before-ring";
    UInt32 mpdus=0;
    UInt16 addressIds[256]{};UInt32 addressCount=0;
    if(type==1 && state.rxMode==2)return "htt-rx-mode-change";
    if(type==1){ // TLV HTT RX_IND: hdr(8), PPDU(36), prefix(4), FW desc, ranges.
        if(end<56)return "htt-rx-ind-short";
        const UInt32 ranges=wmiReadLE32(f+12)>>24;
        const UInt32 fwBytes=htcReadLE16(f+52);
        const UInt32 offset=56+((fwBytes+3u)&~3u);
        if(offset>end || ranges>(end-offset)/4 || offset+4*ranges!=end)return "htt-rx-ind-length";
        for(UInt32 i=0;i<ranges;++i)mpdus+=f[offset+4*i];
        if(mpdus>kHTTRxPosted || state.pendingMPDUs>kHTTRxPosted-mpdus)return "htt-rx-ind-overflow";
    }
    // These layouts release buffers differently; never infer ownership from them.
    if(type==0x12){
        if(state.rxMode==1)return "htt-rx-mode-change";
        // Linux htt.h defines TID bits 0..4, offload bit 5 and fragment bit 6.
        // A fragment indication is valid for chained ring2 buffers; offload
        // still has a different payload ABI and is rejected here.
        // Bit 7 is opaque (RM.4.4.1 emits 0x90/0x92).
        if(end<16 || f[12]!=0 || (f[9]&0x20))return "htt-paddr-flags";
        addressCount=htcReadLE16(f+14);
        if(!addressCount || addressCount>256 || end!=16+8*addressCount)return "htt-paddr-length";
        if(state.pendingMPDUs>kHTTRxPosted-addressCount)return "htt-paddr-overflow";
        for(UInt32 n=0;n<addressCount;++n){
            const UInt32 address=wmiReadLE32(f+16+8*n),length=htcReadLE16(f+20+8*n);
            if(!address || length>16384)return "htt-paddr-span";
            UInt32 id=0;for(;id<kHTTRxPosted;++id)if(state.addresses[id]==address)break;
            if(id==kHTTRxPosted || state.addressPending[id])return "htt-paddr-ownership";
            for(UInt32 k=0;k<n;++k)if(addressIds[k]==id)return "htt-paddr-duplicate";
            addressIds[n]=static_cast<UInt16>(id);
        }
    }
    if(type==0xa){
        // RX_FRAG_IND carries a copied firmware descriptor and no ownership
        // list.  Validate it and let the normal RX ring path consume the
        // associated chained buffers instead of aborting the runtime.
        if(state.rxMode==2)return "htt-fragment-ownership-mode-change";
        if(end<20)return "htt-rx-frag-short";
        const UInt32 fwBytes=htcReadLE16(f+16),offset=20+((fwBytes+3u)&~3u);
        if(!fwBytes || offset!=end)return "htt-rx-frag-length";
        if(state.pendingMPDUs==kHTTRxPosted)return "htt-rx-frag-overflow";
    }
    // TLV ABI: 0x10 is RX_PN_IND, 0x11 is RX_OFFLOAD_DELIVER_IND.
    // PN reports do not transfer DMA ownership; CCMP replay is checked locally.
    if(type==0x10){if(end<16 || f[11]>16 || f[14]>end-16)return "htt-rx-pn-length";}
    if(type==2 && (end!=16 || f[11]>16 || f[13]>1))return "htt-rx-flush-length";
    if(type==0x11)return "htt-rx-offload-layout-not-supported";
    if(connection){const char *error=validateConnectionHTT(*connection,f,end);if(error)return error;}
    HTCCreditState next=credits;
    for(UInt32 p=end;p<declared;){
        if(declared-p<4)return "htt-trailer-header";
        UInt32 id=f[p],len=f[p+1];p+=4;
        if(len>declared-p)return "htt-trailer-record";
        if(id==1){
            if(!len || len%4)return "htt-credit-size";
            for(UInt32 i=0;i<len;i+=4)if(!next.giveBack(f[p+i],f[p+i+1]))return "htt-credit-invalid";
        }
        p+=len;
    }
    credits=next;
    if(connection)commitConnectionHTT(*connection,f);
    if(type==0){state.major=f[10];state.minor=f[9];state.versionReceived=true;}
    if(type==1){
        state.rxMode=1;
        if(connection)connection->firmwareReorder=false;
        const UInt32 ranges=wmiReadLE32(f+12)>>24,offset=56+((htcReadLE16(f+52)+3u)&~3u);
        for(UInt32 i=0;i<ranges;++i)for(UInt32 n=0;n<f[offset+4*i];++n){
            state.mpduPeer[state.statusWrite]=htcReadLE16(f+10);
            state.mpduStatus[state.statusWrite]=f[offset+4*i+1];state.completionFragment[state.statusWrite]=false;
            state.statusWrite=(state.statusWrite+1)&(kHTTRingEntries-1);}
        state.pendingMPDUs+=mpdus;++state.rxIndications;
    }
    if(type==0x12){
        state.rxMode=2;
        if(connection)connection->firmwareReorder=true;
        for(UInt32 n=0;n<addressCount;++n){
            const UInt32 slot=state.statusWrite,id=addressIds[n];
            state.completionIds[slot]=static_cast<UInt16>(id);
            state.completionLengths[slot]=htcReadLE16(f+20+8*n);
            state.completionFragment[slot]=(f[9]&0x40)!=0;
            state.mpduPeer[slot]=htcReadLE16(f+10);
            // rx_desc.h FW_RX_DESC_INFO0_DISCARD: recycle, do not deliver.
            state.mpduStatus[slot]=(f[22+8*n]&1)?8:1;
            state.addressPending[id]=true;
            state.statusWrite=(slot+1)&(kHTTRingEntries-1);
        }
        state.pendingMPDUs+=addressCount;++state.rxIndications;++state.paddrIndications;
    }
    if(type==0xa){
        state.rxMode=1;if(connection)connection->firmwareReorder=false;
        state.mpduPeer[state.statusWrite]=htcReadLE16(f+10);
        state.mpduStatus[state.statusWrite]=(f[20]&1)?8:1;
        state.completionFragment[state.statusWrite]=true;
        state.statusWrite=(state.statusWrite+1)&(kHTTRingEntries-1);
        ++state.pendingMPDUs;++state.rxIndications;++state.fragIndications;
    }
    if(type==0x10)++state.pnIndications;
    ++state.messages;state.lastType=type;
    return nullptr;
}
}
