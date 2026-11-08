// Native front end TX path, end to end on the host (hardware run 7: join and RX
// fine, `netstat` Opkts 0, the core saw only the two EAPOL frames).
//
//   stack (DHCP/ARP/DNS/bulk) -> IONetworkInterface pull model (model of
//   IONetworkingFamily 3.4 as read from the 26.7.2 collection) -> the front
//   end's outputStart and output-thread control (AirportAth10kTx.hpp, the code
//   AirportAth10k.cpp runs) -> IOGatedOutputQueue (model of IOBasicOutputQueue)
//   -> Ath10kPCIDevice::outputPacket (production, extracted) -> submitDataPacket,
//   HTT TX_FRM descriptor, CE4 (production) -> simulated firmware: CE consume,
//   HTT TX completion -> serviceDataTX (production): slot release, wake,
//   serviceTxQueue -> signalOutputThread -> outputStart again.
//
// Not covered here (and why): the vtable dispatch to outputStart (slot 325,
// checked by kc-audit against the real collection) and the RX side, which
// run 7 showed working on hardware (727 frames, IPv6 RA) and which
// test_network_rx/test_rx_pipeline cover with the production RX methods.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <deque>
#include <vector>
#include <string>

// --- mbuf model ----------------------------------------------------------------
struct MockMbuf { std::vector<uint8_t> data; MockMbuf *next=nullptr; };
using mbuf_t=MockMbuf *;
static long liveMbufs=0;
static mbuf_t newPacket(const std::vector<uint8_t> &bytes){++liveMbufs;auto *m=new MockMbuf;m->data=bytes;return m;}
static void mbuf_freem(mbuf_t m){if(!m)return;--liveMbufs;delete m;}
static size_t mbuf_pkthdr_len(mbuf_t m){return m->data.size();}
static int mbuf_copydata(mbuf_t m,size_t offset,size_t length,void *out){
    if(offset+length>m->data.size())return 22;
    memcpy(out,m->data.data()+offset,length);return 0;}

#define ATH10K_AP_MAINTENANCE_TEST
#define ATH10K_NATIVE_TX_MEMBERS \
    UInt32 outputPacket(mbuf_t,void *); \
    void freePacket(mbuf_t m){mbuf_freem(m);} \
    bool &_networkLinkUp=networkUp;
#include "test_connection.cpp"
enum : UInt32 { kIOReturnOutputSuccess=0x00,kIOReturnOutputStall=0x01,kIOReturnOutputDropped=0x02 };
#include "network_output_method.inc"
#include "../native/src/AirportAth10kTx.hpp"

// --- IONetworkInterface output pull model (IONetworkingFamily 3.4, 25G332) ------
// State word as IONetworkInterface keeps it (_reserved->txThreadState):
// 0x001 never run, 0x100 stop requested, 0x800 halted, 0x1000 flush pending.
// init: 0x801. The drain loop, the signal word and the halt/start rules follow
// drainOutputQueue, signalOutputThread, startOutputThread, haltOutputThread and
// if_start_precheck in the collection (see AirportAth10kTx.hpp).
struct PullModelInterface {
    std::deque<mbuf_t> sendq;          // the ifnet send queue (dlil enqueues here)
    uint32_t state=0x801,signalWord=0;
    uint64_t opackets=0,obytes=0;      // ifnet_stat_increment_out in dequeueOutputPackets
    unsigned starts=0,drains=0,driverCalls=0;
    bool kicked=false,inDrain=false;
    std::function<int()> outputStart;  // IONetworkController::outputStart (slot 325)
    // dlil: the stack hands a packet to a TXSTART interface.
    void enqueue(mbuf_t m){sendq.push_back(m);ifnetStart();}
    void ifnetStart(){++starts;kicked=true;run();}
    // The dlil start thread; runs if_start until nothing kicks it again.
    void run(){
        if(inDrain)return; // the start thread is already running: it sees the kick
        while(kicked){kicked=false;inDrain=true;ifStart();inDrain=false;}
    }
    void ifStart(){
        state&=~0x1u;                                   // first start
        if(state&0x300u){state=(state&~0x900u)|0x800u;} // halt request acknowledged
        if(state&0x800u){if(state&0x1000u){purge();state&=~0x1000u;}return;}
        ++drains;
        while(!sendq.empty()){
            signalWord=0;++driverCalls;
            const int r=outputStart();
            if(r==AirportTx::kSuccess)continue;
            if(r==AirportTx::kNoResources){if(signalWord==0){signalWord=1;break;}continue;}
            break;
        }
    }
    // Driver side SPI.
    mbuf_t dequeueOutputPacket(){
        if(sendq.empty())return nullptr;
        mbuf_t m=sendq.front();sendq.pop_front();++opackets;obytes+=m->data.size();return m;
    }
    void signalOutputThread(){
        const uint32_t old=signalWord;signalWord=2;
        if(state&0x201u)return;
        if(old==1)ifnetStart();
    }
    void startOutputThread(){
        const uint32_t before=state;state&=~0x1200u;
        if(before&0x900u){state&=~0x1b00u;if(!(state&1u))ifnetStart();}
    }
    void stopOutputThread(){state|=0x800u;}           // inside the TX work loop's gate: halts at once
    void flushOutputQueue(){purge();}
    void purge(){for(auto *m:sendq)mbuf_freem(m);sendq.clear();}
    bool halted() const {return (state&0x800u)!=0;}
};

