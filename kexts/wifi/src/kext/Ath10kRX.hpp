#pragma once
#include "Ath10kConnection.hpp"

namespace Ath10k {
static constexpr UInt32 kRXAssemblyBytes=16384+300,kRXBatchBytes=16384,kRXSubframes=192;
inline void rxDrop(ConnectionState &s,ConnectionState::RxDrop reason){
    ++s.rxDropped;++s.rxReasons[reason];s.rxLastReason=reason;
}
// All MSDUs in one MPDU are validated before the first one is delivered.
// Compact storage avoids a 192*1514 array on the kernel stack.
struct RXBatch {
    UInt8 descriptor[300]{},data[kRXBatchBytes]{};
    UInt16 offsets[kRXSubframes]{},lengths[kRXSubframes]{},peer=0,sequence=0;
    UInt8 nativeHeader[24]{};bool nativeHeaderValid=false;
    UInt32 count=0,used=0;UInt8 tid=16;bool failed=false,fragment=false;
    void reset(){count=used=0;failed=fragment=false;nativeHeaderValid=false;tid=16;}
};
// 802.11 fragmentation is separate from DMA buffer chaining. A station only
// accepts unicast fragments from its AP. CCMP fragments must have consecutive
// PNs; no PN is committed until the complete MSDU passes normal RX validation.
struct RXDefrag {
    struct Entry {
        UInt8 bytes[2048]{},identity[24]{};UInt32 used=0;
        UInt16 sequence=0,peer=0;UInt8 next=0,key=0,header=24,mode=0;UInt64 pn=0,since=0;
        UInt16 offset[16]{},length[16]{},moreMask=0,qosControl=0;UInt64 fragmentPN[16]{};
        UInt32 securityGeneration=0;UInt8 htControl[16][4]{};
        bool active=false,encrypted=false;
    } entries[17]{};
    UInt32 completed=0,waiting=0,rejected=0,timeouts=0,duplicates=0;
    void reset(){for(auto &e:entries)e.active=false;}
    void poll(UInt64 now){for(auto &e:entries)if(e.active && now-e.since>=1000){e.active=false;++timeouts;}}
    // Return 1: complete (output descriptor+native payload), 0: waiting,
    // -1: rejected. Raw MPDUs retain FCS and CCMP header/MIC; NWIFI does not.
    int receive(ConnectionState &s,const UInt8 *d,UInt32 available,UInt16 peer,
                UInt16 indicated,UInt64 now,UInt8 *out,UInt32 &outBytes){
        auto fail=[&](){++rejected;rxDrop(s,ConnectionState::Fragment);return -1;};
        if(!d || available<324)return fail();
        const UInt32 mode=(connLE32(d+32)>>8)&3,length=indicated?indicated:(connLE32(d+24)&0x3fff);
        if((mode!=0 && mode!=1) || length>available-300 || length<24)return fail();
        const UInt8 *p=d+300;const UInt16 fc=htcReadLE16(p),sc=htcReadLE16(p+22);
        const bool qos=(fc&0x80)!=0,more=(fc&0x400)!=0,encrypted=(fc&0x4000)!=0;
        const UInt8 fragment=sc&15,tid=qos?(d[260]&15):16;
        auto &e=entries[tid];
        UInt32 header=qos?26:24;
        if(mode==1 && qos){
            // Only fragment zero contains LLC. Remember that layout for all
            // continuations: their payload may itself begin with AA AA 03.
            if(fragment){if(!e.active || e.mode!=mode)return fail();header=e.header;}
            else if(length>=32 && p[24]==0xaa && p[25]==0xaa && p[26]==3)header=24;
            else if(length<34 || p[24]!=d[260] || p[25]!=d[261] ||
                    p[26]!=0xaa || p[27]!=0xaa || p[28]!=3)return fail();
        }
        const UInt32 rawHeader=header+((mode==0 && qos && (fc&0x8000))?4:0);
        if((fc&0x030c)!=0x0208 || (p[4]&1) || !sameMAC(p+4,s.local) || !sameMAC(p+10,s.bssid) ||
           !mappedPeer(s,peer) || length<header || (qos && (d[260]&0x80)))return fail();
        if(e.active && now-e.since>=1000){e.active=false;++timeouts;}
        const UInt32 info=connLE32(d+12),attention=connLE32(d+4);
        const UInt32 bad=(1u<<3)|(1u<<4)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<13)|
            (1u<<16)|(1u<<17)|(1u<<24)|(1u<<27)|(1u<<28)|(1u<<29)|(1u<<30);
        if(!(attention&0x80000000u) || (attention&bad)){e.active=false;return fail();}
        const UInt64 pn=UInt64(connLE32(d+16))|(UInt64(htcReadLE16(d+20))<<32);
        const UInt8 *iv=d+236+(qos?((fc&0x8000)?32:28):24);const UInt8 key=iv[3]>>6;
        if(s.secure && (!encrypted || !(info&(1u<<13)) || (info>>28)!=6 ||
            s.securityPhase<3 || key || !(iv[3]&0x20) || !pn || pn<=s.pairPN[tid]))return fail();
        if(!s.secure && encrypted)return fail();
        const UInt32 skip=rawHeader+(mode==0 && encrypted?8:0),trim=mode==0?(encrypted?12:4):0;
        if(length<skip+trim)return fail();
        const UInt32 body=length-skip-trim;
        const UInt16 qosControl=qos?htcReadLE16(d+260):0;
        auto identityMatches=[&](){
            if(e.peer!=peer || e.key!=key || e.encrypted!=encrypted || e.header!=header || e.mode!=mode ||
               e.securityGeneration!=s.securityGeneration || e.qosControl!=qosControl ||
               ((fc^htcReadLE16(e.identity))&0xc3ff))return false;
            for(UInt32 i=4;i<22;++i)if(p[i]!=e.identity[i])return false;
            if(mode==1 && header==26 && (p[24]!=e.bytes[324] || p[25]!=e.bytes[325]))return false;
            return true;
        };
        // Retry of an already accepted fragment does not append data, advance
        // PN, restart fragment zero, or extend the reassembly deadline. Hardware
        // integrity/cipher and the committed replay counter were checked above.
        if(e.active && e.sequence==(sc>>4) && fragment<e.next){
            if(!(fc&0x800) || !identityMatches() || body!=e.length[fragment] ||
               more!=bool(e.moreMask&(1u<<fragment)) || (s.secure && pn!=e.fragmentPN[fragment])){e.active=false;return fail();}
            if(mode==0 && qos && (fc&0x8000))for(UInt32 i=0;i<4;++i)if(p[26+i]!=e.htControl[fragment][i]){e.active=false;return fail();}
            for(UInt32 i=0;i<body;++i)if(p[skip+i]!=e.bytes[e.offset[fragment]+i]){e.active=false;return fail();}
            ++duplicates;return 0;
        }
        if(!fragment){
            e.active=false;e.used=300+header;e.sequence=sc>>4;e.peer=peer;e.next=0;
            e.key=key;e.encrypted=encrypted;e.since=now;
            e.securityGeneration=s.securityGeneration;e.qosControl=qosControl;e.moreMask=0;
            e.header=static_cast<UInt8>(header);e.mode=static_cast<UInt8>(mode);
            for(UInt32 i=0;i<300;++i)e.bytes[i]=d[i];
            for(UInt32 i=0;i<header;++i)e.bytes[300+i]=p[i];
            for(UInt32 i=0;i<24;++i)e.identity[i]=p[i];
        }else if(!e.active || e.sequence!=(sc>>4) || e.next!=fragment || !identityMatches() ||
            (s.secure && pn!=e.pn+1)){
            e.active=false;return fail();
        }else {
            // DA, transmitter and SA must remain identical across fragments.
            for(UInt32 i=4;i<22;++i)if(p[i]!=e.identity[i]){e.active=false;return fail();}
        }
        if(body>sizeof(e.bytes)-e.used || (more && fragment==15)){e.active=false;return fail();}
        e.offset[fragment]=UInt16(e.used);e.length[fragment]=UInt16(body);e.fragmentPN[fragment]=pn;
        // HT Control may change on each new MPDU; a Retry must match the
        // control bytes of that particular accepted fragment.
        for(UInt32 i=0;i<4;++i)e.htControl[fragment][i]=mode==0 && qos && (fc&0x8000)?p[26+i]:0;
        if(more)e.moreMask|=UInt16(1u<<fragment);
        for(UInt32 i=0;i<body;++i)e.bytes[e.used+i]=p[skip+i];
        e.used+=body;
        e.pn=pn;e.next=fragment+1;e.active=more;
        if(more){++waiting;return 0;}
        // Canonical native WiFi descriptor, with final PN and cleared fragment bits.
        htcWriteLE16(e.bytes+300,static_cast<UInt16>(fc&~(0x400u|0x8000u)));
        htcWriteLE16(e.bytes+322,static_cast<UInt16>(sc&~15u));
        htcWriteLE16(e.bytes+236,static_cast<UInt16>(htcReadLE16(e.bytes+236)&~0x400u));
        htcWriteLE16(e.bytes+258,static_cast<UInt16>(sc&~15u));
        htcWriteLE32(e.bytes+16,static_cast<UInt32>(pn));
        htcWriteLE16(e.bytes+20,static_cast<UInt16>(pn>>32));
        htcWriteLE32(e.bytes+24,e.used-300);htcWriteLE32(e.bytes+32,1u<<8);
        htcWriteLE32(e.bytes+56,0xc000);e.bytes[10]=0;
        for(UInt32 i=0;i<e.used;++i)out[i]=e.bytes[i];
        outBytes=e.used;if(fragment || more)++completed;return 1;
    }
};
inline bool normalizeRawRX(const UInt8 *d,UInt32 available,UInt16 indicated,UInt8 *out,UInt32 &outBytes){
    if(!d || !out || available<324)return false;
    const UInt32 length=indicated?indicated:(connLE32(d+24)&0x3fff);
    const UInt8 *p=d+300;const UInt16 fc=htcReadLE16(p);
    const bool qos=(fc&0x80)!=0,encrypted=(fc&0x4000)!=0;
    const UInt32 nativeHeader=qos?26:24,rawHeader=nativeHeader+((qos && (fc&0x8000))?4:0);
    const UInt32 skip=rawHeader+(encrypted?8:0),trim=encrypted?12:4;
    if(length>available-300 || length<skip+trim || (fc&0x400) || (htcReadLE16(p+22)&15))return false;
    if(encrypted && ((connLE32(d+12)>>28)!=6 || !(connLE32(d+12)&(1u<<13))))return false;
    const UInt32 body=length-skip-trim;
    if(300+nativeHeader+body>kRXAssemblyBytes)return false;
    for(UInt32 i=0;i<300;++i)out[i]=d[i];
    for(UInt32 i=0;i<nativeHeader;++i)out[300+i]=p[i];
    htcWriteLE16(out+300,static_cast<UInt16>(fc&~0x8000u));
    for(UInt32 i=0;i<body;++i)out[300+nativeHeader+i]=p[skip+i];
    htcWriteLE32(out+24,nativeHeader+body);htcWriteLE32(out+32,1u<<8);
    outBytes=300+nativeHeader+body;return true;
}
inline bool appendRXBatch(ConnectionState &s,RXBatch &b,const UInt8 *d,UInt32 available,
                           UInt8 status,UInt16 peer,UInt16 indicated=0){
    if(b.failed)return false;
    auto fail=[&](ConnectionState::RxDrop reason){b.failed=true;rxDrop(s,reason);return false;};
    if(!d || available<300)return fail(ConnectionState::Length);
    if(status!=1)return fail(ConnectionState::Status);
    const UInt32 attention=connLE32(d+4);
    if(attention&((1u<<27)|(1u<<28)))return fail(ConnectionState::Length);
    const UInt32 bad=(1u<<3)|(1u<<4)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<13)|
        (1u<<16)|(1u<<17)|(1u<<24)|(1u<<29)|(1u<<30);
    if(!(attention&0x80000000u) || (attention&bad))return fail(ConnectionState::Integrity);
    if(!b.count){
        for(UInt32 i=0;i<300;++i)b.descriptor[i]=d[i];
        b.peer=peer;
        b.nativeHeaderValid=available>=324 && ((connLE32(d+32)>>8)&3)==1;
        for(UInt32 i=0;i<24;++i)b.nativeHeader[i]=b.nativeHeaderValid?d[300+i]:0;
        const UInt8 *h=d+236;b.sequence=static_cast<UInt16>(htcReadLE16(h+22)>>4);
        b.tid=(h[0]&0x80)?(h[24]&15):16;
        if((h[0]&0x80) && (h[24]&0x80) && (connLE32(d+56)&0xc000u)!=0xc000u){
            if(!(connLE32(d+56)&0x4000u))return fail(ConnectionState::Aggregate);
            const UInt32 header=(htcReadLE16(h)&0x8000)?32:28;
            const UInt32 crypto=(connLE32(d+12)&(1u<<13))?8:0;
            const UInt8 rfc[6]={0xaa,0xaa,3,0,0,0};
            if(header+crypto+6<=64 && sameMAC(h+header+crypto,rfc))return fail(ConnectionState::Aggregate);
        }
    }else {
        if(peer!=b.peer)return fail(ConnectionState::Peer);
        // A new FIRST is a different MPDU, not a continuation authorized by
        // this batch. serviceHTTRxOne collects exactly one MPDU through LAST.
        if(connLE32(d+56)&0x4000u)return fail(ConnectionState::Aggregate);
    }
    // Linux v6.12 rx_desc.h: mpdu_start.encrypted, encrypt_type and PN are
    // valid only on FIRST_MSDU. rx_hdr_status is also complete only there.
    // Continuations may contain zeros or unrelated values in these fields;
    // never interpret them as a new cipher, plaintext or replay. Per-MSDU
    // bounds, DONE, attention errors, payload and HTT peer are still checked.
    // The complete MPDU's first descriptor is validated once on release.
    const bool encrypted=(connLE32(b.descriptor+12)&(1u<<13))!=0;
    UInt32 offset=0;
    do {
        UInt8 frame[kEthernetFrameMax];UInt32 next=0;
        const UInt32 bytes=decodeEthernetRX(s,d,available,status,peer,frame,sizeof(frame),indicated,offset,&next);
        if(!bytes){
            const UInt32 mode=(connLE32(d+32)>>8)&3;
            if(!mappedPeer(s,peer) && !(d[300]&1))return fail(ConnectionState::Peer);
            return fail((offset || (mode==2 && available>=314 && connBE16(d+312)<=1500))?
                ConnectionState::Aggregate:ConnectionState::Decap);
        }
        if(!(frame[0]&1) && !sameMAC(frame,s.local))return fail(ConnectionState::Peer);
        if(frame[6]&1)return fail(ConnectionState::Aggregate);
        if(s.secure && !encrypted && !(frame[12]==0x88 && frame[13]==0x8e))return fail(ConnectionState::Plaintext);
        if(s.secure && !s.portAuthorized && !(frame[12]==0x88 && frame[13]==0x8e))return fail(ConnectionState::Port);
        if(b.count==kRXSubframes || bytes>kRXBatchBytes-b.used)return fail(ConnectionState::Aggregate);
        b.offsets[b.count]=static_cast<UInt16>(b.used);b.lengths[b.count++]=static_cast<UInt16>(bytes);
        for(UInt32 i=0;i<bytes;++i)b.data[b.used+i]=frame[i];
        b.used+=bytes;
        if(next && next<=offset)return fail(ConnectionState::Aggregate);
        offset=next;
    }while(offset);
    return true;
}

