// Host test of the AWDL radio binding (src/kext/Ath10kAwdl.hpp) and of the AWDL engine
// bound to it through the session (native/src/AirportAth10kAwdlSession.hpp).
//   - firmware commands byte for byte: WMI TLVs for the monitor vdev, HTT TX descriptors,
//   - the vdev state machine against a firmware model: bring-up, channel retune,
//     stop, infrastructure pre-emption, rejected starts, lost firmware replies,
//   - RX filtering: only AWDL action frames and frames on the AWDL BSSID are taken,
//   - two sessions on a simulated radio medium: peer discovery, election and sync,
//     availability-window scheduling, Ethernet round trip, loss and recovery.
// This proves the logic against a model of the firmware, not the firmware itself.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdint>
using UInt8=uint8_t;using UInt16=uint16_t;using UInt32=uint32_t;using UInt64=uint64_t;using SInt8=int8_t;
#define AIRPORT_ATH10K_AWDL 1
// The minimum of Ath10kConnection.hpp/Ath10kRegulatory.hpp the radio binding uses.
namespace Ath10k {
inline void htcZero(UInt8 *p,UInt32 n){memset(p,0,n);}
inline void htcWriteLE16(UInt8 *p,UInt16 v){p[0]=UInt8(v);p[1]=UInt8(v>>8);}
inline void htcWriteLE32(UInt8 *p,UInt32 v){for(int i=0;i<4;++i)p[i]=UInt8(v>>(8*i));}
inline UInt16 htcReadLE16(const UInt8 *p){return UInt16(p[0]|(p[1]<<8));}
inline UInt32 connLE32(const UInt8 *p){return UInt32(p[0])|(UInt32(p[1])<<8)|(UInt32(p[2])<<16)|(UInt32(p[3])<<24);}
inline void connTLV(UInt8 *p,UInt16 tag,UInt16 bytes){htcWriteLE16(p,bytes);htcWriteLE16(p+2,tag);}
inline void writeChannelPower(UInt8 *a,UInt8 *b,UInt8 p){htcWriteLE32(a,UInt32(p)<<16);htcWriteLE32(b,UInt32(p)<<16);}
struct RegulatoryPolicy{bool allow149=true;UInt8 power=40;};
inline bool channelDefinitionAllowed(UInt32,UInt32,UInt32,bool){return true;}
inline UInt8 regulatoryPower(const RegulatoryPolicy &p,UInt32 c,UInt32,UInt32,bool){return (c==149 && !p.allow149)?0:p.power;}
}
#include "../../src/kext/Ath10kAwdl.hpp"
#include "../src/AirportAth10kAwdlSession.hpp"
using namespace Ath10k;

static const UInt8 kMac[6]={0x02,0x11,0x22,0x33,0x44,0x55};
static AwdlRadio *newRadio(){return new AwdlRadio();}

static void testCommands(){
    RegulatoryPolicy p;auto *rp=newRadio();auto &r=*rp;UInt8 f[256];
    assert(awdlRequestStart(r,kMac,6,p));
    r.step=AwdlStep::Create;
    UInt32 n=buildAwdlRadioCommand(f,sizeof f,2,5,r,p);
    // WMI VDEV_CREATE: vdev 1, type monitor(4), subtype 0, the AWDL MAC; TLV tag 86 (same layout as vdev 0 create).
    assert(n==36 && f[0]==2 && f[1]==1 && f[5]==5 && htcReadLE16(f+2)==28 && connLE32(f+8)==0x5001);
    assert(htcReadLE16(f+12)==20 && htcReadLE16(f+14)==86 && connLE32(f+16)==1 && connLE32(f+20)==4 && connLE32(f+24)==0 && !memcmp(f+28,kMac,6));
    r.step=AwdlStep::Start;n=buildAwdlRadioCommand(f,sizeof f,2,6,r,p);
    // VDEV_START on channel 6: TLV 88 (72 bytes), channel TLV 80 at +88 with the frequency, HT20 on 2.4 GHz.
    assert(n==120 && connLE32(f+8)==0x5003 && htcReadLE16(f+14)==88 && connLE32(f+16)==1 && htcReadLE16(f+90)==80 && connLE32(f+92)==2437 && connLE32(f+96)==2437);
    assert((connLE32(f+104)&63)==5 && (connLE32(f+104)&(1u<<11)));
    // the WMI channel TLV carries the regulatory power limit
    assert(f[110]==40 || connLE32(f+108)==(40u<<16));
    r.channelTarget=44;r.step=AwdlStep::Restart;n=buildAwdlRadioCommand(f,sizeof f,2,7,r,p);
    assert(n==120 && connLE32(f+8)==0x5004 && connLE32(f+16)==1 && connLE32(f+92)==5220 && (connLE32(f+104)&63)==4);
    r.step=AwdlStep::Up;n=buildAwdlRadioCommand(f,sizeof f,2,8,r,p);
    assert(n==32 && connLE32(f+8)==0x5005 && htcReadLE16(f+14)==92 && connLE32(f+16)==1 && connLE32(f+20)==0 && !memcmp(f+24,kMac,6));
    r.step=AwdlStep::Down;n=buildAwdlRadioCommand(f,sizeof f,2,8,r,p);assert(n==20 && connLE32(f+8)==0x5007 && htcReadLE16(f+14)==94 && connLE32(f+16)==1);
    r.step=AwdlStep::Stop;n=buildAwdlRadioCommand(f,sizeof f,2,8,r,p);assert(n==20 && connLE32(f+8)==0x5006 && htcReadLE16(f+14)==93 && connLE32(f+16)==1);
    r.step=AwdlStep::Delete;n=buildAwdlRadioCommand(f,sizeof f,2,8,r,p);assert(n==20 && connLE32(f+8)==0x5002 && htcReadLE16(f+14)==87 && connLE32(f+16)==1);
    // no command without an endpoint or a sane channel
    r.step=AwdlStep::Start;assert(!buildAwdlRadioCommand(f,sizeof f,0,1,r,p));
    r.channel=13;assert(!buildAwdlRadioCommand(f,sizeof f,2,1,r,p));r.channel=6;
    // social channels only, and regulatory: 149 not permitted when the policy forbids it (run 2 was this)
    AwdlRadio s;assert(!awdlRequestStart(s,kMac,1,p));
    RegulatoryPolicy no149;no149.allow149=false;
    AwdlRadio s2;assert(!awdlRequestStart(s2,kMac,149,no149));assert(awdlRequestStart(s2,kMac,149,p));
    // an all-1-bit MAC (multicast) is refused
    UInt8 mc[6]={1,2,3,4,5,6};AwdlRadio s3;assert(!awdlRequestStart(s3,mc,6,p));
    // HTT TX descriptor: action frame (MGMT, TID 17), data frame (RAW, TID 16), vdev id 1 in flags1 bits 0..5
    UInt8 frame[64];memset(frame,0,sizeof frame);frame[0]=0xd0;
    n=buildAwdlHTT(f,sizeof f,frame,60,0x1000,0x2000,3,9,42,true);
    assert(n==24+52 && f[0]==3 && f[5]==9 && f[8]==1 && f[9]==0x65);
    assert(htcReadLE16(f+10)==(0x800u|(17u<<6)|1u) && htcReadLE16(f+12)==60 && htcReadLE16(f+14)==42 && connLE32(f+16)==0x1000 && connLE32(f+20)==0xffff);
    assert(!memcmp(f+24,frame,50));
    n=buildAwdlHTT(f,sizeof f,frame,60,0x1000,0x2000,3,9,43,false);
    assert(f[9]==0x05 && htcReadLE16(f+10)==(0x800u|(16u<<6)|1u) && connLE32(f+16)==0x2000);
    assert(!buildAwdlHTT(f,sizeof f,frame,23,0x1000,0x2000,3,1,1,true)); // shorter than a MAC header
    delete rp;
    printf("Firmware commands: WMI vdev create/start/restart/up/down/stop/delete and HTT descriptors are as specified.\n");
}