// --- IOGatedOutputQueue (IOBasicOutputQueue semantics) ---------------------------
struct GatedQueue {
    std::deque<mbuf_t> q;UInt32 cap=Ath10k::kOutputQueueCapacity;bool started=false,stalled=false,inOutput=false;
    unsigned drops=0,stalls=0,services=0;
    std::function<UInt32(mbuf_t)> output; // Ath10kPCIDevice::outputPacket under the core's gate
    UInt32 getSize() const {return UInt32(q.size());}
    UInt32 getCapacity() const {return cap;}
    void enqueue(mbuf_t m){if(q.size()>=cap){++drops;mbuf_freem(m);return;}q.push_back(m);pump();}
    void pump(){
        if(!started || stalled || inOutput)return;
        inOutput=true;
        while(!q.empty() && started){
            mbuf_t m=q.front();
            const UInt32 r=output(m);
            if(r==kIOReturnOutputStall){stalled=true;++stalls;break;} // the queue keeps the packet
            q.pop_front();                                         // success/drop: the driver freed it
        }
        inOutput=false;
    }
    void service(){++services;stalled=false;pump();}
    void start(){started=true;stalled=false;pump();}
    void stop(){started=false;}
    void flush(){for(auto *m:q)mbuf_freem(m);q.clear();}
};

// --- The simulated air: what the firmware transmits ------------------------------
struct AirFrame { std::vector<uint8_t> ether; uint8_t flags0=0; uint16_t flags1=0,txId=0; };

