#define ATH10K_WMI_VDEV_TEST
#include "test_wmi_echo.cpp"
class VdevDriver:public EchoDriver {
public:
    unsigned creates=0;
    bool returnCredit=true,creditOnCE2=false;
    VdevDriver() {
        _wmiVdevCreate={};
        auto echo=_regs.tx3Consume;
        _regs.tx3Consume=[this,echo](){
            auto *p=static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress());
            if(Ath10k::wmiReadLE32(p+8)==0x1d001){echo();return;}
            if (_wmiRadio[1].completed) _wmiRadio[1].txNext=_wmiVdevCreate.txNext;
            if (_wmiRadio[4].completed) _wmiRadio[4].txNext=_wmiVdevCreate.txNext;
            ++creates;
            assert(_wmiEcho.completed && _wmiEcho.creditReturned && _wmiVdevCreate.txOwned);
            assert(_wmi.credits.outstanding==1 && _wmi.credits.available==1);
            assert(Ath10k::wmiReadLE32(p+8)==0x5001 && p[0]==2 && p[1]==1 && p[5]==5);
            assert(Ath10k::htcReadLE16(p+2)==28 && Ath10k::htcReadLE16(p+12)==20 && Ath10k::htcReadLE16(p+14)==86);
            assert(Ath10k::wmiReadLE32(p+16)==0 && Ath10k::wmiReadLE32(p+20)==2 && Ath10k::wmiReadLE32(p+24)==0);
            assert(!memcmp(p+28,_wmiInit.mac,6) && !p[34] && !p[35]);
            auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[3].virtualAddress());
            assert(ring[_wmiVdevCreate.txSlot].nbytes==36 && ring[_wmiVdevCreate.txSlot].flags==8);
            if(returnCredit)deliver(creditOnCE2?2:1,{UInt8(creditOnCE2?2:0),2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
    }
};
#ifndef ATH10K_WMI_CHANNELS_TEST
int main() {
    for(bool ce2:{false,true}){
        VdevDriver d;d.creditOnCE2=ce2;
        d.tick(1000);assert(!d._wmiVdevCreate.attempted && d.echoes==1);
        d.tick(1010);assert(d._wmiVdevCreate.submitted && !d._wmiVdevCreate.completed && d.creates==1);
        d.tick(1020);assert(d._wmiVdevCreate.completed && d._wmiLive.active && d._wmi.credits.available==2);
        assert(!strcmp(d._wmiVdevCreate.status,"transport-complete-no-create-response"));
        for(unsigned i=0;i<200;++i)d.tick(1030+10*i);
        assert(d.creates==1 && d.echoes==1);d.releaseWMIRuntime();assert(!d._wmiVdevCreate.txOwned);
    }
    VdevDriver delayed;delayed.mode=EchoDriver::ReplyOnly;delayed.tick(1000);delayed.tick(1010);
    assert(!delayed._wmiVdevCreate.attempted);
    delayed.deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});delayed.tick(1020);
    assert(delayed._wmiVdevCreate.submitted && !delayed._wmiVdevCreate.completed);
    delayed.tick(1030);assert(delayed._wmiVdevCreate.completed);delayed.releaseWMIRuntime();
    for(bool txStalled:{false,true}){
        VdevDriver d;d.returnCredit=false;d.tick(1000);d._regs.tx3Timeout=txStalled;d.tick(1010);
        d.tick(6009);assert(d._wmiLive.active && !d._wmiVdevCreate.completed);
        d.tick(6010);assert(!d._wmiLive.active && d.resets==1);
        assert(!strcmp(d._wmiVdevCreate.status,txStalled?"vdev-create-tx-timeout":"vdev-create-credit-timeout"));
        d.releaseWMIRuntime();
    }
    VdevDriver failed;failed.tick(1000);failed._regs.tx3Fail=true;failed.resetOK=false;failed.tick(1010);
    assert(failed._wmiVdevCreate.txOwned && !failed._wmiVdevCreate.submitted && failed._dmaQuarantine);
    assert(failed._wmi.credits.outstanding==1 && failed.creates==0);
    assert(failed.serviceWMIVdevCreate(2000));assert(failed.creates==0);failed.releaseWMIRuntime();
    VdevDriver badMac;bzero(badMac._wmiInit.mac,6);badMac.tick(1000);badMac.tick(1010);
    assert(!badMac._wmiVdevCreate.submitted && badMac.creates==0 && badMac._wmi.credits.outstanding==0);badMac.releaseWMIRuntime();
    VdevDriver wrap;wrap._wmiVdevCreate.completed=true;wrap.tick(1000);wrap.tick(1010);
    wrap._wmiVdevCreate={};wrap._regs.tx3Written=wrap._regs.tx3Read=wrap._rxSetup.wmiNext=31;
    assert(!wrap.serviceWMIVdevCreate(1020));assert(wrap._wmiVdevCreate.txNext==0);
    wrap.tick(1030);assert(wrap._wmiVdevCreate.completed);wrap.releaseWMIRuntime();
    VdevDriver stop;stop.returnCredit=false;stop.tick(1000);stop.tick(1010);stop.setPowerState(0,&stop);
    assert(!stop._wmiVdevCreate.completed && !stop._wmiVdevCreate.txOwned && stop.resets==1);stop.releaseWMIRuntime();
    UInt8 frame[36],mac[6]={2,1,2,3,4,5};
    assert(Ath10k::buildWMIVdevCreate(frame,36,2,mac));
    assert(!Ath10k::buildWMIVdevCreate(frame,35,2,mac));
    assert(!Ath10k::buildWMIVdevCreate(frame,36,0,mac));
    mac[0]=1;assert(!Ath10k::buildWMIVdevCreate(frame,36,2,mac));
    bzero(mac,6);assert(!Ath10k::buildWMIVdevCreate(frame,36,2,mac));
}

#endif
