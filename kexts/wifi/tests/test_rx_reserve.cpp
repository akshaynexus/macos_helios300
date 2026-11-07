#include <cassert>
#include <cstring>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include "Ath10kPerformance.hpp"
#include "Ath10kConnection.hpp"
class Ath10kPCIDevice;
using OSObject=Ath10kPCIDevice;using IOReturn=int;
constexpr int kIOReturnSuccess=0;
constexpr unsigned kIOTimerEventSourceOptionsPriorityWorkLoop=255;
#define OSDynamicCast(type,obj) (obj)
using IOSimpleLock=std::mutex;
static IOSimpleLock *IOSimpleLockAlloc(){return new std::mutex;}
static void IOSimpleLockFree(IOSimpleLock *p){delete p;}
static void IOSimpleLockLock(IOSimpleLock *p){p->lock();}
static void IOSimpleLockUnlock(IOSimpleLock *p){p->unlock();}
struct IOTimerEventSource {
    bool enabled=true,armed=false;
    template<class... T>static IOTimerEventSource *timerEventSource(T...){return new IOTimerEventSource;}
    int setTimeoutMS(unsigned ms){assert(ms==1 || ms==10);armed=true;return 0;}
    void cancelTimeout(){armed=false;}void disable(){enabled=false;}void release(){assert(!armed);delete this;}
};
struct IOWorkLoop {
    static IOWorkLoop *workLoop(){return new IOWorkLoop;}
    int addEventSource(IOTimerEventSource *){return 0;}void removeEventSource(IOTimerEventSource *){}
    template<class T>int runAction(T action,OSObject *owner){return action(owner,nullptr,nullptr,nullptr,nullptr);}
    void release(){delete this;}
};
struct Packet {std::vector<UInt8> data;size_t len;explicit Packet(size_t n):data(n,0xa5),len(n){}};
using mbuf_t=Packet *;
static mbuf_t mbuf_next(mbuf_t){return nullptr;}
static size_t mbuf_len(mbuf_t p){return p->len;}
static void mbuf_setlen(mbuf_t p,size_t n){p->len=n;}
static void mbuf_pkthdr_setlen(mbuf_t p,size_t n){assert(p->len==n);}
class Ath10kPCIDevice {
public:
    IOWorkLoop *_rxReserveWorkLoop=nullptr;IOTimerEventSource *_rxReserveTimer=nullptr;
    IOSimpleLock *_rxReserveLock=nullptr;bool _rxReserveAdded=false;
    mbuf_t _rxReserve[Ath10k::kRXReserveCapacity]{};UInt32 _rxReserveCount=0;Ath10k::PerformanceState _perf{};
    std::atomic<bool> fail{false};std::atomic<int> live{0};
    mbuf_t allocatePacket(UInt32 n){if(fail)return nullptr;++live;return new Packet(n);}
    void freePacket(mbuf_t p){assert(p);--live;delete p;}
    bool prepareRXReserve();void pauseRXReserve();void releaseRXReserve();void refillRXReserve();mbuf_t takeRXPacket(UInt32);
    static void rxReserveTimerFired(OSObject *,IOTimerEventSource *);
    static IOReturn stopRXReserve(OSObject *,void *,void *,void *,void *);
};
#include "rx_reserve_methods.inc"
int main(){
    Ath10kPCIDevice d;assert(d.prepareRXReserve());assert(d._rxReserveCount==256 && d.live==256);
    std::vector<mbuf_t> owned;
    for(UInt32 i=0;i<257;++i){auto p=d.takeRXPacket(64+i%1400);assert(p && p->len==64+i%1400);owned.push_back(p);}
    assert(!d._rxReserveCount && d._perf.reserveHits==256 && d._perf.reserveMisses==1);
    for(auto p:owned)d.freePacket(p);
    d.fail=true;d.refillRXReserve();assert(d._perf.reserveFailures==1 && !d.takeRXPacket(100));
    d.fail=false;d.refillRXReserve();assert(d._rxReserveCount==64);
    std::thread producer([&](){for(UInt32 i=0;i<10000;++i)d.refillRXReserve();});
    for(UInt32 i=0;i<10000;++i){auto p=d.takeRXPacket(14+i%1500);assert(p && p->len==14+i%1500);d.freePacket(p);}
    producer.join();assert(d._rxReserveCount<=256);
    d.pauseRXReserve();assert(!d._rxReserveTimer->armed && !d._rxReserveTimer->enabled);
    d.releaseRXReserve();assert(!d.live && !d._rxReserveTimer && !d._rxReserveLock && !d._rxReserveWorkLoop);
    d.releaseRXReserve();assert(!d.live);
    Ath10kPCIDevice fallback;fallback.fail=true;assert(fallback.prepareRXReserve());
    assert(!fallback._rxReserveCount && fallback._perf.reserveFailures==4);
    fallback.fail=false;auto p=fallback.takeRXPacket(60);assert(p && p->len==60);fallback.freePacket(p);
    fallback.releaseRXReserve();assert(!fallback.live);
}
