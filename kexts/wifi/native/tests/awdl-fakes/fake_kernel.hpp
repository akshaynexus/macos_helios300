// Minimal stand-ins for the kernel/IOKit classes AirportAth10kAwdlDriver.cpp uses, so
// the production creation path (native/src/AirportAth10kAwdlDriver.cpp, compiled unchanged)
// can run on the host. Every call that matters appends to gCalls: the test asserts the order.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <cerrno>
using UInt8=uint8_t;using UInt16=uint16_t;using UInt32=uint32_t;using UInt64=uint64_t;using SInt8=int8_t;using SInt32=int32_t;using IOReturn=int32_t;using IOOptionBits=uint32_t;using UInt=unsigned int;
using errno_t=int;
#define kIOReturnSuccess 0
#define kIOReturnError (-1)
#define kIOReturnUnsupported (-2)
#define kIOReturnBadArgument (-3)
#define kIOReturnNotReady (-4)
#define kIOReturnNotPermitted (-5)
#define kIOReturnNoMemory (-6)
#define kIOReturnOutputDropped 0x1000
#define IOPMAckImplied 0
#define APPLE_KEXT_OVERRIDE override
#define kIOPMDeviceUsable 2
#define kIOPMPowerOn 2
#define kIONetworkLinkValid 1
#define kIONetworkLinkActive 2
#define kIOPacketFilterMulticast 1
#define kIOMediumIEEE80211 0x80
#define MBUF_DONTWAIT 1
#define ETHER_ALIGN 2
#define IFF_UP 1
#define IFF_RUNNING 0x40
#define IFNAMSIZ 16
#define OSSafeReleaseNULL(p) do{if(p){(p)->release();(p)=nullptr;}}while(0)
extern std::vector<std::string> gCalls;
inline void rec(const std::string &s){gCalls.push_back(s);}
#include <cstdlib>
inline void IOLog(const char *,...){}
inline void *IOMalloc(size_t n){return malloc(n);}
inline void IOFree(void *p,size_t){free(p);}
struct OSMetaClass{const char *name;};
class OSObject{public:virtual ~OSObject(){}int rc=1;void retain(){++rc;}void release(){if(--rc==0)delete this;}virtual bool isInactive() const {return inactive;}bool inactive=false;};
template<class T,class U>T *dyn(U *p){return dynamic_cast<T *>(p);}
#define OSDynamicCast(T,p) (dynamic_cast<T *>(p))
#define OSDeclareAbstractStructors(c) public:static OSMetaClass gMetaClass;
#define OSDeclareDefaultStructors(c) public:static OSMetaClass gMetaClass;
#define OSDefineMetaClassAndStructors(c,p) OSMetaClass c::gMetaClass={#c};
#define OSMetaClassDeclareReservedUnused(c,n) virtual void _RESERVED##c##n(){}
class OSString:public OSObject{public:std::string v;static OSString *withCString(const char *c){auto *s=new OSString;s->v=c;return s;}};
class OSSymbol;
class OSDictionary:public OSObject{public:static OSDictionary *withCapacity(unsigned){return new OSDictionary;}};
struct IOPMPowerState{unsigned long v[12];};
class IOService:public OSObject{public:IOService *provider=nullptr;bool attached=false;
    virtual bool attach(IOService *p){provider=p;attached=true;rec(std::string(cls())+".attach");return true;}
    virtual void detach(IOService *){attached=false;provider=nullptr;rec(std::string(cls())+".detach");}
    virtual IOService *getProvider() const {return provider;}
    virtual bool start(IOService *){return true;}
    virtual void stop(IOService *){}
    virtual void free(){}
    virtual const char *cls() const {return "IOService";}
    void registerService(){rec(std::string(cls())+".registerService");}
    void setProperty(const char *k,uint64_t v,unsigned){props.emplace_back(k,v);}
    void setProperty(const char *k,const char *){props.emplace_back(k,1);}
    void setProperty(const char *k,OSObject *){props.emplace_back(k,1);}
    void setProperty(const char *k,const void *,unsigned){props.emplace_back(k,1);}
    std::vector<std::pair<std::string,uint64_t>> props;
    IOReturn registerPowerDriver(IOService *,IOPMPowerState *,unsigned long){return 0;}
};
class IOWorkLoop:public OSObject{public:virtual bool inGate(){return true;}
    IOReturn addEventSource(OSObject *){return 0;}IOReturn removeEventSource(OSObject *){return 0;}};
class IO80211WorkQueue:public IOWorkLoop{public:bool onThread(){return false;}};
typedef void (*TimerAction)(OSObject *,class IOTimerEventSource *);
class IOTimerEventSource:public OSObject{public:OSObject *owner=nullptr;TimerAction action=nullptr;IOWorkLoop *loop=nullptr;int cancels=0;uint32_t lastUs=0;
    static IOTimerEventSource *timerEventSource(OSObject *o,TimerAction a){auto *t=new IOTimerEventSource;t->owner=o;t->action=a;return t;}
    IOReturn setTimeoutMS(uint32_t ms){lastUs=ms*1000;return 0;}
    IOReturn setTimeoutUS(uint32_t us){lastUs=us;return 0;}
    void cancelTimeout(){++cancels;}
    IOWorkLoop *getWorkLoop(){return loop;}
    void fire(){action(owner,this);}
};
typedef IOReturn (*GateAction)(OSObject *,void *,void *,void *,void *);
class IOCommandGate:public OSObject{public:OSObject *target=nullptr;static IOCommandGate *commandGate(OSObject *o){auto *g=new IOCommandGate;g->target=o;return g;}
    IOReturn runAction(GateAction a,void *x=nullptr,void *y=nullptr,void *z=nullptr,void *w=nullptr){return a(target,x,y,z,w);}};
