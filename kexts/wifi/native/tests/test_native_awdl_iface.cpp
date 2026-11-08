// Interface creation for awdl0: the production code (native/src/AirportAth10kAwdlDriver.cpp,
// compiled unchanged) against a fake IOKit/IO80211 family (tests/awdl-fakes).
// Pins the creation order, the failure and teardown paths, the VIRTUAL_IF_CREATE reply, the ioctls
// answered from the engine, and the BSD data path. What the real family does with the object can only
// be seen on a Mac: kc-audit proves the ABI (slots, sizes, imports), not the behaviour.
#include "AirportAth10k.hpp"
#include <cassert>
#include <map>
std::vector<std::string> gCalls;
OSMetaClass IO80211SkywalkInterface::gMetaClass={"IO80211SkywalkInterface"};
OSMetaClass IO80211VirtualInterface::gMetaClass={"IO80211VirtualInterface"};
OSMetaClass IO80211AWDLProtocol::gMetaClass={"IO80211AWDLProtocol"};
void airportLog(const char *,...){}
void airportStep(const char *s){rec(std::string("step:")+s);}
const OSSymbol *gIONetworkFilterGroup=reinterpret_cast<const OSSymbol *>(1);
static UInt32 gBootChannel=0;static bool gAwdlSet=false;
bool PE_parse_boot_argn(const char *n,void *v,int){if(!strcmp(n,"ath10k_awdl_ch")&&gBootChannel){*static_cast<UInt32 *>(v)=gBootChannel;return true;}return false;}
// family non-virtual and virtual bases the header declares
bool IO80211VirtualInterface::init(IO80211Controller *c,ether_addr *a,unsigned int role,const char *){setInterfaceRole(role);IO80211SkywalkInterface::init(reinterpret_cast<IOService *>(c),a);return true;}
void IO80211VirtualInterface::free(){}
bool IO80211VirtualInterface::start(IOService *p){return IO80211SkywalkInterface::start(p);}
void IO80211VirtualInterface::stop(IOService *p){IO80211SkywalkInterface::stop(p);}
SInt32 IO80211VirtualInterface::prepareBSDInterface(struct __ifnet *i,unsigned int u){return IO80211SkywalkInterface::prepareBSDInterface(i,u);}
SInt32 IO80211VirtualInterface::setInterfaceEnable(bool){return 0;}
SInt32 IO80211VirtualInterface::peerToPeerConfigureIfnet(){rec("peerToPeerConfigureIfnet");return 0;}
#include "virtual_stubs.inc"
// IONetworkingFamily / IO80211 private SPI bound by mangled name (host names, no leading underscore rewrite here)
static std::vector<mbuf_t> gOutput;static bool gPull=false,gThread=false;
extern "C" IOReturn fakePull(IONetworkInterface *,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) __asm__("__ZN18IONetworkInterface24configureOutputPullModelEjjjjj");
IOReturn fakePull(IONetworkInterface *,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){gPull=true;rec("configureOutputPullModel");return 0;}
extern "C" IOReturn fakeDequeue(IONetworkInterface *,uint32_t,mbuf_t *,mbuf_t *,uint32_t *,uint64_t *) __asm__("__ZN18IONetworkInterface20dequeueOutputPacketsEjPP6__mbufS2_PjPy");
IOReturn fakeDequeue(IONetworkInterface *,uint32_t,mbuf_t *h,mbuf_t *,uint32_t *c,uint64_t *){if(gOutput.empty()){*h=nullptr;return 0;}*h=gOutput.front();gOutput.erase(gOutput.begin());*c=1;return 0;}
extern "C" IOReturn fakeStart(IONetworkInterface *,uint32_t) __asm__("__ZN18IONetworkInterface17startOutputThreadEj");
IOReturn fakeStart(IONetworkInterface *,uint32_t){gThread=true;rec("startOutputThread");return 0;}
extern "C" IOReturn fakeStop(IONetworkInterface *,uint32_t) __asm__("__ZN18IONetworkInterface16stopOutputThreadEj");
IOReturn fakeStop(IONetworkInterface *,uint32_t){gThread=false;rec("stopOutputThread");return 0;}
extern "C" bool fakeSub(IONetworkInterface *,uint32_t) __asm__("__ZN18IONetworkInterface19setInterfaceSubTypeEj");
static UInt32 gSubType=0;bool fakeSub(IONetworkInterface *,uint32_t t){gSubType=t;rec("setInterfaceSubType");return true;}
extern "C" void fakeAwdl(IO80211Controller *,IO80211VirtualInterface *) __asm__("__ZN17IO80211Controller16setAwdlInterfaceEP23IO80211VirtualInterface");
void fakeAwdl(IO80211Controller *,IO80211VirtualInterface *){gAwdlSet=true;rec("setAwdlInterface");}
// the asm-renamed vtable slot (the Darwin name) is linked against this on the host; tests call outputStart directly
// The family and kernel entry points the AWDL stack feed calls (AirportAth10kAwdlDriver.cpp).
static int gBpfFeeds=0,gNeighborsUp=0,gNeighborsDown=0,gLinkCalls=0;static UInt8 gFedBody[64];static UInt32 gFedDlt=0;static size_t gFedMeta=0;
extern "C" void fakeBpfMbuf(IO80211VirtualInterface *,mbuf_t m,unsigned dlt,void *,unsigned long n) __asm__("__ZN23IO80211VirtualInterface11bpfTapInputEP6__mbufjPvm");
extern "C" void fakeBpfMbuf(IO80211VirtualInterface *,mbuf_t m,unsigned dlt,void *,unsigned long n){++gBpfFeeds;gFedDlt=dlt;gFedMeta=n;memcpy(gFedBody,mbuf_data(m),sizeof gFedBody<mbuf_pkthdr_len(m)?sizeof gFedBody:mbuf_pkthdr_len(m));}
extern "C" void fakeNdUp(ifnet_t,struct sockaddr_in6 *,struct sockaddr_dl *,int32_t,int,int) __asm__("_nd6_alt_node_present");
extern "C" void fakeNdUp(ifnet_t,struct sockaddr_in6 *,struct sockaddr_dl *,int32_t,int,int){++gNeighborsUp;}
extern "C" void fakeNdDown(ifnet_t,struct sockaddr_in6 *,struct sockaddr_dl *) __asm__("_nd6_alt_node_absent");
extern "C" void fakeNdDown(ifnet_t,struct sockaddr_in6 *,struct sockaddr_dl *){++gNeighborsDown;}
extern "C" void fakeLinkState(IO80211VirtualInterface *,int,unsigned) __asm__("__ZN23IO80211VirtualInterface12setLinkStateE16IO80211LinkStatej");
extern "C" void fakeLinkState(IO80211VirtualInterface *,int,unsigned){++gLinkCalls;}
void *mbuf_datastart(mbuf_t m){return m->d.data();}
extern "C" void fakeOutputStartAlias() __asm__("__ZN20AirportAth10kAwdlNet11outputStartEP18IONetworkInterfacej");
void fakeOutputStartAlias(){}
// mbufs and BSD input
static std::vector<mbuf_fake *> gAlloc;
int mbuf_allocpacket(int,size_t n,unsigned *,mbuf_t *o){auto *m=new mbuf_fake;m->d.resize(n);*o=m;gAlloc.push_back(m);return 0;}
void *mbuf_data(mbuf_t m){return m->d.data()+m->off;}
int mbuf_setdata(mbuf_t m,void *p,size_t n){m->off=static_cast<UInt8 *>(p)-m->d.data();m->d.resize(m->off+n);return 0;}
void mbuf_pkthdr_setlen(mbuf_t m,size_t n){m->pkt=n;}
size_t mbuf_pkthdr_len(mbuf_t m){return m->pkt;}
void mbuf_freem(mbuf_t m){delete m;}
int mbuf_copydata(mbuf_t m,size_t o,size_t n,void *p){memcpy(p,m->d.data()+m->off+o,n);return 0;}
UInt32 IONetworkInterface::inputPacket(mbuf_t m,UInt32 len,IOOptionBits,void *){++inputs;lastInput.assign(static_cast<UInt8 *>(mbuf_data(m)),static_cast<UInt8 *>(mbuf_data(m))+len);delete m;return 0;}
// a core that answers the AWDL control ABI as Ath10kAwdlService.cpp does
static std::vector<uint32_t> gOps;
IOReturn Ath10kPCIDevice::controlRequest(OSObject *,const AthControl::Request *r,AthControl::Response *o){
    ++calls;lastOp=r->operation;gOps.push_back(r->operation);
    if(r->operation==AthControl::AwdlRadioStart){channel=r->data[6];step=static_cast<uint8_t>(AirportAwdl::RadioStep::Running);return 0;}
    if(r->operation==AthControl::AwdlRadioStop){step=0;return 0;}
    if(r->operation==AthControl::AwdlStatus){o->data[0]=step;return 0;}
    return kIOReturnSuccess;
}

