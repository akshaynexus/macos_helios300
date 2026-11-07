// Host test for the AWDL engine (AirportAth10kAwdl.hpp): byte-exact PSF/MIF/data frames and
// identical parse/sync/election results against vectors produced by OWL's own C code
// (awdl_vectors.inc, see awdl-vectors/), OWL's unit tests ported, and a two-node simulation
// (master + slave on a shared clock: election, synchronisation, channel following, unicast
// and multicast data delivery through the engine's schedule).
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "../src/AirportAth10kAwdl.hpp"
#include "awdl_vectors.inc"
using namespace AirportAwdl;

static Addr addr(uint8_t a,uint8_t b,uint8_t c,uint8_t d,uint8_t e,uint8_t f){Addr x{{a,b,c,d,e,f}};return x;}
static Addr addrHex(const char *h){Addr a;for(int i=0;i<6;++i){unsigned v;sscanf(h+2*i,"%2x",&v);a.b[i]=uint8_t(v);}return a;}
static const size_t kRadiotap=9; // OWL prepends a 9-byte radiotap header (rate only); the kext never sees one
static void expectFrame(const char *name,const uint8_t *got,size_t n,const uint8_t *vec,size_t vn){
    if(n+kRadiotap!=vn || memcmp(got,vec+kRadiotap,n)){
        printf("%s mismatch: got %zu bytes, OWL %zu\n",name,n,vn-kRadiotap);
        for(size_t i=0;i<n && i+kRadiotap<vn;++i)if(got[i]!=vec[i+kRadiotap]){printf(" first difference at %zu: %02x vs %02x\n",i,got[i],vec[i+kRadiotap]);break;}
        assert(false);
    }
}
struct Captured:RxEvents{std::vector<std::vector<uint8_t>> eth;int added=0,removed=0,syncs=0;
    void data(const uint8_t *e,size_t n,const Addr &,const Addr &) override {eth.emplace_back(e,e+n);}
    void peerAdded(const Peer &) override {++added;} void peerRemoved(const Peer &) override {++removed;} void syncChanged() override {++syncs;}};

