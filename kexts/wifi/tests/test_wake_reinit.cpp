// Sleep/wake with the in-place firmware reload (Ath10kPower.cpp): the actual
// power methods (priority sleep notification, controlPowerState) and the
// actual quiesce/re-initialisation methods run against a mock device whose
// DMA engine models what the QCA6174 can touch. Every buffer/ring the
// hardware was given must still be allocated, with the generation it was given
// with, whenever the "hardware" runs; freeing one while it is programmed
// aborts the test (the "no ring touched after free" rule). 10 cycles with a
// join between them, a failing reload (guarded restart fallback, then a clean
// reload on the next wake), a sleep during a reload, a double wake, and a
// quarantine (no reload, no free) at the end. Then: a sleep that arrives while
// the reload is inside probeTarget and outlasts the bounded wait (quarantine,
// no reset, no free), and a reload with no work loop (no gated step, no crash).
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <set>
#include <vector>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include "Ath10kWMI.hpp"
#include "Ath10kRxSetup.hpp"
#include "Ath10kBoot.hpp"
#include "Ath10kRuntimePower.hpp"
#include "Ath10kPower.hpp"
#include "Ath10kPerformance.hpp"
using IOReturn=int;using vm_size_t=unsigned long;using IOOptionBits=unsigned;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNotReady=2,kIOReturnNoMemory=3,kIOReturnError=4;
constexpr unsigned kIOMessageSystemWillSleep=1,kIOMessageSystemHasPoweredOn=2,kIOMessageSystemWillNotSleep=3;
static void IOLog(const char *,...){}
static unsigned sleeps=0;static void (*sleepHook)()=nullptr;static void IOSleep(unsigned){++sleeps;if(sleepHook)sleepHook();}
static UInt64 clockMs=1000;
UInt64 Ath10k::uptimeNS(){return clockMs*1000000ull;}
static bool OSCompareAndSwap(UInt32 old,UInt32 next,volatile UInt32 *address){if(*address!=old)return false;*address=next;return true;}
class Ath10kPCIDevice;using OSObject=Ath10kPCIDevice;using IOService=Ath10kPCIDevice;
#define OSDynamicCast(type,p) (static_cast<type *>(p))
struct IONotifier{bool removed=false;void remove(){removed=true;}};
static IONotifier notifier;
using Handler=IOReturn(*)(void *,void *,UInt32,IOService *,void *,vm_size_t);
static IONotifier *registerPrioritySleepWakeInterest(Handler,void *,void *){return &notifier;}
struct IOPowerStateChangeNotification{void *powerRef=nullptr;unsigned long returnValue=123;};
static void acknowledgeSleepWakeNotification(void *){}

// --- Hardware model --------------------------------------------------------
struct MockDMA;
struct Hardware {
    struct Given{const MockDMA *buffer;UInt32 generation;};
    std::vector<Given> programmed; // what the target may DMA to/from
    bool running=false;            // firmware alive (after probe, before reset)
    unsigned dmaCycles=0;
    void give(const MockDMA *b);
    void reset(){programmed.clear();running=false;}
    bool holds(const MockDMA *b) const {for(auto &g:programmed)if(g.buffer==b)return true;return false;}
    void dma(); // one burst of target DMA to every buffer it was given
} hw;
static UInt32 nextGeneration=1;static int liveBuffers=0;static unsigned freed=0;
struct MockDMA {
    enum class Role{Ring,Payload};
    bool live=false;UInt32 generation=0;UInt64 bytes=0;
    bool allocate(UInt64 size,UInt64=1,void * =nullptr,Role=Role::Ring){release();live=true;generation=nextGeneration++;bytes=size;++liveBuffers;return true;}
    void release(){
        if(!live)return;
        assert(!hw.holds(this) && "a buffer the target may still DMA to was freed");
        live=false;--liveBuffers;++freed;
    }
    bool allocated() const {return live;}
};
using Ath10kDMABuffer=MockDMA;
void Hardware::give(const MockDMA *b){assert(b->live);programmed.push_back({b,b->generation});}
void Hardware::dma(){
    if(!running)return;
    for(auto &g:programmed){assert(g.buffer->live && g.buffer->generation==g.generation && "target DMA into a freed or reallocated buffer");}
    ++dmaCycles;
}

