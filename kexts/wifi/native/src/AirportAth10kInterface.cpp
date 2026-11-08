// SPDX-License-Identifier: GPL-3.0-or-later
#include "AirportAth10kInterface.hpp"
#include "AirportAth10k.hpp"
#include <kern/clock.h>

#define super IO80211InfraProtocol
OSDefineMetaClassAndStructors(AirportAth10kInterface, IO80211InfraProtocol)

enum : uintptr_t { OpScan, OpScanAbort, OpJoin, OpJoinAbort, OpLeave, OpPoll, OpStop, OpPower, OpQuery, OpBeacon, OpBssInfo, OpExtendedBss, OpPowerSave, OpCountry, OpStationMAC, OpSystemPower };

int AirportAth10kInterface::CoreAdapter::call(const AthControl::Request &r,AthControl::Response &o)
{
    Ath10kPCIDevice *core=owner && owner->controller?owner->controller->core():nullptr;
    if(!core)return kIOReturnNotAttached;
    return core->controlRequest(owner,&r,&o);
}
void AirportAth10kInterface::SinkAdapter::post(uint32_t message,const void *data,size_t length)
{
    if(owner && owner->controller && owner->started && !owner->stopping)owner->controller->postInterfaceMessage(message,const_cast<void *>(data),length);
}
void AirportAth10kInterface::SinkAdapter::linkState(bool up)
{
    if(owner && owner->controller && owner->started && !owner->stopping){owner->controller->nativeLink(up);owner->updateLinkQuality();}
}

bool AirportAth10kInterface::init(AirportAth10k *provider)
{
    if(!IO80211InfraInterface::init())return false;
    controller=provider;
    IOEthernetAddress address;
    if(!controller || controller->getHardwareAddress(&address)!=kIOReturnSuccess)return false;
    memcpy(permanentMAC,address.bytes,6);
    if(!IO80211SkywalkInterface::init(provider,reinterpret_cast<ether_addr *>(&address)))return false;
    coreAdapter.owner=sinkAdapter.owner=this;
    session=new AirportAth10kSession(coreAdapter,sinkAdapter);
    if(!session)return false;
    AIRPORT_STEP("interface init: family init done, MAC copied");
    timer=IOTimerEventSource::timerEventSource(this,pollTimer);
    IO80211WorkQueue *queue=controller->getWorkQueue();
    if(!timer || !queue || queue->addEventSource(timer)!=kIOReturnSuccess)return false;
    // Not armed here: the first poll must not post messages into the family
    // while IO80211Controller::start/attachInterface/IO80211InfraInterface::start
    // are still running on the config thread (startPolling, after registerService).
    return true;
}

void AirportAth10kInterface::startPolling()
{
    if(started || stopping || !timer)return;
    AIRPORT_STEP("interface: polling armed (100 ms)");
    started=true;
    timer->setTimeoutMS(100);
}

void AirportAth10kInterface::stopSession()
{
    if(controller && controller->commandGate())controller->commandGate()->runAction(command,this,reinterpret_cast<void *>(OpStop));
    stopping=true;
    if(timer){
        timer->cancelTimeout();
        if(timer->getWorkLoop())timer->getWorkLoop()->removeEventSource(timer);
        OSSafeReleaseNULL(timer);
    }
    if(controller && controller->core())controller->core()->closeControl(this);
}

