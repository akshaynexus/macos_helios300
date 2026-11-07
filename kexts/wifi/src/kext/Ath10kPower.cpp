#include "Ath10kPCIDevice.hpp"
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/OSAtomic.h>

bool Ath10kPCIDevice::preparePowerNotifications()
{
    if(_powerNotifier)return true;
    _powerNotifier=registerPrioritySleepWakeInterest(powerNotification,this,nullptr);
    return _powerNotifier!=nullptr;
}
void Ath10kPCIDevice::releasePowerNotifications()
{
    // remove() synchronizes with outstanding handlers. Detach before PMstop,
    // work-loop release or PCI unmap; no callback may reach a freed provider.
    if(_powerNotifier){_powerNotifier->remove();_powerNotifier=nullptr;}
}
bool Ath10kPCIDevice::powerIOBlocked()
{
    // Atomic load with the OSAtomic barrier, including polling-only builds.
    return !OSCompareAndSwap(0,0,&_powerIOBlock);
}
bool Ath10kPCIDevice::powerRestartAllowed()
{
    return OSCompareAndSwap(2,2,&_powerIOBlock);
}
void Ath10kPCIDevice::publishPowerState()
{
    setProperty("Ath10kPowerNotificationRegistered",_powerNotifier!=nullptr);
    setProperty("Ath10kPowerIOBlocked",powerIOBlocked());
    setProperty("Ath10kPowerSystemSuspended",_systemSuspended);
    setProperty("Ath10kPowerDeviceOn",_power.deviceOn);
    setProperty("Ath10kPowerRootSleeping",_power.rootSleeping);
    setProperty("Ath10kPowerRestartRequired",_power.needsRestart);
    setProperty("Ath10kPowerSleepCount",_power.sleeps,32);
    setProperty("Ath10kPowerWakeCount",_power.wakes,32);
    setProperty("Ath10kPowerNotificationCount",_power.notifications,32);
    setProperty("Ath10kPowerStatus",_power.status);
    // Keep the wake handoff visible on the PCI provider. That object survives
    // the driver terminate/rematch cycle, unlike this service's properties.
    if (_pci && _power.needsRestart) {
        _pci->setProperty("Ath10kRecoveryPhase", _power.status);
        _pci->setProperty("Ath10kRecoverySleepCount", _power.sleeps, 32);
        _pci->setProperty("Ath10kRecoveryWakeCount", _power.wakes, 32);
    }
}
void Ath10kPCIDevice::requestPowerEvent(Ath10k::PowerEvent event)
{
    if(event==Ath10k::PowerEvent::SystemSleep || event==Ath10k::PowerEvent::DeviceOff){
        OSCompareAndSwap(0,1,&_powerIOBlock);
        OSCompareAndSwap(2,1,&_powerIOBlock);
    }
    // Block new filter/timer/output work before waiting for the hardware gate.
    // The priority root notification does not return until existing work and
    // verified IRQ/reset quiescence finish, while PCI is still powered.
    if(_wmiWorkLoop)_wmiWorkLoop->runAction(controlPowerState,this,
        reinterpret_cast<void *>(static_cast<uintptr_t>(event)));
}
IOReturn Ath10kPCIDevice::controlPowerState(OSObject *owner,void *argument,void *,void *,void *)
{
    auto *self=OSDynamicCast(Ath10kPCIDevice,owner);
    if(!self)return kIOReturnBadArgument;
    const auto event=static_cast<Ath10k::PowerEvent>(reinterpret_cast<uintptr_t>(argument));
    auto &p=self->_power;
    if(event==Ath10k::PowerEvent::SystemSleep){++p.notifications;p.rootSleeping=true;}
    else if(event==Ath10k::PowerEvent::DeviceOff)p.deviceOn=false;
    else if(event==Ath10k::PowerEvent::DeviceOn)p.deviceOn=true;
    else if(event==Ath10k::PowerEvent::SystemWake || event==Ath10k::PowerEvent::SleepCancelled){
        ++p.notifications;p.rootSleeping=false;
    }else return kIOReturnBadArgument;
    const bool suspended=p.rootSleeping || !p.deviceOn;
    if(suspended){
        if(!self->_systemSuspended)++p.sleeps;
        self->_systemSuspended=true;p.needsRestart=true;
        OSCompareAndSwap(2,1,&self->_powerIOBlock);
        self->endWMIRuntime("suspended-awaiting-wake");
        p.status=self->_dmaQuarantine?"suspended-dma-quarantined":"suspended-quiesced";
    }else{
        if(self->_systemSuspended)++p.wakes;
        self->_systemSuspended=false;
        // Do not re-arm old CE rings or reuse security state after PCI sleep.
        // The existing app Restart operation performs full stop/rematch/boot.
        // Keep I/O blocked until that new driver instance is ready.
        if(p.needsRestart){
            OSCompareAndSwap(1,2,&self->_powerIOBlock);
            p.status=self->_dmaQuarantine?"wake-dma-quarantined":"wake-restart-required";
            self->_wmiLive.status=p.status;
        }else{
            OSCompareAndSwap(1,0,&self->_powerIOBlock);p.status="active";
        }
    }
    self->publishPowerState();self->publishWMIRuntimeProperties();
    IOLog("Ath10kMac: POWER event=%u suspended=%s device-on=%s root-sleep=%s restart=%s status=%s\n",
        static_cast<UInt32>(event),self->_systemSuspended?"yes":"no",p.deviceOn?"yes":"no",
        p.rootSleeping?"yes":"no",p.needsRestart?"yes":"no",p.status);
    return kIOReturnSuccess;
}
IOReturn Ath10kPCIDevice::powerNotification(void *target,void *,UInt32 message,
                                          IOService *,void *argument,vm_size_t)
{
    auto *self=OSDynamicCast(Ath10kPCIDevice,static_cast<OSObject *>(target));
    if(!self)return kIOReturnBadArgument;
    if(message==kIOMessageSystemWillSleep){
        self->requestPowerEvent(Ath10k::PowerEvent::SystemSleep);
        // Synchronous quiescence: root can proceed to sleep after this return.
        auto *note=static_cast<IOPowerStateChangeNotification *>(argument);
        if(note){note->returnValue=0;acknowledgeSleepWakeNotification(note->powerRef);}
    }else if(message==kIOMessageSystemHasPoweredOn)
        self->requestPowerEvent(Ath10k::PowerEvent::SystemWake);
    else if(message==kIOMessageSystemWillNotSleep)
        self->requestPowerEvent(Ath10k::PowerEvent::SleepCancelled);
    return kIOReturnSuccess;
}
