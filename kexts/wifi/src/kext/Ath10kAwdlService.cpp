// SPDX-License-Identifier: GPL-3.0-or-later
// AWDL radio binding: the part that talks to the CE rings and HTT. Compiled only with
// AIRPORT_ATH10K_AWDL=1 (kexts/wifi/native/scripts/cross-build-native.sh). The logic it
// runs is in Ath10kAwdl.hpp and is host-tested; this file is the thin glue.
#include "Ath10kPCIDevice.hpp"
#if AIRPORT_ATH10K_AWDL
using Ath10k::AwdlStep;

IOReturn Ath10kPCIDevice::awdlControl(const AthControl::Request &r,AthControl::Response &o)
{
    using namespace AthControl;
    if(r.operation>=AwdlOperationEnd)return kIOReturnBadArgument;
    if(r.operation==AwdlStatus){
        if(!_awdl){o.data[0]=0;o.length=3;return kIOReturnSuccess;}
        const auto &a=*_awdl;o.data[0]=static_cast<UInt8>(a.step);o.data[1]=a.channel;
        const char *e=a.error?a.error:"";UInt32 n=static_cast<UInt32>(strnlen(e,64));o.data[2]=static_cast<UInt8>(n);
        const UInt32 c[]={a.counters.rxAction,a.counters.rxData,a.counters.rxDropped,a.counters.txAction,a.counters.txData,a.counters.txDropped,a.counters.txFailed,a.counters.channelSwitches,a.counters.failures};
        for(UInt32 i=0;i<9;++i)Ath10k::htcWriteLE32(o.data+3+4*i,c[i]);
        bcopy(e,o.data+39,n);o.length=39+n;return kIOReturnSuccess;
    }
    if(!_wmiLive.active || _suspending || _power.reinitRunning)return kIOReturnNotReady;
    if(r.operation==AwdlRadioStart){
        if(r.length!=7)return kIOReturnBadArgument;
        if(!_awdl){_awdl=static_cast<Ath10k::AwdlRadio *>(IOMalloc(sizeof(Ath10k::AwdlRadio)));if(!_awdl)return kIOReturnNoMemory;new(_awdl)Ath10k::AwdlRadio();}
        if(!Ath10k::awdlRequestStart(*_awdl,r.data,r.data[6],_regulatory))return _awdl->step==AwdlStep::Failed?kIOReturnNotReady:kIOReturnUnsupported;
        return kIOReturnSuccess;
    }
    if(!_awdl)return kIOReturnNotReady;
    if(r.operation==AwdlRadioStop){Ath10k::awdlRequestStop(*_awdl);return kIOReturnSuccess;}
    if(r.operation==AwdlSetChannel){
        if(r.length!=1)return kIOReturnBadArgument;
        return Ath10k::awdlRequestChannel(*_awdl,r.data[0],_regulatory)?kIOReturnSuccess:kIOReturnUnsupported;
    }
    return kIOReturnBadArgument;
}

