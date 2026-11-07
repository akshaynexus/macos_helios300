#pragma once
#include <IOKit/IOTypes.h>
namespace Ath10k {
enum class PowerEvent : UInt32 { DeviceOff, DeviceOn, SystemSleep, SystemWake, SleepCancelled };
// Changed only under the shared hardware gate. The atomic I/O block is separate.
struct PowerState {
    bool deviceOn=true, rootSleeping=false, needsRestart=false;
    UInt32 sleeps=0,wakes=0,notifications=0;
    const char *status="active";
};
}