struct IOEthernetAddress{uint8_t bytes[6];};
struct ether_addr{uint8_t octet[6];};
typedef struct __ifnet *ifnet_t;
struct __ifnet{char name[8];unsigned unit;unsigned flags;unsigned eflags;};
inline const char *ifnet_name(ifnet_t i){return i->name;}
inline unsigned ifnet_unit(ifnet_t i){return i->unit;}
inline errno_t ifnet_set_flags(ifnet_t i,uint16_t v,uint16_t m){i->flags=(i->flags&~m)|(v&m);return 0;}
typedef struct mbuf_fake *mbuf_t;
struct mbuf_fake{std::vector<uint8_t> d;size_t off=0;size_t pkt=0;};
int mbuf_allocpacket(int,size_t,unsigned *,mbuf_t *);
void *mbuf_data(mbuf_t);
int mbuf_setdata(mbuf_t,void *,size_t);
void mbuf_pkthdr_setlen(mbuf_t,size_t);
size_t mbuf_pkthdr_len(mbuf_t);
void mbuf_freem(mbuf_t);
int mbuf_copydata(mbuf_t,size_t,size_t,void *);
inline void clock_get_uptime(uint64_t *t){*t=1000000;}
inline void absolutetime_to_nanoseconds(uint64_t a,uint64_t *n){*n=a;}
bool PE_parse_boot_argn(const char *,void *,int);
void airportLog(const char *f,...);
void airportStep(const char *);
#define AIRPORT_LOG(fmt,...) airportLog(fmt,##__VA_ARGS__)
#define AIRPORT_STEP(name) airportStep(name)
class IONetworkMedium:public OSObject{public:static IONetworkMedium *medium(unsigned,uint64_t){return new IONetworkMedium;}static bool addMedium(OSDictionary *,IONetworkMedium *){return true;}};
extern const OSSymbol *gIONetworkFilterGroup;
class IONetworkController;
class IONetworkInterface:public IOService{public:ifnet_t ifp=nullptr;IONetworkController *ctl=nullptr;virtual const char *getNamePrefix() const {return "net";}
    ifnet_t getIfnet() const {return ifp;}
    IONetworkController *getController() const {return ctl;}
    UInt32 inputs=0;std::vector<uint8_t> lastInput;
    virtual UInt32 inputPacket(mbuf_t m,UInt32 len,IOOptionBits,void *);
    virtual IOReturn attachToDataLinkLayer(IOOptionBits,void *){static __ifnet f;strcpy(f.name,getNamePrefix());f.unit=0;ifp=&f;rec("bsd.attachToDataLinkLayer");return 0;}
    virtual void detachFromDataLinkLayer(IOOptionBits,void *){rec("bsd.detach");ifp=nullptr;}
    virtual void bsdDone(){}
};
class IOEthernetInterface:public IONetworkInterface{public:virtual bool init(IONetworkController *c){ctl=c;return true;}};
typedef IONetworkInterface *InterfacePtr;
class IONetworkController:public IOService{public:virtual bool start(IOService *) override {rec("controller.start");return true;}
    virtual void stop(IOService *) override {rec("controller.stop");}
    virtual IOWorkLoop *getWorkLoop() const {return nullptr;}
    virtual IONetworkInterface *createInterface(){return nullptr;}
    virtual bool configureInterface(IONetworkInterface *){return true;}
    virtual UInt32 outputPacket(mbuf_t,void *){return 0;}
    virtual IOReturn enable(IONetworkInterface *){return 0;}
    virtual IOReturn disable(IONetworkInterface *){return 0;}
    virtual IOReturn selectMedium(const IONetworkMedium *){return 0;}
    virtual IOReturn registerWithPolicyMaker(IOService *){return 0;}
    virtual IOReturn setPowerState(unsigned long,IOService *){return 0;}
    virtual IOReturn getPacketFilters(const OSSymbol *,UInt32 *f) const {*f=0;return 0;}
    virtual IOReturn setPromiscuousMode(bool){return 0;}
    virtual IOReturn setMulticastMode(bool){return 0;}
    virtual IOReturn setMulticastList(IOEthernetAddress *,UInt32){return 0;}
    virtual const OSString *newVendorString() const {return nullptr;}
    virtual const OSString *newModelString() const {return nullptr;}
    virtual void _RESERVEDIONetworkController2(){}
    bool publishMediumDictionary(OSDictionary *){return true;}
    bool setCurrentMedium(IONetworkMedium *){return true;}
    bool setSelectedMedium(const IONetworkMedium *){return true;}
    bool setLinkStatus(unsigned s,const IONetworkMedium *,uint64_t){rec("linkStatus."+std::to_string(s));linkStatus=s;return true;}
    virtual bool attachInterface(IONetworkInterface **i,bool){*i=createInterface();if(!*i)return false;
        configureInterface(*i);(*i)->attachToDataLinkLayer(0,nullptr);return true;}
    void detachInterface(IONetworkInterface *i,bool){rec("detachInterface");if(i)i->detachFromDataLinkLayer(0,nullptr);}
    void freePacket(mbuf_t m){mbuf_freem(m);}
    unsigned linkStatus=0;
};
class IOEthernetController:public IONetworkController{public:virtual bool init(void *){return true;}
    virtual IOReturn getHardwareAddress(IOEthernetAddress *)=0;
    virtual IOReturn setHardwareAddress(const void *,UInt32)=0;
};
struct ifnet_init_eparams;
// AWDL stack feed (AirportAth10kAwdlDriver.cpp)
inline void *IOMallocZeroData(size_t n){return calloc(1,n);}
inline void IOFreeData(void *p,size_t){free(p);}
inline unsigned ifnet_index(ifnet_t){return 7;}
void *mbuf_datastart(mbuf_t m);
