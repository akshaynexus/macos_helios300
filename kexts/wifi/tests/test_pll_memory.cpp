#define ATH10K_OTP_FLOW_TEST
#include "test_board_flow.cpp"
static void clock_get_uptime(uint64_t *t){*t=0;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *n){*n=t;}
#include "pll_methods.inc"

static void use9377(Ath10kPCIDevice &d){
    d._deviceId=0x0042;d._subsystemVendorId=0x1028;d._subsystemId=0x1810;
    d._selectedHw=Ath10k::findCandidate(0x0042,1);d._bmiTargetVersion=0x05020001;
}
static void connectedPLL(Ath10kPCIDevice &d){
    std::map<UInt32,UInt32> soc;
    std::vector<std::pair<UInt32,UInt32>> ram;
    d.exchange=[&](const void *p,UInt32 bytes,void *out,UInt32 &n){
        const auto &r=*static_cast<const Ath10k::BMIReadSOCRequest *>(p);
        assert(r.id==6 && bytes==8 && n==4);
        *static_cast<UInt32 *>(out)=soc[r.address];return true;
    };
    d._regs.consume=[&](){
        const auto &desc=static_cast<const Ath10k::CEDescriptor32 *>(d._ceSrcRings[0].virtualAddress())[d._regs.read];
        const UInt32 command=*static_cast<const UInt32 *>(d._bmiRequestBuffer.virtualAddress());
        if(command==7){const auto &r=*static_cast<const Ath10k::BMIWriteSOCRequest *>(d._bmiRequestBuffer.virtualAddress());
            assert(desc.nbytes==12);soc[r.address]=r.value;
        }else{
            const auto &r=*static_cast<const Ath10k::BMIWriteMemoryRequest *>(d._bmiRequestBuffer.virtualAddress());
            assert(command==3 && desc.nbytes==16 && r.length==4);
            UInt32 value=0;memcpy(&value,r.payload,4);ram.emplace_back(r.address,value);
        }
    };
    assert(!d._bmiBoardUploadAttempted);
    assert(d.configureTargetPLL() && d._pll.complete && d._pll.step==13);
    assert((ram==std::vector<std::pair<UInt32,UInt32>>{{0x404028,1},{0x404020,1},{0x404024,176000000}}));
    assert(d._bmiWriteMemoryTransactionCount==3 && d._regs.posts==10);
    assert(!d._bmiBoardUploadAttempted && !d._bmiBoardUploadSucceeded && !d._bmiBoardBytesWritten);
}
int main(){
    // The exact 0.0.86 regression: a legitimate PLL word is rejected by the
    // actual board-only writer before any CE post, even with board preflight OK.
    for(bool qca9377:{false,true}){
        Ath10kPCIDevice d;if(qca9377)use9377(d);
        UInt32 one=1;assert(!d.bmiWriteMemory(0x404028,&one,4) && !d._regs.posts);
        d._bmiBoardUploadAttempted=true;
        assert(!d.bmiWriteMemory(0x404028,&one,4) && !d._regs.posts);
        d._bmiBoardUploadAttempted=false;
        connectedPLL(d);
    }
    for(unsigned fault=0;fault<14;++fault){
        Ath10kPCIDevice d;d._pll.attempted=true;d._pll.step=5;
        UInt32 address=0x404028,value=1;
        switch(fault){
        case 0:d._bmiGetTargetInfoSucceeded=false;break;
        case 1:d._bmiBoardPrereqSucceeded=false;break;
        case 2:d._boot.bmiClosed=true;break;
        case 3:d._dmaQuarantine=true;break;
        case 4:d._bmiTransferUncertain=true;break;
        case 5:d._subsystemId=0x0311;break;
        case 6:d._bmiTargetVersion=0x05020001;break;
        case 7:d._selectedHw=nullptr;break;
        case 8:d._selectedHw=Ath10k::findCandidate(0x003e,8);break;
        case 9:d._pll.attempted=false;break;
        case 10:d._pll.complete=true;break;
        case 11:d._pll.step=6;break;
        case 12:address=0x40402c;break;
        case 13:value=2;break;
        }
        assert(!d.bmiWritePLLWord(address,value) && !d._regs.posts && !d._bmiWriteMemoryTransactionCount);
    }
    for(UInt32 step:{5u,12u,13u}){
        Ath10kPCIDevice d;d._pll.attempted=true;d._pll.step=UInt8(step);
        const UInt32 address=step==5?0x404028:(step==12?0x404020:0x404024);
        const UInt32 value=step==13?176000000:1;
        assert(d.bmiWritePLLWord(address,value) && d._regs.posts==1 && d._bmiWriteMemoryTransactionCount==1);
    }
    for(bool timeout:{false,true}){
        Ath10kPCIDevice d;d._pll.attempted=true;d._pll.step=5;
        d._regs.timeout=timeout;d._regs.doorbellFailure=!timeout;
        assert(!d.bmiWritePLLWord(0x404028,1) && d._bmiTransferUncertain && !d._bmiWriteMemoryTransactionCount);
    }
    {Ath10kPCIDevice d;d._pll.attempted=true;d._pll.step=5;d._bmiRequestBuffer.syncOK=false;
        assert(!d.bmiWritePLLWord(0x404028,1) && !d._regs.posts && !d._bmiTransferUncertain);}
    {Ath10kPCIDevice d;d._pll.attempted=true;d._pll.step=5;d._ceSrcRings[0].syncOK=false;
        assert(!d.bmiWritePLLWord(0x404028,1) && !d._regs.posts && !d._bmiTransferUncertain);}
    puts("PLL RAM integration passed: original board guard reproduced, actual PLL/helper/BMI send/CE0 used for 6174 and 9377; three exact words before board upload, 14 refusal cases, timeout/doorbell uncertainty and DMA sync failures.");
}