static void testVectors(){
    const Addr self=addr(0x02,0x11,0x22,0x33,0x44,0x55),other=addr(0x02,0xaa,0xbb,0xcc,0xdd,0xee);
    State s;Ieee80211State is{};stateInit(s,"helios",self,Chan::opclass44(),5000000);
    s.sync.awCounter=1000;s.sync.lastUpdate=4000000;s.election.selfMetric=60;s.election.selfCounter=0;
    uint8_t buf[2048];size_t n;
    n=buildActionFrame(buf,sizeof buf,s,is,kActionPSF,5123456);expectFrame("PSF",buf,n,v_psf,sizeof v_psf);
    n=buildActionFrame(buf,sizeof buf,s,is,kActionMIF,5234567);expectFrame("MIF",buf,n,v_mif,sizeof v_mif);
    const uint8_t payload[]={0x60,0,0,0,0,8,0x3a,0xff,1,2,3,4,5,6,7,8};
    n=buildDataFrame(buf,sizeof buf,self,addr(0x33,0x33,0,0,0,1),kEtherTypeIPv6,payload,sizeof payload,s,is);expectFrame("DATA",buf,n,v_data,sizeof v_data);
    for(const auto &v:v_sync){
        assert(syncNextAwTu(v.now,s.sync)==v.nextAwTu);assert(syncNextAwUs(v.now,s.sync)==v.nextAwUs);
        assert(syncCurrentAw(v.now,s.sync)==v.curAw);assert(syncCurrentEaw(v.now,s.sync)==v.curEaw);
        assert(isMulticastEaw(s,v.now)==(v.mcast!=0));assert(canSendIn(s,v.now,16)==v.canSend16Us);
    }
    // the receiving node
    State r;stateInit(r,"peer",other,Chan::opclass6(),6000000);r.filterRssi=false;Captured ev;
    n=buildActionFrame(buf,sizeof buf,s,is,kActionPSF,6100000);expectFrame("PSF2",buf,n,v_psf2,sizeof v_psf2);
    assert(rx(buf,n,-40,6100000,r,&ev)==e_rx_psf);
    n=buildActionFrame(buf,sizeof buf,s,is,kActionMIF,6200000);expectFrame("MIF2",buf,n,v_mif2,sizeof v_mif2);
    assert(rx(buf,n,-40,6200000,r,&ev)==e_rx_mif);
    const Peer *p=r.peers.find(self);assert(p);
    assert(p->valid==(e_peer_valid!=0));assert(!strcmp(p->name,e_peer_name));assert(p->version==e_peer_ver);assert(p->devclass==e_peer_devclass);
    assert(p->supportsV2==(e_peer_v2!=0));assert(p->election.height==e_peer_height);assert(p->election.masterMetric==e_peer_master_metric);
    assert(p->election.selfMetric==e_peer_self_metric);assert(p->election.masterCounter==e_peer_master_counter);assert(p->election.selfCounter==e_peer_self_counter);
    assert(p->lastUpdate==e_peer_last_update);assert(p->election.masterAddr==addrHex(e_peer_master));assert(p->election.syncAddr==addrHex(e_peer_sync));
    for(int i=0;i<16;++i)assert(chanNum(p->sequence[i],p->seqEnc)==e_peer_seq[i]);
    assert(ev.added==1);
    electionRun(r.election,r.peers);
    assert(r.election.masterAddr==addrHex(e_elect_master));assert(r.election.height==e_elect_height);assert(r.election.masterMetric==e_elect_metric);assert(r.election.masterCounter==e_elect_counter);
    n=buildActionFrame(buf,sizeof buf,s,is,kActionPSF,6300000);expectFrame("PSF3",buf,n,v_psf3,sizeof v_psf3);
    assert(rx(buf,n,-40,6300000,r,&ev)==e_rx_psf3);
    assert(r.sync.lastUpdate==e_rsync_last_update && r.sync.awCounter==e_rsync_aw_counter); // s is not r's sync master: ignored
    n=buildDataFrame(buf,sizeof buf,self,other,kEtherTypeIPv6,payload,sizeof payload,s,is);expectFrame("DATA2",buf,n,v_data2,sizeof v_data2);
    assert(rx(buf,n,-40,6400000,r,&ev)==e_rx_data);assert(ev.eth.size()==1);
    assert(ev.eth[0].size()==14+sizeof payload && !memcmp(ev.eth[0].data(),v_eth,ev.eth[0].size()));
    for(size_t i=ev.eth[0].size();i<sizeof v_eth;++i)assert(v_eth[i]==0); // OWL pads its 1518-byte buffer with zeros
    // s becomes the better master
    s.election.selfMetric=s.election.masterMetric=100;s.election.selfCounter=s.election.masterCounter=2;
    n=buildActionFrame(buf,sizeof buf,s,is,kActionMIF,6500000);expectFrame("MIF4",buf,n,v_mif4,sizeof v_mif4);
    assert(rx(buf,n,-40,6500000,r,&ev)==e_rx_mif4);
    assert(electionRun(r.election,r.peers));
    assert(r.election.masterAddr==addrHex(e_elect2_master));assert(r.election.syncAddr==addrHex(e_elect2_sync));assert(r.election.height==e_elect2_height);
    assert(r.election.masterMetric==e_elect2_metric);assert(r.election.masterCounter==e_elect2_counter);
    n=buildActionFrame(buf,sizeof buf,s,is,kActionPSF,6600000);expectFrame("PSF5",buf,n,v_psf5,sizeof v_psf5);
    assert(rx(buf,n,-40,6600000,r,&ev)==e_rx_psf5);
    assert(r.sync.lastUpdate==e_rsync2_last_update);assert(r.sync.awCounter==e_rsync2_aw_counter);assert(r.sync.measErr==e_rsync2_err);assert(r.sync.measTotal==e_rsync2_total);
    assert(ev.syncs==1);
    auto check=[&](const auto &t){assert(syncNextAwTu(t.now,r.sync)==t.nextAwTu);assert(syncCurrentEaw(t.now,r.sync)==t.curEaw);assert(sameChannelAsPeer(r,t.now,*p)==(t.sameChan!=0));};
    check(e_rsynct_6600000);check(e_rsynct_6641000);check(e_rsynct_6682000);check(e_rsynct_6723000);check(e_rsynct_6764000);
    printf("OWL vectors: PSF/MIF/data frames byte-exact; parse, election, sync and schedule results identical.\n");
}

