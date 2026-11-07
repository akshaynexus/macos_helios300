#pragma once
#include "Ath10kTarget.hpp"
namespace Ath10k {
// Protected by the driver's short wake lock. No sleep/allocation in the ISR.
// SOC_WAKE and RTC remain accessible while the target sleeps; all other MMIO
// must acquire wake before access, as in Linux pci.c wake/sleep references.
struct RuntimePowerState {
    bool enabled=false,awake=true,asserted=true;
    UInt32 wakes=0,sleeps=0,failures=0,rx=0,tx=0,wmi=0;
    UInt64 activeMs=0;
    template<class Read,class Write,class Delay>
    bool wake(Read read,Write write,Delay delay){
        if(awake)return true;
        const auto &l=kQca6174RegisterLayout;const UInt32 rtc=rtcStateOffset(l);
        asserted=true;
        if(!write(socWakeOffset(l),kPcieSocWakeMask)){++failures;return false;}
        UInt32 us=0,poll=5;
        while(us<30000){UInt32 value=0;
            if(!read(rtc,value) || value==0xffffffffu){++failures;return false;}
            if(rtcStateValue(value)==l.rtcStateValueOn){awake=true;++wakes;return true;}
            delay(poll);us+=poll;if(poll<50)poll+=5;}
        ++failures;return false;
    }
    template<class Read,class Write>
    bool sleep(Read read,Write write){
        if(!awake)return true;
        const auto &l=kQca6174RegisterLayout;UInt32 flush=0;
        // An ambiguous write must force the next access through wake again.
        awake=false;
        if(!write(socWakeOffset(l),kPcieSocWakeReset) || !read(rtcStateOffset(l),flush) || flush==0xffffffffu){++failures;return false;}
        asserted=false;++sleeps;return true;
    }
    bool idle(UInt64 now,bool permitted,UInt32 rxPackets,UInt32 txPackets,UInt32 wmiPackets){
        if(!activeMs || now<activeMs || rx!=rxPackets || tx!=txPackets || wmi!=wmiPackets){
            activeMs=now;rx=rxPackets;tx=txPackets;wmi=wmiPackets;return false;}
        return enabled && permitted && now-activeMs>=60;
    }
};
}