// Runs on the core's WMI work loop, after the station state machine, every runtime tick.
// One firmware command at a time, sharing _connectionCommand with the station path only
// while that path is idle (Disabled, nothing owned).
const char *Ath10kPCIDevice::serviceAwdl(UInt64 nowMs)
{
    auto &r=*_awdl;auto &s=_connection;
    if(r.step==AwdlStep::Off && !r.startRequested)return nullptr;
    if(!_wmiLive.active || _suspending || _power.reinitRunning)return nullptr;
    if(r.step==AwdlStep::Failed)return nullptr;
    // Outstanding CE3 command: same completion rule as every other WMI user.
    if(r.wmiSubmitted){
        Ath10k::CESnapshot snapshot{};
        if(!Ath10k::readCESnapshot(_regs,3,snapshot) || !validateBMIPipeStructure(3,snapshot) || snapshot.srcWriteIndex!=s.wmiNext){Ath10k::awdlFail(r,"awdl-ce-state");return nullptr;}
        if(snapshot.currentSrcRead==s.wmiNext && !_wmi.credits.outstanding){
            if(!_connectionCommand.syncFromDevice() || !_ceSrcRings[3].syncFromDevice()){Ath10k::awdlFail(r,"awdl-ce-sync");return nullptr;}
            s.wmiOwned=false;Ath10k::awdlCommandComplete(r,nowMs);
        }
    }
    // TX completion (the station path is idle, so it does not look at it).
    // A station bring-up that started meanwhile runs the shared completion logic itself.
    if(r.txOwned && !s.txOwned)r.txOwned=false;
    if(r.txOwned){
        Ath10k::CESnapshot snapshot{};
        if(!Ath10k::readCESnapshot(_regs,4,snapshot) || !validateBMIPipeStructure(4,snapshot) || snapshot.srcWriteIndex!=s.txNext){Ath10k::awdlFail(r,"awdl-tx-ce-state");return nullptr;}
        if(snapshot.currentSrcRead==s.txNext && s.txComplete){
            if(!_packetCommand.syncFromDevice() || !_ceSrcRings[4].syncFromDevice()){Ath10k::awdlFail(r,"awdl-tx-ce-sync");return nullptr;}
            s.txOwned=false;r.txOwned=false;
            if(s.txStatus)++r.counters.txFailed;else if(r.txLastAction)++r.counters.txAction;else ++r.counters.txData;
        }else if(nowMs-s.txSinceMs>=5000){Ath10k::awdlFail(r,"awdl-tx-timeout");return nullptr;}
    }
    if(Ath10k::awdlTimedOut(r,nowMs)){Ath10k::awdlFail(r,"awdl-firmware-timeout");return nullptr;}
    if(r.wmiSubmitted || r.eventPending)return nullptr;
    // The station path owns the rings whenever it is doing anything but idling.
    const bool infraIdle=s.stage==Ath10k::LinkStage::Disabled && s.disconnect==Ath10k::DisconnectStage::None && !s.macChange.step;
    const bool busy=s.wmiOwned || s.txOwned || s.dataPending || _wmi.credits.outstanding || !_wmiScan.completed || _wmiScan.txOwned;
    Ath10k::awdlPlan(r,infraIdle,nowMs);
    const bool needsCommand=r.step==AwdlStep::Create || r.step==AwdlStep::Start || r.step==AwdlStep::Up || r.step==AwdlStep::Down ||
                            r.step==AwdlStep::Restart || r.step==AwdlStep::RestartUp || r.step==AwdlStep::Stop || r.step==AwdlStep::Delete;
    if(needsCommand){
        if(busy)return nullptr;
        if(!_connectionCommand.allocated() && !_connectionCommand.allocate(256,128,_dmaMapper))return nullptr;
        const UInt32 bytes=Ath10k::buildAwdlRadioCommand(static_cast<UInt8 *>(_connectionCommand.virtualAddress()),256,_htcWMIControl.endpoint,s.wmiSequence,r,_regulatory);
        if(!bytes){Ath10k::awdlFail(r,"awdl-command-build");return nullptr;}
        if(const char *error=submitNetworkCE(3,_connectionCommand,bytes,s.wmiNext)){(void)error;Ath10k::awdlFail(r,"awdl-ce-submit");return nullptr;}
        ++s.wmiSequence;Ath10k::awdlCommandSubmitted(r,nowMs);return nullptr;
    }
    if(r.running() && r.txCount && !r.txOwned && !busy){
        const Ath10k::AwdlTxEntry &e=r.tx[r.txHead];
        if(!_packetBuffer.allocated() && !_packetBuffer.allocate(2048,128,_dmaMapper,Ath10kDMABuffer::Role::Payload))return nullptr;
        if(!_packetCommand.allocated() && !_packetCommand.allocate(256,128,_dmaMapper))return nullptr;
        if(!_packetFragments.allocated() && !_packetFragments.allocate(16,16,_dmaMapper))return nullptr;
        bzero(_packetBuffer.virtualAddress(),2048);bcopy(e.data,_packetBuffer.virtualAddress(),e.length);
        auto *frags=static_cast<UInt8 *>(_packetFragments.virtualAddress());bzero(frags,16);
        Ath10k::htcWriteLE32(frags,static_cast<UInt32>(_packetBuffer.dmaAddress()));Ath10k::htcWriteLE32(frags+4,e.length);
        s.txId=static_cast<UInt16>((s.txId+1)%1056);s.txComplete=false;s.txStatus=0;s.txManagement=e.action;
        s.lastTxSpectrum=s.lastTxProbe=s.lastTxEAPOL=false;
        const UInt32 length=Ath10k::buildAwdlHTT(static_cast<UInt8 *>(_packetCommand.virtualAddress()),256,e.data,e.length,
            static_cast<UInt32>(_packetBuffer.dmaAddress()),static_cast<UInt32>(_packetFragments.dmaAddress()),_htcHTTData.endpoint,s.httSequence,s.txId,e.action);
        if(!length || !_packetBuffer.syncForDevice() || !_packetFragments.syncForDevice()){Ath10k::awdlFail(r,"awdl-tx-build");return nullptr;}
        s.txSinceMs=nowMs;
        if(submitNetworkCE(4,_packetCommand,length,s.txNext)){Ath10k::awdlFail(r,"awdl-tx-submit");return nullptr;}
        ++s.httSequence;r.txOwned=true;r.txLastAction=e.action;
        r.txHead=(r.txHead+1)%Ath10k::kAwdlTxSlots;--r.txCount;
    }
    return nullptr;
}

