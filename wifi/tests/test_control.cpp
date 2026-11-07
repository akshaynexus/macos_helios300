#include <cassert>
#include <cstring>
#include <cstdint>
#include "Ath10kWMI.hpp"
#include "../src/shared/Ath10kControl.hpp"
// Native IOKit logger; the extracted handler now records protocol metadata.
inline void IOLog(const char *,...){}
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNotFound=2,kIOReturnNotReady=3,kIOReturnExclusiveAccess=4,kIOReturnUnsupported=5,kIOReturnNotPermitted=6,kIOReturnBusy=7,kIOReturnError=8;
struct OSObject {virtual ~OSObject()=default;};
struct IOUserClient:OSObject{};
#define OSDynamicCast(type,p) dynamic_cast<type *>(p)
static void clock_get_uptime(uint64_t *p){*p=100;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *p){*p=t;}
class Ath10kPCIDevice:public OSObject {
public:
 Ath10k::ConnectionState _connection{};Ath10k::WMIBSSCache _wmiBSS{};
 Ath10k::RegulatoryPolicy _regulatory{};
 struct {bool active=true;} _wmiLive;
 Ath10k::WMIScanState _wmiScan{};
 Ath10k::WMIInitState _wmiEvents{};
 struct {UInt8 mac[6]{2,1,2,3,4,5};UInt32 requestedChainMask=3;} _wmiInit;
 struct {UInt32 htCapability=1,vhtCapability=0,rfChains=1;Ath10k::HTCCreditState credits{};} _wmi;
 bool _networkLinkUp=false,_networkEnabled=true;
 IOUserClient *_controlOwner=nullptr;
 static IOReturn controlAction(OSObject *,void *,void *,void *,void *);
 Ath10kPCIDevice(){_wmiScan.completed=true;}
 void fillControlInfo(AthControl::Response &){}
 bool requestDeviceRestart(){return false;}
 void publishConnection(){}
 void networkLink(bool up){_networkLinkUp=up;}
 void endWMIRuntime(const char *){_wmiLive.active=false;_networkLinkUp=false;}
 const char *submitNetworkPacket(const UInt8 *,UInt32,bool,UInt64){_connection.txOwned=true;return nullptr;}
};
#include "control_method.inc"
#ifndef ATH10K_STACK_PROBE
static void extensionTests(){
 using namespace AthControl;Ath10kPCIDevice d;IOUserClient a,b;Request r{};Response o{};
 auto call=[&](Operation op,IOUserClient *who=nullptr){r.operation=op;return d.controlAction(&d,who?who:&a,&r,&o,nullptr);};
 r.index=1;assert(call(Scan)==0 && !d._wmiScan.cancelRequested);r.index=0;
 assert(call(Scan)==0 && d._wmiScan.generation==1 && d._wmiScan.id==0xa037);
 assert(call(Scan)==kIOReturnBusy);d._wmiScan.attempted=true;
 assert(call(Status)==0 && (o.flags&256));r.index=1;
 assert(call(Scan)==0 && d._wmiScan.cancelRequested);r.index=0;
 d._wmiScan.completed=true;auto &s=d._connection;s.stage=Ath10k::LinkStage::Connected;
 d._controlOwner=&a;s.portAuthorized=true;s.secure=true;s.securityPhase=5;
 assert(call(Scan,&b)==kIOReturnNotPermitted);
 s.txOwned=true;assert(call(Scan)==kIOReturnBusy);s.txOwned=false;
 assert(call(Scan)==0 && d._wmiScan.generation==2 && s.portAuthorized);
 d._wmiScan.completed=true;
 r.index=1;assert(call(SecurityHold)==0 && s.eapolPause);
 r.index=0;assert(call(SecurityHold)==0 && !s.eapolPause && s.txWakePending);
 r.length=56;r.data[48]=AthSecurity::cipherCCMP;r.data[49]=1;memset(r.data,0x25,16);memset(r.data+16,0x45,16);
 s.groupCipher=AthSecurity::cipherCCMP;
 memcpy(s.installedPair,r.data,16);s.pairInstalled=true;
 assert(call(RekeyPair)==kIOReturnBadArgument);
 assert(call(RekeyGroup,&b)==kIOReturnNotPermitted);
 d._wmiScan.completed=false;d._wmiScan.cancelRequested=false;
 s.maintenance.step=8;assert(call(RekeyGroup)==kIOReturnBusy && s.portAuthorized);s.maintenance.step=0;
 s.stats.step=2;assert(call(RekeyGroup)==kIOReturnBusy && s.portAuthorized);s.stats.step=0;
 assert(call(RekeyGroup)==0 && d._wmiScan.cancelRequested && s.rekeyKind==1 && s.securityPhase==2 && !s.portAuthorized);
 assert(call(RekeyGroup)==kIOReturnBusy);
 d._wmiScan.completed=true;
 s.rekeyKind=0;s.portAuthorized=true;s.securityPhase=5;
 memcpy(s.installedGroups[1],r.data+16,32);s.groupInstalled[1]=true;s.groupIndex=1;
 assert(call(RekeyGroup)==kIOReturnBadArgument);memset(r.data,0x26,16);
 assert(call(RekeyPair)==0 && s.rekeyKind==2 && s.rekeySkipGroup && s.securityPhase==1);
 assert(call(Cancel)==0 && s.cancelRequested && d._wmiLive.active);
}

