#include "Ath10kTarget.hpp"
#include <cassert>
#include <vector>
#include <utility>
static void IOLog(const char *,...){}
static void IODelay(unsigned){}
struct Registers {
    UInt32 rtc=Ath10k::kQca6174RegisterLayout.rtcStateValueOn;
    bool readOK=true,writeOK=true,failPoll=false,wakeOnWrite=true;
    unsigned reads=0;
    std::vector<std::pair<UInt32,UInt32>> writes;
    bool read32(UInt32 address,UInt32 &out){
        assert(address==Ath10k::rtcStateOffset(Ath10k::kQca6174RegisterLayout));
        ++reads;if(!readOK || (failPoll && reads>1))return false;out=rtc;return true;
    }
    bool write32(UInt32 address,UInt32 value){
        assert(address==Ath10k::socWakeOffset(Ath10k::kQca6174RegisterLayout));
        if(!writeOK)return false;
        writes.emplace_back(address,value);
        if(wakeOnWrite && value==Ath10k::kPcieSocWakeMask)rtc=Ath10k::kQca6174RegisterLayout.rtcStateValueOn;
        return true;
    }
};
class Ath10kPCIDevice {
public:
    Registers _regs;
    UInt32 _rtcStateBefore=0xffffffffu,_rtcStateAfter=0xffffffffu,_wakeDelayUs=0;
    bool _targetWasAlreadyAwake=false;
    bool wakeTarget(bool &);
    void releaseTargetWake(bool);
};
#include "wake_methods.inc"
int main(){
    {Ath10kPCIDevice d;bool held=false;assert(d.wakeTarget(held) && held);
        assert(d._targetWasAlreadyAwake && d._wakeDelayUs==0);
        assert(d._regs.writes.size()==1 && d._regs.writes[0].second==Ath10k::kPcieSocWakeMask);
        bool postReset=false;assert(d.wakeTarget(postReset) && postReset && d._regs.writes.size()==2);
        d.releaseTargetWake(held||postReset);
        assert(d._regs.writes.size()==3 && d._regs.writes.back().second==Ath10k::kPcieSocWakeReset);}
    {Ath10kPCIDevice d;d._regs.rtc=0;bool held=false;assert(d.wakeTarget(held) && held && !d._targetWasAlreadyAwake);}
    {Ath10kPCIDevice d;d._regs.readOK=false;bool held=true;assert(!d.wakeTarget(held) && !held && d._regs.writes.empty());}
    {Ath10kPCIDevice d;d._regs.writeOK=false;bool held=true;assert(!d.wakeTarget(held) && !held && d._regs.writes.empty());}
    {Ath10kPCIDevice d;d._regs.failPoll=true;bool held=false;assert(!d.wakeTarget(held) && held);
        d.releaseTargetWake(held);assert(d._regs.writes.back().second==Ath10k::kPcieSocWakeReset);}
    {Ath10kPCIDevice d;d._regs.rtc=0;d._regs.wakeOnWrite=false;bool held=false;
        assert(!d.wakeTarget(held) && held && d._wakeDelayUs>=Ath10k::kWakeTimeoutUs);
        d.releaseTargetWake(held);assert(d._regs.writes.size()==2);}
    {Ath10kPCIDevice d;d.releaseTargetWake(false);assert(d._regs.writes.empty());}
}