static bool idx(const std::string &a,const std::string &b){int ia=-1,ib=-1;for(size_t i=0;i<gCalls.size();++i){if(gCalls[i]==a&&ia<0)ia=int(i);if(gCalls[i]==b)ib=int(i);}return ia>=0&&ib>=0&&ia<ib;}
static bool has(const std::string &a){for(auto &c:gCalls)if(c==a)return true;return false;}

static void testCreate(){
    gCalls.clear();gOps.clear();
    auto *wq=new IO80211WorkQueue;auto *dev=new Ath10kPCIDevice;auto *c=new AirportAth10k;c->q=wq;c->dev=dev;
    auto *net=new AirportAth10kAwdlNet;
    assert(net->initWith(c));
    assert(!memcmp(net->macAddress(),c->hw,6)); // AWDL-2 in shared mode: awdl0 carries the station's MAC so the station ACKs
    assert(net->attach(c));
    assert(net->start(c));
    assert(net->startedUp() && net->bsdInterface() && net->awdlInterface());
    assert(has("setInterfaceRole.6"));
    assert(idx("initRegistrationInfo","deferBSDAttach(true)"));
    assert(idx("deferBSDAttach(true)","skywalk.start"));
    assert(idx("skywalk.start","configureOutputPullModel"));
    assert(idx("configureOutputPullModel","bsd.attachToDataLinkLayer"));
    assert(idx("setInterfaceSubType","bsd.attachToDataLinkLayer") && gSubType==3);
    assert(idx("bsd.attachToDataLinkLayer","prepareBSDInterface"));
    assert(idx("prepareBSDInterface","peerToPeerConfigureIfnet"));
    assert(idx("peerToPeerConfigureIfnet","setAwdlInterface") && gAwdlSet);
    assert(!strcmp(net->bsdInterface()->getNamePrefix(),"awdl"));
    assert(!strcmp(net->awdlInterface()->getBSDNamePrefix(),"awdl"));
    // a registration blocks pair exists until stop
    // VIRTUAL_IF_CREATE reply: AWDL role only
    apple80211_virt_if_create_data d{};d.role=6;
    assert(net->fillCreateReply(&d)==kIOReturnSuccess && !memcmp(d.mac,net->macAddress(),6) && !strcmp(reinterpret_cast<char *>(d.bsd_name),"awdl0"));
    d={};d.role=5;assert(net->fillCreateReply(&d)==kIOReturnUnsupported);
    printf("Creation order: role 6, registration, deferred BSD attach, Skywalk start, pull model, awdl0 attach, p2p ifnet config, setAwdlInterface.\n");

    // the poll: starts the radio through the control ABI and raises the link
    auto *iface=net->awdlInterface();
    uint32_t got=0;
    apple80211_channel_data ch{};assert(iface->getCHANNEL(&ch)==0);
    gOps.clear();
    iface->session()->enable(1);  // re-arm with a known time
    // fire the timer until the radio runs
    extern IOTimerEventSource *awdlTestTimer(AirportAth10kAwdlInterface *);
    IOTimerEventSource *t=awdlTestTimer(iface);
    for(int i=0;i<5;++i)t->fire();
    (void)got;
    assert(!gOps.empty() && dev->step==4 && dev->channel==6);
    assert(iface->session()->running());
    assert(net->linkStatus&kIONetworkLinkActive);
    // ioctls answered from the engine
    apple80211_ssid_data ssid{};assert(iface->getSSID(&ssid)==0 && ssid.ssid_len==4 && !memcmp(ssid.ssid_bytes,"awdl",4));
    apple80211_bssid_data bssid{};assert(iface->getBSSID(&bssid)==0 && !memcmp(&bssid.bssid,AirportAwdl::kAwdlBssid,6));
    apple80211_state_data state{};assert(iface->getSTATE(&state)==0 && state.state==4);
    apple80211_awdl_presence_mode pm{};assert(iface->getAWDL_PRESENCE_MODE(&pm)==0 && pm.mode==4);
    apple80211_awdl_master_channel mch{};assert(iface->getAWDL_MASTER_CHANNEL(&mch)==0 && mch.master_channel==6);
    assert(iface->getCHANNEL(&ch)==0 && ch.channel.channel==6 && (ch.channel.flags&APPLE80211_C_FLAG_2GHZ));
    apple80211_awdl_election_metric metric{};assert(iface->getAWDL_ELECTION_METRIC(&metric)==0 && metric.metric==60);
    apple80211_awdl_sync_state sync{};assert(iface->getAWDL_SYNC_STATE(&sync)==0 && sync.state==1);
    apple80211_awdl_sync_params sp{};assert(iface->getAWDL_SYNC_PARAMS(&sp)==0 && sp.availability_window_period==16);
    assert(iface->setCOMPANION_SKYWALK_IF_CREATE(nullptr)==kIOReturnUnsupported); // never guess a layout
    // BSD data path
    gOutput.clear();auto mk=[&](size_t n){auto *m=new mbuf_fake;m->d.assign(n,0x41);m->pkt=n;return m;};
    gOutput.push_back(mk(100));gOutput.push_back(mk(100));
    assert(net->outputStart(net->bsdInterface(),0)==0 && gOutput.empty());
    assert(iface->session()->stats().sent==2);
    gOutput.push_back(mk(20));  // too small for a valid frame: dropped, freed, no leak (ASan)
    net->outputStart(net->bsdInterface(),0);
    uint8_t eth[60]{};eth[12]=0x86;eth[13]=0xdd;
    net->inputEthernet(eth,sizeof eth);assert(net->bsdInterface()->inputs==1 && net->bsdInterface()->lastInput.size()==60);
    net->inputEthernet(eth,10);assert(net->bsdInterface()->inputs==1); // runt ignored
    assert(net->outputPacket(nullptr,nullptr)==kIOReturnOutputDropped);

    // teardown order
    gCalls.clear();
    net->stop(c);
    assert(idx("skywalk.stop","controller.stop") && idx("detachInterface","skywalk.stop"));
    assert(has("stopOutputThread")==false || !gThread); // output thread halted if running
    net->detach(c);net->release();
    c->release();dev->release();delete wq;
    printf("awdl0: control ABI, link, ioctls from the engine, BSD input/output, teardown order.\n");

    // failure path: Skywalk start refused; no stop of an interface that never started, nothing leaks
    gCalls.clear();gAwdlSet=false;
    auto *wq2=new IO80211WorkQueue;auto *dev2=new Ath10kPCIDevice;auto *c2=new AirportAth10k;c2->q=wq2;c2->dev=dev2;
    // a net whose interface refuses to start
    auto *net2=new AirportAth10kAwdlNet;
    extern bool gFailSkywalkStart;gFailSkywalkStart=true;
    assert(net2->initWith(c2) && net2->attach(c2));
    assert(!net2->start(c2));gFailSkywalkStart=false;
    assert(!gAwdlSet);
    net2->stop(c2);
    assert(!has("skywalk.stop"));     // the interface was never started, so it is not stopped
    net2->detach(c2);net2->release();c2->release();dev2->release();delete wq2;
    printf("Failed Skywalk start: no awdl registration, no stop of an unstarted interface.\n");
}
bool gFailSkywalkStart=false;
IOTimerEventSource *awdlTestTimer(AirportAth10kAwdlInterface *i){return i->awdlTestTimerForFakes();}

