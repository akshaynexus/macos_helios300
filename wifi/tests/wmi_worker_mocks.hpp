// Lifecycle ordering model only: does not emulate IOKit threads or power plane.
static bool OSCompareAndSwap(UInt32 old,UInt32 next,volatile UInt32 *address){
    if(*address!=old)return false;
    *address=next;return true;
}
class Ath10kPCIDevice;
using OSObject=Ath10kPCIDevice;
using IOService=Ath10kPCIDevice;
using IOReturn=int;
using IOOptionBits=unsigned;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNotReady=2,kIOReturnError=3,kIOPMAckImplied=0;
#define OSDynamicCast(type, obj) (obj)
struct MockService { static void systemWillShutdown(IOOptionBits) {} };
constexpr unsigned kIOTimerEventSourceOptionsPriorityWorkLoop=255;
struct IOTimerEventSource {
    using Action=void(*)(OSObject *,IOTimerEventSource *);
    inline static bool allocFail=false,scheduleFail=false;
    bool armed=false,enabled=true;
    static IOTimerEventSource *timerEventSource(unsigned options,OSObject *,Action) {assert(options==255);return allocFail?nullptr:new IOTimerEventSource;}
    int setTimeoutMS(unsigned ms) {assert(ms==1||ms==10||ms==100);if(scheduleFail)return 1;armed=true;return 0;}
    void cancelTimeout(){armed=false;}
    void disable(){enabled=false;}
    void release(){assert(!armed);delete this;}
};
struct IOFilterInterruptEventSource {void disable(){}void release(){};};
struct IOWorkLoop {
    using Action=IOReturn(*)(OSObject *,void *,void *,void *,void *);
    inline static bool allocFail=false,addFail=false;
    bool added=false;
    static IOWorkLoop *workLoop(){return allocFail?nullptr:new IOWorkLoop;}
    int addEventSource(IOTimerEventSource *){if(addFail)return 1;added=true;return 0;}
    void removeEventSource(IOFilterInterruptEventSource *){}
    void removeEventSource(IOTimerEventSource *timer){assert(added&&!timer->armed&&!timer->enabled);added=false;}
    int runAction(Action action,OSObject *owner,void *reason=nullptr){return action(owner,reason,nullptr,nullptr,nullptr);}
    void retain(){} // test factory transfers its sole reference to runtime
    void release(){assert(!added);delete this;}
};

inline uint64_t mockNowMs=0;
static void clock_get_uptime(uint64_t *now){*now=mockNowMs*1000000u;}
static void absolutetime_to_nanoseconds(uint64_t now,uint64_t *ns){*ns=now;}

namespace Ath10k {UInt64 uptimeNS(){return mockNowMs*1000000u;}}
