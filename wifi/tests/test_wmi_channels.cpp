#define ATH10K_WMI_CHANNELS_TEST
#include "test_wmi_vdev.cpp"
static constexpr UInt32 expectedScanFrequencies[]={2412,2417,2422,2427,2432,2437,2442,2447,2452,2457,2462,2467,2472,5180,5200,5220,5240,5260,5280,5300,5320,5500,5520,5540,5560,5580,5600,5620,5640,5660,5680,5700,5720,5745,5765,5785,5805,5825};
class ChannelsDriver:public VdevDriver {
public:
    unsigned lists=0;
    bool channelCredit=true,channelCE2=false;
    ChannelsDriver() {
        _wmiScanChannels={};
        auto previous=_regs.tx3Consume;
        _regs.tx3Consume=[this,previous](){
            auto *p=static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress());
            if(Ath10k::wmiReadLE32(p+8)!=0x3003){previous();return;}
            ++lists;
            assert(_wmiVdevCreate.completed && _wmiVdevCreate.txConsumed && _wmiScanChannels.txOwned);
            assert(_wmi.credits.outstanding==1 && _wmi.credits.available==1);
            // Independent wire expectation, including nested TLVs and packed half-dBm power fields.
            assert(p[0]==2 && p[1]==1 && p[5]==8 && Ath10k::htcReadLE16(p+2)==1080);
            assert(Ath10k::htcReadLE16(p+12)==4 && Ath10k::htcReadLE16(p+14)==79);
            assert(Ath10k::wmiReadLE32(p+16)==38 && Ath10k::htcReadLE16(p+20)==1064 && p[22]==18);
            for(unsigned i=0;i<38;++i){
                const auto *ch=p+24+28*i;
                assert(Ath10k::htcReadLE16(ch)==24 && Ath10k::htcReadLE16(ch+2)==80);
                assert(Ath10k::wmiReadLE32(ch+4)==expectedScanFrequencies[i] && Ath10k::wmiReadLE32(ch+8)==expectedScanFrequencies[i]);
                assert(!Ath10k::wmiReadLE32(ch+12) && Ath10k::wmiReadLE32(ch+16)==(i<13?0x81u:((i>=17 && i<=32)?0x480u:0x80u)));
                assert(Ath10k::wmiReadLE32(ch+20)==0x00141400 && Ath10k::wmiReadLE32(ch+24)==0x00001400);
                assert(ch[20]==0 && ch[21]==20 && ch[22]==20 && ch[23]==0);
                assert(ch[24]==0 && ch[25]==20 && ch[26]==0 && ch[27]==0);
            }
            auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[3].virtualAddress());
            assert(ring[_wmiScanChannels.txSlot].nbytes==1088 && ring[_wmiScanChannels.txSlot].flags==8);
            if(channelCredit)deliver(channelCE2?2:1,{UInt8(channelCE2?2:0),2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
        };
    }
};
#ifndef ATH10K_WMI_SCAN_TEST
int main() {
    for(bool ce2:{false,true}){
        ChannelsDriver d;d.channelCE2=ce2;
        d.tick(1000);d.tick(1010);assert(!d._wmiScanChannels.attempted);
        d.tick(1020);assert(d._wmiScanChannels.submitted && !d._wmiScanChannels.completed && d.lists==1);
        d.tick(1030);assert(d._wmiScanChannels.completed && d._wmiLive.active && d._wmi.credits.available==2);
        assert(!strcmp(d._wmiScanChannels.status,"transport-complete-scan-not-started"));
        for(unsigned i=0;i<380;++i)d.tick(1040+10*i);
        assert(d.lists==1 && d.creates==1 && d.echoes==1);
        d.releaseWMIRuntime();assert(!d._wmiScanChannels.txOwned);
    }
    ChannelsDriver delayed;delayed.returnCredit=false;
    delayed.tick(1000);delayed.tick(1010);delayed.tick(1020);assert(!delayed._wmiScanChannels.attempted);
    delayed.deliver(1,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});delayed.tick(1030);
    assert(delayed._wmiScanChannels.submitted);delayed.tick(1040);assert(delayed._wmiScanChannels.completed);delayed.releaseWMIRuntime();
    for(bool stalled:{false,true}){
        ChannelsDriver d;d.channelCredit=false;d.tick(1000);d.tick(1010);d._regs.tx3Timeout=stalled;d.tick(1020);
        d.tick(6019);assert(d._wmiLive.active && !d._wmiScanChannels.completed);
        d.tick(6020);assert(!d._wmiLive.active && d.resets==1);
        assert(!strcmp(d._wmiScanChannels.status,stalled?"scan-channels-tx-timeout":"scan-channels-credit-timeout"));
        d.releaseWMIRuntime();
    }
    ChannelsDriver failed;failed.tick(1000);failed.tick(1010);failed._regs.tx3Fail=true;failed.resetOK=false;failed.tick(1020);
    assert(failed._wmiScanChannels.txOwned && !failed._wmiScanChannels.submitted && failed._dmaQuarantine);
    assert(failed._wmi.credits.outstanding==1 && failed.lists==0);
    assert(failed.serviceWMIScanChannels(2000));assert(failed.lists==0);failed.releaseWMIRuntime();
    ChannelsDriver stop;stop.channelCredit=false;stop.tick(1000);stop.tick(1010);stop.tick(1020);stop.setPowerState(0,&stop);
    assert(!stop._wmiScanChannels.completed && !stop._wmiScanChannels.txOwned && stop.resets==1);stop.releaseWMIRuntime();
    ChannelsDriver wrap;wrap._wmiScanChannels.completed=true;wrap.tick(1000);wrap.tick(1010);wrap.tick(1020);
    wrap._wmiScanChannels={};wrap._regs.tx3Written=wrap._regs.tx3Read=wrap._wmiRadio[4].txNext=31;
    assert(!wrap.serviceWMIScanChannels(1030));assert(wrap._wmiScanChannels.txNext==0);
    wrap.tick(1040);assert(wrap._wmiScanChannels.completed);wrap.releaseWMIRuntime();
    UInt8 frame[1089];memset(frame,0xaa,sizeof(frame));
    assert(Ath10k::buildWMIScanChannels(frame,1088,2));assert(frame[1088]==0xaa);
    assert(!Ath10k::buildWMIScanChannels(frame,1087,2));
    assert(!Ath10k::buildWMIScanChannels(frame,1088,0));
    assert(!Ath10k::buildWMIScanChannels(frame,1088,255));
    assert(!Ath10k::buildWMIScanChannels(nullptr,1088,2));
}

#endif
