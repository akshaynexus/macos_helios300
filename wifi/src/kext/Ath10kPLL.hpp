#pragma once
#include <IOKit/IOTypes.h>

namespace Ath10k {
// Linux v6.12 hw.c: qca6174_clk and ath10k_hw_qca6174_enable_pll_clock.
// BMI addresses, not host BAR offsets. Only the hw3.2/hw1.1 profiles use this.
struct PLLClock { UInt32 hz,div,frac,settle,refdiv,outdiv; };
static constexpr PLLClock kPLLClocks[]={
    {48000000,0xe,0x2aaa8,2400,0,1},{19200000,0x24,0x2aaa8,960,0,1},
    {24000000,0x1d,0x15551,1200,0,1},{26000000,0x1b,0x4ec4,1300,0,1},
    {37400000,0x12,0x34b49,1870,0,1},{38400000,0x12,0x15551,1920,0,1},
    {40000000,0x12,0x26665,2000,0,1},{52000000,0x1b,0x4ec4,2600,0,1}
};
struct PLLState {
    bool attempted=false,complete=false;UInt8 reference=0,step=0;
    UInt32 referenceHz=0,polls=0;const char *status="not-attempted";
};
template<class Read,class Write,class Memory,class Delay,class Now>
bool configurePLL(PLLState &s,UInt32 frequency,Read read,Write write,Memory memory,Delay delay,Now now){
    s={};if(!frequency){s.status="profile-not-required";return true;}
    if(frequency!=176000000){s.status="unsupported-frequency";return false;}
    s.attempted=true;s.status="bmi-register-failed";UInt32 value=0;
    s.step=1;if(!read(0xb2c,value))return false;
    s.reference=UInt8((value>>8)&7);const auto &c=kPLLClocks[s.reference];s.referenceHz=c.hz;
    auto modify=[&](UInt32 address,UInt32 clear,UInt32 bits)->bool{
        return read(address,value) && write(address,(value&~clear)|bits);
    };
    s.step=2;if(!modify(0xaf4,0x1fffff,(c.frac&0x3ffff)|((c.outdiv<<18)&0x1c0000)))return false;
    s.step=3;if(!modify(0x1018,0x7ff,c.settle&0x7ff))return false;
    s.step=4;if(!modify(0x914,7,1))return false;
    s.step=5;s.status="bmi-memory-write-failed";if(!memory(0x404028,1))return false;
    s.step=6;s.status="bmi-register-failed";if(!modify(0x1014,0,((c.refdiv<<10)&0x3c00)|(c.div&0x3ff)|0x40000))return false;
    auto wait=[&]()->bool{
        const UInt64 start=now();
        for(UInt32 count=0;count<100000;++count){
            ++s.polls;if(!read(0x1244,value))return false;
            if(!(value&0x20))return true;
            if(now()-start>=1000000){s.status="pll-lock-timeout";return false;}
            delay(10);
        }
        s.status="pll-lock-timeout";return false;
    };
    s.step=7;if(!wait())return false;
    s.step=8;if(!modify(0x1014,0x10000,0))return false;
    s.step=9;if(!wait())return false;
    s.step=10;if(!modify(0x820,3,1))return false;
    s.step=11;if(!modify(0x1014,0x40000,0))return false;
    s.step=12;s.status="bmi-memory-write-failed";if(!memory(0x404020,1))return false;
    s.step=13;if(!memory(0x404024,frequency))return false;
    s.complete=true;s.status="configured";return true;
}
}
