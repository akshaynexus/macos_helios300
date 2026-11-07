#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include <utility>
#include "Ath10kPLL.hpp"
#include "Ath10kHw.hpp"
#include "Ath10kBMI.hpp"
static void IOLog(const char *,...){}
static UInt64 clockUs=0;
static void IODelay(UInt32 n){clockUs+=n;}
static void clock_get_uptime(uint64_t *t){*t=clockUs*1000;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *n){*n=t;}
class Ath10kPCIDevice {
public:
    bool _bmiGetTargetInfoSucceeded=true,_bmiBoardPrereqSucceeded=true;
    bool _bmiTransferUncertain=false,_dmaQuarantine=false,profileMatches=true;
    UInt32 _bmiWriteMemoryTransactionCount=0;
    bool payloadMatchesTarget() const {return profileMatches;}
    struct {bool bmiClosed=false;} _boot;
    const Ath10k::HwCandidate *_selectedHw=Ath10k::findCandidate(Ath10k::kQCA9377Device,1);
    Ath10k::PLLState _pll{};std::map<UInt32,UInt32> words;
    std::vector<std::pair<UInt32,UInt32>> writes,ram;
    unsigned operations=0,failAt=0;bool stuck=false,shortRead=false;
    bool operation(){return ++operations!=failAt;}
    bool bmiExchange(const void *p,UInt32 bytes,void *out,UInt32 &n){
        assert(bytes==8 && n==4);const auto &r=*static_cast<const Ath10k::BMIReadSOCRequest *>(p);assert(r.id==6);
        if(!operation())return false;
        *static_cast<UInt32 *>(out)=r.address==0x1244 && stuck?0x20:words[r.address];if(shortRead)n=3;return true;
    }
    bool bmiSendOnly(const void *p,UInt32 bytes){
        const UInt32 command=*static_cast<const UInt32 *>(p);
        assert((command==7 && bytes==12) || (command==3 && bytes==16));
        if(!operation())return false;
        if(command==3){const auto &r=*static_cast<const Ath10k::BMIWriteMemoryRequest *>(p);
            assert(r.length==4);UInt32 value=0;__builtin_memcpy(&value,r.payload,4);ram.emplace_back(r.address,value);return true;}
        const auto &r=*static_cast<const Ath10k::BMIWriteSOCRequest *>(p);
        writes.emplace_back(r.address,r.value);words[r.address]=r.value;return true;
    }
    bool bmiWriteMemory(UInt32 a,const void *p,UInt32 n){
        assert(n==4);if(!operation())return false;ram.emplace_back(a,*static_cast<const UInt32 *>(p));return true;
    }
    bool bmiReadSOCRegister(UInt32,UInt32 &);bool bmiWriteSOCRegister(UInt32,UInt32);bool bmiWritePLLWord(UInt32,UInt32);bool configureTargetPLL();
};
#include "pll_methods.inc"
int main(){
    const UInt32 expectedFrac[]={0x2aaa8,0x2aaa8,0x15551,0x4ec4,0x34b49,0x15551,0x26665,0x4ec4};
    const UInt32 expectedSettle[]={2400,960,1200,1300,1870,1920,2000,2600};
    const UInt32 expectedDiv[]={14,36,29,27,18,18,18,27};
    for(UInt16 device:{Ath10k::kQCA6174Device,Ath10k::kQCA9377Device})for(UInt8 ref=0;ref<8;++ref){
        Ath10kPCIDevice d;d._selectedHw=Ath10k::findCandidate(device,device==Ath10k::kQCA6174Device?10:1);
        d.words[0xb2c]=UInt32(ref)<<8;d.words[0xaf4]=0x80000000;d.words[0x1018]=0x80000000;d.words[0x914]=0x80000000;
        d.words[0x1014]=0x80010000;d.words[0x820]=0x80000000;clockUs=0;
        assert(d.configureTargetPLL() && d._pll.complete && d._pll.step==13 && d._pll.reference==ref && d._pll.polls==2);
        const std::vector<std::pair<UInt32,UInt32>> expected={
            {0xaf4,0x80040000|expectedFrac[ref]},{0x1018,0x80000000|(expectedSettle[ref]&0x7ff)},
            {0x914,0x80000001},{0x1014,0x80050000|expectedDiv[ref]},
            {0x1014,0x80040000|expectedDiv[ref]},{0x820,0x80000001},{0x1014,0x80000000|expectedDiv[ref]}};
        assert(d.writes==expected);
        assert((d.ram==std::vector<std::pair<UInt32,UInt32>>({{0x404028,1},{0x404020,1},{0x404024,176000000}})));
    }
    for(unsigned op=1;op<=20;++op){Ath10kPCIDevice d;d.failAt=op;assert(!d.configureTargetPLL() && !d._pll.complete && d.operations==op);}
    {Ath10kPCIDevice d;d.stuck=true;clockUs=0;assert(!d.configureTargetPLL() && d._pll.step==7 && clockUs<=1000000 && d.ram.size()==1);}
    {Ath10kPCIDevice d;d.shortRead=true;assert(!d.configureTargetPLL() && d.writes.empty());}
    {Ath10kPCIDevice d;d._bmiBoardPrereqSucceeded=false;assert(!d.configureTargetPLL() && !d.operations && !d.bmiWriteSOCRegister(0x1014,1));}
    {Ath10kPCIDevice d;d._boot.bmiClosed=true;UInt32 v;assert(!d.bmiReadSOCRegister(0xb2c,v) && !d.bmiWriteSOCRegister(0x1014,1) && !d.operations);}
    {Ath10kPCIDevice d;d._selectedHw=Ath10k::findCandidate(Ath10k::kQCA6174Device,8);assert(d.configureTargetPLL() && !d.operations && !d._pll.attempted);}
    puts("PLL passed: actual BMI read/write ABI, 8 efuse crystals for 6174/9377, independent register/RAM vectors, 20 failure stops, bounded timeout, closed-BMI and profile guards.");
}