static std::vector<uint8_t> ethernet(const uint8_t dst[6],const uint8_t src[6],uint16_t type,const std::vector<uint8_t> &payload){
    std::vector<uint8_t> f(14);memcpy(f.data(),dst,6);memcpy(f.data()+6,src,6);f[12]=uint8_t(type>>8);f[13]=uint8_t(type);
    f.insert(f.end(),payload.begin(),payload.end());return f;
}
static uint16_t be16(const uint8_t *p){return uint16_t((p[0]<<8)|p[1]);}
static std::vector<uint8_t> ipv4(uint8_t proto,uint32_t src,uint32_t dst,const std::vector<uint8_t> &l4,uint8_t tos=0){
    std::vector<uint8_t> h(20);h[0]=0x45;h[1]=tos;const uint16_t total=uint16_t(20+l4.size());h[2]=uint8_t(total>>8);h[3]=uint8_t(total);
    h[8]=64;h[9]=proto;for(int i=0;i<4;++i){h[12+i]=uint8_t(src>>(24-8*i));h[16+i]=uint8_t(dst>>(24-8*i));}
    uint32_t sum=0;for(int i=0;i<20;i+=2)sum+=be16(&h[i]);while(sum>>16)sum=(sum&0xffff)+(sum>>16);sum=~sum&0xffff;h[10]=uint8_t(sum>>8);h[11]=uint8_t(sum);
    h.insert(h.end(),l4.begin(),l4.end());return h;
}
static std::vector<uint8_t> udp(uint16_t sport,uint16_t dport,const std::vector<uint8_t> &payload){
    std::vector<uint8_t> u(8);u[0]=uint8_t(sport>>8);u[1]=uint8_t(sport);u[2]=uint8_t(dport>>8);u[3]=uint8_t(dport);
    const uint16_t len=uint16_t(8+payload.size());u[4]=uint8_t(len>>8);u[5]=uint8_t(len);u.insert(u.end(),payload.begin(),payload.end());return u;
}
// BOOTP/DHCP message (RFC 2131) with the message-type option and, for REQUEST, the requested address.
static std::vector<uint8_t> dhcp(uint8_t op,uint8_t type,uint32_t xid,const uint8_t chaddr[6],uint32_t yiaddr=0,uint32_t requested=0){
    std::vector<uint8_t> d(240);d[0]=op;d[1]=1;d[2]=6;for(int i=0;i<4;++i){d[4+i]=uint8_t(xid>>(24-8*i));d[16+i]=uint8_t(yiaddr>>(24-8*i));}
    d[10]=0x80; // broadcast flag, as macOS sets it before it has an address
    memcpy(d.data()+28,chaddr,6);d[236]=99;d[237]=130;d[238]=83;d[239]=99;
    d.insert(d.end(),{53,1,type});
    if(requested)d.insert(d.end(),{50,4,uint8_t(requested>>24),uint8_t(requested>>16),uint8_t(requested>>8),uint8_t(requested)});
    d.push_back(255);return d;
}
static std::vector<uint8_t> arp(uint16_t op,const uint8_t sha[6],uint32_t spa,const uint8_t tha[6],uint32_t tpa){
    std::vector<uint8_t> a={0,1,8,0,6,4,0,uint8_t(op)};a.insert(a.end(),sha,sha+6);
    for(int i=0;i<4;++i)a.push_back(uint8_t(spa>>(24-8*i)));
    a.insert(a.end(),tha,tha+6);
    for(int i=0;i<4;++i)a.push_back(uint8_t(tpa>>(24-8*i)));
    return a;
}
static std::vector<uint8_t> dnsQuery(uint16_t id,const char *name){
    std::vector<uint8_t> q={uint8_t(id>>8),uint8_t(id),1,0,0,1,0,0,0,0,0,0};
    for(const char *p=name;*p;){const char *dot=strchr(p,'.');size_t n=dot?size_t(dot-p):strlen(p);q.push_back(uint8_t(n));q.insert(q.end(),p,p+n);p+=n;if(*p=='.')++p;}
    q.insert(q.end(),{0,0,1,0,1});return q;
}

static const uint8_t kBroadcast[6]={0xff,0xff,0xff,0xff,0xff,0xff},kZero[6]={0,0,0,0,0,0};

