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
    uint8_t wpa[34]{},wpaLength=0; // WPA1 vendor element
    uint32_t snr=0;
    int32_t rssi() const {return int32_t(snr>95?95:snr)-95;} // Linux ath10k convention (Ath10kControl.hpp): signal = SNR + noise floor (-95 dBm)
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
        at+=105+(AIRPORT_ATH10K_WPASEC?19u:0u);
        if(length>=at+1 && d[at]>=6 && d[at]<=sizeof(n.wpa) && length>=at+1u+d[at] && d[at+1]==221 && d[at+2]+2u==d[at]){
            n.wpaLength=d[at];memcpy(n.wpa,d+at+1,n.wpaLength);}
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
    if(n.wpaLength>=6){if(at+n.wpaLength>capacity)return 0;memcpy(out+at,n.wpa,n.wpaLength);at+=n.wpaLength;}
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
    bool secure=false,hidden=false,macSet=false;uint8_t bssid[6]{},ssid[32]{},ssidLength=0,pmk[32]{},mac[6]{};
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
    if(plan.secure && !n->wpa2){
        // Name the reason (the refusal's 802.11 status follows from it, refusalStatus).
        AthSecurity::RSNInfo rsn;
        if(!n->rsnLength)return n->wpaLength?"wpa1-only-not-supported":"wpa2-psk-ccmp-pairwise-required";
        if(!AthSecurity::parseRSN(n->rsn,n->rsnLength,rsn))return "rsn-not-understood";
        if(!rsn.akm(2) && (rsn.akm(8) || rsn.akm(24)))return "wpa3-sae-only-not-supported";
        if(!rsn.akm(2) && (rsn.akm(1) || rsn.akm(5)))return "enterprise-not-supported";
        if(rsn.mfpr)return "pmf-required-not-supported";
        if(!rsn.pairwiseCCMP)return "tkip-pairwise-not-supported";
        return "wpa2-psk-ccmp-pairwise-required";
    }
    if(!plan.secure && n->privacy)return "protected-network-not-supported";
    if(!channelKnown(n->channel))return "bss-channel-unsupported";
    plan.hidden=n->hidden;
    if(!n->hidden && (n->ssidLength!=j.ssidLength || memcmp(n->ssid,j.ssid,j.ssidLength)))return "ssid-mismatch";
    return nullptr;
}
// IEEE 802.11 status for a join this card cannot make: reported to WCL in
// AssocComplete/ConnectComplete as the association result, like a real
// refusal, so macOS records an incompatible network instead of a wrong
// password (a synchronous error or an auth-style failure made airportd ask for
// the password again). 0 = not a capability refusal.
inline uint16_t refusalStatus(const char *why){
    if(!why)return 0;
    if(!strcmp(why,"wpa3-sae-only-not-supported") || !strcmp(why,"enterprise-not-supported") || !strcmp(why,"auth-not-supported"))return 43; // invalid AKMP
    if(!strcmp(why,"tkip-pairwise-not-supported") || !strcmp(why,"wpa1-only-not-supported") || !strcmp(why,"wpa2-psk-ccmp-pairwise-required"))return 42; // invalid pairwise cipher
    if(!strcmp(why,"pmf-required-not-supported"))return 31; // robust management frame policy violation
    if(!strcmp(why,"wep-not-supported"))return 13;           // authentication algorithm not supported
    if(!strcmp(why,"protected-network-not-supported") || !strcmp(why,"rsn-not-understood"))return 40; // invalid element
    if(!strcmp(why,"bss-channel-unsupported"))return 24;     // channels not supported
    return 1;
}
inline void buildConnectRequest(const JoinPlan &p,AthControl::Request &r){
    r=AthControl::Request{};r.operation=AthControl::Connect;r.index=p.hidden?1:0;
    r.data[0]=p.ssidLength;r.data[1]=p.secure?1:0;memcpy(r.data+2,p.bssid,6);memcpy(r.data+8,p.ssid,p.ssidLength);
    r.length=8u+p.ssidLength;
    if(p.macSet){ // Private Wi-Fi Address: the extended block with the station MAC alone (flags 16)
        uint8_t *x=r.data+r.length;memset(x,0,10);x[0]=16;memcpy(x+2,p.mac,6);
        r.index|=2;r.length+=10;
    }
}