bool AirportAth10kInterface::installRegistrationInfo(const IOSkywalkEthernetInterface::RegistrationInfo &info)
{
    static_assert(sizeof(IOSkywalkNetworkInterface::RegistrationInfo)==AirportRegistration::kInfoBytes &&
                  sizeof(IOSkywalkEthernetInterface::RegistrationInfo)==AirportRegistration::kInfoBytes,"RegistrationInfo size (IOSkywalkFamily 1.0)");
    if(!mExpansionData || !mExpansionData2)return false;
    return registration.install(reinterpret_cast<void **>(&mExpansionData->fRegistrationInfo),
                                reinterpret_cast<void **>(&mExpansionData2->fRegistrationInfo),&info,sizeof(info),
                                [](size_t n){return IOMalloc(n);},[](void *p,size_t n){IOFree(p,n);});
}
void AirportAth10kInterface::releaseRegistrationInfo()
{
    registration.release(mExpansionData?reinterpret_cast<void **>(&mExpansionData->fRegistrationInfo):nullptr,
                         mExpansionData2?reinterpret_cast<void **>(&mExpansionData2->fRegistrationInfo):nullptr,
                         [](void *p,size_t n){IOFree(p,n);});
}

// Terminate path (reboot, shutdown, the wake restart): IOSkywalkNetworkInterface::stop
// typed-frees mExpansionData->fRegistrationInfo. Take our blocks back first.
void AirportAth10kInterface::stop(IOService *provider)
{
    AIRPORT_STEP("interface stop");
    stopping=true;
    releaseRegistrationInfo();
    super::stop(provider);
}

void AirportAth10kInterface::free()
{
    if(timer)stopSession();
    if(session){session->stop();delete session;session=nullptr;}
    releaseRegistrationInfo(); // IOSkywalkEthernetInterface::free typed-frees mExpansionData2's block
    super::free();
}

uint64_t AirportAth10kInterface::nowMs() const
{
    uint64_t t=0,ns=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);return ns/1000000u;
}

void AirportAth10kInterface::pollTimer(OSObject *owner,IOTimerEventSource *)
{
    auto *self=OSDynamicCast(AirportAth10kInterface,owner);
    if(!self || self->stopping || !self->started || !self->session)return;
    static bool first=true;
    if(first){first=false;AIRPORT_STEP("interface: first poll of the core");}
    // The timer runs on the IO80211 work queue, under the same gate as the
    // WCL commands below; the session is therefore single-threaded.
    const auto before=self->session->state();
    const bool linked=self->session->linkUp();
    self->session->poll(self->nowMs());
    if(before!=self->session->state() || linked!=self->session->linkUp())self->updateLinkQuality();
    self->mirrorState();
    // 20 ms while a scan or join waits on the core (each step of the join and
    // of the 4-way handshake is taken on the next poll), 100 ms otherwise.
    if(self->timer && !self->stopping)self->timer->setTimeoutMS(self->session->busy()?20:100);
}

// A WCL command changed what the session waits for: poll on the next turn of
// the work queue instead of up to 100 ms later.
void AirportAth10kInterface::kick()
{
    if(started && !stopping && timer)timer->setTimeoutMS(1);
}

