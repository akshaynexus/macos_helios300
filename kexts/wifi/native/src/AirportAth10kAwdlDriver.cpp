// SPDX-License-Identifier: GPL-3.0-or-later
// awdl0 for AirportAth10k: see AirportAth10kAwdlDriver.hpp and docs/WIFI-AWDL.md.
#include "AirportAth10k.hpp"
#if AIRPORT_ATH10K_AWDL
#include "AirportAth10kAwdlDriver.hpp"
#include <IOKit/network/IONetworkMedium.h>
#include <kern/clock.h>
#include <net/ethernet.h>
#include <net/if.h>
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
bool AirportAth10kAwdlInterface::Port::radioStart(const uint8_t *mac,uint8_t channel)
{
    uint8_t data[7];memcpy(data,mac,6);data[6]=channel;return control(AthControl::AwdlRadioStart,data,7,nullptr);
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
}
void AirportAth10kAwdlInterface::Port::hostSyncChanged(){}

// ioctls whose layouts are known (reconstructed apple80211_ioctl.h) answered from the
// engine. Everything else stays Unsupported rather than reading a guessed layout.
#define AWDL_ARG(d) do{if(!(d))return kIOReturnBadArgument;}while(0)
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
IOReturn AirportAth10kAwdlInterface::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *){return kIOReturnSuccess;}

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
    // A locally administered address distinct from the station's (Apple randomises it too).
    memcpy(mac,hw.bytes,6);mac[0]|=2;mac[5]^=0xa5;
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