// Frame calls come from the front end's IO80211 work queue; everything that touches
// the radio struct runs on the core's work loop, as control requests do.
struct AwdlFrameRequest{bool tx,action,ok;const UInt8 *frame;UInt32 length;UInt8 *buffer;UInt32 capacity;UInt32 *outLength;UInt8 *kind,*channel;SInt8 *rssi;UInt64 *timeUs;};
IOReturn Ath10kPCIDevice::awdlFrameAction(OSObject *owner,void *arg,void *,void *,void *)
{
    auto *self=OSDynamicCast(Ath10kPCIDevice,owner);auto *q=static_cast<AwdlFrameRequest *>(arg);
    if(!self || !q || !self->_awdl)return kIOReturnNotReady;
    auto &r=*self->_awdl;
    if(q->tx){q->ok=Ath10k::awdlQueueTx(r,q->frame,q->length,q->action);return kIOReturnSuccess;}
    const UInt8 *data;UInt32 n;
    if(!Ath10k::awdlGetRx(r,&data,&n,q->kind,q->channel,q->rssi,q->timeUs)){q->ok=false;return kIOReturnSuccess;}
    if(n>q->capacity){Ath10k::awdlPopRx(r);q->ok=false;return kIOReturnSuccess;}
    bcopy(data,q->buffer,n);*q->outLength=n;Ath10k::awdlPopRx(r);q->ok=true;return kIOReturnSuccess;
}
bool Ath10kPCIDevice::nativeAwdlTx(const UInt8 *frame,UInt32 length,bool action)
{
    AwdlFrameRequest q{};q.tx=true;q.action=action;q.frame=frame;q.length=length;
    if(!_wmiWorkLoop || _wmiWorkLoop->runAction(awdlFrameAction,this,&q)!=kIOReturnSuccess)return false;
    return q.ok;
}
bool Ath10kPCIDevice::nativeAwdlRx(UInt8 *buffer,UInt32 capacity,UInt32 *length,UInt8 *kind,UInt8 *channel,SInt8 *rssi,UInt64 *timeUs)
{
    AwdlFrameRequest q{};q.tx=false;q.buffer=buffer;q.capacity=capacity;q.outLength=length;q.kind=kind;q.channel=channel;q.rssi=rssi;q.timeUs=timeUs;
    if(!_wmiWorkLoop || _wmiWorkLoop->runAction(awdlFrameAction,this,&q)!=kIOReturnSuccess)return false;
    return q.ok;
}
#endif // AIRPORT_ATH10K_AWDL