// Registry mirror of the session (on the controller), written only on change:
// AirportAth10kJoinState, AirportAth10kLastError, AirportAth10kLastDisconnectReason.
// No log line here: this runs from the 100 ms poll.
void AirportAth10kInterface::mirrorState()
{
    if(!controller || !session || controller->isInactive())return;
    const uint32_t state=static_cast<uint32_t>(session->state());
    if(state!=mirroredState){mirroredState=state;controller->setProperty("AirportAth10kJoinState",AirportAth10kSession::stateName(session->state()));}
    if(session->errorSerial!=mirroredErrorSerial){mirroredErrorSerial=session->errorSerial;controller->setProperty("AirportAth10kLastError",session->lastError?session->lastError:"none");}
    if(session->lastDisconnectReason!=mirroredReason){mirroredReason=session->lastDisconnectReason;controller->setProperty("AirportAth10kLastDisconnectReason",mirroredReason,32);}
    // Timing of the last core scan and of the last successful join (WCL
    // request to authorized port), number of core scans, cached BSSes.
    if(session->lastScanMs!=mirroredScanMs){mirroredScanMs=session->lastScanMs;controller->setProperty("AirportAth10kLastScanMs",mirroredScanMs,32);}
    if(session->lastJoinMs!=mirroredJoinMs){mirroredJoinMs=session->lastJoinMs;controller->setProperty("AirportAth10kLastJoinMs",mirroredJoinMs,32);}
    if(session->scanCount!=mirroredScans){mirroredScans=session->scanCount;controller->setProperty("AirportAth10kScanCount",mirroredScans,32);}
    // System sleep/wake: AirportAth10kLastSleep/LastWake (uptime ms),
    // AirportAth10kWakeMs (wake to driver-available), AirportAth10kWakeResult.
    if(session->wakeSerial!=mirroredWake){
        mirroredWake=session->wakeSerial;
        controller->setProperty("AirportAth10kLastSleep",session->lastSleepMs,64);
        controller->setProperty("AirportAth10kLastWake",session->lastWakeMs,64);
        controller->setProperty("AirportAth10kWakeMs",session->wakeMs,32);
        controller->setProperty("AirportAth10kWakeResult",session->wakeResult);
    }
    if(session->roams!=mirroredRoams || session->lastRoamMs!=mirroredRoamMs){
        mirroredRoams=session->roams;mirroredRoamMs=session->lastRoamMs;
        controller->setProperty("AirportAth10kRoams",mirroredRoams,32);controller->setProperty("AirportAth10kLastRoamMs",mirroredRoamMs,32);
    }
    if(session->networkCount()!=mirroredNetworks){mirroredNetworks=session->networkCount();controller->setProperty("AirportAth10kCachedNetworks",mirroredNetworks,32);}
}

IOReturn AirportAth10kInterface::command(OSObject *,void *a0,void *a1,void *a2,void *)
{
    auto *self=static_cast<AirportAth10kInterface *>(a0);
    if(!self->session)return kIOReturnNotReady;
    if(self->stopping && reinterpret_cast<uintptr_t>(a1)!=OpStop)return kIOReturnNotReady;
    const uint64_t now=self->nowMs();
    switch(reinterpret_cast<uintptr_t>(a1)){
    case OpScan:{
        AirportWCL::ScanRequest scan;
        if(!AirportWCL::decodeScan(a2,0x1550,scan))return kIOReturnBadArgument;
        if(scan.channelCount){
            bool usable=false;
            for(const auto &c:AirportBridge::kChannels)if(AirportWCL::scanChannelSelected(scan.channels,true,c.number))usable=true;
            if(!usable)return kIOReturnUnsupported;
        }
        const IOReturn r=self->session->startScan(scan,now);
        if(r==kIOReturnSuccess)self->kick();
        return r;}
    case OpScanAbort:self->session->abortScan();return kIOReturnSuccess;
    case OpJoin:{
        AirportWCL::JoinRequest join;
        if(!AirportWCL::decodeJoin(a2,0x2cc,join))return kIOReturnBadArgument;
        const IOReturn r=self->session->startJoin(join,now);
        AthSecurity::wipe(&join,sizeof(join));
        if(r!=kIOReturnSuccess)AIRPORT_LOG("join refused: %s (0x%x)",self->session->lastError,r);
        else self->kick();
        return r;}
    case OpJoinAbort:return self->session->abortJoin(now);
    case OpLeave:{const IOReturn r=self->session->leaveNetwork(now);self->kick();return r;}
    case OpStop:self->session->stop();return kIOReturnSuccess;
    case OpPower:return self->session->setPower(a2!=nullptr,now);
    case OpPowerSave:return self->session->setPowerSave(a2!=nullptr);
    case OpSystemPower:
        if(a2)self->session->pmWake(now);else self->session->pmSleep(now);
        self->mirrorState();self->kick();return kIOReturnSuccess;
    case OpBeacon:{
        auto *out=static_cast<uint8_t *>(a2);
        return self->session->beaconInfo(out,0x808,now)?kIOReturnSuccess:kIOReturnNotReady;}
    case OpBssInfo:{
        uint8_t buffer[sizeof(AirportWCL::BeaconMetadata)+0x800];size_t n=0;
        if(!self->session->bssInfo(buffer,sizeof(buffer),n))return kIOReturnNotReady;
        memcpy(a2,buffer,n);return kIOReturnSuccess;}
    case OpStationMAC:
        // The address WCL set (private or the permanent one restored). The join
        // carries it whenever the firmware vdev currently has a different one.
        self->session->setStationMAC(static_cast<const uint8_t *>(a2));return kIOReturnSuccess;
    case OpCountry:{
        auto *cd=static_cast<apple80211_country_code_data *>(a2);
        memset(cd,0,sizeof(*cd));cd->version=APPLE80211_VERSION;
        memcpy(cd->cc,self->session->core_().country,2);return kIOReturnSuccess;}
    default:return kIOReturnUnsupported;
    }
}

