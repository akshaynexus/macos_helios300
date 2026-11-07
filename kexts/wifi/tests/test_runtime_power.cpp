#include <cassert>
#include <cstdio>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>
#include "Ath10kWMI.hpp"
#include "Ath10kRuntimePower.hpp"
#include "Ath10kRegisterIO.hpp"
static std::function<void()> onSync;
void OSSynchronizeIO(){if(onSync)onSync();}
static std::function<void(UInt32)> onDelay;
static void IODelay(UInt32 us){if(onDelay)onDelay(us);}
using IOInterruptState=UInt32;
struct IOSimpleLock{std::mutex mutex;std::atomic<bool> held{false},contended{false};};
static IOSimpleLock *IOSimpleLockAlloc(){return new IOSimpleLock;}
static IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *p){if(!p->mutex.try_lock()){p->contended=true;p->mutex.lock();}p->held=true;return 1;}
static void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *p,IOInterruptState s){assert(s==1 && p->held);p->held=false;p->mutex.unlock();}
namespace Ath10k {static UInt64 nowMs=100;UInt64 uptimeNS(){return nowMs*1000000u;}}
struct PCI{UInt16 control=0x8144;bool fail=false;UInt16 configRead16(UInt8 o){assert(o==0x50);return fail?0xffff:control;}void configWrite16(UInt8,UInt16 v){if(!fail)control=v;}};
class Ath10kPCIDevice {
public:
    Ath10kRegisterIO _regs;std::vector<UInt32> bar=std::vector<UInt32>(0x90000/4);
    PCI pci;PCI *_pci=&pci;bool _hasPCIe=true,_aspmDisableAttempted=true;UInt8 _pcieCapOffset=0x40;UInt16 _pcieLinkControlBefore=0x8146;
    IOSimpleLock *_runtimeWakeLock=nullptr;Ath10k::RuntimePowerState _runtimePower{};
    bool _runtimeWakeAsserted=true,_runtimePowerCleanup=false,_runtimeASPM=false,_runtimeIRQArmed=true,_dmaQuarantine=false,blocked=false;
    Ath10k::ConnectionState _connection{};Ath10k::WMIRuntimeState _wmiLive{};Ath10k::WMIScanState _wmiScan{};Ath10k::WMIBootstrapState _wmi{};
    struct {UInt32 pendingMPDUs=0,rxDoneConsecutiveWaits=0;} _rxSetup;
    bool powerIOBlocked(){return blocked;}
    bool prepareRuntimePower();bool finishRuntimePower();bool runtimeASPM(bool);
    void publishRuntimePower();
    template<class... T>void setProperty(T...){}
    const char *idleRuntimePower(bool,bool &);static bool runtimeMMIOBegin(void *,UInt32,UInt64 &);static void runtimeMMIOEnd(void *,UInt32,UInt64);
    Ath10kPCIDevice(){_regs.attach(reinterpret_cast<UInt8 *>(bar.data()),bar.size()*4);_wmiLive.active=true;_wmiScan.completed=true;
        _connection.stage=Ath10k::LinkStage::Connected;_connection.portAuthorized=true;_connection.radio.requestedPS=_connection.radio.appliedPS=true;
        bar[Ath10k::rtcStateOffset(Ath10k::kQca6174RegisterLayout)/4]=Ath10k::kQca6174RegisterLayout.rtcStateValueOn;}
    ~Ath10kPCIDevice(){_regs.detach();delete _runtimeWakeLock;}
};
#include "runtime_power_methods.inc"
int main(){
    Ath10kPCIDevice d;assert(d.prepareRuntimePower());bool asleep=false;
    const auto &l=Ath10k::kQca6174RegisterLayout;const UInt32 rtc=Ath10k::rtcStateOffset(l),wake=Ath10k::socWakeOffset(l);
    assert(!d.idleRuntimePower(false,asleep) && !asleep && d.pci.control==0x8146);
    Ath10k::nowMs=160;assert(!d.idleRuntimePower(false,asleep) && asleep && !d._runtimeWakeAsserted && d.bar[wake/4]==0);
    d.bar[rtc/4]=0;UInt32 delays=0;onDelay=[&](UInt32 us){delays+=us;d.bar[rtc/4]=l.rtcStateValueOn;};
    assert(d._regs.write32(0x100,42) && d._runtimeWakeAsserted && delays==5 && d._runtimePower.wakes==1 && !d._runtimeWakeLock->held);
    UInt32 v=0;assert(d._regs.read32(0x100,v) && v==42);const UInt32 wakes=d._runtimePower.wakes;
    assert(!d._regs.read32(1,v) && !d._regs.write32(0x90000,7) && d._runtimePower.wakes==wakes); // bounds before wake
    d._connection.dataPending=1;Ath10k::nowMs=300;assert(!d.idleRuntimePower(false,asleep) && !asleep);d._connection.dataPending=0;
    d._connection.maintenance.blocked=true;assert(!d.idleRuntimePower(false,asleep) && !asleep);d._connection.maintenance.blocked=false;
    d._connection.eapolPause=true;assert(!d.idleRuntimePower(false,asleep) && !asleep);d._connection.eapolPause=false;
    assert(!d.idleRuntimePower(true,asleep) && !asleep); // deferred IRQ/RX keeps wake
    // Force a second CPU to attempt sleep between the before-MMIO hook and
    // the actual volatile read. It must block until that read and its fence finish.
    std::thread sleeper;bool launched=false;d._runtimeWakeLock->contended=false;
    onSync=[&](){if(launched)return;launched=true;sleeper=std::thread([&](){assert(!d.idleRuntimePower(false,asleep));});
        while(!d._runtimeWakeLock->contended)std::this_thread::yield();
        assert(d._runtimePower.awake);};
    assert(d._regs.read32(0x100,v) && v==42);sleeper.join();onSync=nullptr;assert(asleep);
    d.blocked=true;assert(!d._regs.read32(0x100,v) && !d._runtimeWakeLock->held);
    assert(d.finishRuntimePower() && d._runtimePower.awake && d.pci.control==0x8144); // PM cleanup owns wake before reset
    d.blocked=false;d._runtimePowerCleanup=false;d._runtimePower.awake=false;d.bar[rtc/4]=0xffffffffu;
    assert(!d._regs.write32(0x100,8) && d.bar[0x100/4]==42 && !d._runtimeWakeLock->held);
    d.bar[rtc/4]=0;onDelay=nullptr;assert(!d._regs.read32(0x100,v) && d._runtimePower.failures==2); // bounded timeout, no access
    d.bar[rtc/4]=l.rtcStateValueOn;assert(d._regs.read32(0x100,v) && v==42);
    d._pcieLinkControlBefore=0x8144;assert(d.runtimeASPM(true) && d.pci.control==0x8144 && !d._runtimeASPM); // BIOS disabled ASPM
    d.pci.fail=true;assert(!d.runtimeASPM(true));
    puts("Runtime PCIe power passed: idle gates, original ASPM bits, MMIO wake/order/lock race, PM cleanup, all-ones failure and bounded timeout.");
}