class NativeTxRig {
public:
    ConnectionDriver d;PullModelInterface iface;GatedQueue gated;
    AirportTx::OutputThread thread;AirportTx::PullStats stats;
    std::vector<AirFrame> air;std::vector<UInt16> pendingIds;
    UInt64 now=1900;bool linked=false;
    const uint8_t *mac;uint8_t router[6]={2,6,7,8,9,10};
    explicit NativeTxRig(bool wpa2=true){
        d.connect();assert(d.networkUp && d._connection.stage==LinkStage::Connected);
        auto &s=d._connection;mac=s.local; // the station address of the join (Private Wi-Fi Address in run 7)
        if(wpa2){s.secure=true;s.securityPhase=5;s.portAuthorized=true;}
        s.ap.wmmPresent=true;for(unsigned ac=0;ac<4;++ac)s.ap.wmm[4*ac]=uint8_t(ac); // WMM, no ACM
        for(unsigned i=0;i<Ath10k::kDataTxSlots;++i){
            auto &b=d._dataBuffers[i];b.payload.isAllocated=b.command.isAllocated=b.fragments.isAllocated=false;
            b.payload.iovm=0x2000000+i*0x10000;b.command.iovm=b.payload.iovm+0x1000;b.fragments.iovm=b.payload.iovm+0x2000;
        }
        // Firmware: CE4 consumes each descriptor; the HTT TX completion comes back later (complete()).
        d._regs.tx4Consume=[this](){
            const auto &s=d._connection;int slot=Ath10k::dataTxSlotForID(s,s.txId);assert(slot>=0);
            const UInt8 *f=static_cast<const UInt8 *>(d._dataBuffers[slot].command.virtualAddress());
            const UInt8 *payload=static_cast<const UInt8 *>(d._dataBuffers[slot].payload.virtualAddress());
            AirFrame a;a.flags0=f[9];a.flags1=Ath10k::htcReadLE16(f+10);a.txId=Ath10k::htcReadLE16(f+14);
            assert(f[8]==1 && a.txId==s.txId && Ath10k::htcReadLE16(f+12)==s.dataTX[slot].bytes);
            assert(Ath10k::wmiReadLE32(f+16)==d._dataBuffers[slot].fragments.dmaAddress());
            const UInt8 *frags=static_cast<const UInt8 *>(d._dataBuffers[slot].fragments.virtualAddress());
            assert(Ath10k::wmiReadLE32(frags)==d._dataBuffers[slot].payload.dmaAddress() && Ath10k::wmiReadLE32(frags+4)==s.dataTX[slot].bytes);
            a.ether.assign(payload,payload+s.dataTX[slot].bytes);
            assert(!memcmp(f+24,payload,a.ether.size()<50?a.ether.size():50)); // HTT prefetch = frame head
            air.push_back(a);pendingIds.push_back(a.txId);
        };
        d.queueEnabled=true;
        // Production serviceTxQueue: q->service(kServiceAsync), then signalOutputThread
        // on the native interface while the core's link is up.
        d.queue.hook=[this](){gated.service();if(d._networkLinkUp)iface.signalOutputThread();};
        gated.output=[this](mbuf_t m){return d.outputPacket(m,nullptr);};
        gated.start(); // Ath10kPCIDevice::networkLink(true) starts the gated queue
        iface.outputStart=[this](){
            struct Ops {
                NativeTxRig *r;
                bool ready() const {return true;}
                UInt32 size() const {return r->gated.getSize();}
                UInt32 capacity() const {return r->gated.getCapacity();}
                mbuf_t dequeue() const {return r->iface.dequeueOutputPacket();}
                void enqueue(mbuf_t m) const {r->gated.enqueue(m);}
            } ops{this};
            return AirportTx::pull(ops,stats);
        };
    }
    // AirportAth10k::nativeLink -> setOutputThread (session data link follows the authorized port).
    void link(bool up){
        struct Ops {PullModelInterface &i;void start(){i.startOutputThread();}void stop(){i.stopOutputThread();}void flush(){i.flushOutputQueue();}} ops{iface};
        thread.link(ops,up);linked=up;
    }
    void send(const std::vector<uint8_t> &frame){iface.enqueue(newPacket(frame));}
    // The AP acknowledges what was transmitted: HTT TX completion indication(s), then the runtime tick.
    void complete(size_t max=~size_t(0)){
        std::vector<UInt16> ids;
        while(!pendingIds.empty() && ids.size()<max && ids.size()<Ath10k::kDataTxSlots){ids.push_back(pendingIds.front());pendingIds.erase(pendingIds.begin());}
        if(!ids.empty()){
            std::vector<UInt8> p((12+ids.size()*2+3)&~size_t(3));p[0]=1;p[8]=7;p[10]=UInt8(ids.size());
            Ath10k::htcWriteLE16(p.data()+2,UInt16(p.size()-8));
            for(size_t i=0;i<ids.size();++i)Ath10k::htcWriteLE16(p.data()+12+i*2,ids[i]);
            d.deliver(1,p);
        }
        now+=10;d.tick(now);assert(!d.serviceDataTX(now));
    }
    void drainAll(){for(unsigned guard=0;guard<1000 && (!pendingIds.empty() || !gated.q.empty() || !iface.sendq.empty());++guard)complete();}
};

static uint8_t tidOf(const AirFrame &a){return uint8_t((a.flags1>>6)&31);}

// 1. Run 7 reproduced: without startOutputThread nothing leaves the interface.
static void outputThreadNeverStarted(){
    NativeTxRig r;
    for(int i=0;i<20;++i)r.send(ethernet(kBroadcast,r.mac,0x0806,arp(1,r.mac,0,kZero,0xc0a80101)));
    assert(r.iface.halted() && r.iface.opackets==0 && r.iface.driverCalls==0 && r.air.empty());
    assert(r.d._connection.txPackets==0 && r.stats.calls==0 && r.iface.sendq.size()==20);
    r.iface.purge();r.d.releaseWMIRuntime();
    puts("  run 7 reproduced: output thread halted from init, Opkts 0, core TX 0");
}