// --- Scan requests --------------------------------------------------------
// Core scan channels are kChannels indices (the core's kWMIScanPlan order).
inline uint64_t allChannelMask(){return (uint64_t(1)<<kChannelCount)-1;}
inline uint64_t channelBit(unsigned channel){for(unsigned i=0;i<kChannelCount;++i)if(kChannels[i].number==channel)return uint64_t(1)<<i;return 0;}
// The channels a WCL scan request names (its apple80211 bitmap), all of them when it names none.
inline uint64_t scanMaskFromWCL(const AirportWCL::ScanRequest &s){
    if(!s.channelCount)return allChannelMask();
    uint64_t mask=0;
    for(unsigned i=0;i<kChannelCount;++i)if(AirportWCL::scanChannelSelected(s.channels,true,kChannels[i].number))mask|=uint64_t(1)<<i;
    return mask;
}
struct ScanPlan { uint64_t mask=0; uint8_t ssid[32]{}; uint8_t ssidLength=0; bool passive=false; };
// Control ABI Scan, index 0: with the core's scan options (Info capability
// 2048) a 42-byte block: channel mask u64 (0 = all), flags (1 passive only),
// SSID length, SSID; a full undirected scan stays the plain request.
inline void buildScanRequest(const ScanPlan &p,bool options,AthControl::Request &r){
    r=AthControl::Request{};r.operation=AthControl::Scan;r.index=0;
    if(!options)return;
    const uint64_t mask=p.mask==allChannelMask()?0:p.mask;
    if(!mask && !p.ssidLength && !p.passive)return;
    put64(r.data,mask);r.data[8]=p.passive?1:0;r.data[9]=p.ssidLength;memcpy(r.data+10,p.ssid,p.ssidLength);r.length=42;
}

// Snapshot of the core from one Status + Info poll.
struct CoreStatus {
    bool reachable=false,runtime=false,scanDone=false,link=false,txBusy=false,secure=false,eapolTxFailed=false,disconnecting=false,scanning=false;
    uint32_t stage=0,phase=0,count=0,scanGeneration=0,channel=0,width=0,nss=0,scanBits=0,stateBits=0,psBits=0,beaconMisses=0,txHalfDbm=0,caps=0;
    int32_t rssi=INT32_MIN;uint8_t local[6]{},bssid[6]{};char error[96]{};char country[3]{'0','0',0};bool stationDFS=false;
};

