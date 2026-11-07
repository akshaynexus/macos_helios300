#define ATH10K_CHANNEL_SWITCH_TEST
#include "test_connection.cpp"
#include <cstdio>
class ReviewDriver:public ConnectionDriver {
public:
 bool echoHeld=false;
 ReviewDriver(){connect();_connection.ap.timPresent=true;_connection.ap.dtimPeriod=1;_connection.radio.appliedDTIM=1;auto original=_regs.tx3Consume;
 _regs.tx3Consume=[this,original](){
  if(!_connection.radio.step){original();return;}
  ++commands;
  if(_connection.radio.step==8){if(!echoHeld)deliver(2,echoPacket(Ath10k::radioCookie(_connection)));}
  else deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
 };}
};
int main(){
 {ReviewDriver d;auto &s=d._connection;
  s.regulatory.country=Ath10k::regulatoryCountry("US");
  s.csa.phase=Ath10k::ChannelSwitchPhase::Barrier;s.csa.submitted=true;s.csa.blocked=true;
  s.csa.sinceMs=2000;s.csa.primary=6;s.csa.center=6;s.csa.width=20;s.csa.generation=1;
  d._connectionEcho={};d._connectionEcho.cookie=Ath10k::channelSwitchCookie(s);d._connectionEcho.submitted=true;
  const UInt32 original=d._connectionEcho.cookie;
  s.powerConstraintDb=25;
  for(unsigned t=2000;t<2250;t+=10)d.tick(t);
  printf("CSA: old cookie=%08x final cookie=%08x phase=%u radio-completed=%u active=%d\n",original,d._connectionEcho.cookie,static_cast<unsigned>(s.csa.phase),s.radio.completed,d._wmiLive.active);
  assert(d._wmiLive.active && s.radio.completed==0);
  assert(d._connectionEcho.cookie==original && s.csa.phase==Ath10k::ChannelSwitchPhase::Barrier);
  d.releaseWMIRuntime();
 }
 {ReviewDriver d;auto &s=d._connection;d.echoHeld=true;
  s.regulatory.country=Ath10k::regulatoryCountry("US");s.radio.requestedPS=true;
  for(unsigned t=2000;t<2250;t+=10)d.tick(t);
  assert(s.radio.step==8 && s.radio.targetPower==20);
  s.powerConstraintDb=25;
  d.deliver(2,echoPacket(Ath10k::radioCookie(s)));d.tick(2250);
  printf("Power: committed=%u current-limit=%u blocked=%d step=%u active=%d\n",s.txPowerHalfDbm,Ath10k::stationPowerLimit(s),s.radio.blocked,s.radio.step,d._wmiLive.active);
  assert(d._wmiLive.active && s.txPowerHalfDbm==20 && Ath10k::stationPowerLimit(s)==10 && s.radio.blocked && s.radio.powerBlocked);
  d.echoHeld=false;for(unsigned t=2260;t<2500;t+=10)d.tick(t);
  assert(s.txPowerHalfDbm==10 && !s.radio.blocked && !s.radio.powerBlocked);
  d.releaseWMIRuntime();
 }
 {
  Ath10k::ConnectionState s;s.stage=LinkStage::Connected;s.portAuthorized=true;s.htEnabled=true;s.vhtEnabled=true;
  s.operatingChannel=36;s.centerChannel=36;s.width=20;s.negotiatedWidth=40;
  s.regulatory.country=Ath10k::regulatoryCountry("US");
  std::vector<UInt8> ies={61,22,36,5};ies.resize(24,0);ies.insert(ies.end(),{192,5,1,42,0,255,255});
  assert(Ath10k::channelSwitchIEs(s,ies.data(),ies.size(),false,36));
  assert(s.csa.phase==Ath10k::ChannelSwitchPhase::Announced && s.csa.width==40);
  s.width=40;s.centerChannel=38;
  s.csa.phase=Ath10k::ChannelSwitchPhase::Confirm;s.csa.primary=36;s.csa.center=38;s.csa.width=40;s.csa.widthChanged=true;
  assert(Ath10k::channelSwitchIEs(s,ies.data(),ies.size(),false,36));
  printf("Width: AP=80 STA=%u target=%u confirmed=%d\n",s.width,s.csa.width,s.csa.confirmed);
  assert(s.csa.confirmed);
  s.csa.confirmed=false;ies[27]=58;assert(!Ath10k::channelSwitchIEs(s,ies.data(),ies.size(),false,36));assert(!s.csa.confirmed);
 }
 puts("Review regressions passed: CSA response ownership, power changes during ECHO and negotiated width confirmation.");
}