IOReturn AirportAth10kInterface::gated(uintptr_t op,void *arg)
{
    IOCommandGate *gate=controller?controller->commandGate():nullptr;
    if(!gate)return kIOReturnNotReady;
    return gate->runAction(command,this,reinterpret_cast<void *>(op),arg);
}

void AirportAth10kInterface::stationMACChanged(const uint8_t *mac,bool)
{
    uint8_t copy[6];memcpy(copy,mac,6);
    gated(OpStationMAC,copy);
}
uint32_t AirportAth10kInterface::sessionState() const {return session?static_cast<uint32_t>(session->state()):0;}
void AirportAth10kInterface::powerChanged(bool on){gated(OpPower,on?this:nullptr);}
void AirportAth10kInterface::systemPowerChanged(bool on){gated(OpSystemPower,on?this:nullptr);}
IOReturn AirportAth10kInterface::countryCode(apple80211_country_code_data *cd){return gated(OpCountry,cd);}

// Link quality for WCL: 100/50/20 by RSSI while the port is up, -2 otherwise.
void AirportAth10kInterface::updateLinkQuality()
{
    if(stopping || !started || !session)return;
    const int quality=static_cast<int>(session->lqmQuality());
    if(quality!=reportedQuality){setLQM(static_cast<unsigned long long>(static_cast<long long>(quality)));reportedQuality=quality;}
}

