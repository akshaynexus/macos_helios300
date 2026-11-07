// SPDX-License-Identifier: GPL-3.0-or-later
// AirportAth10kBridge.hpp — the native front end's logic, kept free of IOKit
// so it runs in host tests: it maps WCL scan/join requests onto the ath10k
// core's control ABI (src/shared/Ath10kControl.hpp, the same calls the
// menu-bar app made), runs the WPA2 supplicant in the kernel, converts the
// core's BSS cache into WCL scan-result messages and drives the join state
// machine from periodic polls of the core. The kext (AirportAth10kInterface)
// supplies Core (controlRequest) and Sink (postMessage/link state) and a timer.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "AirportWCL.hpp"
#include "../../src/shared/Ath10kControl.hpp"
#include "../../src/shared/Ath10kSupplicant.hpp"

namespace AirportBridge {
// IOReturn values (IOKit/IOReturn.h); the kext static_asserts they match.
constexpr int kSuccess=0;
constexpr int kError=int(0xe00002bc),kBadArgument=int(0xe00002c2),kExclusiveAccess=int(0xe00002c5);
constexpr int kUnsupported=int(0xe00002c7),kBusy=int(0xe00002d5),kTimeout=int(0xe00002d6);
constexpr int kOffline=int(0xe00002d7),kNotReady=int(0xe00002d8),kNotAttached=int(0xe00002d9);
constexpr int kAborted=int(0xe00002eb),kNotFound=int(0xe00002f0),kNotPermitted=int(0xe00002e2);

struct Core { virtual int call(const AthControl::Request &,AthControl::Response &)=0; };
struct Sink {
    virtual void post(uint32_t message,const void *data,size_t length)=0; // IO80211Controller::postMessage
    virtual void linkState(bool up)=0;                                       // BSD/IO80211 link
};

inline uint16_t le16(const uint8_t *p){return uint16_t(p[0]|(uint16_t(p[1])<<8));}
inline uint32_t le32(const uint8_t *p){return uint32_t(le16(p))|(uint32_t(le16(p+2))<<16);}
inline void put16(uint8_t *p,uint16_t v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
inline void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;++i)p[i]=uint8_t(v>>(8*i));}
inline bool sameMAC(const uint8_t *a,const uint8_t *b){return memcmp(a,b,6)==0;}

// Channels the core scans (src/kext/Ath10kScanPlan.hpp kWMIScanPlan; a host
// test keeps the two tables equal). DFS = receive-only unless the core's
// Ath10kStationDFS policy allows it (Info bit 256).
struct Channel { uint8_t number; uint16_t mhz; };
static constexpr Channel kChannels[]={
{1,2412},{2,2417},{3,2422},{4,2427},{5,2432},{6,2437},{7,2442},{8,2447},{9,2452},{10,2457},{11,2462},{12,2467},{13,2472},
{36,5180},{40,5200},{44,5220},{48,5240},{52,5260},{56,5280},{60,5300},{64,5320},{100,5500},{104,5520},{108,5540},{112,5560},
{116,5580},{120,5600},{124,5620},{128,5640},{132,5660},{136,5680},{140,5700},{144,5720},{149,5745},{153,5765},{157,5785},{161,5805},{165,5825}};
static constexpr unsigned kChannelCount=sizeof(kChannels)/sizeof(kChannels[0]);
inline bool channelDFS(unsigned c){return c>=52 && c<=144;}
inline bool channelKnown(unsigned c){for(const auto &k:kChannels)if(k.number==c)return true;return false;}

