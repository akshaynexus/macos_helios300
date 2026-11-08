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
    bool reachable=true,runtime=true,scanDone=true,scanning=false,link=false,txBusy=false,disconnecting=false,scanOptions=true;
    uint32_t stage=0,phase=0,generation=1,stateBits=0,psBits=0,beaconMisses=0,width=80;int32_t rssi=-55;
    std::vector<B::Network> bss;std::vector<std::vector<uint8_t>> eapolIn;std::vector<std::vector<uint8_t>> eapolOut;
    std::vector<uint8_t> installedKeys;bool authorized=false,secure=false,rejectConnect=false,failAfterConnect=false,connected=false;
    unsigned scans=0,scanCancels=0,cancels=0,disconnects=0,restarts=0,psRequests=0,macChanges=0,connects=0,scanBusy=0;uint32_t lastPS=9;int connectIndex=-1;std::string error="none";uint8_t stationMAC[6]={2,1,2,3,4,5};
    uint32_t lastScanLength=0;uint64_t lastScanMask=0;uint8_t lastScanFlags=0;std::string lastScanSSID;
    std::vector<std::vector<uint8_t>> missing; // BSSIDs the core's own cache lacks until its next scan
    static void le32(uint8_t *p,uint32_t v){for(int i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
    int call(const AthControl::Request &r,AthControl::Response &o) override {
        using namespace AthControl;
        if(!reachable)return B::kNotReady;
        o=Response{};o.stage=stage;o.phase=phase;o.count=uint32_t(bss.size());memcpy(o.local,stationMAC,6);
        o.flags=128|512|(runtime?1:0)|(scanDone?2:0)|(link?4:0)|(txBusy?8:0)|(secure?16:0)|(disconnecting?64:0)|(scanning?256:0);
        strncpy(o.error,error.c_str(),95);
        if(r.operation==Status)return 0;
        if(r.operation==Info){le32(o.data,0x2ff|256|(scanOptions?2048:0));le32(o.data+4,generation);le32(o.data+8,44);le32(o.data+12,uint32_t(rssi));le32(o.data+16,width);le32(o.data+20,2);
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
        if(r.operation==Scan){
            if(r.index==1){if(scanning)++scanCancels;scanning=false;scanDone=true;return 0;}
            if(scanBusy){--scanBusy;return B::kBusy;}
            if(stage && !(stage==10 && link))return B::kBusy;
            if(r.length && (!scanOptions || r.length!=42))return B::kBadArgument;
            lastScanLength=r.length;lastScanMask=0;lastScanFlags=0;lastScanSSID.clear();
            if(r.length){for(int i=0;i<8;++i)lastScanMask|=uint64_t(r.data[i])<<(8*i);lastScanFlags=r.data[8];lastScanSSID.assign((const char *)r.data+10,r.data[9]);}
            ++scans;scanDone=false;scanning=true;++generation;missing.clear();return 0;}
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
            for(auto &m:missing)if(!memcmp(m.data(),r.data+2,6))found=false;
            if(!found || rejectConnect){error="configured-bss-not-found";strncpy(o.error,error.c_str(),95);return B::kUnsupported;}
            ++connects;connectIndex=int(r.index);secure=r.data[1];stage=1;error="none";return 0;}
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
        const uint8_t *r=out+sizeof(*m)+71;
#if AIRPORT_ATH10K_WPASEC
        assert(r[0]==48 && r[1]==24 && r[14]==2 && r[19]==2 && r[23]==8); // gate on: SAE is usable, WCL sees both AKMs
#else
        assert(r[0]==48 && r[1]==20 && r[14]==1 && r[19]==2 && r[20]==0x80);
#endif
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
#if !AIRPORT_ATH10K_WPASEC
        B::Network sae=wpa;sae.wpa2=false;assert(!strcmp(B::planJoin(join(sae,true),&sae,plan),"wpa2-psk-ccmp-pairwise-required"));
        j=join(wpa,true);j.upperAuth=1u<<2;j.keyLength=0;assert(!strcmp(B::planJoin(j,&wpa,plan),"enterprise-not-supported"));
        j=join(wpa,true);j.upperAuth=1u<<12;assert(!strcmp(B::planJoin(j,&wpa,plan),"auth-not-supported"));
#else
        j=join(wpa,true);j.upperAuth=1u<<2;j.keyLength=0;assert(!strcmp(B::planJoin(j,&wpa,plan),"enterprise-akm-not-offered"));
        j=join(wpa,true);j.upperAuth=1u<<12;assert(!strcmp(B::planJoin(j,&wpa,plan),"sae-not-offered"));
#endif
        j=join(wpa,true);j.ssid[0]='x';assert(!strcmp(B::planJoin(j,&wpa,plan),"ssid-mismatch"));
        assert(!B::planJoin(join(wpa,true),nullptr,plan)); // unknown BSS: decided after a scan
        // Unsupported security names its reason; refusalStatus maps it to the 802.11 status WCL gets.
        {B::Network wpa1=open;wpa1.privacy=true;const uint8_t ie[]={221,22,0,0x50,0xf2,1,1,0,0,0x50,0xf2,2,1,0,0,0x50,0xf2,2,1,0,0,0x50,0xf2,2};
         memcpy(wpa1.wpa,ie,sizeof(ie));wpa1.wpaLength=sizeof(ie);
         assert(!strcmp(B::planJoin(join(wpa1,true),&wpa1,plan),"wpa1-only-not-supported") && B::refusalStatus("wpa1-only-not-supported")==42);
         uint8_t out[1024];const size_t n=B::buildScanResult(wpa1,out,sizeof(out));const uint8_t *ie0=out+sizeof(W::BeaconMetadata);
         bool found=false;for(size_t at=0;at+2<=n-sizeof(W::BeaconMetadata);at+=2+ie0[at+1])if(ie0[at]==221 && !memcmp(ie0+at,ie,sizeof(ie)))found=true;
         assert(found);} // WCL sees "WPA Personal", not WEP
        {B::Network sae2=wpa;const uint8_t r[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,8,0xc0,0};
         sae2.rsnLength=sizeof(r);memcpy(sae2.rsn,r,sizeof(r));sae2.wpa2=false;
         assert(!strcmp(B::planJoin(join(sae2,true),&sae2,plan),"wpa3-sae-only-not-supported") && B::refusalStatus("wpa3-sae-only-not-supported")==43);
         const uint8_t tk[]={48,20,1,0,0,15,172,2,1,0,0,15,172,2,1,0,0,15,172,2,0,0};
         sae2.rsnLength=sizeof(tk);memcpy(sae2.rsn,tk,sizeof(tk));
         assert(!strcmp(B::planJoin(join(sae2,true),&sae2,plan),"tkip-pairwise-not-supported") && B::refusalStatus("tkip-pairwise-not-supported")==42);
         const uint8_t pmf[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0xc0,0};
         sae2.rsnLength=sizeof(pmf);memcpy(sae2.rsn,pmf,sizeof(pmf));
#if AIRPORT_ATH10K_WPASEC
         assert(!B::planJoin(join(sae2,true),&sae2,plan) && plan.mfpc && plan.mfpr && plan.extended && plan.akm==2); // gate on: PMF-required WPA2 joins
         assert(!strcmp(B::planJoin(join(sae2,true),&sae2,plan,0),"pmf-required-not-supported") && B::refusalStatus("pmf-required-not-supported")==31);}
#else
         assert(!strcmp(B::planJoin(join(sae2,true),&sae2,plan),"pmf-required-not-supported") && B::refusalStatus("pmf-required-not-supported")==31);}
#endif
        assert(B::refusalStatus("wep-not-supported")==13 && B::refusalStatus("enterprise-not-supported")==43);
    }
    { // Full WPA2 join: ready, scan (cache first), join, in-kernel handshake, keys, link, LQM; then AP deauth.
        FakeCore core;FakeSink sink;Session s(core,sink);core.bss={wpa,open};
        uint64_t t=1000;s.poll(t);
        assert(s.state()==Session::State::Ready && sink.count(W::DriverAvailable)==1 && sink.count(W::ResetInterface)==1 && s.networkCount()==2);
        W::ScanRequest scan{};scan.type=1;
        assert(s.startScan(scan,t)==B::kSuccess && s.scanning() && core.scans==0);
        assert(s.startScan(scan,t)==B::kSuccess); // merged into the owed scan, never busy
        s.poll(t+=20); // the cached BSSes at once, then the core scan (all channels: the plain request)
        assert(sink.count(W::ScanResult)==2 && core.scans==1 && core.lastScanLength==0 && sink.count(W::ScanDone)==0);
        core.advance();s.poll(t+=20);
        assert(!s.scanning() && sink.count(W::ScanResult)==4 && sink.count(W::ScanDone)==1 && *reinterpret_cast<const int *>(sink.last(W::ScanDone)->data.data())==0);
        assert(s.lastScanMs==20 && s.scanCount==1);
        assert(s.startJoin(join(wpa,true),t)==B::kSuccess && s.joining() && s.busy());
        assert(s.startJoin(join(wpa,true),t)==B::kBusy);
        assert(!s.associated());
        for(int i=0;i<12 && core.stage!=10;++i){s.poll(t+=20);core.advance();}
        assert(core.stage==10 && core.connectIndex==0 && core.secure && core.connects==1);
        s.poll(t+=20); // associated: AssocComplete, LinkStatus, FirstBeacon; M1 -> M2
        assert(sink.count(W::AssocComplete)==1 && sink.count(W::LinkStatus)==1 && sink.count(W::FirstBeacon)==1);
        // WCL's setCurrentBSS reads EXTENDED_BSS_INFO/BSS_INFO now (hardware run 6 failed here):
        // the BSS is answered while the handshake runs, the BSD link waits for the port.
        uint8_t info[2048];size_t n;
        assert(s.associated() && !s.linkUp() && s.bssInfo(info,sizeof(info),n) && n>sizeof(W::BeaconMetadata) && sink.links.empty());
        assert(reinterpret_cast<const W::BeaconMetadata *>(info)->rssi==-55);
        assert(core.eapolOut.size()==1 && core.eapolOut[0].size()==sizeof(v_m2) && !memcmp(core.eapolOut[0].data(),v_m2,sizeof(v_m2)));
        core.advance();s.poll(t+=20); // M3 -> keys installed, M4 queued until phase 3
        assert(core.installedKeys.size()==(AIRPORT_ATH10K_WPASEC?81u:56u) && !memcmp(core.installedKeys.data(),v_ptk+32,16) && !memcmp(core.installedKeys.data()+16,v_gtk,16) && core.installedKeys[48]==4);
        assert(core.eapolOut.size()==1);
        core.advance();s.poll(t+=20); // phase 3: M4 sent and Authorize requested
        assert(core.eapolOut.size()==2 && core.eapolOut[1].size()==sizeof(v_m4) && !memcmp(core.eapolOut[1].data(),v_m4,sizeof(v_m4)) && core.authorized);
        core.advance();s.poll(t+=20); // port authorized -> BSD link, ConnectComplete(0)
        assert(s.state()==Session::State::Connected && s.linkUp() && sink.count(W::ConnectComplete)==1 && sink.links.size()==1 && sink.links[0]);
        auto *c=reinterpret_cast<const W::ConnectionEvent *>(sink.last(W::ConnectComplete)->data.data());assert(c->status==0 && !memcmp(c->peers[0].bssid,v_ap,6));
        assert(s.lastJoinMs>0 && s.lastJoinMs<=20*16 && !s.busy());
        assert(core.psRequests==1 && core.lastPS==1 && s.lqmQuality()==100);
        size_t lqm=sink.count(W::LqmUpdate);s.poll(t+=2500);assert(sink.count(W::LqmUpdate)==lqm+1);
        assert(s.bssInfo(info,sizeof(info),n) && n>sizeof(W::BeaconMetadata));
        assert(s.beaconInfo(info,sizeof(info),t) && s.beaconInfo(info,sizeof(info),t+100));
        assert(!strcmp(s.core_().country,"US") && s.core_().channel==44 && s.core_().width==80 && s.core_().nss==2);
        // A scan while connected runs in the core (associated, port authorized).
        assert(s.startScan(scan,t)==B::kSuccess);s.poll(t+=20);assert(core.scans==2);core.advance();s.poll(t+=20);assert(sink.count(W::ScanDone)==2);
        // AP drops the link: core reports stage 0 without a disconnect in progress.
        core.link=false;core.stage=0;core.secure=false;core.beaconMisses=5;s.poll(t+=100);
        assert(s.state()==Session::State::Ready && sink.count(W::Deauth)==1 && sink.links.size()==2 && !sink.links[1]);
        auto *d=reinterpret_cast<const W::DeauthEvent *>(sink.last(W::Deauth)->data.data());assert(d->reason==4 && !memcmp(d->bssid,v_ap,6));
        assert(s.lastDisconnectReason==4 && !strcmp(Session::stateName(s.state()),"ready")); // registry mirror (AirportAth10kLastDisconnectReason / JoinState)
        // WCL rejoins (reconnect after the AP drop) without a new scan.
        assert(s.startJoin(join(wpa,true),t)==B::kSuccess);core.eapolOut.clear();core.authorized=false;core.phase=0;
        for(int i=0;i<30 && !s.linkUp();++i){s.poll(t+=20);core.advance();}
        assert(s.linkUp() && sink.count(W::ConnectComplete)==2 && core.connects==2);
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
        B::Network open2=open;core.bss={wpa,open2};
        W::ScanRequest scan{};scan.type=1;s.startScan(scan,t);s.poll(t+=100);core.advance();s.poll(t+=100);
        assert(s.startJoin(join(open2,false),t)==B::kSuccess);
        for(int i=0;i<12 && core.stage!=10;++i){s.poll(t+=100);core.advance();}
        assert(core.macChanges==2 && !memcmp(core.stationMAC,permanent,6) && core.connectIndex==2);
        core.link=false;core.stage=0;s.poll(t+=100);
        // Same address as the vdev: a plain Connect.
        assert(s.startJoin(join(open2,false),t)==B::kSuccess);
        for(int i=0;i<12 && core.stage!=10;++i){s.poll(t+=100);core.advance();}
        assert(core.macChanges==2 && core.connectIndex==0);
    }
    { // Open join, voluntary leave, join of a BSS missing from the cache (directed scan first), rejection, timeout, abort.
        FakeCore core;FakeSink sink;Session s(core,sink);core.bss={open};
        uint64_t t=5000;s.poll(t);
        assert(s.startJoin(join(open,false),t)==B::kSuccess);
        for(int i=0;i<12 && !s.linkUp();++i){s.poll(t+=100);core.advance();}
        assert(s.linkUp() && core.connectIndex==0 && !core.secure && sink.count(W::ConnectComplete)==1 && sink.links.size()==1 && sink.links[0]);
        assert(s.leaveNetwork(t)==B::kSuccess && s.state()==Session::State::Leaving && core.disconnects==1 && sink.links.back()==false);
        assert(s.leaveNetwork(t)==B::kSuccess && core.disconnects==1); // WCL repeats setDISASSOCIATE: harmless
        core.advance();s.poll(t+=100);assert(s.state()==Session::State::Ready);
        // WCL asks for a BSSID that is not in the last scan: a directed scan of its channel runs first.
        core.bss={open,wpa};
        assert(s.startJoin(join(wpa,true),t)==B::kSuccess && s.joining());
        s.poll(t+=20);s.poll(t+=20);
        assert(core.scans==1 && s.coreScanning() && core.lastScanLength==42 && core.lastScanMask==B::channelBit(44) && core.lastScanSSID=="home");
        core.advance();s.poll(t+=20);
        assert(!s.scanning() && sink.count(W::ScanDone)==0 && core.stage>=1);
        for(int i=0;i<30 && !s.linkUp();++i){s.poll(t+=100);core.advance();}
        assert(s.linkUp() && sink.count(W::ConnectComplete)==2);
        // Association rejected by the AP.
        core.link=false;core.stage=0;core.secure=false;s.poll(t+=100);assert(s.state()==Session::State::Ready);
        core.failAfterConnect=true;assert(s.startJoin(join(open,false),t)==B::kSuccess);
        for(int i=0;i<6 && s.joining();++i){s.poll(t+=100);core.advance();}
        s.poll(t+=100);
        assert(!s.joining() && s.state()==Session::State::Ready && core.cancels>=1 && core.stage==0);
        auto *c=reinterpret_cast<const W::ConnectionEvent *>(sink.last(W::ConnectComplete)->data.data());assert(c->status==1);
        // Timeout: the core never associates.
        core.failAfterConnect=false;assert(s.startJoin(join(open,false),t)==B::kSuccess);s.poll(t+=100);
        s.poll(t+=31000);assert(!s.joining());c=reinterpret_cast<const W::ConnectionEvent *>(sink.last(W::ConnectComplete)->data.data());assert(c->status==1 && c->reason==1004);
        core.stage=0;
        // Abort (accepted while the timed-out connection is still being torn down).
        assert(s.startJoin(join(open,false),t)==B::kSuccess);assert(s.abortJoin(t)==B::kSuccess && sink.count(W::JoinAbortComplete)==1 && !s.joining());
        // An auth type the card cannot do: accepted, then refused with an 802.11 status (no driver error).
#if AIRPORT_ATH10K_WPASEC
        W::JoinRequest j=join(wpa,true);j.upperAuth=1u<<12;assert(s.startJoin(j,t)==B::kSuccess && !strcmp(s.lastError,"sae-not-offered"));
#else
        W::JoinRequest j=join(wpa,true);j.upperAuth=1u<<12;assert(s.startJoin(j,t)==B::kSuccess && !strcmp(s.lastError,"auth-not-supported"));
#endif
        s.poll(t+=20);c=reinterpret_cast<const W::ConnectionEvent *>(sink.last(W::ConnectComplete)->data.data());
        assert(!s.joining() && c->status==1 && c->reason==43 && c->peers[0].driverStatus==B::kUnsupported);
        assert(reinterpret_cast<const W::AssociationEvent *>(sink.last(W::AssocComplete)->data.data())->status==43);
    }
    { // Scans: channel subsets and directed SSIDs reach the core, overlapping requests merge,
      // passive requests stay passive, a busy core is retried, abort, the boot scan is adopted,
      // and a request while a leave owns the radio is answered from the cache.
        FakeCore core;FakeSink sink;Session s(core,sink);core.bss={wpa,open};uint64_t t=20000;s.poll(t);
        W::ScanRequest one{};one.type=1;one.channelCount=1;one.channels[0]|=1u<<6;memcpy(one.ssid,"cafe",4);one.ssidLength=4;
        assert(s.startScan(one,t)==B::kSuccess);s.poll(t+=20);
        assert(sink.count(W::ScanResult)==1 && core.scans==1 && core.lastScanLength==42 && core.lastScanMask==B::channelBit(6) && core.lastScanSSID=="cafe" && !core.lastScanFlags);
        W::ScanRequest all{};all.type=1;assert(s.startScan(all,t)==B::kSuccess); // widened while the first runs
        core.advance();s.poll(t+=20);
        assert(sink.count(W::ScanDone)==0 && core.scans==2 && core.lastScanMask==(B::allChannelMask()&~B::channelBit(6)) && core.lastScanSSID.empty());
        core.advance();s.poll(t+=20);assert(sink.count(W::ScanDone)==1 && !s.scanning());
        W::ScanRequest passive{};passive.type=2;assert(s.startScan(passive,t)==B::kSuccess);s.poll(t+=20);
        assert(core.scans==3 && core.lastScanLength==42 && core.lastScanFlags==1 && !core.lastScanMask);
        core.advance();s.poll(t+=20);assert(sink.count(W::ScanDone)==2);
        core.scanBusy=3;assert(s.startScan(all,t)==B::kSuccess);
        for(int i=0;i<3;++i){s.poll(t+=20);assert(core.scans==3 && s.scanning());}
        s.poll(t+=20);assert(core.scans==4);core.advance();s.poll(t+=20);assert(sink.count(W::ScanDone)==3);
        assert(s.startScan(all,t)==B::kSuccess);s.poll(t+=20);assert(core.scans==5);
        s.abortScan();assert(sink.count(W::ScanDone)==4 && *reinterpret_cast<const int *>(sink.last(W::ScanDone)->data.data())==B::kAborted && core.scanCancels==1);
        s.poll(t+=20);assert(!s.scanning());
        // A core without scan options gets the plain request.
        core.scanOptions=false;s.poll(t+=20);assert(s.startScan(one,t)==B::kSuccess);s.poll(t+=20);assert(core.scans==6 && core.lastScanLength==0);
        core.advance();s.poll(t+=20);assert(sink.count(W::ScanDone)==5);core.scanOptions=true;
        // The core's own scan (boot) is adopted instead of being refused or repeated.
        FakeCore b;FakeSink bs;Session sb(b,bs);b.bss={open};b.scanning=true;b.scanDone=false;sb.poll(t);
        assert(sb.startScan(all,t)==B::kSuccess);sb.poll(t+=20);assert(b.scans==0 && sb.scanning());
        b.advance();sb.poll(t+=20);assert(bs.count(W::ScanDone)==1 && bs.count(W::ScanResult)==1 && b.scans==0);
        // Leaving owns the radio: cached results now, ScanDone after the grace period, no core scan.
        assert(s.startJoin(join(open,false),t)==B::kSuccess);
        for(int i=0;i<12 && !s.linkUp();++i){s.poll(t+=20);core.advance();}
        assert(s.linkUp() && s.leaveNetwork(t)==B::kSuccess && s.state()==Session::State::Leaving);
        const unsigned scans=core.scans;const size_t results=sink.count(W::ScanResult),done=sink.count(W::ScanDone);
        assert(s.startScan(all,t)==B::kSuccess);s.poll(t+=20);assert(sink.count(W::ScanResult)==results+2 && sink.count(W::ScanDone)==done);
        s.poll(t+=1600);assert(sink.count(W::ScanDone)==done+1 && core.scans==scans && !s.scanning());
    }
    { // Join robustness: a running WCL scan is cancelled for the join (and answered), a BSS the
      // core's own cache lacks is rescanned, and a join is accepted while a leave completes.
        FakeCore core;FakeSink sink;Session s(core,sink);core.bss={wpa,open};uint64_t t=40000;s.poll(t);
        W::ScanRequest all{};all.type=1;assert(s.startScan(all,t)==B::kSuccess);s.poll(t+=20);assert(core.scans==1 && core.scanning);
        assert(s.startJoin(join(open,false),t)==B::kSuccess);s.poll(t+=20);assert(core.scanCancels==1);
        s.poll(t+=20);assert(sink.count(W::ScanDone)==1 && core.connects==1);
        for(int i=0;i<12 && !s.linkUp();++i){s.poll(t+=20);core.advance();}
        assert(s.linkUp());
        // Join another BSS straight away: the session disconnects, waits, then connects.
        core.missing={{v_ap,v_ap+6}};
        assert(s.leaveNetwork(t)==B::kSuccess && s.startJoin(join(wpa,true),t)==B::kSuccess);
        core.eapolOut.clear();core.authorized=false;core.phase=0;
        for(int i=0;i<40 && !s.linkUp();++i){s.poll(t+=20);core.advance();}
        assert(s.linkUp() && core.scans==2 && core.lastScanMask==B::channelBit(44) && core.connects==2 && sink.count(W::ConnectComplete)==2);
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
        // A scan owed at power-off is completed (offline), the link goes down.
        scan.type=1;assert(p.startScan(scan,t)==B::kSuccess);
        assert(p.setPower(false,t)==B::kSuccess && p.state()==Session::State::Leaving && !p.powered() && !p.scanning());
        assert(*reinterpret_cast<const int *>(k2.last(W::ScanDone)->data.data())==B::kOffline && k2.links.back()==false);
        assert(p.startScan(scan,t)==B::kNotReady);c2.advance();p.poll(t+=100);
        assert(p.setPower(true,t)==B::kSuccess && p.startScan(scan,t)==B::kSuccess);
    }
    { // Hidden SSID, switching networks without a leave, refusing a network while connected.
        FakeCore core;FakeSink sink;Session s(core,sink);B::Network hidden=open;hidden.hidden=true;hidden.ssidLength=0;memset(hidden.ssid,0,32);
        hidden.bssid[5]=0x44;core.bss={hidden,wpa};uint64_t t=70000;s.poll(t);
        W::JoinRequest hj=join(open,false);memcpy(hj.bssid,hidden.bssid,6); // WCL names the SSID of the hidden BSSID
        assert(s.startJoin(hj,t)==B::kSuccess);
        for(int i=0;i<12 && !s.linkUp();++i){s.poll(t+=20);core.advance();}
        assert(s.linkUp() && core.connectIndex==1); // Connect index 1: the SSID names the hidden BSSID
        // Switch to the WPA2 network straight away: disconnect, connect, handshake.
        assert(s.startJoin(join(wpa,true),t)==B::kSuccess);core.eapolOut.clear();core.authorized=false;core.phase=0;
        for(int i=0;i<40 && !(s.linkUp() && core.secure);++i){s.poll(t+=20);core.advance();}
        assert(s.linkUp() && core.secure && core.disconnects==1 && sink.count(W::ConnectComplete)==2);
        // A WPA3-only network while connected: the old link goes down, the join is refused with status 43.
        B::Network sae=wpa;sae.bssid[5]=0x55;const uint8_t r[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,8,0xc0,0};
        sae.rsnLength=sizeof(r);memcpy(sae.rsn,r,sizeof(r));sae.wpa2=false;core.bss.push_back(sae);
        W::ScanRequest scan{};scan.type=1;s.startScan(scan,t);s.poll(t+=20);core.advance();s.poll(t+=20);
        assert(s.startJoin(join(sae,true),t)==B::kSuccess && core.disconnects==2 && sink.links.back()==false);
        s.poll(t+=20);
        auto *c=reinterpret_cast<const W::ConnectionEvent *>(sink.last(W::ConnectComplete)->data.data());
        assert(c->status==1 && c->reason==43 && !strcmp(s.lastError,"wpa3-sae-only-not-supported") && s.state()==Session::State::Leaving);
        core.advance();s.poll(t+=20);assert(s.state()==Session::State::Ready);
    }
    { // Roaming to another BSSID of the same SSID (mesh): a new ASSOCIATE while connected,
      // served as disconnect + connect with the same PMK; counted and timed for the registry.
        FakeCore core;FakeSink sink;Session s(core,sink);B::Network second=wpa;second.bssid[5]^=0x40;second.channel=36;second.htOp[0]=36;
        core.bss={wpa,second};uint64_t t=80000;s.poll(t);
        assert(s.startJoin(join(wpa,true),t)==B::kSuccess);
        for(int i=0;i<30 && !s.linkUp();++i){s.poll(t+=20);core.advance();}
        assert(s.linkUp() && !s.roams);
        // The handshake vectors are for v_ap; the roam target only has to reach stage 10 and the M1.
        assert(s.startJoin(join(second,true),t)==B::kSuccess && s.roams==1);
        for(int i=0;i<40 && !(core.connects==2 && core.stage==10);++i){s.poll(t+=20);core.advance();}
        assert(core.disconnects==1 && core.connects==2 && core.stage==10);
        s.poll(t+=20);assert(sink.count(W::AssocComplete)==2);
        auto *a=reinterpret_cast<const W::AssociationEvent *>(sink.last(W::AssocComplete)->data.data());
        assert(!memcmp(a->bssid,second.bssid,6));
    }
    { // System sleep/wake with the core's in-place firmware reload: no Restart while state bit 8
      // is set, driver-available again when the runtime returns, the registry mirror's result.
        FakeCore core;FakeSink sink;Session s(core,sink);core.bss={open};uint64_t t=60000;s.poll(t);
        assert(s.startJoin(join(open,false),t)==B::kSuccess);
        for(int i=0;i<12 && !s.linkUp();++i){s.poll(t+=20);core.advance();}
        assert(s.linkUp());
        s.pmSleep(t);assert(s.setPower(false,t)==B::kSuccess && sink.links.back()==false && !strcmp(s.wakeResult,"sleeping"));
        core.runtime=false;core.stateBits=1;core.link=false;core.stage=0;s.poll(t+=100); // suspended
        assert(s.state()==Session::State::Offline && core.restarts==0 && reinterpret_cast<const W::DriverAvailability *>(sink.last(W::DriverAvailable)->data.data())->available==0);
        s.pmWake(t+=60000);s.setPower(true,t);core.stateBits=8;                         // firmware reload running
        for(int i=0;i<30;++i)s.poll(t+=100);
        assert(core.restarts==0 && s.state()==Session::State::Offline && !strcmp(s.wakeResult,"pending"));
        const size_t available=sink.count(W::DriverAvailable),resets=sink.count(W::ResetInterface);
        core.runtime=true;core.stateBits=0;s.poll(t+=100);                               // reload done
        assert(s.state()==Session::State::Ready && sink.count(W::DriverAvailable)==available+1 && sink.count(W::ResetInterface)==resets+1);
        assert(!strcmp(s.wakeResult,"reinit") && s.wakeMs==3100 && s.lastWakeMs>s.lastSleepMs);
        // WCL rescans and auto-joins.
        W::ScanRequest scan{};scan.type=1;assert(s.startScan(scan,t)==B::kSuccess);s.poll(t+=20);core.advance();s.poll(t+=20);
        assert(s.startJoin(join(open,false),t)==B::kSuccess);
        for(int i=0;i<12 && !s.linkUp();++i){s.poll(t+=20);core.advance();}
        assert(s.linkUp());
        // A failed reload falls back to the guarded restart.
        s.pmSleep(t);s.setPower(false,t);core.runtime=false;core.stateBits=1;core.link=false;core.stage=0;s.poll(t+=100);
        s.pmWake(t+=1000);s.setPower(true,t);core.stateBits=4;s.poll(t+=100);
        assert(core.restarts==1 && !strcmp(s.wakeResult,"restart-requested"));
    }
    { // Chanspecs with the BSS's width (run 7: WCL saw "bandWidth 2" = 20 MHz for an HT40 BSS).
        const uint8_t *b5=v_ap;const uint8_t b24[]={2,6,7,8,9,21}; // the WPA2 vectors are bound to v_ap
        B::Network vht=network(b5,"home",44,true);vht.htOp[1]=1|4;vht.vhtPresent=vht.vhtOpPresent=true;vht.vhtOp[0]=1;vht.vhtOp[1]=42; // VHT80 36-48, primary 44 (HT40+ with 48)
        B::Network ht40=network(b24,"home",2,true);ht40.htOp[1]=1|4;                                                                   // HT40+, primary 2
        auto w=B::bssWidth(vht);assert(w.mhz==80 && w.center==42 && w.primary==44);
        assert(B::widthSpec(w)==(0xc000|0x2000|0x0200|42) && B::controlChannel(B::widthSpec(w))==44);
        w=B::bssWidth(ht40);assert(w.mhz==40 && w.center==4 && B::widthSpec(w)==(0x1800|4) && B::controlChannel(0x1804)==2);
        assert(B::bssWidth(vht,40).mhz==40 && B::widthSpec(B::bssWidth(vht,40))==(0xc000|0x1800|46) && B::controlChannel(0xd82e)==44 && B::controlChannel(0xd92e)==48);
        assert(B::bssWidth(vht,20).mhz==20 && B::widthSpec(B::bssWidth(vht,20))==0xd02c);
        for(unsigned p:{36u,40u,44u,48u}){B::Network n=vht;n.channel=uint8_t(p);n.htOp[0]=uint8_t(p);n.htOp[1]=uint8_t((p==36||p==44?1:3)|4);
            auto x=B::bssWidth(n);assert(x.mhz==80 && x.center==42 && B::controlChannel(B::widthSpec(x))==p);}
        B::Network bad=vht;bad.vhtOp[1]=50;assert(B::bssWidth(bad).mhz==40);         // inconsistent CCFS0: keep 40
        bad=ht40;bad.htOp[1]=3|4;assert(B::bssWidth(bad).mhz==20);                    // HT40- on channel 2 does not exist
        bad=ht40;bad.htOp[0]=6;assert(B::bssWidth(bad).mhz==20);                      // HT op primary disagrees with the BSS channel
        B::Network old160=vht;old160.vhtOp[0]=2;old160.vhtOp[1]=50;assert(B::bssWidth(old160).center==42); // deprecated 160: primary 80 of it
        uint8_t out[1024];auto *m=reinterpret_cast<W::BeaconMetadata *>(out);
        assert(B::buildScanResult(vht,out,sizeof(out)) && m->channelSpec==0xe22a && m->channel==44);
        assert(B::buildScanResult(vht,out,sizeof(out),20) && m->channelSpec==0xd02c); // airportath10k_bw=0
        // The session: scan results carry the width, the joined BSS what the core negotiated (Info width 80 here).
        FakeCore core;FakeSink sink;Session s(core,sink);core.bss={vht,ht40};uint64_t t=1000;s.poll(t);
        W::ScanRequest scan{};scan.type=1;assert(s.startScan(scan,t)==B::kSuccess);s.poll(t+=20);core.advance();s.poll(t+=20);
        bool saw5=false,saw24=false;
        for(auto &e:sink.messages)if(e.type==W::ScanResult){auto *r=reinterpret_cast<const W::BeaconMetadata *>(e.data.data());
            if(r->channel==44){saw5=true;assert(r->channelSpec==0xe22a);}if(r->channel==2){saw24=true;assert(r->channelSpec==0x1804);}}
        assert(saw5 && saw24);
        // A join candidate chanspec from WCL (80 MHz form) resolves to the primary channel.
        W::JoinRequest j=join(vht,true);j.channelSpec=0xe22a;j.candidateCount=1;memcpy(j.candidateBSSID[0],b5,6);j.candidateSpec[0]=0xe22a;
        assert(s.startJoin(j,t)==B::kSuccess);
        for(int i=0;i<14 && !s.linkUp();++i){s.poll(t+=20);core.advance();}
        assert(s.linkUp());
        uint8_t info[2048];size_t n=0;assert(s.bssInfo(info,sizeof(info),n) && reinterpret_cast<W::BeaconMetadata *>(info)->channelSpec==0xe22a);
        // A core that reports no width (0) or an odd one: 20 MHz, never a guessed 80; 40 is passed through.
        for(uint32_t cw:{0u,160u,40u,80u}){core.width=cw;s.poll(t+=20);core.advance();assert(s.linkUp() && s.core_().width==cw);
            const unsigned want=cw==40||cw==80?cw:20;assert(s.linkMHz()==want && s.bssInfo(info,sizeof(info),n));
            assert(reinterpret_cast<W::BeaconMetadata *>(info)->channelSpec==(want==80?0xe22a:want==40?0xd82e:0xd02c));}
        assert(!strncmp(s.candidateText(),"1: ch44 -55 80MHz*",18));
        // Candidates: WCL's first choice is the 2.4 GHz BSS; band preference (opt-in) takes the 5 GHz one at -68 dBm or better.
        FakeCore c2;FakeSink k2;Session s2(c2,k2);B::Network v5=vht;v5.snr=35;B::Network h24=ht40;h24.snr=69;c2.bss={h24,v5};
        t=5000;s2.poll(t);assert(s2.startScan(scan,t)==B::kSuccess);s2.poll(t+=20);c2.advance();s2.poll(t+=20);
        W::JoinRequest two=join(h24,true);two.candidateCount=2;memcpy(two.candidateBSSID[0],b24,6);two.candidateSpec[0]=0x1804;
        memcpy(two.candidateBSSID[1],b5,6);two.candidateSpec[1]=0xe22a;
        assert(s2.startJoin(two,t)==B::kSuccess && !strcmp(s2.candidateText(),"2: ch2 -26 40MHz*, ch44 -60 80MHz"));
        s2.leaveNetwork(t);for(int i=0;i<5;++i){s2.poll(t+=20);c2.advance();}
        s2.bandPreference=true;assert(s2.startJoin(two,t)==B::kSuccess && !strcmp(s2.candidateText(),"2: ch2 -26 40MHz, ch44 -60 80MHz*"));
        for(int i=0;i<14 && !s2.linkUp();++i){s2.poll(t+=20);c2.advance();}
        assert(s2.linkUp() && s2.associated() && s2.associated()->channel==44);
        s2.leaveNetwork(t);for(int i=0;i<5;++i){s2.poll(t+=20);c2.advance();}
        B::Network weak5=v5;weak5.snr=20;c2.bss={h24,weak5};s2.startScan(scan,t);s2.poll(t+=20);c2.advance();s2.poll(t+=20);
        assert(s2.startJoin(two,t)==B::kSuccess && !strcmp(s2.candidateText(),"2: ch2 -26 40MHz*, ch44 -75 80MHz")); // below -68: WCL's choice stays
    }
    { // decodeJoin: every candidate (count at 0x214, 18-byte entries from 0x218, BSSID +4, chanspec +0x10).
        std::vector<uint8_t> raw(0x218+3*18);W::JoinRequest j{};
        raw[0x14]=8;raw[0x1c]=4;memcpy(raw.data()+0x20,"home",4);raw[0x214]=3;
        const uint8_t a[]={2,1,1,1,1,1},b[]={2,2,2,2,2,2},mc[]={3,3,3,3,3,3};
        memcpy(raw.data()+0x218+4,a,6);raw[0x218+0x10]=0x04;raw[0x218+0x11]=0x18;
        memcpy(raw.data()+0x218+18+4,b,6);raw[0x218+18+0x10]=0x2a;raw[0x218+18+0x11]=0xe2;
        memcpy(raw.data()+0x218+36+4,mc,6); // group address: skipped
        assert(W::decodeJoin(raw.data(),raw.size(),j) && j.candidateCount==2 && !memcmp(j.bssid,a,6) && j.channelSpec==0x1804);
        assert(!memcmp(j.candidateBSSID[1],b,6) && j.candidateSpec[1]==0xe22a && B::controlChannel(j.candidateSpec[1])==44);
    }
    { // Full scan cache: a strong 5 GHz BSS heard after 64 2.4 GHz ones is kept; weaker ones are dropped.
        FakeCore core;FakeSink sink;Session s(core,sink);uint64_t t=1000;
        for(unsigned i=0;i<64;++i){uint8_t b[6]={2,9,9,9,uint8_t(i>>8),uint8_t(i)};B::Network n=network(b,"neighbour",uint8_t(1+i%11),true);n.snr=10+i%20;core.bss.push_back(n);}
        s.poll(t);W::ScanRequest scan{};scan.type=1;s.startScan(scan,t);s.poll(t+=20);core.advance();s.poll(t+=20);
        assert(s.networkCount()==64);
        const uint8_t b5[]={2,6,7,8,9,30};B::Network strong=network(b5,"home",44,true);strong.snr=50;
        const uint8_t bw[]={2,6,7,8,9,31};B::Network weak=network(bw,"faint",1,false);weak.snr=5;
        core.bss={strong,weak};s.startScan(scan,t);s.poll(t+=20);core.advance();s.poll(t+=20);
        bool kept=false,keptWeak=false;for(uint32_t i=0;i<s.networkCount();++i){if(!memcmp(s.network(i).bssid,b5,6))kept=true;if(!memcmp(s.network(i).bssid,bw,6))keptWeak=true;}
        assert(kept && !keptWeak && s.networkCount()==64 && s.cacheEvicted==1 && s.cacheDropped==1);
        // Entries older than the fresh window go first.
        s.startScan(scan,t+=40000);s.poll(t);core.advance();s.poll(t+=20);
        assert(s.cacheDropped==1);
    }
    puts("Native bridge tests passed (WCL request mapping, scan-result conversion, cache-first/merged/directed/passive scans, in-kernel WPA2 join with BSS info during the handshake, key install, link events, leave, rescans, rejection, timeout, abort, offline/restart, power).");
}