struct Gate{
    Ath10kPCIDevice *owner=nullptr;bool inside=false;
    using Action=IOReturn(*)(OSObject *,void *,void *,void *,void *);
    IOReturn runAction(Action a,Ath10kPCIDevice *d,void *p0=nullptr,void *p1=nullptr,void *p2=nullptr,void *p3=nullptr);
    template<class T> void removeEventSource(T *){}
};
struct Timer{bool enabled=true,armed=false;void cancelTimeout(){armed=false;}void disable(){enabled=false;armed=false;}void enable(){enabled=true;}
    IOReturn setTimeoutMS(unsigned){if(!enabled)return kIOReturnError;armed=true;return kIOReturnSuccess;}};
static int irqSources=0;
struct IRQSource{void disable(){}void release(){--irqSources;delete this;}};
using IOFilterInterruptEventSource=IRQSource;
struct Regs{bool hooked=false;void setAccessHook(void *a,void *,void *){hooked=a!=nullptr;}};
constexpr UInt8 kIOPCIConfigCommand=4;
struct PCI{unsigned writes=0;UInt16 command=0;bool gone=false;template<class... T>void setProperty(T...){++writes;}
    void setMemoryEnable(bool on){command=on?(command|2):(command&~2);}void setBusMasterEnable(bool on){command=on?(command|4):(command&~4);}
    UInt16 configRead16(UInt8 offset){assert(offset==kIOPCIConfigCommand);return gone?0xffff:command;}};
using thread_call_t=int *;
static bool callPending=false;
static bool thread_call_enter(thread_call_t){const bool was=callPending;callPending=true;return was;}

