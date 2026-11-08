#include "Ath10kPCIDevice.hpp"
#include "Ath10kUserClient.hpp"
#include <IOKit/IOLib.h>
#include <kern/task.h>
#define super IOUserClient
OSDefineMetaClassAndStructors(Ath10kUserClient,IOUserClient)
// Local login sessions may control the adapter; root remains available for diagnostics.
static bool athControlAllowed(task_t task){
 return task && (IOUserClient::clientHasPrivilege(task,kIOClientPrivilegeLocalUser)==kIOReturnSuccess ||
                 IOUserClient::clientHasPrivilege(task,kIOClientPrivilegeAdministrator)==kIOReturnSuccess);
}
bool Ath10kUserClient::initWithTask(task_t task,void *security,UInt32 type,OSDictionary *properties){
 if(!athControlAllowed(task) || !super::initWithTask(task,security,type,properties))return false;
 openingTask=task;task_reference(openingTask);return true;
}
void Ath10kUserClient::free(){
 if(openingTask){task_deallocate(openingTask);openingTask=nullptr;}
 super::free();
}
bool Ath10kUserClient::start(IOService *p){
 device=OSDynamicCast(Ath10kPCIDevice,p);
 return device && super::start(p);
}
void Ath10kUserClient::stop(IOService *p){
 if(device){device->closeControl(this);device=nullptr;}
 super::stop(p);
}
IOReturn Ath10kUserClient::clientClose(){
 if(device)device->closeControl(this);
 terminate();return kIOReturnSuccess;
}
IOReturn Ath10kUserClient::externalMethod(uint32_t selector,IOExternalMethodArguments *a,IOExternalMethodDispatch *,OSObject *,void *){
 if(!device || isInactive())return kIOReturnNotAttached;
 if(selector || !a || a->structureInputSize!=sizeof(AthControl::Request) ||
    a->structureOutputSize!=sizeof(AthControl::Response) || !a->structureInput || !a->structureOutput ||
    a->scalarInputCount || a->scalarOutputCount || a->structureInputDescriptor || a->structureOutputDescriptor)return kIOReturnBadArgument;
 // Authentication at open is not enough for a port transferred to another process.
 if(current_task()!=openingTask || !athControlAllowed(openingTask))return kIOReturnNotPrivileged;
 return device->controlRequest(this,static_cast<const AthControl::Request *>(a->structureInput),static_cast<AthControl::Response *>(a->structureOutput));
}
IOReturn Ath10kPCIDevice::newUserClient(task_t task,void *security,UInt32 type,OSDictionary *properties,IOUserClient **out){
 if(type!=AthControl::clientType)return IOEthernetController::newUserClient(task,security,type,properties,out);
 if(!athControlAllowed(task))return kIOReturnNotPrivileged;
 auto *client=new Ath10kUserClient;
 if(!client)return kIOReturnNoMemory;
 if(!client->initWithTask(task,security,type,properties)){client->release();return kIOReturnError;}
 client->setProperty("IOUserClientDefaultLocking",kOSBooleanTrue);
 if(!client->attach(this)){client->release();return kIOReturnError;}
 if(!client->start(this)){client->detach(this);client->release();return kIOReturnError;}
 *out=client;return kIOReturnSuccess;
}
bool Ath10kPCIDevice::requestDeviceRestart(){
 if(_systemSuspended || _reprobeRequested || !_pci || _power.reinitRunning || (powerIOBlocked() && !powerRestartAllowed()))return false;
 if(_wmiLive.active)endWMIRuntime("app-restart-requested");
 if(_dmaQuarantine)return false;
 _reprobeRequested=true;
 _pci->setProperty("Ath10kRecoveryPhase","restart-requested");
 _pci->setProperty("Ath10kRecoveryDetail","App requested guarded driver termination and PCI rematch");
 // Only prepare under the hardware gate. IOService termination runs after
 // runAction returns, so stop() can never release a currently executing gate.
 return true;
}
void Ath10kPCIDevice::fillControlInfo(AthControl::Response &o){
 // Capability 2048: Scan takes the 42-byte options block (channels, SSID, passive).
 Ath10k::htcWriteLE32(o.data,0x2ff|2048u|(AIRPORT_ATH10K_WPASEC?1024u:0u)|(_regulatory.stationDFS?256u:0u));Ath10k::htcWriteLE32(o.data+4,_wmiScan.generation);
 Ath10k::htcWriteLE32(o.data+8,_connection.operatingChannel);
 Ath10k::htcWriteLE32(o.data+12,static_cast<UInt32>(_connection.signalDbm));
 Ath10k::htcWriteLE32(o.data+16,_connection.width);Ath10k::htcWriteLE32(o.data+20,_connection.nss);
 Ath10k::htcWriteLE32(o.data+24,_connection.securityGeneration);
 // 8: the firmware is being reloaded in place after a wake; no restart wanted.
 Ath10k::htcWriteLE32(o.data+28,(_systemSuspended?1u:0u)|(_dmaQuarantine?2u:0u)|(_power.needsRestart?4u:0u)|(_power.reinitRunning?8u:0u));
 o.data[32]=_connection.ssidLength;bcopy(_connection.ssid,o.data+33,_connection.ssidLength);
 strlcpy(reinterpret_cast<char *>(o.data+68),ATH10K_VERSION,16);o.length=112;
 const auto &r=_connection.radio;
 Ath10k::htcWriteLE32(o.data+84,(r.requestedPS?1u:0u)|(r.appliedPS?2u:0u)|(r.step?4u:0u));
 Ath10k::htcWriteLE32(o.data+88,_connection.beaconMisses);Ath10k::htcWriteLE32(o.data+92,_connection.txPowerHalfDbm);
 Ath10k::htcWriteLE32(o.data+96,_connection.powerConstraintDb);Ath10k::htcWriteLE32(o.data+100,r.completed);
 Ath10k::htcWriteLE32(o.data+104,_connection.beaconTimeoutMs);
 Ath10k::htcWriteLE32(o.data+108,(_wmiScan.completed?1u:0u)|(_wmiScan.failed?2u:0u)|(_wmiScan.cancelRequested?4u:0u)|(_wmiScan.started?8u:0u));
 // [112..113] regulatory alpha2, [114] 0, [115] bit 0 native front end; the
 // native front end (AirportAth10k) reports the country to WCL from here.
 const char *cc=_regulatory.country?_regulatory.country->alpha2:"00";
 o.data[112]=static_cast<UInt8>(cc[0]);o.data[113]=static_cast<UInt8>(cc[1]);o.data[114]=0;o.data[115]=_nativeFrontEnd?1:0;o.length=116;
#if AIRPORT_ATH10K_WPASEC
 // [116] security state, [120..125] the station MAC in use (Ath10kControl.hpp).
 const auto &s=_connection;const bool idle=s.stage==Ath10k::LinkStage::Disabled;
 const UInt8 *mac=idle?_vdevMAC:s.local;
 Ath10k::htcWriteLE32(o.data+116,(s.externalAuth && s.stage==Ath10k::LinkStage::Auth?1u:0u)|(s.pmf && !idle?2u:0u)|(s.enterprise && !idle?4u:0u)|
  (Ath10k::sameMAC(mac,_wmiInit.mac)?0u:8u)|(s.saQuery.active?16u:0u));
 bcopy(mac,o.data+120,6);o.length=128;
#endif
}
IOReturn Ath10kPCIDevice::controlRequest(OSObject *client,const AthControl::Request *r,AthControl::Response *o){
 if(!_wmiWorkLoop || isInactive())return kIOReturnNotReady;
 // Keep this object alive if termination detaches its clients immediately.
 retain();
 IOReturn result=_wmiWorkLoop->runAction(controlAction,this,client,const_cast<AthControl::Request *>(r),o);
 if(result==kIOReturnSuccess && r->operation==AthControl::Restart){
  if(!terminate(kIOServiceRequired)){_reprobeRequested=false;result=kIOReturnNotReady;}
 }
 release();return result;
}
void Ath10kPCIDevice::closeControl(OSObject *client){
 if(_wmiWorkLoop)_wmiWorkLoop->runAction(controlAction,this,client,nullptr,nullptr);
}
IOReturn Ath10kPCIDevice::controlAction(OSObject *owner,void *who,void *input,void *output,void *){
 auto *self=OSDynamicCast(Ath10kPCIDevice,owner);if(!self)return kIOReturnBadArgument;
 auto &s=self->_connection;using namespace AthControl;
 if(!input){if(self->_controlOwner==who){self->_controlOwner=nullptr;
   if(!self->_wmiScan.completed)self->_wmiScan.cancelRequested=true;
   uint64_t t=0,n=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&n);
   if(Ath10k::requestDisconnect(s,n/1000000u)){s.diagnostics.cancelOrigin=s.diagnostics.lastCancelOrigin="client-close";self->networkLink(false);self->publishConnection();}
   else if(self->_wmiLive.active && s.stage!=Ath10k::LinkStage::Disabled){s.diagnostics.cancelOrigin="client-close";s.cancelRequested=true;s.portAuthorized=false;self->networkLink(false);self->publishConnection();}}return kIOReturnSuccess;}
 const auto &r=*static_cast<const Request *>(input);auto &o=*static_cast<Response *>(output);
 if(r.version!=version || r.length>512 || r.operation>=(AIRPORT_ATH10K_WPASEC?OperationCount:UInt32(SendAuth)))return kIOReturnBadArgument;
 bzero(&o,sizeof(o));o.version=version;o.stage=static_cast<UInt32>(s.stage);o.phase=s.securityPhase;
 o.flags=128|512|((self->_wmiScan.attempted && !self->_wmiScan.completed)?256:0)|(s.disconnect!=Ath10k::DisconnectStage::None?64:0)|(self->_wmiLive.active && !self->_power.reinitRunning?1:0)|(self->_wmiScan.completed?2:0)|(self->_networkLinkUp?4:0)|(s.txOwned?8:0)|(s.secure?16:0)|(s.eapolTxFailed?32:0);
 o.count=self->_wmiBSS.count;bcopy(s.local,o.local,6);bcopy(s.bssid,o.bssid,6);
 if(s.stage==Ath10k::LinkStage::Disabled)bcopy(self->_vdevMAC,o.local,6);
 strlcpy(o.error,s.error,sizeof(o.error));
 if(r.operation==Status)return kIOReturnSuccess;
 if(r.operation==BSS){
  if(r.index>=self->_wmiBSS.count)return kIOReturnNotFound;
  const auto &b=self->_wmiBSS.entries[r.index];o.data[0]=b.ssidLength;o.data[1]=b.privacy;
  bcopy(b.bssid,o.data+2,6);bcopy(b.ssid,o.data+8,b.ssidLength);
  o.data[40]=b.advertisedChannel?b.advertisedChannel:static_cast<UInt8>(b.rxChannel);
  o.data[41]=AthSecurity::supportsWPA2(b.rsn,b.rsnLength);
  Ath10k::htcWriteLE16(o.data+42,b.rsnLength);bcopy(b.rsn,o.data+44,b.rsnLength);o.length=44+b.rsnLength;Ath10k::htcWriteLE32(o.data+o.length,b.snr);o.length+=4;
  Ath10k::htcWriteLE32(o.data+o.length,b.hidden?2u:0u);o.length+=4;
  // Native front end extension (105 bytes): what WCL's scan results need
  // beyond SSID/RSN. Capabilities, beacon interval, rates and the HT/VHT
  // elements as received. Always fits: 44+257+8+105 < 512.
  UInt8 *x=o.data+o.length;
  Ath10k::htcWriteLE16(x,b.capabilities);Ath10k::htcWriteLE16(x+2,b.beaconInterval);x[4]=b.rateCount;bcopy(b.rates,x+5,32);
  x[37]=static_cast<UInt8>((b.htPresent?1:0)|(b.htOperation?2:0)|(b.vhtPresent?4:0)|(b.vhtOperation?8:0));
  bcopy(b.ht,x+38,26);bcopy(b.htOp,x+64,22);bcopy(b.vht,x+86,12);bcopy(b.vhtOp,x+98,5);
  x[103]=b.powerConstraintPresent?1:0;x[104]=b.powerConstraintDb;o.length+=105;
#if AIRPORT_ATH10K_WPASEC
  o.data[o.length]=b.rsnxeLength;bcopy(b.rsnxe,o.data+o.length+1,18);o.length+=19;
#endif
  // WPA1 vendor element (length u8 + element): WCL needs it to recognise
  // WPA-only networks instead of taking them for WEP (privacy bit, no RSN).
  o.data[o.length]=b.wpaLength;bcopy(b.wpa,o.data+o.length+1,b.wpaLength);o.length+=1u+b.wpaLength;
  return kIOReturnSuccess;
 }
 if(r.operation==Info){self->fillControlInfo(o);return kIOReturnSuccess;}
 if(r.operation==Restart){
  if(self->_controlOwner && self->_controlOwner!=who)return kIOReturnNotPermitted;
  return self->requestDeviceRestart()?kIOReturnSuccess:kIOReturnNotReady;
 }
 if(!self->_wmiLive.active)return kIOReturnNotReady;
 // Sleep quiesce or wake re-initialisation: no new radio work.
 if(self->_suspending || self->_power.reinitRunning)return kIOReturnNotReady;
 if(s.disconnect!=Ath10k::DisconnectStage::None)return kIOReturnBusy;
 if(r.operation==RadioOptions){
  if(self->_controlOwner && self->_controlOwner!=who)return kIOReturnNotPermitted;
  if(r.index>1 || r.length)return kIOReturnBadArgument;
  s.radio.requestedPS=r.index!=0;return kIOReturnSuccess;
 }
 if(r.operation==Scan){
  if(self->_controlOwner && self->_controlOwner!=who)return kIOReturnNotPermitted;
  if(r.index>1)return kIOReturnBadArgument;
  if(r.index==1){if(!self->_wmiScan.completed)self->_wmiScan.cancelRequested=true;return kIOReturnSuccess;}
  // Options (length 42): channel mask u64 (kWMIScanPlan indices, 0 = all),
  // flags u8 (1 = passive only), SSID length u8, SSID[32] (directed probe).
  Ath10k::WMIScanOptions options{};
  if(r.length){
   if(r.length!=42 || (r.data[8]&~1u) || r.data[9]>32)return kIOReturnBadArgument;
   for(unsigned i=0;i<8;++i)options.channels|=UInt64(r.data[i])<<(8*i);
   if(options.channels>>Ath10k::kWMIScanChannelCount)return kIOReturnBadArgument;
   options.passiveOnly=(r.data[8]&1)!=0;options.ssidLength=r.data[9];bcopy(r.data+10,options.ssid,options.ssidLength);
   bool any=false;for(UInt32 i=0;i<Ath10k::kWMIScanChannelCount;++i)if(Ath10k::wmiScanSelected(&self->_regulatory,&options,i))any=true;
   if(!any)return kIOReturnUnsupported;
  }
  if(self->_wmiEvents.rfKill)return kIOReturnNotReady;
  if(s.stats.step || s.maintenance.step || s.radio.step || s.csa.phase!=Ath10k::ChannelSwitchPhase::Idle || !self->_wmiScan.completed || s.wmiOwned || s.txOwned || self->_wmi.credits.outstanding ||
     (s.stage!=Ath10k::LinkStage::Disabled && (s.stage!=Ath10k::LinkStage::Connected || !s.portAuthorized)))return kIOReturnBusy;
  // Do not build default cache/state temporaries on the kernel stack.
  // The reported double-fault frame matches the 28496-byte cache plus
  // call overhead. A shared prologue can reserve that temporary even for
  // read-only Status/Info calls that never execute the Scan branch.
  // Probe where regulatory rules allow it, plus beacon hints from earlier
  // scans; a changed mask re-sends the firmware channel list first.
  const UInt64 hints=self->_wmiBSS.onChannelMask;
  options.probe=Ath10k::wmiScanProbeMask(&self->_regulatory,hints);
  if(options.probe!=self->_scanProbeMask){self->_wmiScanChannels=Ath10k::WMIScanChannelsState{};self->_scanChannelsResend=true;}
  self->_scanOptions=options;
  const UInt32 generation=self->_wmiScan.generation+1;
  Ath10k::resetInPlace(self->_wmiScan);self->_wmiScan.generation=generation;
  self->_wmiScan.id=0xa000|((0x36+generation)&0xfff);Ath10k::resetInPlace(self->_wmiBSS);self->_wmiBSS.onChannelMask=hints;
  return kIOReturnSuccess;
 }
 if(r.operation==Connect){
  if(self->_wmiEvents.rfKill)return kIOReturnNotReady;
  if(self->_controlOwner || s.stage!=Ath10k::LinkStage::Disabled)return kIOReturnExclusiveAccess;
  // A scan cancelled for this join still left the BSSes it heard in the cache;
  // selectOpenBSS below requires the selected BSSID to be among them.
  if(!self->_wmiScan.completed || self->_wmiScan.failed || !self->_networkEnabled)return kIOReturnNotReady;
  if(r.index>3 || r.length<9 || !r.data[0] || r.data[0]>32 || r.length<8u+r.data[0] || r.data[1]>1 || (r.data[2]&1))return kIOReturnBadArgument;
  const bool extended=(r.index&2)!=0;const UInt8 *x=r.data+8+r.data[0];const UInt32 xBytes=r.length-8u-r.data[0];
  if(!extended && xBytes)return kIOReturnBadArgument;
  // Station MAC only (flags 16, nothing else): Private Wi-Fi Address, a release
  // feature. Every other extension stays behind the gate (docs/WIFI-SECURITY.md).
  const bool macOnly=extended && xBytes==10 && x[0]==16 && !x[1] && !x[8] && !x[9];
  if(extended && !macOnly && !AIRPORT_ATH10K_WPASEC)return kIOReturnUnsupported; // paused feature set
  if(extended){
   // Extended block: flags, AKM, station MAC, RSN element, RSNXE (Ath10kControl.hpp).
   if(xBytes<10 || x[8]>64 || xBytes<10u+x[8] || x[9+x[8]]>18 || xBytes!=10u+x[8]+x[9+x[8]] || (x[0]&~31u))return kIOReturnBadArgument;
   const UInt8 akm=x[1];const bool external=(x[0]&1)!=0,pmfCapable=(x[0]&2)!=0,pmfRequired=(x[0]&4)!=0,enterprise=(x[0]&8)!=0,withMAC=(x[0]&16)!=0;
   if(r.data[1] && !macOnly){
    if(akm!=AthSecurity::akm8021X && akm!=AthSecurity::akmPSK && akm!=AthSecurity::akm8021XSHA256 && akm!=AthSecurity::akmPSKSHA256 && akm!=AthSecurity::akmSAE)return kIOReturnBadArgument;
    AthSecurity::RSNInfo own;
    if(!x[8] || !AthSecurity::parseRSN(x+9,x[8],own) || !own.akm(akm) || !own.pairwiseCCMP || (pmfRequired && !pmfCapable) || (own.mfpc!=pmfCapable) || (own.mfpr!=pmfRequired))return kIOReturnBadArgument;
    if(external!=(akm==AthSecurity::akmSAE) || enterprise!=AthSecurity::akmEnterprise(akm))return kIOReturnBadArgument;
   } else if(external || pmfCapable || pmfRequired || enterprise || x[8] || x[9+x[8]])return kIOReturnBadArgument;
   if(withMAC && ((x[2]&1) || !(x[2]|x[3]|x[4]|x[5]|x[6]|x[7])))return kIOReturnBadArgument;
   const UInt8 xe=x[9+x[8]];
   if(xe && (xe<3 || x[10+x[8]]!=244 || x[11+x[8]]+2u!=xe))return kIOReturnBadArgument;
  }
  s.regulatory=self->_regulatory;s.hiddenSSID=(r.index&1)!=0;
  s.secure=r.data[1];s.ssidLength=r.data[0];bcopy(r.data+2,s.bssid,6);bcopy(r.data+8,s.ssid,s.ssidLength);bcopy(self->_vdevMAC,s.local,6);
  s.akm=AthSecurity::akmPSK;s.pmfCapable=s.pmfRequired=s.pmf=s.externalAuth=s.enterprise=false;s.stationRSNLength=s.stationRSNXELength=0;
  s.authDone=s.authTxPending=false;s.authCount=s.authRead=s.authWrite=0;s.macChange={};
  if(extended){
   if(r.data[1] && !macOnly){s.akm=x[1];s.externalAuth=(x[0]&1)!=0;s.pmfCapable=(x[0]&2)!=0;s.pmfRequired=(x[0]&4)!=0;s.enterprise=(x[0]&8)!=0;
    s.stationRSNLength=x[8];bcopy(x+9,s.stationRSN,x[8]);s.stationRSNXELength=x[9+x[8]];bcopy(x+10+x[8],s.stationRSNXE,s.stationRSNXELength);}
   if(x[0]&16){bcopy(x+2,s.local,6);s.macChange.pending=!Ath10k::sameMAC(s.local,self->_vdevMAC);}
  }
  s.firmwareHT=self->_wmi.htCapability;s.firmwareVHT=self->_wmi.vhtCapability;
  s.rfChains=static_cast<UInt8>(self->_wmiInit.requestedChainMask==1?1:2);
  const char *error=Ath10k::selectOpenBSS(s,self->_wmiBSS);
  // o.error was filled before the request ran: report this rejection's reason.
  if(error){s.error=error;strlcpy(o.error,error,sizeof(o.error));return kIOReturnUnsupported;}
  self->_controlOwner=static_cast<OSObject *>(who);s.error="none";s.stage=Ath10k::LinkStage::Scan;
  self->publishConnection();return kIOReturnSuccess;
 }
 if(self->_controlOwner!=who)return kIOReturnNotPermitted;
 if(r.operation==SecurityHold){
  if(r.index>1)return kIOReturnBadArgument;
  s.eapolPause=r.index!=0;if(!s.eapolPause)s.txWakePending=true;return kIOReturnSuccess;
 }
 if(r.operation==Cancel){
  if(!self->_wmiScan.completed)self->_wmiScan.cancelRequested=true;
  s.diagnostics.cancelOrigin="app-cancel";
  IOLog("Ath10kMac: APP CANCEL stage=%u security=%u peers=%u first-error=%s\n",static_cast<UInt32>(s.stage),s.securityPhase,s.peerCount,s.error);
  s.cancelRequested=true;s.portAuthorized=false;self->networkLink(false);self->publishConnection();return kIOReturnSuccess;
 }
 if(r.operation==Disconnect){
  uint64_t t=0,n=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&n);
  if(!Ath10k::requestDisconnect(s,n/1000000u))return kIOReturnBusy;
  IOLog("Ath10kMac: APP DISCONNECT stage=%u security=%u peers=%u first-error=%s\n",static_cast<UInt32>(s.stage),s.securityPhase,s.peerCount,s.error);
  if(!self->_wmiScan.completed)self->_wmiScan.cancelRequested=true;
  self->networkLink(false);self->publishConnection();return kIOReturnSuccess;
 }
 if(r.operation==Abort){self->endWMIRuntime("app-disconnected-reload-required");return kIOReturnSuccess;}