// Bounded host reorder used for RX_IND only. Explicit IN_ORD_PADDR ownership
// already implies firmware ordering and is never re-reordered here.
struct RXReorder {
    struct Session {
        RXBatch *slots[64]{};UInt16 peer=0,head=0;UInt8 tid=0;
        UInt32 generation=0,pending=0;
        bool initialized=false;UInt64 gapSince=0;
    } sessions[64]{};
    UInt32 held=0,queued=0,released=0,duplicates=0,late=0,timeouts=0,holes=0,overflow=0;
    static UInt16 distance(UInt16 a,UInt16 b){return static_cast<UInt16>((a-b)&4095);}
    template<class Release,class Destroy>
    void step(Session &q,bool discard,Release release,Destroy destroy){
        RXBatch *&p=q.slots[q.head&63];
        if(p){RXBatch *b=p;p=nullptr;--held;--q.pending;if(!discard){release(*b);++released;}destroy(b);}
        else if(q.pending)++holes;
        q.head=static_cast<UInt16>((q.head+1)&4095);
    }
    template<class Release,class Destroy>
    void ready(Session &q,UInt64 now,Release release,Destroy destroy){
        while(q.slots[q.head&63])step(q,false,release,destroy);
        q.gapSince=now;
    }
    template<class Clone,class Release,class Destroy>
    void receive(ConnectionState &s,RXBatch &b,UInt64 now,Clone clone,Release release,Destroy destroy){
        if(b.failed || !b.count)return;
        UInt32 index=64;
        if(!s.firmwareReorder && b.tid<16 && !b.fragment && !(b.descriptor[240]&1))
            for(UInt32 i=0;i<64;++i)if(s.ba[i].window && s.ba[i].peer==b.peer && s.ba[i].tid==b.tid){index=i;break;}
        if(index==64){
            // One batch is one complete MPDU, including every A-MSDU MSDU.
            // CCMP uses the PN gate at delivery; never let unauthenticated
            // encrypted input poison the sequence cache before that gate.
            if(!s.secure){const UInt8 *h=b.descriptor+236;const UInt16 fc=htcReadLE16(h);
                const UInt8 tid=b.tid<17?b.tid:16;const bool group=(h[4]&1)!=0;
                UInt16 &last=group?s.groupSeq[tid]:s.pairSeq[tid];
                bool &valid=group?s.groupSeqValid[tid]:s.pairSeqValid[tid];
                if((fc&0x0800) && valid && last==b.sequence){++duplicates;rxDrop(s,ConnectionState::Duplicate);return;}
                last=b.sequence;valid=true;
            }
            release(b);return;
        }
        auto &q=sessions[index];
        if(!q.initialized || q.peer!=b.peer || q.tid!=b.tid || q.generation!=s.ba[index].generation){
            clear(q,true,release,destroy);q.initialized=true;q.peer=b.peer;q.tid=b.tid;
            q.head=s.ba[index].startValid?s.ba[index].startSeq:b.sequence;
            q.generation=s.ba[index].generation;q.gapSince=now;}
        UInt16 delta=distance(b.sequence,q.head);
        if(delta>=2048){++late;rxDrop(s,ConnectionState::Duplicate);return;}
        const UInt32 window=s.ba[index].window;
        while(delta>=window){step(q,false,release,destroy);--delta;}
        if(!delta){release(b);q.head=static_cast<UInt16>((q.head+1)&4095);ready(q,now,release,destroy);return;}
        if(q.slots[b.sequence&63]){++duplicates;rxDrop(s,ConnectionState::Duplicate);return;}
        if(held==128){++overflow;rxDrop(s,ConnectionState::Allocation);return;}
        RXBatch *copy=clone(b);if(!copy){rxDrop(s,ConnectionState::Allocation);return;}
        if(!q.pending)q.gapSince=now;
        q.slots[b.sequence&63]=copy;++held;++q.pending;++queued;
    }
    template<class Release,class Destroy>
    void clear(Session &q,bool discard,Release release,Destroy destroy){
        for(UInt32 n=0;n<64;++n)if(q.slots[n]){
            RXBatch *b=q.slots[n];q.slots[n]=nullptr;--held;--q.pending;
            if(!discard)release(*b);
            destroy(b);}
        q.initialized=false;
    }
    template<class Release,class Destroy>
    void poll(ConnectionState &s,UInt64 now,Release release,Destroy destroy){
        for(UInt32 i=0;i<64;++i){auto &q=sessions[i];if(!q.initialized)continue;
            if(s.ba[i].window && (s.ba[i].generation!=q.generation ||
               s.ba[i].peer!=q.peer || s.ba[i].tid!=q.tid)){
                // A fresh ADDBA/reused slot must not release stale MPDUs
                // from the old session before the next receive initializes it.
                clear(q,true,release,destroy);continue;
            }
            if(s.firmwareReorder || !s.ba[i].window || s.ba[i].peer!=q.peer || s.ba[i].tid!=q.tid){
                // DELBA: drain in sequence order, then forget the session.
                for(UInt32 n=0;n<64;++n)step(q,false,release,destroy);
                q.initialized=false;continue;}
            if(now-q.gapSince<100)continue;
            UInt32 skip=64;for(UInt32 n=0;n<64;++n)if(q.slots[(q.head+n)&63]){skip=n;break;}
            if(skip==64)continue;
            ++timeouts;for(UInt32 n=0;n<skip;++n)step(q,false,release,destroy);
            ready(q,now,release,destroy);
        }
    }
    template<class Destroy>void discardForKeyChange(Destroy destroy){
        // Buffered plaintext belongs to the old key epoch. Keep the BA session
        // and advance past its queued sequence numbers instead of restarting
        // from the original ADDBA SSN after a rekey.
        for(auto &q:sessions){
            for(UInt32 n=0;n<64 && q.pending;++n)
                step(q,true,[](RXBatch &){},destroy);
            q.gapSince=0;
        }
    }
    template<class Destroy>void reset(Destroy destroy){
        for(auto &q:sessions)clear(q,true,[](RXBatch &){},destroy);
    }
};
} // namespace Ath10k
