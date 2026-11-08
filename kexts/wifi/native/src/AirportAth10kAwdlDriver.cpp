// SPDX-License-Identifier: GPL-3.0-or-later
// awdl0 for AirportAth10k: see AirportAth10kAwdlDriver.hpp and docs/WIFI-AWDL.md.
#include "AirportAth10k.hpp"
#if AIRPORT_ATH10K_AWDL
#include "AirportAth10kAwdlDriver.hpp"
#include <IOKit/network/IONetworkMedium.h>
#include <kern/clock.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <netinet/in.h>
#include <sys/kpi_mbuf.h>

// IONetworkingFamily SPI, the same symbols the infrastructure interface binds
// (AirportAth10k.cpp): see there for what each does and how it was verified.
extern "C" IOReturn awdlConfigureOutputPullModel(IONetworkInterface *,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) __asm__("__ZN18IONetworkInterface24configureOutputPullModelEjjjjj");
extern "C" IOReturn awdlDequeueOutputPackets(IONetworkInterface *,uint32_t,mbuf_t *,mbuf_t *,uint32_t *,uint64_t *) __asm__("__ZN18IONetworkInterface20dequeueOutputPacketsEjPP6__mbufS2_PjPy");
extern "C" IOReturn awdlStartOutputThread(IONetworkInterface *,uint32_t) __asm__("__ZN18IONetworkInterface17startOutputThreadEj");
extern "C" IOReturn awdlStopOutputThread(IONetworkInterface *,uint32_t) __asm__("__ZN18IONetworkInterface16stopOutputThreadEj");
extern "C" bool awdlSetInterfaceSubType(IONetworkInterface *,uint32_t) __asm__("__ZN18IONetworkInterface19setInterfaceSubTypeEj");
// IO80211Family 1200.13.1: tells the controller which interface is the AWDL one.
extern "C" void awdlSetAwdlInterface(IO80211Controller *,IO80211VirtualInterface *) __asm__("__ZN17IO80211Controller16setAwdlInterfaceEP23IO80211VirtualInterface");
// IO80211Family 1200.13.1: the path AppleBCMWLANProximityInterface::handleActionFrame_rx uses to hand a received
// AWDL action frame to the family's peer manager (DLT 12), copying the bytes into a family buffer. Read from the
// 26.7.2 collection; docs/WIFI-AWDL.md section 5d.
extern "C" void awdlBpfTapInputMbuf(IO80211VirtualInterface *,mbuf_t,unsigned int,void *,unsigned long) __asm__("__ZN23IO80211VirtualInterface11bpfTapInputEP6__mbufjPvm");
// xnu bsd/netinet6/nd6.c (the AWDL path): a peer's neighbor entry comes from its AWDL address, not from
// Neighbor Discovery over the air (Owly SPEC NET-2). Called by the family's peer manager on a Mac.
extern "C" void awdlNd6AltNodePresent(ifnet_t,struct sockaddr_in6 *,struct sockaddr_dl *,int32_t,int,int) __asm__("_nd6_alt_node_present");
extern "C" void awdlNd6AltNodeAbsent(ifnet_t,struct sockaddr_in6 *,struct sockaddr_dl *) __asm__("_nd6_alt_node_absent");
extern "C" void awdlSetLinkState(IO80211VirtualInterface *,int,unsigned int) __asm__("__ZN23IO80211VirtualInterface12setLinkStateE16IO80211LinkStatej");

static constexpr uint32_t kAwdlOutputQueue=64;

// ---------------------------------------------------------------- AirportAth10kAwdlInterface
#undef super
#define super IO80211AWDLProtocol
OSDefineMetaClassAndStructors(AirportAth10kAwdlInterface,IO80211AWDLProtocol)