int main(){
 extensionTests();
 using namespace AthControl;
 Ath10kPCIDevice d;IOUserClient a,b;Request r{};Response o{};
 auto call=[&](Operation op,IOUserClient *who=nullptr){if(!who)who=&a;r.operation=op;return d.controlAction(&d,who,&r,&o,nullptr);};
 r.version=2;assert(call(Status)==kIOReturnBadArgument);r.version=version;
 assert(call(Status)==0 && o.version==version && o.flags==(131|512));
 assert(call(BSS)==kIOReturnNotFound);
 auto &ap=d._wmiBSS.entries[0];d._wmiBSS.count=1;ap.bssid[0]=2;ap.bssid[5]=7;ap.ssid[0]='T';ap.ssidLength=1;
 ap.capabilities=1;ap.beaconInterval=100;ap.rxChannel=1;ap.rates[0]=0x82;ap.rateCount=1;
 assert(call(BSS)==0 && o.length==52 && o.data[8]=='T');
 // Binding a supplied SSID to a hidden BSSID is explicit and preserves RSN/security checks.
 ap.hidden=true;ap.ssidLength=0;r.index=1;r.length=9;r.data[0]=1;memcpy(r.data+2,ap.bssid,6);r.data[8]='T';
 d._wmiScan.failed=true;assert(call(Connect)==kIOReturnNotReady);d._wmiScan.failed=false;
 d._wmiScan.cancelRequested=true;assert(call(Connect)==kIOReturnNotReady);d._wmiScan.cancelRequested=false;
 assert(call(Connect)==0 && d._connection.hiddenSSID);
 d._controlOwner=nullptr;Ath10k::resetDisconnected(d._connection);ap.hidden=false;ap.ssidLength=1;r.index=0;
 r.length=9;r.data[0]=1;memcpy(r.data+2,ap.bssid,6);r.data[8]='T';
 assert(call(Connect)==0 && d._controlOwner==&a);
 assert(call(Connect,&b)==kIOReturnExclusiveAccess);
 assert(call(Abort,&b)==kIOReturnNotPermitted);
 d._connection.stage=Ath10k::LinkStage::Connected;d._connection.secure=true;d._connection.groupCipher=AthSecurity::cipherCCMP;
 assert(call(ReceiveEAPOL)==kIOReturnNotReady);
 auto &s=d._connection;s.eapolCount=1;s.eapolLength[0]=20;s.eapolFrames[0][0]=42;
 assert(call(ReceiveEAPOL)==0 && o.length==20 && o.data[0]==42 && s.eapolFrames[0][0]==0);
 r.length=55;assert(call(InstallKeys)==kIOReturnBadArgument);
 r.length=56;r.data[48]=AthSecurity::cipherCCMP;r.data[49]=4;assert(call(InstallKeys)==kIOReturnBadArgument);
 r.data[49]=1;r.data[50]=9;assert(call(InstallKeys)==0 && s.securityPhase==1 && s.groupPN[0]==9);
 assert(call(InstallKeys)==kIOReturnBadArgument);
 assert(call(Authorize)==kIOReturnNotReady);
 s.securityPhase=3;s.lastTxEAPOL=true;s.txComplete=true;s.txOwned=true;
 assert(call(Authorize)==kIOReturnNotReady);s.txOwned=false;s.txStatus=1;
 assert(call(Authorize)==kIOReturnNotReady);s.txStatus=0;
 assert(call(Authorize)==0 && s.authorizeRequested);
 r.length=18;memset(r.data,0,512);r.data[12]=0x88;r.data[13]=0x8e;
 assert(call(SendEAPOL)==kIOReturnBadArgument);
 memcpy(r.data,s.bssid,6);memcpy(r.data+6,s.local,6);
 const unsigned installingPhases[]={1,2,4};
 for(unsigned phase:installingPhases){s.securityPhase=phase;assert(call(SendEAPOL)==kIOReturnNotReady && !s.txOwned);}
 s.securityPhase=5;s.rekeyKind=1;assert(call(SendEAPOL)==kIOReturnNotReady && !s.txOwned);
 s.rekeyKind=0;s.securityPhase=3;
 s.maintenance.step=8;assert(call(SendEAPOL)==kIOReturnBusy && !s.txOwned && d._wmiLive.active);s.maintenance.step=0;
 assert(call(SendEAPOL)==0 && s.txOwned);
 d.controlAction(&d,&b,nullptr,nullptr,nullptr);assert(d._wmiLive.active);
 assert(call(Disconnect,&b)==kIOReturnNotPermitted);
 s.portAuthorized=false;assert(call(Disconnect)==kIOReturnBusy);
 s.portAuthorized=true;s.txOwned=false;
 assert(call(Disconnect)==0 && d._wmiLive.active && !d._networkLinkUp);
 assert(call(Status)==0 && (o.flags&64) && (o.flags&128));
 assert(call(Connect,&b)==kIOReturnBusy);
 d.controlAction(&d,&a,nullptr,nullptr,nullptr);assert(d._wmiLive.active && !d._controlOwner);
 // The production service releases ownership only after firmware teardown.
 Ath10k::resetDisconnected(s);r={};r.length=9;r.data[0]=1;memcpy(r.data+2,ap.bssid,6);r.data[8]='T';assert(call(Connect,&b)==0 && d._controlOwner==&b);
 d.controlAction(&d,&b,nullptr,nullptr,nullptr);assert(d._wmiLive.active && !d._controlOwner && s.cancelRequested);
}

#endif // ATH10K_STACK_PROBE