// One BSS as the core reports it (control op BSS, with the extensions after
// the RSN IE: SNR, flags, capabilities, interval, rates, HT/VHT elements).
struct Network {
    uint8_t bssid[6]{},ssid[32]{},ssidLength=0,channel=0;
    bool privacy=false,wpa2=false,hidden=false,htPresent=false,htOpPresent=false,vhtPresent=false,vhtOpPresent=false,powerConstraintPresent=false;
    uint16_t rsnLength=0,capabilities=0,beaconInterval=100;
    uint8_t rsn[257]{},rateCount=0,rates[32]{},ht[26]{},htOp[22]{},vht[12]{},vhtOp[5]{},powerConstraintDb=0;
    uint32_t snr=0;
    int32_t rssi() const {return int32_t(snr)-95;} // Linux ath10k convention (Ath10kControl.hpp)
};
inline bool decodeNetwork(const uint8_t *d,uint32_t length,Network &n){
    if(!d || length<44)return false;
    n=Network{};
    n.ssidLength=d[0];if(n.ssidLength>32)return false;
    n.privacy=d[1]!=0;memcpy(n.bssid,d+2,6);memcpy(n.ssid,d+8,32);n.channel=d[40];n.wpa2=d[41]!=0;
    n.rsnLength=le16(d+42);if(n.rsnLength>257 || 44u+n.rsnLength>length)return false;
    memcpy(n.rsn,d+44,n.rsnLength);
    uint32_t at=44u+n.rsnLength;
    if(length>=at+4){n.snr=le32(d+at);at+=4;}
    if(length>=at+4){n.hidden=(le32(d+at)&2u)!=0;at+=4;}
    if(length>=at+105){
        n.capabilities=le16(d+at);n.beaconInterval=le16(d+at+2);n.rateCount=d[at+4];if(n.rateCount>32)n.rateCount=32;
        memcpy(n.rates,d+at+5,32);const uint8_t present=d[at+37];
        n.htPresent=present&1;n.htOpPresent=present&2;n.vhtPresent=present&4;n.vhtOpPresent=present&8;
        memcpy(n.ht,d+at+38,26);memcpy(n.htOp,d+at+64,22);memcpy(n.vht,d+at+86,12);memcpy(n.vhtOp,d+at+98,5);
        n.powerConstraintPresent=d[at+103]!=0;n.powerConstraintDb=d[at+104];
    }
    if(!n.beaconInterval)n.beaconInterval=100;
    return true;
}
inline uint16_t channelSpec(unsigned channel){return uint16_t(channel|(channel>14?0xd000:0x1000));}
// Information elements as a beacon would carry them, in the usual order.
inline size_t buildIEs(const Network &n,uint8_t *out,size_t capacity){
    size_t at=0;
    auto put=[&](uint8_t id,const uint8_t *v,size_t len){if(at+2+len>capacity)return false;out[at++]=id;out[at++]=uint8_t(len);memcpy(out+at,v,len);at+=len;return true;};
    uint8_t zeros[32]{};
    if(!put(0,n.hidden?zeros:n.ssid,n.hidden?0:n.ssidLength))return 0;
    const size_t basic=n.rateCount<8?n.rateCount:8;
    if(basic && !put(1,n.rates,basic))return 0;
    const uint8_t ds=n.channel;
    if(!put(3,&ds,1))return 0;
    if(n.powerConstraintPresent && !put(32,&n.powerConstraintDb,1))return 0;
    if(n.rateCount>8 && !put(50,n.rates+8,n.rateCount-8))return 0;
    if(n.htPresent && !put(45,n.ht,26))return 0;
    if(n.htOpPresent && !put(61,n.htOp,22))return 0;
    if(n.rsnLength>=2){if(at+n.rsnLength>capacity)return 0;memcpy(out+at,n.rsn,n.rsnLength);at+=n.rsnLength;}
    if(n.vhtPresent && !put(191,n.vht,12))return 0;
    if(n.vhtOpPresent && !put(192,n.vhtOp,5))return 0;
    return at;
}
// WCL scan-result message: BeaconMetadata followed by the IEs (WCL.hpp layout).
inline size_t buildScanResult(const Network &n,uint8_t *out,size_t capacity){
    if(capacity<sizeof(AirportWCL::BeaconMetadata)+64)return 0;
    auto *m=reinterpret_cast<AirportWCL::BeaconMetadata *>(out);
    *m=AirportWCL::BeaconMetadata{};
    uint8_t *ies=out+sizeof(*m);
    size_t length=buildIEs(n,ies,capacity-sizeof(*m));
    if(!length)return 0;
    length=AirportWCL::selectWPA2TransitionMode(ies,length);
    m->ieLength=uint32_t(length);m->channelSpec=channelSpec(n.channel);m->channel=n.channel;
    m->ssidLength=n.hidden?0:n.ssidLength;if(!n.hidden)memcpy(m->ssid,n.ssid,n.ssidLength);
    memcpy(m->bssid,n.bssid,6);m->rssi=n.rssi();m->noise=-95;m->snr=int16_t(n.snr);
    m->interval=n.beaconInterval;m->capability=n.capabilities;
    m->flags=AirportWCL::SSIDPresent|AirportWCL::RSSIValid;
    return sizeof(*m)+length;
}
// A beacon frame for BEACON_INFO (length + 802.11 beacon). The core keeps no
// raw beacons; WCL polls this to see fresh reception, so the TSF advances.
inline size_t buildBeaconFrame(const Network &n,uint64_t tsfMicroseconds,uint16_t sequence,uint8_t *out,size_t capacity){
    if(capacity<4+36+64)return 0;
    uint8_t *f=out+4;memset(f,0,36);
    f[0]=0x80;memset(f+4,0xff,6);memcpy(f+10,n.bssid,6);memcpy(f+16,n.bssid,6);put16(f+22,uint16_t(sequence<<4));
    put64(f+24,tsfMicroseconds);put16(f+32,n.beaconInterval);put16(f+34,n.capabilities);
    size_t ies=buildIEs(n,f+36,capacity-4-36);
    if(!ies)return 0;
    const uint32_t length=uint32_t(36+ies);memcpy(out,&length,4);
    return 4+length;
}

