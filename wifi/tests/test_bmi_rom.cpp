#define ATH10K_OTP_FLOW_TEST
#include "test_board_flow.cpp"
#include "bmi_transport_methods.inc"
#include "quiesce_method.inc"
struct ROMModel:Ath10kPCIDevice {
    unsigned calls=0,failCall=0; bool change=false,candidate=false,bufferChange=false,sourceChange=false;
    UInt32 buffer=0xa114f000,bytes=4;
    ROMModel(){
        _deviceId=Ath10k::kQCA9377Device;_bmiTimedOut=true;_bmiGetTargetInfoSucceeded=false;
        _bmiTransferUncertain=true;_bmiEvidenceCaptured[0]=_bmiEvidenceCaptured[1]=true;
        auto *pre=_bmiEvidenceCE[0][0];auto *now=_bmiEvidenceCE[1][0];
        pre[0]=now[0]=1;pre[4]=now[4]=0xa114ee48;pre[5]=now[5]=32;
        now[13]=now[14]=1;now[16]=1;
        _regs.allowRX=true;_regs.romSnapshot=true;
        _regs.written=_regs.read=1;_regs.rxWritten=2;_regs.rxRead=1;
        diagReadHook=[this](UInt32 a,void *p,UInt32 n){
            ++calls;assert(calls<=6);if(calls==failCall){_ceDiagTransferUncertain=true;return false;}
            auto *b=static_cast<UInt8 *>(p);memset(p,0,n);
            if(a==0x0044ee48){assert(n==8);UInt32 addr=buffer;
                if(change && calls==3)addr+=4;
                for(unsigned i=0;i<4;++i)b[i]=static_cast<UInt8>(addr>>(8*i));
                b[4]=static_cast<UInt8>(bytes);b[6]=0xfc;b[7]=0xff;
            }else if(a==0x0044ec68){assert(n==8);b[0]=0x48;b[1]=0xeb;b[2]=0x14;b[3]=0xa1;b[4]=(sourceChange && calls==6)?8:12;
            }else {assert(a==0x0044f000 && n==12);b[0]=candidate?12:8;
                if(candidate){b[4]=1;b[6]=2;b[7]=5;b[8]=8;}
                if(bufferChange && calls==4)b[4]^=2;}
            return true;
        };
    }
};
int main(){
    {ROMModel d;d._regs.rxRead=0;d.captureBMIROMEvidence(true);
        assert(d.calls==4 && d._bmiROMPreStable && d._bmiROMPreStatus==4 && d._bmiROMPreWords[0]==8);
        assert(d._bmiROMPreDescriptorCPU==0x0044ee48 && !d._regs.posts && !d._regs.rxPosts);}
    {ROMModel d;d._regs.rxRead=0;d.bufferChange=true;d.captureBMIROMEvidence(true);
        assert(d.calls==4 && !d._bmiROMPreStable && d._bmiROMPreStatus==5);}
    for(unsigned fail=1;fail<=4;++fail){ROMModel d;d._regs.rxRead=0;d.failCall=fail;d.captureBMIROMEvidence(true);
        assert(d.calls==fail && d._ceDiagTransferUncertain && !d._bmiROMPreStable && !d._regs.posts && !d._regs.rxPosts);}
    {ROMModel d;d.captureBMIROMEvidence();assert(d.calls==4 && d._bmiROMStatus==8 && d._bmiROMStable);
     assert(d._bmiROMWord==8 && d._bmiROMDescriptorCPU==0x0044ee48 && d._bmiROMBufferCPU==0x0044f000);
     assert(d._bmiTransferUncertain && d._regs.posts==0 && d._regs.rxPosts==0);
     assert(d.quiesceTargetAfterBMI());assert(d._bmiROMStatus==8 && d._bmiROMWord==8);}
    for(unsigned fail=1;fail<=4;++fail){ROMModel d;d.failCall=fail;d.captureBMIROMEvidence();
        assert(d.calls==fail && d._ceDiagTransferUncertain && !d._bmiROMStable);
        assert(d._bmiROMStatus==(fail==1?4u:fail==2?6u:7u));}
    {ROMModel d;d.change=true;d.captureBMIROMEvidence();assert(d.calls==4 && d._bmiROMStatus==7 && !d._bmiROMStable);}
    {ROMModel d;d.buffer=0xdeadbeef;d.captureBMIROMEvidence();assert(d.calls==1 && d._bmiROMStatus==5);}
    {ROMModel d;d.bytes=256;d.captureBMIROMEvidence();assert(d.calls==1 && d._bmiROMStatus==5);}
    {ROMModel d;d._bmiEvidenceCE[1][0][4]=0x12340000;d._bmiEvidenceCE[0][0][4]=0x12340000;
        d.captureBMIROMEvidence();assert(d.calls==0 && d._bmiROMStatus==3);}
    {ROMModel d;d._bmiEvidenceCE[1][0][16]=2;d.captureBMIROMEvidence();assert(d.calls==0 && d._bmiROMStatus==1);}
    {ROMModel d;d._deviceId=Ath10k::kQCA6174Device;d.captureBMIROMEvidence();assert(d.calls==0);}
    {ROMModel d;d._ceDiagTransferUncertain=true;d.captureBMIROMEvidence();assert(d.calls==0);}
    for(bool sourceChange:{false,true}){ROMModel d;d.candidate=true;d.sourceChange=sourceChange;d._regs.romCE1Snapshot=true;
        for(unsigned phase=0;phase<2;++phase){auto *ce=d._bmiEvidenceCE[phase][1];ce[0]=1;ce[2]=0xa114ec68;ce[3]=32;}
        d.captureBMIROMEvidence();assert(d.calls==6 && d._bmiROMResponseCandidate && d._bmiROMVersion==0x05020001 && d._bmiROMType==8);
        assert(d._bmiROMCE1Stable==!sourceChange && d._bmiROMCE1Bytes==12 && d._bmiROMCE1DescriptorCPU==0x0044ec68);
        assert(d._bmiTransferUncertain && !d._bmiGetTargetInfoSucceeded && d._regs.posts==0);
    }
    {ROMModel d;d.candidate=true;d.bufferChange=true;d.captureBMIROMEvidence();assert(d.calls==4 && !d._bmiROMResponseCandidate && !d._bmiROMStable);}
    puts("BMI ROM diagnostic passed: bounded response/CE1 reads, candidate identity, address checks, failures, stability, no BMI repost and retention after quiesce.");
}