#if AIRPORT_ATH10K_WPASEC
 if(r.operation==SendAuth || r.operation==ReceiveAuth || r.operation==AuthResult){
  // External authentication (SAE): only while the core waits in the Auth stage.
  if(!s.externalAuth || s.stage!=Ath10k::LinkStage::Auth || s.authDone || s.cancelRequested)return kIOReturnNotReady;
  if(r.operation==ReceiveAuth){
   if(!s.authCount)return kIOReturnNotReady;
   o.length=s.authLength[s.authRead];bcopy(s.authFrames[s.authRead],o.data,o.length);
   AthSecurity::wipe(s.authFrames[s.authRead],512);s.authRead=(s.authRead+1)%2;--s.authCount;return kIOReturnSuccess;
  }
  if(r.operation==SendAuth){
   if(r.length<6 || r.length>sizeof(s.authTx) || r.index)return kIOReturnBadArgument;
   if(s.authTxPending || s.txOwned)return kIOReturnBusy;
   bcopy(r.data,s.authTx,r.length);s.authTxLength=static_cast<UInt16>(r.length);s.authTxPending=true;return kIOReturnSuccess;
  }
  if(r.index>1)return kIOReturnBadArgument;
  if(r.index==1){s.authDone=true;return kIOReturnSuccess;}
  s.error="external-authentication-failed";s.stage=Ath10k::LinkStage::Failed;s.cancelRequested=true;self->publishConnection();return kIOReturnSuccess;
 }
