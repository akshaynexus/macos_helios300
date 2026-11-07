#include <cassert>
#include <cstring>
#include <vector>
#include "Ath10kRX.hpp"
#include "Ath10kRxSetup.hpp"
using namespace Ath10k;
static ConnectionState connected(){
    ConnectionState s;s.stage=LinkStage::Connected;s.peerId=7;s.peerCount=1;s.peerIDs[0]=1u<<7;
    const UInt8 local[]={2,1,2,3,4,5},ap[]={2,6,7,8,9,10};
    memcpy(s.local,local,6);memcpy(s.bssid,ap,6);s.portAuthorized=true;return s;
}
static void ethernet(UInt8 *d,const ConnectionState &s,UInt16 seq=1,UInt64 pn=0){
    memset(d,0,2048);htcWriteLE32(d+4,0x80000000);htcWriteLE32(d+24,18);htcWriteLE32(d+32,2<<8);
    htcWriteLE32(d+56,0xc000);memcpy(d+300,s.local,6);memcpy(d+306,s.bssid,6);d[312]=8;d[314]=0x45;
    UInt8 *h=d+236;h[0]=0x88;h[1]=s.secure?0x42:2;memcpy(h+4,s.local,6);memcpy(h+10,s.bssid,6);
    htcWriteLE16(h+22,static_cast<UInt16>(seq<<4));h[31]=0x20;
    if(pn){htcWriteLE32(d+12,(6u<<28)|(1u<<13));htcWriteLE32(d+16,static_cast<UInt32>(pn));htcWriteLE16(d+20,static_cast<UInt16>(pn>>32));}
}
static RXBatch batch(ConnectionState &s,UInt16 seq,UInt64 pn=0){
    UInt8 d[2048];ethernet(d,s,seq,pn);RXBatch b;assert(appendRXBatch(s,b,d,sizeof(d),1,7));return b;
}
static void aggregate(){
    auto s=connected();s.secure=true;s.securityPhase=5;UInt8 d[2048];ethernet(d,s,10,5);
    UInt8 *p=d+300;d[260]=0x80;
    for(UInt32 at:{0u,28u}){
        memcpy(p+at,s.local,6);memcpy(p+at+6,s.bssid,6);p[at+12]=0;p[at+13]=12;
        UInt8 snap[]={0xaa,0xaa,3,0,0,0,8,0,1,2,3,4};memcpy(p+at+14,snap,12);
    }
    htcWriteLE32(d+24,54);RXBatch b;
    assert(appendRXBatch(s,b,d,2048,1,7) && b.count==2);
    assert(acceptSecureRX(s,b.descriptor,b.data,b.lengths[0]));
    assert(!acceptSecureRX(s,b.descriptor,b.data,b.lengths[0]) && s.rxLastReason==ConnectionState::Replay);
    // Malformed last subframe invalidates the whole MPDU before delivery.
    b.reset();p[41]=255;assert(!appendRXBatch(s,b,d,2048,1,7) && b.failed);
    p[41]=12;htcWriteLE32(d+24,56);b.reset();assert(!appendRXBatch(s,b,d,2048,1,7));
    // RFC1042 header cannot impersonate a destination address.
    htcWriteLE32(d+24,54);UInt8 rfc[]={0xaa,0xaa,3,0,0,0};memcpy(p,rfc,6);
    b.reset();assert(!appendRXBatch(s,b,d,2048,1,7));
    // RX subframe count is independent of the TX AMSDU_LIMIT=3 experiment.
    ethernet(d,s,11,6);p=d+300;d[260]=0x80;
    for(UInt32 n=0;n<5;++n){UInt32 at=24*n;memcpy(p+at,s.local,6);memcpy(p+at+6,s.bssid,6);
        p[at+12]=0;p[at+13]=9;UInt8 snap[]={0xaa,0xaa,3,0,0,0,8,0,9};memcpy(p+at+14,snap,9);}
    htcWriteLE32(d+24,119);b.reset();assert(appendRXBatch(s,b,d,2048,1,7) && b.count==5);
    // HTT 802.3+SNAP decap uses the same bounded subframe conversion.
    htcWriteLE32(d+32,3<<8);b.reset();assert(appendRXBatch(s,b,d,2048,1,7) && b.count==5);
    p[109]=255;b.reset();assert(!appendRXBatch(s,b,d,2048,1,7));
    resetDisconnected(s);assert(s.rxReasons[ConnectionState::Aggregate] && s.rxDropped);
}
static void reorder(){
    auto s=connected();s.secure=true;s.securityPhase=5;s.ba[0].peer=7;s.ba[0].window=64;
    RXReorder q;std::vector<UInt16> received;unsigned allocations=0;
    auto clone=[&](const RXBatch &b){++allocations;return new RXBatch(b);};
    auto destroy=[&](RXBatch *p){--allocations;delete p;};
    auto release=[&](RXBatch &b){assert(acceptSecureRX(s,b.descriptor,b.data,b.lengths[0]));received.push_back(b.sequence);};
    auto send=[&](UInt16 seq,UInt64 pn,UInt64 now){auto b=batch(s,seq,pn);q.receive(s,b,now,clone,release,destroy);};
    send(10,10,0);send(12,12,1);assert(received.size()==1 && q.held==1 && s.pairPN[0]==10);
    send(12,12,2);assert(q.duplicates==1 && allocations==1);
    send(11,11,3);assert((received==std::vector<UInt16>{10,11,12}) && !q.held && !allocations);
    send(10,10,4);assert(q.late==1);
    send(15,15,5);q.poll(s,102,release,destroy);assert(received.size()==3);
    q.poll(s,105,release,destroy);assert(received.back()==15 && q.timeouts==1 && !q.held);
    send(17,17,105);s.ba[0].window=0;q.poll(s,106,release,destroy);assert(received.back()==17 && !allocations);
    q.reset(destroy);s=connected();s.ba[0].peer=7;s.ba[0].window=8;received.clear();
    auto openRelease=[&](RXBatch &b){received.push_back(b.sequence);};
    for(UInt16 seq:{UInt16(4095),UInt16(1),UInt16(0)}){auto b=batch(s,seq);q.receive(s,b,0,clone,openRelease,destroy);}
    assert((received==std::vector<UInt16>{4095,0,1}) && !allocations);
    // Explicit-paddr firmware reorder never queues into the host window.
    s.firmwareReorder=true;auto b=batch(s,50);q.receive(s,b,0,clone,openRelease,destroy);assert(!q.held && received.back()==50);
    q.reset(destroy);
    // Known ADDBA SSN: do not release a packet before the missing first sequence.
    s.firmwareReorder=false;s.ba[0].startValid=true;s.ba[0].startSeq=100;++s.ba[0].generation;
    b=batch(s,101);q.receive(s,b,0,clone,openRelease,destroy);assert(q.held==1);
    b=batch(s,100);q.receive(s,b,1,clone,openRelease,destroy);assert(received.back()==101 && !q.held);
    b=batch(s,103);q.receive(s,b,2,clone,openRelease,destroy);assert(q.held==1);
    const auto delivered=received.size();++s.ba[0].generation;
    q.poll(s,102,openRelease,destroy);assert(!q.held && received.size()==delivered && !allocations);
    q.reset(destroy);assert(!allocations);
}
static void fragments(){
    auto s=connected();s.secure=true;s.securityPhase=5;RXDefrag f;UInt8 d[2048],out[kRXAssemblyBytes];UInt32 bytes=0;
    auto input=[&](UInt8 number,bool more,UInt32 pn,const UInt8 *body,UInt32 length){
        ethernet(d,s,8,pn);htcWriteLE32(d+32,1<<8);htcWriteLE32(d+24,24+length);
        UInt8 *p=d+300;p[0]=8;p[1]=more?0x46:0x42;memcpy(p+4,s.local,6);memcpy(p+10,s.bssid,6);memcpy(p+16,s.bssid,6);
        htcWriteLE16(p+22,static_cast<UInt16>(0x80|number));memcpy(p+24,body,length);
        memcpy(d+236,p,24);d[263]=0x20;
    };
    UInt8 first[]={0xaa,0xaa,3,0,0,0},last[]={8,0,0x45};
    input(0,true,6,first,6);assert(f.receive(s,d,2048,7,0,0,out,bytes)==0);
    input(1,false,7,last,3);assert(f.receive(s,d,2048,7,0,1,out,bytes)==1 && f.completed==1);
    RXBatch b;assert(appendRXBatch(s,b,out,bytes,1,7) && b.lengths[0]==15);
    assert(acceptSecureRX(s,b.descriptor,b.data,15) && s.pairPN[16]==7);
    input(0,true,8,first,6);assert(f.receive(s,d,2048,7,0,10,out,bytes)==0);
    input(1,false,10,last,3);assert(f.receive(s,d,2048,7,0,11,out,bytes)==-1);
    input(0,true,8,first,6);assert(f.receive(s,d,2048,7,0,12,out,bytes)==0);
    f.poll(1012);assert(f.timeouts==1);
    input(1,false,9,last,3);assert(f.receive(s,d,2048,7,0,1013,out,bytes)==-1);
    // Raw CCMP format: keep header, strip IV/MIC/FCS only after successful DMA.
    input(0,false,11,first,6);htcWriteLE32(d+32,0);htcWriteLE32(d+24,53);
    UInt8 body[]={0xaa,0xaa,3,0,0,0,8,0,0x45};memcpy(d+332,body,9);
    assert(normalizeRawRX(d,2048,0,out,bytes));b.reset();assert(appendRXBatch(s,b,out,bytes,1,7));
    assert(acceptSecureRX(s,b.descriptor,b.data,b.lengths[0]));
    // A corrupt continuation must not borrow the clean first descriptor.
    input(0,true,12,first,6);assert(f.receive(s,d,2048,7,0,20,out,bytes)==0);
    input(1,false,13,last,3);htcWriteLE32(d+4,0x80010000);
    assert(f.receive(s,d,2048,7,0,21,out,bytes)==-1 && !f.entries[16].active);
    // Raw QoS+HT Control: strip four control bytes before joining fragments,
    // but retain the original descriptor layout for CCMP key/PN validation.
    auto rawControl=[&](UInt8 number,bool more,UInt32 pn,const UInt8 *body,UInt32 n){
        input(number,more,pn,body,n);UInt8 *p=d+300;p[0]=0x88;p[1]|=0x80;p[24]=p[25]=0;
        htcWriteLE32(d+32,0);htcWriteLE32(d+24,30+8+n+12);
        memcpy(d+338,body,n);memcpy(d+236,p,24);d[260]=0;d[271]=0x20;
    };
    rawControl(0,true,14,first,6);assert(f.receive(s,d,2048,7,0,22,out,bytes)==0);
    rawControl(1,false,15,last,3);assert(f.receive(s,d,2048,7,0,23,out,bytes)==1);
    b.reset();assert(appendRXBatch(s,b,out,bytes,1,7) && b.lengths[0]==15);
    assert(acceptSecureRX(s,b.descriptor,b.data,15) && s.pairPN[0]==15);
}
static void negotiation(){
    auto s=connected();s.operatingChannel=36;s.rfChains=2;s.firmwareHT=1|2|0x800;s.firmwareVHT=1<<5;
    auto &a=s.ap;a.htPresent=a.htOperation=a.wmmPresent=a.vhtPresent=a.vhtOperation=true;
    htcWriteLE16(a.ht,0x6e);a.ht[3]=a.ht[4]=255;a.htOp[0]=36;a.htOp[1]=5;
    htcWriteLE16(a.vht+4,0xfffa);htcWriteLE16(a.vht+8,0xfffa);htcWriteLE32(a.vht,1<<5);
    a.vhtOp[0]=1;a.vhtOp[1]=42;htcWriteLE16(a.vhtOp+3,0xfffc);
    negotiateHighThroughput(s);assert(s.vhtEnabled && s.width==80 && s.nss==2 && s.centerChannel==42 && s.shortGI);
    assert(s.vhtMap==0xfffa && s.htRateCount==16 && (s.localHT&12)==12);
    UInt8 wire[256];s.stage=LinkStage::Start;s.ap.beaconInterval=100;
    assert(buildConnectionWMI(wire,256,2,s)==120 && connLE32(wire+96)==5210 && (connLE32(wire+104)&63)==10);
    s.stage=LinkStage::PeerAssoc;s.wmmConfigured=true;assert(buildConnectionWMI(wire,256,2,s));
    assert(connLE32(wire+64)==2 && (connLE32(wire+36)&0x04002000)==0x04002000);
    a.vhtOp[1]=58;negotiateHighThroughput(s);assert(s.width==40 && s.centerChannel==38); // DFS segment rejected
    a.vhtOp[0]=0;a.htOp[1]=0;negotiateHighThroughput(s);assert(s.width==20 && s.vhtMap==0xfffa && peerRateSet(s,2,20).rx==0xfff5);
    s.rfChains=1;negotiateHighThroughput(s);assert(s.nss==1 && s.vhtMap==0xfffe && peerRateSet(s,1,20).rx==0xfffd);
    s.firmwareHT=1;s.firmwareVHT=0;negotiateHighThroughput(s);assert(!s.shortGI);
    // HT40 follows a valid AP secondary channel in the same allowed band.
    s.operatingChannel=6;a.htOp[1]=5;negotiateHighThroughput(s);assert(s.htEnabled && !s.vhtEnabled && s.width==40 && s.centerChannel==8);
    s.operatingChannel=13;negotiateHighThroughput(s);assert(s.width==20);
    a.htOp[8]=1;negotiateHighThroughput(s);assert(!s.htEnabled);
}
static void fragmentIndication(){
    UInt8 f[24]{};f[0]=1;f[2]=16;f[8]=0xa;f[10]=7;f[16]=1;
    RxSetupState rx;rx.ringOwned=true;HTCCreditState credits;
    assert(!parseHTTSetup(f,24,1,credits,rx));assert(rx.pendingMPDUs==1 && rx.fragIndications==1 && rx.completionFragment[0]);
    auto before=rx.pendingMPDUs;f[16]=5;assert(parseHTTSetup(f,24,1,credits,rx) && rx.pendingMPDUs==before);
    f[8]=0x10;f[16]=0;f[2]=8;assert(!parseHTTSetup(f,16,1,credits,rx) && rx.pendingMPDUs==before && rx.pnIndications==1);
    f[8]=0x11;assert(parseHTTSetup(f,16,1,credits,rx));
}
int main(){aggregate();reorder();fragments();negotiation();fragmentIndication();}