// OWL's own unit tests (tests/test_awdl_sync.cpp, test_awdl_election.cpp, test_awdl_peers.cpp).
static void testOwlUnit(){
    {SyncState st;syncInit(st,0);uint64_t now=0;for(uint64_t tu=0;tu<4*64;++tu)for(;now<tuToUs(tu+1);now+=97)assert(syncNextAwTu(now,st)==64-(tu%64));}
    {for(uint16_t aw:{uint16_t(0),uint16_t(1337),uint16_t(0xffff)}){SyncState st;syncInit(st,0);st.awCounter=aw;assert(syncCurrentAw(0,st)==aw);}}
    {SyncState st;syncInit(st,0);uint64_t tu=0;for(uint32_t aw=0;aw<0xffff;++aw)for(;tu<(aw+1u)*st.awPeriod;tu+=3)assert(syncCurrentAw(tuToUs(tu),st)==aw);}
    const Addr a0=addr(0,0,0,0,0,0),a1=addr(1,1,1,1,1,1);
    {ElectionState s;electionInit(s,a0);assert(s.selfCounter==0&&s.selfMetric==60&&s.masterCounter==0&&s.masterMetric==60&&s.height==0);assert(s.masterAddr==a0&&s.syncAddr==a0&&s.selfAddr==a0);}
    auto addValid=[](PeerTable &t,const Addr &a){Peer *p;t.add(a,0,&p,nullptr);p->valid=true;return p;};
    {ElectionState s;PeerTable t;t.init();electionInit(s,a0);addValid(t,a1);electionRun(s,t);assert(s.masterAddr==a1);}
    {ElectionState s;PeerTable t;t.init();electionInit(s,a0);t.add(a1,0,nullptr,nullptr);electionRun(s,t);assert(s.masterAddr==a0);}
    {ElectionState s;PeerTable t;t.init();electionInit(s,a0);Peer *p=addValid(t,a1);
        electionRun(s,t);assert(s.masterAddr==a1);p->election.masterMetric=1000;electionRun(s,t);assert(s.masterAddr==a1);
        s.selfMetric=1000;electionRun(s,t);assert(s.masterAddr==a1);s.selfMetric=1001;electionRun(s,t);assert(s.masterAddr==a0);
        p->election.masterCounter=1;electionRun(s,t);assert(s.masterAddr==a1);s.selfCounter=1;electionRun(s,t);assert(s.masterAddr==a0);}
    {ElectionState s;PeerTable t;t.init();electionInit(s,a0);Peer *p=addValid(t,a1);p->election.masterMetric=p->election.selfMetric=1000;p->election.height=1;p->election.masterAddr=a0;
        electionRun(s,t);assert(s.masterAddr==a0);}
    {PeerTable t;t.init();assert(t.length()==0);assert(t.add(a0,0,nullptr,nullptr)==kPeersOk);assert(t.length()==1);assert(t.add(a1,0,nullptr,nullptr)==kPeersOk);assert(t.length()==2);
        assert(t.add(a0,0,nullptr,nullptr)==kPeersUpdate);assert(t.length()==2);assert(t.remove(a0,nullptr)==kPeersOk);assert(t.remove(a0,nullptr)==kPeersMissing);assert(t.length()==1);}
    {PeerTable t;t.init();for(int i=0;i<kMaxPeers;++i)assert(t.add(addr(2,0,0,0,0,uint8_t(i)),0,nullptr,nullptr)==kPeersOk);assert(t.add(addr(2,0,0,0,1,0),0,nullptr,nullptr)==kPeersFull);}
    printf("OWL unit tests (sync, election, peers) pass on the port.\n");
}