#endif
 if(!s.secure || s.stage!=Ath10k::LinkStage::Connected)return kIOReturnNotReady;
 if(r.operation==ReceiveEAPOL){
  if(!s.eapolCount)return kIOReturnNotReady;
  o.length=s.eapolLength[s.eapolRead];bcopy(s.eapolFrames[s.eapolRead],o.data,o.length);
  AthSecurity::wipe(s.eapolFrames[s.eapolRead],512);s.eapolRead=(s.eapolRead+1)%4;--s.eapolCount;return kIOReturnSuccess;
 }
 if(r.operation==SendEAPOL){
  // Do not acknowledge the new key before its firmware ACK/ECHO barrier.
  if(s.rekeyKind || (s.securityPhase && s.securityPhase!=3 && s.securityPhase!=5))return kIOReturnNotReady;
  if(s.maintenance.step || s.radio.blocked || s.radio.powerBlocked || s.radio.appliedPS || s.csa.blocked || s.txOwned || s.dataPending || s.txPauseMask)return kIOReturnBusy;
  if(r.length<18 || r.data[12]!=0x88 || r.data[13]!=0x8e || !Ath10k::sameMAC(r.data,s.bssid) ||
     !Ath10k::sameMAC(r.data+6,s.local) || AthSecurity::be16(r.data+16)!=r.length-18)return kIOReturnBadArgument;
  uint64_t now=0,ns=0;clock_get_uptime(&now);absolutetime_to_nanoseconds(now,&ns);s.eapolTxFailed=false;
  const char *e=self->submitNetworkPacket(r.data,r.length,false,ns/1000000u);
  if(e){self->endWMIRuntime(e);return kIOReturnError;}
  const UInt16 keyInfo=r.data[15]==3 && r.length>=21?AthSecurity::be16(r.data+19):0;
  IOLog("Ath10kMac: EAPOL TX submit id=%u type=%u key-info=0x%04x bytes=%u security=%u\n",s.txId,r.data[15],keyInfo,r.length,s.securityPhase);
  return kIOReturnSuccess;
 }
 if(r.operation==RekeyGroup || r.operation==RekeyPair){
  if(s.stats.step || s.maintenance.step || s.radio.step || s.csa.phase!=Ath10k::ChannelSwitchPhase::Idle || (r.length!=56 && (!AIRPORT_ATH10K_WPASEC || r.length!=81)) || r.data[48]!=s.groupCipher || r.data[49]>3 || !s.portAuthorized || s.securityPhase!=5 || s.wmiOwned || s.rekeyKind)return kIOReturnBusy;
  if(r.length==81 && r.data[56]>1)return kIOReturnBadArgument;
  const UInt32 groupKeyLength=s.groupCipher==AthSecurity::cipherTKIP?32:16;
  if(groupKeyLength==16)for(unsigned i=16;i<32;++i)if(r.data[16+i])return kIOReturnBadArgument;
  const bool pair=r.operation==RekeyPair;
  if(pair && AthSecurity::equal(r.data,s.installedPair,16))return kIOReturnBadArgument;
  bool sameGroup=s.groupInstalled[r.data[49]] && AthSecurity::equal(r.data+16,s.installedGroups[r.data[49]],groupKeyLength);
  for(unsigned i=0;i<4;++i)if(s.groupInstalled[i] && AthSecurity::equal(r.data+16,s.installedGroups[i],groupKeyLength) && i!=r.data[49])return kIOReturnBadArgument;
  if(sameGroup && (!pair || r.data[49]!=s.groupIndex))return kIOReturnBadArgument;
  bcopy(r.data,s.temporalKey,16);bcopy(r.data+16,s.groupKey,32);s.groupIndex=r.data[49];bcopy(r.data+50,s.groupRSC,6);
  if(r.length==81 && r.data[56])Ath10k::installIGTK(s,r.data+57);
  if(!self->_wmiScan.completed)self->_wmiScan.cancelRequested=true;
  s.rekeyKind=pair?2:1;s.rekeySkipGroup=pair && sameGroup;s.securityPhase=pair?1:2;
  s.portAuthorized=false;s.submitted=false;s.authorizeRequested=false;s.securityAck=false;
  uint64_t t=0,n=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&n);s.sinceMs=n/1000000u;
  return kIOReturnSuccess;
 }
 if(r.operation==InstallKeys){
  if((r.length!=56 && (!AIRPORT_ATH10K_WPASEC || r.length!=81)) || r.data[48]!=s.groupCipher || r.data[49]>3 || s.securityPhase || s.portAuthorized)return kIOReturnBadArgument;
  if(r.length==81 && r.data[56]>1)return kIOReturnBadArgument;
  if(s.groupCipher==AthSecurity::cipherCCMP)for(unsigned i=16;i<32;++i)if(r.data[16+i])return kIOReturnBadArgument;
  bcopy(r.data,s.temporalKey,16);bcopy(r.data+16,s.groupKey,32);s.groupIndex=r.data[49];bcopy(r.data+50,s.groupRSC,6);
  if(r.length==81 && r.data[56])Ath10k::installIGTK(s,r.data+57);
  UInt64 pn=0;for(unsigned i=0;i<6;++i)pn|=UInt64(s.groupRSC[i])<<(8*i);
  for(auto &v:s.groupPN)v=pn;
  s.securityPhase=1;s.submitted=false;return kIOReturnSuccess;
 }
 if(r.operation==Authorize){
  if(s.securityPhase!=3 || !s.lastTxEAPOL || s.txOwned || !s.txComplete || s.txStatus || s.eapolTxFailed)return kIOReturnNotReady;
  s.authorizeRequested=true;return kIOReturnSuccess;
 }
 return kIOReturnUnsupported;
}