class Ath10kPCIDevice {
public:
    // --- state the production methods use (names as in Ath10kPCIDevice.hpp)
    Gate loop;Gate *_wmiWorkLoop=&loop;
    Ath10k::PowerState _power{};volatile UInt32 _powerIOBlock=0;IONotifier *_powerNotifier=nullptr;
    bool _systemSuspended=false,_dmaQuarantine=false,_suspending=false;volatile bool _wakeReinitRequested=false;
    int callStorage=0;thread_call_t _deferredProbeCall=&callStorage;volatile UInt32 _deferredProbeState=2;
    PCI pci;PCI *_pci=&pci;bool inactive=false;bool isInactive() const {return inactive;}
    Ath10k::ConnectionState _connection{};Ath10k::RegulatoryPolicy _regulatory{};
    Ath10k::WMIScanState _wmiScan{};Ath10k::WMIBSSCache _wmiBSS{};
    Ath10k::WMIEchoState _wmiEcho{},_filterEcho{},_connectionEcho{};
    Ath10k::RxSetupState _rxSetup{};Ath10k::WMIVdevCreateState _wmiVdevCreate{};
    Ath10k::WMIScanChannelsState _wmiRadio[5]{},_wmiScanChannels{};
    Ath10k::WMIScanOptions _scanOptions{};UInt64 _scanProbeMask=0;bool _scanChannelsResend=false;
    Ath10k::WMIInitState _wmiEvents{},_wmiInit{};Ath10k::WMIRuntimeState _wmiLive{};
    Ath10k::WMIBootstrapState _wmi{};Ath10k::BootState _boot{};
    Ath10k::RuntimePowerState _runtimePower{};bool _runtimePowerCleanup=false,_runtimeASPM=false;
    volatile bool _runtimeIRQArmed=false,_runtimeIRQMaskFailed=false;bool _runtimeIRQDeferred=false;UInt32 _runtimeIRQUnmaskStep=0;
    UInt64 _nextRxDoneRetryMs=0;UInt32 _lastMaintenanceRxPackets=0,_lastMaintenanceTxPackets=0;bool _dataTXStalled=false;
    OSObject *_controlOwner=nullptr;
    bool _ceDiagTransferUncertain=false,_bmiTransferUncertain=false,_htcControlExchangeUncertain=false;
    bool _runtimeActive=false,_runtimeInterruptsEnabled=false;
    IRQSource *_runtimeIRQ=nullptr;bool _runtimeIRQAdded=false;Regs _regs;
    Timer timer;Timer *_wmiTimer=&timer;Timer reserveTimer;Timer *_rxReserveTimer=&reserveTimer;Gate reserveLoop;Gate *_rxReserveWorkLoop=&reserveLoop;
    void *_dmaMapper=nullptr;
    struct DataBuffers{MockDMA payload,command,fragments;};
    DataBuffers _dataBuffers[Ath10k::kDataTxSlots]{};
    MockDMA _connectionCommand,_packetBuffer,_packetCommand,_packetFragments;
    MockDMA ceRings[8],wmiRx,htcReady,hostMemory[4],httRing,httPool;
    static constexpr UInt32 kSleepQuiesceMs=500,kWakeReinitWaitMs=30000;
    // --- mock behaviour knobs and counters
    bool failProbe=false,quarantineOnReset=false,stuckTeardown=false;unsigned probes=0,arms=0,teardownTicks=0,linkDowns=0;
    void (*duringProbe)(Ath10kPCIDevice &)=nullptr; // runs inside probeTarget, rings programmed, target running (another thread)
    template<class... T>void setProperty(T...){}
    void publishWMIRuntimeProperties(){}void publishDiagnosticProperties(){}void publishConnection(){}
    void networkLink(bool up){if(!up)++linkDowns;}
    void stopRuntimeWorkerIRQ(){_runtimeIRQArmed=false;}
    // endWMIRuntime as in Ath10kPCIDevice.cpp: IRQs off, target reset (no
    // more DMA), the connection marked failed; a failed reset quarantines.
    void endWMIRuntime(const char *reason){
        assert(loop.inside);
        if(!_wmiLive.active)return;
        _runtimeIRQArmed=false;_runtimeActive=_runtimeInterruptsEnabled=false;
        pci.setBusMasterEnable(false); // quiesceTargetAfterBMI
        if(quarantineOnReset){_dmaQuarantine=true;hw.running=false;} // reset unverified: mappings stay with the target
        else { // verified reset: the target holds nothing; the release path's owners are cleared
            hw.reset();_connection.txOwned=_connection.wmiOwned=false;_wmi.rxOwned=_boot.rxOwned=false;
            _wmiInit.txOwned=_wmiInit.hostMemoryOwned=false;_rxSetup.txOwned=_rxSetup.ringOwned=false;
        }
        if(_connection.stage!=Ath10k::LinkStage::Disabled){_connection.stage=Ath10k::LinkStage::Failed;_connection.error=reason;}
        _wmiLive.active=false;_wmiLive.status=reason;timer.disable();reserveTimer.disable();
    }
    // The first start's resources (probeTarget's clearTargetProbeState frees and
    // reallocates the BMI/HTC/WMI/HTT buffers; allocateCERings the rings).
    bool allocateCERings(){
        for(auto &r:ceRings){if(!r.allocate(4096))return false;}
        return true;
    }
    bool probeTarget(){
        ++probes;assert(!loop.inside && !hw.running && hw.programmed.empty() && !_regs.hooked && !_runtimeIRQ);
        assert((pci.command&6)==6); // D0 with bus mastering before the target is touched
        wmiRx.allocate(2048);htcReady.allocate(2048);for(auto &m:hostMemory)m.allocate(65536);
        for(auto &r:ceRings){hw.give(&r);}
        hw.give(&wmiRx);hw.give(&htcReady);
        for(auto &m:hostMemory){hw.give(&m);}
        hw.running=true;clockMs+=5; // the reload takes time (boot ~2-4 s on hardware)
        if(duringProbe)duringProbe(*this);
        if(failProbe){hw.reset();return false;} // the real failure paths quiesce the target before returning
        _runtimeActive=true;_wmiInit.ready=true;_wmiLive.active=true;timer.enable();
        return true;
    }
    static IOReturn controlWMIRuntime(OSObject *owner,void *reason,void *,void *,void *){
        auto *self=owner;assert(!reason && self->loop.inside);
        if(!self->_wmiLive.active || self->timer.setTimeoutMS(10))return kIOReturnNotReady;
        assert(!self->powerIOBlocked());
        self->_runtimeIRQ=new IRQSource;++irqSources;self->_runtimeIRQAdded=true;self->_runtimeIRQArmed=true;self->_regs.hooked=true;
        ++self->arms;return kIOReturnSuccess;
    }
    // Normal runtime timer: rx setup, vdev, then the connection's DMA; the
    // disconnect sequence completes after a few ticks (vdev down, keys, peer, stop).
    void tick(){
        if(!_wmiLive.active || powerIOBlocked())return;
        hw.dma();
        auto &s=_connection;
        if(!stuckTeardown && (s.disconnect!=Ath10k::DisconnectStage::None || s.cancelRequested)){
            if(++teardownTicks>=3){Ath10k::resetDisconnected(s);s.txOwned=false;s.dataPending=0;teardownTicks=0;}
        }
        if(_wmiScan.cancelRequested){_wmiScan.completed=true;}
    }
    // deferredProbeCall: the probe thread.
    void probeThread(){
        assert(callPending);callPending=false;
        OSCompareAndSwap(0,1,&_deferredProbeState);
        assert(_wakeReinitRequested);runWakeReinit();
        OSCompareAndSwap(1,2,&_deferredProbeState);
    }
    // --- production methods
    bool preparePowerNotifications();void releasePowerNotifications();bool powerIOBlocked();bool powerRestartAllowed();
    void requestPowerEvent(Ath10k::PowerEvent);void publishPowerState();
    static IOReturn controlPowerState(OSObject *,void *,void *,void *,void *);
    static IOReturn powerNotification(void *,void *,UInt32,IOService *,void *,vm_size_t);
    void quiesceForSleep();static IOReturn controlSleepQuiesce(OSObject *,void *,void *,void *,void *);
    void waitForWakeReinit();bool queueWakeReinit();bool wakeDMAIdle() const;void resetRuntimeServices();
    static IOReturn controlWakeReinit(OSObject *,void *,void *,void *,void *);void runWakeReinit();
};
IOReturn Gate::runAction(Action a,Ath10kPCIDevice *d,void *p0,void *p1,void *p2,void *p3){
    assert(!inside);d->tick(); // the runtime timer runs between gated actions
    inside=true;const IOReturn r=a(d,p0,p1,p2,p3);inside=false;return r;
}
#include "power_methods.inc"
#include "wake_reinit_methods.inc"

