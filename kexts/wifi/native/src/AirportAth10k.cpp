// SPDX-License-Identifier: GPL-3.0-or-later
#include "AirportAth10k.hpp"
#include "Ath10kVersion.hpp"
#include <IOKit/network/IONetworkMedium.h>
#include <sys/random.h>

#define super IO80211Controller
OSDefineMetaClassAndStructors(AirportAth10k, IO80211Controller)

static_assert(AirportBridge::kNotReady==kIOReturnNotReady && AirportBridge::kBusy==kIOReturnBusy && AirportBridge::kUnsupported==kIOReturnUnsupported &&
              AirportBridge::kBadArgument==kIOReturnBadArgument && AirportBridge::kTimeout==kIOReturnTimeout && AirportBridge::kOffline==kIOReturnOffline &&
              AirportBridge::kAborted==kIOReturnAborted && AirportBridge::kNotFound==kIOReturnNotFound && AirportBridge::kError==kIOReturnError &&
              AirportBridge::kExclusiveAccess==kIOReturnExclusiveAccess && AirportBridge::kNotAttached==kIOReturnNotAttached,"IOReturn mirror");

volatile SInt32 gAirportAth10kStep=0;

// Pre-flight data for tools/kc-audit.py: sizeof() of every Apple class this
// kext subclasses, as compiled from the reconstructed headers. The audit
// compares each with the classSize the real collection's MetaClass
// constructor passes; a mismatch means our object overlaps the parent's ivars.
struct KcAuditParentSize { const char *name; uint64_t size; };
extern "C" __attribute__((used)) const KcAuditParentSize kKcAuditParentSizes[]={
    {"IO80211Controller",sizeof(IO80211Controller)},
    {"IO80211InfraProtocol",sizeof(IO80211InfraProtocol)},
    {"IO80211InfraInterface",sizeof(IO80211InfraInterface)},
    {"IO80211SkywalkInterface",sizeof(IO80211SkywalkInterface)},
    {"IOSkywalkEthernetInterface",sizeof(IOSkywalkEthernetInterface)},
    {"IOSkywalkNetworkInterface",sizeof(IOSkywalkNetworkInterface)},
    {"IOSkywalkInterface",sizeof(IOSkywalkInterface)},
    {"IOEthernetController",sizeof(IOEthernetController)},
    {"IONetworkController",sizeof(IONetworkController)},
    {"IOEthernetInterface",sizeof(IOEthernetInterface)},
    {"IONetworkInterface",sizeof(IONetworkInterface)},
    {"IO80211WorkQueue",sizeof(IO80211WorkQueue)},
    {"IOService",sizeof(IOService)},
    {nullptr,0}};

bool airportAth10kRandom(uint8_t *p,size_t n){if(!p || !n)return false;read_random(p,static_cast<u_int>(n));return true;}

bool AirportAth10k::init(OSDictionary *properties)
{
    if(!super::init(properties))return false;
    powerState=1;geoCountry[0]=0;return true;
}

IOService *AirportAth10k::probe(IOService *provider,SInt32 *score)
{
    AIRPORT_STEP("probe");
    auto *n=OSDynamicCast(AirportAth10kNub,provider);
    if(!n || !n->core() || !n->queue()){AIRPORT_LOG("provider is not a started AirportAth10kNub");return nullptr;}
    return super::probe(provider,score);
}

