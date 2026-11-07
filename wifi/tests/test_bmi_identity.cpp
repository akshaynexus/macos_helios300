#define ATH10K_OTP_FLOW_TEST
#include "test_board_flow.cpp"
#include "bmi_identity_method.inc"
int main(){
    for(UInt32 version:{0x05020001u,0x05030000u,0u,0xffffffffu})for(UInt32 type:{8u,0u,7u})for(UInt32 length:{12u,0u,8u}){
        Ath10kPCIDevice d;d._deviceId=0x0042;d._subsystemVendorId=0x1028;d._subsystemId=0x1810;d._selectedHw=Ath10k::findCandidate(0x0042,1);
        d.exchange=[&](const void *request,UInt32 n,void *out,UInt32 &bytes){
            assert(n==4 && *static_cast<const UInt32 *>(request)==8 && bytes==12);
            *static_cast<Ath10k::BMIGetTargetInfoResponse *>(out)={length,version,type};return true;
        };
        bool ok=version==0x05020001u && type==8 && length==12;
        assert(d.runBMIGetTargetInfoTest()==ok && d._bmiGetTargetInfoSucceeded==ok && d._bmiSucceeded==ok && !d._regs.posts);
    }
    for(UInt32 bytes:{0u,4u,8u,11u}){
        Ath10kPCIDevice d;d.exchange=[&](const void *,UInt32,void *,UInt32 &n){n=bytes;return true;};
        assert(!d.runBMIGetTargetInfoTest() && !d._bmiGetTargetInfoSucceeded);
    }
    Ath10kPCIDevice timeout;timeout.exchange=[](const void *,UInt32,void *,UInt32 &){return false;};
    assert(!timeout.runBMIGetTargetInfoTest() && !timeout._bmiSucceeded && !timeout._regs.posts);
    puts("BMI identity passed: actual GET_TARGET_INFO rejects truncated/zero/wrong QCA9377 identity and transport failure; no board/OTP/firmware writes.");
}