static void sleepWake(Ath10kPCIDevice &d){
    IOPowerStateChangeNotification note;note.powerRef=&d;
    d.powerNotification(&d,nullptr,kIOMessageSystemWillSleep,nullptr,&note,sizeof(note));
    clockMs+=1000;d.requestPowerEvent(Ath10k::PowerEvent::DeviceOff);
    assert(!hw.running && d._systemSuspended && d.powerIOBlocked());
    clockMs+=60000;d.requestPowerEvent(Ath10k::PowerEvent::DeviceOn);
    clockMs+=5;d.powerNotification(&d,nullptr,kIOMessageSystemHasPoweredOn,nullptr,nullptr,0);
}
static void join(Ath10kPCIDevice &d){
    auto &s=d._connection;s.stage=Ath10k::LinkStage::Connected;s.secure=true;s.portAuthorized=true;s.securityPhase=5;
    memset(s.temporalKey,0x5a,16);s.txOwned=true;s.dataPending=2; // a frame in flight when the lid closes
    d._wmiScan.completed=true;d._wmiScan.attempted=true;++d._wmiScan.generation;d._wmiBSS.onChannelMask|=1ull<<15;
    for(int i=0;i<5;++i)d.tick();
    assert(hw.dmaCycles>0);
}
static int baseline=0;static Ath10kPCIDevice *hookDevice=nullptr;static unsigned hookSleeps=0;
static void checkAfterReinit(Ath10kPCIDevice &d,unsigned reinits,UInt32 generation){
    const auto &p=d._power;
    assert(!p.reinitRunning && p.wakeResult==Ath10k::WakeResult::Reinit && p.reinits==reinits && !p.needsRestart && !strcmp(p.status,"active"));
    assert(!d.powerIOBlocked() && d._wmiLive.active && !d._suspending && !d._wakeReinitRequested && d._deferredProbeState==2);
    assert(d.timer.enabled && d.timer.armed && d._runtimeIRQ && irqSources==1 && d._regs.hooked && d.reserveTimer.enabled && d.reserveTimer.armed);
    // Fresh connection, no key material, configuration and correlation kept.
    const auto &s=d._connection;assert(s.stage==Ath10k::LinkStage::Disabled && !s.txOwned && !s.dataPending && !s.portAuthorized);
    for(auto b:s.temporalKey)assert(!b);
    assert(d._wmiScan.generation==generation && !d._wmiScan.completed && !d._wmiScan.attempted && d._wmiBSS.onChannelMask==(1ull<<15));
    assert(!d._wmiVdevCreate.attempted && !d._rxSetup.stage && !d._wmiScanChannels.attempted && !d._controlOwner && p.wakeMs==5);
    // Memory: exactly the first start's set of live buffers, nothing stale held by the target.
    if(!baseline)baseline=liveBuffers;
    assert(liveBuffers==baseline && !d._packetBuffer.live && !d._connectionCommand.live);
    for(auto &g:hw.programmed)assert(g.buffer->live && g.buffer->generation==g.generation);
    hw.dma();
}
// The model must catch the bug it exists for: freeing (or reallocating) a
// buffer the target still holds aborts, in a child process.
static void modelSelfTest(){
    for(int mode=0;mode<2;++mode){
        const pid_t child=fork();assert(child>=0);
        if(!child){
            MockDMA ring;ring.allocate(64);hw.give(&ring);hw.running=true;
            if(mode)ring.allocate(64);else ring.release();
            _exit(0);
        }
        int status=0;waitpid(child,&status,0);assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
    }
}
int main(){
    modelSelfTest();
    Ath10kPCIDevice d;d.pci.command=6;
    // First start: rings, probe, arm (what runDeferredProbe does).
    assert(d.preparePowerNotifications() && d.allocateCERings() && d.probeTarget());
    for(auto &b:d._dataBuffers){b.payload.allocate(2048);b.command.allocate(256);b.fragments.allocate(16);}
    d.loop.runAction(Ath10kPCIDevice::controlWMIRuntime,&d);
    d._connection.beaconTimeoutMs=45000;d._connection.radio.requestedPS=true;
    unsigned reinits=0;
    for(unsigned cycle=1;cycle<=10;++cycle){
        // No join while the runtime is down (after the failed reload of cycle 4).
        if(d._wmiLive.active){join(d);d._packetBuffer.allocate(2048);d._connectionCommand.allocate(256);}
        const UInt32 generation=d._wmiScan.generation;
        const unsigned probes=d.probes;
        if(cycle==4)d.failProbe=true;    // firmware reload fails once
        d.stuckTeardown=cycle==7;          // the AP teardown does not finish: the bounded quiesce gives up, the reset covers it
        if(cycle==10)d.quarantineOnReset=true; // the target reset cannot be verified
        sleepWake(d);
        // Sleep: the connection was taken down by the normal runtime before the reset.
        assert(d.linkDowns>0 && d._power.lastSleepMs && d._power.lastWakeMs>d._power.lastSleepMs);
        if(cycle==10){
            assert(d._dmaQuarantine && !callPending && d._power.wakeResult==Ath10k::WakeResult::Quarantined && d.powerIOBlocked());
            break; // terminal: nothing is freed, the mappings stay with the device
        }
        assert(callPending && d._power.reinitRunning && !d._power.needsRestart);
        // A second wake notification while the reload is queued changes nothing.
        d.powerNotification(&d,nullptr,kIOMessageSystemHasPoweredOn,nullptr,nullptr,0);
        assert(callPending && d._power.reinitRunning);
        d.probeThread();
        assert(d.probes==probes+1);
        if(cycle==4){
            const auto &p=d._power;
            assert(p.wakeResult==Ath10k::WakeResult::FallbackRestart && p.needsRestart && p.reinitFailures==1 && !p.reinitRunning);
            assert(d.powerRestartAllowed() && !d._wmiLive.active && !hw.running && hw.programmed.empty() && !strcmp(p.status,"wake-restart-required"));
            d.failProbe=false;
            // The guarded restart would follow; instead the next wake reloads in place again.
            continue;
        }
        checkAfterReinit(d,++reinits,generation);
        if(cycle==6){ // idempotent: a stray repeat of the reset step frees nothing new
            const int live=liveBuffers;d.loop.inside=true;d.resetRuntimeServices();d.resetRuntimeServices();d.loop.inside=false;
            assert(liveBuffers==live && d._wmiScan.generation==generation);
            d._wmiLive.active=true;
        }
        if(cycle==8){ // a sleep while a reload runs waits for it; one that finishes within the bound leaves no quarantine
            hookDevice=&d;hookSleeps=0;d._power.reinitRunning=true;const unsigned before=sleeps;
            sleepHook=[]{if(++hookSleeps==100)hookDevice->_power.reinitRunning=false;};
            d.waitForWakeReinit();sleepHook=nullptr;
            assert(sleeps-before==100 && !d._dmaQuarantine && !d._power.reinitRunning);
        }
    }
    assert(reinits==8 && hw.dmaCycles>=10);
    // Reload disabled (ath10k_wake_reinit=0): the guarded restart, as before.
    hw=Hardware{}; // a second, independent device (the quarantined one keeps its mappings)
    Ath10kPCIDevice legacy;legacy.pci.command=6;legacy._power.reinitEnabled=false;legacy.allocateCERings();legacy.probeTarget();
    legacy.loop.runAction(Ath10kPCIDevice::controlWMIRuntime,&legacy);
    hw.reset();legacy._wmiLive.active=true;sleepWake(legacy);
    assert(!callPending && legacy._power.needsRestart && legacy.powerRestartAllowed() && legacy._power.wakeResult==Ath10k::WakeResult::FallbackRestart);

    // A sleep while the reload is inside probeTarget (the probe thread, outside
    // the gate, rings programmed, target running) that outlasts the 30 s wait:
    // quarantine. The target is not reset and nothing is freed under it; the
    // second wait of the same sleep returns at once; the late reload fails into
    // the quarantine and the next wake does not reload again.
    hw=Hardware{};
    Ath10kPCIDevice stuck;stuck.pci.command=6;stuck.allocateCERings();stuck.probeTarget();
    stuck.loop.runAction(Ath10kPCIDevice::controlWMIRuntime,&stuck);
    sleepWake(stuck);
    assert(callPending && stuck._power.reinitRunning);
    stuck.duringProbe=[](Ath10kPCIDevice &x){
        x.duringProbe=nullptr;
        const unsigned before=sleeps,freedBefore=freed;const size_t given=hw.programmed.size();
        IOPowerStateChangeNotification note;note.powerRef=&x;
        x.powerNotification(&x,nullptr,kIOMessageSystemWillSleep,nullptr,&note,sizeof(note));
        x.requestPowerEvent(Ath10k::PowerEvent::DeviceOff);
        assert(sleeps-before==Ath10kPCIDevice::kWakeReinitWaitMs/10); // one bounded wait, not one per caller
        assert(x._dmaQuarantine && x._power.reinitRunning && x._systemSuspended && x.powerIOBlocked() && !note.returnValue);
        assert(!strcmp(x._power.status,"suspended-dma-quarantined") && !strcmp(x._power.wakeDetail,"reinit-timeout"));
        assert(hw.running && hw.programmed.size()==given && given && freed==freedBefore); // no reset, no free
    };
    stuck.probeThread();
    {const auto &p=stuck._power;
     assert(!p.reinitRunning && p.reinitFailures==1 && p.needsRestart && p.wakeResult==Ath10k::WakeResult::Quarantined);
     assert(!strcmp(p.status,"wake-dma-quarantined") && !strcmp(p.wakeDetail,"dma-quarantine") && !stuck._wmiLive.active);}
    stuck.requestPowerEvent(Ath10k::PowerEvent::DeviceOn);
    stuck.powerNotification(&stuck,nullptr,kIOMessageSystemHasPoweredOn,nullptr,nullptr,0);
    assert(!callPending && stuck._power.wakeResult==Ath10k::WakeResult::Quarantined && !strcmp(stuck._power.status,"wake-dma-quarantined"));
    {const unsigned before=sleeps;stuck.waitForWakeReinit();assert(sleeps==before);}

    // No work loop (released under a queued reload): queueWakeReinit refuses,
    // and a reload that runs anyway records the failure without a gated step.
    Ath10kPCIDevice orphan;orphan._wmiWorkLoop=nullptr;
    assert(!orphan.queueWakeReinit() && !callPending && !orphan._power.reinitRunning && orphan._deferredProbeState==2);
    orphan._wmiWorkLoop=&orphan.loop;assert(orphan.queueWakeReinit() && callPending); // the loop was the only reason
    callPending=false;orphan._wmiWorkLoop=nullptr;orphan._powerIOBlock=1;
    orphan.runWakeReinit();
    {const auto &p=orphan._power;
     assert(!p.reinitRunning && p.needsRestart && p.reinitFailures==1 && p.wakeResult==Ath10k::WakeResult::FallbackRestart);
     assert(!strcmp(p.wakeDetail,"no-work-loop") && !strcmp(p.status,"wake-restart-required") && orphan.powerRestartAllowed() && !orphan._wakeReinitRequested);}
    {const unsigned before=sleeps;orphan.waitForWakeReinit();assert(sleeps==before);}
    puts("Wake re-init tests passed (10 sleep/wake cycles with joins: target reset before D3, no buffer freed while the target holds it, rings/buffers rebuilt, runtime state reset, keys wiped, generation/hints kept, failure -> guarded restart -> clean reload, double wake, sleep during reload, quarantine, reload disabled, sleep past the reload wait -> quarantine without reset, no work loop).");
}
