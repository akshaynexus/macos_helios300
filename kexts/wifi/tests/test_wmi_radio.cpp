#define ATH10K_WMI_BSS_TEST
#include "test_wmi_scan.cpp"
class RadioDriver: public ScanDriver {
public:
    unsigned commands=0;
    unsigned hold=0;
    bool onCE2=false;
    RadioDriver() {
        for(auto &radio:_wmiRadio)radio={};
        for(unsigned i=2;i<5;++i){_wmiRadio[i].completed=true;_wmiRadio[i].txConsumed=true;}
        auto previous=_regs.tx3Consume;
        _regs.tx3Consume=[this,previous](){
            auto *p=static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress());
            if(Ath10k::wmiReadLE32(p+8)!=0x4003){previous();return;}
            const unsigned param=++commands;
            assert(param<=2 && _wmiVdevCreate.completed && !_wmiScanChannels.attempted);
            assert(param==1 || (_wmiRadio[0].completed && _wmiRadio[0].creditReturned));
            assert(p[0]==2 && p[1]==1 && p[5]==param+5);
            assert(Ath10k::htcReadLE16(p+2)==20 && Ath10k::htcReadLE16(p+12)==12);
            assert(Ath10k::htcReadLE16(p+14)==82 && Ath10k::wmiReadLE32(p+16)==0);
            assert(Ath10k::wmiReadLE32(p+20)==param && Ath10k::wmiReadLE32(p+24)==3);
            assert(_wmi.credits.outstanding==1 && _wmiRadio[param-1].txOwned);
            _wmiRadio[4].txNext=_wmiRadio[param-1].txNext;
            if(hold!=param) deliver(onCE2?2:1,{UInt8(onCE2?2:0),2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
    }
};
#ifndef ATH10K_RX_SETUP_TEST
int main(){
    for(bool ce2:{false,true}){
        RadioDriver d;d.onCE2=ce2;
        for(unsigned t=1000;t<=1060;t+=10)d.tick(t);
        assert(d.commands==2 && d.lists==1 && d.scans==1 && d._wmiLive.active);
        assert(d._wmiRadio[0].completed && d._wmiRadio[1].completed);
        assert(d._wmiLive.rawCount==d._wmiLive.rawCE1+d._wmiLive.rawCE2);
        d.deliver(2,scanPacket(1));d.tick(1070);
        d.deliver(2,mgmtPacket());d.tick(1080);
        assert(d._wmiLive.rawMgmt==1 && d._wmiScan.mgmtCount==1);
        d.deliver(2,scanPacket(2));d.tick(1090);
        assert(d._wmiScan.completed);
        assert(d.commands==2);d.releaseWMIRuntime();
        assert(!d._wmiRadio[0].txOwned && !d._wmiRadio[1].txOwned);
    }
    for(unsigned hold:{1u,2u}){
        RadioDriver d;d.hold=hold;
        for(unsigned t=1000;t<=1040;t+=10)d.tick(t);
        assert(d.commands==hold && !d.lists && !d.scans);
        d.tick(6040);assert(!d._wmiLive.active && d.resets==1);
        assert(!strcmp(d._wmiRadio[hold-1].status,"radio-credit-timeout"));
        d.releaseWMIRuntime();
    }
    RadioDriver failed;failed.tick(1000);failed.tick(1010);
    failed._regs.tx3Fail=true;failed.resetOK=false;failed.tick(1020);
    assert(failed._dmaQuarantine && failed._wmiRadio[0].txOwned && !failed.lists);
    failed.releaseWMIRuntime();
    RadioDriver stop;stop.hold=1;stop.tick(1000);stop.tick(1010);stop.tick(1020);
    stop.setPowerState(0,&stop);assert(stop.resets==1 && !stop._wmiRadio[0].txOwned);
    stop.releaseWMIRuntime();
    UInt8 f[29];memset(f,0xaa,sizeof(f));
    for(unsigned mask:{1u,3u,7u})for(unsigned param:{1u,2u}){
        assert(Ath10k::buildWMIRadio(f,28,2,param,mask));assert(f[28]==0xaa);
        assert(Ath10k::wmiReadLE32(f+24)==mask);
    }
    for(unsigned mask:{0u,2u,4u,8u,0xffffffffu})assert(!Ath10k::buildWMIRadio(f,28,2,1,mask));
    assert(!Ath10k::buildWMIRadio(f,27,2,1,3));
    assert(!Ath10k::buildWMIRadio(f,28,0,1,3));
    assert(!Ath10k::buildWMIRadio(f,28,255,1,3));
    assert(Ath10k::buildWMIRadio(f,28,2,3,3));
    assert(!Ath10k::buildWMIRadio(nullptr,28,2,1,3));
}

#endif
