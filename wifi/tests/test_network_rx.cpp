#include <cassert>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <vector>
#include "Ath10kRX.hpp"
#include "Ath10kPerformance.hpp"
using namespace Ath10k;
static UInt64 nowMs=0;
static void clock_get_uptime(uint64_t *p){*p=nowMs*1000000;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *p){*p=t;}
namespace Ath10k {UInt64 uptimeNS(){return nowMs*1000000u;}}
static void *IOMalloc(size_t n){return malloc(n);}
static void IOFree(void *p,size_t){free(p);}
struct Packet {std::vector<UInt8> bytes;explicit Packet(size_t n):bytes(n){}};
using mbuf_t=Packet *;
static constexpr int MBUF_DONTWAIT=0;
static int mbuf_copyback(mbuf_t p,size_t at,size_t bytes,const void *data,int){
    assert(at+bytes<=p->bytes.size());memcpy(p->bytes.data()+at,data,bytes);return 0;
}
class Ath10kPCIDevice {
public:
    ConnectionState _connection{};RXBatch _rxBatch{};RXReorder _rxReorder{};RXDefrag _rxDefrag{};
    UInt8 _rxNormalized[kRXAssemblyBytes]{};
    bool _networkEnabled=true,_networkLinkUp=true,failAllocation=false;
    bool _rxDeliveryBatch=false;UInt32 _rxDeliveryQueued=0;
    PerformanceState _perf{};
    mbuf_t takeRXPacket(UInt32 bytes){return allocatePacket(bytes);}
    void flushNetworkRX();
    struct Interface {
        static constexpr UInt32 kInputOptionQueuePacket=1;
        std::vector<std::vector<UInt8>> queued;
        UInt32 flushes=0;
        std::vector<std::vector<UInt8>> frames;
        void inputPacket(mbuf_t packet,UInt32 bytes,UInt32 options=0){assert(bytes==packet->bytes.size());
            (options?queued:frames).push_back(packet->bytes);delete packet;}
        void flushInputQueue(){frames.insert(frames.end(),queued.begin(),queued.end());queued.clear();++flushes;}
    } interface;
    Interface *_networkInterface=&interface;
    mbuf_t allocatePacket(UInt32 bytes){return failAllocation?nullptr:new Packet(bytes);}
    void freePacket(mbuf_t p){delete p;}
    template<class... T>void setProperty(T...){}
    void deliverNetworkRX(const UInt8 *,UInt8,UInt16,UInt16,UInt32);
    void finishNetworkRX();void releaseRXBatch(RXBatch &);void serviceRXReorder(UInt64);
    ~Ath10kPCIDevice(){_rxReorder.reset([](RXBatch *b){IOFree(b,sizeof(*b));});}
};
using IONetworkInterface=Ath10kPCIDevice::Interface;
#include "network_rx_methods.inc"
static void descriptor(UInt8 *d,const ConnectionState &s,UInt16 seq,UInt64 pn,bool encrypted=true){
    memset(d,0,2048);htcWriteLE32(d+4,0x80000000);htcWriteLE32(d+32,2<<8);htcWriteLE32(d+24,18);htcWriteLE32(d+56,0xc000);
    UInt8 *h=d+236;h[0]=0x88;h[1]=encrypted?0x42:2;memcpy(h+4,s.local,6);memcpy(h+10,s.bssid,6);
    htcWriteLE16(h+22,static_cast<UInt16>(seq<<4));h[31]=0x20;
    if(encrypted){htcWriteLE32(d+12,(6u<<28)|(1u<<13));htcWriteLE32(d+16,static_cast<UInt32>(pn));}
    memcpy(d+300,s.local,6);memcpy(d+306,s.bssid,6);d[312]=8;d[314]=0x45;
}
static void aggregate(UInt8 *d,const ConnectionState &s,bool invalid=false,bool plaintext=false){
    descriptor(d,s,2,6,!plaintext);d[260]=0x80;UInt8 *p=d+300;
    for(UInt32 n=0;n<5;++n){UInt32 at=24*n;memcpy(p+at,s.local,6);memcpy(p+at+6,s.bssid,6);
        p[at+12]=0;p[at+13]=9;UInt8 snap[]={0xaa,0xaa,3,0,0,0,8,0,9};memcpy(p+at+14,snap,9);}
    htcWriteLE32(d+24,119);if(invalid)p[109]=255;
    if(plaintext){p[20]=0x88;p[21]=0x8e;} // first EAPOL must not authorize remaining plaintext data
}
static void nativeMSDU(UInt8 *d,const ConnectionState &s,bool first,UInt64 pn=30){
    descriptor(d,s,30,pn);htcWriteLE32(d+32,1u<<8);htcWriteLE32(d+24,33);
    htcWriteLE32(d+56,first?0x4000:0x8000);
    UInt8 *p=d+300;memset(p,0,33);htcWriteLE16(p,0x4208);
    memcpy(p+4,s.local,6);memcpy(p+10,s.bssid,6);memcpy(p+16,s.bssid,6);
    htcWriteLE16(p+22,30u<<4);
    const UInt8 snap[]={0xaa,0xaa,3,0,0,0,8,0,0x45};memcpy(p+24,snap,9);
    if(first)d[260]=0x80; // original QoS header indicates A-MSDU
    else {
        // FIRST-only MPDU words and original header are undefined here.
        memset(d+12,0,12);memset(d+236,0x5f,64);
    }
}
static void firstOnlyMetadata(){
    // Match the 0.0.69 evidence: Native WiFi LAST with zero MPDU info and an
    // original-header area that is not another complete 802.11 header.
    const UInt32 undefinedInfo[]={0,(7u<<28)|(1u<<13),6u<<28,0xffffffffu};
    for(UInt32 info:undefinedInfo){
        Ath10kPCIDevice d;auto &s=d._connection;s.stage=LinkStage::Connected;
        s.secure=s.portAuthorized=s.firmwareReorder=true;s.securityPhase=5;
        s.peerId=7;s.peerCount=1;s.peerIDs[0]=1u<<7;s.local[0]=s.bssid[0]=2;s.local[5]=1;s.bssid[5]=2;
        UInt8 first[2048],last[2048];nativeMSDU(first,s,true);nativeMSDU(last,s,false);
        htcWriteLE32(last+12,info);htcWriteLE32(last+16,99);htcWriteLE16(last+20,0xffff);
        auto feed=[&](){d._rxBatch.reset();d.deliverNetworkRX(first,1,7,0,2048);
            d.deliverNetworkRX(last,1,7,0,2048);d.finishNetworkRX();};
        d.deliverNetworkRX(first,1,7,0,2048);d.deliverNetworkRX(last,1,7,0,2048);
        assert(d.interface.frames.empty() && !s.pairPN[0] && d._rxBatch.count==2 && !d._rxBatch.failed);
        d.finishNetworkRX();assert(d.interface.frames.size()==2 && s.pairPN[0]==30 && s.rxAmsduSubframes==2);
        assert(!s.rxDropped && !s.rxSecurity.valid);
        // The same complete MPDU is still a replay, regardless of LAST PN.
        feed();assert(d.interface.frames.size()==2 && s.rxReasons[ConnectionState::Replay]==1 && s.pairPN[0]==30);
        nativeMSDU(first,s,true,31);
        // A corrupt LAST must invalidate the entire batch, not leak FIRST.
        htcWriteLE32(last+4,0xa0000000u);feed();
        assert(d.interface.frames.size()==2 && s.rxReasons[ConnectionState::Integrity]==1 && s.pairPN[0]==30);
        htcWriteLE32(last+4,0xc0000000u);feed();
        assert(d.interface.frames.size()==2 && s.rxReasons[ConnectionState::Integrity]==2 && s.pairPN[0]==30);
        htcWriteLE32(last+4,0);feed();
        assert(d.interface.frames.size()==2 && s.rxReasons[ConnectionState::Integrity]==3 && s.pairPN[0]==30);
        htcWriteLE32(last+4,0x80000000u);
        // Bad key/cipher/original transmitter on FIRST remains rejected.
        first[267]=0x60;feed();assert(d.interface.frames.size()==2 && s.rxReasons[ConnectionState::Key]==1);
        first[267]=0x20;htcWriteLE32(first+12,(7u<<28)|(1u<<13));feed();
        assert(d.interface.frames.size()==2 && s.rxReasons[ConnectionState::Key]==2);
        htcWriteLE32(first+12,(6u<<28)|(1u<<13));first[246]^=4;feed();
        assert(d.interface.frames.size()==2 && s.rxSecurity.last==RXSecurityFault::Transmitter && s.pairPN[0]==30);
        first[246]^=4;
        d._rxBatch.reset();d.deliverNetworkRX(first,1,7,0,2048);d.deliverNetworkRX(last,1,8,0,2048);d.finishNetworkRX();
        assert(d.interface.frames.size()==2 && s.rxReasons[ConnectionState::Peer]==1 && s.pairPN[0]==30);
        // First plaintext or unauthorized data cannot borrow encrypted LAST.
        htcWriteLE32(first+12,0);feed();assert(d.interface.frames.size()==2 && s.rxReasons[ConnectionState::Plaintext]==1);
        htcWriteLE32(first+12,(6u<<28)|(1u<<13));s.portAuthorized=false;feed();
        assert(d.interface.frames.size()==2 && s.rxReasons[ConnectionState::Port]==1 && s.pairPN[0]==30);
    }
}
int main(){
    firstOnlyMetadata();
    {
        Ath10kPCIDevice batch;auto &s=batch._connection;s.stage=LinkStage::Connected;
        s.portAuthorized=s.firmwareReorder=true;s.local[0]=s.bssid[0]=2;s.peerCount=1;s.peerIDs[0]=1u<<7;
        UInt8 frame[2048];batch._rxDeliveryBatch=true;
        for(UInt32 i=0;i<65;++i){descriptor(frame,s,i,i,false);batch._rxBatch.reset();
            batch.deliverNetworkRX(frame,1,7,0,2048);batch.finishNetworkRX();}
        assert(batch.interface.frames.size()==64 && batch.interface.queued.size()==1 && batch._perf.rxFlushMax==64);
        batch.flushNetworkRX();assert(batch.interface.frames.size()==65 && batch.interface.queued.empty() && batch.interface.flushes==2);
        batch.flushNetworkRX();assert(batch.interface.flushes==2);
        aggregate(frame,s,true);batch._rxBatch.reset();batch.deliverNetworkRX(frame,1,7,0,2048);batch.finishNetworkRX();
        batch.flushNetworkRX();assert(batch.interface.frames.size()==65);
    }
    Ath10kPCIDevice d;auto &s=d._connection;s.stage=LinkStage::Connected;s.secure=true;s.securityPhase=5;s.groupCipher=AthSecurity::cipherCCMP;s.portAuthorized=true;
    s.peerId=7;s.peerCount=1;s.peerIDs[0]=1u<<7;s.local[0]=s.bssid[0]=2;s.local[5]=1;s.bssid[5]=2;
    s.firmwareReorder=true;UInt8 desc[2048];
    auto feed=[&](){d._rxBatch.reset();d.deliverNetworkRX(desc,1,7,0,2048);d.finishNetworkRX();};
    descriptor(desc,s,1,5);feed();assert(d.interface.frames.size()==1 && s.pairPN[0]==5);
    feed();assert(d.interface.frames.size()==1 && s.rxReasons[ConnectionState::Replay]==1);
    aggregate(desc,s,true);feed();assert(d.interface.frames.size()==1 && s.pairPN[0]==5);
    aggregate(desc,s);feed();assert(d.interface.frames.size()==6 && s.pairPN[0]==6 && s.rxAmsduSubframes==5);
    aggregate(desc,s,false,true);feed();assert(d.interface.frames.size()==6 && s.rxReasons[ConnectionState::Plaintext]==1);
    // Loss classification and secure EAPOL receive queue.
    descriptor(desc,s,3,7,false);desc[312]=0x88;desc[313]=0x8e;feed();assert(s.eapolCount==1 && d.interface.frames.size()==6);
    descriptor(desc,s,4,7);d.failAllocation=true;feed();assert(s.rxReasons[ConnectionState::Allocation]==1 && d.interface.frames.size()==6);
    d.failAllocation=false;
    // The production methods retain pending packets, and enforce PN after reorder.
    s.firmwareReorder=false;s.ba[0].peer=7;s.ba[0].tid=0;s.ba[0].window=64;
    descriptor(desc,s,10,10);feed();assert(d.interface.frames.size()==7);
    descriptor(desc,s,12,12);nowMs=1;feed();assert(d.interface.frames.size()==7 && d._rxReorder.held==1 && s.pairPN[0]==10);
    descriptor(desc,s,11,11);nowMs=2;feed();assert(d.interface.frames.size()==9 && !d._rxReorder.held && s.pairPN[0]==12);
    descriptor(desc,s,15,15);nowMs=1000;feed();d.serviceRXReorder(1099);assert(d._rxReorder.held==1);
    d.serviceRXReorder(1100);assert(!d._rxReorder.held && d.interface.frames.size()==10);
    // Group RSC and key index apply to outer receiver address.
    s.firmwareReorder=true;s.groupIndex=1;s.groupRSC[0]=20;
    descriptor(desc,s,20,20);desc[240]=desc[300]=0xff;desc[267]=0x60;feed();assert(s.rxLastReason==ConnectionState::Replay);
    descriptor(desc,s,21,21);desc[240]=desc[300]=0xff;desc[267]=0x60;feed();assert(s.groupPN[0]==21 && d.interface.frames.size()==11);
    UInt8 shortDescriptor[33]{};d._rxBatch.reset();d.deliverNetworkRX(shortDescriptor,1,7,0,sizeof(shortDescriptor));
    d.finishNetworkRX();assert(s.rxReasons[ConnectionState::Length]==1 && d.interface.frames.size()==11);
    d._rxBatch.reset();d.deliverNetworkRX(nullptr,1,7,0,0);d.finishNetworkRX();
    assert(s.rxReasons[ConnectionState::Length]==2 && d.interface.frames.size()==11);
    // Native payload remains valid while the original header is absent.
    // Keep rejecting it and retain both layouts so hardware can identify why.
    descriptor(desc,s,30,30);htcWriteLE32(desc+32,1<<8);htcWriteLE32(desc+24,33);
    UInt8 *native=desc+300;memset(native,0,33);htcWriteLE16(native,0x4208);
    memcpy(native+4,s.local,6);memcpy(native+10,s.bssid,6);memcpy(native+16,s.bssid,6);
    const UInt8 snap[]={0xaa,0xaa,3,0,0,0,8,0,0x45};memcpy(native+24,snap,9);
    memset(desc+236,0,26);const UInt64 previous=s.pairPN[0];feed();
    assert(d.interface.frames.size()==11 && s.rxReasons[ConnectionState::Security]==1 && s.pairPN[0]==previous);
    assert(s.rxSecurity.last==RXSecurityFault::HeaderType && s.rxSecurity.nativeValid && s.rxSecurity.msduCount==1);
    assert(s.rxSecurity.peer==7 && s.rxSecurity.originalHeader[0]==0 && s.rxSecurity.nativeHeader[0]==8);
}