bool AirportAth10k::initCCLogs()
{
    CCPipeOptions options={};
    options.pipe_type=0;options.log_data_type=1;options.pipe_size=0x200000;options.min_log_size_notify=0xccccc;options.notify_threshold=1000;
    strlcpy(options.file_name,"AirportAth10k_Logs",sizeof(options.file_name));
    snprintf(options.name,sizeof(options.name),"wlan%d",0);
    strlcpy(options.directory_name,"WiFi",sizeof(options.directory_name));
    options.pad9=0x1000000;options.pad10=2;options.file_options=0;options.log_policy=0;
    driverLogPipe=CCPipe::withOwnerNameCapacity(this,"com.helios300.AirportAth10k","DriverLogs",&options);
    if(!driverLogPipe || !getWorkLoop())return false;
    // The factory only initialises the pipe; startPipe creates its event sources.
    logPipeStarted=driverLogPipe->CCPipe::startPipe();
    if(!logPipeStarted)return false;
    CCStreamOptions loggerOptions={};
    CCStream *logger=CCStream::withPipeAndName(driverLogPipe,"AirportAth10k",&loggerOptions);
    driverLogger=OSDynamicCast(CCLogStream,logger);
    if(!driverLogger){OSSafeReleaseNULL(logger);return false;}

    memset(&options,0,sizeof(options));
    options.pipe_type=0;options.log_data_type=0;options.pipe_size=0x200000;options.min_log_size_notify=0xccccc;options.notify_threshold=1000;
    strlcpy(options.file_name,"AppleBCMWLAN_Datapath",sizeof(options.file_name));
    strlcpy(options.directory_name,"WiFi",sizeof(options.directory_name));
    options.pad9=static_cast<uint32_t>(0x202800000ull>>32);options.pad10=static_cast<uint32_t>(0x202800000ull);
    driverDataPathPipe=CCPipe::withOwnerNameCapacity(this,"com.helios300.AirportAth10k","DatapathEvents",&options);
    if(!driverDataPathPipe)return false;
    dataPathPipeStarted=driverDataPathPipe->CCPipe::startPipe();
    if(!dataPathPipeStarted)return false;

    memset(&options,0,sizeof(options));
    options.pipe_type=0x200000001ull;options.log_data_type=2;
    strlcpy(options.file_name,"StateSnapshots",sizeof(options.file_name));
    strlcpy(options.name,"0",sizeof(options.name));
    strlcpy(options.directory_name,"WiFi",sizeof(options.directory_name));
    options.pipe_size=128;
    driverSnapshotsPipe=CCPipe::withOwnerNameCapacity(this,"com.helios300.AirportAth10k","StateSnapshots",&options);
    if(!driverSnapshotsPipe)return false;
    snapshotsPipeStarted=driverSnapshotsPipe->CCPipe::startPipe();
    if(!snapshotsPipeStarted)return false;

    CCStreamOptions faultOptions={};
    faultOptions.stream_type=1;faultOptions.console_level=0xFFFFFFFFFFFFFFFFull;
    driverFaultStream=CCStream::withPipeAndName(driverSnapshotsPipe,"FaultReporter",&faultOptions);
    CCDataStream *dataStream=OSDynamicCast(CCDataStream,driverFaultStream);
    if(!dataStream || !workQueue)return false;
    driverCoreFaultReporter=CCFaultReporter::withStreamWorkloop(dataStream,workQueue);
    if(!driverCoreFaultReporter)return false;
    driverFaultReporter=IO80211FaultReporter::allocWithParams(driverCoreFaultReporter);
    return OSDynamicCast(CommonFaultReporter,driverFaultReporter)!=nullptr;
}

void AirportAth10k::releaseAll()
{
    if(netif)netif->stopSession();
    OSSafeReleaseNULL(driverFaultReporter);
    OSSafeReleaseNULL(driverCoreFaultReporter);
    OSSafeReleaseNULL(driverFaultStream);
    OSSafeReleaseNULL(driverLogger);
    if(snapshotsPipeStarted){driverSnapshotsPipe->CCPipe::stopPipe();snapshotsPipeStarted=false;}
    if(dataPathPipeStarted){driverDataPathPipe->CCPipe::stopPipe();dataPathPipeStarted=false;}
    if(logPipeStarted){driverLogPipe->CCPipe::stopPipe();logPipeStarted=false;}
    OSSafeReleaseNULL(driverLogPipe);
    OSSafeReleaseNULL(driverDataPathPipe);
    OSSafeReleaseNULL(driverSnapshotsPipe);
    if(workQueue){
        if(gate){workQueue->removeEventSource(gate);gate->release();gate=nullptr;}
        workQueue->release();workQueue=nullptr;
    }
}

// ath10k_native_delay=<seconds>: start() returns at once and the whole
// bring-up (IO80211Controller::start, interfaces, Skywalk, registerService)
// runs from a thread call N seconds later, so the system reaches login and SSH
// first and `log stream --predicate 'sender == "AirportAth10k"'` shows the
// breadcrumbs while it initialises. Diagnostic only; the default is immediate.
bool AirportAth10k::start(IOService *provider)
{
    AIRPORT_STEP("start");
    nub=OSDynamicCast(AirportAth10kNub,provider);
    ath10k=nub?nub->core():nullptr;
    if(!ath10k)return false;
    UInt32 delay=0;
    const bool parsed=PE_parse_boot_argn("ath10k_native_delay",&delay,sizeof(delay));
    AIRPORT_LOG("boot-arg ath10k_native_delay: %s (%u)",parsed?"present":"absent",delay);
    if(parsed && delay){
        if(delay>600)delay=600;
        deferredCall=thread_call_allocate(deferredStart,this);
        if(!deferredCall)return false;
        deferredProvider=provider;provider->retain();retain();
        uint64_t deadline=0;clock_interval_to_deadline(delay,kSecondScale,&deadline);
        AIRPORT_LOG("ath10k_native_delay=%u: bring-up deferred %u s (start returns now)",delay,delay);
        AIRPORT_STEP("start: deferred");
        thread_call_enter_delayed(deferredCall,deadline);
        return true;
    }
    return bringUp(provider);
}