// Firmware model: command completions and vdev events under test control.
struct Fw {
    AwdlRadio &r;RegulatoryPolicy p;UInt64 now=1000;std::vector<UInt32> cmds;UInt8 buf[256];bool rejectStart=false;
    bool infraIdle=true;
    explicit Fw(AwdlRadio &a):r(a){}
    bool issue(){
        awdlPlan(r,infraIdle,false,0,now);
        const bool cmd=r.step==AwdlStep::Create||r.step==AwdlStep::Start||r.step==AwdlStep::Up||r.step==AwdlStep::Down||r.step==AwdlStep::Restart||r.step==AwdlStep::RestartUp||r.step==AwdlStep::Stop||r.step==AwdlStep::Delete;
        if(!cmd||r.wmiSubmitted||r.eventPending)return false;
        UInt32 n=buildAwdlRadioCommand(buf,sizeof buf,2,0,r,p);assert(n);
        cmds.push_back(connLE32(buf+8));awdlCommandSubmitted(r,now);return true;
    }
    void ce(){now+=2;awdlCommandComplete(r,now);}
    void event(UInt32 id,UInt32 resp,UInt32 status,UInt32 vdev=1){
        UInt8 e[64]{};htcWriteLE16(e+12,16);htcWriteLE16(e+14,id==0x5001?40:41);htcWriteLE32(e+16,vdev);htcWriteLE32(e+24,resp);htcWriteLE32(e+28,status);
        now+=3;assert(awdlConsumeVdevEvent(r,e,40,id,now)==(vdev==1));
    }
    // A full round of the state machine until it stops issuing.
    void pump(UInt32 resp=0){
        for(int guard=0;guard<40 && issue();++guard){
            const bool ev=r.eventPending;
            ce();
            if(ev){if(r.step==AwdlStep::Stop)event(0x5002,0,0);else event(0x5001,resp,rejectStart?5:0);}
        }
    }
};