bool AirportAth10kAwdlInterface::initWith(AirportAth10kAwdlNet *owner,AirportAth10k *c,const uint8_t *mac)
{
    if(!IO80211VirtualInterface::init(c,reinterpret_cast<ether_addr *>(const_cast<uint8_t *>(mac)),6,"awdl"))return false;
    net=owner;controller=c;memcpy(macAddress,mac,6);
    port.owner=this;
    PE_parse_boot_argn("ath10k_awdl_stack",&stack,sizeof(stack));
    lastChannelSequence=static_cast<apple80211_awdl_sync_channel_sequence *>(IOMallocZeroData(sizeof(*lastChannelSequence)));
    awdlSession=new AirportAwdl::Session(port,port);
    if(!awdlSession)return false;
    timer=IOTimerEventSource::timerEventSource(this,pollTimer);
    IO80211WorkQueue *queue=controller->getWorkQueue();
    if(!timer || !queue || queue->addEventSource(timer)!=kIOReturnSuccess)return false;
    return true;
}
void AirportAth10kAwdlInterface::stop(IOService *provider)
{
    stopSession();
    releaseRegistrationInfo();
    super::stop(provider);
}
void AirportAth10kAwdlInterface::free()
{
    stopSession();
    if(awdlSession){delete awdlSession;awdlSession=nullptr;}
    if(lastChannelSequence){IOFreeData(lastChannelSequence,sizeof(*lastChannelSequence));lastChannelSequence=nullptr;}
    releaseRegistrationInfo();
    super::free();
}
bool AirportAth10kAwdlInterface::installRegistrationInfo(const IOSkywalkEthernetInterface::RegistrationInfo &info)
{
    if(!mExpansionData || !mExpansionData2)return false;
    return registration.install(reinterpret_cast<void **>(&mExpansionData->fRegistrationInfo),reinterpret_cast<void **>(&mExpansionData2->fRegistrationInfo),&info,sizeof(info),
                                [](size_t n){return IOMalloc(n);},[](void *p,size_t n){IOFree(p,n);});
}
void AirportAth10kAwdlInterface::releaseRegistrationInfo()
{
    registration.release(mExpansionData?reinterpret_cast<void **>(&mExpansionData->fRegistrationInfo):nullptr,
                         mExpansionData2?reinterpret_cast<void **>(&mExpansionData2->fRegistrationInfo):nullptr,[](void *p,size_t n){IOFree(p,n);});
}
uint64_t AirportAth10kAwdlInterface::nowUs() const
{
    uint64_t t=0,ns=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);return ns/1000u;
}
void AirportAth10kAwdlInterface::startSession()
{
    if(!awdlSession || !timer)return;
    stopping=false;
    uint32_t channel=6;
    PE_parse_boot_argn("ath10k_awdl_ch",&channel,sizeof(channel));
    awdlSession->configure("helios300",macAddress,static_cast<uint8_t>(channel));
    awdlSession->enable(nowUs());
    timer->setTimeoutMS(1);
}
void AirportAth10kAwdlInterface::stopSession()
{
    stopping=true;
    if(awdlSession)awdlSession->disable();
    if(timer){
        timer->cancelTimeout();
        if(timer->getWorkLoop())timer->getWorkLoop()->removeEventSource(timer);
        OSSafeReleaseNULL(timer);
    }
}
void AirportAth10kAwdlInterface::kick()
{
    if(timer && !stopping)timer->setTimeoutUS(1);
}
void AirportAth10kAwdlInterface::pollTimer(OSObject *owner,IOTimerEventSource *)
{
    auto *self=OSDynamicCast(AirportAth10kAwdlInterface,owner);
    if(!self || self->stopping || !self->awdlSession)return;
    const uint64_t now=self->nowUs(),next=self->awdlSession->poll(now);
    self->mirror();
    if(self->timer && !self->stopping){
        const uint64_t delay=next>now?next-now:1;
        self->timer->setTimeoutUS(static_cast<uint32_t>(delay<1000?1000:(delay>100000?100000:delay)));
    }
}
void AirportAth10kAwdlInterface::mirror()
{
    if(!net || net->isInactive())return;
    const uint32_t state=static_cast<uint32_t>(awdlSession->state());
    const uint32_t peers=static_cast<uint32_t>(awdlSession->peerCount());
    if(state!=mirroredState){
        mirroredState=state;
        net->setProperty("AirportAth10kAwdlState",state,32);
        const bool running=awdlSession->running();
        net->setLinkUp(running);
    }
    if(peers!=mirroredPeers){mirroredPeers=peers;net->setProperty("AirportAth10kAwdlPeers",peers,32);}
    const uint8_t placement=static_cast<uint8_t>(awdlSession->placement());
    if(placement!=mirroredPlacement){mirroredPlacement=placement;net->setProperty("AirportAth10kAwdlPlacement",placement,32);}
    if(ioctlDirty){ioctlDirty=false;net->setProperty("AirportAth10kAwdlIoctls",ioctlCalls,sizeof(ioctlCalls));}
    if(++mirrorTicks%16==0){
        net->setProperty("AirportAth10kAwdlFedFrames",afFed,32);net->setProperty("AirportAth10kAwdlFedDrops",afFedDropped,32);
        net->setProperty("AirportAth10kAwdlOob",oobCalls,32);net->setProperty("AirportAth10kAwdlOobTx",oobTx,32);
        net->setProperty("AirportAth10kAwdlEchoes",awdlSession->stats().echoes,32);
    }
}