// Join parameters after validation against what the core supports.
struct JoinPlan {
    bool secure=false,hidden=false;uint8_t bssid[6]{},ssid[32]{},ssidLength=0,pmk[32]{};
};
// WCL hands Personal networks the PMK (apple80211 cipher PMK/PMKSA, 32 bytes;
// WCL.hpp classifyLegacyAuth). WPA2-Personal and WPA2/WPA3 transition networks
// (PSK offered) are accepted; SAE-only, enterprise, WEP and PMF-required are not.
inline const char *planJoin(const AirportWCL::JoinRequest &j,const Network *n,JoinPlan &plan){
    plan=JoinPlan{};
    switch(AirportWCL::classifyLegacyAuth(j)){
    case AirportWCL::LegacyAuth::Open:plan.secure=false;break;
    case AirportWCL::LegacyAuth::Personal:plan.secure=true;break;
    case AirportWCL::LegacyAuth::Enterprise:return "enterprise-not-supported";
    case AirportWCL::LegacyAuth::Wep:return "wep-not-supported";
    default:return "auth-not-supported";
    }
    if(plan.secure)memcpy(plan.pmk,j.key,32);
    memcpy(plan.bssid,j.bssid,6);
    plan.ssidLength=uint8_t(j.ssidLength);memcpy(plan.ssid,j.ssid,j.ssidLength);
    if(!n)return nullptr; // not in the last scan: the session scans first and re-checks
    if(plan.secure){if(!n->wpa2)return "wpa2-psk-ccmp-pairwise-required";}
    else if(n->privacy)return "protected-network-not-supported";
    if(!channelKnown(n->channel))return "bss-channel-unsupported";
    plan.hidden=n->hidden;
    if(!n->hidden && (n->ssidLength!=j.ssidLength || memcmp(n->ssid,j.ssid,j.ssidLength)))return "ssid-mismatch";
    return nullptr;
}
inline void buildConnectRequest(const JoinPlan &p,AthControl::Request &r){
    r=AthControl::Request{};r.operation=AthControl::Connect;r.index=p.hidden?1:0;
    r.data[0]=p.ssidLength;r.data[1]=p.secure?1:0;memcpy(r.data+2,p.bssid,6);memcpy(r.data+8,p.ssid,p.ssidLength);
    r.length=8u+p.ssidLength;
}

// Snapshot of the core from one Status + Info poll.
struct CoreStatus {
    bool reachable=false,runtime=false,scanDone=false,link=false,txBusy=false,secure=false,eapolTxFailed=false,disconnecting=false,scanning=false;
    uint32_t stage=0,phase=0,count=0,scanGeneration=0,channel=0,width=0,nss=0,scanBits=0,stateBits=0,psBits=0,beaconMisses=0,txHalfDbm=0;
    int32_t rssi=INT32_MIN;uint8_t local[6]{},bssid[6]{};char error[96]{};char country[3]{'0','0',0};bool stationDFS=false;
};