static void testStateMachine(){
    RegulatoryPolicy p;auto *rp=newRadio();auto &r=*rp;Fw f(r);
    assert(r.step==AwdlStep::Off);
    assert(awdlRequestStart(r,kMac,6,p));
    f.pump();
    assert(r.step==AwdlStep::Running && r.channel==6);
    assert((f.cmds==std::vector<UInt32>{0x5001,0x5003,0x5005}));
    // retune: down / restart / up, keeping the vdev
    f.cmds.clear();assert(awdlRequestChannel(r,44,p));f.pump(1);
    assert(r.step==AwdlStep::Running && r.channel==44 && r.counters.channelSwitches==1);
    assert((f.cmds==std::vector<UInt32>{0x5007,0x5004,0x5005}));
    // restart response with the wrong type or a stale response is ignored (start resp on a restart)
    assert(awdlRequestChannel(r,6,p));assert(f.issue());
    f.ce();                          // Down CE done; plan chooses Restart next
    assert(f.issue() && r.step==AwdlStep::Restart);f.ce();
    f.event(0x5001,0,0);             // a resp_type 0 event is not the reply to a restart
    assert(r.step==AwdlStep::Restart && r.eventPending);
    f.event(0x5001,1,0);             // the real one, after the CE
    assert(r.step==AwdlStep::RestartUp);
    f.pump();assert(r.step==AwdlStep::Running && r.channel==6);
    // event seen before the CE completion (CE and event order is not guaranteed)
    assert(awdlRequestChannel(r,44,p));assert(f.issue());f.ce();assert(f.issue() && r.step==AwdlStep::Restart);
    f.event(0x5001,1,0);assert(r.step==AwdlStep::Restart);f.ce();assert(r.step==AwdlStep::RestartUp);
    f.pump();assert(r.channel==44);
    // stop: down / stop / delete
    f.cmds.clear();awdlRequestStop(r);f.pump();
    assert(r.step==AwdlStep::Off && (f.cmds==std::vector<UInt32>{0x5007,0x5006,0x5002}));
    // a rejected start fails the radio without touching anything else
    r.reset();f.cmds.clear();f.rejectStart=true;assert(awdlRequestStart(r,kMac,6,p));f.pump();
    assert(r.step==AwdlStep::Failed && r.error && !strcmp(r.error,"awdl-vdev-start-rejected") && !r.running());
    assert(!awdlRequestStart(r,kMac,6,p));   // stays failed until a firmware reload resets it
    r.reset();assert(awdlRequestStart(r,kMac,6,p));f.rejectStart=false;f.pump();assert(r.running());
    // VDEV_DELETE's opaque event is consumed by correlation, whatever its payload says
    {UInt8 e[64]{};memset(e,0xee,sizeof e);awdlRequestStop(r);f.pump();assert(r.step==AwdlStep::Off && r.deletesPending==1);
     assert(awdlConsumeVdevEvent(r,e,40,0x5006,f.now) && r.deletesPending==0);
     assert(!awdlConsumeVdevEvent(r,e,40,0x5006,f.now));assert(awdlRequestStart(r,kMac,6,p));f.pump();assert(r.running());}
    // an infrastructure join tears the monitor down and the front end restarts it when idle
    f.cmds.clear();f.infraIdle=false;f.pump();assert(r.step==AwdlStep::Off);
    assert((f.cmds==std::vector<UInt32>{0x5007,0x5006,0x5002}));
    // never comes up while the station path is busy
    f.cmds.clear();assert(awdlRequestStart(r,kMac,6,p));f.pump();assert(r.step==AwdlStep::Off && f.cmds.empty() && r.startRequested);
    f.infraIdle=true;f.pump();assert(r.running());
    // firmware never answers VDEV_START: timeout after 3 s, failed, reported
    r.reset();assert(awdlRequestStart(r,kMac,6,p));assert(f.issue());f.ce();assert(f.issue());f.ce();assert(r.eventPending);
    f.now+=2900;assert(!awdlTimedOut(r,f.now));f.now+=1000;assert(awdlTimedOut(r,f.now));
    // events for other vdevs are not consumed (the station's still reach the core parser)
    r.reset();assert(awdlRequestStart(r,kMac,6,p));assert(f.issue());f.ce();assert(f.issue());f.ce();
    f.event(0x5001,0,0,0);assert(r.eventPending && r.step==AwdlStep::Start);
    f.event(0x5001,0,0,1);assert(r.step==AwdlStep::Up);
    r.reset();
    // wake re-init: reset() drops the state; a fresh start works
    assert(awdlRequestStart(r,kMac,6,p));f.pump();assert(r.running());r.reset();assert(r.step==AwdlStep::Off && !r.startRequested);
    delete rp;
    printf("Radio state machine: bring-up, retune, stop, pre-emption by a join, rejected start, timeouts and event ordering.\n");
}

