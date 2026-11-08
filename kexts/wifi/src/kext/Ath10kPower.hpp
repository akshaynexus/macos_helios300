#pragma once
#include <IOKit/IOTypes.h>
namespace Ath10k {
enum class PowerEvent : UInt32 { DeviceOff, DeviceOn, SystemSleep, SystemWake, SleepCancelled };
// Result of the last wake (Ath10kPowerWakeResult; Info state bit 8 while running).
enum class WakeResult : UInt32 { None, Pending, Reinit, FallbackRestart, Quarantined };
inline const char *wakeResultName(WakeResult r){
    switch(r){case WakeResult::None:return "none";case WakeResult::Pending:return "reinit-running";
    case WakeResult::Reinit:return "reinit-ok";case WakeResult::FallbackRestart:return "fallback-restart";
    case WakeResult::Quarantined:return "dma-quarantined";}
    return "?";
}
// Changed only under the shared hardware gate. The atomic I/O block is separate.
struct PowerState {
    bool deviceOn=true, rootSleeping=false, needsRestart=false;
    // In-place re-initialisation on wake (Ath10kPower.cpp, after Linux
    // ath10k_pci_resume -> hif_power_up -> core_start): enabled unless
    // ath10k_wake_reinit=0; running while the probe thread reloads firmware.
    bool reinitEnabled=true, reinitRunning=false;
    UInt32 sleeps=0,wakes=0,notifications=0,reinits=0,reinitFailures=0;
    UInt64 lastSleepMs=0,lastWakeMs=0,wakeMs=0;
    WakeResult wakeResult=WakeResult::None;
    const char *status="active", *wakeDetail="none";
};
}
