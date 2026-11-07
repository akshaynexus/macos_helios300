#include <cassert>
#include <cstdio>
#include <map>
#include "Ath10kTarget.hpp"
static void IOLog(const char *,...){}
static unsigned elapsed=0;static void IOSleep(UInt32 ms){elapsed+=ms;}
struct Registers {
    std::map<UInt32,UInt32> words;unsigned enables=0,clears=0;bool fail=false,ready=true,event=false,removed=false;
    bool read32(UInt32 a,UInt32 &v){
        if(a==Ath10k::kQca6174RegisterLayout.fwIndicatorAddress){v=removed?0xffffffff:(event?1:(ready && elapsed>=30?2:0));return true;}
        v=words[a];return !fail;
    }
    bool write32(UInt32 a,UInt32 v){
        if(fail)return false;
        words[a]=v;
        if(a==Ath10k::socCoreOffset(Ath10k::kQca6174RegisterLayout,Ath10k::kPCIeIntrEnableAddress) && v)++enables;
        if(a==Ath10k::socCoreOffset(Ath10k::kQca6174RegisterLayout,Ath10k::kPCIeIntrClearAddress))++clears;
        return true;
    }
};
class Ath10kPCIDevice {
public:
    Registers _regs;bool _targetInitWaitAttempted=false,_targetInitWaitSucceeded=false,_irqMessaged=false;
    bool _dmaQuarantine=false;
    bool _fwIndicatorEventPending=false,_fwIndicatorNonZeroSeen=false;
    UInt32 _targetInitWaitMs=0,_fwIndicatorSampleCount=0,_fwIndicatorChangeCount=0,_fwIndicatorBefore=0,_fwIndicatorAfter=0;
    bool waitForTargetInit();
};
#include "target_init_method.inc"
int main(){
    {Ath10kPCIDevice d;elapsed=0;assert(d.waitForTargetInit() && d._regs.enables==3 && d._regs.clears==1 && elapsed==30);}
    {Ath10kPCIDevice d;elapsed=0;d._irqMessaged=true;assert(d.waitForTargetInit() && !d._regs.enables && !d._regs.clears);}
    {Ath10kPCIDevice d;elapsed=0;d._regs.fail=true;assert(!d.waitForTargetInit() && !d._targetInitWaitSucceeded && !elapsed && d._dmaQuarantine);}
    {Ath10kPCIDevice d;elapsed=0;d._regs.event=true;assert(!d.waitForTargetInit() && d._fwIndicatorEventPending && !d._regs.enables);}
    {Ath10kPCIDevice d;elapsed=0;d._regs.removed=true;assert(!d.waitForTargetInit() && elapsed==3000 && !d._regs.enables);}
    puts("INTx target-init passed: actual wait repeats target mask until READY, masks/clears/flushed handoff, MSI unchanged, failed MMIO, crash and removed-device stops.");
}