// 2. The fix: link-up starts the thread; N frames reach HTT TX, complete, Opkts=N.
static void framesReachHTT(){
    NativeTxRig r;r.link(true);assert(!r.iface.halted() && r.thread.started());
    const unsigned N=700; // more than the gated queue (256) + the TX window (64): NoResources and signalOutputThread run
    for(unsigned i=0;i<N;++i){
        auto payload=udp(uint16_t(50000+i),9,std::vector<uint8_t>(100+i%900,uint8_t(i)));
        r.send(ethernet(r.router,r.mac,0x0800,ipv4(17,0xc0a80119,0xc0a80101,payload)));
        if(i%7==0)r.complete(3); // the air is slower than the stack: slots fill, the queue stalls, then resumes
    }
    r.drainAll();
    const auto &s=r.d._connection;
    assert(r.air.size()==N && r.iface.opackets==N && s.txPackets==N && !s.dataPending && !s.txErrors && !s.txLinkDropped);
    assert(r.gated.q.empty() && r.iface.sendq.empty() && !r.d._dataTXStalled && r.iface.signalWord!=1);
    for(unsigned i=0;i<N;++i){
        const auto &a=r.air[i];
        assert(a.flags0==0x42);         // HTT TX_FRM: Ethernet (802.3) frame, encrypted by firmware (WPA2 port open)
        assert(tidOf(a)==0);            // best effort (DSCP 0) on a WMM AP
        assert(!memcmp(a.ether.data()+6,r.mac,6) && be16(a.ether.data()+12)==0x0800);
        assert(be16(a.ether.data()+14+20)==uint16_t(50000+i)); // order kept
    }
    assert(r.gated.stalls>0 && r.d._perf.txWakeups>0 && r.stats.full>0); // flow control was exercised
    assert(liveMbufs==0);
    r.d.releaseWMIRuntime();
    printf("  %u frames: outputStart -> gated queue -> outputPacket -> HTT TX -> completion; %u stalls, %u wakes, Opkts=%llu\n",
           N,r.gated.stalls,r.d._perf.txWakeups,(unsigned long long)r.iface.opackets);
}

// 3. Pause/resume accounting returns to zero; link-down halts and flushes.
static void pauseResumeAndLinkDown(){
    NativeTxRig r;r.link(true);
    auto &s=r.d._connection;
    s.txPauseMask=1u<<1; // firmware TX pause (MCC), vdev 0
    for(int i=0;i<10;++i)r.send(ethernet(r.router,r.mac,0x0800,ipv4(17,0xc0a80119,0xc0a80101,udp(1,2,{1,2,3}))));
    assert(r.air.empty() && r.gated.stalls==1 && r.gated.q.size()==10 && s.stallFirmwarePause==1 && r.d._dataTXStalled);
    // WAKE: txWakePending -> serviceTxQueue (Ath10kNetwork.cpp, the same path the WMI pause event takes).
    s.txPauseMask=0;s.txWakePending=true;r.complete();
    r.drainAll();
    assert(r.air.size()==10 && r.iface.opackets==10 && !s.dataPending && r.gated.q.empty() && !r.d._dataTXStalled);
    // Link down (leave, link loss, sleep): thread halted and the send queue flushed.
    r.link(false);r.d.networkLink(false);r.gated.stop();r.gated.flush();
    for(int i=0;i<5;++i)r.send(ethernet(r.router,r.mac,0x0800,ipv4(17,1,2,udp(1,2,{}))));
    assert(r.iface.halted() && r.iface.opackets==10 && r.air.size()==10);
    r.link(false);assert(r.thread.stops==1); // idempotent
    // Rejoin: link up again, the queued frames flow.
    r.d.networkLink(true);r.gated.start();r.link(true);r.drainAll();
    assert(r.iface.opackets==15 && r.air.size()==15 && r.thread.starts==2 && liveMbufs==0);
    r.d.releaseWMIRuntime();
    puts("  firmware TX pause stalls and resumes to 0 pending; link-down halts + flushes; rejoin restarts");
}

