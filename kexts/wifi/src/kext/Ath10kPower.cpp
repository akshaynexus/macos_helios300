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
    setProperty("Ath10kPowerWakeReinit",_power.reinitEnabled);
    setProperty("Ath10kPowerWakeReinitRunning",_power.reinitRunning);
    setProperty("Ath10kPowerWakeReinits",_power.reinits,32);
    setProperty("Ath10kPowerWakeReinitFailures",_power.reinitFailures,32);
    setProperty("Ath10kPowerLastSleepMs",_power.lastSleepMs,64);
    setProperty("Ath10kPowerLastWakeMs",_power.lastWakeMs,64);
    setProperty("Ath10kPowerWakeMs",_power.wakeMs,64);
    setProperty("Ath10kPowerWakeResult",Ath10k::wakeResultName(_power.wakeResult));
    setProperty("Ath10kPowerWakeDetail",_power.wakeDetail);
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
    // A wake re-initialisation runs on the probe thread outside the gate; a
    // new sleep must not reset the target underneath it. Wait for it first.
    if(event==Ath10k::PowerEvent::SystemSleep || event==Ath10k::PowerEvent::DeviceOff)waitForWakeReinit();
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
        if(!self->_systemSuspended){++p.sleeps;p.lastSleepMs=Ath10k::uptimeNS()/1000000u;}
        self->_systemSuspended=true;p.needsRestart=true;
        OSCompareAndSwap(2,1,&self->_powerIOBlock);
        // Linux power-off suspend (no WoWLAN): connection and scan were taken
        // down by quiesceForSleep; this stops HTC/WMI/CE, masks IRQs and
        // resets the target, so no DMA runs while PCI goes to D3.
        self->endWMIRuntime("suspended-awaiting-wake");
        p.status=self->_dmaQuarantine?"suspended-dma-quarantined":"suspended-quiesced";
    }else{
        if(self->_systemSuspended){++p.wakes;p.lastWakeMs=Ath10k::uptimeNS()/1000000u;}
        self->_systemSuspended=false;
        // Never re-arm old CE rings or reuse security state after PCI sleep:
        // the wake re-initialisation (probe thread) rebuilds rings and
        // reloads firmware in place; if it cannot run, the guarded restart
        // (terminate + rematch) remains. I/O stays blocked until either is done.
        if(p.reinitRunning){
            // A second wake notification while the reload is queued or running: nothing to do.
        }else if(p.needsRestart && !self->_dmaQuarantine && self->queueWakeReinit()){
            p.status="wake-reinit-queued";self->_wmiLive.status=p.status;
        }else if(p.needsRestart){
            OSCompareAndSwap(1,2,&self->_powerIOBlock);
            p.status=self->_dmaQuarantine?"wake-dma-quarantined":"wake-restart-required";
            p.wakeResult=self->_dmaQuarantine?Ath10k::WakeResult::Quarantined:Ath10k::WakeResult::FallbackRestart;
            self->_wmiLive.status=p.status;
        }else{
            OSCompareAndSwap(1,0,&self->_powerIOBlock);p.status="active";self->_suspending=false;
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
        self->quiesceForSleep();
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

// --- Sleep quiesce and in-place wake re-initialisation -----------------------
// Mirrors Linux ath10k's power-off suspend/resume (no WoWLAN): mac80211 takes
// the connection and the scan down, ath10k_stop -> hif_power_down resets the
// target; ath10k_pci_resume -> hif_power_up (chip reset, CE pipe init) ->
// ath10k_core_start (BMI, firmware, HTC, WMI) on wake. The front end and its
// IO80211 interface stay attached; WCL sees driver-unavailable, then
// driver-available, and rescans/auto-joins.
//
// DMA safety: the target is reset before PCI leaves D0 (endWMIRuntime); the
// re-initialisation starts only when every DMA ownership flag is clear, the
// runtime is stopped and no quarantine is set, frees and reallocates every
// ring and buffer (Ath10kDMABuffer/Ath10kCERing allocate releases first, so
// each step is idempotent) and hands any failure to the guarded restart.

void Ath10kPCIDevice::quiesceForSleep()
{
    if(!_wmiWorkLoop)return;
    waitForWakeReinit();
    if(_wmiWorkLoop->runAction(controlSleepQuiesce,this,reinterpret_cast<void *>(1))!=kIOReturnSuccess)return;
    // Bounded: the connection teardown (vdev down, keys, peer delete, vdev
    // stop, barrier) and the scan stop run on the normal runtime timer.
    for(UInt32 waited=0;waited<kSleepQuiesceMs;waited+=10){
        if(_wmiWorkLoop->runAction(controlSleepQuiesce,this,nullptr)==kIOReturnSuccess)return;
        IOSleep(10);
    }
}
IOReturn Ath10kPCIDevice::controlSleepQuiesce(OSObject *owner,void *begin,void *,void *,void *)
{
    auto *self=OSDynamicCast(Ath10kPCIDevice,owner);if(!self)return kIOReturnBadArgument;
    auto &s=self->_connection;
    if(!self->_wmiLive.active || self->powerIOBlocked()){self->_suspending=begin!=nullptr;return begin?kIOReturnNotReady:kIOReturnSuccess;}
    if(begin){
        self->_suspending=true;
        if(!self->_wmiScan.completed)self->_wmiScan.cancelRequested=true;
        if(s.stage!=Ath10k::LinkStage::Disabled){
            if(!Ath10k::requestDisconnect(s,Ath10k::uptimeNS()/1000000u))s.cancelRequested=true;
            s.diagnostics.cancelOrigin="system-sleep";
            s.portAuthorized=false;self->networkLink(false);self->publishConnection();
        }
        return kIOReturnSuccess;
    }
    const bool idle=s.stage==Ath10k::LinkStage::Disabled && s.disconnect==Ath10k::DisconnectStage::None &&
        (self->_wmiScan.completed || !self->_wmiScan.attempted) && !s.wmiOwned && !s.txOwned && !s.dataPending && !self->_wmi.credits.outstanding;
    return idle?kIOReturnSuccess:kIOReturnNotReady;
}
void Ath10kPCIDevice::waitForWakeReinit()
{
    // The probe thread clears reinitRunning as its last gated step.
    for(UInt32 waited=0;_power.reinitRunning && waited<kWakeReinitWaitMs;waited+=10)IOSleep(10);
}
// Under the gate (controlPowerState): hand the wake to the probe thread.
bool Ath10kPCIDevice::queueWakeReinit()
{
    auto &p=_power;
    if(!p.reinitEnabled || !_deferredProbeCall || _deferredProbeState!=2 || isInactive() || !_pci || p.reinitRunning)return false;
    p.reinitRunning=true;p.wakeResult=Ath10k::WakeResult::Pending;p.wakeDetail="queued";p.needsRestart=false;
    _wakeReinitRequested=true;_deferredProbeState=0;
    if(thread_call_enter(_deferredProbeCall)){ // already pending: cannot happen with state 2
        _wakeReinitRequested=false;p.reinitRunning=false;p.needsRestart=true;_deferredProbeState=2;return false;}
    return true;
}
// The DMA owners the release path trusts (stopDMAForRelease), without side
// effects. A verified suspend reset (quiesceTargetAfterBMI: bus mastering off,
// cold reset, CE deinit) clears them; anything left means the target may
// still hold a mapping, and nothing may be freed.
bool Ath10kPCIDevice::wakeDMAIdle() const
{
    if(_connection.wmiOwned || _connection.txOwned || _ceDiagTransferUncertain || _bmiTransferUncertain ||
       _rxSetup.txOwned || _rxSetup.ringOwned || _htcControlExchangeUncertain || _wmi.rxOwned || _wmiInit.txOwned || _wmiInit.hostMemoryOwned ||
       _wmiEcho.txOwned || _wmiVdevCreate.txOwned || _wmiScanChannels.txOwned || _wmiScan.txOwned || _boot.rxOwned || _dmaQuarantine)return false;
    for(const auto &radio:_wmiRadio)if(radio.txOwned)return false;
    return true;
}
// Runtime service state back to what a new driver instance has, keeping only
// configuration (regulatory policy, beacon timeout, PS request, multicast
// filter), the scan generation (the front end correlates scans by it) and the
// beacon hints. Idempotent.
void Ath10kPCIDevice::resetRuntimeServices()
{
    const UInt32 beaconTimeout=_connection.beaconTimeoutMs;const bool requestedPS=_connection.radio.requestedPS;
    const auto multicast=_connection.multicast;
    Ath10k::wipeConnectionSecrets(_connection);
    Ath10k::resetInPlace(_connection);
    _connection.beaconTimeoutMs=beaconTimeout;_connection.radio.requestedPS=requestedPS;_connection.multicast=multicast;
    _connection.regulatory=_regulatory;
    const UInt32 generation=_wmiScan.generation;
    Ath10k::resetInPlace(_wmiScan);_wmiScan.generation=generation;_wmiScan.id=0xa000|((0x36+generation)&0xfff);
    const UInt64 hints=_wmiBSS.onChannelMask;Ath10k::resetInPlace(_wmiBSS);_wmiBSS.onChannelMask=hints;
    _wmiEcho=Ath10k::WMIEchoState{};_filterEcho=Ath10k::WMIEchoState{};_connectionEcho=Ath10k::WMIEchoState{};
    Ath10k::resetInPlace(_rxSetup);
    _wmiVdevCreate=Ath10k::WMIVdevCreateState{};
    for(auto &radio:_wmiRadio)radio=Ath10k::WMIScanChannelsState{};
    _wmiScanChannels=Ath10k::WMIScanChannelsState{};
    _scanOptions=Ath10k::WMIScanOptions{};_scanProbeMask=0;_scanChannelsResend=false;
    Ath10k::resetInPlace(_wmiEvents);
    _wmiLive=Ath10k::WMIRuntimeState{};
    _runtimePower=Ath10k::RuntimePowerState{};_runtimePowerCleanup=false;_runtimeASPM=false;
    _runtimeIRQArmed=false;_runtimeIRQMaskFailed=false;_runtimeIRQDeferred=false;_runtimeIRQUnmaskStep=0;
    _nextRxDoneRetryMs=0;_lastMaintenanceRxPackets=_lastMaintenanceTxPackets=0;_dataTXStalled=false;
    _controlOwner=nullptr;
}
// Steps under the gate: 1 prepare (verify, quiesce leftovers, reset state,
// drop the runtime IRQ source so bootstrap can register its raw handler
// again), 2 finish (buffers for data TX, RX reserve, unblock, arm), 3 fail.
IOReturn Ath10kPCIDevice::controlWakeReinit(OSObject *owner,void *step,void *reason,void *,void *)
{
    auto *self=OSDynamicCast(Ath10kPCIDevice,owner);if(!self)return kIOReturnBadArgument;
    auto &p=self->_power;
    switch(reinterpret_cast<uintptr_t>(step)){
    case 1:{
        if(self->isInactive() || !self->_pci || self->_systemSuspended){p.wakeDetail="driver-inactive-or-suspended";return kIOReturnNotReady;}
        if(self->_wmiLive.active)self->endWMIRuntime("wake-reinit-stale-runtime");
        if(self->_runtimeActive || self->_runtimeInterruptsEnabled){p.wakeDetail="runtime-still-active";return kIOReturnNotReady;}
        if(!self->wakeDMAIdle()){p.wakeDetail=self->_dmaQuarantine?"dma-quarantine":"dma-ownership-pending";return kIOReturnNotReady;}
        // D0: IOPCIFamily restored the config space saved at sleep, which has
        // bus mastering off (the suspend quiesce disabled it). Memory decode and
        // bus mastering on again, as start() does, before any target access.
        self->_pci->setMemoryEnable(true);self->_pci->setBusMasterEnable(true);
        {const UInt16 command=self->_pci->configRead16(kIOPCIConfigCommand);
         if(command==0xffffu || (command&6u)!=6u){p.wakeDetail="pci-not-in-d0";return kIOReturnNotReady;}}
        self->networkLink(false);
        self->stopRuntimeWorkerIRQ();
        if(self->_runtimeIRQ){ // its provider registration blocks the raw bootstrap handler
            if(self->_runtimeIRQAdded && self->_wmiWorkLoop)self->_wmiWorkLoop->removeEventSource(self->_runtimeIRQ);
            self->_runtimeIRQAdded=false;self->_runtimeIRQ->release();self->_runtimeIRQ=nullptr;
        }
        self->_regs.setAccessHook(nullptr,nullptr,nullptr); // bootstrap MMIO is unhooked, as at first start
        if(self->_wmiTimer){self->_wmiTimer->cancelTimeout();self->_wmiTimer->disable();}
        self->resetRuntimeServices();
        // Lazily allocated packet buffers: dropped, allocated again on first use.
        self->_connectionCommand.release();self->_packetBuffer.release();self->_packetCommand.release();self->_packetFragments.release();
        self->_wmiLive.status="wake-reinit-running";self->publishWMIRuntimeProperties();
        return kIOReturnSuccess;}
    case 2:{
        if(!self->_wmiLive.active || self->_dmaQuarantine || !self->_wmiInit.ready){p.wakeDetail="runtime-not-prepared";return kIOReturnNotReady;}
        for(auto &b:self->_dataBuffers)
            if(!b.payload.allocate(2048,128,self->_dmaMapper,Ath10kDMABuffer::Role::Payload) ||
               !b.command.allocate(256,128,self->_dmaMapper) || !b.fragments.allocate(16,16,self->_dmaMapper)){p.wakeDetail="data-buffer-allocation";return kIOReturnNoMemory;}
        // The RX reserve timer lives on its own work loop (paused by endWMIRuntime).
        if(self->_rxReserveWorkLoop && self->_rxReserveTimer)
            self->_rxReserveWorkLoop->runAction([](OSObject *o,void *,void *,void *,void *)->IOReturn{
                auto *d=OSDynamicCast(Ath10kPCIDevice,o);
                if(d && d->_rxReserveTimer){d->_rxReserveTimer->enable();d->_rxReserveTimer->setTimeoutMS(10);}
                return kIOReturnSuccess;},self);
        OSCompareAndSwap(1,0,&self->_powerIOBlock);
        const IOReturn armed=controlWMIRuntime(self,nullptr,nullptr,nullptr,nullptr);
        if(armed!=kIOReturnSuccess){OSCompareAndSwap(0,1,&self->_powerIOBlock);p.wakeDetail="runtime-arm";return armed;}
        p.needsRestart=false;p.status="active";p.wakeResult=Ath10k::WakeResult::Reinit;p.wakeDetail="firmware-reloaded";++p.reinits;
        self->_suspending=false;
        return kIOReturnSuccess;}
    case 3:{
        // Failure: stop whatever came up and leave the guarded restart (the
        // front end's Restart terminates and rematches) or the quarantine.
        if(self->_wmiLive.active)self->endWMIRuntime(static_cast<const char *>(reason));
        ++p.reinitFailures;p.needsRestart=true;self->_suspending=false;
        if(self->_dmaQuarantine){p.wakeResult=Ath10k::WakeResult::Quarantined;p.status="wake-dma-quarantined";}
        else {OSCompareAndSwap(0,1,&self->_powerIOBlock);OSCompareAndSwap(1,2,&self->_powerIOBlock);p.wakeResult=Ath10k::WakeResult::FallbackRestart;p.status="wake-restart-required";}
        self->_wmiLive.status=p.status;
        if(self->_pci)self->_pci->setProperty("Ath10kRecoveryPhase","wake-reinit-failed");
        return kIOReturnSuccess;}
    case 4: // last gated step of either outcome
        p.reinitRunning=false;p.wakeMs=Ath10k::uptimeNS()/1000000u-p.lastWakeMs;
        self->publishPowerState();self->publishWMIRuntimeProperties();
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
// Probe thread (deferredProbeCall with _wakeReinitRequested).
void Ath10kPCIDevice::runWakeReinit()
{
    _wakeReinitRequested=false;
    IOLog("Ath10kMac: WAKE re-init begin (in place: rings, BMI, firmware, HTC, WMI)\n");
    const char *failure=nullptr;
    if(!_wmiWorkLoop || _wmiWorkLoop->runAction(controlWakeReinit,this,reinterpret_cast<void *>(1))!=kIOReturnSuccess)failure=_power.wakeDetail;
    // Outside the gate, exactly like the first start: nothing else touches the
    // hardware (timer disabled, IRQ source gone, control ops refused).
    if(!failure && !allocateCERings())failure="ce-ring-allocation";
    if(!failure && !probeTarget())failure=_dmaQuarantine?"probe-dma-quarantine":"target-probe-failed";
    if(!failure && _wmiWorkLoop->runAction(controlWakeReinit,this,reinterpret_cast<void *>(2))!=kIOReturnSuccess)failure=_power.wakeDetail;
    if(failure){
        _power.wakeDetail=failure;
        _wmiWorkLoop->runAction(controlWakeReinit,this,reinterpret_cast<void *>(3),const_cast<char *>(failure));
    }
    _wmiWorkLoop->runAction(controlWakeReinit,this,reinterpret_cast<void *>(4));
    publishDiagnosticProperties();
    if(_pci){_pci->setProperty("Ath10kRecoveryPhase",failure?"wake-reinit-failed":"ready");
        _pci->setProperty("Ath10kRecoveryDetail",failure?failure:"Firmware reloaded in place after wake");}
    IOLog("Ath10kMac: WAKE re-init %s detail=%s %llu ms\n",failure?"FAILED (guarded restart)":"PASS",_power.wakeDetail,
          static_cast<unsigned long long>(_power.wakeMs));
}