void AirportAth10k::deferredStart(thread_call_param_t self_,thread_call_param_t)
{
    auto *self=static_cast<AirportAth10k *>(self_);
    AIRPORT_STEP("deferred bring-up begins");
    if(self->isInactive())AIRPORT_LOG("deferred bring-up: already inactive");
    else if(!self->bringUp(self->deferredProvider))AIRPORT_LOG("deferred bring-up FAILED (see the last step line)");
    self->deferredProvider->release();self->deferredProvider=nullptr;
    self->release();
}

bool AirportAth10k::bringUp(IOService *provider)
{
    UInt8 builtIn=0;
    setProperty("built-in",OSData::withBytes(&builtIn,sizeof(builtIn)));
    setProperty("DriverKitDriver",kOSBooleanFalse);
    setProperty("AirportAth10kVersion",ATH10K_VERSION);
    // IO80211Controller::start consults createWorkQueue/getWorkQueue. The
    // queue is the nub's, so provider->getWorkLoop() == getWorkLoop() in
    // IONetworkController::start (see AirportAth10kNub.hpp).
    AIRPORT_STEP("work queue (from the nub)");
    workQueue=nub->queue();
    if(!workQueue){AIRPORT_LOG("no IO80211WorkQueue");return false;}
    workQueue->retain();
    AIRPORT_STEP("corecapture pipes");
    if(!initCCLogs()){AIRPORT_LOG("corecapture pipes failed");releaseAll();return false;}
    AIRPORT_STEP("IO80211Controller::start");
    if(!super::start(provider)){releaseAll();return false;}
    AIRPORT_STEP("command gate");
    gate=IOCommandGate::commandGate(this);
    if(!gate || workQueue->addEventSource(gate)!=kIOReturnSuccess){AIRPORT_LOG("no command gate");super::stop(provider);releaseAll();return false;}

    AIRPORT_STEP("medium tables");
    OSDictionary *media=OSDictionary::withCapacity(2);
    IONetworkMedium *medium=IONetworkMedium::medium(kIOMediumIEEE80211,54000000);
    IONetworkMedium *none=IONetworkMedium::medium(kIOMediumIEEE80211None,0);
    bool ok=media && medium && none && IONetworkMedium::addMedium(media,medium) && IONetworkMedium::addMedium(media,none) && publishMediumDictionary(media);
    if(ok)ok=setCurrentMedium(medium) && setSelectedMedium(medium);
    activeMedium=medium;
    if(medium)medium->release();if(none)none->release();if(media)media->release();
    if(!ok){AIRPORT_LOG("medium tables failed");super::stop(provider);releaseAll();return false;}

    AIRPORT_STEP("interface init");
    netif=new AirportAth10kInterface;
    if(!netif || !netif->init(this)){AIRPORT_LOG("interface init failed");if(netif){netif->release();netif=nullptr;}super::stop(provider);releaseAll();return false;}
    netif->setInterfaceRole(1);
    netif->setInterfaceId(1);
    AIRPORT_STEP("interface attach + IO80211Controller::attachInterface");
    if(!netif->attach(this) || !attachInterface(netif,this)){AIRPORT_LOG("attachInterface failed");super::stop(provider);releaseAll();return false;}
    interfaceAttached=true;
    AIRPORT_STEP("BSD interface (IONetworkController::attachInterface)");
    if(!IONetworkController::attachInterface(reinterpret_cast<IONetworkInterface **>(&bsd),false)){AIRPORT_LOG("BSD interface attach failed");super::stop(provider);releaseAll();return false;}
    AIRPORT_STEP("Skywalk registration info");
    IOSkywalkEthernetInterface::RegistrationInfo info;memset(&info,0,sizeof(info));
    if(!netif->initRegistrationInfo(&info,1,sizeof(info))){AIRPORT_LOG("initRegistrationInfo failed");super::stop(provider);releaseAll();return false;}
    netif->mExpansionData->fRegistrationInfo=static_cast<IOSkywalkNetworkInterface::RegistrationInfo *>(IOMalloc(sizeof(IOSkywalkNetworkInterface::RegistrationInfo)));
    netif->mExpansionData2->fRegistrationInfo=static_cast<IOSkywalkEthernetInterface::RegistrationInfo *>(IOMalloc(sizeof(IOSkywalkEthernetInterface::RegistrationInfo)));
    if(!netif->mExpansionData->fRegistrationInfo || !netif->mExpansionData2->fRegistrationInfo){super::stop(provider);releaseAll();return false;}
    memcpy(netif->mExpansionData->fRegistrationInfo,&info,sizeof(info));
    memcpy(netif->mExpansionData2->fRegistrationInfo,&info,sizeof(info));
    if(netif->getInterfaceRole()==1)netif->deferBSDAttach(true);
    AIRPORT_STEP("IO80211InfraInterface::start");
    if(!netif->start(this)){AIRPORT_LOG("Skywalk interface start failed");super::stop(provider);releaseAll();return false;}
    AIRPORT_STEP("core nativeAttachInterface");
    IOReturn attach=ath10k->nativeAttachInterface(bsd,getOutputQueue());
    if(attach!=kIOReturnSuccess){AIRPORT_LOG("core refused the interface 0x%x",attach);super::stop(provider);releaseAll();return false;}
    coreAttached=true;
    IOEthernetController::setLinkStatus(kIONetworkLinkValid);
    AIRPORT_STEP("registerService (BSD interface, controller)");
    bsd->registerService();
    registerService();
    broughtUp=true;
    AIRPORT_STEP("started; arming the core poll");
    netif->startPolling();
    AIRPORT_LOG("started over %s via %s (version %s)",ath10k->getName(),nub->getName(),ATH10K_VERSION);
    return true;
}