// 4. DHCP, ARP and DNS after the port opens (what macOS sends first on en1).
static void dhcpArpDns(){
    NativeTxRig r;r.link(true);
    const uint32_t xid=0x3903f326,offered=0xc0a80119,server=0xc0a80101;
    // DISCOVER: 0.0.0.0 -> 255.255.255.255, broadcast frame from the station MAC.
    r.send(ethernet(kBroadcast,r.mac,0x0800,ipv4(17,0,0xffffffff,udp(68,67,dhcp(1,1,xid,r.mac)))));
    r.drainAll();assert(r.air.size()==1);
    {const auto &a=r.air[0];assert(!memcmp(a.ether.data(),kBroadcast,6) && be16(a.ether.data()+12)==0x0800 && a.ether[14+9]==17);
     assert(be16(a.ether.data()+14+20+2)==67 && a.ether[14+28+240+2]==1 && a.flags0==0x42 && tidOf(a)==0);}
    // OFFER (from the AP, broadcast): the core's RX filter delivers broadcasts.
    assert(r.d._connection.multicast.accepts(kBroadcast));
    // REQUEST
    r.send(ethernet(kBroadcast,r.mac,0x0800,ipv4(17,0,0xffffffff,udp(68,67,dhcp(1,3,xid,r.mac,0,offered)))));
    r.drainAll();assert(r.air.size()==2 && r.air[1].ether[14+28+240+2]==3);
    // ACK unicast to the station MAC: accepted (unicast is never filtered).
    assert(r.d._connection.multicast.accepts(r.mac));
    // ARP probe (sender 0.0.0.0) and announcement, then the router's address.
    r.send(ethernet(kBroadcast,r.mac,0x0806,arp(1,r.mac,0,kZero,offered)));
    r.send(ethernet(kBroadcast,r.mac,0x0806,arp(1,r.mac,offered,kZero,offered)));
    r.send(ethernet(kBroadcast,r.mac,0x0806,arp(1,r.mac,offered,kZero,server)));
    r.drainAll();assert(r.air.size()==5);
    for(size_t i=2;i<5;++i){const auto &a=r.air[i];assert(be16(a.ether.data()+12)==0x0806 && be16(a.ether.data()+14+6)==1 && tidOf(a)==0 && a.flags0==0x42);}
    // DNS to the router (unicast, learned by ARP), then a TCP SYN with DSCP CS6 network control -> TID 6 (VO).
    r.send(ethernet(r.router,r.mac,0x0800,ipv4(17,offered,server,udp(53001,53,dnsQuery(0x1234,"apple.com")))));
    std::vector<uint8_t> syn(20);syn[12]=0x50;syn[13]=0x02;
    r.send(ethernet(r.router,r.mac,0x0800,ipv4(6,offered,0x11223344,syn,46u<<2)));
    r.drainAll();assert(r.air.size()==7);
    {const auto &a=r.air[5];assert(!memcmp(a.ether.data(),r.router,6) && be16(a.ether.data()+14+20+2)==53 && tidOf(a)==0);}
    assert(tidOf(r.air[6])==6); // EF (DSCP 46) -> TID 6, as mac80211
    assert(r.iface.opackets==7 && r.d._connection.txPackets==7 && liveMbufs==0);
    r.d.releaseWMIRuntime();
    puts("  DHCP DISCOVER/REQUEST, ARP probe/announce/request, DNS: all on the air as 802.3 frames, BE; EF -> TID 6");
}

// 5. A frame whose source is not the join's station address is refused by the
// core (outputPacket's sameMAC check): the BSD address and the vdev must agree,
// which the Private Wi-Fi Address path keeps (AirportAth10k::setHardwareAddress
// changes the ifnet address, the join carries it to the core).
static void sourceAddressMustMatch(){
    NativeTxRig r;r.link(true);
    const uint8_t other[6]={0x02,0xaa,0xbb,0xcc,0xdd,0xee};
    r.send(ethernet(r.router,other,0x0800,ipv4(17,1,2,udp(1,2,{}))));
    r.drainAll();
    assert(r.air.empty() && r.iface.opackets==1 && r.d._connection.txErrors==1 && liveMbufs==0);
    r.d.releaseWMIRuntime();
    puts("  foreign source MAC refused by the core (txErrors), not sent");
}

// 6. An open network: unencrypted Ethernet frames (flags0 0x46), non-QoS AP -> TID 16.
static void openNetwork(){
    NativeTxRig r(false);r.d._connection.ap.wmmPresent=false;r.link(true);
    r.send(ethernet(kBroadcast,r.mac,0x0800,ipv4(17,0,0xffffffff,udp(68,67,dhcp(1,1,7,r.mac)))));
    r.drainAll();
    assert(r.air.size()==1 && r.air[0].flags0==0x46 && tidOf(r.air[0])==16);
    r.d.releaseWMIRuntime();
    puts("  open network: plain 802.3 frames, non-QoS TID 16");
}

