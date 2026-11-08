// SPDX-License-Identifier: GPL-3.0-or-later
// AWDL radio binding of the ath10k core (docs/WIFI-AWDL.md section 5a): a monitor
// vdev (id 1) that the AWDL engine in the front end tunes per channel slot, with
// action-frame and data-frame TX through HTT and RX filtering of AWDL frames.
//
// Everything here is pure logic over byte buffers: the firmware commands it builds
// are checked byte for byte by native/tests/test_native_awdl_radio.cpp, and the state
// machine is driven there against a firmware model. Compiled only when
// AIRPORT_ATH10K_AWDL is 1; with the gate off no byte of this file reaches a kext.
//
// Two placements (Owly's, https://github.com/t4t5/owly, MIT): "exclusive" (fixed), the original
// mode, with the station vdev idle and the monitor vdev on a social channel; and "shared", with
// the station associated on a social channel, the monitor vdev on that same channel using the
// station's channel definition (Linux ath10k_monitor_vdev_start does the same) and the station's
// MAC, so the station's receive path ACKs unicast addressed to it.
//
// What this does not establish (only a real run can): that WLAN.RM.4.4.1 accepts a monitor vdev
// next to a joined station on the same channel, that frames injected on it are radiated, and that
// it passes other-BSS AWDL frames to the host. Linux reaches the air through the station vdev for
// frames carrying the station's MAC; this driver keeps to the monitor vdev (docs/WIFI-AWDL.md).
#pragma once
#ifndef AIRPORT_ATH10K_AWDL
#define AIRPORT_ATH10K_AWDL 0
#endif
#if AIRPORT_ATH10K_AWDL
namespace Ath10k {
// vdev 0 is the station vdev; the firmware vdev limit is 4 for this build.
constexpr UInt8 kAwdlVdevId=1;
constexpr UInt32 kAwdlVdevTypeMonitor=4,kAwdlRxSlots=16,kAwdlTxSlots=8,kAwdlFrameMax=2048;
constexpr UInt32 kAwdlWMIFrameBytes=256,kAwdlTimeoutMs=3000;
inline constexpr UInt8 kAwdlBssidBytes[6]={0x00,0x25,0x00,0xff,0x94,0x73};
inline constexpr UInt8 kAwdlOuiBytes[3]={0x00,0x17,0xf2};
// One-owner radio states. Create..Up bring it up, Down..RestartUp retune it while it
// stays created, Down..Delete remove it. Failed needs a firmware reload (or stop).
enum class AwdlStep:UInt8{Off,Create,Start,Up,Running,Down,Restart,RestartUp,Stop,Delete,Failed};
enum class AwdlRx:UInt8{Action=1,Data=2};
struct AwdlRxEntry{UInt16 length=0;UInt8 kind=0,channel=0;SInt8 rssi=0;UInt64 timeUs=0;UInt8 data[kAwdlFrameMax];};
struct AwdlTxEntry{UInt16 length=0;bool action=false;UInt8 data[kAwdlFrameMax];};
struct AwdlCounters{UInt32 rxAction=0,rxData=0,rxDropped=0,txAction=0,txData=0,txDropped=0,txFailed=0,channelSwitches=0,failures=0;};
struct AwdlRadio {
    AwdlStep step=AwdlStep::Off;
    bool wmiSubmitted=false,eventPending=false;   // current command sent / reply outstanding
    bool startRequested=false,stopRequested=false;
    UInt8 mac[6]{};UInt8 channel=0,channelTarget=0;UInt8 powerHalfDbm=0;
    // Shared placement: the station's channel definition, copied at start (center channel, WMI phymode word).
    bool shared=false;UInt8 sharedCenter=0;UInt32 sharedPhy=0;
    UInt64 sinceMs=0;const char *error=nullptr;
    AwdlRxEntry rx[kAwdlRxSlots];UInt32 rxHead=0,rxCount=0;
    AwdlTxEntry tx[kAwdlTxSlots];UInt32 txHead=0,txCount=0;
    UInt16 txLastLength=0;bool txLastAction=false,txOwned=false;
    UInt32 deletesPending=0;
    AwdlCounters counters{};
    bool active() const {return step!=AwdlStep::Off && step!=AwdlStep::Failed;}
    bool running() const {return step==AwdlStep::Running;}
    void reset(){step=AwdlStep::Off;wmiSubmitted=eventPending=startRequested=stopRequested=txOwned=shared=false;sharedCenter=0;sharedPhy=0;deletesPending=0;error=nullptr;channel=channelTarget=0;
        rxHead=rxCount=0;txHead=txCount=0;}
};

inline UInt8 awdlChannelPhyMode(UInt8 channel){return channel>14?4:5;} // WMI MODE_11NA_HT20 / MODE_11NG_HT20
inline UInt32 awdlChannelFrequency(UInt8 channel){
    if(!channel)return 0;
    if(channel==14)return 2484;
    return channel<14?2407u+5u*channel:5000u+5u*channel;
}
// The channels a frame may be radiated on from this host: social channels only (6/44/149)
// and only when the regulatory policy allows active TX there (run 2: 44 was no-IR).
inline bool awdlChannelAllowed(const RegulatoryPolicy &p,UInt8 channel){
    if(channel!=6 && channel!=44 && channel!=149)return false;
    return channelDefinitionAllowed(channel,channel,20,false) && regulatoryPower(p,channel,channel,20,true)!=0;
}
// Shared placement: the AP's channel is whatever the station joined; only the social-channel test applies here.
inline bool awdlSharedChannelAllowed(UInt8 channel){return channel==6 || channel==44 || channel==149;}
inline void awdlHeader(UInt8 *f,UInt8 endpoint,UInt8 sequence,UInt32 bytes,UInt32 command){
    f[0]=endpoint;f[1]=1;f[5]=sequence;htcWriteLE16(f+2,static_cast<UInt16>(bytes-8));htcWriteLE32(f+8,command);
}
// Builds the firmware command for the current step of r. Returns the frame length or 0.
inline UInt32 buildAwdlRadioCommand(UInt8 *f,UInt32 capacity,UInt8 endpoint,UInt8 sequence,const AwdlRadio &r,const RegulatoryPolicy &p){
    if(!f || capacity<kAwdlWMIFrameBytes || !endpoint || endpoint==255)return 0;
    htcZero(f,kAwdlWMIFrameBytes);
    const UInt8 channel=r.step==AwdlStep::Restart?r.channelTarget:r.channel;
    UInt32 bytes=0,cmd=0;
    switch(r.step){
    case AwdlStep::Create:
        cmd=0x5001;bytes=36;connTLV(f+12,86,20);htcWriteLE32(f+16,kAwdlVdevId);htcWriteLE32(f+20,kAwdlVdevTypeMonitor);
        for(UInt32 i=0;i<6;++i)f[28+i]=r.mac[i];
        break;
    case AwdlStep::Start:
    case AwdlStep::Restart:{
        UInt8 power=0;UInt32 center=channel,phy=UInt32(awdlChannelPhyMode(channel))|(1u<<11);
        if(r.shared){
            if(!awdlSharedChannelAllowed(channel) || !r.powerHalfDbm || !r.sharedPhy || !r.sharedCenter)return 0;
            power=r.powerHalfDbm;center=r.sharedCenter;phy=r.sharedPhy;
        }else{
            const UInt8 limit=regulatoryPower(p,channel,channel,20,true);
            if(!awdlChannelAllowed(p,channel) || !limit)return 0;
            power=r.powerHalfDbm && r.powerHalfDbm<limit?r.powerHalfDbm:limit;
        }
        cmd=r.step==AwdlStep::Restart?0x5004:0x5003;bytes=120;connTLV(f+12,88,72);
        htcWriteLE32(f+16,kAwdlVdevId);
        connTLV(f+88,80,24);htcWriteLE32(f+92,awdlChannelFrequency(channel));htcWriteLE32(f+96,awdlChannelFrequency(UInt8(center)));
        htcWriteLE32(f+104,phy);
        writeChannelPower(f+108,f+112,power);connTLV(f+116,18,0);
        break;
    }
    case AwdlStep::Up:
    case AwdlStep::RestartUp:
        cmd=0x5005;bytes=32;connTLV(f+12,92,16);htcWriteLE32(f+16,kAwdlVdevId);
        for(UInt32 i=0;i<6;++i)f[24+i]=r.mac[i];
        break;
    case AwdlStep::Down:
        cmd=0x5007;bytes=20;connTLV(f+12,94,4);htcWriteLE32(f+16,kAwdlVdevId);break;
    case AwdlStep::Stop:
        cmd=0x5006;bytes=20;connTLV(f+12,93,4);htcWriteLE32(f+16,kAwdlVdevId);break;
    case AwdlStep::Delete:
        cmd=0x5002;bytes=20;connTLV(f+12,87,4);htcWriteLE32(f+16,kAwdlVdevId);break;
    default:return 0;
    }
    awdlHeader(f,endpoint,sequence,bytes,cmd);
    return bytes;
}
// Does this step wait for a WMI event after its CE completion?
inline bool awdlStepExpectsEvent(AwdlStep s){return s==AwdlStep::Start || s==AwdlStep::Restart || s==AwdlStep::Stop;}

inline bool awdlRequestStart(AwdlRadio &r,const UInt8 *mac,UInt8 channel,const RegulatoryPolicy &p){
    if(r.step==AwdlStep::Failed || !mac || (mac[0]&1) || !awdlChannelAllowed(p,channel))return false;
    if(r.active()){
        if(r.shared)return false;                      // the placement changes through a stop
        r.stopRequested=false;for(UInt32 i=0;i<6;++i)r.mac[i]=mac[i];r.channelTarget=channel;return true;
    }
    r.reset();for(UInt32 i=0;i<6;++i)r.mac[i]=mac[i];
    r.channel=r.channelTarget=channel;r.startRequested=true;return true;
}
// The channel definition of the station the monitor vdev shares (Ath10kAwdlService.cpp fills it in).
struct AwdlSharedChannel{UInt8 channel=0,center=0;UInt32 phy=0;UInt8 power=0;};
inline bool awdlRequestStartShared(AwdlRadio &r,const UInt8 *mac,const AwdlSharedChannel &c){
    if(r.step==AwdlStep::Failed || !mac || (mac[0]&1) || !awdlSharedChannelAllowed(c.channel) || !c.power || !c.phy || !c.center)return false;
    if(r.active())return r.shared && r.channel==c.channel;   // already there; a different placement changes through a stop
    r.reset();for(UInt32 i=0;i<6;++i)r.mac[i]=mac[i];
    r.channel=r.channelTarget=c.channel;r.shared=true;r.sharedCenter=c.center;r.sharedPhy=c.phy;r.powerHalfDbm=c.power;r.startRequested=true;
    return true;
}
inline void awdlRequestStop(AwdlRadio &r){if(r.active())r.stopRequested=true;r.startRequested=false;}
inline bool awdlRequestChannel(AwdlRadio &r,UInt8 channel,const RegulatoryPolicy &p){
    if(!r.active() || r.stopRequested || !awdlChannelAllowed(p,channel))return false;
    r.channelTarget=channel;return true;
}
// A command the core just queued in CE3.
inline void awdlCommandSubmitted(AwdlRadio &r,UInt64 nowMs){if(r.step==AwdlStep::Delete)++r.deletesPending;r.wmiSubmitted=true;r.eventPending=awdlStepExpectsEvent(r.step);r.sinceMs=nowMs;}
inline void awdlFail(AwdlRadio &r,const char *why){r.step=AwdlStep::Failed;r.error=why;r.wmiSubmitted=r.eventPending=false;++r.counters.failures;}
// Called when the CE3 completion of the outstanding command was seen.
inline void awdlCommandComplete(AwdlRadio &r,UInt64 nowMs){
    r.wmiSubmitted=false;
    if(r.eventPending)return; // keep waiting for the event
    // Start/Restart/Stop whose event arrived before the CE completion:
    if(r.step==AwdlStep::Start){r.step=AwdlStep::Up;r.sinceMs=nowMs;return;}
    if(r.step==AwdlStep::Restart){r.step=AwdlStep::RestartUp;r.sinceMs=nowMs;return;}
    if(r.step==AwdlStep::Stop){r.step=AwdlStep::Delete;r.sinceMs=nowMs;return;}
    switch(r.step){
    case AwdlStep::Create:r.step=AwdlStep::Start;break;
    case AwdlStep::Up:r.step=AwdlStep::Running;r.sinceMs=nowMs;break;
    case AwdlStep::RestartUp:r.step=AwdlStep::Running;r.channel=r.channelTarget;++r.counters.channelSwitches;r.sinceMs=nowMs;break;
    case AwdlStep::Down:r.step=r.stopRequested?AwdlStep::Stop:AwdlStep::Restart;break;
    case AwdlStep::Delete:r.step=AwdlStep::Off;r.stopRequested=false;break;
    default:break;
    }
}
// Vdev start/restart response (status 0 = ok).
inline void awdlStartResponse(AwdlRadio &r,UInt32 respType,UInt32 status,UInt64 nowMs){
    const bool restart=r.step==AwdlStep::Restart;
    if((r.step!=AwdlStep::Start && !restart) || respType!=(restart?1u:0u) || !r.eventPending)return;
    r.eventPending=false;
    if(status){awdlFail(r,restart?"awdl-vdev-restart-rejected":"awdl-vdev-start-rejected");return;}
    if(!r.wmiSubmitted){r.step=restart?AwdlStep::RestartUp:AwdlStep::Up;r.sinceMs=nowMs;}
}
inline void awdlStopped(AwdlRadio &r,UInt64 nowMs){
    if(r.step!=AwdlStep::Stop || !r.eventPending)return;
    r.eventPending=false;if(!r.wmiSubmitted){r.step=AwdlStep::Delete;r.sinceMs=nowMs;}
}
// What should the radio do next, with no command outstanding? Returns the step to
// issue a command for (the step is set) or leaves it alone.
inline void awdlPlan(AwdlRadio &r,bool infraIdle,bool infraJoined,UInt8 infraChannel,UInt64 nowMs){
    if(r.step==AwdlStep::Failed)return;
    // Exclusive needs the station vdev idle; shared needs the station joined on the channel the monitor vdev shares.
    const bool allowed=r.shared?(infraJoined && infraChannel==r.channel):infraIdle;
    if(r.startRequested && r.step==AwdlStep::Off && allowed){r.startRequested=false;r.step=AwdlStep::Create;r.sinceMs=nowMs;return;}
    if(r.step==AwdlStep::Running){
        // Any infrastructure change takes priority over AWDL: the monitor vdev goes away and
        // the front end restarts it in whatever placement then applies.
        if(!allowed)r.stopRequested=true;
        if(r.stopRequested || r.channelTarget!=r.channel){r.step=AwdlStep::Down;r.sinceMs=nowMs;}
    }
}
inline bool awdlTimedOut(const AwdlRadio &r,UInt64 nowMs){
    return (r.wmiSubmitted || r.eventPending) && nowMs-r.sinceMs>=kAwdlTimeoutMs;
}
// Consumes a WMI event that belongs to the AWDL vdev. Returns true when the core
// parser must not look at it (the station path would reject it as unsolicited).
inline bool awdlConsumeVdevEvent(AwdlRadio &r,const UInt8 *f,UInt32 end,UInt32 eventId,UInt64 nowMs){
    if(eventId!=0x5001 && eventId!=0x5002 && eventId!=0x5006)return false;
    // VDEV_DELETE's event payload is opaque (Linux does not read it): correlated by count.
    if(eventId==0x5006 && r.deletesPending){--r.deletesPending;return true;}
    if(end<20 || connLE32(f+16)!=kAwdlVdevId)return false; // first field: vdev id
    if(eventId==0x5001){
        if(end<32 || htcReadLE16(f+14)!=40)return true;
        awdlStartResponse(r,connLE32(f+24),connLE32(f+28),nowMs);
        return true;
    }
    if(eventId==0x5002)awdlStopped(r,nowMs);
    return true;
}
inline bool awdlPutRx(AwdlRadio &r,AwdlRx kind,const UInt8 *frame,UInt32 length,UInt8 channel,SInt8 rssi,UInt64 timeUs){
    if(length>kAwdlFrameMax || !frame){++r.counters.rxDropped;return false;}
    if(r.rxCount==kAwdlRxSlots){++r.counters.rxDropped;return false;}
    AwdlRxEntry &e=r.rx[(r.rxHead+r.rxCount)%kAwdlRxSlots];
    e.length=static_cast<UInt16>(length);e.kind=static_cast<UInt8>(kind);e.channel=channel;e.rssi=rssi;e.timeUs=timeUs;
    for(UInt32 i=0;i<length;++i)e.data[i]=frame[i];
    ++r.rxCount;
    return true;
}
inline bool awdlIsAwdlAction(const UInt8 *frame,UInt32 length){
    // FC = management/action, category 127, OUI 00:17:f2, type 8.
    return length>=24+6 && (htcReadLE16(frame)&0x00fc)==0x00d0 && frame[24]==127 &&
        frame[25]==kAwdlOuiBytes[0] && frame[26]==kAwdlOuiBytes[1] && frame[27]==kAwdlOuiBytes[2] && frame[28]==8;
}
inline bool awdlIsBssid(const UInt8 *bssid){for(UInt32 i=0;i<6;++i)if(bssid[i]!=kAwdlBssidBytes[i])return false;return true;}
// From the WMI management RX event. mgmtStatus nonzero means a corrupt frame. RSSI = SNR - 95 (the core's convention).
inline bool awdlRxManagement(AwdlRadio &r,const UInt8 *frame,UInt32 length,UInt32 channel,UInt32 snr,UInt32 status,UInt64 timeUs){
    if(!r.running() || status || !frame || !awdlIsAwdlAction(frame,length))return false;
    if(!awdlPutRx(r,AwdlRx::Action,frame,length,static_cast<UInt8>(channel),static_cast<SInt8>(snr<=95?int(snr)-95:0),timeUs))return false;
    ++r.counters.rxAction;return true;
}
// From HTT RX: one MPDU with its 300-byte descriptor, raw (FCS included, mode 0) or native-WiFi
// (mode 1, no FCS). Only data frames to/from the AWDL BSSID are taken.
inline bool awdlRxHtt(AwdlRadio &r,const UInt8 *d,UInt32 available,UInt16 indicated,UInt8 status,UInt64 timeUs){
    if(!r.running() || status!=1 || !d || available<324)return false;
    const UInt32 mode=(connLE32(d+32)>>8)&3,length=indicated?indicated:(connLE32(d+24)&0x3fff);
    if((mode!=0 && mode!=1) || length<24 || length>available-300)return false;
    const UInt8 *mac=d+300;const UInt16 fc=htcReadLE16(mac);
    if((fc&0x000c)!=0x0008 || !awdlIsBssid(mac+16))return false; // data, address 3 is the AWDL BSSID
    const UInt32 keep=mode==0?length-4:length; // raw MPDUs still carry the FCS
    if(keep<24)return false;
    if(!awdlPutRx(r,AwdlRx::Data,mac,keep,r.channel,0,timeUs))return false;
    ++r.counters.rxData;return true;
}
inline bool awdlGetRx(AwdlRadio &r,const UInt8 **frame,UInt32 *length,UInt8 *kind,UInt8 *channel,SInt8 *rssi,UInt64 *timeUs){
    if(!r.rxCount)return false;
    const AwdlRxEntry &e=r.rx[r.rxHead];
    *frame=e.data;*length=e.length;*kind=e.kind;*channel=e.channel;*rssi=e.rssi;*timeUs=e.timeUs;
    return true;
}
inline void awdlPopRx(AwdlRadio &r){if(r.rxCount){r.rxHead=(r.rxHead+1)%kAwdlRxSlots;--r.rxCount;}}
inline bool awdlQueueTx(AwdlRadio &r,const UInt8 *frame,UInt32 length,bool action){
    if(!r.running() || !frame || length<24 || length>kAwdlFrameMax || r.txCount==kAwdlTxSlots){++r.counters.txDropped;return false;}
    AwdlTxEntry &e=r.tx[(r.txHead+r.txCount)%kAwdlTxSlots];
    e.length=static_cast<UInt16>(length);e.action=action;for(UInt32 i=0;i<length;++i)e.data[i]=frame[i];
    ++r.txCount;return true;
}
// HTT TX_FRM for an AWDL frame on the monitor vdev: the same descriptor as buildHTTPacket
// with the vdev id in flags1 bits 0..5 and packet type MGMT (action) or RAW (data).
inline UInt32 buildAwdlHTT(UInt8 *f,UInt32 capacity,const UInt8 *frame,UInt32 length,UInt32 address,UInt32 fragments,
                           UInt8 endpoint,UInt8 httSequence,UInt16 txId,bool action){
    if(!f || !frame || capacity<256 || !endpoint || endpoint==255 || !address || !fragments || length<24 || length>kAwdlFrameMax)return 0;
    htcZero(f,256);f[0]=endpoint;f[5]=httSequence;
    const UInt32 prefetch=length<50?length:50,bytes=24+((prefetch+3u)&~3u);
    f[8]=1;
    f[9]=action?0x65:0x05;      // MGMT|HDR_PRESENT|NO_ENCRYPT, or RAW|HDR_PRESENT|NO_ENCRYPT
    const UInt16 tid=action?17:16; // 17 management, 16 non-QoS multicast/broadcast
    htcWriteLE16(f+10,static_cast<UInt16>(0x800u|(tid<<6)|kAwdlVdevId));
    htcWriteLE16(f+12,static_cast<UInt16>(length));htcWriteLE16(f+14,txId);
    htcWriteLE32(f+16,action?address:fragments);htcWriteLE32(f+20,0xffff);
    for(UInt32 i=0;i<prefetch;++i)f[24+i]=frame[i];
    htcWriteLE16(f+2,static_cast<UInt16>(bytes-8));
    return bytes;
}
inline const char *awdlStepName(AwdlStep s){
    switch(s){case AwdlStep::Off:return "off";case AwdlStep::Create:return "create";case AwdlStep::Start:return "start";case AwdlStep::Up:return "up";
    case AwdlStep::Running:return "running";case AwdlStep::Down:return "down";case AwdlStep::Restart:return "restart";case AwdlStep::RestartUp:return "restart-up";
    case AwdlStep::Stop:return "stop";case AwdlStep::Delete:return "delete";case AwdlStep::Failed:return "failed";}
    return "?";
}
} // namespace Ath10k
#endif // AIRPORT_ATH10K_AWDL
