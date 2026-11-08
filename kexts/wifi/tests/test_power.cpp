#include <cassert>
#include <cstring>
#include <cstdint>
#include "Ath10kPower.hpp"
using IOReturn=int;using vm_size_t=unsigned long;
constexpr int kIOReturnBadArgument=1,kIOReturnSuccess=0;
constexpr unsigned kIOMessageSystemWillSleep=1,kIOMessageSystemHasPoweredOn=2,kIOMessageSystemWillNotSleep=3;
class Ath10kPCIDevice;using OSObject=Ath10kPCIDevice;using IOService=Ath10kPCIDevice;
#define OSDynamicCast(type,p) (p)
static void IOLog(const char *,...){}
static bool OSCompareAndSwap(UInt32 old,UInt32 next,volatile UInt32 *address){
    if(*address!=old)return false;
    *address=next;return true;
}
struct IONotifier{bool removed=false;void remove(){removed=true;}};
static IONotifier notifier;static bool registrationFail=false;static unsigned registrations=0;
using Handler=IOReturn(*)(void *,void *,UInt32,IOService *,void *,vm_size_t);
static IONotifier *registerPrioritySleepWakeInterest(Handler,void *,void *){++registrations;return registrationFail?nullptr:&notifier;}
struct IOPowerStateChangeNotification{void *powerRef=nullptr;unsigned long returnValue=123;};
static void *acked=nullptr;static void acknowledgeSleepWakeNotification(void *ref){acked=ref;}
struct Gate{
 bool inside=false;
 template<class F> int runAction(F fn,Ath10kPCIDevice *d,void *p){inside=true;int r=fn(d,p,nullptr,nullptr,nullptr);inside=false;return r;}
};
namespace Ath10k{static UInt64 uptimeNS(){static UInt64 t=0;return t+=1000000;}}
class Ath10kPCIDevice{
public:
 bool _suspending=false;unsigned quiesces=0,reinitQueued=0;bool reinitOK=false;
 void waitForWakeReinit(){}void quiesceForSleep(){++quiesces;}
 bool queueWakeReinit(){if(!reinitOK)return false;++reinitQueued;_power.reinitRunning=true;_power.needsRestart=false;return true;}
 Gate loop;Gate *_wmiWorkLoop=&loop;
 Ath10k::PowerState _power{};volatile UInt32 _powerIOBlock=0;
 IONotifier *_powerNotifier=nullptr;bool _systemSuspended=false,_dmaQuarantine=false;
 struct{bool active=true;const char *status="active";} _wmiLive;
 unsigned resets=0,published=0;bool resetQuarantines=false,pciPowered=true;
 bool preparePowerNotifications();void releasePowerNotifications();bool powerIOBlocked();bool powerRestartAllowed();
 void requestPowerEvent(Ath10k::PowerEvent);void publishPowerState();
 static IOReturn controlPowerState(OSObject *,void *,void *,void *,void *);
 static IOReturn powerNotification(void *,void *,UInt32,IOService *,void *,vm_size_t);
 template<class... T> void setProperty(T...){}
 struct PCIMock{unsigned recoveryWrites=0;template<class... T> void setProperty(T...){++recoveryWrites;}} pciMock;PCIMock *_pci=&pciMock;
 void publishWMIRuntimeProperties(){++published;}
 void endWMIRuntime(const char *reason){
  assert(loop.inside && powerIOBlocked());
  if(!_wmiLive.active)return;
  assert(pciPowered);++resets;_wmiLive.active=false;_wmiLive.status=reason;
  if(resetQuarantines)_dmaQuarantine=true;
 }
};
#include "power_methods.inc"
int main(){
 using Ath10k::PowerEvent;
 Ath10kPCIDevice d;assert(d.preparePowerNotifications() && registrations==1);
 assert(d.preparePowerNotifications() && registrations==1);
 // Priority root notice quiesces while PCI is still on, before acknowledging.
 IOPowerStateChangeNotification note;note.powerRef=&d;
 assert(d.powerNotification(&d,nullptr,kIOMessageSystemWillSleep,nullptr,&note,sizeof(note))==0);
 assert(note.returnValue==0 && acked==&d && d.resets==1 && d.powerIOBlocked());
 d.pciPowered=false;d.requestPowerEvent(PowerEvent::DeviceOff);
 d.requestPowerEvent(PowerEvent::DeviceOff);
 assert(d.resets==1 && d._power.sleeps==1 && d._systemSuspended);
 // DeviceOn can occur before SystemHasPoweredOn; it must not permit recovery.
 d.pciPowered=true;d.requestPowerEvent(PowerEvent::DeviceOn);
 assert(d._systemSuspended && d._power.needsRestart && d.powerIOBlocked() && !d.powerRestartAllowed());
 d.powerNotification(&d,nullptr,kIOMessageSystemHasPoweredOn,nullptr,nullptr,0);
 assert(!d._systemSuspended && d._power.wakes==1 && !d._wmiLive.active && d.powerIOBlocked() && d.powerRestartAllowed());
 assert(!strcmp(d._wmiLive.status,"wake-restart-required"));
 d.powerNotification(&d,nullptr,kIOMessageSystemHasPoweredOn,nullptr,nullptr,0);
 assert(d._power.wakes==1 && d.resets==1);d.releasePowerNotifications();
 assert(notifier.removed && !d._powerNotifier);d.releasePowerNotifications();
 // A second sleep before the app completed rematching blocks Restart again.
 d.requestPowerEvent(PowerEvent::SystemSleep);
 assert(d._systemSuspended && !d.powerRestartAllowed() && d._power.sleeps==2 && d.resets==1);
 // DeviceOff may be the first notice; later root notice must not access PCI.
 Ath10kPCIDevice deviceFirst;deviceFirst.requestPowerEvent(PowerEvent::DeviceOff);
 deviceFirst.pciPowered=false;deviceFirst.requestPowerEvent(PowerEvent::SystemSleep);
 assert(deviceFirst.resets==1 && deviceFirst._systemSuspended);
 // Root wake before the device returns also stays blocked until both are on.
 Ath10kPCIDevice late;late.requestPowerEvent(PowerEvent::SystemSleep);
 late.requestPowerEvent(PowerEvent::DeviceOff);late.requestPowerEvent(PowerEvent::SystemWake);
 assert(late._systemSuspended);late.requestPowerEvent(PowerEvent::DeviceOn);
 assert(!late._systemSuspended && late.powerIOBlocked() && late.resets==1);
 // Cancelled sleep after quiescence still needs a clean boot, not old DMA.
 Ath10kPCIDevice cancel;cancel.requestPowerEvent(PowerEvent::SystemSleep);
 cancel.requestPowerEvent(PowerEvent::SleepCancelled);
 assert(!cancel._systemSuspended && cancel._power.needsRestart && cancel.powerIOBlocked());
 // Initial DeviceOn and unsolicited wake never stop a running connection.
 Ath10kPCIDevice initial;initial.requestPowerEvent(PowerEvent::DeviceOn);
 initial.requestPowerEvent(PowerEvent::SystemWake);initial.requestPowerEvent(PowerEvent::SleepCancelled);
 assert(initial._wmiLive.active && !initial.resets && !initial.powerIOBlocked() && !initial._power.needsRestart);
 // Ambiguous DMA ownership remains a blocker after wake.
 Ath10kPCIDevice fail;fail.resetQuarantines=true;fail.requestPowerEvent(PowerEvent::SystemSleep);
 fail.requestPowerEvent(PowerEvent::SystemWake);
 assert(fail._dmaQuarantine && fail.powerIOBlocked() && !strcmp(fail._power.status,"wake-dma-quarantined"));
 // In-place wake re-initialisation: the wake hands over to the probe thread, I/O stays
 // blocked (no Restart allowed), the front end is told (Info bit 8) not to restart.
 Ath10kPCIDevice inplace;inplace.reinitOK=true;
 IOPowerStateChangeNotification n2;n2.powerRef=&inplace;
 inplace.powerNotification(&inplace,nullptr,kIOMessageSystemWillSleep,nullptr,&n2,sizeof(n2));
 assert(inplace.quiesces==1 && inplace.resets==1 && inplace._power.lastSleepMs);
 inplace.powerNotification(&inplace,nullptr,kIOMessageSystemHasPoweredOn,nullptr,nullptr,0);
 assert(inplace.reinitQueued==1 && inplace._power.reinitRunning && !inplace._power.needsRestart && inplace._power.lastWakeMs>inplace._power.lastSleepMs);
 assert(inplace.powerIOBlocked() && !inplace.powerRestartAllowed() && !strcmp(inplace._power.status,"wake-reinit-queued"));
 // Quarantined DMA never re-initialises in place.
 Ath10kPCIDevice q;q.reinitOK=true;q.resetQuarantines=true;q.requestPowerEvent(PowerEvent::SystemSleep);q.requestPowerEvent(PowerEvent::SystemWake);
 assert(!q.reinitQueued && q._power.wakeResult==Ath10k::WakeResult::Quarantined);
 registrationFail=true;Ath10kPCIDevice alloc;assert(!alloc.preparePowerNotifications() && !alloc._powerNotifier);
}