void AirportAth10k::stop(IOService *provider)
{
    AIRPORT_STEP("stop");
    if(deferredCall){
        if(thread_call_cancel(deferredCall)){ // never ran: drop the references it held
            if(deferredProvider){deferredProvider->release();deferredProvider=nullptr;}
            release();
        }
        thread_call_free(deferredCall);deferredCall=nullptr;
    }
    if(!broughtUp){IOService::stop(provider);return;} // deferred bring-up never happened: IO80211Controller::start never ran
    if(netif)netif->stopSession();
    if(coreAttached && ath10k){ath10k->nativeDetachInterface();coreAttached=false;}
    super::stop(provider);
    IOEthernetController::setLinkStatus(kIONetworkLinkValid);
    if(interfaceAttached && netif){detachInterface(netif,true);interfaceAttached=false;}
    OSSafeReleaseNULL(netif);
    releaseAll();
    ath10k=nullptr;
}

void AirportAth10k::free()
{
    OSSafeReleaseNULL(netif);
    super::free();
}

bool AirportAth10k::createWorkQueue(){return workQueue!=nullptr;}
IOWorkLoop *AirportAth10k::getWorkLoop() const {return workQueue;}
IO80211WorkQueue *AirportAth10k::getWorkQueue() const {return workQueue;}

IONetworkInterface *AirportAth10k::createInterface()
{
    auto *interface=new AirportAth10kEthernetInterface;
    if(!interface)return nullptr;
    if(!interface->initWithSkywalkInterfaceAndProvider(this,netif)){interface->release();return nullptr;}
    return interface;
}

bool AirportAth10k::configureInterface(IONetworkInterface *interface)
{
    if(!super::configureInterface(interface))return false;
    interface->setProperty("IOModel","Qualcomm Atheros QCA6174 (ath10k)");
    interface->setProperty("IOUserVisibleName","Wi-Fi");
    return true;
}

// Output is gated on the core's work loop and lands in Ath10kPCIDevice::outputPacket.
IOOutputQueue *AirportAth10k::createOutputQueue()
{
    if(!ath10k || !coreController()->getWorkLoop())return nullptr;
    return IOGatedOutputQueue::withTarget(ath10k,coreController()->getWorkLoop(),Ath10k::kOutputQueueCapacity);
}

UInt32 AirportAth10k::outputPacket(mbuf_t packet,void *param)
{
    // Only reached if something bypasses the output queue; keep the core's gate.
    IOOutputQueue *queue=getOutputQueue();
    if(queue)return queue->enqueue(packet,param);
    if(packet)freePacket(packet);
    return kIOReturnOutputDropped;
}

