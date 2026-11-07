#define ATH10K_AP_MAINTENANCE_TEST
#include "test_connection.cpp"
using P=Ath10k::ChannelSwitchPhase;
int main(){
    auto owned=std::make_unique<ConnectionDriver>();auto &d=*owned;d.connect();auto &s=d._connection;
    s.operatingChannel=36;s.centerChannel=42;s.width=80;s.htEnabled=s.vhtEnabled=true;s.nss=2;
    s.wmmConfigured=true;s.localHT=s.peerHT=0x6e;s.localVHT=s.peerVHT=1u<<5;s.vhtRxMap=s.vhtTxMap=0xfffa;
    s.ap.ht[0]=14;s.htRateCount=16;for(UInt8 i=0;i<16;++i)s.htRates[i]=i;
    s.regulatory.country=Ath10k::regulatoryCountry("US");s.regulatory.indoor=true;s.ap.vhtPresent=s.ap.vhtOperation=true;
    Ath10k::htcWriteLE16(s.ap.vht+4,0xfffa);Ath10k::htcWriteLE16(s.ap.vht+8,0xfffa);
    Ath10k::rememberNegotiatedRates(s);s.peerOperatingNSS=1;Ath10k::applyPeerRates(s,1,80);
    s.pairPN[0]=99;s.installedPair[0]=42;
    s.maintenance.applied=Ath10k::desiredAPConfig(s);s.maintenance.valid=true;s.maintenance.blocked=false;
    bool reassoc2=false,holdEcho=false;UInt16 firmwareMap=0xffff;
    d._regs.tx3Consume=[&](){
        ++d.commands;const auto *f=static_cast<const UInt8 *>(d._connectionCommand.virtualAddress());
        if(Ath10k::wmiReadLE32(f+8)==0x6005){
            assert(!Ath10k::wmiReadLE32(f+28)); // new_assoc=0 retains keys
            const UInt32 legacy=(s.ap.rateCount+3u)&~3u,ht=(Ath10k::wmiReadLE32(f+88)+3u)&~3u;
            firmwareMap=Ath10k::htcReadLE16(f+108+legacy+ht);
            if(s.maintenance.step==11 && s.maintenance.target.nss==2){assert(firmwareMap==0xfff5);reassoc2=true;}
        }
        if(s.csa.phase==P::Restart){std::vector<UInt8> p(32);p[0]=2;p[2]=24;p[8]=1;p[9]=0x50;p[12]=16;p[14]=40;
            Ath10k::htcWriteLE32(p.data()+24,1);addPkgCredit(p);d.deliver(2,p);return;}
        if(s.csa.phase==P::Barrier){d.deliver(2,echoPacket(Ath10k::channelSwitchCookie(s)));return;}
        if(s.maintenance.step==8 && !holdEcho){d.deliver(2,echoPacket(Ath10k::apMaintenanceCookie(s)));return;}
        d.deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
    };
    const UInt8 down[]={37,3,1,36,0,62,1,0};assert(Ath10k::channelSwitchIEs(s,down,sizeof(down),true));
    for(unsigned t=2000;t<2350;t+=10)d.tick(t);
    assert(s.width==20 && s.csa.phase==P::Confirm && s.vhtTxMap==0xfffd && s.negotiatedVHTTx==0xfffa);
    UInt8 confirm[24]={61,22,36,0};assert(Ath10k::channelSwitchIEs(s,confirm,sizeof(confirm),false,36));
    d.tick(2360);assert(s.csa.phase==P::Idle);
    // Finish the width maintenance, then raise peer NSS while delaying ECHO.
    for(unsigned t=2370;t<2640;t+=10)d.tick(t);
    s.peerOperatingNSS=2;s.maintenance.blocked=true;holdEcho=true;
    for(unsigned t=2650;t<2920;t+=10)d.tick(t);
    assert(reassoc2 && s.maintenance.step==8 && s.nss==1 && s.maintenance.blocked && s.vhtTxMap==0xfffd);
    holdEcho=false;d.deliver(2,echoPacket(Ath10k::apMaintenanceCookie(s),false));d.tick(2930);
    assert(s.nss==2 && s.vhtTxMap==0xfff5 && s.htRateCount==16 && !s.maintenance.blocked);
    assert(s.pairPN[0]==99 && s.installedPair[0]==42 && firmwareMap==s.vhtTxMap);
    // Wrapper Country is pending until restart acknowledgement, even with OM20.
    const UInt8 wrapped[]={37,3,1,44,0,196,13,7,6,'U','S',' ',36,4,5,194,3,1,42,0,199,1,0};
    s.peerOperatingWidth=0;assert(Ath10k::channelSwitchIEs(s,wrapped,sizeof(wrapped),false,36));
    assert(!s.ap.countryPresent && s.csa.countryPresent && s.csa.width==80 && s.peerOperatingWidth==20);
    assert(!d.serviceChannelSwitch(2940));assert(!s.ap.countryPresent);
    for(unsigned t=2950;t<3330;t+=10)d.tick(t);
    assert(s.width==80 && s.operatingChannel==44 && s.csa.phase==P::Confirm && s.ap.countryPresent && s.ap.country[5]==5 && s.txPowerHalfDbm==10);
    UInt8 wideConfirm[31]={61,22,44,5};wideConfirm[24]=192;wideConfirm[25]=5;wideConfirm[26]=1;wideConfirm[27]=42;
    assert(Ath10k::channelSwitchIEs(s,wideConfirm,sizeof(wideConfirm),false,44));assert(s.csa.confirmed);
    d.tick(3340);assert(s.csa.phase==P::Idle && s.ap.countryPresent && Ath10k::peerBandwidth(s)==20 && s.nss==1);
    d.releaseWMIRuntime();puts("S5 runtime passed: 2SS restored in host/firmware rate sets only after ECHO; CSA80/OM20 and Country commit preserve keys/PN.");
}