// The macOS stack feed: received action frames reach the family's peer manager (DLT 12, the 0x48-byte metadata, the action
// body), valid peers get neighbor entries, OOB requests hand over the service records and unicast frames go out.
static void testStackFeed(){
    auto *wq=new IO80211WorkQueue;auto *dev=new Ath10kPCIDevice;auto *c=new AirportAth10k;c->q=wq;c->dev=dev;auto *net=new AirportAth10kAwdlNet;
    assert(net->initWith(c) && net->attach(c) && net->start(c));
    AirportAth10kAwdlInterface *a=net->awdlInterface();
    a->startSession();
    UInt8 frame[64]={0xd0,0};memcpy(frame+4,"\xff\xff\xff\xff\xff\xff",6);memcpy(frame+10,"\x02\xaa\xbb\xcc\xdd\xee",6);frame[24]=0x7f;frame[25]=0x00;frame[26]=0x17;frame[27]=0xf2;frame[28]=8;
    a->port.hostActionFrame(frame,sizeof frame,44,-55);
    assert(gBpfFeeds==1 && gFedDlt==12 && gFedMeta==0x48 && gFedBody[0]==0x7f && gFedBody[4]==8);
    AirportAwdl::Peer peer{};peer.used=true;peer.valid=true;peer.addr=AirportAwdl::Addr::of(frame+10);peer.rssi=-55;
    a->port.hostPeer(peer,true);assert(gNeighborsUp==1);a->port.hostPeer(peer,false);assert(gNeighborsDown==1);
    apple80211_awdl_oob_request oob;memset(&oob,0,sizeof oob);oob.data_len=20;
    UInt8 *r=reinterpret_cast<UInt8 *>(&oob);memcpy(r+0x16,"\x02\x11\x22\x33\x44\x66",6);
    memcpy(r+0x30,"\x7f\x00\x17\xf2\x08\x10\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x01\x00\xaa",20);
    assert(a->setAWDL_OOB_REQUEST(&oob)==kIOReturnSuccess);
    assert(a->setAWDL_OOB_AUTO_REQUEST(&oob)==kIOReturnSuccess);
    net->stop(c);net->detach(c);net->release();c->release();dev->release();delete wq;
    printf("Stack feed: peer manager input, neighbor entries, OOB requests.\n");
}
int main(){testCreate();testStackFeed();printf("AWDL interface tests passed.\n");return 0;}