bool AirportAth10k::setLinkStatus(UInt32 status,const IONetworkMedium *medium,UInt64 speed,OSData *data)
{
    const bool ok=super::setLinkStatus(status,medium,speed,data);
    if(bsd)bsd->setLinkState((status&kIONetworkLinkActive)?kIO80211NetworkLinkUp:kIO80211NetworkLinkDown);
    return ok;
}

void AirportAth10k::nativeLink(bool up)
{
    setLinkStatus(kIONetworkLinkValid|(up?kIONetworkLinkActive:0),up?activeMedium:nullptr,up?54000000:0);
}

void AirportAth10k::postInterfaceMessage(unsigned int message,void *data,unsigned long length)
{
    if(netif)postMessage(netif,message,data,length,true);
}

IOReturn AirportAth10k::enable(IONetworkInterface *)
{
    AIRPORT_STEP("enable(IONetworkInterface)");
    if(!ath10k)return kIOReturnNotReady;
    const IOReturn r=ath10k->nativeEnable(true);
    return r==kIOReturnNotReady?kIOReturnSuccess:r; // the core enables once its runtime is up
}
IOReturn AirportAth10k::disable(IONetworkInterface *)
{
    if(ath10k)ath10k->nativeEnable(false);
    return kIOReturnSuccess;
}

IOReturn AirportAth10k::getHardwareAddress(IOEthernetAddress *address)
{
    return ath10k?coreController()->getHardwareAddress(address):kIOReturnNotReady;
}
// WCL applies private (per-network) addresses here and replays the current one
// after every driver-available notification. The firmware's station address is
// fixed by the core at VDEV_CREATE, so only the unchanged address is accepted.
IOReturn AirportAth10k::setHardwareAddress(const void *address,UInt32 length)
{
    IOEthernetAddress current;
    if(!address || getHardwareAddress(&current)!=kIOReturnSuccess)return kIOReturnNotReady;
    switch(AirportWCL::planMacUpdate(current.bytes,static_cast<const uint8_t *>(address),length,false)){
    case AirportWCL::MacUpdate::Unchanged:return kIOReturnSuccess;
    case AirportWCL::MacUpdate::Invalid:return kIOReturnBadArgument;
    default:AIRPORT_LOG("private Wi-Fi address not supported; turn it off for this network");return kIOReturnUnsupported;
    }
}
IOReturn AirportAth10k::getPacketFilters(const OSSymbol *group,UInt32 *filters) const
{
    if(group==gIONetworkFilterGroup){*filters=kIOPacketFilterMulticast;return kIOReturnSuccess;}
    return IOEthernetController::getPacketFilters(group,filters);
}
IOReturn AirportAth10k::setPromiscuousMode(bool){return kIOReturnSuccess;}
IOReturn AirportAth10k::setMulticastMode(bool enabled){return ath10k?coreController()->setMulticastMode(enabled):kIOReturnNotReady;}
IOReturn AirportAth10k::setMulticastList(IOEthernetAddress *list,UInt32 count){return ath10k?coreController()->setMulticastList(list,count):kIOReturnNotReady;}
UInt32 AirportAth10k::getFeatures() const {return ath10k?coreController()->getFeatures():0;}
const OSString *AirportAth10k::newVendorString() const {return OSString::withCString("Qualcomm Atheros");}
const OSString *AirportAth10k::newModelString() const {return OSString::withCString("QCA6174 802.11ac (ath10k)");}
IOReturn AirportAth10k::selectMedium(const IONetworkMedium *medium){setSelectedMedium(medium);return kIOReturnSuccess;}
SInt32 AirportAth10k::enableFeature(IO80211FeatureCode code,void *){return code==kIO80211Feature80211n?0:102;}
bool AirportAth10k::getLogPipes(CCPipe **logPipe,CCPipe **eventPipe,CCPipe **snapshotsPipe)
{
    bool ret=false;
    if(logPipe){*logPipe=driverLogPipe;ret=true;}
    if(eventPipe){*eventPipe=driverDataPathPipe;ret=true;}
    if(snapshotsPipe){*snapshotsPipe=driverSnapshotsPipe;ret=true;}
    return ret;
}