// Two engines on a shared clock. Frames cross the "air" only when both are on the same channel.
struct Node;
struct Air{std::vector<Node*> nodes;};
struct Node:Sink{
    Engine e;Air *air;uint8_t chan=0;std::vector<uint8_t> channels;Captured got;size_t txAction=0,txData=0;
    void setChannel(uint8_t c) override {chan=c;channels.push_back(c);}
    bool transmit(const uint8_t *f,size_t n,bool action) override;
    void data(const uint8_t *d,size_t n,const Addr &s,const Addr &t) override {got.data(d,n,s,t);}
    void peerAdded(const Peer &p) override {got.peerAdded(p);} void peerRemoved(const Peer &p) override {got.peerRemoved(p);} void syncChanged() override {got.syncChanged();}
};
static uint64_t g_now=0;
bool Node::transmit(const uint8_t *f,size_t n,bool action){
    if(action)++txAction;else ++txData;
    for(Node *o:air->nodes)if(o!=this && o->chan==chan && o->chan)o->e.receive(f,n,-45,g_now);
    return true;
}
static void testSimulation(){
    Air air;Node a,b;a.air=b.air=&air;air.nodes={&a,&b};
    const Addr aa=addr(0x02,1,1,1,1,1),ab=addr(0x02,2,2,2,2,2);
    g_now=10000000;
    a.e.start("alpha",aa,Chan::opclass44(),g_now,&a);b.e.start("bravo",ab,Chan::opclass44(),g_now+3333333,&b);
    a.e.state.election.selfMetric=a.e.state.election.masterMetric=500;a.e.state.election.selfCounter=a.e.state.election.masterCounter=7; // alpha is the better master
    // bravo's clock starts unsynchronised: its availability windows are offset from alpha's
    b.e.state.sync.lastUpdate=g_now+777777;b.e.state.sync.awCounter=4000;
    auto run=[&](uint64_t until){while(g_now<until){uint64_t na=a.e.tick(g_now),nb=b.e.tick(g_now);uint64_t next=na<nb?na:nb;if(next<=g_now)next=g_now+1;if(next>until)next=until;g_now=next;}};
    run(g_now+6000000); // 6 s
    assert(a.chan==44 && b.chan==44);
    // both learned about each other, bravo elected alpha, alpha stays master
    assert(a.e.state.peers.find(ab) && a.e.state.peers.find(ab)->valid);
    assert(b.e.state.peers.find(aa) && b.e.state.peers.find(aa)->valid);
    assert(b.e.state.election.masterAddr==aa && b.e.state.election.height==1);
    assert(a.e.state.election.masterAddr==aa && a.e.state.election.height==0);
    assert(b.e.state.psfInterval==kPsfIntervalSlaveTu && a.e.state.psfInterval==kPsfIntervalMasterTu);
    // bravo synchronised its AW counter/phase to alpha within the threshold
    assert(b.got.syncs>0);
    int64_t err=int64_t(syncNextAwTu(g_now,a.e.state.sync))-int64_t(syncNextAwTu(g_now,b.e.state.sync));
    assert(err<=kSyncThresholdTu && err>=-kSyncThresholdTu);
    assert((syncCurrentAw(g_now,a.e.state.sync)&0xfffc)==(syncCurrentAw(g_now,b.e.state.sync)&0xfffc));
    assert(a.txAction>=50 && b.txAction>=10);
    // unicast bravo -> alpha and multicast alpha -> all, through the schedule
    std::vector<uint8_t> eth(14+40,0);memcpy(eth.data(),aa.b,6);memcpy(eth.data()+6,ab.b,6);put16be(eth.data()+12,kEtherTypeIPv6);eth[14]=0x60;eth[53]=0x5a;
    assert(b.e.send(eth.data(),eth.size(),g_now));
    std::vector<uint8_t> mc(14+40,0);mc[0]=0x33;mc[1]=0x33;mc[5]=1;memcpy(mc.data()+6,aa.b,6);put16be(mc.data()+12,kEtherTypeIPv6);mc[14]=0x60;mc[53]=0x7e;
    assert(a.e.send(mc.data(),mc.size(),g_now));
    run(g_now+1500000);
    assert(a.got.eth.size()==1 && a.got.eth[0]==eth);
    assert(b.got.eth.size()==1 && b.got.eth[0]==mc);
    assert(a.e.state.stats.rxData==1 && b.e.state.stats.rxData==1 && b.e.state.stats.txDataUnicast==1 && a.e.state.stats.txDataMulticast==1);
    // a frame to an unknown peer is dropped, one to self is looped back
    std::vector<uint8_t> unk=eth;unk[5]=0x77;assert(b.e.send(unk.data(),unk.size(),g_now));std::vector<uint8_t> self=eth;memcpy(self.data(),ab.b,6);assert(b.e.send(self.data(),self.size(),g_now));
    run(g_now+200000);assert(b.e.unicast.empty() && b.got.eth.size()==2 && b.got.eth[1]==self);
    // alpha goes away: bravo removes it after the timeout and becomes master again
    a.e.stop();run(g_now+3500000);
    assert(!b.e.state.peers.find(aa) && b.got.removed==1 && b.e.state.election.masterAddr==ab && b.e.state.psfInterval==kPsfIntervalMasterTu);
    // channel following: a master with the default 149/6 sequence; the slave adopts the master's schedule timing (its own sequence stays static, as in OWL)
    printf("Simulation: two engines elect, synchronise (|error| <= %d TU), exchange unicast + multicast data and recover from peer loss.\n",kSyncThresholdTu);
}