bool AirportAth10kAwdlInterface::Port::control(uint32_t op,const void *data,uint32_t length,AthControl::Response *out)
{
    AthControl::Request request;AthControl::Response response;
    request.operation=op;request.length=length;
    if(length)memcpy(request.data,data,length);
    Ath10kPCIDevice *core=owner->controller?owner->controller->core():nullptr;
    if(!core)return false;
    const IOReturn r=core->controlRequest(owner,&request,out?out:&response);
    return r==kIOReturnSuccess;
}
bool AirportAth10kAwdlInterface::Port::radioStart(const uint8_t *mac,uint8_t channel,bool shared)
{
    uint8_t data[8];memcpy(data,mac,6);data[6]=channel;data[7]=shared?1:0;return control(AthControl::AwdlRadioStart,data,8,nullptr);
}
// The station link, as the core reports it (AwdlStatus: joined u8, channel u8 at offset 39).
void AirportAth10kAwdlInterface::Port::radioStation(AirportAwdl::StationInfo &out)
{
    AthControl::Response response;out=AirportAwdl::StationInfo{};
    if(!control(AthControl::AwdlStatus,nullptr,0,&response) || response.length<41)return;
    out.associated=response.data[39]!=0;out.channel=response.data[40];
}
void AirportAth10kAwdlInterface::Port::hostActionFrame(const uint8_t *frame,size_t length,uint8_t channel,int8_t rssi)
{
    owner->feedPeerManager(frame,length,channel,rssi);
}
void AirportAth10kAwdlInterface::Port::radioStop(){control(AthControl::AwdlRadioStop,nullptr,0,nullptr);}
bool AirportAth10kAwdlInterface::Port::radioSetChannel(uint8_t channel){return control(AthControl::AwdlSetChannel,&channel,1,nullptr);}
AirportAwdl::RadioStep AirportAth10kAwdlInterface::Port::radioStep()
{
    AthControl::Response response;
    if(!control(AthControl::AwdlStatus,nullptr,0,&response))return AirportAwdl::RadioStep::Failed;
    return static_cast<AirportAwdl::RadioStep>(response.data[0]);
}
bool AirportAth10kAwdlInterface::Port::radioTransmit(const uint8_t *frame,size_t length,bool action)
{
    Ath10kPCIDevice *core=owner->controller?owner->controller->core():nullptr;
    return core && core->nativeAwdlTx(frame,static_cast<UInt32>(length),action);
}
bool AirportAth10kAwdlInterface::Port::radioReceive(uint8_t *buffer,size_t capacity,AirportAwdl::RxFrame &out)
{
    Ath10kPCIDevice *core=owner->controller?owner->controller->core():nullptr;
    UInt32 length=0;UInt8 kind=0,channel=0;SInt8 rssi=0;UInt64 timeUs=0;
    if(!core || !core->nativeAwdlRx(buffer,static_cast<UInt32>(capacity),&length,&kind,&channel,&rssi,&timeUs))return false;
    out.data=buffer;out.length=length;out.rssi=rssi;out.channel=channel;out.data_=kind==2;out.timeUs=timeUs;
    return true;
}
void AirportAth10kAwdlInterface::Port::hostEthernet(const uint8_t *eth,size_t length){if(owner->net)owner->net->inputEthernet(eth,length);}
void AirportAth10kAwdlInterface::Port::hostPeer(const AirportAwdl::Peer &peer,bool added)
{
    AIRPORT_LOG("awdl: peer %s %02x:%02x:%02x:%02x:%02x:%02x name '%s' rssi %d",added?"added":"removed",peer.addr.b[0],peer.addr.b[1],peer.addr.b[2],
                peer.addr.b[3],peer.addr.b[4],peer.addr.b[5],peer.name,peer.rssi);
    if(owner->stack&kAwdlStackNeighbors)owner->neighbor(peer,added);
}
// Metadata the family reads next to an AWDL action frame; layout from AppleBCMWLANProximityInterface::handleActionFrame_rx
// and IO80211AWDLPeerManager::isValidAF on 26.7.2 (size checked there as >= 0x48): source address at 0, the channel at
// 0x0c as an apple80211_channel, rate at 0x28, RSSI in dBm at 0x2c, MAC time at 0x30. The same layout Bananz0 found on
// the older family (https://github.com/Bananz0/AirportItlwm-Tahoe-AWDL, GPL-2.0-or-later).
struct AwdlActionFrameMeta {uint8_t source[6];uint8_t pad0[6];apple80211_channel channel;uint8_t pad1[0x10];uint32_t rate;int32_t rssi;uint32_t macTime;uint8_t pad2[0x14];} __attribute__((packed));
static_assert(sizeof(AwdlActionFrameMeta)==0x48 && offsetof(AwdlActionFrameMeta,channel)==0x0c && offsetof(AwdlActionFrameMeta,rate)==0x28 && offsetof(AwdlActionFrameMeta,rssi)==0x2c,"AWDL action frame metadata layout");
void AirportAth10kAwdlInterface::feedPeerManager(const uint8_t *frame,size_t length,uint8_t channel,int8_t rssi)
{
    if(!(stack&kAwdlStackPeerManagerRx) || length<=24)return;
    if(length>AirportAwdl::kMaxFrame){++afFedDropped;return;}
    mbuf_t m=nullptr;unsigned int chunks=1;
    if(mbuf_allocpacket(MBUF_DONTWAIT,length-24,&chunks,&m) || !m){++afFedDropped;return;}
    if(chunks!=1 || mbuf_setdata(m,mbuf_datastart(m),length-24)){mbuf_freem(m);++afFedDropped;return;}
    memcpy(mbuf_data(m),frame+24,length-24);mbuf_pkthdr_setlen(m,length-24); // the body, starting at the action category
    AwdlActionFrameMeta meta;bzero(&meta,sizeof(meta));
    memcpy(meta.source,frame+10,6);meta.channel.version=APPLE80211_VERSION;meta.channel.channel=channel;
    meta.channel.flags=(channel<=14?APPLE80211_C_FLAG_2GHZ:APPLE80211_C_FLAG_5GHZ)|APPLE80211_C_FLAG_ACTIVE|APPLE80211_C_FLAG_20MHZ;
    meta.rssi=rssi;
    awdlBpfTapInputMbuf(this,m,12,&meta,sizeof(meta)); // the family copies; the mbuf stays ours
    mbuf_freem(m);
    ++afFed;
}
// NET-2: fe80:: from the peer's AWDL address, neighbor entry from that address rather than from NDP.
void AirportAth10kAwdlInterface::neighbor(const AirportAwdl::Peer &peer,bool present)
{
    if(!net || !net->bsdInterface() || !net->bsdInterface()->getIfnet())return;
    ifnet_t ifp=net->bsdInterface()->getIfnet();
    struct sockaddr_in6 sin6;bzero(&sin6,sizeof(sin6));
    sin6.sin6_len=sizeof(sin6);sin6.sin6_family=AF_INET6;
    AirportAwdl::Session::ipv6LinkLocal(peer.addr,reinterpret_cast<uint8_t *>(&sin6.sin6_addr));
    struct sockaddr_dl sdl;bzero(&sdl,sizeof(sdl));
    sdl.sdl_len=sizeof(sdl);sdl.sdl_family=AF_LINK;sdl.sdl_index=ifnet_index(ifp);sdl.sdl_type=IFT_ETHER;sdl.sdl_nlen=0;sdl.sdl_alen=6;
    memcpy(LLADDR(&sdl),peer.addr.b,6);
    if(present)awdlNd6AltNodePresent(ifp,&sin6,&sdl,peer.rssi,50 /* IFNET_LQM_THRESH_GOOD */,0);
    else awdlNd6AltNodeAbsent(ifp,&sin6,&sdl);
}
void AirportAth10kAwdlInterface::Port::hostSyncChanged(){}

