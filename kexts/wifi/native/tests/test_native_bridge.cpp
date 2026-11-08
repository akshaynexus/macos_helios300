// Host test for the native front end's logic: apple80211/WCL request mapping,
// scan-result conversion, key install and the join state machine, driven
// against a simulated ath10k control ABI that replays the shared WPA2 vectors.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include "../src/AirportAth10kCrypto.hpp"
#include "../src/AirportAth10kBridge.hpp"
#include "../../tests/security_vectors.inc"
#include "Ath10kScanPlan.hpp"
static bool testRandom(uint8_t *p,size_t n){for(size_t i=0;i<n;++i)p[i]=uint8_t(i);return true;} // = v_snonce
using Crypto=AirportCrypto::Policy<testRandom>;
using Session=AirportBridge::Session<Crypto>;
namespace B=AirportBridge;namespace W=AirportWCL;
struct Message{uint32_t type;std::vector<uint8_t> data;};
struct FakeSink:B::Sink{
    std::vector<Message> messages;std::vector<bool> links;
    void post(uint32_t m,const void *d,size_t n) override {Message e{m,{}};if(n)e.data.assign((const uint8_t *)d,(const uint8_t *)d+n);messages.push_back(e);}
    void linkState(bool up) override {links.push_back(up);}
    size_t count(uint32_t t) const {size_t c=0;for(auto &m:messages)if(m.type==t)++c;return c;}
    const Message *last(uint32_t t) const {for(size_t i=messages.size();i>0;--i)if(messages[i-1].type==t)return &messages[i-1];return nullptr;}
};
// Simulated core: the subset of Ath10kUserClient.cpp controlAction the front end uses.
struct FakeCore:B::Core{
    bool reachable=true,runtime=true,scanDone=true,scanning=false,link=false,txBusy=false,disconnecting=false;
    uint32_t stage=0,phase=0,generation=1,stateBits=0,psBits=0,beaconMisses=0;int32_t rssi=-55;
    std::vector<B::Network> bss;std::vector<std::vector<uint8_t>> eapolIn;std::vector<std::vector<uint8_t>> eapolOut;
    std::vector<uint8_t> installedKeys;bool authorized=false,secure=false,rejectConnect=false,failAfterConnect=false,connected=false;
    unsigned scans=0,cancels=0,disconnects=0,restarts=0,psRequests=0,macChanges=0;uint32_t lastPS=9;int connectIndex=-1;std::string error="none";uint8_t stationMAC[6]={2,1,2,3,4,5};
    static void le32(uint8_t *p,uint32_t v){for(int i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
    int call(const AthControl::Request &r,AthControl::Response &o) override {
        using namespace AthControl;
        if(!reachable)return B::kNotReady;
        o=Response{};o.stage=stage;o.phase=phase;o.count=uint32_t(bss.size());memcpy(o.local,stationMAC,6);
        o.flags=128|512|(runtime?1:0)|(scanDone?2:0)|(link?4:0)|(txBusy?8:0)|(secure?16:0)|(disconnecting?64:0)|(scanning?256:0);
        strncpy(o.error,error.c_str(),95);
        if(r.operation==Status)return 0;
        if(r.operation==Info){le32(o.data,0x2ff|256);le32(o.data+4,generation);le32(o.data+8,44);le32(o.data+12,uint32_t(rssi));le32(o.data+16,80);le32(o.data+20,2);
            le32(o.data+28,stateBits);le32(o.data+84,psBits);le32(o.data+88,beaconMisses);le32(o.data+92,46);le32(o.data+108,scanDone?1:0);o.data[112]='U';o.data[113]='S';o.data[115]=1;o.length=116;return 0;}
        if(r.operation==BSS){
            if(r.index>=bss.size())return B::kNotFound;
            const auto &b=bss[r.index];o.data[0]=b.ssidLength;o.data[1]=b.privacy;memcpy(o.data+2,b.bssid,6);memcpy(o.data+8,b.ssid,32);o.data[40]=b.channel;o.data[41]=b.wpa2;
            B::put16(o.data+42,b.rsnLength);memcpy(o.data+44,b.rsn,b.rsnLength);uint32_t at=44+b.rsnLength;le32(o.data+at,b.snr);at+=4;le32(o.data+at,b.hidden?2:0);at+=4;
            B::put16(o.data+at,b.capabilities);B::put16(o.data+at+2,b.beaconInterval);o.data[at+4]=b.rateCount;memcpy(o.data+at+5,b.rates,32);
            o.data[at+37]=uint8_t((b.htPresent?1:0)|(b.htOpPresent?2:0)|(b.vhtPresent?4:0)|(b.vhtOpPresent?8:0));
            memcpy(o.data+at+38,b.ht,26);memcpy(o.data+at+64,b.htOp,22);memcpy(o.data+at+86,b.vht,12);memcpy(o.data+at+98,b.vhtOp,5);o.data[at+103]=b.powerConstraintPresent;o.data[at+104]=b.powerConstraintDb;
            o.length=at+105;return 0;}
        if(r.operation==Restart){++restarts;runtime=false;return 0;}
        if(!runtime)return B::kNotReady;
        if(r.operation==Scan){if(r.index==1){scanning=false;return 0;}if(stage && !(stage==10 && link))return B::kBusy;++scans;scanDone=false;scanning=true;++generation;return 0;}
        if(r.operation==RadioOptions){++psRequests;lastPS=r.index;psBits=r.index?3:0;return 0;}
        if(r.operation==Connect){
            if(stage)return B::kExclusiveAccess;
            if(!scanDone)return B::kNotReady;
            if(r.index&2){ // extended block: only the station-MAC-only form is accepted with the gate off
                const uint8_t *x=r.data+8+r.data[0];
                if(r.length!=18u+r.data[0] || x[0]!=16 || x[1] || x[8] || x[9])return B::kUnsupported;
                memcpy(stationMAC,x+2,6);macChanges++;
            } else if(r.length!=8u+r.data[0] || r.data[1]>1)return B::kBadArgument;
            if(r.data[1]>1)return B::kBadArgument;
            bool found=false;for(auto &b:bss)if(!memcmp(b.bssid,r.data+2,6))found=true;
            if(!found || rejectConnect){error="bss-not-found";return B::kUnsupported;}
            connectIndex=int(r.index);secure=r.data[1];stage=1;error="none";return 0;}
        if(r.operation==Disconnect){if(stage!=10 || !link)return B::kBusy;++disconnects;disconnecting=true;link=false;return 0;}
        if(r.operation==Cancel){++cancels;stage=0;link=false;secure=false;return 0;}
        if(r.operation==ReceiveEAPOL){if(eapolIn.empty())return B::kNotReady;o.length=uint32_t(eapolIn.front().size());memcpy(o.data,eapolIn.front().data(),o.length);eapolIn.erase(eapolIn.begin());return 0;}
        if(r.operation==SendEAPOL){if(phase && phase!=3 && phase!=5)return B::kNotReady;eapolOut.push_back({r.data,r.data+r.length});return 0;}
        if(r.operation==InstallKeys){if((r.length!=56 && r.length!=81) || phase)return B::kBadArgument;installedKeys.assign(r.data,r.data+r.length);phase=1;return 0;}
        if(r.operation==Authorize){if(phase!=3)return B::kNotReady;authorized=true;return 0;}
        return B::kUnsupported;
    }
    // One step of the simulated adapter.
    void advance(){
        if(scanning){scanning=false;scanDone=true;}
        if(disconnecting){disconnecting=false;stage=0;secure=false;connected=false;}
        if(stage && stage<10){if(failAfterConnect){stage=11;error="association-rejected";}else{++stage;if(stage==10){connected=true;if(secure)eapolIn.push_back({v_m1,v_m1+sizeof(v_m1)});else link=true;}}}
        if(stage==10 && secure){
            if(!eapolOut.empty() && eapolOut.back().size()==sizeof(v_m2) && !memcmp(eapolOut.back().data(),v_m2,sizeof(v_m2)) && eapolIn.empty() && eapolOut.size()==1)eapolIn.push_back({v_m3,v_m3+sizeof(v_m3)});
            if(phase==1)phase=3;
            if(authorized && phase==3){phase=5;link=true;}
        }
    }
};
static B::Network network(const uint8_t *bssid,const char *ssid,uint8_t channel,bool wpa2){
    B::Network n{};memcpy(n.bssid,bssid,6);n.ssidLength=uint8_t(strlen(ssid));memcpy(n.ssid,ssid,n.ssidLength);n.channel=channel;n.snr=40;n.capabilities=0x411;n.beaconInterval=100;
    n.rateCount=8;const uint8_t rates[]={0x8c,0x12,0x98,0x24,0xb0,0x48,0x60,0x6c};memcpy(n.rates,rates,8);
    if(wpa2){n.privacy=n.wpa2=true;n.capabilities|=0x10;n.rsnLength=sizeof(v_rsn);memcpy(n.rsn,v_rsn,sizeof(v_rsn));}
    n.htPresent=true;n.ht[0]=0xef;n.ht[1]=0x19;n.htOpPresent=true;n.htOp[0]=channel;return n;
}
static W::JoinRequest join(const B::Network &n,bool personal){
    W::JoinRequest j{};j.lowerAuth=1;j.upperAuth=personal?(1u<<3):0;j.ssidLength=n.ssidLength;memcpy(j.ssid,n.ssid,32);memcpy(j.bssid,n.bssid,6);
    if(personal){j.keyLength=32;j.keyCipher=6;memcpy(j.key,v_pmk,32);}
    j.channelSpec=B::channelSpec(n.channel);return j;
}
int main(){
    // The channel table mirrors the core's scan plan.
    assert(B::kChannelCount==Ath10k::kWMIScanChannelCount);
    for(unsigned i=0;i<B::kChannelCount;++i)assert(B::kChannels[i].number==Ath10k::kWMIScanPlan[i].channel && B::kChannels[i].mhz==Ath10k::kWMIScanPlan[i].frequency);
    for(unsigned c=1;c<200;++c)assert(B::channelDFS(c)==Ath10k::channelNeedsDFS(c) || !Ath10k::wmiScanChannelAllowed(c));
    const uint8_t open_bssid[]={2,6,7,8,9,11};
    B::Network wpa=network(v_ap,"home",44,true),open=network(open_bssid,"cafe",6,false);
    { // Scan-result conversion: metadata, IE order, transition-mode RSN rewrite, synthesized beacon.
        uint8_t out[1024];size_t n=B::buildScanResult(wpa,out,sizeof(out));
        auto *m=reinterpret_cast<W::BeaconMetadata *>(out);assert(n==sizeof(*m)+m->ieLength);
        assert(m->channelSpec==0xd02c && m->channel==44 && m->ssidLength==4 && !memcmp(m->ssid,"home",4) && !memcmp(m->bssid,v_ap,6));
        assert(m->rssi==-55 && m->interval==100 && (m->capability&0x10) && m->flags==(W::SSIDPresent|W::RSSIValid));
        const uint8_t *ie=out+sizeof(*m);assert(W::validIEs(ie,m->ieLength));
        assert(ie[0]==0 && ie[1]==4 && ie[6]==1 && ie[7]==8 && ie[16]==3 && ie[17]==1 && ie[18]==44 && ie[19]==45 && ie[20]==26 && ie[47]==61 && ie[48]==22 && ie[71]==48 && ie[72]==20);
        B::Network hidden=wpa;hidden.hidden=true;n=B::buildScanResult(hidden,out,sizeof(out));assert(n && m->ssidLength==0 && ie[1]==0);
        B::Network transition=wpa; // RSN with PSK + SAE AKMs, PMF capable: WCL must see plain WPA2-PSK
        const uint8_t rsn[]={48,24,1,0,0,15,172,4,1,0,0,15,172,4,2,0,0,15,172,2,0,15,172,8,0x80,0};
        transition.rsnLength=sizeof(rsn);memcpy(transition.rsn,rsn,sizeof(rsn));
        n=B::buildScanResult(transition,out,sizeof(out));assert(n);
        const uint8_t *r=out+sizeof(*m)+71;assert(r[0]==48 && r[1]==20 && r[14]==1 && r[19]==2 && r[20]==0x80);
        uint8_t beacon[1024];n=B::buildBeaconFrame(wpa,123456,7,beacon,sizeof(beacon));
        uint32_t length;memcpy(&length,beacon,4);assert(n==4+length && beacon[4]==0x80 && !memcmp(beacon+4+10,v_ap,6) && !memcmp(beacon+4+16,v_ap,6));
        assert(beacon[4+24]==0x40 && beacon[4+25]==0xe2 && beacon[4+32]==100 && beacon[4+36]==0);
        B::Network decoded;AthControl::Response o;FakeCore fc;fc.bss={wpa};AthControl::Request q{};q.operation=AthControl::BSS;fc.call(q,o);
        assert(B::decodeNetwork(o.data,o.length,decoded) && decoded.wpa2 && decoded.rsnLength==sizeof(v_rsn) && decoded.htPresent && decoded.ht[0]==0xef && decoded.rateCount==8 && decoded.beaconInterval==100);
    }
    { // Request mapping.
        B::JoinPlan plan;W::JoinRequest j=join(wpa,true);
        assert(!B::planJoin(j,&wpa,plan) && plan.secure && !memcmp(plan.pmk,v_pmk,32) && !memcmp(plan.bssid,v_ap,6) && plan.ssidLength==4);
        AthControl::Request r;B::buildConnectRequest(plan,r);assert(r.operation==AthControl::Connect && r.index==0 && r.length==12 && r.data[0]==4 && r.data[1]==1 && !memcmp(r.data+2,v_ap,6) && !memcmp(r.data+8,"home",4));
        assert(!B::planJoin(join(open,false),&open,plan) && !plan.secure);
        assert(!strcmp(B::planJoin(join(wpa,false),&wpa,plan),"protected-network-not-supported"));
        B::Network sae=wpa;sae.wpa2=false;assert(!strcmp(B::planJoin(join(sae,true),&sae,plan),"wpa2-psk-ccmp-pairwise-required"));
        j=join(wpa,true);j.upperAuth=1u<<2;j.keyLength=0;assert(!strcmp(B::planJoin(j,&wpa,plan),"enterprise-not-supported"));
        j=join(wpa,true);j.upperAuth=1u<<12;assert(!strcmp(B::planJoin(j,&wpa,plan),"auth-not-supported"));
        j=join(wpa,true);j.ssid[0]='x';assert(!strcmp(B::planJoin(j,&wpa,plan),"ssid-mismatch"));
        assert(!B::planJoin(join(wpa,true),nullptr,plan)); // unknown BSS: decided after a scan
    }
    { // Full WPA2 join: ready, scan, join, in-kernel handshake, keys, link, LQM; then AP deauth.
        FakeCore core;FakeSink sink;Session s(core,sink);core.bss={wpa,open};
        uint64_t t=1000;s.poll(t);
        assert(s.state()==Session::State::Ready && sink.count(W::DriverAvailable)==1 && sink.count(W::ResetInterface)==1 && s.networkCount()==2);
        W::ScanRequest scan{};assert(s.startScan(scan,t)==B::kSuccess && core.scans==1 && s.scanning());
        assert(s.startScan(scan,t)==B::kBusy);
        s.poll(t+=100);assert(s.scanning());core.advance();s.poll(t+=100);
        assert(!s.scanning() && sink.count(W::ScanResult)==2 && sink.count(W::ScanDone)==1 && *reinterpret_cast<const int *>(sink.last(W::ScanDone)->data.data())==0);
        assert(s.startJoin(join(wpa,true),t)==B::kSuccess && s.joining());
        assert(s.startJoin(join(wpa,true),t)==B::kBusy);
        for(int i=0;i<12 && core.stage!=10;++i){s.poll(t+=100);core.advance();}
        assert(core.stage==10 && core.connectIndex==0 && core.secure);
        s.poll(t+=100); // M1 arrives -> M2 sent; assoc events
        assert(sink.count(W::AssocComplete)==1 && sink.count(W::LinkStatus)==1 && sink.count(W::FirstBeacon)==1 && sink.links.size()==1 && sink.links[0]);
        assert(core.eapolOut.size()==1 && core.eapolOut[0].size()==sizeof(v_m2) && !memcmp(core.eapolOut[0].data(),v_m2,sizeof(v_m2)));
        core.advance();s.poll(t+=100); // M3 -> keys installed, M4 queued until phase 3
        assert(core.installedKeys.size()==(AIRPORT_ATH10K_WPASEC?81u:56u) && !memcmp(core.installedKeys.data(),v_ptk+32,16) && !memcmp(core.installedKeys.data()+16,v_gtk,16) && core.installedKeys[48]==4);
        assert(core.eapolOut.size()==1);
        core.advance();s.poll(t+=100); // phase 3: M4 sent and Authorize requested
        assert(core.eapolOut.size()==2 && core.eapolOut[1].size()==sizeof(v_m4) && !memcmp(core.eapolOut[1].data(),v_m4,sizeof(v_m4)) && core.authorized);
        core.advance();s.poll(t+=100); // link up -> ConnectComplete(0)
        assert(s.state()==Session::State::Connected && s.linkUp() && sink.count(W::ConnectComplete)==1);
        auto *c=reinterpret_cast<const W::ConnectionEvent *>(sink.last(W::ConnectComplete)->data.data());assert(c->status==0 && !memcmp(c->peers[0].bssid,v_ap,6));
        assert(core.psRequests==1 && core.lastPS==1 && s.lqmQuality()==100);
        size_t lqm=sink.count(W::LqmUpdate);s.poll(t+=2500);assert(sink.count(W::LqmUpdate)==lqm+1);
        uint8_t info[2048];size_t n;assert(s.bssInfo(info,sizeof(info),n) && n>sizeof(W::BeaconMetadata));
        assert(s.beaconInfo(info,sizeof(info),t) && s.beaconInfo(info,sizeof(info),t+100));
        assert(!strcmp(s.core_().country,"US") && s.core_().channel==44 && s.core_().width==80 && s.core_().nss==2);
        // AP drops the link: core reports stage 0 without a disconnect in progress.
        core.link=false;core.stage=0;core.secure=false;core.beaconMisses=5;s.poll(t+=100);
        assert(s.state()==Session::State::Ready && sink.count(W::Deauth)==1 && sink.links.size()==2 && !sink.links[1]);
        auto *d=reinterpret_cast<const W::DeauthEvent *>(sink.last(W::Deauth)->data.data());assert(d->reason==4 && !memcmp(d->bssid,v_ap,6));
    }
    { // Private Wi-Fi Address: WCL sets a station MAC; the join carries it (extended Connect, flags 16),
      // the supplicant uses it, and clearing it (permanent address) sends a plain Connect again.
        FakeCore core;FakeSink sink;Session s(core,sink);core.bss={wpa};const uint8_t permanent[]={2,9,9,9,9,9};memcpy(core.stationMAC,permanent,6);
        uint64_t t=3000;s.poll(t);
        s.setStationMAC(v_sta); // the handshake vectors are for v_sta as the station
        assert(s.startJoin(join(wpa,true),t)==B::kSuccess);
        for(int i=0;i<12 && core.stage!=10;++i){s.poll(t+=100);core.advance();}
        assert(core.macChanges==1 && !memcmp(core.stationMAC,v_sta,6) && core.connectIndex==2);
        for(int i=0;i<12 && !s.linkUp();++i){s.poll(t+=100);core.advance();}
        assert(s.linkUp() && core.eapolOut.size()==2 && !memcmp(core.eapolOut[0].data(),v_m2,sizeof(v_m2)));
        core.link=false;core.stage=0;core.secure=false;s.poll(t+=100);assert(s.state()==Session::State::Ready);
        // WCL restores the permanent address while the vdev still has the private one: the next join carries it.
        s.setStationMAC(permanent);
        B::Network open2=open;core.bss={wpa,open2};s.poll(t+=100);
        assert(s.startJoin(join(open2,false),t)==B::kSuccess);
        for(int i=0;i<12 && core.stage!=10;++i){s.poll(t+=100);core.advance();}
        assert(core.macChanges==2 && !memcmp(core.stationMAC,permanent,6) && core.connectIndex==2);
        core.link=false;core.stage=0;s.poll(t+=100);
        // Same address as the vdev: a plain Connect.
        assert(s.startJoin(join(open2,false),t)==B::kSuccess);
        for(int i=0;i<12 && core.stage!=10;++i){s.poll(t+=100);core.advance();}
        assert(core.macChanges==2 && core.connectIndex==0);
    }
    { // Open join, voluntary leave, join of a BSS missing from the cache (scan first), rejection, timeout, abort.
        FakeCore core;FakeSink sink;Session s(core,sink);core.bss={open};
        uint64_t t=5000;s.poll(t);
        assert(s.startJoin(join(open,false),t)==B::kSuccess);
        for(int i=0;i<12 && !s.linkUp();++i){s.poll(t+=100);core.advance();}
        s.poll(t+=100);assert(s.linkUp() && core.connectIndex==0 && !core.secure && sink.count(W::ConnectComplete)==1);
        assert(s.leaveNetwork(t)==B::kSuccess && s.state()==Session::State::Leaving && core.disconnects==1 && sink.links.back()==false);
        core.advance();s.poll(t+=100);assert(s.state()==Session::State::Ready);
        // WCL asks for a BSSID that is not in the last scan: a scan runs first, then the join proceeds.
        core.bss={open,wpa};
        assert(s.startJoin(join(wpa,true),t)==B::kSuccess && s.joining());
        s.poll(t+=100);assert(core.scans==1 && s.scanning());core.advance();s.poll(t+=100);
        assert(!s.scanning() && sink.count(W::ScanDone)==0 && core.stage>=1);
        for(int i=0;i<30 && !s.linkUp();++i){s.poll(t+=100);core.advance();}
        assert(s.linkUp() && sink.count(W::ConnectComplete)==2);
        // Association rejected by the AP.
        core.link=false;core.stage=0;core.secure=false;s.poll(t+=100);assert(s.state()==Session::State::Ready);
        core.failAfterConnect=true;assert(s.startJoin(join(open,false),t)==B::kSuccess);
        for(int i=0;i<6 && s.joining();++i){s.poll(t+=100);core.advance();}
        assert(!s.joining() && s.state()==Session::State::Ready && core.cancels>=1 && core.stage==0);
        auto *c=reinterpret_cast<const W::ConnectionEvent *>(sink.last(W::ConnectComplete)->data.data());assert(c->status==1);
        // Timeout: the core never associates.
        core.failAfterConnect=false;assert(s.startJoin(join(open,false),t)==B::kSuccess);s.poll(t+=100);
        s.poll(t+=31000);assert(!s.joining());c=reinterpret_cast<const W::ConnectionEvent *>(sink.last(W::ConnectComplete)->data.data());assert(c->status==1 && c->reason==1004);
        core.stage=0;
        // Abort.
        assert(s.startJoin(join(open,false),t)==B::kSuccess);assert(s.abortJoin(t)==B::kSuccess && sink.count(W::JoinAbortComplete)==1 && !s.joining());
        // Unsupported requests are refused synchronously.
        W::JoinRequest j=join(wpa,true);j.upperAuth=1u<<12;assert(s.startJoin(j,t)==B::kUnsupported && !strcmp(s.lastError,"auth-not-supported"));
    }
    { // Offline core, wake restart request, power off.
        FakeCore core;FakeSink sink;Session s(core,sink);core.bss={open};core.reachable=false;
        uint64_t t=9000;s.poll(t);assert(s.state()==Session::State::Offline && sink.count(W::DriverAvailable)==0);
        W::ScanRequest scan{};assert(s.startScan(scan,t)==B::kNotReady);
        core.reachable=true;s.poll(t+=100);assert(s.state()==Session::State::Ready && sink.count(W::DriverAvailable)==1);
        core.runtime=false;s.poll(t+=100);assert(s.state()==Session::State::Restarting && sink.count(W::DriverAvailable)==2 && core.restarts==1 && sink.count(W::ResetInterface)==2);
        core.runtime=true;core.stateBits=4;s.poll(t+=2000);assert(core.restarts==2 && s.state()==Session::State::Restarting);
        core.stateBits=2;core.runtime=true;s.poll(t+=2000);assert(core.restarts==2); // quarantine bit with the runtime up: nothing to do
        FakeCore q;FakeSink kq;Session sq(q,kq);q.bss={open};sq.poll(t);assert(sq.state()==Session::State::Ready);
        q.runtime=false;q.stateBits=2;sq.poll(t+=100);assert(q.restarts==0 && !strcmp(sq.lastError,"dma-quarantine") && sq.state()==Session::State::Offline);
        FakeCore c2;FakeSink k2;Session p(c2,k2);c2.bss={open};p.poll(t);
        assert(p.startJoin(join(open,false),t)==B::kSuccess);
        for(int i=0;i<12 && !p.linkUp();++i){p.poll(t+=100);c2.advance();}
        p.poll(t+=100);assert(p.linkUp());
        assert(p.setPower(false,t)==B::kSuccess && p.state()==Session::State::Leaving && !p.powered());
        assert(p.startScan(scan,t)==B::kNotReady);c2.advance();p.poll(t+=100);
        assert(p.setPower(true,t)==B::kSuccess && p.startScan(scan,t)==B::kSuccess);
    }
    puts("Native bridge tests passed (WCL request mapping, scan-result conversion, in-kernel WPA2 join/key install, link events, leave, scan-first join, rejection, timeout, abort, offline/restart, power).");
}