// 7. outputStart itself: room accounting and return codes.
static void pullUnit(){
    struct Ops {std::deque<int> src;std::vector<int> dst;UInt32 cap=4;bool ok=true;
        bool ready() const {return ok;}UInt32 size() const {return UInt32(dst.size());}UInt32 capacity() const {return cap;}
        int *dequeue(){if(src.empty())return nullptr;static int v;v=src.front();src.pop_front();return &v;}
        void enqueue(int *v){dst.push_back(*v);}} o;
    AirportTx::PullStats st;
    o.src={1,2};assert(AirportTx::pull(o,st)==AirportTx::kSuccess && o.dst.size()==2 && st.packets==2);
    o.src={3,4,5,6};assert(AirportTx::pull(o,st)==AirportTx::kNoResources && o.dst.size()==3 && o.src.size()==3 && st.full==1);
    o.ok=false;assert(AirportTx::pull(o,st)==AirportTx::kNoResources && st.full==2);
    puts("  outputStart: success when drained, NoResources when the gated queue is full or absent");
}

#if AIRPORT_ATH10K_WPASEC
// 802.1X before the port opens: EAPOL from eapolclient leaves in the clear
// (submitNetworkPacket), ordinary packets are dropped so they cannot hold the
// queue behind a stalled head (production outputPacket).
static void enterpriseEAPOLBeforePort(){
    NativeTxRig r;auto &s=r.d._connection;
    s.secure=true;s.enterprise=true;s.portAuthorized=false;s.securityPhase=0;
    std::vector<std::vector<uint8_t>> air;std::vector<uint8_t> flags;
    r.d._regs.tx4Consume=[&](){
        const UInt8 *cmd=static_cast<const UInt8 *>(r.d._packetCommand.virtualAddress());
        const UInt16 n=Ath10k::htcReadLE16(cmd+12);const UInt8 *f=static_cast<const UInt8 *>(r.d._packetBuffer.virtualAddress());
        air.emplace_back(f,f+n);flags.push_back(cmd[9]);r.d.deliver(1,completion(s.txId));
    };
    r.link(true);
    const std::vector<uint8_t> eap={2,0,0,5,1,1,0,5,1};
    const auto dropped=s.txLinkDropped;
    r.send(ethernet(r.router,r.mac,0x0800,ipv4(17,0xc0a80119,0xc0a80101,udp(50000,9,std::vector<uint8_t>(64,1)))));
    assert(air.empty() && s.txLinkDropped==dropped+1 && !r.d._dataTXStalled && r.gated.q.empty()); // dropped, not stalled
    r.send(ethernet(r.router,r.mac,0x888e,eap));
    assert(air.size()==1 && be16(air[0].data()+12)==0x888e && flags[0]==0x46 && s.lastTxEAPOL && s.txOwned && !s.txManagement);
    // The next EAPOL frame waits for the completion instead of being dropped.
    r.send(ethernet(r.router,r.mac,0x888e,eap));
    assert(air.size()==1 && r.gated.q.size()==1);
    for(int i=0;i<5 && s.txOwned;++i){r.now+=20;r.d.tick(r.now);}
    assert(!s.txOwned);r.gated.service();
    assert(air.size()==2 && r.gated.q.empty());
    for(int i=0;i<5 && s.txOwned;++i){r.now+=20;r.d.tick(r.now);}
    r.iface.purge();r.link(false);assert(liveMbufs==0);
    r.d.releaseWMIRuntime();
    puts("  802.1X before the port opens: EAPOL out in the clear, IP dropped, not stalled");
}
#endif
int main(){
    puts("Native TX path (front end + core, host models of IONetworkingFamily 3.4 and IOBasicOutputQueue):");
    pullUnit();
    outputThreadNeverStarted();
    framesReachHTT();
    pauseResumeAndLinkDown();
    dhcpArpDns();
    sourceAddressMustMatch();
    openNetwork();
#if AIRPORT_ATH10K_WPASEC
    enterpriseEAPOLBeforePort();
#endif
    puts("Native TX path tests passed.");
    return 0;
}