static void testRobustness(){
    State r;stateInit(r,"x",addr(2,0,0,0,0,1),Chan::opclass6(),0);r.filterRssi=false;
    // truncated frames and TLVs never read out of bounds (ASan) and are rejected
    uint8_t buf[600];State s;Ieee80211State is{};stateInit(s,"y",addr(2,0,0,0,0,2),Chan::opclass44(),0);
    size_t n=buildActionFrame(buf,sizeof buf,s,is,kActionMIF,123456);assert(n>100);
    // (a cut on a TLV boundary parses as a shorter but well-formed frame, as in OWL)
    for(size_t cut=0;cut<n;++cut){int res=rx(buf,cut,-40,1000,r,nullptr);assert(res<=kRxIgnorePeer && res>=kRxPeersFull);}
    assert(rx(buf,n,-40,2000,r,nullptr)==kRxOk);
    // RSSI filter: unknown peers below the threshold are ignored, known ones get the grace
    State f;stateInit(f,"f",addr(2,0,0,0,0,3),Chan::opclass6(),0);
    assert(rx(buf,n,-70,3000,f,nullptr)==kRxIgnoreRssi);assert(rx(buf,n,-60,3000,f,nullptr)==kRxOk);assert(rx(buf,n,-68,3000,f,nullptr)==kRxOk);assert(rx(buf,n,-71,3000,f,nullptr)==kRxIgnoreRssi);
    // a too-small buffer yields 0, never a partial frame
    assert(buildActionFrame(buf,100,s,is,kActionPSF,1)==0);
    // frames from self and foreign management are ignored
    assert(rx(buf,n,-40,4000,s,nullptr)==kRxIgnoreFromSelf);
    uint8_t beacon[40]={0x80,0};assert(rx(beacon,sizeof beacon,-40,1,r,nullptr)==kRxUnexpectedType);
    // A-MSDU data: two subframes
    const uint8_t pl1[]={0x60,1},pl2[]={0x60,2,3};uint8_t d[300];Writer w(d,sizeof d);Ieee80211State js{};
    initAwdlHeader(w,s.self,r.self,js,kFcQosData);w.le16(kQosAmsduPresent);
    for(int k=0;k<2;++k){const uint8_t *pl=k?pl2:pl1;size_t pn=k?sizeof pl2:sizeof pl1;size_t start=w.len;w.addr(r.self);w.addr(s.self);w.be16(uint16_t(8+8+pn));
        w.u8(0xaa);w.u8(0xaa);w.u8(0x03);w.bytes(kAwdlOui,3);w.be16(kLlcProtocolId);w.le16(kDataHead);w.le16(0);w.le16(0);w.be16(kEtherTypeIPv6);w.bytes(pl,pn);
        if(k==0)w.zeros((4-((w.len-start)%4))%4);}
    Captured ev;assert(rx(d,w.len,-40,5000,r,&ev)==kRxOk);assert(ev.eth.size()==2 && ev.eth[1].size()==14+sizeof pl2 && ev.eth[1][16]==3);
    printf("Robustness: truncation, RSSI filter, buffer limits, foreign frames and A-MSDU handled.\n");
}

int main(){testVectors();testOwlUnit();testSimulation();testRobustness();printf("AWDL engine tests passed.\n");return 0;}
