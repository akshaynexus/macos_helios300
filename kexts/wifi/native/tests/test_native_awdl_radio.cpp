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
        awdlPlan(r,infraIdle,now);
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
    bool radioStart(const uint8_t *m,uint8_t c) override {if(refuseStart)return false;if(failNext){failNext=false;st=RadioStep::Failed;return true;}memcpy(mac,m,6);channel=c;st=RadioStep::Running;return true;}
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
struct SimNode{SimRadio radio;SimHost host;Session session;uint64_t next=0;SimNode():session(radio,host){}};

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

int main(){testCommands();testStateMachine();testRxFilter();testSession();printf("AWDL radio binding and session tests passed.\n");return 0;}