// ioctls whose layouts are known (reconstructed apple80211_ioctl.h) answered from the
// engine. Everything else stays Unsupported rather than reading a guessed layout.
#define AWDL_ARG(d) do{if(!(d))return kIOReturnBadArgument;}while(0)
#define AWDL_NOTE(n) do{if((n)<sizeof(ioctlCalls)/sizeof(ioctlCalls[0])){++ioctlCalls[n];ioctlDirty=true;}}while(0)
IOReturn AirportAth10kAwdlInterface::getSSID(apple80211_ssid_data *d){AWDL_ARG(d);d->version=APPLE80211_VERSION;d->ssid_len=4;memcpy(d->ssid_bytes,"awdl",4);return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::setSSID(apple80211_ssid_data *){return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::getBSSID(apple80211_bssid_data *d){AWDL_ARG(d);d->version=APPLE80211_VERSION;memcpy(&d->bssid,AirportAwdl::kAwdlBssid,6);return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::getSTATE(apple80211_state_data *d){AWDL_ARG(d);d->version=APPLE80211_VERSION;d->state=awdlSession && awdlSession->running()?APPLE80211_S_RUN:APPLE80211_S_INIT;return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::getCHANNEL(apple80211_channel_data *d)
{
    AWDL_ARG(d);if(!awdlSession)return kIOReturnNotReady;
    bzero(&d->channel,sizeof(d->channel));d->version=APPLE80211_VERSION;
    const uint8_t c=awdlSession->currentChannel();d->channel.channel=c?c:awdlSession->masterChannel();
    d->channel.flags=(d->channel.channel<=14?APPLE80211_C_FLAG_2GHZ:APPLE80211_C_FLAG_5GHZ)|APPLE80211_C_FLAG_20MHZ;
    return kIOReturnSuccess;
}
IOReturn AirportAth10kAwdlInterface::setCHANNEL(apple80211_channel_data *){return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *d)
{
    AWDL_ARG(d);if(!awdlSession)return kIOReturnNotReady;
    d->version=APPLE80211_VERSION;
    const auto &sync=awdlSession->engine().state.sync;
    d->availability_window_length=sync.awPeriod;d->availability_window_period=sync.awPeriod;
    d->extension_length=uint32_t(sync.presenceMode)*sync.awPeriod;
    d->synchronization_frame_period=awdlSession->engine().state.psfInterval;return kIOReturnSuccess;
}
IOReturn AirportAth10kAwdlInterface::setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *){return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *d){AWDL_ARG(d);d->version=APPLE80211_VERSION;d->unk1=0;d->enabled=syncEnabled;return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *d){AWDL_ARG(d);syncEnabled=d->enabled;return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::getAWDL_SYNC_STATE(apple80211_awdl_sync_state *d){AWDL_ARG(d);d->version=APPLE80211_VERSION;d->state=awdlSession && awdlSession->running()?1:0;return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::setAWDL_SYNC_STATE(apple80211_awdl_sync_state *){return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::getAWDL_ELECTION_ID(apple80211_awdl_election_id *d){AWDL_ARG(d);d->version=APPLE80211_VERSION;d->election_id=electionId;return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::setAWDL_ELECTION_ID(apple80211_awdl_election_id *d){AWDL_ARG(d);electionId=d->election_id;return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *d)
{AWDL_ARG(d);if(!awdlSession)return kIOReturnNotReady;d->version=APPLE80211_VERSION;d->metric=awdlSession->engine().state.election.selfMetric;return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *){return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::setAWDL_BSSID(apple80211_awdl_bssid *d){AWDL_ARG(d);bssidValid=!memcmp(d->bssid,AirportAwdl::kAwdlBssid,6);return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *d)
{AWDL_ARG(d);if(!awdlSession)return kIOReturnNotReady;d->version=APPLE80211_VERSION;d->master_channel=awdlSession->masterChannel();return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *){return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *d)
{AWDL_ARG(d);if(!awdlSession)return kIOReturnNotReady;d->version=APPLE80211_VERSION;d->mode=awdlSession->engine().state.sync.presenceMode;return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *){return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlInterface::getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size *d){AWDL_ARG(d);d->version=APPLE80211_VERSION;d->max_peers=AirportAwdl::kMaxPeers;return kIOReturnSuccess;}
// The family's chosen sequence: stored and returned (this node's own schedule stays the engine's, docs/WIFI-AWDL.md).
IOReturn AirportAth10kAwdlInterface::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *d)
{
    AWDL_NOTE(0);AWDL_ARG(d);
    if(lastChannelSequence){memcpy(lastChannelSequence,d,sizeof(*lastChannelSequence));ioctlDirty=true;}
    return kIOReturnSuccess;
}
IOReturn AirportAth10kAwdlInterface::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *d)
{
    AWDL_NOTE(1);AWDL_ARG(d);
    if(!lastChannelSequence || !lastChannelSequence->length)return kIOReturnUnsupported;
    memcpy(d,lastChannelSequence,sizeof(*d));return kIOReturnSuccess;
}
// Apple's peer manager sends what firmware would transmit (out-of-band action frames) through these: the records
// (IO80211AWDLPeerManager::updatePrimaryPayloadMI) go in our MIFs, one-shot unicast frames go out as they are.
// Layout read from AppleBCMWLANProximityInterface::setAWDL_OOB_REQUEST on 26.7.2: destination at +0x16, length (u16) at
// +0x28, the frame pointer at +0x30 (older families carried the bytes inline; apple80211_ioctl.h still says so).
IOReturn AirportAth10kAwdlInterface::handleOob(apple80211_awdl_oob_request *d,bool autoRequest)
{
    AWDL_ARG(d);if(!awdlSession)return kIOReturnNotReady;
    ++oobCalls;
    const uint8_t *r=reinterpret_cast<const uint8_t *>(d);
    const uint16_t length=uint16_t(r[0x28])|(uint16_t(r[0x29])<<8);
    if(!length)return kIOReturnSuccess;
    if(length>AirportAwdl::kMaxFrame-24)return kIOReturnBadArgument;
    uint64_t pointer=0;memcpy(&pointer,r+0x30,sizeof(pointer));
    const uint8_t *body=nullptr;
    if((pointer>>48)==0xffff)body=reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(pointer));
    else if(length<=sizeof(d->data))body=r+0x30;
    else return kIOReturnBadArgument;
    if(stack&kAwdlStackServiceTlvs)awdlSession->setServiceTlvsFromFrame(body,length);
    const uint8_t *dst=r+0x16;
    if(!autoRequest && !(dst[0]&1) && awdlSession->running()){
        uint8_t frame[AirportAwdl::kMaxFrame];
        AirportAwdl::Writer w(frame,sizeof(frame));AirportAwdl::Ieee80211State &seq=awdlSession->engine().ieee80211;
        AirportAwdl::initAwdlHeader(w,AirportAwdl::Addr::of(macAddress),AirportAwdl::Addr::of(dst),seq,AirportAwdl::kFcMgmtAction);
        w.bytes(body,length);
        if(!w.overflow && port.radioTransmit(frame,w.len,true))++oobTx;
    }
    return kIOReturnSuccess;
}
IOReturn AirportAth10kAwdlInterface::setAWDL_OOB_REQUEST(apple80211_awdl_oob_request *d){AWDL_NOTE(2);return handleOob(d,false);}
IOReturn AirportAth10kAwdlInterface::setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request *d){AWDL_NOTE(3);return handleOob(d,true);}
// Accepted without any effect beyond the call count, which lands in the registry (AirportAth10kAwdlIoctls) so a real
// run shows what wifip2pd and the peer manager actually use. These configure firmware behaviour this host-side MAC does
// differently (election, schedule, rate) or does not have (encryption, DFS proxy, real-time groups).
#define AWDL_ACCEPT(name,type,index) IOReturn AirportAth10kAwdlInterface::name(type *d){AWDL_NOTE(index);AWDL_ARG(d);return kIOReturnSuccess;}
#define AWDL_REFUSE(name,type,index) IOReturn AirportAth10kAwdlInterface::name(type *){AWDL_NOTE(index);return kIOReturnUnsupported;}
AWDL_ACCEPT(setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS,apple80211_awdl_extension_state_machine_parameter,4)
AWDL_ACCEPT(setAWDL_ELECTION_RSSI_THRESHOLDS,apple80211_awdl_election_rssi_thresholds,5)
AWDL_ACCEPT(setPEER_CACHE_CONTROL,apple80211_peer_cache_control,6)
AWDL_ACCEPT(setAWDL_SYNC_FRAME_TEMPLATE,apple80211_awdl_sync_frame_template,7)
AWDL_ACCEPT(setAWDL_ELECTION_PARAMS,apple80211_awdl_election_params,8)
AWDL_ACCEPT(setAWDL_PEER_TRAFFIC_REGISTRATION,apple80211_awdl_peer_traffic_registration,9)
AWDL_ACCEPT(setAWDL_SECONDARY_MASTER_CHANNEL,apple80211_awdl_secondary_master_channel,10)
AWDL_ACCEPT(setAWDL_AF_TX_MODE,apple80211_awdl_af_tx_mode,11)
AWDL_ACCEPT(setAWDL_MIN_RATE,apple80211_awdl_min_rate,12)
AWDL_ACCEPT(setAWDL_ENCRYPTION_KEYS,apple80211_key,13)
AWDL_ACCEPT(setAWDL_ENCRYPTION_TYPE,apple80211_encrypt,14)
AWDL_ACCEPT(setAWDL_TXCAL_PERIOD,apple80211_awdl_txcal_period,15)
AWDL_ACCEPT(setAWDL_AWDL_ADVERTISERS,awdl_peer_advert_add,16)
AWDL_ACCEPT(setAWDL_DFSP_CONFIG,apple80211_awdl_dfsp_config,17)
AWDL_ACCEPT(setAWDL_DFSP_UCSA_CONFIG,apple80211_awdl_dfsp_ucsa_config,18)
AWDL_ACCEPT(setAWDL_OOB_SEC_PAYLOAD_REQUEST,apple80211_awdl_af_sec_payload_request,19)
AWDL_ACCEPT(setLLW_PARAMS,apple80211_llw_params,20)
AWDL_ACCEPT(setTX_RATE,apple80211_tx_rate,21)
AWDL_ACCEPT(setP2P_COEX_PROTECTION,apple80211_p2p_coex_protection,22)
AWDL_ACCEPT(setPSF_DWELL,apple80211_awdl_psf_dwell_params,23)
AWDL_ACCEPT(setAWDL_CONFIG,apple80211_awdl_config_request,24)
AWDL_ACCEPT(setAWDL_RTG_PEER_STATS_CONFIG,apple80211_rtg_peer_stats_cfg,25)
// Not implemented: another virtual interface is not offered, and these read back state the host-side MAC does not keep.
AWDL_REFUSE(setCOMPANION_SKYWALK_IF_CREATE,apple80211_companion_skywalk_if_create,26)
AWDL_REFUSE(getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS,apple80211_awdl_extension_state_machine_parameter,27)
AWDL_REFUSE(getAWDL_ELECTION_RSSI_THRESHOLDS,apple80211_awdl_election_rssi_thresholds,28)
AWDL_REFUSE(getAWDL_ELECTION_PARAMS,apple80211_awdl_election_params,29)
AWDL_REFUSE(getAWDL_DEVICE_CAPABILITIES,apple80211_awdl_device_capabilities,30)
AWDL_REFUSE(getAWDL_AF_TX_MODE,apple80211_awdl_af_tx_mode,31)
AWDL_REFUSE(getAWDL_MIN_RATE,apple80211_awdl_min_rate,32)
AWDL_REFUSE(getAWDL_ENCRYPTION_TYPE,apple80211_encrypt,33)
AWDL_REFUSE(getAWDL_TXCAL_PERIOD,apple80211_awdl_txcal_period,34)
AWDL_REFUSE(getAWDL_DEV_STATS,apple80211_awdl_dev_stats,35)
AWDL_REFUSE(getAWDL_DFSP_CONFIG,apple80211_awdl_dfsp_config,36)
AWDL_REFUSE(getAWDL_CCA,apple80211_awdl_cca,37)
AWDL_REFUSE(getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES,apple80211_awdl_cap,38)
AWDL_REFUSE(getLLW_PARAMS,apple80211_llw_params,39)
IOReturn AirportAth10kAwdlInterface::getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *d)
{AWDL_NOTE(40);AWDL_ARG(d);d->version=APPLE80211_VERSION;d->secondary_master_channel=0;return kIOReturnSuccess;}

// ---------------------------------------------------------------- BSD interface (awdl0)
#undef super
#define super IOEthernetInterface
OSDefineMetaClassAndStructors(AirportAth10kAwdlBsd,IOEthernetInterface)

bool AirportAth10kAwdlBsd::initWithSkywalkInterface(IONetworkController *c,IO80211SkywalkInterface *s)
{
    if(!super::init(c))return false;
    interface=s;attached=false;return true;
}
IOReturn AirportAth10kAwdlBsd::prepareGated(OSObject *,void *arg0,void *,void *,void *)
{
    auto *self=static_cast<AirportAth10kAwdlBsd *>(arg0);
    auto *awdl=OSDynamicCast(IO80211VirtualInterface,self->interface);
    if(!awdl)return kIOReturnNotPermitted;
    IOReturn r=awdl->prepareBSDInterface(self->getIfnet(),0);
    if(r!=kIOReturnSuccess)return r;
    awdl->peerToPeerConfigureIfnet(); // IFEF_AWDL + the peer-to-peer ifnet parameters
    return kIOReturnSuccess;
}
IOReturn AirportAth10kAwdlBsd::attachToDataLinkLayer(IOOptionBits options,void *parameter)
{
    if(!awdlSetInterfaceSubType(this,3)){AIRPORT_LOG("awdl: setInterfaceSubType failed");return kIOReturnNotReady;} // IFNET_SUBFAMILY_WIFI
    IOReturn ret=super::attachToDataLinkLayer(options,parameter);
    if(ret!=kIOReturnSuccess || !interface)return ret;
    char name[IFNAMSIZ];snprintf(name,sizeof(name),"%s%u",ifnet_name(getIfnet()),ifnet_unit(getIfnet()));
    if(OSString *v=OSString::withCString(name)){interface->setProperty("IOInterfaceName",v);v->release();}
    auto *net=OSDynamicCast(AirportAth10kAwdlNet,getController());
    IOCommandGate *gate=net?net->commandGate():nullptr;
    ret=gate?gate->runAction(prepareGated,this):kIOReturnNotReady;
    if(ret!=kIOReturnSuccess){AIRPORT_LOG("awdl: prepareBSDInterface failed 0x%x",ret);super::detachFromDataLinkLayer(options,parameter);return ret;}
    attached=true;
    interface->registerService();
    return kIOReturnSuccess;
}
void AirportAth10kAwdlBsd::detachFromDataLinkLayer(IOOptionBits options,void *parameter)
{
    super::detachFromDataLinkLayer(options,parameter);attached=false;
}
IOService *AirportAth10kAwdlBsd::getProvider() const {return attached?interface:super::getProvider();}

// ---------------------------------------------------------------- the network controller
#undef super
#define super IOEthernetController
OSDefineMetaClassAndStructors(AirportAth10kAwdlNet,IOEthernetController)

bool AirportAth10kAwdlNet::initWith(AirportAth10k *c)
{
    if(!super::init(nullptr))return false;
    controller=c;
    IOEthernetAddress hw;
    if(!c || c->getHardwareAddress(&hw)!=kIOReturnSuccess)return false;
    // awdl0's address is the station's MAC (AWDL-2; shared mode needs the station's MAC so its receive path ACKs the peer's
    // unicast, and the address must not change while the node is visible, so it is the same in every placement).
    memcpy(mac,hw.bytes,6);
    return true;
}
IOWorkLoop *AirportAth10kAwdlNet::getWorkLoop() const {return controller?controller->getWorkLoop():nullptr;}
bool AirportAth10kAwdlNet::start(IOService *provider)
{
    if(!super::start(provider))return false;
    netStarted=true;
    AIRPORT_STEP("awdl: controller started");
    awdl=new AirportAth10kAwdlInterface;
    if(!awdl || !awdl->initWith(this,controller,mac)){AIRPORT_LOG("awdl: interface init failed");return false;}
    awdl->setInterfaceId(2);
    if(!awdl->attach(this)){AIRPORT_LOG("awdl: attach failed");return false;}
    gate=IOCommandGate::commandGate(this);
    if(!gate || getWorkLoop()->addEventSource(gate)!=kIOReturnSuccess){AIRPORT_LOG("awdl: no command gate");return false;}
    IOSkywalkEthernetInterface::RegistrationInfo info;memset(&info,0,sizeof(info));
    if(!awdl->initRegistrationInfo(&info,1,sizeof(info)) || !awdl->installRegistrationInfo(info)){AIRPORT_LOG("awdl: registration info failed");return false;}
    awdl->deferBSDAttach(true);
    if(!awdl->start(this)){AIRPORT_LOG("awdl: Skywalk start failed");return false;}
    interfaceStarted=true;
    OSDictionary *media=OSDictionary::withCapacity(1);
    IONetworkMedium *m=IONetworkMedium::medium(kIOMediumIEEE80211,54000000);
    const bool mediaOk=media && m && IONetworkMedium::addMedium(media,m) && publishMediumDictionary(media) && setCurrentMedium(m) && setSelectedMedium(m);
    activeMedium=m;
    if(m)m->release();
    if(media)media->release();
    if(!mediaOk){AIRPORT_LOG("awdl: medium tables failed");return false;}
    if(!attachInterface(reinterpret_cast<IONetworkInterface **>(&bsd),false) || !bsd){AIRPORT_LOG("awdl: BSD interface attach failed");return false;}
    attached=true;
    awdlSetAwdlInterface(controller,awdl);
    bsd->registerService();
    registerService();
    up=true;
    setLinkStatus(kIONetworkLinkValid,nullptr,0);
    awdl->startSession();
    AIRPORT_STEP("awdl: awdl0 started");
    return true;
}
void AirportAth10kAwdlNet::stop(IOService *provider)
{
    if(awdl)awdl->stopSession();
    if(bsd && threadRunning){awdlStopOutputThread(bsd,0);threadRunning=false;}
    if(attached && bsd){detachInterface(bsd,true);attached=false;}
    if(awdl){
        if(interfaceStarted){awdl->stop(this);interfaceStarted=false;}
        if(awdl->getProvider()==this)awdl->detach(this);
    }
    OSSafeReleaseNULL(awdl);
    if(gate){getWorkLoop()->removeEventSource(gate);OSSafeReleaseNULL(gate);}
    if(netStarted){super::stop(provider);netStarted=false;}
    up=false;
}
void AirportAth10kAwdlNet::free()
{
    OSSafeReleaseNULL(awdl);
    super::free();
}
IONetworkInterface *AirportAth10kAwdlNet::createInterface()
{
    auto *i=new AirportAth10kAwdlBsd;
    if(!i)return nullptr;
    if(!i->initWithSkywalkInterface(this,awdl)){i->release();return nullptr;}
    return i;
}
bool AirportAth10kAwdlNet::configureInterface(IONetworkInterface *interface)
{
    if(!super::configureInterface(interface))return false;
    interface->setProperty("IOModel","Qualcomm Atheros QCA6174 AWDL (ath10k)");
    const IOReturn r=awdlConfigureOutputPullModel(interface,kAwdlOutputQueue,1 /* kIONetworkWorkLoopSynchronous */,0,0,0);
    if(r!=kIOReturnSuccess){AIRPORT_LOG("awdl: configureOutputPullModel failed 0x%x",r);return false;}
    return true;
}
IOReturn AirportAth10kAwdlNet::getHardwareAddress(IOEthernetAddress *a){memcpy(a->bytes,mac,6);return kIOReturnSuccess;}
IOReturn AirportAth10kAwdlNet::setHardwareAddress(const void *,UInt32){return kIOReturnUnsupported;}
IOReturn AirportAth10kAwdlNet::getPacketFilters(const OSSymbol *group,UInt32 *filters) const
{
    if(group==gIONetworkFilterGroup){*filters=kIOPacketFilterMulticast;return kIOReturnSuccess;}
    return IOEthernetController::getPacketFilters(group,filters);
}
IOReturn AirportAth10kAwdlNet::enable(IONetworkInterface *i)
{
    if(!threadRunning){const IOReturn r=awdlStartOutputThread(i,0);if(r==kIOReturnSuccess)threadRunning=true;else AIRPORT_LOG("awdl: startOutputThread 0x%x",r);}
    return kIOReturnSuccess;
}
IOReturn AirportAth10kAwdlNet::disable(IONetworkInterface *i)
{
    if(threadRunning){awdlStopOutputThread(i,0);threadRunning=false;}
    return kIOReturnSuccess;
}
UInt32 AirportAth10kAwdlNet::outputPacket(mbuf_t m,void *){if(m)freePacket(m);return kIOReturnOutputDropped;}
// Pull model output: Ethernet frames go to the engine's queues, which transmit them in the
// peer's availability window. Runs under the IO80211 work-queue gate, as the poll does.
IOReturn AirportAth10kAwdlNet::outputStart(IONetworkInterface *interface,IOOptionBits)
{
    uint8_t frame[AirportAwdl::kMaxEthernet];
    for(unsigned i=0;i<16;++i){
        mbuf_t head=nullptr;uint32_t count=0;
        if(awdlDequeueOutputPackets(interface,1,&head,nullptr,&count,nullptr)!=kIOReturnSuccess || !head)break;
        size_t length=mbuf_pkthdr_len(head);
        if(length<=sizeof(frame) && !mbuf_copydata(head,0,length,frame) && awdl && awdl->session()){
            uint64_t t=0,ns=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);
            if(awdl->session()->send(frame,length,ns/1000u))awdl->kick();
        }
        mbuf_freem(head);
    }
    return kIOReturnSuccess;
}
IOReturn AirportAth10kAwdlNet::registerWithPolicyMaker(IOService *policyMaker)
{
    static IOPMPowerState states[2]={{1,0,0,0,0,0,0,0,0,0,0,0},{1,kIOPMDeviceUsable,kIOPMPowerOn,kIOPMPowerOn,0,0,0,0,0,0,0,0}};
    return policyMaker->registerPowerDriver(this,states,2);
}
bool AirportAth10kAwdlNet::setLinkUp(bool wantUp)
{
    if(wantUp==linkUp)return true;
    linkUp=wantUp;
    setLinkStatus(kIONetworkLinkValid|(wantUp?kIONetworkLinkActive:0),wantUp?activeMedium:nullptr,wantUp?54000000:0);
    if(awdl && (awdl->stackFeatures()&kAwdlStackLinkState))awdlSetLinkState(awdl,wantUp?kIO80211NetworkLinkUp:kIO80211NetworkLinkDown,0);
    if(bsd && bsd->getIfnet()){
        if(wantUp)ifnet_set_flags(bsd->getIfnet(),IFF_UP|IFF_RUNNING,IFF_UP|IFF_RUNNING);
        else ifnet_set_flags(bsd->getIfnet(),0,IFF_RUNNING);
    }
    return true;
}
// The reply to VIRTUAL_IF_CREATE for the AWDL role (wifip2pd): the MAC and the BSD name.
IOReturn AirportAth10kAwdlNet::fillCreateReply(apple80211_virt_if_create_data *data) const
{
    if(!data || data->role!=6 || !up || !bsd)return kIOReturnUnsupported;
    memcpy(data->mac,mac,6);
    ifnet_t ifp=bsd->getIfnet();
    if(ifp)snprintf(reinterpret_cast<char *>(data->bsd_name),sizeof(data->bsd_name),"%s%u",ifnet_name(ifp),ifnet_unit(ifp));
    return kIOReturnSuccess;
}
void AirportAth10kAwdlNet::inputEthernet(const uint8_t *eth,size_t length)
{
    if(!bsd || !bsd->getIfnet() || length<14)return;
    unsigned int chunks=1;mbuf_t packet=nullptr;
    if(mbuf_allocpacket(MBUF_DONTWAIT,length+ETHER_ALIGN,&chunks,&packet) || !packet)return;
    uint8_t *data=static_cast<uint8_t *>(mbuf_data(packet))+ETHER_ALIGN;
    memcpy(data,eth,length);mbuf_setdata(packet,data,length);mbuf_pkthdr_setlen(packet,length);
    bsd->inputPacket(packet,static_cast<UInt32>(length),0,nullptr);
}
#endif // AIRPORT_ATH10K_AWDL