static void testRxFilter(){
    auto *rp=newRadio();auto &r=*rp;r.step=AwdlStep::Running;r.channel=6;
    UInt8 a[100]{};a[0]=0xd0;a[24]=127;a[25]=0x00;a[26]=0x17;a[27]=0xf2;a[28]=8;
    // WMI management RX: only category 127 / Apple OUI / type 8, status 0
    assert(awdlRxManagement(r,a,80,6,50,0,123));
    const UInt8 *d;UInt32 n;UInt8 k,c;SInt8 rssi;UInt64 t;
    assert(awdlGetRx(r,&d,&n,&k,&c,&rssi,&t) && n==80 && k==UInt8(AwdlRx::Action) && c==6 && rssi==-45 && t==123);awdlPopRx(r);
    assert(!awdlRxManagement(r,a,80,6,50,1,1));          // RX status error
    UInt8 b[100];memcpy(b,a,100);b[24]=4;assert(!awdlRxManagement(r,b,80,6,50,0,1)); // public action (not vendor)
    memcpy(b,a,100);b[27]=0xf3;assert(!awdlRxManagement(r,b,80,6,50,0,1));          // other OUI
    memcpy(b,a,100);b[28]=9;assert(!awdlRxManagement(r,b,80,6,50,0,1));             // other type
    memcpy(b,a,100);b[0]=0x80;assert(!awdlRxManagement(r,b,80,6,50,0,1));           // beacon
    assert(!awdlRxManagement(r,a,20,6,50,0,1));                                     // truncated
    r.step=AwdlStep::Down;assert(!awdlRxManagement(r,a,80,6,50,0,1));r.step=AwdlStep::Running; // only while running
    // ring: bounded, oldest kept, overruns counted
    for(UInt32 i=0;i<kAwdlRxSlots;++i)assert(awdlRxManagement(r,a,80,6,50,0,i));
    assert(!awdlRxManagement(r,a,80,6,50,0,99) && r.counters.rxDropped==1);
    assert(awdlGetRx(r,&d,&n,&k,&c,&rssi,&t) && t==0);
    for(UInt32 i=0;i<kAwdlRxSlots;++i)awdlPopRx(r);
    assert(!awdlGetRx(r,&d,&n,&k,&c,&rssi,&t));
    // HTT data: raw MPDU after the 300-byte descriptor, FCS trimmed, on the AWDL BSSID only
    static UInt8 h[600];memset(h,0,sizeof h);
    htcWriteLE32(h+32,0u<<8);htcWriteLE32(h+24,100);
    UInt8 *m=h+300;m[0]=0x08;m[16]=0x00;m[17]=0x25;m[18]=0x00;m[19]=0xff;m[20]=0x94;m[21]=0x73;
    assert(awdlRxHtt(r,h,600,100,1,777));
    assert(awdlGetRx(r,&d,&n,&k,&c,&rssi,&t) && n==96 && k==UInt8(AwdlRx::Data) && t==777);awdlPopRx(r);
    htcWriteLE32(h+32,1u<<8);assert(awdlRxHtt(r,h,600,100,1,1));          // native WiFi keeps its length
    assert(awdlGetRx(r,&d,&n,&k,&c,&rssi,&t) && n==100);awdlPopRx(r);
    htcWriteLE32(h+32,2u<<8);assert(!awdlRxHtt(r,h,600,100,1,1));          // 802.3 decap is not raw
    htcWriteLE32(h+32,0);
    assert(!awdlRxHtt(r,h,600,100,2,1));                                   // status != 1
    assert(!awdlRxHtt(r,h,600,10,1,1));assert(!awdlRxHtt(r,h,320,100,1,1)); // implausible lengths
    m[20]=0x95;assert(!awdlRxHtt(r,h,600,100,1,1));m[20]=0x94;             // another BSSID
    m[0]=0xd0;assert(!awdlRxHtt(r,h,600,100,1,1));m[0]=0x08;               // not a data frame
    // TX queue: running only, bounded
    UInt8 fr[60]{};assert(awdlQueueTx(r,fr,60,true));
    for(UInt32 i=1;i<kAwdlTxSlots;++i)assert(awdlQueueTx(r,fr,60,false));
    assert(!awdlQueueTx(r,fr,60,false) && r.counters.txDropped==1);
    assert(!awdlQueueTx(r,fr,10,false));                                   // shorter than a header
    r.step=AwdlStep::Off;assert(!awdlQueueTx(r,fr,60,false));
    delete rp;
    printf("RX filter and queues: AWDL frames only, FCS handling, bounded with drops counted.\n");
}

// -------------------------------------------------- the session over a simulated radio
using namespace AirportAwdl;
static uint64_t gNow=0;
struct SimRadio:RadioPort{
    struct Air *air=nullptr;RadioStep st=RadioStep::Off;uint8_t channel=0;int startDelay=0;int setChannels=0;
    uint8_t mac[6]{};bool failNext=false;std::vector<std::vector<uint8_t>> rx;std::vector<uint64_t> rxTimes;std::vector<int8_t> rxRssi;
    size_t txAction=0,txData=0;bool refuseStart=false;
    bool radioStart(const uint8_t *m,uint8_t c,bool) override {if(refuseStart)return false;if(failNext){failNext=false;st=RadioStep::Failed;return true;}memcpy(mac,m,6);channel=c;st=RadioStep::Running;return true;}
    void radioStop() override {st=RadioStep::Off;channel=0;}
    bool radioSetChannel(uint8_t c) override {channel=c;++setChannels;return true;}
    bool radioTransmit(const uint8_t *f,size_t n,bool action) override;
    bool radioReceive(uint8_t *b,size_t cap,RxFrame &o) override {
        if(rx.empty())return false;auto &v=rx.front();assert(v.size()<=cap);memcpy(b,v.data(),v.size());
        o.data=b;o.length=v.size();o.rssi=rxRssi.front();o.channel=channel;o.data_=false;o.timeUs=rxTimes.front();
        rx.erase(rx.begin());rxTimes.erase(rxTimes.begin());rxRssi.erase(rxRssi.begin());return true;
    }
    RadioStep radioStep() override {return st;}
};
struct Air{std::vector<SimRadio *> radios;uint32_t lost=0;};
bool SimRadio::radioTransmit(const uint8_t *f,size_t n,bool action){
    if(st!=RadioStep::Running)return false;
    if(action)++txAction;else ++txData;
    for(SimRadio *o:air->radios)if(o!=this && o->st==RadioStep::Running && o->channel==channel && channel){
        o->rx.emplace_back(f,f+n);o->rxTimes.push_back(gNow);o->rxRssi.push_back(-45);
    }
    return true;
}
struct SimHost:HostPort{std::vector<std::vector<uint8_t>> eth;int added=0,removed=0,syncs=0;
    void hostEthernet(const uint8_t *e,size_t n) override {eth.emplace_back(e,e+n);}
    void hostPeer(const Peer &,bool a) override {if(a)++added;else ++removed;}
    void hostSyncChanged() override {++syncs;}};
struct SimNode{SimRadio radio;SimHost host;Session session;uint64_t next=0;SimNode():session(radio,host){session.setNeighbourEcho(false);}};

static void run(SimNode **nodes,int count,uint64_t until){
    while(gNow<until){
        uint64_t soonest=until;
        for(int i=0;i<count;++i){
            if(nodes[i]->next<=gNow)nodes[i]->next=nodes[i]->session.poll(gNow);
            if(nodes[i]->next<soonest)soonest=nodes[i]->next;
        }
        gNow=soonest<=gNow?gNow+1:soonest;
    }
}