// The session: WCL requests in, core polls, WCL events out. Single-threaded
// (the kext runs every entry point under the IO80211 work-queue gate).
//
// Scans: a WCL request is never refused for being busy. Cached results
// (heard in the last 30 s) are posted at once, so the menu fills before the
// radio has moved; a core scan of the requested channels then runs as soon
// as the core can (a scan the core started itself is adopted, overlapping
// requests are merged), its results are posted as they arrive, and ScanDone
// follows when every requested channel has been covered. While a join or
// leave owns the radio the request is answered from the cache after 1.5 s.
//
// Joins: setWCL_ASSOCIATE starts the core's Connect (vdev, peer, auth, assoc)
// once the core is idle, scanning first (directed, the candidate's channel)
// when the core's BSS cache lacks the BSSID. AssocComplete, LinkStatus and
// FirstBeacon are posted when the core reaches stage 10 (associated, vdev up);
// the in-kernel supplicant runs the 4-way handshake with the PMK from WCL and
// installs the keys; the BSD link and ConnectComplete follow the authorized
// port. Nothing waits on a fixed delay; the kext polls every 20 ms while a
// scan or join is in progress.
template<class Crypto> class Session {
public:
    enum class State : uint8_t { Offline, Ready, Joining, Connected, Leaving, Restarting };
    static constexpr uint64_t kCacheFreshMs=30000,kCacheKeepMs=120000,kScanGraceMs=1500,kScanLimitMs=40000,kJoinLimitMs=30000;
    Session(Core &core,Sink &sink):core(core),sink(sink){}
    State state() const {return state_;}
    static const char *stateName(State s){
        switch(s){case State::Offline:return "offline";case State::Ready:return "ready";case State::Joining:return "joining";
        case State::Connected:return "connected";case State::Leaving:return "leaving";case State::Restarting:return "restarting";}
        return "?";
    }
    bool scanning() const {return wclScanOwed || coreScan;}
    bool wclScanPending() const {return wclScanOwed;}
    bool coreScanning() const {return coreScan;}
    bool joining() const {return state_==State::Joining;}
    // Fast polling while something waits on the core.
    bool busy() const {return state_==State::Joining || state_==State::Leaving || scanning();}
    bool linkUp() const {return state_==State::Connected && status.link;}
    const CoreStatus &core_() const {return status;}
    // The BSS WCL is associated with: from AssocComplete on (WCL then reads
    // EXTENDED_BSS_INFO, BSS_INFO, RSSI, CHANNEL before the 4-way handshake ends).
    const Network *associated() const {return (state_==State::Connected || (state_==State::Joining && assocNotified))?&joined:nullptr;}
    uint32_t networkCount() const {return count;}
    const Network &network(uint32_t i) const {return networks[i<count?i:0];}
    bool powered() const {return power;}

    // --- WCL requests ----------------------------------------------------
    int startScan(const AirportWCL::ScanRequest &req,uint64_t nowMs){
        if(state_==State::Offline || state_==State::Restarting || !power)return kNotReady;
        const uint64_t mask=scanMaskFromWCL(req);
        if(!mask)return kUnsupported;
        if(wclScanOwed){ // merged into the scan WCL already waits for (the wildcard probe covers different SSIDs)
            wclWant.mask|=mask;
            if(req.type!=2 && (req.ssidLength!=wclWant.ssidLength || memcmp(req.ssid,wclWant.ssid,wclWant.ssidLength)))wclWant.ssidLength=0;
            if(req.type!=2)wclWant.passive=false;
            return kSuccess;
        }
        wclScanOwed=true;wclWant=ScanPlan{};wclWant.mask=mask;wclWant.passive=req.type==2;
        if(req.type!=2 && req.ssidLength && req.ssidLength<=32){wclWant.ssidLength=uint8_t(req.ssidLength);memcpy(wclWant.ssid,req.ssid,req.ssidLength);}
        wclScanRequestedMs=nowMs;wclCachePending=true;
        return kSuccess;
    }
    void abortScan(){
        if(!wclScanOwed)return;
        if(coreScan && !coreScanForJoin)call(AthControl::Scan,1);
        finishWCLScan(kAborted);
    }
    int startJoin(const AirportWCL::JoinRequest &j,uint64_t nowMs){
        if(state_==State::Offline || state_==State::Restarting || !power)return kNotReady;
        if(state_==State::Joining)return kBusy;
        const Network *n=find(j.bssid);
        JoinPlan plan;
        const char *why=planJoin(j,n,plan);
        if(why){
            setError(why);AthSecurity::wipe(&plan,sizeof(plan));
            if(!strcmp(why,"ssid-mismatch"))return kBadArgument;
            // A network this card cannot join: accepted, then refused with an
            // 802.11 status on the next poll (refusalStatus), not as a driver error.
            if(state_==State::Connected){if(linkIndicated)postLink(false,3);requestDisconnect();} // a card leaves the old network for a new join
            memcpy(joinBSSID,j.bssid,6);refusal=refusalStatus(why);
            state_=State::Joining;joinStartedMs=nowMs;joinPhase=JoinPhase::Refuse;assocNotified=false;
            return kSuccess;
        }
        pending=plan;AthSecurity::wipe(&plan,sizeof(plan));
        memcpy(joinBSSID,j.bssid,6);
        joinChannel=uint8_t(j.channelSpec&0xff);
        if(!channelKnown(joinChannel))joinChannel=n?n->channel:0;
        state_=State::Joining;joinStartedMs=nowMs;joinPhase=JoinPhase::Prepare;joinRescanned=false;joinCancelMs=joinScanCancelMs=0;
        assocNotified=firstBeaconNotified=false;keysSubmitted=m4Sent=authorized=false;pendingLength=0;clearRekey();
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
        if(state_==State::Connected){
            if(linkIndicated)postLink(false,3);
            requestDisconnect();
            state_=State::Leaving;
        }
        wipeSecurity();
        return kSuccess;
    }
    int setPower(bool on,uint64_t nowMs){
        if(power==on)return kSuccess;
        power=on;
        if(!on){if(wclScanOwed)finishWCLScan(kOffline);if(coreScan)call(AthControl::Scan,1);leaveNetwork(nowMs);}
        return kSuccess;
    }
    int setPowerSave(bool on){powerSave=on;powerSaveSynced=false;return kSuccess;}
    // System sleep/wake (the controller's PM state, not the user's Wi-Fi
    // switch): timestamps and the outcome for the registry mirror.
    void pmSleep(uint64_t nowMs){lastSleepMs=nowMs;waking=false;wakeResult="sleeping";++wakeSerial;}
    void pmWake(uint64_t nowMs){lastWakeMs=nowMs;waking=true;wakeResult="pending";++wakeSerial;}
    // Private Wi-Fi Address from WCL (nullptr = back to the permanent address);
    // applied to the firmware vdev by the next join (extended Connect, flags 16).
    void setStationMAC(const uint8_t *mac){stationMACSet=mac!=nullptr;if(mac)memcpy(stationMAC,mac,6);else memset(stationMAC,0,6);}
    bool hasStationMAC() const {return stationMACSet;}
    void stop(){if(wclScanOwed)finishWCLScan(kOffline);finishJoin(1,0,kOffline);wipeSecurity();coreScan=false;state_=State::Offline;}

    // --- Periodic poll -----------------------------------------------------
    void poll(uint64_t nowMs){
        pollMs=nowMs;
        const bool was=status.reachable && status.runtime;
        readStatus();
        if(!status.reachable || !status.runtime){
            if(was)goOffline(nowMs);
            // 1: suspended, 8: the core reloads its firmware in place after a
            // wake (it comes back by itself); otherwise ask for the guarded restart.
            if(status.reachable && state_!=State::Restarting && !(status.stateBits&(1|8)))requestRestart(nowMs);
            if(waking && (status.stateBits&2)){wakeResult="dma-quarantine";++wakeSerial;}
            return;
        }
        if(!was){ // the core became ready (first time or after a restart)
            state_=State::Ready;restartRequests=0;coreScan=false;
            uint32_t reset=1;sink.post(AirportWCL::ResetInterface,&reset,sizeof(reset));
            AirportWCL::DriverAvailability a{};a.version=3;a.available=1;a.reason=0xe0821803;
            sink.post(AirportWCL::DriverAvailable,&a,sizeof(a));
            if(status.scanDone)loadNetworks(nowMs,false);
            if(waking){waking=false;wakeMs=uint32_t(nowMs-lastWakeMs);if(strcmp(wakeResult,"restart-requested"))wakeResult="reinit";++wakeSerial;}
        }
        if(status.stateBits&4){requestRestart(nowMs);return;} // wake: restart required
        trackScans(nowMs);
        switch(state_){
        case State::Joining:pollJoin(nowMs);break;
        case State::Connected:pollConnected(nowMs);break;
        case State::Leaving:
            if(!status.disconnecting && status.stage==0)state_=State::Ready;
            else if(status.stage && status.stage!=10 && !status.disconnecting && nowMs>=leaveCancelMs+2000){requestCancel();leaveCancelMs=nowMs;}
            break;
        default:break;
        }
        startScans(nowMs);
    }

    // --- Queries for the get* handlers ---------------------------------------
    bool bssInfo(uint8_t *out,size_t capacity,size_t &length) const {
        if(!associated())return false;
        length=buildScanResult(joined,out,capacity);
        if(length && status.rssi!=INT32_MIN && status.rssi<0 && status.rssi>-120){ // live signal of the associated BSS
            auto *m=reinterpret_cast<AirportWCL::BeaconMetadata *>(out);m->rssi=status.rssi;m->snr=int16_t(status.rssi+95);
        }
        return length!=0;
    }
    bool beaconInfo(uint8_t *out,size_t capacity,uint64_t nowMs){
        if(!associated())return false;
        ++beaconSequence;
        return buildBeaconFrame(joined,(nowMs-joinStartedMs)*1000u,beaconSequence,out,capacity)!=0;
    }
    // Last failure, copied (core texts live in transient responses); errorSerial
    // changes with every update so the registry mirror sees new text.
    const char *lastError="none";uint32_t errorSerial=0;
    uint32_t lastDisconnectReason=0; // 802.11 reason of the last link-down/deauth we reported (255 = driver-initiated)
    uint32_t lastScanMs=0,lastJoinMs=0,scanCount=0; // registry mirror: duration of the last core scan and of the last successful join
    // Last system sleep/wake (uptime ms), wake to driver-available (ms) and how
    // the core came back: reinit (firmware reloaded in place), restart-requested
    // (guarded terminate + rematch), dma-quarantine, pending, sleeping.
    uint64_t lastSleepMs=0,lastWakeMs=0;uint32_t wakeMs=0,wakeSerial=0;const char *wakeResult="none";bool waking=false;
    uint32_t lqmQuality() const { // 100/50/20 like AirportItlwm, -2 when down
        if(!linkUp() || status.rssi==INT32_MIN)return uint32_t(-2);
        return status.rssi>=-70?100:(status.rssi>=-85?50:20);
    }
private:
    enum class JoinPhase : uint8_t { Refuse, Prepare, ScanFirst, WaitScan, Connect, Associate, Handshake };
    uint16_t refusal=0;
    Core &core;Sink &sink;
    State state_=State::Offline;
    CoreStatus status{};
    // Scan cache: every BSS heard, with the time it was last heard.
    Network networks[64]{};uint64_t seenMs[64]{};uint32_t count=0;
    Network joined{};JoinPlan pending{};uint8_t joinBSSID[6]{},joinChannel=0;
    // WCL scan owed (ScanDone not posted yet) and the core scan in progress.
    bool wclScanOwed=false,wclCachePending=false;ScanPlan wclWant{};uint64_t wclScanRequestedMs=0;
    bool coreScan=false,coreScanForJoin=false;uint64_t coreScanMask=0,coreScanStartedMs=0;uint32_t coreScanGeneration=0,coreScanPublished=0;
    JoinPhase joinPhase=JoinPhase::Prepare;uint64_t pollMs=0,joinStartedMs=0,joinCancelMs=0,joinScanCancelMs=0,leaveCancelMs=0;bool joinRescanned=false;
    bool assocNotified=false,firstBeaconNotified=false,linkIndicated=false,dataLink=false;
    bool keysSubmitted=false,m4Sent=false,authorized=false;
    AthSecurity::Supplicant<Crypto> supplicant;
    // 56 bytes (TK, GTK, cipher, index, RSC) in the release build; 81 adds the IGTK block when AIRPORT_ATH10K_WPASEC is on.
    static constexpr uint32_t kKeyPayloadBytes=AIRPORT_ATH10K_WPASEC?81:56;
    uint8_t pendingFrame[160]{};size_t pendingLength=0;
    uint8_t rekeyKeys[AthSecurity::Supplicant<Crypto>::keyPayloadLength]{};uint32_t rekeyOperation=0;bool rekeyKeysReady=false,rekeyResponsePending=false;
    uint16_t beaconSequence=0;uint64_t lqmReportedMs=0,restartRequestedMs=0;uint32_t restartRequests=0;
    bool power=true,powerSave=true,powerSaveSynced=false;
    uint8_t stationMAC[6]{};bool stationMACSet=false;
    const Network *find(const uint8_t *bssid) const {for(uint32_t i=0;i<count;++i)if(sameMAC(networks[i].bssid,bssid))return &networks[i];return nullptr;}
    int call(uint32_t op,AthControl::Response &o,const AthControl::Request *req=nullptr){
        AthControl::Request r{};if(req)r=*req;r.operation=op;
        o=AthControl::Response{};return core.call(r,o);
    }
    int call(uint32_t op,uint32_t index=0,const uint8_t *data=nullptr,uint32_t length=0){
        AthControl::Request r{};r.operation=op;r.index=index;r.length=length;if(length)memcpy(r.data,data,length);
        AthControl::Response o{};return core.call(r,o);
    }
    char errorText[96]{};
    void setError(const char *text){
        size_t n=0;if(text)for(;n<sizeof(errorText)-1 && text[n];++n)errorText[n]=text[n];
        errorText[n]=0;lastError=n?errorText:"unknown";++errorSerial;
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
            const uint32_t caps=le32(i.data);status.caps=caps;status.stationDFS=(caps&256)!=0;
            status.scanGeneration=le32(i.data+4);status.channel=le32(i.data+8);status.rssi=int32_t(le32(i.data+12));
            status.width=le32(i.data+16);status.nss=le32(i.data+20);status.stateBits=le32(i.data+28);
            status.psBits=le32(i.data+84);status.beaconMisses=le32(i.data+88);status.txHalfDbm=le32(i.data+92);status.scanBits=le32(i.data+108);
            if(i.length>=116 && i.data[112]){status.country[0]=char(i.data[112]);status.country[1]=char(i.data[113]);status.country[2]=0;}
        }
    }
    void goOffline(uint64_t){
        if(wclScanOwed)finishWCLScan(kOffline);
        coreScan=false;finishJoin(1,0,kOffline);
        if(linkIndicated)postLink(false,255);
        setDataLink(false);
        wipeSecurity();state_=State::Offline;
        AirportWCL::DriverAvailability a{};a.version=3;a.available=0;a.reason=0xe0821804;
        sink.post(AirportWCL::DriverAvailable,&a,sizeof(a));
    }
    void requestRestart(uint64_t nowMs){
        if(status.stateBits&2){setError("dma-quarantine");return;} // never restart a quarantined adapter
        if(restartRequests>=3 || nowMs<restartRequestedMs+1000)return;
        if(state_!=State::Restarting){
            if(linkIndicated)postLink(false,255);
            setDataLink(false);
            if(wclScanOwed)finishWCLScan(kOffline);
            coreScan=false;finishJoin(1,0,kOffline);wipeSecurity();
            uint32_t reset=0;sink.post(AirportWCL::ResetInterface,&reset,sizeof(reset));
            state_=State::Restarting;
        }
        restartRequestedMs=nowMs;++restartRequests;
        if(waking){wakeResult="restart-requested";++wakeSerial;}
        call(AthControl::Restart); // on success the core terminates and rematches; this front end goes with it
    }
    void requestDisconnect(){if(call(AthControl::Disconnect)!=kSuccess)call(AthControl::Cancel);}
    void requestCancel(){call(AthControl::Cancel);}

    // --- Scan cache ----------------------------------------------------------
    bool readNetwork(uint32_t index,Network &n){
        AthControl::Request q{};q.index=index;AthControl::Response r{};
        return call(AthControl::BSS,r,&q)==kSuccess && decodeNetwork(r.data,r.length,n);
    }
    void merge(const Network &n,uint64_t nowMs){
        uint32_t slot=count;
        for(uint32_t i=0;i<count;++i)if(sameMAC(networks[i].bssid,n.bssid)){slot=i;break;}
        if(slot==count){
            if(count<64)++count;
            else {slot=0;for(uint32_t i=1;i<count;++i)if(seenMs[i]<seenMs[slot])slot=i;} // full: replace the stalest
        }
        networks[slot]=n;seenMs[slot]=nowMs;
    }
    void pruneCache(uint64_t nowMs){
        for(uint32_t i=0;i<count;)
            if(nowMs>seenMs[i]+kCacheKeepMs){networks[i]=networks[count-1];seenMs[i]=seenMs[count-1];--count;}
            else ++i;
    }
    void publish(const Network &n){
        uint8_t buffer[sizeof(AirportWCL::BeaconMetadata)+600];
        const size_t length=buildScanResult(n,buffer,sizeof(buffer));
        if(length)sink.post(AirportWCL::ScanResult,buffer,length);
    }
    bool wanted(const Network &n) const {return wclScanOwed && (channelBit(n.channel)&wclWant.mask)!=0;}
    void publishCached(uint64_t nowMs){
        for(uint32_t i=0;i<count;++i)if(nowMs<=seenMs[i]+kCacheFreshMs && wanted(networks[i]))publish(networks[i]);
    }
    // All entries of the core's BSS cache (the last core scan) into ours.
    void loadNetworks(uint64_t nowMs,bool post){
        for(uint32_t i=0;i<status.count && i<64;++i){Network n;if(readNetwork(i,n)){merge(n,nowMs);if(post && wanted(n))publish(n);}}
        pruneCache(nowMs);
    }

    // --- Scans -----------------------------------------------------------------
    bool startCoreScan(const ScanPlan &plan,uint64_t nowMs,bool forJoin){
        const bool options=(status.caps&2048)!=0;
        AthControl::Request r;buildScanRequest(plan,options,r);AthControl::Response o{};
        const uint32_t expected=status.scanGeneration+1;
        if(core.call(r,o)!=kSuccess)return false; // busy (connection/key/radio work): the next poll retries
        coreScan=true;coreScanForJoin=forJoin;coreScanMask=options?plan.mask:allChannelMask();
        coreScanGeneration=expected;coreScanStartedMs=nowMs;coreScanPublished=0;++scanCount;
        return true;
    }
    void pollCoreScan(uint64_t nowMs){
        const bool done=status.scanGeneration>=coreScanGeneration && status.scanDone;
        if(!done){
            // Results as they arrive: the core fills its cache entry by entry.
            if(status.scanGeneration==coreScanGeneration && status.count>coreScanPublished){
                for(uint32_t i=coreScanPublished;i<status.count && i<64;++i){Network n;if(readNetwork(i,n)){merge(n,nowMs);if(wanted(n))publish(n);}}
                coreScanPublished=status.count;
            }
            if(wclScanOwed && nowMs>coreScanStartedMs+kScanLimitMs)finishWCLScan(kTimeout);
            return;
        }
        coreScan=false;lastScanMs=uint32_t(nowMs-coreScanStartedMs);
        loadNetworks(nowMs,true); // final values (an entry is updated by every frame heard)
        coreScanForJoin=false;
        if(wclScanOwed){
            const bool failed=(status.scanBits&2)!=0;
            wclWant.mask&=~coreScanMask;
            if(failed || !wclWant.mask)finishWCLScan(failed?kTimeout:kSuccess);
        }
    }
    void trackScans(uint64_t nowMs){
        if(wclScanOwed && wclCachePending){wclCachePending=false;publishCached(nowMs);}
        if(coreScan)pollCoreScan(nowMs);
    }
    void startScans(uint64_t nowMs){
        if(!wclScanOwed || coreScan)return;
        const bool radioBusy=state_==State::Joining || state_==State::Leaving || status.disconnecting;
        if(!radioBusy && status.scanning){ // a scan the core started itself (the boot scan): it serves this request
            coreScan=true;coreScanForJoin=false;coreScanMask=allChannelMask();coreScanGeneration=status.scanGeneration;
            coreScanStartedMs=nowMs;coreScanPublished=0;return;
        }
        if(!radioBusy && startCoreScan(wclWant,nowMs,false))return;
        if(nowMs>=wclScanRequestedMs+kScanGraceMs)finishWCLScan(kSuccess); // answered from the cache
    }
    void finishWCLScan(int result){
        if(!wclScanOwed)return;
        wclScanOwed=wclCachePending=false;
        sink.post(AirportWCL::ScanDone,&result,sizeof(result));
    }

    // --- Link and join events -------------------------------------------------
    // WCL LinkStatus event; the BSD link follows the core's data path (the
    // authorized port), so DHCP never starts before the 4-way handshake.
    void postLink(bool up,uint32_t reason){
        if(!up)lastDisconnectReason=reason;
        AirportWCL::LinkStatusEvent e{};memcpy(e.bssid,joinBSSID,6);e.up=up?1:0;e.reason=reason;
        linkIndicated=up;
        sink.post(AirportWCL::LinkStatus,&e,sizeof(e));
        setDataLink(up && status.link);
    }
    void setDataLink(bool up){if(up!=dataLink){dataLink=up;sink.linkState(up);}}
    void finishJoin(uint16_t st,uint16_t reason,int driverStatus){
        if(state_!=State::Joining)return;
        AirportWCL::ConnectionEvent e{};e.status=st;e.reason=reason;memcpy(e.peers[0].bssid,joinBSSID,6);e.peers[0].driverStatus=driverStatus;
        sink.post(AirportWCL::ConnectComplete,&e,sizeof(e));
        if(st){
            if(linkIndicated)postLink(false,reason?reason:1);
            setDataLink(false);
            wipeSecurity();
            // Tear down whatever the core holds for this join; the next join or
            // scan waits for it (state Leaving).
            const bool held=(status.stage && joinPhase>=JoinPhase::Associate) || status.disconnecting;
            if(status.stage && joinPhase>=JoinPhase::Associate)requestDisconnect();
            state_=held?State::Leaving:State::Ready;leaveCancelMs=pollMs;
        } else state_=State::Connected;
    }
    void postDeauth(uint16_t reason){
        lastDisconnectReason=reason;
        AirportWCL::DeauthEvent e{};e.reason=reason;memcpy(e.bssid,joinBSSID,6);
        sink.post(AirportWCL::Deauth,&e,sizeof(e));
    }
    void wipeSecurity(){supplicant.reset();AthSecurity::wipe(pendingFrame,sizeof(pendingFrame));pendingLength=0;clearRekey();AthSecurity::wipe(&pending,sizeof(pending));}
    void clearRekey(){AthSecurity::wipe(rekeyKeys,sizeof(rekeyKeys));rekeyOperation=0;rekeyKeysReady=rekeyResponsePending=false;}
    void pollJoin(uint64_t nowMs){
        if(nowMs>joinStartedMs+kJoinLimitMs){setError("join-timeout");finishJoin(1,1004,kTimeout);return;}
        switch(joinPhase){
        case JoinPhase::Refuse:refuse(refusal);return;
        case JoinPhase::Prepare:
            // The core must be idle: no connection, no teardown, no scan.
            if(status.disconnecting)return;
            if(status.stage){
                if(nowMs>=joinCancelMs+2000){joinCancelMs=nowMs;if(status.stage==10)requestDisconnect();else requestCancel();}
                return;
            }
            if(coreScan || status.scanning){
                if(!coreScanForJoin && nowMs>=joinScanCancelMs+2000){joinScanCancelMs=nowMs;call(AthControl::Scan,1);}
                return;
            }
            joinPhase=find(joinBSSID)?JoinPhase::Connect:JoinPhase::ScanFirst;
            if(joinPhase==JoinPhase::Connect)connect();
            return;
        case JoinPhase::ScanFirst:{
            if(coreScan || status.scanning)return;
            // Directed scan: the candidate's channel (all channels when unknown), the SSID probed by name.
            ScanPlan p{};p.mask=channelBit(joinChannel);if(!p.mask)p.mask=allChannelMask();
            p.ssidLength=pending.ssidLength;memcpy(p.ssid,pending.ssid,pending.ssidLength);
            if(startCoreScan(p,nowMs,true))joinPhase=JoinPhase::WaitScan;
            return;}
        case JoinPhase::WaitScan:
            if(coreScan)return;
            joinPhase=JoinPhase::Connect;connect();
            return;
        case JoinPhase::Connect:connect();return;
        case JoinPhase::Associate:
        case JoinPhase::Handshake:
            if(status.stage==0 && !status.disconnecting){setError(status.error);finishJoin(1,1,kError);return;}
            if(status.stage==11){setError(status.error);requestCancel();finishJoin(1,1,kError);return;}
            if(status.stage!=10)return;
            if(!assocNotified){
                // Associated (vdev up, peer associated): WCL's join manager
                // continues with setCurrentBSS (EXTENDED_BSS_INFO, BSS_INFO).
                assocNotified=true;
                AirportWCL::AssociationEvent a{};memcpy(a.bssid,joinBSSID,6);a.driverStatus=kSuccess;
                sink.post(AirportWCL::AssocComplete,&a,sizeof(a));
                postLink(true,255);
                AirportWCL::FirstBeaconEvent f{};memcpy(f.peer.bssid,joinBSSID,6);
                sink.post(AirportWCL::FirstBeacon,&f,sizeof(f));firstBeaconNotified=true;
                joinPhase=JoinPhase::Handshake;
            }
            if(pending.secure){if(!serviceSupplicant())return;}
            if(status.link){
                setDataLink(true);
                lastJoinMs=uint32_t(nowMs-joinStartedMs);
                finishJoin(0,0,kSuccess);lqmReportedMs=nowMs;powerSaveSynced=false;
                if(state_==State::Connected)syncPowerSave();
            }
            return;
        }
    }
    // The association result WCL gets for a network the card cannot join.
    void refuse(uint16_t code){
        AirportWCL::AssociationEvent a{};a.status=code?code:1;memcpy(a.bssid,joinBSSID,6);a.driverStatus=kUnsupported;
        sink.post(AirportWCL::AssocComplete,&a,sizeof(a));
        finishJoin(1,a.status,kUnsupported);
        if(status.stage || status.disconnecting){state_=State::Leaving;leaveCancelMs=pollMs;}
    }
    void connect(){
        const Network *n=find(joinBSSID);
        if(!n){
            if(!joinRescanned){joinRescanned=true;joinPhase=JoinPhase::ScanFirst;return;}
            setError("bss-not-in-scan-cache");finishJoin(1,1,kNotFound);return;
        }
        AirportWCL::JoinRequest j{};memcpy(j.ssid,pending.ssid,32);j.ssidLength=pending.ssidLength;memcpy(j.bssid,joinBSSID,6);
        JoinPlan plan=pending;
        memcpy(j.key,plan.pmk,32);j.keyLength=plan.secure?32:0;j.keyCipher=6;j.lowerAuth=1;j.upperAuth=plan.secure?(1u<<3):0;
        const char *why=planJoin(j,n,plan);AthSecurity::wipe(&j,sizeof(j));
        if(why){setError(why);AthSecurity::wipe(&plan,sizeof(plan));refuse(refusalStatus(why));return;}
        if(stationMACSet && memcmp(stationMAC,status.local,6)){plan.macSet=true;memcpy(plan.mac,stationMAC,6);}
        uint8_t station[6];memcpy(station,plan.macSet?plan.mac:status.local,6);
        AthControl::Request r;buildConnectRequest(plan,r);AthControl::Response o{};
        AthSecurity::wipe(&plan,sizeof(plan));
        const int rc=core.call(r,o);
        if(rc==kNotReady || rc==kBusy){joinPhase=JoinPhase::Connect;return;}            // scan still completing: next poll
        if(rc==kExclusiveAccess){joinPhase=JoinPhase::Prepare;return;}                  // the core still holds a connection
        if(rc==kUnsupported && !strcmp(o.error,"configured-bss-not-found") && !joinRescanned){
            joinRescanned=true;joinPhase=JoinPhase::ScanFirst;return;}                   // the core's last scan missed it
        if(rc!=kSuccess){setError(o.error[0]?o.error:"connect-rejected");finishJoin(1,1,rc);return;}
        if(pending.secure){
            uint8_t nonce[32]{};
            if(!Crypto::random(nonce,32) || !supplicant.begin(pending.pmk,nonce,station,n->bssid,n->rsn,n->rsnLength)){
                AthSecurity::wipe(nonce,32);setError("supplicant-begin-failed");requestCancel();joinPhase=JoinPhase::Associate;finishJoin(1,1,kError);return;}
            AthSecurity::wipe(nonce,32);
        }
        joined=*n;joinPhase=JoinPhase::Associate;
    }
    // Returns false when the join has been failed.
    bool serviceSupplicant(){
        using S=AthSecurity::Supplicant<Crypto>;
        for(unsigned i=0;i<4 && !rekeyKeysReady && !rekeyResponsePending;++i){
            AthControl::Response rx{};
            if(call(AthControl::ReceiveEAPOL,rx)!=kSuccess)break;
            const auto result=supplicant.receive(rx.data,rx.length);
            AthSecurity::wipe(rx.data,sizeof(rx.data));
            if(result==S::Fatal){setError(supplicant.error);fail(23);return false;}
            if(result==S::Keys){
                AthControl::Request keys{};keys.length=kKeyPayloadBytes;memcpy(keys.data,supplicant.keys,keys.length);
                AthControl::Response o{};keys.operation=AthControl::InstallKeys;
                const int r=core.call(keys,o);
                AthSecurity::wipe(keys.data,sizeof(keys.data));AthSecurity::wipe(supplicant.keys,sizeof(supplicant.keys));
                if(r){setError("install-keys-rejected");fail(19);return false;}
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
            else if(r!=kBusy && r!=kNotReady){setError("rekey-rejected");fail(18);return false;}
        }
        const bool maySend=rekeyResponsePending?status.phase>=5:(!keysSubmitted || status.phase>=3);
        if(pendingLength && !rekeyKeysReady && maySend){
            const int r=call(AthControl::SendEAPOL,0,pendingFrame,uint32_t(pendingLength));
            if(!r){pendingLength=0;if(rekeyResponsePending)rekeyResponsePending=false;else if(keysSubmitted)m4Sent=true;}
            else if(r!=kBusy && r!=kNotReady){setError("eapol-send-failed");fail(15);return false;}
        }
        if(m4Sent && !authorized && !status.txBusy && status.phase==3){
            if(call(AthControl::Authorize)==kSuccess)authorized=true;
        }
        return true;
    }
    void fail(uint16_t reason){
        if(state_==State::Joining)finishJoin(1,reason,kError);
        else if(state_==State::Connected){postLink(false,reason);postDeauth(reason);requestDisconnect();wipeSecurity();state_=State::Leaving;leaveCancelMs=pollMs;}
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
            state_=(status.stage==0 && !status.disconnecting)?State::Ready:State::Leaving;leaveCancelMs=nowMs;
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
