#include <cassert>
#include <map>
#include <string>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include "Ath10kTarget.hpp"
#include "Ath10kPerformance.hpp"
class Ath10kPCIDevice;
using OSObject=Ath10kPCIDevice;using IOReturn=int;using SInt32=int32_t;
constexpr int kIOReturnSuccess=0;constexpr UInt16 kIOPCIConfigCommand=4;
#define OSDynamicCast(type,obj) (obj)
static void OSIncrementAtomic(volatile SInt32 *p){++*p;}
using IOInterruptState=bool;
struct IOSimpleLock {std::mutex mutex;std::atomic<bool> contended{false};};
static bool lockAllocationFail=false;
static IOSimpleLock *IOSimpleLockAlloc(){return lockAllocationFail?nullptr:new IOSimpleLock;}
static void IOSimpleLockFree(IOSimpleLock *p){delete p;}
static IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *p){
    if(!p->mutex.try_lock()){p->contended=true;p->mutex.lock();}return true;
}
static void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *p,IOInterruptState state){assert(state);p->mutex.unlock();}
namespace Ath10k {
constexpr UInt32 kCEHostIS=0x30,kCEHostIE=0x2c,kCEHostInterruptMask=0x1f,kCEHostCopyCompleteMask=1;
static UInt32 ceBaseAddress(UInt32 id){return kQca6174RegisterLayout.ce0BaseAddress+0x400*id;}
}
struct IOInterruptEventSource {virtual ~IOInterruptEventSource()=default;};
struct IOFilterInterruptEventSource:IOInterruptEventSource {
    inline static bool allocationFail=false;
    bool enabled=false;
    template<class... T>static IOFilterInterruptEventSource *filterInterruptEventSource(T...){return allocationFail?nullptr:new IOFilterInterruptEventSource;}
    void disable(){enabled=false;}void enable(){enabled=true;}void release(){delete this;}
};
struct Timer {unsigned armed=0;bool fail=false;int setTimeoutMS(unsigned n){assert(n==1);armed=n;return fail?1:0;}};
struct WorkLoop {bool fail=false;int addEventSource(IOInterruptEventSource *){return fail?1:0;}};
struct PCI {
    bool fail=false,configFail=false;UInt16 command=0x407;UInt32 unregisters=0;
    int unregisterInterrupt(int n){assert(n==0);++unregisters;return fail?1:0;}
    UInt16 configRead16(UInt16 a){assert(a==4);return command;}
    void configWrite16(UInt16 a,UInt16 v){assert(a==4);if(!configFail)command=v;}
};
struct Registers {
    std::map<UInt32,UInt32> words;UInt32 reads=0,writes=0;bool fail=false;
    std::function<void(UInt32,UInt32)> afterWrite;
    UInt32 failAddress=0xffffffff,badReadAddress=0xffffffff;
    bool read32(UInt32 a,UInt32 &v){++reads;if(fail || a==failAddress)return false;v=a==badReadAddress?0:words[a];return true;}
    bool write32(UInt32 a,UInt32 v){++writes;if(fail || a==failAddress)return false;words[a]=v;if(afterWrite)afterWrite(a,v);return true;}
};
class Ath10kPCIDevice {
public:
    Registers _regs;PCI pci;PCI *_pci=&pci;WorkLoop loop;WorkLoop *_wmiWorkLoop=&loop;Timer timer;Timer *_wmiTimer=&timer;
    bool _irqPrepared=true,_irqMessaged=true,_irqRegistered=true,_runtimeInterruptsEnabled=false;
    int _irqIndex=0;volatile UInt32 _irqObservedCount=0;
    IOFilterInterruptEventSource *_runtimeIRQ=nullptr;
    bool _runtimeIRQAdded=false,_runtimeIRQDeferred=false;
    volatile bool _runtimeIRQArmed=false,_runtimeIRQMaskFailed=false;
    UInt32 _runtimeIRQWorkers=0,_runtimeIRQReschedules=0,services=0;
    struct {bool active=true;} _wmiLive;
    struct {UInt32 pendingMPDUs=0;} _rxSetup;
    const char *_runtimeStatus="prepared";
    const char *serviceError=nullptr;std::string stopped;
    const char *serviceRuntime(){++services;return serviceError;}
    const char *idleRuntimePower(bool,bool &asleep){asleep=false;return nullptr;}
    void endWMIRuntime(const char *error){stopped=error;_wmiLive.active=false;stopRuntimeWorkerIRQ();}
    IOSimpleLock *_runtimeIRQLock=nullptr;
    UInt32 _runtimeIRQUnmaskStep=0,_runtimeIRQUnmaskEnableValue=0;
    bool powerBlocked=false;bool powerIOBlocked(){return powerBlocked;}
    bool maskRuntimeWorkerIRQ();bool unmaskRuntimeWorkerIRQ();
    bool maskRuntimeWorkerIRQLocked();bool unmaskRuntimeWorkerIRQLocked();
    const char *startRuntimeWorkerIRQ();void stopRuntimeWorkerIRQ();
    static bool runtimeIRQFilter(OSObject *,IOFilterInterruptEventSource *);
    static void runtimeIRQAction(OSObject *,IOInterruptEventSource *,int);
    ~Ath10kPCIDevice(){stopRuntimeWorkerIRQ();if(_runtimeIRQ)_runtimeIRQ->release();if(_runtimeIRQLock)IOSimpleLockFree(_runtimeIRQLock);}
};
#include "runtime_irq_methods.inc"
int main(){
    const auto &layout=Ath10k::kQca6174RegisterLayout;
    const UInt32 irqEnable=Ath10k::socCoreOffset(layout,Ath10k::kPCIeIntrEnableAddress);
    const UInt32 irqCore=Ath10k::socCoreOffset(layout,Ath10k::kCoreCtrlAddress);
    // A real MSI may arrive between unmask writes and the flush read. Force
    // that interleaving on a second CPU: it must wait until the readback has
    // completed, then mask the target and leave an action for the work loop.
    Ath10kPCIDevice race;assert(!race.startRuntimeWorkerIRQ());
    // Reproduce the old unprotected transaction: an immediate filter masks
    // IRQ between the final write and readback, yielding a false failure.
    race._regs.afterWrite=[&](UInt32 address,UInt32 value){
        if(address==irqCore && (value&Ath10k::kCoreCtrlPCIeReg31Mask))
            assert(Ath10kPCIDevice::runtimeIRQFilter(&race,race._runtimeIRQ));
    };
    assert(!race.unmaskRuntimeWorkerIRQLocked()); // Deliberately omit outer lock.
    assert(race._runtimeIRQUnmaskStep==5 && race._runtimeIRQUnmaskEnableValue==0);
    race._regs.afterWrite=nullptr;
    for(unsigned iteration=0;iteration<100;++iteration){
        assert(race.maskRuntimeWorkerIRQ());
        std::thread irq;bool accepted=false;
        race._runtimeIRQLock->contended=false;
        race._regs.afterWrite=[&](UInt32 address,UInt32 value){
            if(address!=irqCore || !(value&Ath10k::kCoreCtrlPCIeReg31Mask))return;
            irq=std::thread([&]{accepted=Ath10kPCIDevice::runtimeIRQFilter(&race,race._runtimeIRQ);});
            while(!race._runtimeIRQLock->contended.load())std::this_thread::yield();
        };
        assert(race.unmaskRuntimeWorkerIRQ());irq.join();race._regs.afterWrite=nullptr;
        assert(accepted && race._runtimeIRQUnmaskStep==0 && race._regs.words[irqEnable]==0);
        assert(!(race._regs.words[irqCore]&Ath10k::kCoreCtrlPCIeReg31Mask));
        Ath10kPCIDevice::runtimeIRQAction(&race,race._runtimeIRQ,1);
        assert(race.stopped.empty() && race._regs.words[irqEnable]!=0);
    }
    assert(race._irqObservedCount==101 && race.services==100);
    // Genuine MMIO failures/readback mismatches still fail; zero is never
    // blindly accepted just to suppress the reported runtime error.
    race._regs.badReadAddress=irqEnable;assert(!race.unmaskRuntimeWorkerIRQ());
    assert(race._runtimeIRQUnmaskStep==5 && race._runtimeIRQUnmaskEnableValue==0);
    race._regs.badReadAddress=0xffffffff;
    race._regs.failAddress=irqCore;assert(!race.unmaskRuntimeWorkerIRQ());
    assert(race._runtimeIRQUnmaskStep==2);race._regs.failAddress=0xffffffff;
    race.stopRuntimeWorkerIRQ();const auto stoppedCalls=race._regs.reads+race._regs.writes;
    assert(!race.unmaskRuntimeWorkerIRQ());
    assert(!Ath10kPCIDevice::runtimeIRQFilter(&race,race._runtimeIRQ));
    assert(stoppedCalls==race._regs.reads+race._regs.writes);
    lockAllocationFail=true;{Ath10kPCIDevice bad;assert(bad.startRuntimeWorkerIRQ());assert(!bad._runtimeIRQLock && !bad._runtimeIRQArmed);}
    lockAllocationFail=false;
    Ath10kPCIDevice d;assert(!d.startRuntimeWorkerIRQ());
    assert(d.pci.unregisters==1 && !d._irqRegistered && d._runtimeIRQArmed && d._runtimeIRQ->enabled);
    for(UInt32 ce=1;ce<=3;++ce)assert(d._regs.words[Ath10k::ceBaseAddress(ce)+Ath10k::kCEHostIE]==1);
    assert(!d._regs.words.count(Ath10k::ceBaseAddress(4)+Ath10k::kCEHostIE));
    const auto &l=Ath10k::kQca6174RegisterLayout;
    const UInt32 enable=Ath10k::socCoreOffset(l,Ath10k::kPCIeIntrEnableAddress);
    assert(Ath10kPCIDevice::runtimeIRQFilter(&d,d._runtimeIRQ));
    assert(!d.services && d._irqObservedCount==1 && d._regs.words[enable]==0);
    Ath10kPCIDevice::runtimeIRQAction(&d,d._runtimeIRQ,1);
    assert(d.services==1 && d._regs.words[enable]!=0 && d._runtimeIRQWorkers==1);
    d._rxSetup.pendingMPDUs=65;assert(Ath10kPCIDevice::runtimeIRQFilter(&d,d._runtimeIRQ));
    Ath10kPCIDevice::runtimeIRQAction(&d,d._runtimeIRQ,1);
    assert(d.services==2 && d._runtimeIRQDeferred && d.timer.armed==1 && d._regs.words[enable]==0);
    d._rxSetup.pendingMPDUs=0;d._regs.words[l.ceWrapperBaseAddress]=0x200;
    Ath10kPCIDevice::runtimeIRQAction(&d,d._runtimeIRQ,1);
    assert(d._runtimeIRQDeferred && d._runtimeIRQReschedules==2); // CE arrived during drain
    d._regs.words[l.ceWrapperBaseAddress]=0;d.serviceError="rx-failure";
    Ath10kPCIDevice::runtimeIRQAction(&d,d._runtimeIRQ,1);
    assert(d.stopped=="rx-failure" && !d._runtimeIRQArmed && !d._runtimeIRQ->enabled);
    const UInt32 calls=d._regs.reads+d._regs.writes;
    assert(!Ath10kPCIDevice::runtimeIRQFilter(&d,d._runtimeIRQ));
    Ath10kPCIDevice::runtimeIRQAction(&d,d._runtimeIRQ,1);assert(calls==d._regs.reads+d._regs.writes);
    for(UInt32 mode=0;mode<4;++mode){
        Ath10kPCIDevice bad;if(mode==0)bad.pci.fail=true;
        if(mode==1)IOFilterInterruptEventSource::allocationFail=true;
        if(mode==2)bad.loop.fail=true;
        if(mode==3)bad._regs.fail=true;
        assert(bad.startRuntimeWorkerIRQ() && !bad._runtimeIRQArmed);
        IOFilterInterruptEventSource::allocationFail=false;
    }
    Ath10kPCIDevice mask;assert(!mask.startRuntimeWorkerIRQ());mask._regs.fail=true;
    assert(Ath10kPCIDevice::runtimeIRQFilter(&mask,mask._runtimeIRQ));
    Ath10kPCIDevice::runtimeIRQAction(&mask,mask._runtimeIRQ,1);
    assert(mask.stopped=="runtime-irq-mask" && !mask.services);
    // After WillSleep begins, inaccessible PCI must not be touched by new
    // filter/action/mask/unmask work, even before the gated stop executes.
    Ath10kPCIDevice asleep;assert(!asleep.startRuntimeWorkerIRQ());
    asleep.powerBlocked=true;asleep._regs.fail=true;
    const UInt32 beforeSleep=asleep._regs.reads+asleep._regs.writes;
    assert(!Ath10kPCIDevice::runtimeIRQFilter(&asleep,asleep._runtimeIRQ));
    Ath10kPCIDevice::runtimeIRQAction(&asleep,asleep._runtimeIRQ,1);
    assert(asleep.maskRuntimeWorkerIRQ() && asleep.unmaskRuntimeWorkerIRQ());
    assert(beforeSleep==asleep._regs.reads+asleep._regs.writes && !asleep.services && asleep.stopped.empty());
    asleep.powerBlocked=false;
    assert(Ath10kPCIDevice::runtimeIRQFilter(&asleep,asleep._runtimeIRQ));
    Ath10kPCIDevice::runtimeIRQAction(&asleep,asleep._runtimeIRQ,1);
    assert(asleep.stopped=="runtime-irq-mask"); // Real failure while awake is fatal.
    // INTx must ignore another device sharing the line without masking it.
    const UInt32 cause=Ath10k::socCoreOffset(layout,Ath10k::kPCIeIntrCauseAddress);
    {Ath10kPCIDevice legacy;legacy._irqMessaged=false;assert(!legacy.startRuntimeWorkerIRQ());
     assert(std::string(legacy._runtimeStatus)=="wmi-intx-worker" && legacy.pci.command==7);
     const auto writes=legacy._regs.writes;legacy._regs.words[cause]=1;
     assert(!Ath10kPCIDevice::runtimeIRQFilter(&legacy,legacy._runtimeIRQ));
     assert(legacy._regs.writes==writes && !legacy._irqObservedCount && !legacy.services);
     legacy._regs.words[cause]=Ath10k::kPCIeIntrFirmwareMask;
     assert(Ath10kPCIDevice::runtimeIRQFilter(&legacy,legacy._runtimeIRQ));
     assert(legacy._regs.words[irqEnable]==0 && legacy._irqObservedCount==1);
     Ath10kPCIDevice::runtimeIRQAction(&legacy,legacy._runtimeIRQ,1);
     assert(legacy.services==1 && legacy._regs.words[irqEnable]!=0 && legacy.stopped.empty());
     legacy.powerBlocked=true;const auto reads=legacy._regs.reads;
     assert(!Ath10kPCIDevice::runtimeIRQFilter(&legacy,legacy._runtimeIRQ) && legacy._regs.reads==reads);
     legacy.powerBlocked=false;legacy._regs.words[cause]=0xffffffff;
     assert(Ath10kPCIDevice::runtimeIRQFilter(&legacy,legacy._runtimeIRQ));
     Ath10kPCIDevice::runtimeIRQAction(&legacy,legacy._runtimeIRQ,1);
     assert(legacy.stopped=="runtime-irq-mask" && legacy.services==1);}
    {Ath10kPCIDevice fail;fail._irqMessaged=false;fail.pci.configFail=true;
     assert(std::string(fail.startRuntimeWorkerIRQ())=="runtime-intx-pci-enable" && fail._regs.words[irqEnable]==0);fail.stopRuntimeWorkerIRQ();}
    Ath10kPCIDevice schedule;assert(!schedule.startRuntimeWorkerIRQ());schedule.timer.fail=true;
    schedule._rxSetup.pendingMPDUs=1;
    Ath10kPCIDevice::runtimeIRQAction(&schedule,schedule._runtimeIRQ,1);
    assert(schedule.stopped=="runtime-timer-failed");
}
