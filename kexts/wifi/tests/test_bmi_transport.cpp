#define ATH10K_OTP_FLOW_TEST
#include "test_board_flow.cpp"
#include "bmi_transport_methods.inc"
#include "quiesce_method.inc"
class BMIModel : public Ath10kPCIDevice {
public:
    bool descriptorFirst=true,suppressReply=false,oversize=false;
    UInt32 command=8;
    explicit BMIModel(bool bounce=false){
        _bmiGetTargetInfoAttempted=true;_bmiGetTargetInfoSucceeded=false;
        _regs.allowRX=true;_regs.rxCtrl1=2048;
        _regs.written=_regs.read=15;_regs.rxWritten=_regs.rxRead=511;
        _ceDstRings[1].size=4096;
        if(bounce){_ceSrcRings[0].bounce=true;_ceDstRings[1].bounce=true;
            _bmiRequestBuffer.bounce=true;_bmiResponseBuffer.bounce=true;}
        _regs.consume=[this](){
            assert(_regs.rxPosts==1 && _regs.rxWritten==0); // RX doorbell precedes TX
            auto *tx=reinterpret_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[0].bounce?
                _ceSrcRings[0].deviceData.data():_ceSrcRings[0].data.data());
            assert(tx[15].nbytes==4 && tx[15].flags==0xfffc);
            auto *req=reinterpret_cast<const UInt32 *>(_bmiRequestBuffer.bounce?
                _bmiRequestBuffer.deviceData.data():_bmiRequestBuffer.data.data());
            assert(*req==command);
            if(suppressReply)return;
            auto *rx=reinterpret_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[1].bounce?
                _ceDstRings[1].deviceData.data():_ceDstRings[1].data.data());
            assert(rx[511].nbytes==0 && rx[511].address==_bmiResponseBuffer.dmaAddress());
            Ath10k::BMIGetTargetInfoResponse response{12,0x05020001,8};
            memcpy(_bmiResponseBuffer.bounce?_bmiResponseBuffer.deviceData.data():_bmiResponseBuffer.data.data(),&response,12);
            rx[511].nbytes=oversize?13:12;
            if(!descriptorFirst)_regs.rxRead=0;
        };
    }
    bool query(Ath10k::BMIGetTargetInfoResponse &r,UInt32 &bytes){return bmiTransportExchange(&command,4,&r,bytes);}
};
int main(){
    {BMIModel d;d._regs.diagnosticReadFails=true;Ath10k::BMIGetTargetInfoResponse r{};UInt32 bytes=12;
        assert(d.query(r,bytes) && bytes==12 && d._regs.diagnosticReads==2);
        assert(d._bmiEvidenceEntered && d._bmiEvidenceCondition && d._bmiEvidenceCommand==8);
        assert(d._bmiEvidenceCaptured[0] && !d._bmiEvidenceCaptured[1]);
        assert(d._bmiEvidenceDescriptor[0][1]==4 && d._bmiEvidenceDescriptor[0][2]==0xfffc);
        assert(d._bmiEvidenceCE[0][0][1]==0 && !d._bmiMiscRead[0][0] && d._regs.miscReads==2);}
    {BMIModel d;d._regs.diagnosticReadFails=true;d.suppressReply=true;Ath10k::BMIGetTargetInfoResponse r{};UInt32 bytes=12;
        assert(!d.query(r,bytes) && d._bmiTimedOut && d._bmiTransferUncertain && d._regs.diagnosticReads==4);
        assert(d._bmiEvidenceCaptured[0] && d._bmiEvidenceCaptured[1]);
        assert(d._bmiEvidenceDescriptor[1][4]==0);
        assert(d._regs.miscReads==4 && !d._bmiMiscRead[1][1]);
        UInt32 retained[2][2][18];memcpy(retained,d._bmiEvidenceCE,sizeof(retained));
        assert(d.quiesceTargetAfterBMI() && d.resets==1);
        assert(d._bmiEvidenceEntered && d._bmiEvidenceCaptured[1] && d._bmiEvidenceCommand==8);
        assert(memcmp(retained,d._bmiEvidenceCE,sizeof(retained))==0);}

    for(bool bounce:{false,true})for(bool descriptorFirst:{false,true}){
        BMIModel d(bounce);d.descriptorFirst=descriptorFirst;d.strictOK=false;
        Ath10k::BMIGetTargetInfoResponse r{};UInt32 bytes=12;
        assert(d.query(r,bytes) && bytes==12 && r.version==0x05020001 && r.type==8);
        assert(d._bmiPipeStrictValidationMismatch && d._bmiPipeStructuralValidationSucceeded);
        assert(!d._bmiTransferUncertain && d._regs.posts==1 && d._regs.rxPosts==1);
        assert(d._bmiWaitUs==0 && d._bmiCE0SrcRead==0);
        assert(d._bmiCE1DstRead==(descriptorFirst?511u:0u));
    }
    {BMIModel d;d.suppressReply=true;Ath10k::BMIGetTargetInfoResponse r{};UInt32 bytes=12;
        d._regs.miscStatus=0x180;
        assert(!d.query(r,bytes) && d._bmiTimedOut && d._bmiTransferUncertain);
        assert(d._bmiCompletedBytes==0 && d._bmiCE0SrcRead==0 && d._bmiCE1DstRead==511);
        assert(d._bmiWakeAtTimeout==1 && d._bmiRTCAtTimeout==3);
        assert(d._bmiMiscRead[1][0] && d._bmiMiscStatus[1][0]==0x180 && d._regs.miscStatus==0x180); // read only, never W1C
        assert(!d.query(r,bytes) && d._regs.posts==1 && d._regs.rxPosts==1);}
    {BMIModel d;d._deviceId=Ath10k::kQCA9377Device;d._regs.romSnapshot=true;d._regs.rxWritten=d._regs.rxRead=0;
        d.diagReadHook=[&](UInt32,void *,UInt32){assert(!d._regs.posts && !d._regs.rxPosts);d._ceDiagTransferUncertain=true;return false;};
        Ath10k::BMIGetTargetInfoResponse r{};UInt32 bytes=12;
        assert(!d.query(r,bytes) && d._ceDiagTransferUncertain && !d._regs.posts && !d._regs.rxPosts && !d._bmiTransferUncertain);}
    {BMIModel d;d.structureOK=false;Ath10k::BMIGetTargetInfoResponse r{};UInt32 bytes=12;
        assert(!d.query(r,bytes) && !d._regs.posts && !d._regs.rxPosts);}
    {BMIModel d;d._regs.rxDoorbellFails=true;Ath10k::BMIGetTargetInfoResponse r{};UInt32 bytes=12;
        assert(!d.query(r,bytes) && d._bmiTransferUncertain && d._regs.rxPosts==1 && !d._regs.posts);}
    {BMIModel d;d._regs.doorbellFailure=true;Ath10k::BMIGetTargetInfoResponse r{};UInt32 bytes=12;
        assert(!d.query(r,bytes) && d._bmiTransferUncertain && d._regs.rxPosts==1 && d._regs.posts==1);}
    {BMIModel d;d.oversize=true;Ath10k::BMIGetTargetInfoResponse r{};UInt32 bytes=12;
        assert(!d.query(r,bytes) && !d._bmiTransferUncertain && !d._bmiTimedOut);}
    {BMIModel d;d._bmiRequestBuffer.syncOK=false;Ath10k::BMIGetTargetInfoResponse r{};UInt32 bytes=12;
        assert(!d.query(r,bytes) && !d._bmiTransferUncertain && !d._regs.rxPosts);}
}
