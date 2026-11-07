#pragma once
#include "Ath10kHTC.hpp"
#include "Ath10kBSS.hpp"
#include "Ath10kConnection.hpp"
#include "Ath10kFirmwareStats.hpp"

namespace Ath10k {
// QCA6174 firmware container selects WMI TLV (op=4).
// Linux v6.12 wmi-tlv.h: event 1, service-ready fixed TLV tag 32.
static constexpr UInt32 kWMIRxBytes = 2048;
static constexpr UInt32 kWMIServiceReadyTimeoutMs = 5000;
// Linux v6.12 wmi-tlv.c uses min_len = sizeof(hal_reg_capabilities):
// nine LE32 words. Firmware may append fields inside this same TLV.
static constexpr UInt32 kWMIRegCapabilitiesMinBytes = 9 * sizeof(UInt32);
inline UInt32 wmiReadLE32(const UInt8 *p) {
    return static_cast<UInt32>(htcReadLE16(p)) |
           (static_cast<UInt32>(htcReadLE16(p + 2)) << 16);
}

// Single-owner accounting for the bounded, synchronous WMI INIT exchange.
struct HTCCreditState {
    UInt16 total = 0, available = 0, outstanding = 0, size = 0;
    UInt8 endpoint = 0xff;
    bool initialize(UInt16 count, UInt16 bytes, UInt8 ep) {
        if (!count || !bytes || ep == 0 || ep == 0xff) return false;
        total = available = count; outstanding = 0; size = bytes; endpoint = ep;
        return true;
    }
    bool reserve(UInt32 frameBytes) {
        if (!size || !frameBytes) return false;
        const UInt32 cost = frameBytes / size + ((frameBytes % size) != 0);
        if (cost > available) return false;
        available -= static_cast<UInt16>(cost);
        outstanding += static_cast<UInt16>(cost);
        return true;
    }
    bool giveBack(UInt8 ep, UInt16 count) {
        if (!size || ep != endpoint || count > outstanding) return false;
        available += count; outstanding -= count; return true;
    }
};

static constexpr UInt32 kWMIHostMemoryMax=16,kWMIHostMemoryPerRequest=2*1024*1024,kWMIHostMemoryTotal=8*1024*1024;
struct WMIHostMemoryRequest {UInt32 id=0,unitBytes=0,flags=0,units=0;};
struct WMIHostMemoryChunk {UInt32 id=0,address=0,bytes=0;};
inline UInt32 hostMemoryRequestBytes(const WMIHostMemoryRequest &r){
    if(!r.unitBytes || r.unitBytes>kWMIHostMemoryPerRequest || (r.flags&~7u))return 0;
    const UInt32 units=(r.flags&6)?34:((r.flags&1)?5:r.units); // INIT: 33 peers, 4 vdevs (+self).
    const UInt32 aligned=(r.unitBytes+3)&~3u;
    if(!units || units>kWMIHostMemoryPerRequest/aligned)return 0;
    return units*aligned;
}
struct WMIBootstrapState {
    bool attempted = false, rxOwned = false, rxPosted = false;
    bool serviceReady = false, creditsInitialized = false;
    UInt32 slot = 0, next = 0, rxBytes = 0, waitMs = 0;
    UInt8 servicePacket[kWMIRxBytes]{};
    UInt32 servicePacketBytes = 0;
    UInt32 availableCount = 0, rxCount = 0, repostCount = 0;
    UInt32 availableBits = 0, availableBitmap[4]{};
    UInt32 abi[6]{}, serviceBitmap[32]{};
    bool serviceBitmapPresent = false, memoryArrayEmpty = false;
    UInt32 eventId = 0, fwBuild = 0, abi0 = 0, rfChains = 0, memoryRequests = 0;
    UInt32 htCapability=0,vhtCapability=0,vhtMCS=0;
    WMIHostMemoryRequest hostMemory[kWMIHostMemoryMax]{};UInt32 hostMemoryCount=0;
    bool regCapsPresent=false;
    // Last-frame diagnostics, including failed parsing; not accepted RF caps.
    UInt32 regCapsBytes=0,regCapsOffset=0,regCapsCount=0;
    UInt32 eepromRD=0,eepromRDExt=0,regCap1=0,regCap2=0,wirelessModes=0;
    UInt32 low2=0,high2=0,low5=0,high5=0,hwMinPower=0,hwMaxPower=0;
    const char *status = "not-run";
    HTCCreditState credits{};
};

// Validate the HTC envelope and TLV stream, identify AVAILABLE/READY and record
// its fixed fields. Memory requests/capabilities are not acted on until WMI_INIT.
inline const char *parseWMIBootstrapEvent(const UInt8 *f, UInt32 bytes,
                                      UInt8 endpoint, WMIBootstrapState &out) {
    out.serviceReady = false;
    out.regCapsBytes=out.regCapsOffset=out.regCapsCount=0;
    if (!f || bytes < 8 || bytes > kWMIRxBytes) return "wmi-frame-size";
    if (f[0] != endpoint) return "wmi-endpoint-mismatch";
    if (f[1] & ~(kHTCTrailerPresent | kHTCMoreBlock)) return "wmi-rx-flags";
    const UInt32 payload = htcReadLE16(f + 2), declared = 8 + payload;
    if (declared > bytes || (bytes != declared && bytes != ((declared + 3u) & ~3u)))
        return "wmi-frame-length";
    const UInt32 trailer = (f[1] & kHTCTrailerPresent) ? f[4] : 0;
    if (trailer > payload || ((f[1] & kHTCTrailerPresent) && trailer < 4))
        return "wmi-trailer-length";
    const UInt32 end = declared - trailer;
    if (end < 12) return "wmi-missing-event-header";
    out.eventId = wmiReadLE32(f + 8) & 0x00ffffffu;
    if (out.eventId != 1 && out.eventId != 3) return "wmi-unexpected-event";
    const bool available = out.eventId == 3;
    bool fixed = false;
    UInt32 build = 0, abi = 0, chains = 0, mem = 0;
    UInt32 ht=0,vht=0,mcs=0,regWords[9]{},minPower=0,maxPower=0;bool regPresent=false;
    UInt32 bits = 0, bitmap[4]{};
    UInt32 abiWords[6]{}, serviceWords[32]{};
    bool bitmapPresent = false, emptyMemory = false,memoryPresent=false;
    WMIHostMemoryRequest memory[kWMIHostMemoryMax]{};UInt32 memoryCount=0,memoryBytes=0;
    for (UInt32 p = 12; p < end;) {
        if (end - p < 4) return "wmi-truncated-tlv-header";
        const UInt32 len = htcReadLE16(f + p), tag = htcReadLE16(f + p + 2);
        p += 4;
        if (len > end - p) return "wmi-truncated-tlv";
        // Linux WMI_TLV_TAG_STRUCT_SERVICE_AVAILABLE_EVENT = 0x22f;
        // observed QCA6174 packet: 20-byte TLV, 128 bits, four LE32 words.
        if (available && tag == 0x22f) {
            if (fixed || len < 4) return "wmi-service-available-fixed-size";
            bits = wmiReadLE32(f + p);
            if (!bits || bits > 128 || len < 4 + ((bits + 31) / 32) * 4)
                return "wmi-service-available-bitmap-size";
            for (UInt32 i = 0; i < (bits + 31) / 32; ++i)
                bitmap[i] = wmiReadLE32(f + p + 4 + i * 4);
            fixed = true;
        }
        if (!available && tag == 16 && !bitmapPresent) {
            if (len != 128) return "wmi-service-bitmap-size";
            for (UInt32 i = 0; i < 32; ++i) serviceWords[i] = wmiReadLE32(f + p + 4*i);
            bitmapPresent = true;
        }
        if (!available && tag == 18){
            if(memoryPresent)return "wmi-memory-array-duplicate";
            memoryPresent=true;emptyMemory=len==0;
            for(UInt32 m=0;m<len;){
                if(len-m<20 || htcReadLE16(f+p+m)!=16 || htcReadLE16(f+p+m+2)!=34 || memoryCount>=kWMIHostMemoryMax)return "wmi-memory-request-layout";
                auto &r=memory[memoryCount];const UInt8 *q=f+p+m+4;
                r.id=wmiReadLE32(q);r.unitBytes=wmiReadLE32(q+4);r.flags=wmiReadLE32(q+8);r.units=wmiReadLE32(q+12);
                for(UInt32 i=0;i<memoryCount;++i)if(memory[i].id==r.id)return "wmi-memory-request-duplicate";
                const UInt32 requested=hostMemoryRequestBytes(r);
                if(!requested || requested>kWMIHostMemoryTotal-memoryBytes)return "wmi-memory-request-size";
                memoryBytes+=requested;++memoryCount;m+=20;
            }
        }
        if(!available && tag==33){
            out.regCapsBytes=len;out.regCapsOffset=p-4;++out.regCapsCount;
            if(regPresent)return "wmi-reg-capabilities-duplicate";
            if(len<kWMIRegCapabilitiesMinBytes)return "wmi-reg-capabilities-size";
            for(UInt32 i=0;i<9;++i)regWords[i]=wmiReadLE32(f+p+4*i);
            regPresent=true;
        }
        if (!available && tag == 32) {
            if (fixed || len < 104) return "wmi-service-ready-fixed-size";
            fixed = true;
            build = wmiReadLE32(f + p); abi = wmiReadLE32(f + p + 4);
            for (UInt32 i = 0; i < 6; ++i) abiWords[i] = wmiReadLE32(f + p + 4 + 4*i);
            chains = wmiReadLE32(f + p + 36); mem = wmiReadLE32(f + p + 72);
            ht=wmiReadLE32(f+p+40);vht=wmiReadLE32(f+p+44);mcs=wmiReadLE32(f+p+48);
            minPower=wmiReadLE32(f+p+52);maxPower=wmiReadLE32(f+p+56);
        }
        p += len;
    }
    if (!fixed) return available ? "wmi-service-available-tlv-missing" : "wmi-service-ready-tlv-missing";
    if(!available && (mem!=memoryCount || mem>kWMIHostMemoryMax))return "wmi-memory-request-count";
    // Apply reports transactionally only after every record is valid.
    HTCCreditState credits = out.credits;
    for (UInt32 p = end; p < declared;) {
        if (declared - p < 4) return "wmi-truncated-trailer-header";
        const UInt32 id = f[p], len = f[p + 1]; p += 4;
        if (len > declared - p) return "wmi-truncated-trailer";
        if (id == 1) {
            if (!len || len % 4) return "wmi-credit-record-size";
            for (UInt32 i = 0; i < len; i += 4)
                if (!credits.giveBack(f[p + i], f[p + i + 1])) return "wmi-credit-report-invalid";
        }
        p += len;
    }
    out.credits = credits;
    if (available) {
        out.availableBits = bits;
        for (UInt32 i = 0; i < 4; ++i) out.availableBitmap[i] = bitmap[i];
        ++out.availableCount;
        return nullptr;
    }
    for (UInt32 i = 0; i < 6; ++i) out.abi[i] = abiWords[i];
    for (UInt32 i = 0; i < 32; ++i) out.serviceBitmap[i] = serviceWords[i];
    out.serviceBitmapPresent = bitmapPresent; out.memoryArrayEmpty = emptyMemory;
    out.fwBuild = build; out.abi0 = abi; out.rfChains = chains; out.memoryRequests = mem;
    out.regCapsPresent=regPresent;out.eepromRD=regWords[0];out.eepromRDExt=regWords[1];
    out.regCap1=regWords[2];out.regCap2=regWords[3];out.wirelessModes=regWords[4];
    out.low2=regWords[5];out.high2=regWords[6];out.low5=regWords[7];out.high5=regWords[8];
    out.hwMinPower=minPower;out.hwMaxPower=maxPower;
    out.htCapability=ht;out.vhtCapability=vht;out.vhtMCS=mcs;
    out.hostMemoryCount=memoryCount;for(UInt32 i=0;i<memoryCount;++i)out.hostMemory[i]=memory[i];
    for (UInt32 i=0;i<bytes;++i) out.servicePacket[i]=f[i];
    out.servicePacketBytes=bytes;
    out.serviceReady = true;
    return nullptr;
}

static constexpr UInt32 kWMIInitFrameBytes = 228;
static constexpr UInt32 kWMIHostABI[6] = {0x01000000, 53, 0x5f414351, 0x4c4d, 0, 0};
inline bool wmiABICompatible(const UInt32 *abi) {
    // Linux deliberately does not require firmware ABI word 1 to equal 53.
    return abi[0] == kWMIHostABI[0] && abi[2] == kWMIHostABI[2] &&
           abi[3] == kWMIHostABI[3] && abi[4] == 0 && abi[5] == 0;
}
struct WMIInitState {
    bool attempted = false, txOwned = false, submitted = false, txConsumed = false;
    bool ready = false, creditReturned = false, completed = false;
    UInt32 txSlot = 0, txNext = 0, waitMs = 0, rxCount = 0, statusCode = 0xffffffffu;
    UInt32 diagCount = 0, diagBytes = 0, lastEventId = 0;
    UInt32 statsCount=0,statsBytes=0,statsDecoded=0,statsUnsupported=0;
    UInt32 unknownCount=0,unknownId=0,keyCompleteCount=0,kickoutCount=0,temperatureCount=0;
    int32_t temperature=INT32_MIN;bool rfKill=false;UInt32 rfKillCount=0;
    UInt32 pkgIdCount = 0, pkgId = 0;
    UInt32 diagSupportedCount = 0, diagSupportedEntries = 0;
    UInt32 requestedChainMask=0;
    bool hostMemoryOwned=false;UInt32 hostMemoryCount=0,frameBytes=kWMIInitFrameBytes;
    WMIHostMemoryChunk hostMemory[kWMIHostMemoryMax]{};
    UInt8 mac[6]{};
    const char *status = "not-run";
};
inline const char *buildWMIInit(UInt8 *f, UInt32 capacity, const WMIBootstrapState &boot,
                               UInt8 endpoint,UInt32 hardwareChains=3,const WMIHostMemoryChunk *chunks=nullptr,UInt32 count=0) {
    if(!hardwareChains || hardwareChains>3)return "wmi-hardware-chains-invalid";
    if (!f || capacity < kWMIInitFrameBytes || !boot.serviceReady || !endpoint || endpoint == 255)
        return "wmi-init-prerequisite";
    if (!wmiABICompatible(boot.abi)) return "wmi-init-abi-mismatch";
    if(count>kWMIHostMemoryMax || count!=boot.memoryRequests || count!=boot.hostMemoryCount || (count && !chunks) || (!count && !boot.memoryArrayEmpty))return "wmi-init-memory-requests-unsupported";
    const UInt32 frameBytes=kWMIInitFrameBytes+20*count;
    if(capacity<frameBytes)return "wmi-init-capacity";
    for(UInt32 i=0;i<count;++i){
        if(chunks[i].id!=boot.hostMemory[i].id || !chunks[i].address || (chunks[i].address&3) ||
           chunks[i].bytes!=hostMemoryRequestBytes(boot.hostMemory[i]) || UInt64(chunks[i].address)+chunks[i].bytes>0x100000000ULL)return "wmi-init-memory-chunk-invalid";
        for(UInt32 j=0;j<i;++j)if(UInt64(chunks[i].address)<UInt64(chunks[j].address)+chunks[j].bytes &&
           UInt64(chunks[j].address)<UInt64(chunks[i].address)+chunks[i].bytes)return "wmi-init-memory-chunk-overlap";
    }
    if (!boot.serviceBitmapPresent || boot.rfChains == 0 || boot.rfChains > 3)
        return "wmi-init-resources-invalid";
    htcZero(f, frameBytes);
    f[0] = endpoint; f[1] = 1; // NEED_CREDIT_UPDATE, endpoint sequence zero.
    htcWriteLE16(f+2, frameBytes-8);
    htcWriteLE32(f+8, 1); // WMI_TLV_INIT_CMDID
    htcWriteLE16(f+12,28); htcWriteLE16(f+14,74);
    for (UInt32 i=0;i<6;++i) htcWriteLE32(f+16+4*i,kWMIHostABI[i]);
    htcWriteLE32(f+40,count);
    htcWriteLE16(f+44,176); htcWriteLE16(f+46,75);
    // Linux TLV defaults for QCA6174. No vdev/peer is actually created here.
    UInt32 cfg[44] = {4,33,0,0,2,66,16,7,7,100,100,100,40,1,4,4,4,8,
                     0,0,0,1024,32,0,0,0,0,2,1056,2,1,32,2,5,22,6,0,1,1,0,0,0,0,0};
    // Firmware reports the number of RF chains. Linux derives its operational
    // masks as (1 << num_rf_chains) - 1 then sends PDEV parameters after INIT.
    // INIT and the subsequent PDEV TX/RX commands request the same mask.
    // rfChains was bounded to 1..3 above, before this shift.
    const UInt32 chains=boot.rfChains<hardwareChains?boot.rfChains:hardwareChains;
    cfg[7]=cfg[8]=(1u<<chains)-1u;
    // Firmware service 65 is encoded using four service bits per LE32 word.
    if (boot.serviceBitmap[65/4] & (1u << (65%4))) cfg[2] = cfg[3] = 4;
    // host_capab remains zero: no management bundle completion consumer yet.
    for (UInt32 i=0;i<44;++i) htcWriteLE32(f+48+4*i,cfg[i]);
    htcWriteLE16(f+224,static_cast<UInt16>(20*count));htcWriteLE16(f+226,18);
    for(UInt32 i=0;i<count;++i){UInt8 *q=f+228+20*i;connTLV(q,76,16);
        htcWriteLE32(q+4,chunks[i].id);htcWriteLE32(q+8,chunks[i].address);htcWriteLE32(q+12,chunks[i].bytes);}
    return nullptr;
}

// One in-flight runtime command; no retransmission after a doorbell attempt.
struct WMIEchoState {
    bool attempted=false, submitted=false, txOwned=false, txConsumed=false;
    bool replyReceived=false, creditReturned=false, completed=false;
    UInt32 cookie=0xa7100033u, txSlot=0, txNext=0;
    UInt64 startedMs=0, elapsedMs=0;
    const char *status="not-run";
};
static constexpr UInt32 kWMIEchoFrameBytes=20;
inline bool buildWMIEcho(UInt8 *f, UInt32 capacity, UInt8 endpoint, UInt32 cookie) {
    if (!f || capacity<kWMIEchoFrameBytes || !endpoint || endpoint==255) return false;
    htcZero(f,kWMIEchoFrameBytes);
    f[0]=endpoint;f[1]=1;f[5]=1; // NEED_CREDIT_UPDATE; INIT used sequence 0.
    htcWriteLE16(f+2,12);
    htcWriteLE32(f+8,0x1d001); // WMI_TLV_ECHO_CMDID.
    htcWriteLE16(f+12,4);htcWriteLE16(f+14,149);
    htcWriteLE32(f+16,cookie);
    return true;
}

struct WMIVdevCreateState {
    bool attempted=false, submitted=false, txOwned=false, txConsumed=false;
    bool creditReturned=false, completed=false;
    UInt32 txSlot=0, txNext=0;
    UInt64 startedMs=0, elapsedMs=0;
    const char *status="not-run";
};
static constexpr UInt32 kWMIVdevCreateFrameBytes=36;
inline bool buildWMIVdevCreate(UInt8 *f, UInt32 capacity, UInt8 endpoint, const UInt8 *mac) {
    if (!f || capacity<kWMIVdevCreateFrameBytes || !endpoint || endpoint==255 || !mac || (mac[0]&1))
        return false;
    UInt8 nonzero=0;for(UInt32 i=0;i<6;++i)nonzero|=mac[i];
    if (!nonzero) return false;
    htcZero(f,kWMIVdevCreateFrameBytes);
    f[0]=endpoint;f[1]=1;f[5]=5; // After temporary VDEV create/delete/ECHO barrier.
    htcWriteLE16(f+2,28);htcWriteLE32(f+8,0x5001);
    htcWriteLE16(f+12,20);htcWriteLE16(f+14,86);
    // vdev_id=0, type=STA(2), subtype=NONE(0); MAC union occupies eight bytes.
    htcWriteLE32(f+20,2);
    for(UInt32 i=0;i<6;++i)f[28+i]=mac[i];
    return true;
}

// Bounded scan preparation, Linux v6.12 ath10k WMI TLV ABI.
// This command supplies a channel list; it does NOT start a scan.
struct WMIScanChannelsState {
    bool attempted=false, submitted=false, txOwned=false, txConsumed=false;
    bool creditReturned=false, completed=false;
    UInt32 txSlot=0, txNext=0;
    UInt64 startedMs=0, elapsedMs=0;
    const char *status="not-run";
};
// TLV PDEV_SET_PARAM: pdev_id reserved zero, param 1=TX / 2=RX.
static constexpr UInt32 kWMIRadioFrameBytes=28;
inline bool buildWMIRadio(UInt8 *f, UInt32 capacity, UInt8 endpoint, UInt32 param, UInt32 mask) {
    if (!f || capacity<kWMIRadioFrameBytes || !endpoint || endpoint==255 ||
        (param<1 || param>4) || ((param<=2)?(mask!=1 && mask!=3 && mask!=7):(!mask || mask>20))) return false;
    htcZero(f,kWMIRadioFrameBytes);
    f[0]=endpoint;f[1]=1;f[5]=static_cast<UInt8>(param+5);
    htcWriteLE16(f+2,20);htcWriteLE32(f+8,0x4003);
    htcWriteLE16(f+12,12);htcWriteLE16(f+14,82);
    htcWriteLE32(f+20,param);htcWriteLE32(f+24,mask);
    return true;
}
static constexpr UInt32 kWMIRegdomainFrameBytes=40;
inline bool buildWMIRegdomain(UInt8 *f,UInt32 capacity,UInt8 endpoint,const RegulatoryPolicy &p){
    if(!f || capacity<40 || !endpoint || endpoint==255 || !p.firmwareCapsValid)return false;
    htcZero(f,40);f[0]=endpoint;f[1]=1;f[5]=8;
    htcWriteLE16(f+2,32);htcWriteLE32(f+8,0x4001);
    htcWriteLE16(f+12,24);htcWriteLE16(f+14,81);
    htcWriteLE32(f+20,p.domain);htcWriteLE32(f+24,p.domain);htcWriteLE32(f+28,p.domain);
    htcWriteLE32(f+32,p.ctl2);htcWriteLE32(f+36,p.ctl5);return true;
}
static constexpr UInt32 kWMIScanChannelsFrameBytes=24+28*kWMIScanChannelCount;
inline bool buildWMIScanChannels(UInt8 *f, UInt32 capacity, UInt8 endpoint,const RegulatoryPolicy *policy=nullptr) {
    const UInt32 count=regulatoryScanCount(policy),frameBytes=24+28*count;
    if (!f || !count || capacity<frameBytes || !endpoint || endpoint==255) return false;
    htcZero(f,frameBytes);
    f[0]=endpoint;f[1]=1;f[5]=8;
    htcWriteLE16(f+2,frameBytes-8);htcWriteLE32(f+8,0x3003);
    htcWriteLE16(f+12,4);htcWriteLE16(f+14,79);
    htcWriteLE32(f+16,count);
    htcWriteLE16(f+20,28*count);htcWriteLE16(f+22,18);
    UInt32 slot=0;
    for(UInt32 i=0;i<kWMIScanChannelCount;++i){
        if(policy && !regulatoryScanChannel(*policy,wmiScanChannel(i)))continue;
        UInt8 *ch=f+24+28*slot++;
        htcWriteLE16(ch,24);htcWriteLE16(ch+2,80);
        htcWriteLE32(ch+4,wmiScanFrequency(i));htcWriteLE32(ch+8,wmiScanFrequency(i));
        htcWriteLE32(ch+16,wmiScanFrequency(i)<3000?0x81:(channelNeedsDFS(wmiScanChannel(i))?0x480:0x80)); // PASSIVE | MODE_11G(1) / MODE_11A(0).
        // wmi_channel reginfo0: min/max/reg/class; reginfo1: antenna/max_tx/reserved.
        writeChannelPower(ch+20,ch+24,policy?regulatoryPower(*policy,wmiScanChannel(i),wmiScanChannel(i),20,false):channelPowerHalfDbm(wmiScanChannel(i)));
        // min, antenna gain, regulatory class and reserved bytes remain zero.
    }
    return true;
}

// Raw channel counters are firmware snapshots, not packet or utilization rates.
struct WMIChannelSample {
    UInt32 count=0, errors=0, flags=0, noise=0, clear=0, cycles=0;
    UInt32 rxFrames=0, errorCode=0;
    bool rxFramesPresent=false;
};
struct WMIScanState {
    UInt32 generation=0,id=0xa036;bool cancelRequested=false,stopSubmitted=false,stopConsumed=false;
    UInt32 stopSlot=0,stopNext=0;
    bool attempted=false, submitted=false, txOwned=false, txConsumed=false;
    bool creditReturned=false, started=false, terminal=false, failed=false, completed=false;
    UInt32 txSlot=0, txNext=0, eventCount=0, lastType=0, reason=0, frequency=0;
    UInt32 mgmtCount=0, mgmtBytes=0; UInt64 visitedMask=0;
    UInt32 channelInfoCount=0, channelInfoErrors=0; UInt64 channelInfoMask=0;
    WMIChannelSample channels[kWMIScanChannelCount]{};
    WMIChannelSample firstChannels[kWMIScanChannelCount]{};
    UInt64 startedMs=0, elapsedMs=0;
    const char *status="not-run";
};
static constexpr UInt32 kWMIScanFrameBytes=140+4*kWMIScanChannelCount;
static constexpr UInt32 kWMIScanId=0xa036, kWMIScanRequestId=0xa001;
inline bool buildWMIStopScan(UInt8 *f,UInt32 capacity,UInt8 endpoint,UInt32 id,UInt8 sequence){
    if(!f || capacity<32 || !endpoint || endpoint==255 || (id&0xf000)!=0xa000)return false;
    htcZero(f,32);f[0]=endpoint;f[1]=1;f[5]=sequence;
    htcWriteLE16(f+2,24);htcWriteLE32(f+8,0x3002);htcWriteLE16(f+12,16);htcWriteLE16(f+14,78);
    htcWriteLE32(f+16,kWMIScanRequestId);htcWriteLE32(f+20,id);
    // wmi_stop_scan_cmd order is requestor, scan_id, req_type, vdev_id.
    return true;
}
inline bool buildWMIScan(UInt8 *f, UInt32 capacity, UInt8 endpoint,UInt32 id=kWMIScanId,bool connected=false,const RegulatoryPolicy *policy=nullptr) {
    const UInt32 count=regulatoryScanCount(policy),frameBytes=140+4*count;
    if (!f || !count || capacity<frameBytes || !endpoint || endpoint==255) return false;
    htcZero(f,frameBytes);
    f[0]=endpoint;f[1]=1;f[5]=9;
    htcWriteLE16(f+2,frameBytes-8);htcWriteLE32(f+8,0x3001);
    htcWriteLE16(f+12,100);htcWriteLE16(f+14,77);
    // Fixed common: IDs, VDEV, priority, event mask, dwell/rest/probe times,
    // bounded firmware scan time, PASSIVE | CHAN_STAT_EVENT plus TLV filter inversion.
    const UInt32 common[15]={id,kWMIScanRequestId,0,1,0x1ff,50,connected?100u:kWMIScanDwellMs,connected?150u:50u,500,0,0,0,kWMIScanFirmwareMs,5,0x31};
    for(UInt32 i=0;i<15;++i)htcWriteLE32(f+16+4*i,common[i]);
    htcWriteLE32(f+80,count); // num_channels; burst_duration at 76 remains zero.
    htcWriteLE32(f+84,1); // num_bssids; num_ssids, IE length, probes and MAC/mask zero.
    htcWriteLE16(f+116,4*count);htcWriteLE16(f+118,16);
    UInt32 slot=0;for(UInt32 i=0;i<kWMIScanChannelCount;++i)
        if(!policy || regulatoryScanChannel(*policy,wmiScanChannel(i)))htcWriteLE32(f+120+4*slot++,wmiScanFrequency(i));
    const UInt32 tail=120+4*count;
    htcWriteLE16(f+tail+2,19); // empty SSID ARRAY_FIXED_STRUCT
    htcWriteLE16(f+tail+4,8);htcWriteLE16(f+tail+6,19);
    for(UInt32 i=0;i<6;++i)f[tail+8+i]=255; // broadcast BSSID
    htcWriteLE16(f+tail+18,17); // empty IE ARRAY_BYTE
    return true;
}

struct WMIRuntimeState {
    UInt64 lastMaintenanceMs=0;
    UInt32 pollIntervalMs=10;
    bool attempted = false, active = false;
    UInt32 rawCount = 0, rawCE1 = 0, rawCE2 = 0, rawMgmt = 0, rejected = 0;
    UInt32 polls = 0, rxCount = 0, budgetHits = 0, lastEventId = 0, lastBytes = 0;
    const char *status = "not-run";
};

// Accept READY on the negotiated WMI endpoint and trailer-only reports on
// endpoint 0 or WMI. Credits are committed only after the entire frame validates.
inline const char *parseWMIInitResponse(const UInt8 *f, UInt32 bytes, UInt8 endpoint,
                                      HTCCreditState &credits, WMIInitState &out, WMIEchoState *echo=nullptr, WMIScanState *scan=nullptr, WMIBSSCache *bss=nullptr, bool allowVdevDelete=false, ConnectionState *connection=nullptr) {
    if (!f || bytes < 8 || bytes > kWMIRxBytes) return "wmi-init-rx-size";
    if (f[0] != 0 && f[0] != endpoint) return "wmi-init-rx-endpoint";
    if (f[1] & ~(kHTCTrailerPresent | kHTCMoreBlock)) return "wmi-init-rx-flags";
    const UInt32 payload=htcReadLE16(f+2), declared=8+payload;
    if (declared > bytes || (bytes!=declared && bytes!=((declared+3u)&~3u))) return "wmi-init-rx-length";
    const UInt32 trailer=(f[1]&kHTCTrailerPresent)?f[4]:0;
    if (trailer>payload || ((f[1]&kHTCTrailerPresent)&&trailer<4)) return "wmi-init-trailer-size";
    const UInt32 end=declared-trailer;
    bool pauseEvent=false,pauseFixed=false;UInt32 pauseId=0,pauseAction=0,pauseMap=0;
    const UInt8 *statsHeader=nullptr,*statsData=nullptr;UInt32 statsHeaderBytes=0;
    bool peerStateEvent=false,peerStateFixed=false;
    bool statsEvent=false,statsFixed=false,statsArray=false;UInt32 statsBytes=0;
    bool peerDeleteEvent=false,peerDeleteFixed=false;
    bool stopEvent=false,stopFixed=false;
    bool roamEvent=false,roamFixed=false;UInt32 roamReason=0;
    bool startEvent=false,startFixed=false,restartEvent=false;UInt32 startStatus=0;
    bool ready=false, diagnostic=false, diagArray=false, package=false, packageFixed=false;
    UInt32 packageId=0;
    bool echoEvent=false, echoFixed=false, deleteEvent=false,rollbackEcho=false,rollbackStart=false;
    bool informational=false,kickout=false,kickoutFixed=false,temperatureEvent=false,temperatureFixed=false,rfKillEvent=false,rfKillFixed=false;
    UInt8 kickedMAC[6]{};UInt32 temperatureValue=0,radioState=0;
    bool channelInfo=false, channelInfoFixed=false;
    bool scanEvent=false, scanFixed=false, scanStale=false, mgmt=false, mgmtFixed=false, mgmtArray=false;
    UInt32 scanType=0, scanReason=0, scanFreq=0, mgmtLen=0, mgmtArrayLen=0;
    const UInt8 *mgmtData=nullptr;
    UInt32 mgmtChannel=0,mgmtSnr=0,mgmtStatus=0;
    WMIScanState scanNext{};if(scan)scanNext=*scan;
    bool supported=false, supportedFixed=false, supportedArray=false;
    UInt32 supportedEntries=0, supportedArrayEntries=0;
    UInt32 diagBytes=0, eventId=0;
    UInt32 status=0xffffffffu; UInt8 mac[6]{};
    if (end>8) {
        if (f[0]!=endpoint || end<12) return "wmi-init-unexpected-event";
        eventId=wmiReadLE32(f+8)&0xffffffu;
        pauseEvent=eventId==0x4005; // WMI_TLV_TX_PAUSE_EVENTID
        kickout=eventId==0x6001;temperatureEvent=eventId==0x4008;rfKillEvent=eventId==0x1e004;
        roamEvent=eventId==0xb001;
        startEvent=eventId==0x5001;
        statsEvent=eventId==0x1d004; // Linux WMI_TLV_UPDATE_STATS_EVENTID; informational, not STOPPED.
        stopEvent=eventId==0x5002;
        peerDeleteEvent=eventId==0x6007;
        if(peerDeleteEvent && (!connection || connection->disconnect<DisconnectStage::DeletePeer))return "peer-delete-unsolicited";
        if(stopEvent && (!connection || connection->disconnect!=DisconnectStage::Stop || !connection->submitted || connection->stopReply))return "vdev-stop-unsolicited";
        peerStateEvent=eventId==0x6005; // WMI_TLV_PEER_STATE_EVENTID; informational only.
        if(startEvent){
            restartEvent=connection && connection->csa.phase==ChannelSwitchPhase::Restart && connection->csa.submitted && !connection->csa.restartReply;
            rollbackStart=connection && connection->rollback.active && connection->rollback.startPending;
            if(!restartEvent && !rollbackStart && (!connection || connection->stage!=LinkStage::Start || !connection->submitted || connection->startReply))return "vdev-start-unsolicited";
        }
        diagnostic=eventId==0x1d011 || eventId==0x5003; // WMI_TLV_DIAG_EVENTID, not firmware READY.
        package=eventId==0x1d019; // WMI_PKGID_EVENTID; informational chip package ID.
        supported=eventId==0x16006; // WMI_DIAG_EVENT_LOG_SUPPORTED_EVENTID.
        echoEvent=eventId==0x1d001;
        // An old, exactly correlated ECHO may arrive after cancellation. It
        // cannot acknowledge the new teardown ECHO or release command DMA.
        rollbackEcho=echoEvent && connection && connection->rollback.active && connection->rollback.echoPending &&
            end>=20 && htcReadLE16(f+12)==4 && htcReadLE16(f+14)==54 && wmiReadLE32(f+16)==connection->rollback.echoCookie;
        deleteEvent=eventId==0x5006 && allowVdevDelete; // Linux treats the bounded payload as opaque.
        if(eventId==0x5006 && !allowVdevDelete)return "vdev-delete-unsolicited";
        scanEvent=eventId==0x3001;mgmt=eventId==0x7001;channelInfo=eventId==0x4002;
        if ((scanEvent || mgmt || channelInfo) && (!scan || (!scan->submitted && !scan->generation))) return "scan-unsolicited-event";
        if (scanEvent && scan->terminal) return "scan-event-after-terminal";
        if (echoEvent && !rollbackEcho && (!echo || !echo->submitted || echo->replyReceived || echo->completed))
            return "wmi-echo-unexpected-reply";
        informational=eventId!=2 && !diagnostic && !package && !supported && !echoEvent && !scanEvent && !mgmt && !channelInfo && !deleteEvent && !startEvent && !stopEvent && !statsEvent && !peerDeleteEvent && !peerStateEvent && !pauseEvent && !roamEvent && !kickout && !temperatureEvent && !rfKillEvent;
        // A new SERVICE_READY means a changed boot epoch; it is not opaque.
        if(!eventId || eventId==1)return "wmi-init-unexpected-event";
        for (UInt32 p=12;p<end;) {
            if (end-p<4) return "wmi-init-tlv-header";
            const UInt32 len=htcReadLE16(f+p),tag=htcReadLE16(f+p+2);p+=4;
            if (len>end-p) return "wmi-init-tlv-size";
            if(kickout){
                if(tag!=43 || len<8 || len%4 || kickoutFixed)return "peer-kickout-layout";
                kickoutFixed=true;for(UInt32 i=0;i<6;++i)kickedMAC[i]=f[p+i];
            }
            if(temperatureEvent){
                if(tag!=343 || len<8 || len%4 || temperatureFixed || wmiReadLE32(f+p+4))return "temperature-layout";
                temperatureValue=wmiReadLE32(f+p);const int32_t value=static_cast<int32_t>(temperatureValue);
                if(value<-40 || value>200)return "temperature-value";
                temperatureFixed=true;
            }
            if(rfKillEvent){
                if(tag!=231 || len<12 || len%4 || rfKillFixed)return "rfkill-layout";
                radioState=wmiReadLE32(f+p+8);if(radioState<1 || radioState>2)return "rfkill-state";rfKillFixed=true;
            }
            if(roamEvent){
                if(tag!=48 || len<12 || len%4 || roamFixed || wmiReadLE32(f+p))return "roam-event-layout";
                roamFixed=true;roamReason=wmiReadLE32(f+p+4);
            }
            if(pauseEvent){
                if(tag!=230 || len<20 || len%4 || pauseFixed)return "tx-pause-layout";
                pauseId=wmiReadLE32(f+p);pauseAction=wmiReadLE32(f+p+4);pauseMap=wmiReadLE32(f+p+8);
                if(pauseAction>1)return "tx-pause-action";
                pauseFixed=true;
            }
            if(peerStateEvent){
                // Captured RM.4.4.1: tag 300, vdev, padded MAC, opaque state.
                // This notification never authorizes TX, DMA release or link-up.
                if(tag!=300 || len<16 || len%4 || peerStateFixed)return "peer-state-layout";
                if(!connection || wmiReadLE32(f+p)!=0 || !sameMAC(f+p+4,connection->bssid))return "peer-state-peer";
                peerStateFixed=true;
            }
            if(statsEvent){
                // Linux wmi_tlv_stats_ev has a 40-byte common header. RM.4.4.1
                // emitted 52 bytes in the capture; accept bounded extensions.
                // An unknown ABI remains opaque; the optional decoder below
                // accepts only whole records of the pinned, bounded layout.
                if(tag==70){
                    if(len<40 || len%4 || statsFixed)return "wmi-stats-fixed-invalid";
                    statsFixed=true;statsHeader=f+p;statsHeaderBytes=len;
                }else if(tag==17){
                    if(statsArray)return "wmi-stats-array-duplicate";
                    statsArray=true;statsBytes=len;statsData=f+p;
                }
            }
            if(peerDeleteEvent){
                if(tag!=451 || len!=12 || peerDeleteFixed || wmiReadLE32(f+p) || !sameMAC(f+p+4,connection->bssid))return "peer-delete-layout";
                peerDeleteFixed=true;
            }
            if(stopEvent){
                if(tag!=41 || len!=4 || stopFixed || wmiReadLE32(f+p))return "vdev-stop-layout";
                stopFixed=true;
            }
            if(startEvent){
                // Linux wmi-tlv policy specifies a 16-byte minimum. QCA6174
                // WLAN.RM.4.4.1 returns 40 bytes; only the common prefix is
                // interpreted. The enclosing TLV bounds cover the extension.
                if(tag!=40 || len<16 || (len%4) || startFixed)return "vdev-start-response-layout";
                if(wmiReadLE32(f+p) || wmiReadLE32(f+p+4) || wmiReadLE32(f+p+8)!=(restartEvent?1u:0u))return "vdev-start-response-id";
                startStatus=wmiReadLE32(f+p+12);startFixed=true;
            }
            // Diagnostic payload is opaque, but its TLV must be bounded.
            if (diagnostic && tag==17) {
                if (diagArray) return "wmi-diag-duplicate-array";
                diagArray=true;diagBytes=len;
            }
            if (package) {
                // WMI_TAG_PKGID_EVENT: exactly one u32 after its TLV header.
                if (tag!=0x25d || len!=4 || packageFixed) return "wmi-pkgid-fixed-invalid";
                packageFixed=true;packageId=wmiReadLE32(f+p);
            }
            if (supported) {
                if (tag==0x1a2) {
                    if (supportedFixed || len!=4) return "wmi-diag-supported-fixed";
                    supportedFixed=true;supportedEntries=wmiReadLE32(f+p);
                    if (supportedEntries>kWMIRxBytes/4) return "wmi-diag-supported-count";
                } else if (tag==16) { // ARRAY_UINT32, one packed descriptor per entry.
                    if (supportedArray || len%4) return "wmi-diag-supported-array";
                    supportedArray=true;supportedArrayEntries=len/4;
                } else return "wmi-diag-supported-tag";
            }
            if (channelInfo) {
                // Common prefix is 24 bytes. Extra fields vary by firmware;
                // access rx_frame_count only when its full word is present.
                if (tag!=38 || len<24 || (len%4) || channelInfoFixed) return "scan-channel-info-fixed";
                channelInfoFixed=true;
                const UInt32 frequency=wmiReadLE32(f+p+4);
                if (!wmiScanFrequencyAllowed(frequency)) return "scan-channel-info-frequency";
                const UInt32 index=wmiScanFrequencyIndex(frequency);
                auto &sample=scanNext.channels[index];
                ++scanNext.channelInfoCount;++sample.count;
                scanNext.channelInfoMask|=1ULL<<index;
                const UInt32 errorCode=wmiReadLE32(f+p);sample.errorCode=errorCode;
                if (errorCode) {++scanNext.channelInfoErrors;++sample.errors;}
                sample.flags=wmiReadLE32(f+p+8);sample.noise=wmiReadLE32(f+p+12);
                sample.clear=wmiReadLE32(f+p+16);sample.cycles=wmiReadLE32(f+p+20);
                sample.rxFramesPresent=len>=36;
                sample.rxFrames=sample.rxFramesPresent?wmiReadLE32(f+p+32):0;
                if (sample.count==1) scanNext.firstChannels[index]=sample;
            }
            if (scanEvent) {
                // Legacy six-u32 event (24 bytes), or Qualcomm extension with
                // trailing TSF timestamp (28 bytes), observed on QCA6174.
                // The optional timestamp is bounded by the TLV and not used.
                if (tag!=36 || (len!=24 && len!=28) || scanFixed) return "scan-event-fixed";
                scanFixed=true;
                scanType=wmiReadLE32(f+p);scanReason=wmiReadLE32(f+p+4);scanFreq=wmiReadLE32(f+p+8);
                if (wmiReadLE32(f+p+12)!=kWMIScanRequestId || wmiReadLE32(f+p+16)!=scan->id ||
                    wmiReadLE32(f+p+20)!=0){
                    if(!scan->generation || wmiReadLE32(f+p+12)!=kWMIScanRequestId ||
                       wmiReadLE32(f+p+20)!=0 || (wmiReadLE32(f+p+16)&0xf000)!=0xa000)return "scan-event-id-mismatch";
                    scanStale=true;
                }
                if(!scanStale && !scan->submitted)return "scan-unsolicited-event";
                if (!scanType || (scanType&(scanType-1)) || scanType>256)
                    return "scan-event-value";
                if (!scanStale && scanType==1 && scan->started) return "scan-duplicate-start";
                if (scanType!=1 && scanType!=2 && scanType!=16 && scanType!=32 && scanType!=64 && !scan->started && !scanStale)
                    return "scan-event-before-start";
                if ((scanType==8 || scanType==256) && !wmiScanFrequencyAllowed(scanFreq)) return "scan-unexpected-channel";
            }
            if (mgmt) {
                if (tag==44) {
                    if (mgmtFixed || len<40) return "scan-mgmt-header";
                    mgmtFixed=true;mgmtLen=wmiReadLE32(f+p+16);
                    mgmtChannel=wmiReadLE32(f+p);mgmtSnr=wmiReadLE32(f+p+4);mgmtStatus=wmiReadLE32(f+p+20);
                } else if (tag==17) {
                    if (mgmtArray) return "scan-mgmt-duplicate-array";
                    mgmtArray=true;mgmtArrayLen=len;mgmtData=f+p;
                } else return "scan-mgmt-tag";
            }
            if (echoEvent) {
                if (tag!=54 || len!=4 || echoFixed) return "wmi-echo-fixed-invalid";
                if (wmiReadLE32(f+p)!=(rollbackEcho?connection->rollback.echoCookie:echo->cookie)) return "wmi-echo-cookie-mismatch";
                echoFixed=true;
            }
            if (eventId==2 && tag==35) {
                if (ready || len<36) return "wmi-ready-fixed-size";
                UInt32 abi[6]; for (UInt32 i=0;i<6;++i) abi[i]=wmiReadLE32(f+p+4*i);
                if (!wmiABICompatible(abi)) return "wmi-ready-abi-mismatch";
                status=wmiReadLE32(f+p+32);
                for (UInt32 i=0;i<6;++i) mac[i]=f[p+24+i];
                ready=true;
            }
            p+=len;
        }
        if (scanEvent) {
            if (!scanFixed) return "scan-event-missing";
            if(!scanStale){++scanNext.eventCount;scanNext.lastType=scanType;scanNext.reason=scanReason;scanNext.frequency=scanFreq;
            // reason is a completion status, not a validity check for STARTED
            // or channel notifications (the captured STARTED has reason=7).
            // Preserve the raw value; any nonzero completion remains a failure.
            if (scanType==8) scanNext.visitedMask|=1ULL<<wmiScanFrequencyIndex(scanFreq);
            if (scanType==1) scanNext.started=true;
            if (scanType==2 || scanType==16 || scanType==32 || scanType==64) {
                scanNext.terminal=true;
                scanNext.failed=scanType!=2 || ((!scanNext.cancelRequested || scanReason!=1) && scanReason!=0) || (!scanNext.started && !scanNext.cancelRequested);
            }}
        } else if (channelInfo) {
            if (!channelInfoFixed) return "scan-channel-info-missing";
        } else if (mgmt) {
            if (!mgmtFixed || !mgmtArray || mgmtLen>mgmtArrayLen)
                return "scan-mgmt-length";
            ++scanNext.mgmtCount;scanNext.mgmtBytes+=mgmtLen;
        } else if(kickout){if(!kickoutFixed)return "peer-kickout-missing";
        } else if(temperatureEvent){if(!temperatureFixed)return "temperature-missing";
        } else if(rfKillEvent){if(!rfKillFixed)return "rfkill-missing";
        } else if(informational){
            // Opaque, bounded TLVs may return validated HTC credits, but never
            // acknowledge a key/command, authorize a port or release DMA.
        } else if(roamEvent){
            if(!roamFixed)return "roam-event-missing";
        } else if(pauseEvent){
            if(!pauseFixed)return "tx-pause-missing";
        } else if(peerStateEvent){
            if(!peerStateFixed)return "peer-state-missing";
        } else if(statsEvent){
            if(!statsFixed || !statsArray)return "wmi-stats-missing";
        } else if(peerDeleteEvent){
            if(!peerDeleteFixed)return "peer-delete-missing";
        } else if(stopEvent){
            if(!stopFixed)return "vdev-stop-missing";
        } else if(startEvent){
            if(!startFixed)return "vdev-start-response-missing";
        } else if (deleteEvent) {
            // Informational only; the separate ECHO remains the required barrier.
        } else if (echoEvent) {
            if (!echoFixed) return "wmi-echo-fixed-missing";
        } else if (supported) {
            if (!supportedFixed || !supportedArray) return "wmi-diag-supported-missing";
            if (supportedEntries!=supportedArrayEntries) return "wmi-diag-supported-count";
        } else if (package) {
            if (!packageFixed) return "wmi-pkgid-fixed-missing";
        } else if (diagnostic) {
            if (!diagArray && eventId!=0x5003) return "wmi-diag-array-missing";
        } else {
            if (!ready) return "wmi-ready-tlv-missing";
            out.statusCode=status;
            if (status) return "wmi-ready-firmware-rejected";
        }
    } else if (!trailer) return "wmi-init-empty-frame";
    HTCCreditState next=credits;
    for (UInt32 p=end;p<declared;) {
        if (declared-p<4) return "wmi-init-trailer-header";
        const UInt32 id=f[p],len=f[p+1];p+=4;
        if (len>declared-p) return "wmi-init-trailer-record";
        if (id==1) {
            if (!len || len%4) return "wmi-init-credit-size";
            for (UInt32 i=0;i<len;i+=4)
                if (!next.giveBack(f[p+i],f[p+i+1])) return "wmi-init-credit-invalid";
        }
        p+=len;
    }
    credits=next;
    if(informational){++out.unknownCount;out.unknownId=eventId;}
    if(eventId==0x5003)++out.keyCompleteCount; // HTT SECURITY_IND remains the key acknowledgement.
    if(temperatureEvent){++out.temperatureCount;out.temperature=static_cast<int32_t>(temperatureValue);}
    if(rfKillEvent){++out.rfKillCount;out.rfKill=radioState==1;
        if(out.rfKill && connection){connection->radio.powerBlocked=true;connection->cancelRequested=true;connectionFailure(*connection,"radio-hardware-blocked");}}
    if(kickout){++out.kickoutCount;
        if(connection && sameMAC(kickedMAC,connection->bssid) && connection->stage==LinkStage::Connected){
            connection->cancelRequested=true;connection->radio.powerBlocked=true;connectionFailure(*connection,"ap-peer-kickout");}}
    if(roamEvent && connection && roamReason==2 && connection->stage==LinkStage::Connected && !connection->beaconWarning){
        connection->beaconWarning=true;connection->beaconWarningSinceMs=0;
    }
    if(pauseEvent && connection){
        ++connection->txPauseEvents;
        // Same vdev-scoped reasons as Linux ath10k_wmi_tlv_event_tx_pause.
        if((pauseMap&1) && (pauseId==1 || pauseId==4 || pauseId==5 || pauseId==7 || pauseId==8)){
            UInt32 before=connection->txPauseMask;
            if(!pauseAction)connection->txPauseMask|=1u<<pauseId;
            else connection->txPauseMask&=~(1u<<pauseId);
            if(before && !connection->txPauseMask)connection->txWakePending=true;
        }
    }
    if(statsEvent){++out.statsCount;out.statsBytes+=statsBytes;
        if(connection && connection->stage==LinkStage::Connected && scan && scan->completed && connection->csa.phase==ChannelSwitchPhase::Idle){
            if(decodeFirmwareStats(connection->fwStats,statsHeader,statsHeaderBytes,statsData,statsBytes,connection->bssid))++out.statsDecoded;
            else ++out.statsUnsupported;}}
    if(peerDeleteEvent)connection->deleteReply=true;
    if(stopEvent)connection->stopReply=true;
    if(startEvent && restartEvent){connection->csa.restartReply=true;connection->csa.failed=startStatus!=0;
        if(startStatus)connection->csa.status="vdev-restart-rejected";
    }
    if(startEvent && !restartEvent){connection->startReply=true;connection->vdevStarted=!startStatus;
        if(rollbackStart)connection->rollback.startPending=false;
        if(startStatus){connection->associationStatus=startStatus;
            connectionFailure(*connection,"vdev-start-rejected");
            if(!rollbackStart)connection->stage=LinkStage::Failed;}}
    if(mgmt && connection){
        receiveStationManagement(*connection,mgmtData,mgmtLen,mgmtStatus,mgmtChannel);
        if(mgmtLen>=24 && sameMAC(mgmtData+16,connection->bssid) && mgmtSnr<=95)
            connection->signalDbm=static_cast<int32_t>(mgmtSnr)-95;
    }
    if(scanEvent || mgmt || channelInfo)*scan=scanNext;
    if(mgmt && bss)recordWMIBSS(*bss,mgmtData,mgmtLen,mgmtChannel,mgmtSnr,mgmtStatus);
    if (echoEvent && !rollbackEcho) echo->replyReceived=true;
    if (rollbackEcho) connection->rollback.echoPending=false;
    out.lastEventId=eventId;
    if (supported) { ++out.diagSupportedCount; out.diagSupportedEntries=supportedEntries; }
    if (package) { ++out.pkgIdCount; out.pkgId=packageId; }
    if (diagnostic) { ++out.diagCount; out.diagBytes+=diagBytes; }
    if (ready) { out.ready=true; for (UInt32 i=0;i<6;++i) out.mac[i]=mac[i]; }
    out.creditReturned=credits.outstanding==0;
    return nullptr;
}
} // namespace Ath10k