static void testSession(){
    static Air air;static SimNode a,b;a.radio.air=b.radio.air=&air;air.radios={&a.radio,&b.radio};
    SimNode *nodes[2]={&a,&b};
    const uint8_t ma[6]={0x02,1,1,1,1,1},mb[6]={0x02,2,2,2,2,2};
    gNow=5000000;
    a.session.configure("alpha",ma,6);b.session.configure("bravo",mb,6);
    a.session.enable(gNow);b.session.enable(gNow);a.next=gNow;b.next=gNow+777;
    run(nodes,2,gNow+200000);
    assert(a.session.running() && b.session.running() && a.radio.channel==6);
    // alpha becomes the better master
    auto &ae=const_cast<Engine &>(a.session.engine());ae.state.election.selfMetric=ae.state.election.masterMetric=500;ae.state.election.selfCounter=ae.state.election.masterCounter=7;
    auto &be=const_cast<Engine &>(b.session.engine());be.state.sync.lastUpdate+=777777;  // an unsynchronised clock
    run(nodes,2,gNow+6000000);
    // discovery, election, synchronisation (OWL's threshold)
    assert(a.host.added==1 && b.host.added==1 && a.session.peerCount()==1 && b.session.peerCount()==1);
    assert(be.state.election.masterAddr==Addr::of(ma) && be.state.election.height==1);
    assert(b.host.syncs>0);
    int64_t err=int64_t(syncNextAwTu(gNow,ae.state.sync))-int64_t(syncNextAwTu(gNow,be.state.sync));
    assert(err<=kSyncThresholdTu && err>=-kSyncThresholdTu);
    assert(b.session.state()==Session::State::Running && b.radio.setChannels==1); // static channel: one tune, no hop
    assert(a.radio.txAction>=20 && b.radio.txAction>=5);
    // an Ethernet frame round trip through the schedule (unicast + the multicast window)
    std::vector<uint8_t> eth(14+60,0);memcpy(eth.data(),ma,6);memcpy(eth.data()+6,mb,6);eth[12]=0x86;eth[13]=0xdd;eth[14]=0x60;eth[73]=0x5a;
    assert(b.session.send(eth.data(),eth.size(),gNow));
    std::vector<uint8_t> mc(14+40,0);mc[0]=0x33;mc[1]=0x33;mc[5]=0xfb;memcpy(mc.data()+6,ma,6);mc[12]=0x86;mc[13]=0xdd;mc[14]=0x60;
    assert(a.session.send(mc.data(),mc.size(),gNow));
    run(nodes,2,gNow+2000000);
    assert(a.host.eth.size()==1 && a.host.eth[0]==eth);
    assert(b.host.eth.size()==1 && b.host.eth[0]==mc);
    assert(a.radio.txData==1 && b.radio.txData==1);
    // reply in the other direction
    std::vector<uint8_t> reply(14+20,0);memcpy(reply.data(),mb,6);memcpy(reply.data()+6,ma,6);reply[12]=0x86;reply[13]=0xdd;reply[14]=0x60;
    assert(a.session.send(reply.data(),reply.size(),gNow));run(nodes,2,gNow+1000000);
    assert(b.host.eth.size()==2 && b.host.eth[1]==reply);
    // a corrupted frame from the air is counted and survives
    b.radio.rx.emplace_back(40,0xff);b.radio.rxTimes.push_back(gNow);b.radio.rxRssi.push_back(-40);
    run(nodes,2,gNow+100000);assert(b.session.stats().rxBad>=1 && b.session.running());
    // the radio is torn down under us (an infrastructure join): the engine stops and peers go away
    const int removedBefore=b.host.removed;
    b.radio.st=RadioStep::Down;run(nodes,2,gNow+200000);
    assert(!b.session.running() && b.host.removed==removedBefore+1);
    // ... and it comes back
    b.radio.st=RadioStep::Off;run(nodes,2,gNow+2500000);
    assert(b.session.running() && b.session.stats().restarts>=1);
    // a failing radio does not spin (retried once per second)
    a.radio.st=RadioStep::Failed;a.radio.failNext=true;
    const uint64_t r0=a.session.stats().restarts;run(nodes,2,gNow+3000000);
    assert(a.session.stats().restarts-r0<=4);
    // peer loss recovers mastership
    a.session.disable();assert(!a.session.running() && a.radio.st==RadioStep::Off);
    run(nodes,2,gNow+3500000);
    assert(b.session.peerCount()==0 && be.state.election.masterAddr==Addr::of(mb) && be.state.psfInterval==kPsfIntervalMasterTu);
    b.session.disable();
    // refused start: the session stays Waiting and retries, and does not wedge
    static SimNode c;c.radio.air=&air;air.radios={&c.radio};SimNode *one[1]={&c};gNow=30000000;c.radio.refuseStart=true;c.session.configure("c",ma,6);c.session.enable(gNow);c.next=gNow;
    run(one,1,gNow+3500000);assert(c.session.state()==Session::State::Waiting && !c.session.running() && c.session.stats().restarts>=3);
    c.radio.refuseStart=false;run(one,1,gNow+1200000);assert(c.session.running());c.session.disable();
    printf("Session over the radio model: discovery, election, sync (|error| <= %d TU), AW-scheduled Ethernet round trip, loss and recovery.\n",kSyncThresholdTu);
}