IOReturn AirportAth10k::getDRIVER_VERSION(IO80211SkywalkInterface *,apple80211_version_data *v)
{
    if(!v)return kIOReturnBadArgument;
    v->version=APPLE80211_VERSION;
    snprintf(v->string,sizeof(v->string),"AirportAth10k %s (ath10k core, WLAN.RM.4.4.1-00309)",ATH10K_VERSION);
    v->string_len=static_cast<u_int16_t>(strlen(v->string));return kIOReturnSuccess;
}
IOReturn AirportAth10k::getHARDWARE_VERSION(IO80211SkywalkInterface *,apple80211_version_data *v)
{
    if(!v)return kIOReturnBadArgument;
    v->version=APPLE80211_VERSION;
    strlcpy(v->string,"QCA6174 hw3.2 (168c:003e 11ad:0807)",sizeof(v->string));
    v->string_len=static_cast<u_int16_t>(strlen(v->string));return kIOReturnSuccess;
}
// After AirportItlwm's WCL build: WPA2 with CCMP (TKIP group keys), WME,
// short GI, scan completion events; no WPA1, WEP, PMF, AP or IBSS modes.
IOReturn AirportAth10k::getCARD_CAPABILITIES(IO80211SkywalkInterface *,apple80211_capability_data *cd)
{
    if(!cd)return kIOReturnBadArgument;
    memset(cd,0,sizeof(*cd));
    cd->version=APPLE80211_VERSION;
    cd->capabilities[0]=(1<<APPLE80211_CAP_AES_CCM)|(1<<APPLE80211_CAP_TKIP);
    cd->capabilities[1]=(1<<(APPLE80211_CAP_SHSLOT-8))|(1<<(APPLE80211_CAP_SHPREAMBLE-8))|(1<<(APPLE80211_CAP_WPA2-8))|(1<<(APPLE80211_CAP_TKIPMIC-8));
    cd->capabilities[2]=(1<<(APPLE80211_CAP_WME-16))|(1<<(APPLE80211_CAP_SHORT_GI_40MHZ-16))|(1<<(APPLE80211_CAP_SHORT_GI_20MHZ-16));
    cd->capabilities[5]=0x40;
    return kIOReturnSuccess;
}
IOReturn AirportAth10k::getPOWER(IO80211SkywalkInterface *,apple80211_power_data *pd)
{
    if(!pd)return kIOReturnBadArgument;
    pd->version=APPLE80211_VERSION;pd->num_radios=1;
    for(unsigned i=0;i<APPLE80211_MAX_RADIO;++i)pd->power_state[i]=powerState;
    return kIOReturnSuccess;
}
IOReturn AirportAth10k::setPOWER(IO80211SkywalkInterface *,apple80211_power_data *pd)
{
    if(!pd)return kIOReturnBadArgument;
    if(pd->num_radios>0){
        const bool on=pd->power_state[0]!=0;
        AIRPORT_STEP(on?"setPOWER on":"setPOWER off");
        powerState=on?1:0;
        if(netif)netif->powerChanged(on);
        if(ath10k)ath10k->nativeEnable(on);
    }
    return kIOReturnSuccess;
}
IOReturn AirportAth10k::getCOUNTRY_CODE(IO80211SkywalkInterface *,apple80211_country_code_data *cd)
{
    if(!cd)return kIOReturnBadArgument;
    return netif?netif->countryCode(cd):kIOReturnNotReady;
}
IOReturn AirportAth10k::setCOUNTRY_CODE(IO80211SkywalkInterface *,apple80211_country_code_data *cd)
{
    if(!cd)return kIOReturnBadArgument;
    // The core's regulatory policy (Ath10kCountryCode / ath10k_cc) decides;
    // the location hint is remembered for diagnostics only.
    if(cd->cc[0]!='x' && cd->cc[0]!='X')memcpy(geoCountry,cd->cc,sizeof(geoCountry));
    return kIOReturnSuccess;
}

static IOPMPowerState powerStates[2]={
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,kIOPMDeviceUsable,kIOPMPowerOn,kIOPMPowerOn,0,0,0,0,0,0,0,0}};
IOReturn AirportAth10k::registerWithPolicyMaker(IOService *policyMaker)
{
    pmPolicyMaker=policyMaker;pmState=1;
    return policyMaker->registerPowerDriver(this,powerStates,2);
}
// Sleep/wake is owned by the core (Ath10kPower.cpp); after wake the session
// sees the core's restart-required bit and asks for the guarded restart.
IOReturn AirportAth10k::setPowerState(unsigned long state,IOService *)
{
    pmState=state;return IOPMAckImplied;
}
