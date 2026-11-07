#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include <cstdint>
using UInt16=uint16_t;using UInt32=uint32_t;using SInt32=int32_t;using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOInterruptTypePCIMessaged=0x10000;
constexpr UInt16 kIOPCIConfigCommand=4,kMSIMessageControlOffset=2,kMSIControlEnable=1;
static void IOLog(const char *,...){}
class Ath10kPCIDevice;
struct PCI {
    std::vector<int> sources{1,0x10000};std::map<UInt16,UInt16> config{{4,0x407},{0x52,0}};
    int failRegistration=-1;bool enableMSI=true,ignoreINTx=false;
    std::vector<int> registered,unregistered,disabled;
    int getInterruptType(int i,int *type){if(i>=int(sources.size()))return 1;*type=sources[i];return 0;}
    template<class T>int registerInterrupt(int i,Ath10kPCIDevice *,T,void *){
        if(i==failRegistration)return 1;
        registered.push_back(i);if((sources[i]&0x10000) && enableMSI)config[0x52]|=1;return 0;
    }
    int unregisterInterrupt(int i){unregistered.push_back(i);return 0;}
    int disableInterrupt(int i){disabled.push_back(i);return 0;}
    UInt16 configRead16(UInt16 a){return config[a];}
    void configWrite16(UInt16 a,UInt16 v){if(!ignoreINTx || a!=4)config[a]=v;}
};
class Ath10kPCIDevice {
public:
    PCI pci;PCI *_pci=&pci;bool _irqPrepareAttempted=false,_irqPrepared=false,_irqRegistered=false,_irqEnabled=false,_irqMessaged=false;
    bool _hasMSI=true,_intxConfigChanged=false;UInt16 _msiCapOffset=0x50,_intxCommandBefore=0,_intxMSIBefore=0;
    bool _dmaQuarantine=false;
    SInt32 _irqIndex=-1;UInt32 _irqType=0,_irqObservedCount=0;
    UInt16 _msiControlBefore=0,_msiControlAfterRegister=0,_msiControlPostReset=0,_msiControlAfterWait=0;
    static void interruptHandler(){}void logMSIState(const char *){}
    bool prepareIRQParity();void teardownIRQParity();
};
#include "irq_prepare_methods.inc"
int main(){
    {Ath10kPCIDevice d;assert(d.prepareIRQParity() && d._irqMessaged && d._irqIndex==1 && d.pci.registered==std::vector<int>{1});d.teardownIRQParity();assert(!d._irqRegistered);}
    for(unsigned scenario=0;scenario<3;++scenario){
        Ath10kPCIDevice d;if(!scenario)d.pci.sources={1};if(scenario==1)d.pci.failRegistration=1;if(scenario==2)d.pci.enableMSI=false;
        assert(d.prepareIRQParity() && !d._irqMessaged && d._irqIndex==0 && d._intxConfigChanged);
        assert((d.pci.config[4]&0x400) && !(d.pci.config[0x52]&1));
        d._irqRegistered=false; // Handoff to IOFilter; config still needs restoration.
        d.pci.config[4]&=~UInt16(4); // Quarantine disables bus master independently.
        d.teardownIRQParity();assert(!d._intxConfigChanged && (d.pci.config[4]&0x400) && !(d.pci.config[4]&4));
    }
    {Ath10kPCIDevice d;d.pci.sources={};assert(!d.prepareIRQParity() && d._irqIndex<0);}
    {Ath10kPCIDevice d;d.pci.sources={1};d.pci.failRegistration=0;assert(!d.prepareIRQParity() && !d._irqRegistered && !d._intxConfigChanged && d.pci.config[4]==0x407);}
    {Ath10kPCIDevice d;d.pci.sources={1};d.pci.ignoreINTx=true;d.pci.config[4]=7;assert(!d.prepareIRQParity() && !d._irqRegistered && d.pci.registered.empty());}
    {Ath10kPCIDevice d;d.pci.sources={1};d.pci.config[4]=0xffff;assert(!d.prepareIRQParity() && !d._intxConfigChanged);}
    {Ath10kPCIDevice d;d.pci.sources={0};assert(d.prepareIRQParity() && !d._irqMessaged);d.teardownIRQParity();}
    {Ath10kPCIDevice d;d.pci.sources={1};d.pci.config[4]=7;d.pci.config[0x52]=1;assert(d.prepareIRQParity());d._dmaQuarantine=true;
     d.pci.config[4]&=~UInt16(4);d.teardownIRQParity();assert(d._intxConfigChanged && (d.pci.config[4]&0x400) && !(d.pci.config[4]&4) && !(d.pci.config[0x52]&1));}
    puts("IRQ fallback passed: actual MSI preference, absent/registration/enable failure to INTx, configuration readback, no source, rollback after filter handoff and bus-master quarantine preserved.");
}