// Shared placement in the core: the monitor vdev copies the station's channel definition, carries the station's MAC,
// and stays only while the station is joined on that channel.
static void testSharedRadio(){
    RegulatoryPolicy p;auto *rp=newRadio();auto &r=*rp;Fw f(r);UInt8 b[256];
    static const UInt8 sta[6]={0x00,0x03,0x7f,0x12,0x34,0x56};
    AwdlSharedChannel c{};c.channel=44;c.center=46;c.phy=7u|(1u<<11)|(1u<<12);c.power=30;
    assert(awdlRequestStartShared(r,sta,c));assert(r.shared && r.channel==44);
    // refused while the station is not joined on the channel: stays requested, no command
    f.infraIdle=true;awdlPlan(r,true,false,0,f.now);assert(r.step==AwdlStep::Off && r.startRequested);
    awdlPlan(r,false,true,36,f.now);assert(r.step==AwdlStep::Off);
    awdlPlan(r,false,true,44,f.now);assert(r.step==AwdlStep::Create);
    UInt32 n=buildAwdlRadioCommand(b,sizeof b,2,5,r,p);
    assert(n==36 && connLE32(b+8)==0x5001 && connLE32(b+20)==4 && !memcmp(b+28,sta,6));   // monitor vdev, the station's MAC
    r.step=AwdlStep::Start;n=buildAwdlRadioCommand(b,sizeof b,2,6,r,p);
    assert(n==120 && connLE32(b+92)==5220 && connLE32(b+96)==5230 && connLE32(b+104)==c.phy); // center and phy from the station
    assert(connLE32(b+108)==(30u<<16));
    // running: a join to another channel, or the station leaving, stops it
    r.step=AwdlStep::Running;r.channelTarget=44;awdlPlan(r,false,true,44,f.now);assert(r.step==AwdlStep::Running);
    awdlPlan(r,false,true,36,f.now);assert(r.step==AwdlStep::Down && r.stopRequested);
    r.reset();assert(awdlRequestStartShared(r,sta,c));r.step=AwdlStep::Running;r.channelTarget=44;
    awdlPlan(r,true,false,0,f.now);assert(r.step==AwdlStep::Down);
    // no shared start on a non-social channel, without phy or power, or with a multicast MAC
    AwdlSharedChannel bad=c;bad.channel=36;r.reset();assert(!awdlRequestStartShared(r,sta,bad));
    bad=c;bad.phy=0;assert(!awdlRequestStartShared(r,sta,bad));
    bad=c;bad.power=0;assert(!awdlRequestStartShared(r,sta,bad));
    UInt8 mc[6]={1,2,3,4,5,6};assert(!awdlRequestStartShared(r,mc,c));
    // exclusive plan unchanged
    r.reset();assert(awdlRequestStart(r,sta,6,p));awdlPlan(r,true,false,0,f.now);assert(r.step==AwdlStep::Create);
    delete rp;
    printf("Shared placement: station channel definition and MAC on the monitor vdev, gated on the station's channel.\n");
}
// Session placement over the radio model.
struct PlacementRadio:RadioPort{
    RadioStep st=RadioStep::Off;uint8_t channel=0;bool shared=false;int starts=0,stops=0;StationInfo info{};std::vector<std::vector<uint8_t>> tx;
    bool radioStart(const uint8_t *,uint8_t c,bool s) override {channel=c;shared=s;st=RadioStep::Running;++starts;return true;}
    void radioStop() override {st=RadioStep::Off;++stops;}
    bool radioSetChannel(uint8_t c) override {channel=c;return true;}
    bool radioTransmit(const uint8_t *f,size_t n,bool) override {tx.emplace_back(f,f+n);return true;}
    bool radioReceive(uint8_t *,size_t,RxFrame &) override {return false;}
    RadioStep radioStep() override {return st;}
    void radioStation(StationInfo &o) override {o=info;}
};
struct NullHost:HostPort{void hostEthernet(const uint8_t *,size_t) override {}void hostPeer(const Peer &,bool) override {}void hostSyncChanged() override {}};
static void testPlacement(){
    auto *pr=new PlacementRadio;PlacementRadio &r=*pr;NullHost h;auto *sp=new Session(r,h);Session &s=*sp;
    static const uint8_t mac[6]={2,3,4,5,6,7};s.configure("placement",mac,6);s.setNeighbourEcho(false);
    uint64_t t=1000;s.enable(t);s.poll(t);assert(r.starts==1 && !r.shared && r.channel==6);t+=1000;s.poll(t);
    assert(s.running() && s.placement()==Session::Placement::Fixed && s.currentChannel()==6);
    // joins on channel 44: tear down, come back shared on 44
    r.info.associated=true;r.info.channel=44;t+=1000;s.poll(t);assert(r.stops==1 && r.st==RadioStep::Off);
    t+=2000000;s.poll(t);assert(r.starts==2 && r.shared && r.channel==44);t+=1000;s.poll(t);
    assert(s.running() && s.placement()==Session::Placement::Shared && s.currentChannel()==44);
    // PSF/MIF go out with the station's MAC and the shared master channel
    r.tx.clear();t+=300000;s.poll(t);assert(!r.tx.empty() && !memcmp(r.tx[0].data()+10,mac,6));
    // a station on channel 36 is out of reach: the radio stops, nothing runs
    r.info.channel=36;t+=1000;s.poll(t);assert(!s.running() && s.placement()==Session::Placement::None && r.st==RadioStep::Off);
    int stops=r.stops;t+=1000000;s.poll(t);assert(r.stops==stops && r.st==RadioStep::Off);   // no spinning
    // station gone: back to fixed on the configured channel
    r.info.associated=false;r.info.channel=0;t+=2000000;s.poll(t);assert(!r.shared && r.channel==6 && r.st==RadioStep::Running);t+=1000;s.poll(t);assert(s.placement()==Session::Placement::Fixed);
    delete sp;delete pr;
    printf("Placement: follows the station (shared on its social channel, nothing elsewhere, fixed when idle) without spinning.\n");
}
// Owly's adopt_sequence (AWDL-12): advertise only the master's slots on our channel; keep ours if none match.
static void testSharedSequence(){
    auto *ep=new Engine;Engine &e=*ep;struct S:Sink{void setChannel(uint8_t) override {}bool transmit(const uint8_t *,size_t,bool) override {return true;}}sink;
    static const uint8_t me[6]={2,0,0,0,0,9},master[6]={2,0,0,0,0,1};
    e.start("x",Addr::of(me),Chan::opclass44(),1,&sink);e.setPresence(kPresenceShare,Chan::opclass44());
    Peer *p=nullptr;bool ok=false;assert(e.state.peers.add(Addr::of(master),5,&p,&ok)>=0);p->valid=p->sentMif=true;p->version=0x34;p->devclass=1;p->seqEnc=kChanEncOpclass;
    for(int i=0;i<kChanSeqLength;++i)p->sequence[i]=Chan::null();
    p->sequence[1]=Chan::opclass44();p->sequence[9]=Chan::opclass44();p->sequence[3]=Chan::opclass6();
    p->election.masterMetric=600;p->election.selfMetric=600;p->election.masterCounter=10;p->election.height=0;
    e.state.peers.peers[0]=*p; // same slot table
    assert(electionRun(e.state.election,e.state.peers,false) || e.state.election.syncAddr==Addr::of(master));
    e.tick(2000000);assert(e.state.election.syncAddr==Addr::of(master));
    for(int i=0;i<kChanSeqLength;++i){int n=chanNum(e.state.channel.sequence[i],e.state.channel.enc);assert(n==((i==1||i==9)?44:0));}
    // a master that never names our channel: keep what we had (the phone must keep addressing us)
    Peer &q=*e.state.peers.find(Addr::of(master));for(int i=0;i<kChanSeqLength;++i)q.sequence[i]=Chan::opclass6();
    q.lastUpdate=3900000;e.tick(4000000);assert(e.state.noOverlap && chanNum(e.state.channel.sequence[1],e.state.channel.enc)==44);
    // fixed: every slot; multicast in any slot with the 3 TU guard
    e.setPresence(kPresenceFixed,Chan::opclass44());assert(isMulticastEaw(e.state,12345678) && multicastGuardTu(e.state)==kUnicastGuardTu);
    delete ep;
    printf("Shared sequence: the master's slots on our channel only (AWDL-12), no-overlap keeps the previous sequence.\n");
}
// The session rules Owly documents: OWLY-2 echo, AWDL-18 early SYN hold, A-MSDU bound, data keeps a peer alive,
// and the service records from the macOS stack in MIFs (AWDL-2 addressing stays the station MAC).
static void testSessionRules(){
    Addr a=Addr::of((const uint8_t *)"\x02\x11\x22\x33\x44\x55"),b=Addr::of((const uint8_t *)"\x06\x77\x88\x99\xaa\xbb");
    uint8_t f[120];size_t n=Session::buildEcho(f,sizeof f,a,b,0x1234);
    assert(n==14+40+16 && !memcmp(f,b.b,6) && !memcmp(f+6,a.b,6) && f[12]==0x86 && f[13]==0xdd);
    // fe80:: + modified EUI-64 of both ends, ICMPv6 echo request, hop limit 255, checksum verifies
    assert(f[14+8]==0xfe && f[14+9]==0x80 && f[14+16]==(a.b[0]^2) && f[14+19]==0xff && f[14+20]==0xfe && f[14+7]==255 && f[14+6]==58);
    assert(f[14+40]==128 && get16be(f+14+40+6)==0x1234);
    uint32_t sum=0;for(size_t i=8;i<40;i+=2)sum+=get16be(f+14+i);sum+=16;sum+=58;for(size_t i=0;i<16;i+=2)sum+=get16be(f+14+40+i);while(sum>>16)sum=(sum&0xffff)+(sum>>16);assert((sum&0xffff)==0xffff);
    // A-MSDU bound
    auto *ep=new Engine;Engine &e=*ep;struct S:Sink{int frames=0;void setChannel(uint8_t) override {}bool transmit(const uint8_t *,size_t,bool) override {return true;}void data(const uint8_t *,size_t,const Addr &,const Addr &) override {++frames;}}sink;
    e.start("n",a,Chan::opclass6(),1,&sink);e.state.filterRssi=false;
    assert(e.state.peers.add(b,5,nullptr,nullptr)>=0);
    uint8_t amsdu[3000];memset(amsdu,0,sizeof amsdu);put16le(amsdu,kFcQosData);memcpy(amsdu+4,a.b,6);memcpy(amsdu+10,b.b,6);memcpy(amsdu+16,kAwdlBssid,6);put16le(amsdu+24,kQosAmsduPresent);
    uint8_t *q=amsdu+26;
    for(int i=0;i<17;++i){memcpy(q,a.b,6);memcpy(q+6,b.b,6);put16be(q+12,8+8+4);q+=14;
        memcpy(q,"\xaa\xaa\x03\x00\x17\xf2\x08\x00",8);put16le(q+8,kDataHead);put16be(q+14,kEtherTypeIPv6);q+=8+8+4+2;}
    assert(rx(amsdu,size_t(q-amsdu),-40,10,e.state,&sink)==kRxUnexpectedFormat);   // 17 subframes: dropped
    sink.frames=0;
    // data frames count as hearing the peer
    Peer *p=e.state.peers.find(b);uint64_t before=p->lastUpdate;
    uint8_t d[80];memset(d,0,sizeof d);put16le(d,kFcData);memcpy(d+4,a.b,6);memcpy(d+10,b.b,6);memcpy(d+16,kAwdlBssid,6);uint8_t *m=d+24;memcpy(m,"\xaa\xaa\x03\x00\x17\xf2\x08\x00",8);put16le(m+8,kDataHead);put16be(m+14,kEtherTypeIPv6);
    assert(rx(d,24+8+8+4,-40,777,e.state,&sink)==kRxOk && sink.frames==1 && p->lastUpdate==777 && before!=777);
    delete ep;
    // service records from the OOB frame end up in MIFs, the ARPA TLV replaces our host name
    auto *r=new PlacementRadio;NullHost h;auto *sp=new Session(*r,h);Session &s=*sp;s.configure("hostname",a.b,6);s.setNeighbourEcho(false);
    uint64_t t=1000;s.enable(t);s.poll(t);t+=1000;s.poll(t);assert(s.running());
    uint8_t body[16+6+7+9],*w=body;memset(body,0,sizeof body);body[0]=kVendorSpecific;memcpy(body+1,kAwdlOui,3);body[4]=kAwdlType;body[5]=0x10;body[6]=3;
    w+=16;w[0]=kTlvServiceResponse;put16le(w+1,3);memcpy(w+3,"abc",3);w+=6;w[0]=kTlvArpa;put16le(w+1,4);w[3]=3;w[4]=1;w[5]='m';w[6]='e';w+=7;w[0]=99;put16le(w+1,6);
    assert(s.setServiceTlvsFromFrame(body,sizeof body)==2);
    assert(s.setServiceTlvsFromFrame(body,10)<0);   // truncated TLV rejected
    r->tx.clear();t+=2000000;for(int i=0;i<40;++i){s.poll(t);t+=64*1024;}
    bool sawMif=false;for(auto &f2:r->tx)if(f2.size()>40 && f2[0]==0xd0 && f2[30]==3){sawMif=true;
        bool found2=false,found16=false;size_t at=24+16;while(at+3<=f2.size()){uint8_t ty=f2[at];size_t l=f2[at+1]|(size_t(f2[at+2])<<8);if(at+3+l>f2.size())break;if(ty==2&&l==3&&!memcmp(f2.data()+at+3,"abc",3))found2=true;if(ty==16&&l==4&&f2[at+5]=='m')found16=true;at+=3+l;}
        assert(found2&&found16);}
    assert(sawMif);
    delete sp;delete r;
    printf("Session rules: OWLY-2 echo, data keeps a peer, A-MSDU bound, the macOS stack's service records in MIFs.\n");
}
// AWDL-18: a TCP SYN from a peer that is not valid yet is held and delivered once the peer turns valid.
static void testEarlySyn(){
    struct Host:HostPort{int up=0,eth=0;void hostEthernet(const uint8_t *,size_t) override {++eth;}void hostPeer(const Peer &p,bool added) override {if(added)++up;(void)p;}void hostSyncChanged() override {}}h;
    auto *r=new PlacementRadio;auto *sp=new Session(*r,h);Session &s=*sp;
    Addr self=Addr::of((const uint8_t *)"\x02\x11\x22\x33\x44\x55"),peer=Addr::of((const uint8_t *)"\x06\x77\x88\x99\xaa\xbb");
    s.configure("n",self.b,6);s.setNeighbourEcho(false);uint64_t t=1000;s.enable(t);s.poll(t);t+=1000;s.poll(t);assert(s.running());
    Engine &e=s.engine();e.state.filterRssi=false;
    // data frame with an IPv6 TCP SYN from a known, not-yet-valid peer
    Peer *p=nullptr;assert(e.state.peers.add(peer,t,&p,nullptr)>=0);
    uint8_t d[120];memset(d,0,sizeof d);put16le(d,kFcData);memcpy(d+4,self.b,6);memcpy(d+10,peer.b,6);memcpy(d+16,kAwdlBssid,6);
    uint8_t *m=d+24;memcpy(m,"\xaa\xaa\x03\x00\x17\xf2\x08\x00",8);put16le(m+8,kDataHead);put16be(m+14,kEtherTypeIPv6);m+=16;m[0]=0x60;m[6]=6;m[40+13]=0x02;
    size_t len=24+16+40+20;
    assert(e.receive(d,len,-40,t)==kRxOk);assert(s.stats().synsHeld==1 && h.eth==0);
    // the peer turns valid: the host gets the neighbor first, then the SYN
    s.poll(t);p=e.state.peers.find(peer);p->sentMif=true;p->version=0x34;p->devclass=1;bool valid=false;Peer *q=nullptr;e.state.peers.add(peer,t,&q,&valid);assert(valid);s.peerAdded(*q);
    assert(h.up==1 && h.eth==1 && s.stats().synsDelivered==1);
    // a held SYN expires after 2 s
    Addr other=Addr::of((const uint8_t *)"\x06\x01\x02\x03\x04\x05");e.state.peers.add(other,t,nullptr,nullptr);memcpy(d+10,other.b,6);assert(e.receive(d,len,-40,t)==kRxOk);assert(s.stats().synsHeld==2);
    t+=3000000;s.poll(t);assert(s.stats().synsExpired==1 && h.eth==1);
    delete sp;delete r;
    printf("Early SYN: held until the peer is valid, delivered after its neighbor entry, expired after 2 s.\n");
}
int main(){testCommands();testStateMachine();testRxFilter();testSession();testSharedRadio();testPlacement();testSharedSequence();testSessionRules();testEarlySyn();printf("AWDL radio binding and session tests passed.\n");return 0;}