// --- WCL commands ---------------------------------------------------------
IOReturn AirportAth10kInterface::setWCL_SCAN_REQ(apple80211ScanRequest *request){return request?gated(OpScan,request):kIOReturnBadArgument;}
IOReturn AirportAth10kInterface::setWCL_SCAN_ABORT(void *){return gated(OpScanAbort);}
IOReturn AirportAth10kInterface::setWCL_ASSOCIATE(apple80211AssocCandidates *request){return request?gated(OpJoin,request):kIOReturnBadArgument;}
IOReturn AirportAth10kInterface::setWCL_JOIN_ABORT(apple80211_wcl_abort_join *){return gated(OpJoinAbort);}
IOReturn AirportAth10kInterface::setWCL_LEAVE_NETWORK(apple80211_leave_network *){return gated(OpLeave);}
IOReturn AirportAth10kInterface::setCLEAR_PMKSA_CACHE(void *){return kIOReturnSuccess;} // no PMKSA cache: PSK only
IOReturn AirportAth10kInterface::setCIPHER_KEY(apple80211_key *key)
{
    // WCL delivers PMK/MSK here for enterprise networks only; Personal keys
    // arrive inside the association request and the kernel supplicant
    // installs PTK/GTK itself (never through this entry).
    if(!key)return kIOReturnBadArgument;
    return kIOReturnUnsupported;
}
IOReturn AirportAth10kInterface::setWCL_SET_SCAN_HOME_AWAY_TIME(scanHomeAndAwayTime *data)
{
    if(!data)return kIOReturnBadArgument;
    scanHomeAwayMs=static_cast<uint16_t>(AirportWCL::read32(reinterpret_cast<const uint8_t *>(data)));
    return kIOReturnSuccess; // the core's scan dwell is fixed (Ath10kScanPlan.hpp)
}
// Cisco Fast Lane: refusing it makes WCL tear the link down ~0.4 s after link-up.
IOReturn AirportAth10kInterface::setWCL_UPDATE_FAST_LANE(apple80211_fastlane *data){return data?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn AirportAth10kInterface::setWCL_LINK_STATE_UPDATE(apple80211_wcl_update_link_state *data)
{
    if(!data)return kIOReturnBadArgument;
    const IOReturn status=IO80211InfraInterface::setWCL_LINK_STATE_UPDATE(data);
    if(status==kIOReturnSuccess)updateLinkQuality();
    return status;
}
IOReturn AirportAth10kInterface::setMacAddress(mloAddrArray &addresses)
{
    return controller?controller->setHardwareAddress(&addresses,6):kIOReturnNotReady;
}
IOReturn AirportAth10kInterface::setPOWERSAVE(apple80211_powersave_data *pd)
{
    if(!pd)return kIOReturnBadArgument;
    return gated(OpPowerSave,pd->powersave_level!=APPLE80211_POWERSAVE_MODE_DISABLED?this:nullptr);
}

// --- Queries ----------------------------------------------------------------
static uint32_t bandFlags(unsigned channel){return channel>14?APPLE80211_C_FLAG_5GHZ:APPLE80211_C_FLAG_2GHZ;}
IOReturn AirportAth10kInterface::getCHANNEL(apple80211_channel_data *cd)
{
    if(!cd)return kIOReturnBadArgument;
    const auto *n=session?session->associated():nullptr;
    if(!n)return kIOReturnNotReady;
    const auto &s=session->core_();
    memset(cd,0,sizeof(*cd));cd->version=APPLE80211_VERSION;cd->channel.version=APPLE80211_VERSION;
    cd->channel.channel=s.channel?s.channel:n->channel;
    cd->channel.flags=bandFlags(cd->channel.channel)|APPLE80211_C_FLAG_ACTIVE|(s.width==80?APPLE80211_C_FLAG_80MHZ:s.width==40?APPLE80211_C_FLAG_40MHZ:APPLE80211_C_FLAG_20MHZ);
    return kIOReturnSuccess;
}
IOReturn AirportAth10kInterface::getPOWERSAVE(apple80211_powersave_data *pd)
{
    if(!pd)return kIOReturnBadArgument;
    pd->version=APPLE80211_VERSION;
    pd->powersave_level=(session && (session->core_().psBits&2))?APPLE80211_POWERSAVE_MODE_80211:APPLE80211_POWERSAVE_MODE_DISABLED;
    return kIOReturnSuccess;
}
IOReturn AirportAth10kInterface::getTXPOWER(apple80211_txpower_data *td)
{
    if(!td)return kIOReturnBadArgument;
    if(!session || !session->associated())return kIOReturnNotReady;
    memset(td,0,sizeof(*td));td->version=APPLE80211_VERSION;td->txpower_unit=APPLE80211_UNIT_DBM;
    td->txpower=static_cast<int32_t>(session->core_().txHalfDbm/2);return kIOReturnSuccess;
}
// The core reports width and streams, not the current MCS: the rate is the
// top PHY rate of the negotiated width/NSS (short GI), as Linux reports until
// firmware stats arrive.
IOReturn AirportAth10kInterface::getRATE(apple80211_rate_data *rd)
{
    if(!rd)return kIOReturnBadArgument;
    if(!session || !session->associated())return kIOReturnNotReady;
    const auto &s=session->core_();
    const uint32_t nss=s.nss?s.nss:1;
    uint32_t rate=s.width==80?433:(s.width==40?150:72);
    memset(rd,0,sizeof(*rd));rd->version=APPLE80211_VERSION;rd->num_radios=1;rd->rate[0]=rate*nss;
    return kIOReturnSuccess;
}
// The core reports width and streams, not the live MCS (as getRATE): the top
// index of the negotiated PHY (VHT 9, HT 7 per stream), which is what the
// menu's rate estimate derives from until firmware stats are exported.
IOReturn AirportAth10kInterface::getMCS(apple80211_mcs_data *md)
{
    if(!md)return kIOReturnBadArgument;
    const auto *n=session?session->associated():nullptr;
    if(!n)return kIOReturnNotReady;
    const auto &s=session->core_();
    const uint32_t nss=s.nss?s.nss:1;
    memset(md,0,sizeof(*md));md->version=APPLE80211_VERSION;
    md->index=n->vhtPresent?9:(7+8*(nss-1));
    return kIOReturnSuccess;
}
// VHT link details for the menu's option-click / wdutil (AirportItlwm getMCS_VHT,
// the one getter of its list that is still an IO80211InfraProtocol slot on
// macOS 26; SSID/BSSID/STATE/NOISE/... are answered by WCL from its own state).
// MCS as getMCS (top index of the negotiated PHY), NSS/width from the core,
// short GI when the AP advertises it for the width in use.
IOReturn AirportAth10kInterface::getMCS_VHT(apple80211_mcs_vht_data *data)
{
    if(!data)return kIOReturnBadArgument;
    const auto *n=session?session->associated():nullptr;
    if(!n || !n->vhtPresent || n->channel<=14)return kIOReturnNotReady;
    const auto &s=session->core_();
    const uint32_t width=s.width==80 || s.width==40?s.width:20;
    const uint32_t vhtInfo=AirportBridge::le32(n->vht),htInfo=n->htPresent?AirportBridge::le16(n->ht):0u;
    const bool sgi=width==80?(vhtInfo&0x20)!=0:(width==40?(htInfo&0x40)!=0:(htInfo&0x20)!=0);
    memset(data,0,sizeof(*data));data->version=APPLE80211_VERSION;
    data->index=9;data->nss=s.nss?s.nss:1;data->bw=width;data->guard_interval=sgi?APPLE80211_GI_SHORT:APPLE80211_GI_LONG;
    return kIOReturnSuccess;
}
IOReturn AirportAth10kInterface::getOP_MODE(apple80211_opmode_data *od)
{
    if(!od)return kIOReturnBadArgument;
    od->version=APPLE80211_VERSION;od->op_mode=APPLE80211_M_STA;return kIOReturnSuccess;
}
IOReturn AirportAth10kInterface::getRSSI(apple80211_rssi_data *rd)
{
    if(!rd)return kIOReturnBadArgument;
    if(!session || !session->associated())return kIOReturnNotReady;
    int32_t rssi=session->core_().rssi;
    if(rssi==INT32_MIN){const auto *n=session->associated();if(!n)return kIOReturnNotReady;rssi=n->rssi();}
    memset(rd,0,sizeof(*rd));rd->version=APPLE80211_VERSION;rd->num_radios=1;rd->rssi_unit=APPLE80211_UNIT_DBM;
    rd->rssi[0]=rd->aggregate_rssi=rd->rssi_ext[0]=rd->aggregate_rssi_ext=rssi;
    return kIOReturnSuccess;
}
IOReturn AirportAth10kInterface::getSUPPORTED_CHANNELS(apple80211_sup_channel_data *ad)
{
    if(!ad)return kIOReturnBadArgument;
    ad->version=APPLE80211_VERSION;ad->num_channels=0;
    for(const auto &c:AirportBridge::kChannels){
        if(ad->num_channels>=APPLE80211_MAX_CHANNELS)break;
        auto &e=ad->supported_channels[ad->num_channels++];
        e.version=APPLE80211_VERSION;e.channel=c.number;e.flags=bandFlags(c.number)|APPLE80211_C_FLAG_ACTIVE|APPLE80211_C_FLAG_20MHZ;
    }
    return kIOReturnSuccess;
}
IOReturn AirportAth10kInterface::getHW_SUPPORTED_CHANNELS(apple80211_sup_channel_data *ad){return getSUPPORTED_CHANNELS(ad);}
IOReturn AirportAth10kInterface::getCHANNELS_INFO(apple80211_channels_info *data)
{
    if(!data)return kIOReturnBadArgument;
    auto *info=reinterpret_cast<AirportWCL::NativeChannelInfo *>(data);
    memset(info,0,sizeof(*info));
    const bool dfsAllowed=session && session->core_().stationDFS;
    for(const auto &c:AirportBridge::kChannels){
        const bool dfs=AirportBridge::channelDFS(c.number);
        if(dfs && !dfsAllowed)continue;
        AirportWCL::appendChannel(*info,c.number,c.number>14,dfs,dfs,true,c.number>14);
    }
    if(session)memcpy(info->country,session->core_().country,2);
    return info->count?kIOReturnSuccess:kIOReturnNotReady;
}
IOReturn AirportAth10kInterface::getCOUNTRY_CHANNELS_INFO(apple80211_channels_info *data){return getCHANNELS_INFO(data);}
IOReturn AirportAth10kInterface::getWCL_CHANNELS_INFO(apple80211ChannelInfo *data)
{
    if(!data)return kIOReturnBadArgument;
    auto *info=reinterpret_cast<AirportWCL::ChannelInfo *>(data);
    memset(info,0,sizeof(*info));
    const bool dfsAllowed=session && session->core_().stationDFS;
    for(const auto &c:AirportBridge::kChannels){
        if(info->count>=400)break;
        const bool dfs=AirportBridge::channelDFS(c.number);
        if(dfs && !dfsAllowed)continue;
        auto &e=info->channels[info->count++];
        e.channel=c.number;e.channelSpec=AirportBridge::channelSpec(c.number);
        if(dfs)e.flags|=2|4;
    }
    if(session)memcpy(info->country,session->core_().country,2);
    return info->count?kIOReturnSuccess:kIOReturnNotReady;
}
IOReturn AirportAth10kInterface::getBEACON_INFO(apple80211_beacon_info_t *data){return data?gated(OpBeacon,data):kIOReturnBadArgument;}
IOReturn AirportAth10kInterface::getWCL_BSS_INFO(apple80211_beacon_msg *data){return data?gated(OpBssInfo,data):kIOReturnBadArgument;}
IOReturn AirportAth10kInterface::getNSS(apple80211_nss_data *data)
{
    if(!data)return kIOReturnBadArgument;
    memset(data,0,sizeof(*data));data->version=APPLE80211_VERSION;
    data->nss=static_cast<uint8_t>(session && session->core_().nss?session->core_().nss:1);
    return kIOReturnSuccess;
}
IOReturn AirportAth10kInterface::getHW_ADDR(apple80211_hw_mac_address *data)
{
    if(!data)return kIOReturnBadArgument;
    AirportWCL::HardwareAddress value;
    if(!AirportWCL::copyHardwareAddress(permanentMAC,value))return kIOReturnNotReady;
    memcpy(data,&value,sizeof(value));return kIOReturnSuccess;
}
IOReturn AirportAth10kInterface::getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info *data)
{
    if(!data)return kIOReturnBadArgument;
    memset(data,0,4);return kIOReturnSuccess;
}
IOReturn AirportAth10kInterface::getWCL_TRAFFIC_COUNTERS(apple80211_wcl_traffic_counters *data)
{
    if(!data)return kIOReturnBadArgument;
    uint64_t tx[4]{},rx[4]{};
    if(controller && controller->bsdInterface()){
        IONetworkData *nd=controller->bsdInterface()->getParameter(kIONetworkStatsKey);
        auto *stats=nd?static_cast<const IONetworkStats *>(nd->getBuffer()):nullptr;
        if(stats){tx[0]=stats->outputPackets;rx[0]=stats->inputPackets;}
    }
    uint64_t t=0,ns=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);
    const auto counters=AirportWCL::makeTrafficCounters(tx,rx,ns);
    memcpy(data,&counters,sizeof(counters));return kIOReturnSuccess;
}
IOReturn AirportAth10kInterface::fillRateSet(apple80211_rate_set_data *ad)
{
    const auto *n=session?session->associated():nullptr;
    if(!n)return kIOReturnNotReady;
    memset(ad,0,sizeof(*ad));ad->version=APPLE80211_VERSION;
    const unsigned count=n->rateCount<APPLE80211_MAX_RATES?n->rateCount:APPLE80211_MAX_RATES;
    ad->num_rates=static_cast<u_int16_t>(count);
    for(unsigned i=0;i<count;++i){ad->rates[i].version=APPLE80211_VERSION;ad->rates[i].rate=(n->rates[i]&0x7f)/2;ad->rates[i].flags=0;}
    return kIOReturnSuccess;
}
IOReturn AirportAth10kInterface::fillMCSSet(apple80211_mcs_index_set_data *ad)
{
    const auto *n=session?session->associated():nullptr;
    if(!n)return kIOReturnNotReady;
    memset(ad,0,sizeof(*ad));ad->version=APPLE80211_VERSION;
    if(n->htPresent)memcpy(ad->mcs_set_map,n->ht+3,sizeof(ad->mcs_set_map)<16?sizeof(ad->mcs_set_map):16); // HT capabilities IE: caps(2) ampdu(1) MCS set(16)
    return kIOReturnSuccess;
}
// Layout after AirportItlwm (macOS 26): rate set, HT MCS set at 0xbc, VHT map
// at 0xcc, HE version/map at 0xd4/0xd8, the negotiated RSN IE at 0x113.
// WCL reads it right after AssocComplete (setCurrentBSS), before the 4-way
// handshake: AirportItlwm answers once net80211 is in RUN (associated). Hardware
// run 6 answered NotReady until the port was authorized, WCL left with
// "Update Bss fail" and the join failed (statusCode 1, extendedCode 3).
IOReturn AirportAth10kInterface::getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *data)
{
    if(!data)return kIOReturnBadArgument;
    const auto *n=session?session->associated():nullptr;
    if(!n)return kIOReturnNotReady;
    static_assert(sizeof(apple80211_rate_set_data)==0xbc,"Extended BSS rates size");
    static_assert(sizeof(apple80211_mcs_index_set_data)==0x10,"Extended BSS MCS size");
    auto *bytes=reinterpret_cast<uint8_t *>(data);
    memset(bytes,0,0x214);
    IOReturn status=fillRateSet(reinterpret_cast<apple80211_rate_set_data *>(bytes));
    if(status)return status;
    status=fillMCSSet(reinterpret_cast<apple80211_mcs_index_set_data *>(bytes+0xbc));
    if(status)return status;
    apple80211_vht_mcs_index_set_data vht={};
    vht.version=APPLE80211_VERSION;vht.mcs_map=0xffff;
    if(n->vhtPresent)vht.mcs_map=static_cast<u_int16_t>(n->vht[8]|(n->vht[9]<<8)); // VHT capabilities: info(4) rx map(2) rx rate(2) tx map(2)
    memcpy(bytes+0xcc,&vht,sizeof(vht));
    const uint32_t heVersion=APPLE80211_VERSION;const uint16_t heMap=0xffff;
    memcpy(bytes+0xd4,&heVersion,sizeof(heVersion));memcpy(bytes+0xd8,&heMap,sizeof(heMap));
    if(n->rsnLength && n->rsnLength<=0x214-0x113){
        size_t rsnBytes=0;const uint8_t *rsn=AthSecurity::stationRSNForGroup(AthSecurity::wpa2GroupCipher(n->rsn,n->rsnLength),rsnBytes);
        if(rsn)memcpy(bytes+0x113,rsn,rsnBytes);
    }
    return kIOReturnSuccess;
}
