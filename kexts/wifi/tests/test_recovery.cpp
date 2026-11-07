#include <cassert>
#include <cstring>
#include "Ath10kWMI.hpp"
#include "Ath10kPower.hpp"
#include "../src/shared/Ath10kControl.hpp"
using IOReturn=int;
constexpr unsigned kIOServiceRequired=1,kIOServiceSynchronous=2;
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=3;
struct IOUserClient{};
struct Gate {
 bool inside=false;
 template<class F,class D,class C,class R,class O> int runAction(F f,D *d,C *c,R *r,O *o){inside=true;int result=f(d,c,r,o);inside=false;return result;}
};
class Ath10kPCIDevice {
public:
 Ath10k::PowerState _power{};
 bool _systemSuspended=false,_reprobeRequested=false,_dmaQuarantine=false,inactive=false;
 void *_pci=this;bool terminateOK=true,resetQuarantines=false;unsigned resets=0,terminations=0,options=0,refs=1;
 Gate gate;Gate *_wmiWorkLoop=&gate;
 struct {bool active=true;} _wmiLive;
 Ath10k::WMIScanState _wmiScan{};Ath10k::ConnectionState _connection{};
 Ath10k::RegulatoryPolicy _regulatory{};
 unsigned powerBlock=0;bool powerIOBlocked(){return powerBlock!=0;}bool powerRestartAllowed(){return powerBlock==2;}
 bool requestDeviceRestart();void fillControlInfo(AthControl::Response &);
 IOReturn controlRequest(IOUserClient *,const AthControl::Request *,AthControl::Response *);
 bool isInactive(){return inactive;}void retain(){++refs;}void release(){--refs;}
 static int controlAction(Ath10kPCIDevice *d,IOUserClient *,AthControl::Request *r,AthControl::Response *o){
  assert(d->gate.inside);if(r->operation==AthControl::Restart)return d->requestDeviceRestart()?0:kIOReturnNotReady;
  d->fillControlInfo(*o);return 0;
 }
 void endWMIRuntime(const char *){++resets;_wmiLive.active=false;if(resetQuarantines)_dmaQuarantine=true;}
 bool terminate(unsigned flags){assert(!gate.inside && refs==2);++terminations;options=flags;return terminateOK;}
};
#include "recovery_methods.inc"
int main(){
 AthControl::Request request{};request.operation=AthControl::Restart;AthControl::Response response{};IOUserClient client;
 auto restart=[&](Ath10kPCIDevice &d){return d.controlRequest(&client,&request,&response);};
 Ath10kPCIDevice asleep;asleep._systemSuspended=true;assert(restart(asleep)==kIOReturnNotReady && !asleep.resets && !asleep.terminations);
 Ath10kPCIDevice waiting;waiting.powerBlock=1;assert(restart(waiting)==kIOReturnNotReady && !waiting.resets && !waiting.terminations);
 Ath10kPCIDevice woke;woke.powerBlock=2;woke._power.needsRestart=true;assert(restart(woke)==0 && woke.terminations==1);
 Ath10kPCIDevice stuck;stuck.resetQuarantines=true;assert(restart(stuck)==kIOReturnNotReady && stuck.resets==1 && !stuck.terminations);
 Ath10kPCIDevice good;assert(restart(good)==0 && good.resets==1 && good.terminations==1 && good.options==kIOServiceRequired && good._reprobeRequested && good.refs==1);
 assert(!(good.options&kIOServiceSynchronous));assert(restart(good)==kIOReturnNotReady && good.terminations==1 && good.refs==1);
 Ath10kPCIDevice rejected;rejected.terminateOK=false;assert(restart(rejected)==kIOReturnNotReady && !rejected._reprobeRequested && rejected.refs==1);
 auto &s=good._connection;s.ssidLength=3;memcpy(s.ssid,"abc",3);s.operatingChannel=36;s.width=80;s.nss=2;s.signalDbm=-50;
 good._wmiScan.generation=10;good.fillControlInfo(response);
 assert(response.length==112 && response.data[32]==3 && !memcmp(response.data+33,"abc",3));
 assert(Ath10k::wmiReadLE32(response.data+4)==10 && int32_t(Ath10k::wmiReadLE32(response.data+12))==-50);
 assert(!strcmp(reinterpret_cast<char *>(response.data+68),"0.0.98"));
 assert(Ath10k::wmiReadLE32(response.data)&512);
 good._wmiScan.completed=true;good._wmiScan.failed=true;good._wmiScan.cancelRequested=true;good._wmiScan.started=true;
 good.fillControlInfo(response);assert(Ath10k::wmiReadLE32(response.data+108)==15);
 good._wmiScan.failed=false;good._wmiScan.cancelRequested=false;good.fillControlInfo(response);
 assert(Ath10k::wmiReadLE32(response.data+108)==9);
}