template<class Crypto> class Session {
public:
    enum class State : uint8_t { Offline, Ready, Joining, Connected, Leaving, Restarting };
    Session(Core &core,Sink &sink):core(core),sink(sink){}
    State state() const {return state_;}
    bool scanning() const {return scanPending;}
    bool joining() const {return state_==State::Joining;}
    bool linkUp() const {return state_==State::Connected && status.link;}
    const CoreStatus &core_() const {return status;}
    const Network *associated() const {return (state_==State::Connected || state_==State::Joining)?&joined:nullptr;}
    uint32_t networkCount() const {return count;}
    const Network &network(uint32_t i) const {return networks[i<count?i:0];}
    bool powered() const {return power;}

    // --- WCL requests ----------------------------------------------------
    int startScan(const AirportWCL::ScanRequest &,uint64_t nowMs){
        if(state_==State::Offline || state_==State::Restarting || !power)return kNotReady;
        if(scanPending)return kBusy;
        if(state_==State::Joining || state_==State::Leaving)return kBusy;
        int r=requestScan();
        if(r)return r;
        scanPending=true;scanStartedMs=nowMs;scanForJoin=false;
        return kSuccess;
    }
    void abortScan(){finishScan(kAborted);}
    int startJoin(const AirportWCL::JoinRequest &j,uint64_t nowMs){
        if(state_==State::Offline || state_==State::Restarting || !power)return kNotReady;
        if(state_==State::Joining || state_==State::Leaving || scanPending)return kBusy;
        const Network *n=find(j.bssid);
        JoinPlan plan;
        const char *why=planJoin(j,n,plan);
        if(why){lastError=why;AthSecurity::wipe(&plan,sizeof(plan));return kUnsupported;}
        pending=plan;AthSecurity::wipe(&plan,sizeof(plan));
        memcpy(joinBSSID,j.bssid,6);
        state_=State::Joining;joinStartedMs=nowMs;joinPhase=JoinPhase::Begin;
        assocNotified=firstBeaconNotified=false;keysSubmitted=m4Sent=authorized=false;pendingLength=0;clearRekey();
        if(!n){ // Not in the last scan: scan first, then connect.
            if(status.link || status.stage){joinPhase=JoinPhase::Disconnect;}
            else joinPhase=JoinPhase::ScanFirst;
        } else if(status.link || status.stage)joinPhase=JoinPhase::Disconnect;
        return kSuccess;
    }
    int abortJoin(uint64_t nowMs){
        leaveNetwork(nowMs);
        sink.post(AirportWCL::JoinAbortComplete,nullptr,0);
        return kSuccess;
    }
    int leaveNetwork(uint64_t nowMs){
        (void)nowMs;
        if(state_==State::Joining)finishJoin(1,3,kAborted);
        const bool wasLinked=state_==State::Connected;
        if(wasLinked){
            if(status.link)postLink(false,3);
            requestDisconnect();
            state_=State::Leaving;
        } else if(state_==State::Joining){requestCancel();state_=State::Leaving;}
        wipeSecurity();
        return kSuccess;
    }
    int setPower(bool on,uint64_t nowMs){
        if(power==on)return kSuccess;
        power=on;
        if(!on){finishScan(kOffline);leaveNetwork(nowMs);}
        return kSuccess;
    }
    int setPowerSave(bool on){powerSave=on;powerSaveSynced=false;return kSuccess;}
    void stop(){finishScan(kOffline);finishJoin(1,0,kOffline);wipeSecurity();state_=State::Offline;}

    // --- Periodic poll -----------------------------------------------------
    void poll(uint64_t nowMs){
        const bool was=status.reachable && status.runtime;
        readStatus();
        if(!status.reachable || !status.runtime){
            if(was)goOffline(nowMs);
            if(status.reachable && state_!=State::Restarting && !(status.stateBits&1))requestRestart(nowMs);
            return;
        }
        if(!was){ // the core became ready (first time or after a restart)
            state_=State::Ready;restartRequests=0;
            uint32_t reset=1;sink.post(AirportWCL::ResetInterface,&reset,sizeof(reset));
            AirportWCL::DriverAvailability a{};a.version=3;a.available=1;a.reason=0xe0821803;
            sink.post(AirportWCL::DriverAvailable,&a,sizeof(a));
            if(status.scanDone)loadNetworks();
        }
        if(status.stateBits&4){requestRestart(nowMs);return;} // wake: restart required
        if(scanPending)pollScan(nowMs);
        switch(state_){
        case State::Joining:pollJoin(nowMs);break;
        case State::Connected:pollConnected(nowMs);break;
        case State::Leaving:
            if(!status.disconnecting && status.stage==0)state_=State::Ready;
            break;
        default:break;
        }
    }

    // --- Queries for the get* handlers ---------------------------------------
    bool bssInfo(uint8_t *out,size_t capacity,size_t &length) const {
        if(!associated())return false;
        length=buildScanResult(joined,out,capacity);return length!=0;
    }
    bool beaconInfo(uint8_t *out,size_t capacity,uint64_t nowMs){
        if(!associated() || !assocNotified)return false;
        ++beaconSequence;
        return buildBeaconFrame(joined,(nowMs-joinStartedMs)*1000u,beaconSequence,out,capacity)!=0;
    }
    const char *lastError="none";
    uint32_t lqmQuality() const { // 100/50/20 like AirportItlwm, -2 when down
        if(!linkUp() || status.rssi==INT32_MIN)return uint32_t(-2);
        return status.rssi>=-70?100:(status.rssi>=-85?50:20);
    }
private:
    enum class JoinPhase : uint8_t { Begin, Disconnect, ScanFirst, Connect, Associate, Handshake };
    Core &core;Sink &sink;
    State state_=State::Offline;
    CoreStatus status{};
    Network networks[64]{};uint32_t count=0;
    Network joined{};JoinPlan pending{};uint8_t joinBSSID[6]{};
    bool scanPending=false,scanForJoin=false;uint64_t scanStartedMs=0;uint32_t scanGeneration=0;
    JoinPhase joinPhase=JoinPhase::Begin;uint64_t joinStartedMs=0;
    bool assocNotified=false,firstBeaconNotified=false,linkIndicated=false;
    bool keysSubmitted=false,m4Sent=false,authorized=false;
    AthSecurity::Supplicant<Crypto> supplicant;
    // 56 bytes (TK, GTK, cipher, index, RSC) in the release build; 81 adds the IGTK block when AIRPORT_ATH10K_WPASEC is on.
    static constexpr uint32_t kKeyPayloadBytes=AIRPORT_ATH10K_WPASEC?81:56;
    uint8_t pendingFrame[160]{};size_t pendingLength=0;
    uint8_t rekeyKeys[AthSecurity::Supplicant<Crypto>::keyPayloadLength]{};uint32_t rekeyOperation=0;bool rekeyKeysReady=false,rekeyResponsePending=false;
    uint16_t beaconSequence=0;uint64_t lqmReportedMs=0,restartRequestedMs=0;uint32_t restartRequests=0;
    bool power=true,powerSave=true,powerSaveSynced=false;
    const Network *find(const uint8_t *bssid) const {for(uint32_t i=0;i<count;++i)if(sameMAC(networks[i].bssid,bssid))return &networks[i];return nullptr;}
    int call(uint32_t op,AthControl::Response &o,const AthControl::Request *req=nullptr){
        AthControl::Request r{};if(req)r=*req;r.operation=op;
        o=AthControl::Response{};return core.call(r,o);
    }
    int call(uint32_t op,uint32_t index=0,const uint8_t *data=nullptr,uint32_t length=0){
        AthControl::Request r{};r.operation=op;r.index=index;r.length=length;if(length)memcpy(r.data,data,length);
        AthControl::Response o{};return core.call(r,o);
    }
    void readStatus(){
        AthControl::Response s{},i{};
        const int rs=call(AthControl::Status,s);
        status.reachable=rs==kSuccess;
        if(!status.reachable){status.runtime=false;return;}
        status.runtime=(s.flags&1)!=0;status.scanDone=(s.flags&2)!=0;status.link=(s.flags&4)!=0;status.txBusy=(s.flags&8)!=0;
        status.secure=(s.flags&16)!=0;status.eapolTxFailed=(s.flags&32)!=0;status.disconnecting=(s.flags&64)!=0;status.scanning=(s.flags&256)!=0;
        status.stage=s.stage;status.phase=s.phase;status.count=s.count;memcpy(status.local,s.local,6);memcpy(status.bssid,s.bssid,6);
        memcpy(status.error,s.error,sizeof(status.error));status.error[95]=0;
        if(call(AthControl::Info,i)==kSuccess && i.length>=112){
            const uint32_t caps=le32(i.data);status.stationDFS=(caps&256)!=0;
            status.scanGeneration=le32(i.data+4);status.channel=le32(i.data+8);status.rssi=int32_t(le32(i.data+12));
            status.width=le32(i.data+16);status.nss=le32(i.data+20);status.stateBits=le32(i.data+28);
            status.psBits=le32(i.data+84);status.beaconMisses=le32(i.data+88);status.txHalfDbm=le32(i.data+92);status.scanBits=le32(i.data+108);
            if(i.length>=116 && i.data[112]){status.country[0]=char(i.data[112]);status.country[1]=char(i.data[113]);status.country[2]=0;}
        }
    }
    void goOffline(uint64_t){
        finishScan(kOffline);finishJoin(1,0,kOffline);
        if(linkIndicated)postLink(false,255);
        wipeSecurity();state_=State::Offline;
        AirportWCL::DriverAvailability a{};a.version=3;a.available=0;a.reason=0xe0821804;
        sink.post(AirportWCL::DriverAvailable,&a,sizeof(a));
    }
    void requestRestart(uint64_t nowMs){
        if(status.stateBits&2){lastError="dma-quarantine";return;} // never restart a quarantined adapter
        if(restartRequests>=3 || nowMs<restartRequestedMs+1000)return;
        if(state_!=State::Restarting){
            if(linkIndicated)postLink(false,255);
            finishScan(kOffline);finishJoin(1,0,kOffline);wipeSecurity();
            uint32_t reset=0;sink.post(AirportWCL::ResetInterface,&reset,sizeof(reset));
            state_=State::Restarting;
        }
        restartRequestedMs=nowMs;++restartRequests;
        call(AthControl::Restart); // on success the core terminates and rematches; this front end goes with it
    }
    int requestScan(){scanGeneration=status.scanGeneration+1;return call(AthControl::Scan,0);}
    void requestDisconnect(){if(call(AthControl::Disconnect)!=kSuccess)call(AthControl::Cancel);}
    void requestCancel(){call(AthControl::Cancel);}
    void loadNetworks(){
        count=0;
        for(uint32_t i=0;i<status.count && count<64;++i){
            AthControl::Request q{};q.index=i;AthControl::Response r{};
            if(call(AthControl::BSS,r,&q)!=kSuccess)continue;
            if(decodeNetwork(r.data,r.length,networks[count]))++count;
        }
    }
    void publishNetworks(){
        uint8_t buffer[sizeof(AirportWCL::BeaconMetadata)+600];
        for(uint32_t i=0;i<count;++i){
            const size_t n=buildScanResult(networks[i],buffer,sizeof(buffer));
            if(n)sink.post(AirportWCL::ScanResult,buffer,n);
        }
    }
    void pollScan(uint64_t nowMs){
        if(status.scanGeneration>=scanGeneration && status.scanDone){
            loadNetworks();publishNetworks();
            finishScan((status.scanBits&2)?kTimeout:kSuccess);
            return;
        }
        if(nowMs>scanStartedMs+60000)finishScan(kTimeout);
    }
    void finishScan(int result){
        if(!scanPending)return;
        scanPending=false;
        if(scanForJoin){scanForJoin=false;if(state_==State::Joining)joinPhase=JoinPhase::Connect;return;}
        if(result!=kSuccess)call(AthControl::Scan,1);
        sink.post(AirportWCL::ScanDone,&result,sizeof(result));
    }
    void postLink(bool up,uint32_t reason){
        AirportWCL::LinkStatusEvent e{};memcpy(e.bssid,joinBSSID,6);e.up=up?1:0;e.reason=reason;
        linkIndicated=up;sink.linkState(up);
        sink.post(AirportWCL::LinkStatus,&e,sizeof(e));
    }
    void finishJoin(uint16_t st,uint16_t reason,int driverStatus){
        if(state_!=State::Joining)return;
        AirportWCL::ConnectionEvent e{};e.status=st;e.reason=reason;memcpy(e.peers[0].bssid,joinBSSID,6);e.peers[0].driverStatus=driverStatus;
        sink.post(AirportWCL::ConnectComplete,&e,sizeof(e));
        if(st){
            if(linkIndicated)postLink(false,reason?reason:1);
            wipeSecurity();state_=State::Ready;
            if(status.stage && status.stage!=10)requestCancel();
        } else state_=State::Connected;
    }
    void postDeauth(uint16_t reason){
        AirportWCL::DeauthEvent e{};e.reason=reason;memcpy(e.bssid,joinBSSID,6);
        sink.post(AirportWCL::Deauth,&e,sizeof(e));
    }
    void wipeSecurity(){supplicant.reset();AthSecurity::wipe(pendingFrame,sizeof(pendingFrame));pendingLength=0;clearRekey();AthSecurity::wipe(&pending,sizeof(pending));}
    void clearRekey(){AthSecurity::wipe(rekeyKeys,sizeof(rekeyKeys));rekeyOperation=0;rekeyKeysReady=rekeyResponsePending=false;}
    void pollJoin(uint64_t nowMs){
        if(nowMs>joinStartedMs+30000){finishJoin(1,1004,kTimeout);return;}
        switch(joinPhase){
        case JoinPhase::Begin:joinPhase=JoinPhase::Connect;return;
        case JoinPhase::Disconnect:
            if(status.link || (status.stage && !status.disconnecting))requestDisconnect();
            if(!status.disconnecting && status.stage==0)joinPhase=(find(joinBSSID)?JoinPhase::Connect:JoinPhase::ScanFirst);
            return;
        case JoinPhase::ScanFirst:{
            if(scanPending)return;
            if(status.scanning)return; // a scan the core started itself; wait
            if(requestScan()==kSuccess){scanPending=true;scanForJoin=true;scanStartedMs=nowMs;}
            return;}
        case JoinPhase::Connect:{
            if(scanPending)return;
            const Network *n=find(joinBSSID);
            AirportWCL::JoinRequest j{};memcpy(j.ssid,pending.ssid,32);j.ssidLength=pending.ssidLength;memcpy(j.bssid,joinBSSID,6);
            JoinPlan plan=pending;
            if(!n){lastError="bss-not-in-scan-cache";finishJoin(1,1,kNotFound);return;}
            memcpy(j.key,plan.pmk,32);j.keyLength=plan.secure?32:0;j.keyCipher=6;j.lowerAuth=1;j.upperAuth=plan.secure?(1u<<3):0;
            const char *why=planJoin(j,n,plan);AthSecurity::wipe(&j,sizeof(j));
            if(why){lastError=why;AthSecurity::wipe(&plan,sizeof(plan));finishJoin(1,1,kUnsupported);return;}
            if(plan.secure){
                uint8_t nonce[32]{};
                if(!Crypto::random(nonce,32) || !supplicant.begin(plan.pmk,nonce,status.local,n->bssid,n->rsn,n->rsnLength)){
                    AthSecurity::wipe(nonce,32);lastError="supplicant-begin-failed";finishJoin(1,1,kError);return;}
                AthSecurity::wipe(nonce,32);
            }
            AthControl::Request r;buildConnectRequest(plan,r);AthControl::Response o{};
            AthSecurity::wipe(&plan,sizeof(plan));
            const int rc=core.call(r,o);
            if(rc!=kSuccess){lastError=o.error[0]?o.error:"connect-rejected";supplicant.reset();finishJoin(1,1,rc);return;}
            joined=*n;joinPhase=JoinPhase::Associate;return;}
        case JoinPhase::Associate:
        case JoinPhase::Handshake:
            if(status.stage==0 && !status.disconnecting){lastError=status.error;finishJoin(1,1,kError);return;}
            if(status.stage==11){lastError=status.error;requestCancel();finishJoin(1,1,kError);return;}
            if(status.stage!=10)return;
            if(!assocNotified){
                assocNotified=true;
                AirportWCL::AssociationEvent a{};memcpy(a.bssid,joinBSSID,6);a.driverStatus=kSuccess;
                sink.post(AirportWCL::AssocComplete,&a,sizeof(a));
                postLink(true,255);
                AirportWCL::FirstBeaconEvent f{};memcpy(f.peer.bssid,joinBSSID,6);
                sink.post(AirportWCL::FirstBeacon,&f,sizeof(f));firstBeaconNotified=true;
                joinPhase=JoinPhase::Handshake;
            }
            if(pending.secure){if(!serviceSupplicant())return;}
            if(status.link){finishJoin(0,0,kSuccess);lqmReportedMs=nowMs;powerSaveSynced=false;if(state_==State::Connected)syncPowerSave();}
            return;
        }
    }
    // Returns false when the join has been failed.
    bool serviceSupplicant(){
        using S=AthSecurity::Supplicant<Crypto>;
        for(unsigned i=0;i<4 && !rekeyKeysReady && !rekeyResponsePending;++i){
            AthControl::Response rx{};
            if(call(AthControl::ReceiveEAPOL,rx)!=kSuccess)break;
            const auto result=supplicant.receive(rx.data,rx.length);
            AthSecurity::wipe(rx.data,sizeof(rx.data));
            if(result==S::Fatal){lastError=supplicant.error;fail(23);return false;}
            if(result==S::Keys){
                AthControl::Request keys{};keys.length=kKeyPayloadBytes;memcpy(keys.data,supplicant.keys,keys.length);
                AthControl::Response o{};keys.operation=AthControl::InstallKeys;
                const int r=core.call(keys,o);
                AthSecurity::wipe(keys.data,sizeof(keys.data));AthSecurity::wipe(supplicant.keys,sizeof(supplicant.keys));
                if(r){lastError="install-keys-rejected";fail(19);return false;}
                keysSubmitted=true;
            }
            if(result==S::GroupKeys || result==S::PairKeys){
                memcpy(rekeyKeys,supplicant.keys,sizeof(rekeyKeys));
                rekeyOperation=result==S::GroupKeys?AthControl::RekeyGroup:AthControl::RekeyPair;
                rekeyKeysReady=rekeyResponsePending=true;AthSecurity::wipe(supplicant.keys,sizeof(supplicant.keys));
            }
            if(result==S::Keys || result==S::Reply || result==S::GroupKeys || result==S::PairKeys){
                memcpy(pendingFrame,supplicant.output,supplicant.outputLength);pendingLength=supplicant.outputLength;
            }
            if(rekeyKeysReady)break;
        }
        if(rekeyKeysReady){
            const int r=call(rekeyOperation,0,rekeyKeys,kKeyPayloadBytes);
            if(!r){AthSecurity::wipe(rekeyKeys,sizeof(rekeyKeys));rekeyOperation=0;rekeyKeysReady=false;}
            else if(r!=kBusy && r!=kNotReady){lastError="rekey-rejected";fail(18);return false;}
        }
        const bool maySend=rekeyResponsePending?status.phase>=5:(!keysSubmitted || status.phase>=3);
        if(pendingLength && !rekeyKeysReady && maySend){
            const int r=call(AthControl::SendEAPOL,0,pendingFrame,uint32_t(pendingLength));
            if(!r){pendingLength=0;if(rekeyResponsePending)rekeyResponsePending=false;else if(keysSubmitted)m4Sent=true;}
            else if(r!=kBusy && r!=kNotReady){lastError="eapol-send-failed";fail(15);return false;}
        }
        if(m4Sent && !authorized && !status.txBusy && status.phase==3){
            if(call(AthControl::Authorize)==kSuccess)authorized=true;
        }
        return true;
    }
    void fail(uint16_t reason){
        if(state_==State::Joining)finishJoin(1,reason,kError);
        else if(state_==State::Connected){postLink(false,reason);postDeauth(reason);requestDisconnect();wipeSecurity();state_=State::Leaving;}
    }
    void syncPowerSave(){
        const bool have=(status.psBits&1)!=0;
        if(have!=powerSave){if(call(AthControl::RadioOptions,powerSave?1:0)==kSuccess)powerSaveSynced=true;}
        else powerSaveSynced=true;
    }
    void pollConnected(uint64_t nowMs){
        if(status.stage!=10 || status.disconnecting || !status.link){
            // The core dropped the link (AP deauth, beacon loss, cancel).
            const uint16_t reason=status.stage==10 && status.disconnecting?3:(status.beaconMisses?4:1);
            postLink(false,reason);postDeauth(reason);wipeSecurity();
            state_=(status.stage==0 && !status.disconnecting)?State::Ready:State::Leaving;
            return;
        }
        if(status.secure && !serviceSupplicant())return;
        if(!powerSaveSynced)syncPowerSave();
        if(nowMs>=lqmReportedMs+2000){
            const uint32_t expected=uint32_t((nowMs-lqmReportedMs)*1000u/(uint32_t(joined.beaconInterval)*1024u));
            lqmReportedMs=nowMs;
            AirportWCL::LqmBeaconUpdate u;
            if(AirportWCL::makeBeaconUpdate(expected?expected:1,u))sink.post(AirportWCL::LqmUpdate,&u,sizeof(u));
        }
    }
};
}
