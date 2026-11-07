#import <Cocoa/Cocoa.h>
#import <IOKit/IOKitLib.h>
#import <CommonCrypto/CommonHMAC.h>
#import <CommonCrypto/CommonCryptor.h>
#import <CommonCrypto/CommonKeyDerivation.h>
#import <Security/Security.h>
#import <ServiceManagement/ServiceManagement.h>
#include <unistd.h>
#include "../src/shared/Ath10kControl.hpp"
#include "../src/shared/Ath10kSupplicant.hpp"
struct MacCrypto {
 static bool random(uint8_t *p,size_t n){return SecRandomCopyBytes(kSecRandomDefault,n,p)==errSecSuccess;}
 static bool hmac(const uint8_t *k,size_t kl,const uint8_t *p,size_t n,uint8_t *out){CCHmac(kCCHmacAlgSHA1,k,kl,p,n,out);return true;}
 static bool decrypt(const uint8_t *key,const uint8_t *in,uint8_t *out){size_t done=0;return CCCrypt(kCCDecrypt,kCCAlgorithmAES,kCCOptionECBMode,key,16,nullptr,in,16,out,16,&done)==kCCSuccess && done==16;}
};
static uint32_t readLE32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
@interface Controller:NSObject<NSApplicationDelegate> {
 NSWindow *_window;
 NSStatusItem *_statusItem;
 NSMenu *_statusMenu;
 NSMenuItem *_menuHeading,*_menuProgress;
 NSPopUpButton *_networks;
 NSSecureTextField *_password;
 NSTextField *_status,*_statusHeading;
 NSButton *_connect,*_disconnect,*_refresh,*_remember,*_copyNetworkDetails;
 NSMutableArray<NSData *> *_rows;
 NSTimer *_timer;
 io_connect_t _driver;
 AthSecurity::Supplicant<MacCrypto> _supplicant;
 uint8_t _pending[160];size_t _pendingLength;
 uint8_t _pendingRekeyKeys[AthSecurity::Supplicant<MacCrypto>::keyPayloadLength];uint32_t _pendingRekeyOperation;
 NSString *_pendingSSID,*_passwordForSave,*_connectedSSID;
 NSString *_menuProgressText;
 NSTimeInterval _scanStartedAt;
 NSTimeInterval _wakeNextCheckAt,_wakeDeadline;
 NSUInteger _iconFrame,_wakeRestartAttempts;
 BOOL _disconnecting,_rekeyKeysReady,_rekeyResponsePending;
 BOOL _session,_secure,_keysSubmitted,_m4Sent,_authorized,_failed,_scanInProgress,_waitingForDriver,_waitingForInitialScan,_autoConnectAttempted,_didSaveCredentials,_menuShowsConnected,_wakeRecoveryPending,_wakeRestartIssued;
}
-(void)updateMenuProgress:(NSString *)text;
-(void)rebuildStatusMenu;
-(void)updateStatusIcon;
-(void)clearRekeyState;
-(kern_return_t)openDriver;
-(void)beginRecoveryWithHeading:(NSString *)heading message:(NSString *)message;
-(void)requestSafeAdapterRestartAt:(NSTimeInterval)now;
-(void)workspaceDidWake:(NSNotification *)note;
-(void)recoverAfterWake;
-(NSInteger)rowIndexForSSID:(NSString *)ssid;
-(void)selectNetworkForSSID:(NSString *)ssid;
-(void)selectNetworkData:(NSData *)data;
-(void)copySelectedNetworkDetails:(id)sender;
@end
@implementation Controller
-(void)clearRekeyState {
 AthSecurity::wipe(_pendingRekeyKeys,sizeof(_pendingRekeyKeys));_pendingRekeyOperation=0;_rekeyKeysReady=NO;_rekeyResponsePending=NO;
}
-(kern_return_t)call:(uint32_t)op request:(AthControl::Request *)input response:(AthControl::Response *)output {
 AthControl::Request empty{};if(!input)input=&empty;input->operation=op;
 AthControl::Response temporary{};if(!output)output=&temporary;
 size_t size=sizeof(*output);kern_return_t r=IOConnectCallStructMethod(_driver,0,input,sizeof(*input),output,&size);
 if(r==KERN_SUCCESS && (size!=sizeof(*output)||output->version!=AthControl::version))return kIOReturnBadMessageID;
 return r;
}
-(void)fail:(NSString *)message {
 _statusHeading.stringValue=@"Connection failed";_status.stringValue=message;_failed=YES;_pendingLength=0;
 if(_session)[self call:AthControl::Abort request:nullptr response:nullptr];
 _supplicant.reset();AthSecurity::wipe(_pending,sizeof(_pending));[self clearRekeyState];_password.stringValue=@"";
 _passwordForSave=nil;_pendingSSID=nil;_connect.enabled=NO;_disconnect.enabled=NO;_remember.enabled=YES;
 [self updateMenuProgress:message];[self rebuildStatusMenu];
}
-(NSString *)ssidForRow:(const uint8_t *)b {
 NSString *ssid=[[NSString alloc] initWithBytes:b+8 length:b[0] encoding:NSUTF8StringEncoding];
 return ssid.length?ssid:nil;
}
-(NSString *)securityForRow:(const uint8_t *)b length:(NSUInteger)length {
 if(!b[1])return @"Open";
 if(!b[41]||length<44)return @"Unsupported security";
 const uint16_t n=AthSecurity::le16(b+42);
 if(n>length-44)return @"Unsupported security";
 const uint8_t group=AthSecurity::wpa2GroupCipher(b+44,n);
 return group==AthSecurity::cipherTKIP?@"WPA2 AES · TKIP group":@"WPA2 AES";
}
-(NSDictionary *)keychainQueryForSSID:(NSString *)ssid {
 return @{(__bridge id)kSecClass:(__bridge id)kSecClassGenericPassword,
          (__bridge id)kSecAttrService:@"br.com.jonasw8.Ath10kConnectivity",
          (__bridge id)kSecAttrAccount:ssid};
}
-(NSString *)savedPasswordForSSID:(NSString *)ssid {
 NSMutableDictionary *query=[[self keychainQueryForSSID:ssid] mutableCopy];
 query[(__bridge id)kSecReturnData]=@YES;query[(__bridge id)kSecMatchLimit]=(__bridge id)kSecMatchLimitOne;
 CFTypeRef result=nullptr;OSStatus status=SecItemCopyMatching((__bridge CFDictionaryRef)query,&result);
 if(status!=errSecSuccess||!result)return nil;
 NSData *data=CFBridgingRelease(result);return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}
-(BOOL)savePassword:(NSString *)password forSSID:(NSString *)ssid {
 NSData *data=[password dataUsingEncoding:NSUTF8StringEncoding];if(!data.length)return NO;
 NSDictionary *query=[self keychainQueryForSSID:ssid];
 NSDictionary *values=@{(__bridge id)kSecValueData:data};
 OSStatus status=SecItemUpdate((__bridge CFDictionaryRef)query,(__bridge CFDictionaryRef)values);
 if(status==errSecItemNotFound){NSMutableDictionary *item=[query mutableCopy];[item addEntriesFromDictionary:values];item[(__bridge id)kSecAttrAccessible]=(__bridge id)kSecAttrAccessibleWhenUnlocked;status=SecItemAdd((__bridge CFDictionaryRef)item,nullptr);}
 return status==errSecSuccess;
}
-(void)deleteSavedPasswordForSSID:(NSString *)ssid {
 if(ssid.length)SecItemDelete((__bridge CFDictionaryRef)[self keychainQueryForSSID:ssid]);
}
-(NSArray<NSString *> *)savedNetworks {
 NSUserDefaults *defaults=NSUserDefaults.standardUserDefaults;
 NSArray *stored=[defaults arrayForKey:@"Ath10kSavedNetworks"];
 if(stored.count)return [stored filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(id value,NSDictionary *bindings){(void)bindings;return [value isKindOfClass:NSString.class]&&[(NSString *)value length]>0;}]];
 NSString *legacy=[defaults stringForKey:@"Ath10kLastSSID"];
 return legacy.length?@[legacy]:@[];
}
-(void)prioritizeSSID:(NSString *)ssid {
 if(!ssid.length)return;
 NSMutableArray<NSString *> *profiles=[[self savedNetworks] mutableCopy];[profiles removeObject:ssid];[profiles insertObject:ssid atIndex:0];
 [NSUserDefaults.standardUserDefaults setObject:profiles forKey:@"Ath10kSavedNetworks"];
 [NSUserDefaults.standardUserDefaults setObject:ssid forKey:@"Ath10kLastSSID"];
}
-(void)forgetSSID:(NSString *)ssid {
 if(!ssid.length)return;
 [self deleteSavedPasswordForSSID:ssid];NSMutableArray<NSString *> *profiles=[[self savedNetworks] mutableCopy];[profiles removeObject:ssid];
 NSUserDefaults *defaults=NSUserDefaults.standardUserDefaults;
 if(profiles.count){[defaults setObject:profiles forKey:@"Ath10kSavedNetworks"];[defaults setObject:profiles.firstObject forKey:@"Ath10kLastSSID"];}
 else{[defaults removeObjectForKey:@"Ath10kSavedNetworks"];[defaults removeObjectForKey:@"Ath10kLastSSID"];}
 [self rebuildStatusMenu];
}
-(NSInteger)rowIndexForSSID:(NSString *)ssid {
 for(NSUInteger i=0;i<_rows.count;++i){const uint8_t *b=static_cast<const uint8_t *>(_rows[i].bytes);if([[self ssidForRow:b] isEqualToString:ssid])return (NSInteger)i;}
 return NSNotFound;
}
-(void)selectNetworkForSSID:(NSString *)ssid {
 if(!ssid.length)return;
 NSArray<NSMenuItem *> *items=_networks.itemArray;
 for(NSUInteger i=0;i<items.count;++i){NSData *data=items[i].representedObject;
  if([data isKindOfClass:NSData.class]&&[[self ssidForRow:(const uint8_t *)data.bytes] isEqualToString:ssid]){[_networks selectItemAtIndex:(NSInteger)i];_copyNetworkDetails.enabled=YES;return;}
 }
}
-(void)selectNetworkData:(NSData *)data {
 if(!data)return;
 NSArray<NSMenuItem *> *items=_networks.itemArray;
 for(NSUInteger i=0;i<items.count;++i)if(items[i].representedObject==data){[_networks selectItemAtIndex:(NSInteger)i];_copyNetworkDetails.enabled=YES;return;}
}
-(void)networkSelectionChanged:(id)sender {
 (void)sender;if(_session)return;
 NSData *data=_networks.selectedItem.representedObject;
 if(![data isKindOfClass:NSData.class]||[_rows indexOfObjectIdenticalTo:data]==NSNotFound)return;
 _copyNetworkDetails.enabled=YES;
 const uint8_t *b=(const uint8_t *)data.bytes;NSString *ssid=[self ssidForRow:b]?:@"network";
 NSString *saved=b[1]?[self savedPasswordForSSID:ssid]:nil;_password.stringValue=saved?:@"";
 if(b[40]>=52&&b[40]<=144){_statusHeading.stringValue=@"Channel not supported";_status.stringValue=@"This preview cannot connect on DFS channels. Use channels 1–13, 36–48, or 149–165.";return;}
 if(b[1]&&!b[41]){_statusHeading.stringValue=@"Security not supported";_status.stringValue=@"This driver needs WPA2-Personal with AES/CCMP pairwise. TKIP is supported only for the group key; WPA3, required PMF, TKIP-only pairwise, and enterprise networks are unsupported.";return;}
 _statusHeading.stringValue=@"Network selected";
 _status.stringValue=b[1]?[NSString stringWithFormat:@"Selected %@. Enter its WPA2 password if it is not saved in Keychain.",ssid]:[NSString stringWithFormat:@"Selected %@, an open network.",ssid];
}
-(kern_return_t)openDriver {
 if(_driver)return KERN_SUCCESS;
 io_service_t service=IOServiceGetMatchingService(kIOMasterPortDefault,IOServiceMatching("Ath10kPCIDevice"));
 if(!service)return kIOReturnNotFound;
 kern_return_t result=IOServiceOpen(service,mach_task_self(),AthControl::clientType,&_driver);IOObjectRelease(service);return result;
}
-(void)beginRecoveryWithHeading:(NSString *)heading message:(NSString *)message {
 NSTimeInterval now=NSProcessInfo.processInfo.systemUptime;
 _wakeRecoveryPending=YES;_wakeRestartIssued=NO;_wakeRestartAttempts=0;_wakeDeadline=now+120.0;_wakeNextCheckAt=now;
 _failed=NO;_scanInProgress=NO;_waitingForInitialScan=NO;
 _connect.enabled=NO;_networks.enabled=NO;_password.enabled=NO;_refresh.enabled=NO;
 _statusHeading.stringValue=heading;_status.stringValue=message;
 [self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];
}
-(void)requestSafeAdapterRestartAt:(NSTimeInterval)now {
 AthControl::Request restart{};
 kern_return_t result=[self call:AthControl::Restart request:&restart response:nullptr];
 if(result){
  if(result==kIOReturnNotAttached&&_driver){IOServiceClose(_driver);_driver=0;}
  ++_wakeRestartAttempts;_wakeNextCheckAt=now+1.0;
  if(_wakeRestartAttempts>=3){
   _wakeRecoveryPending=NO;_failed=YES;_statusHeading.stringValue=@"Safe Wi-Fi restart unavailable";
   _status.stringValue=[NSString stringWithFormat:@"Ath10kMac refused a safe adapter restart (0x%x). Restart macOS and try again.",result];
  }else{
   _statusHeading.stringValue=@"Retrying safe Wi-Fi restart";
   _status.stringValue=[NSString stringWithFormat:@"The driver has not accepted a safe adapter restart yet (0x%x). Retrying…",result];
  }
  [self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];return;
 }
 if(_driver){IOServiceClose(_driver);_driver=0;}
 _wakeRestartIssued=YES;_wakeNextCheckAt=now+1.0;
 _statusHeading.stringValue=@"Restarting Wi-Fi safely";
 _status.stringValue=@"The adapter is inactive. The driver accepted a safe restart; waiting for it to return…";
 [self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];
}
-(void)workspaceDidWake:(NSNotification *)note {
 if(!NSThread.isMainThread){dispatch_async(dispatch_get_main_queue(),^{[self workspaceDidWake:note];});return;}
 (void)note;if(!_timer)return;
 [self beginRecoveryWithHeading:@"Restoring Wi-Fi after wake" message:@"Checking the adapter state and restarting it safely if needed…"];
}
-(void)recoverAfterWake {
 NSTimeInterval now=NSProcessInfo.processInfo.systemUptime;if(now<_wakeNextCheckAt)return;
 if(now>_wakeDeadline){_wakeRecoveryPending=NO;_failed=YES;_statusHeading.stringValue=@"Wi-Fi recovery timed out";_status.stringValue=@"The adapter did not return after a safe restart. Restart macOS, then try again.";[self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];return;}
 if(_wakeRestartIssued){
  kern_return_t result=[self openDriver];if(result){_wakeNextCheckAt=now+1.0;return;}
  _wakeRecoveryPending=NO;_wakeRestartIssued=NO;_waitingForDriver=YES;_waitingForInitialScan=NO;_scanInProgress=NO;_scanStartedAt=now;_autoConnectAttempted=NO;_session=NO;_secure=NO;_disconnecting=NO;_menuShowsConnected=NO;_connectedSSID=nil;
  _keysSubmitted=NO;_m4Sent=NO;_authorized=NO;_failed=NO;_pendingLength=0;_pendingSSID=nil;_passwordForSave=nil;_supplicant.reset();AthSecurity::wipe(_pending,sizeof(_pending));[self clearRekeyState];
  [_rows removeAllObjects];[_networks removeAllItems];_connect.enabled=NO;_refresh.enabled=NO;
  _statusHeading.stringValue=@"Starting Wi-Fi after wake";_status.stringValue=@"The adapter is restarting safely. It will scan and reconnect to your preferred saved network when ready.";
  [self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];return;
 }
 if(!_driver){kern_return_t opened=[self openDriver];if(opened){_wakeNextCheckAt=now+1.0;return;}}
 AthControl::Response info{};kern_return_t result=[self call:AthControl::Info request:nullptr response:&info];
 if(result){
  if(result==kIOReturnNotAttached&&_driver){IOServiceClose(_driver);_driver=0;}
  if(result==kIOReturnNotAttached||result==kIOReturnNotReady){_wakeNextCheckAt=now+1.0;return;}
  _wakeNextCheckAt=now+1.0;return;
 }
 uint32_t state=readLE32(info.data+28);
 if(state&1u){_wakeNextCheckAt=now+1.0;return;}
 if(state&2u){_wakeRecoveryPending=NO;_failed=YES;_statusHeading.stringValue=@"Wi-Fi recovery is blocked";_status.stringValue=@"The driver reports a DMA safety quarantine. It refused automatic restart; a full system restart may be required.";[self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];return;}
 if(state&4u){[self requestSafeAdapterRestartAt:now];return;}
 AthControl::Response status{};result=[self call:AthControl::Status request:nullptr response:&status];
 if(result){_wakeNextCheckAt=now+1.0;return;}
 if(!(status.flags&1u)){
  // Some wake paths leave the service inactive without setting the explicit
  // restart bit. The driver's Restart operation still performs its guarded
  // quiesce/rematch checks and refuses quarantine or suspended hardware.
  [self requestSafeAdapterRestartAt:now];return;
 }
 _wakeRecoveryPending=NO;_autoConnectAttempted=NO;
 if(status.flags&4){_menuShowsConnected=YES;_statusHeading.stringValue=@"Wi-Fi restored";_status.stringValue=@"The driver kept the connection through wake.";_connect.enabled=NO;_disconnect.enabled=YES;_networks.enabled=NO;_password.enabled=NO;_refresh.enabled=NO;}
 else if(status.stage!=0){
  if(!_session){[self requestSafeAdapterRestartAt:now];return;}
  kern_return_t abortResult=[self call:AthControl::Abort request:nullptr response:nullptr];
  if(abortResult){[self requestSafeAdapterRestartAt:now];return;}
  _session=NO;_disconnecting=NO;_secure=NO;_connectedSSID=nil;_menuShowsConnected=NO;_supplicant.reset();AthSecurity::wipe(_pending,sizeof(_pending));_pendingLength=0;[self clearRekeyState];_wakeRecoveryPending=YES;_wakeNextCheckAt=now+1.0;return;
 }
 else{_session=NO;_disconnecting=NO;_connectedSSID=nil;_menuShowsConnected=NO;_connect.enabled=YES;_networks.enabled=YES;_password.enabled=YES;_remember.enabled=YES;_refresh.enabled=YES;_statusHeading.stringValue=@"Checking saved networks";_status.stringValue=@"The adapter is ready. Scanning and reconnecting to your preferred saved network…";[self refresh:nil];}
 [self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];
}
-(NSString *)connectionProgressForStatus:(const AthControl::Response &)status {
 if(status.flags&32u)return @"Waiting for the router to retry a WPA2 security message…";
 if(_secure&&status.stage>0&&status.stage<10){
  switch(status.stage){
   case 1:return @"Starting the connection with the access point…";
   case 2:return @"Starting the Wi-Fi interface…";
   case 3:return @"Registering the access point…";
   case 4:return @"Authenticating with the access point…";
   case 5:return @"Associating with the network…";
   case 6:return @"Completing access point setup…";
   case 7:return @"Authorizing the network link…";
   case 8:return @"Bringing the network interface up…";
   case 9:return @"Checking the firmware connection state…";
  }
 }
 if(_secure){
  if(_rekeyKeysReady||_rekeyResponsePending)return @"Refreshing WPA2 session keys…";
  if(!_keysSubmitted)return @"Waiting for the router’s WPA2 handshake…";
  if(!_m4Sent)return @"Installing WPA2 session keys…";
  if(!_authorized)return @"Authorizing the secured link…";
  return @"Waiting for macOS to bring the network interface up…";
 }
 switch(status.stage){
  case 1:return @"Starting the connection with the access point…";
  case 2:return @"Starting the Wi-Fi interface…";
  case 3:return @"Registering the access point…";
  case 4:return @"Authenticating with the access point…";
  case 5:return @"Associating with the network…";
  case 6:return @"Completing access point setup…";
  case 7:return @"Authorizing the network link…";
  case 8:return @"Bringing the network interface up…";
  case 9:return @"Checking the firmware connection state…";
  case 10:return @"Waiting for macOS to configure the network…";
  default:return @"Waiting for the device and router to finish connecting…";
 }
}
-(void)updateStatusIcon {
 if(!_statusItem.button)return;
 BOOL connected=_session&&_menuShowsConnected&&!_disconnecting;
 BOOL connecting=!_failed&&(_waitingForDriver||_waitingForInitialScan||_scanInProgress||_disconnecting||(_session&&!_disconnecting&&!connected)||_wakeRecoveryPending);
 NSImage *image=[[NSImage alloc] initWithSize:NSMakeSize(18,18)];[image lockFocus];
 NSColor *ink=NSColor.labelColor;
 CGFloat alpha[3]={1,1,1};
 if(connecting){NSUInteger frame=_iconFrame++%6;alpha[0]=(frame==0||frame==1||frame==2)?1.0:0.22;alpha[1]=(frame==1||frame==2||frame==3)?1.0:0.22;alpha[2]=(frame==2||frame==3||frame==4)?1.0:0.22;}
 else if(!connected||_failed){alpha[0]=0.38;alpha[1]=0.38;alpha[2]=0.38;}
 for(NSUInteger i=0;i<3;++i){NSBezierPath *arc=[NSBezierPath bezierPath];arc.lineWidth=1.5;arc.lineCapStyle=NSLineCapStyleRound;
  CGFloat radii[3]={4.4,7.6,10.8};[arc appendBezierPathWithArcWithCenter:NSMakePoint(9,2.5) radius:radii[i] startAngle:45 endAngle:135];
  [[ink colorWithAlphaComponent:alpha[i]] setStroke];[arc stroke];}
 NSBezierPath *dot=[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(8,1.5,2,2)];[ink setFill];[dot fill];
 if(!connected||_failed){NSBezierPath *slash=[NSBezierPath bezierPath];slash.lineWidth=1.7;slash.lineCapStyle=NSLineCapStyleRound;[slash moveToPoint:NSMakePoint(3,3)];[slash lineToPoint:NSMakePoint(15,15)];[ink setStroke];[slash stroke];}
 [image unlockFocus];[image setTemplate:YES];_statusItem.button.image=image;
 NSString *tip=connected?[NSString stringWithFormat:@"Connected to %@",_connectedSSID?:@"Wi-Fi"]:(_disconnecting?@"Disconnecting from Wi-Fi":(_wakeRecoveryPending?@"Restoring Wi-Fi after wake":(connecting?(_waitingForDriver?@"Starting Ath10k Wi-Fi":@"Connecting to Wi-Fi"):(_failed?@"Wi-Fi connection needs attention":@"Ath10k Wi-Fi disconnected"))));
 _statusItem.button.toolTip=tip;
}
-(void)updateMenuProgress:(NSString *)text {
 if(![text isEqualToString:_menuProgressText]){_menuProgressText=[text copy];if(_menuProgress)_menuProgress.title=_menuProgressText;}
 if(_menuHeading){NSString *summary=_failed?@"Wi-Fi needs attention":(_wakeRecoveryPending?@"Restoring Wi-Fi after wake":(_session?(_disconnecting?@"Disconnecting…":(_menuShowsConnected?[NSString stringWithFormat:@"Connected to %@",_connectedSSID?:@"network"]:[NSString stringWithFormat:@"Connecting to %@",_connectedSSID?:@"network"])):@"Ath10k Wi-Fi"));_menuHeading.title=summary;}
 [self updateStatusIcon];
}
-(void)rebuildStatusMenu {
 if(!_statusItem)return;
 NSMenu *menu=[[NSMenu alloc] initWithTitle:@"Ath10k Wi-Fi"];
 NSString *summary=_failed?@"Wi-Fi needs attention":(_wakeRecoveryPending?@"Restoring Wi-Fi after wake":(_session?(_disconnecting?@"Disconnecting…":(_menuShowsConnected?[NSString stringWithFormat:@"Connected to %@",_connectedSSID?:@"network"]:[NSString stringWithFormat:@"Connecting to %@",_connectedSSID?:@"network"])):@"Ath10k Wi-Fi"));
 _menuHeading=[[NSMenuItem alloc] initWithTitle:summary action:nil keyEquivalent:@""];_menuHeading.enabled=NO;[menu addItem:_menuHeading];
 _menuProgress=[[NSMenuItem alloc] initWithTitle:_menuProgressText?:(_waitingForDriver?@"Wi-Fi adapter is starting…":(_failed?@"Connection needs attention":@"Ready")) action:nil keyEquivalent:@""];_menuProgress.enabled=NO;[menu addItem:_menuProgress];
 if(_failed){NSMenuItem *retry=[[NSMenuItem alloc] initWithTitle:@"Retry Wi-Fi" action:@selector(retryConnection:) keyEquivalent:@""];retry.target=self;retry.enabled=_driver!=0;[menu addItem:retry];}
 NSMenuItem *scan=[[NSMenuItem alloc] initWithTitle:_scanInProgress?@"Scanning…":@"Scan Networks…" action:@selector(refresh:) keyEquivalent:@""];
 scan.target=self;scan.enabled=_driver&&!_session&&!_scanInProgress&&!_waitingForDriver&&!_waitingForInitialScan&&!_wakeRecoveryPending;[menu addItem:scan];[menu addItem:NSMenuItem.separatorItem];
 NSArray<NSString *> *profiles=[self savedNetworks];
 if(profiles.count){NSMenuItem *savedHeading=[[NSMenuItem alloc] initWithTitle:@"Saved Networks · preferred first" action:nil keyEquivalent:@""];savedHeading.enabled=NO;[menu addItem:savedHeading];
  for(NSString *ssid in profiles){NSInteger row=[self rowIndexForSSID:ssid];NSMenuItem *profile=[[NSMenuItem alloc] initWithTitle:row==NSNotFound?[NSString stringWithFormat:@"%@  ·  not in range",ssid]:ssid action:nil keyEquivalent:@""];
   NSMenu *actions=[[NSMenu alloc] initWithTitle:ssid];NSMenuItem *connect=[[NSMenuItem alloc] initWithTitle:@"Connect" action:@selector(connectSavedNetwork:) keyEquivalent:@""];connect.target=self;connect.representedObject=ssid;connect.enabled=row!=NSNotFound;[actions addItem:connect];
   NSMenuItem *preferred=[[NSMenuItem alloc] initWithTitle:@"Make Preferred" action:@selector(makePreferredNetwork:) keyEquivalent:@""];preferred.target=self;preferred.representedObject=ssid;preferred.state=[profiles.firstObject isEqualToString:ssid]?NSControlStateValueOn:NSControlStateValueOff;[actions addItem:preferred];
   [actions addItem:NSMenuItem.separatorItem];NSMenuItem *forget=[[NSMenuItem alloc] initWithTitle:@"Forget Network" action:@selector(forgetNetworkFromMenu:) keyEquivalent:@""];forget.target=self;forget.representedObject=ssid;[actions addItem:forget];profile.submenu=actions;[menu addItem:profile];
  }
  [menu addItem:NSMenuItem.separatorItem];
 }
 NSMenuItem *section=[[NSMenuItem alloc] initWithTitle:@"Nearby Networks" action:nil keyEquivalent:@""];section.enabled=NO;[menu addItem:section];
 if(!_rows.count){NSMenuItem *empty=[[NSMenuItem alloc] initWithTitle:@"Scan to find networks" action:nil keyEquivalent:@""];empty.enabled=NO;[menu addItem:empty];}
 for(NSUInteger i=0;i<_rows.count;++i){const uint8_t *b=static_cast<const uint8_t *>(_rows[i].bytes);NSString *ssid=[self ssidForRow:b]?:@"Hidden network";
  NSString *security=[self securityForRow:b length:_rows[i].length];
  BOOL saved=[profiles containsObject:ssid];NSString *title=[NSString stringWithFormat:@"%@%@  ·  %@  ·  Ch %u",saved?@"★ ":@"",ssid,security,b[40]];
  NSMenuItem *item=[[NSMenuItem alloc] initWithTitle:title action:@selector(selectNetworkFromMenu:) keyEquivalent:@""];
  item.target=self;item.representedObject=_rows[i];item.state=(_menuShowsConnected&&[_connectedSSID isEqualToString:ssid])?NSControlStateValueOn:NSControlStateValueOff;item.enabled=!b[1]||b[41];[menu addItem:item];
 }
 [menu addItem:NSMenuItem.separatorItem];
 NSMenuItem *disconnect=[[NSMenuItem alloc] initWithTitle:@"Disconnect" action:@selector(disconnect:) keyEquivalent:@""];disconnect.target=self;disconnect.enabled=_session&&!_disconnecting;[menu addItem:disconnect];
 NSMenuItem *open=[[NSMenuItem alloc] initWithTitle:@"Open Ath10k Connectivity…" action:@selector(showWindow:) keyEquivalent:@""];open.target=self;[menu addItem:open];
 NSMenuItem *launch=nil;
 if(@available(macOS 13.0,*)){launch=[[NSMenuItem alloc] initWithTitle:@"Launch at Login" action:@selector(toggleLaunchAtLogin:) keyEquivalent:@""];launch.target=self;launch.state=[SMAppService.mainAppService status]==SMAppServiceStatusEnabled?NSControlStateValueOn:NSControlStateValueOff;}
 else{launch=[[NSMenuItem alloc] initWithTitle:@"Launch at Login (macOS 13+)" action:nil keyEquivalent:@""];launch.enabled=NO;}
 [menu addItem:launch];
 [menu addItem:NSMenuItem.separatorItem];NSMenuItem *quit=[[NSMenuItem alloc] initWithTitle:@"Quit Ath10k Connectivity" action:@selector(terminate:) keyEquivalent:@""];quit.target=NSApp;[menu addItem:quit];
 _statusMenu=menu;_statusItem.menu=menu;[self updateStatusIcon];
}
-(void)connectSavedNetwork:(NSMenuItem *)sender {
 NSString *ssid=sender.representedObject;if(!ssid.length)return;NSInteger index=[self rowIndexForSSID:ssid];if(index==NSNotFound)return;
 [self selectNetworkForSSID:ssid];const uint8_t *b=static_cast<const uint8_t *>(_rows[(NSUInteger)index].bytes);
 if(b[1]){NSString *password=[self savedPasswordForSSID:ssid];_password.stringValue=password?:@"";if(!password.length){[self showWindow:nil];[_window makeFirstResponder:_password];_statusHeading.stringValue=@"Password needed";_status.stringValue=@"Enter the password for this saved WPA2 network, then connect.";return;}}
 [self connect:nil];
}
-(void)makePreferredNetwork:(NSMenuItem *)sender {[self prioritizeSSID:sender.representedObject];[self rebuildStatusMenu];}
-(void)forgetNetworkFromMenu:(NSMenuItem *)sender {[self forgetSSID:sender.representedObject];}
-(void)retryConnection:(NSMenuItem *)sender {
 (void)sender;if(!_driver||!_failed)return;
 _supplicant.reset();AthSecurity::wipe(_pending,sizeof(_pending));[self clearRekeyState];_pendingLength=0;
 [self beginRecoveryWithHeading:@"Retrying Wi-Fi" message:@"Checking the driver state and restarting the adapter safely if needed…"];
}
-(void)toggleLaunchAtLogin:(NSMenuItem *)sender {
 (void)sender;
 if(@available(macOS 13.0,*)){
  NSError *error=nil;BOOL enabled=SMAppService.mainAppService.status==SMAppServiceStatusEnabled;
  BOOL ok=enabled?[SMAppService.mainAppService unregisterAndReturnError:&error]:[SMAppService.mainAppService registerAndReturnError:&error];
  if(!ok){_statusHeading.stringValue=@"Launch at Login unchanged";_status.stringValue=error.localizedDescription?:@"macOS could not update the login item.";}
  else{_statusHeading.stringValue=enabled?@"Launch at Login off":@"Launch at Login on";_status.stringValue=enabled?@"Ath10k Connectivity will no longer open automatically when you sign in.":@"Ath10k Connectivity will start in the menu bar when you sign in. The window stays hidden.";}
 }else{_statusHeading.stringValue=@"Not available";_status.stringValue=@"Automatic launch requires macOS 13 or later.";}
 [self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];
}
-(void)selectNetworkFromMenu:(NSMenuItem *)sender {
 NSData *data=sender.representedObject;if(![data isKindOfClass:NSData.class]||[_rows indexOfObjectIdenticalTo:data]==NSNotFound)return;
 [self selectNetworkData:data];const uint8_t *b=(const uint8_t *)data.bytes;
 if(b[1]){NSString *ssid=[self ssidForRow:b];NSString *saved=ssid?[self savedPasswordForSSID:ssid]:nil;_password.stringValue=saved?:@"";
  if(saved.length){_password.stringValue=saved;[self connect:nil];}
  else{[self showWindow:nil];[_window makeFirstResponder:_password];_statusHeading.stringValue=@"Password needed";_status.stringValue=@"Enter the password for this WPA2 network, then click Connect.";}
 }else [self connect:nil];
}
-(void)showWindow:(id)sender {
 (void)sender;[_window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
}
-(void)copySelectedNetworkDetails:(id)sender {
 (void)sender;NSData *data=_networks.selectedItem.representedObject;
 if(![data isKindOfClass:NSData.class]||data.length<44||[_rows indexOfObjectIdenticalTo:data]==NSNotFound)return;
 const uint8_t *b=static_cast<const uint8_t *>(data.bytes);uint16_t rsnLength=AthSecurity::le16(b+42);
 if(rsnLength>257||data.length<44+rsnLength)return;
 NSMutableString *rsn=[NSMutableString stringWithCapacity:rsnLength*2];
 for(NSUInteger i=0;i<rsnLength;++i)[rsn appendFormat:@"%02x",b[44+i]];
 NSString *ssid=[self ssidForRow:b]?:@"Hidden or binary SSID";
 NSString *bssid=[NSString stringWithFormat:@"%02x:%02x:%02x:%02x:%02x:%02x",(unsigned)b[2],(unsigned)b[3],(unsigned)b[4],(unsigned)b[5],(unsigned)b[6],(unsigned)b[7]];
 NSString *security=b[1]?(b[41]?[NSString stringWithFormat:@"driver accepts %@",[self securityForRow:b length:data.length]]:@"driver rejects this protected network"): @"open network";
 NSString *details=[NSString stringWithFormat:@"Ath10k selected-network security details\nSSID: %@\nBSSID: %@\nChannel: %u\nPrivacy: %@\nDriver classification: %@\nRSN IE length: %u\nRSN IE (hex): %@\n\nThis report contains no Wi-Fi password.",ssid,bssid,(unsigned)b[40],b[1]?@"protected":@"open",security,(unsigned)rsnLength,rsnLength?rsn:@"(none)"];
 NSPasteboard *pasteboard=NSPasteboard.generalPasteboard;[pasteboard clearContents];[pasteboard setString:details forType:NSPasteboardTypeString];
 _statusHeading.stringValue=@"Network details copied";_status.stringValue=@"Paste the copied report here so I can identify which advertised security field is rejected. You may redact the BSSID.";
 [self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];
}
-(void)rememberChanged:(id)sender {
 (void)sender;NSUserDefaults *defaults=NSUserDefaults.standardUserDefaults;
 BOOL enabled=_remember.state==NSControlStateValueOn;[defaults setBool:enabled forKey:@"Ath10kRememberAndReconnect"];
 if(!enabled){for(NSString *ssid in [self savedNetworks])[self deleteSavedPasswordForSSID:ssid];[defaults removeObjectForKey:@"Ath10kSavedNetworks"];[defaults removeObjectForKey:@"Ath10kLastSSID"];
  _statusHeading.stringValue=@"Automatic reconnect off";_status.stringValue=@"Saved networks and their Keychain passwords were removed.";
 }else{_statusHeading.stringValue=@"Ready to remember this network";_status.stringValue=@"After a successful connection, its password will be saved in your macOS Keychain for automatic reconnect.";}
 [self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];
}
-(void)saveSuccessfulConnection {
 if(_didSaveCredentials)return;_didSaveCredentials=YES;
 if(_remember.state!=NSControlStateValueOn||!_pendingSSID.length){_statusHeading.stringValue=@"Connected";_status.stringValue=@"The link is up. macOS is requesting network settings; keep this app open while you use the connection.";_passwordForSave=nil;_pendingSSID=nil;[self updateMenuProgress:_status.stringValue];return;}
 if(_secure&&!_passwordForSave.length){_statusHeading.stringValue=@"Connected";_status.stringValue=@"Connected, but automatic reconnect could not save a password. You can enter it next time.";_passwordForSave=nil;_pendingSSID=nil;[self updateMenuProgress:_status.stringValue];return;}
 if(_secure&&![self savePassword:_passwordForSave forSSID:_pendingSSID]){_statusHeading.stringValue=@"Connected";_status.stringValue=@"Connected, but Keychain could not save the password. You can enter it next time.";_passwordForSave=nil;_pendingSSID=nil;[self updateMenuProgress:_status.stringValue];return;}
 [self prioritizeSSID:_pendingSSID];_statusHeading.stringValue=@"Connected";_status.stringValue=@"Connected. This network is first in your saved-network reconnect order.";
 [self rebuildStatusMenu];
 _passwordForSave=nil;_pendingSSID=nil;
}
-(void)applicationDidFinishLaunching:(NSNotification *)note {
 (void)note;_rows=[NSMutableArray array];
 _window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,760,480) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
 [_window setReleasedWhenClosed:NO];
 _window.title=@"Ath10k Connectivity 0.1.12 · Security Diagnostics";[_window center];
 NSView *v=_window.contentView;
 NSTextField *title=[NSTextField labelWithString:@"Connect to a network"];title.font=[NSFont boldSystemFontOfSize:25];title.frame=NSMakeRect(30,420,690,34);[v addSubview:title];
 NSTextField *subtitle=[NSTextField labelWithString:@"Experimental support for open networks and WPA2-Personal with AES pairwise; TKIP group keys are supported."];subtitle.textColor=NSColor.secondaryLabelColor;subtitle.frame=NSMakeRect(32,394,690,22);[v addSubview:subtitle];

 NSBox *networkCard=[[NSBox alloc] initWithFrame:NSMakeRect(24,174,712,201)];
 networkCard.boxType=NSBoxCustom;networkCard.borderType=NSLineBorder;networkCard.borderWidth=1;networkCard.cornerRadius=10;
 networkCard.borderColor=NSColor.separatorColor;networkCard.fillColor=NSColor.controlBackgroundColor;[v addSubview:networkCard];
 NSTextField *networkHeading=[NSTextField labelWithString:@"Network"];networkHeading.font=[NSFont boldSystemFontOfSize:15];networkHeading.frame=NSMakeRect(44,340,650,24);[v addSubview:networkHeading];
 NSTextField *networkHint=[NSTextField labelWithString:@"Choose a nearby network, then enter its password if required."];networkHint.textColor=NSColor.secondaryLabelColor;networkHint.frame=NSMakeRect(44,319,650,18);[v addSubview:networkHint];
 _networks=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(44,277,530,32) pullsDown:NO];[_networks addItemWithTitle:@"Choose a network…"];_networks.target=self;_networks.action=@selector(networkSelectionChanged:);_networks.toolTip=@"Networks show their name, security type and channel.";[v addSubview:_networks];
 _refresh=[NSButton buttonWithTitle:@"Scan" target:self action:@selector(refresh:)];_refresh.frame=NSMakeRect(588,277,126,32);_refresh.toolTip=@"Scan again for nearby networks.";[v addSubview:_refresh];
 NSTextField *passwordLabel=[NSTextField labelWithString:@"Password"];passwordLabel.frame=NSMakeRect(44,239,160,20);[v addSubview:passwordLabel];
 _password=[[NSSecureTextField alloc] initWithFrame:NSMakeRect(44,205,530,30)];_password.placeholderString=@"WPA2 password (8–63 bytes)";[v addSubview:_password];
 _connect=[NSButton buttonWithTitle:@"Connect" target:self action:@selector(connect:)];_connect.frame=NSMakeRect(588,204,126,32);[v addSubview:_connect];
 _remember=[NSButton checkboxWithTitle:@"Remember password in Keychain and reconnect automatically" target:self action:@selector(rememberChanged:)];_remember.frame=NSMakeRect(44,177,520,20);
 _remember.state=[NSUserDefaults.standardUserDefaults boolForKey:@"Ath10kRememberAndReconnect"]?NSControlStateValueOn:NSControlStateValueOff;[v addSubview:_remember];

 NSBox *statusCard=[[NSBox alloc] initWithFrame:NSMakeRect(24,82,712,94)];
 statusCard.boxType=NSBoxCustom;statusCard.borderType=NSLineBorder;statusCard.borderWidth=1;statusCard.cornerRadius=10;
 statusCard.borderColor=NSColor.separatorColor;statusCard.fillColor=NSColor.controlBackgroundColor;[v addSubview:statusCard];
 _statusHeading=[NSTextField labelWithString:@"Status"];_statusHeading.font=[NSFont boldSystemFontOfSize:14];_statusHeading.frame=NSMakeRect(44,142,500,22);[v addSubview:_statusHeading];
 _status=[NSTextField wrappingLabelWithString:@"Looking for the Ath10kMac driver…"];_status.textColor=NSColor.secondaryLabelColor;_status.frame=NSMakeRect(44,103,525,34);[v addSubview:_status];
 _copyNetworkDetails=[NSButton buttonWithTitle:@"Copy network details" target:self action:@selector(copySelectedNetworkDetails:)];_copyNetworkDetails.frame=NSMakeRect(574,101,160,30);_copyNetworkDetails.enabled=NO;_copyNetworkDetails.toolTip=@"Copy the selected access point's advertised RSN information. This does not include its password.";[v addSubview:_copyNetworkDetails];
 _disconnect=[NSButton buttonWithTitle:@"Disconnect" target:self action:@selector(disconnect:)];_disconnect.frame=NSMakeRect(588,138,126,30);_disconnect.enabled=NO;[v addSubview:_disconnect];
 NSTextField *foot=[NSTextField wrappingLabelWithString:@"Keep this app open while connected. Supports WPA2-PSK with AES/CCMP pairwise and AES or TKIP group keys, including group and pairwise key renewal. WPA3, required PMF, TKIP pairwise, and enterprise networks are not supported."];foot.font=[NSFont systemFontOfSize:11];foot.textColor=NSColor.tertiaryLabelColor;foot.frame=NSMakeRect(30,25,700,42);[v addSubview:foot];
 _statusItem=[[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
 NSImage *wifiIcon=[NSImage imageWithSystemSymbolName:@"wifi" accessibilityDescription:@"Ath10k Wi-Fi"];
 if(wifiIcon){[wifiIcon setTemplate:YES];_statusItem.button.image=wifiIcon;}else _statusItem.button.title=@"Wi-Fi";
 _statusItem.button.toolTip=@"Ath10k Wi-Fi";[self rebuildStatusMenu];
 [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self selector:@selector(workspaceDidWake:) name:NSWorkspaceDidWakeNotification object:nil];
 kern_return_t r=[self openDriver];
 if(r==kIOReturnNotFound){_statusHeading.stringValue=@"Driver not found";_status.stringValue=@"Ath10kMac was not found. Check that the driver is loaded, then reopen this app.";_connect.enabled=NO;_refresh.enabled=NO;[self updateMenuProgress:_status.stringValue];return;}
 if(r){_statusHeading.stringValue=@"Driver unavailable";_status.stringValue=[NSString stringWithFormat:@"Could not open the driver (0x%x). Check its version and permissions.",r];_connect.enabled=NO;_refresh.enabled=NO;return;}
 _waitingForDriver=YES;_scanStartedAt=NSProcessInfo.processInfo.systemUptime;_refresh.enabled=NO;_refresh.title=@"Starting…";
 _statusHeading.stringValue=@"Starting Wi-Fi driver";_status.stringValue=@"macOS can finish starting while Ath10kMac initializes the adapter. Wi-Fi will appear when initialization completes.";
 [self rebuildStatusMenu];_timer=[NSTimer scheduledTimerWithTimeInterval:0.5 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
}
-(void)refresh:(id)sender {
 (void)sender;if(!_driver||_session)return;
 if(_scanInProgress)return;
 AthControl::Response st{};if([self call:AthControl::Status request:nullptr response:&st])return;
 if(!(st.flags&2)){_waitingForInitialScan=YES;_scanStartedAt=NSProcessInfo.processInfo.systemUptime;_refresh.enabled=NO;_refresh.title=@"Waiting…";_statusHeading.stringValue=@"Preparing scan";_status.stringValue=@"Waiting for the driver's first scan to finish…";[self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];return;}
 AthControl::Request scan{};
 kern_return_t result=[self call:AthControl::Scan request:&scan response:nullptr];
 if(result){_statusHeading.stringValue=@"Scan could not start";_status.stringValue=[NSString stringWithFormat:@"The driver could not start a scan (0x%x). Wait a moment and try again.",result];[self updateMenuProgress:_status.stringValue];return;}
 _scanInProgress=YES;_scanStartedAt=NSProcessInfo.processInfo.systemUptime;
 [_rows removeAllObjects];[_networks removeAllItems];[_networks addItemWithTitle:@"Choose a network…"];_password.stringValue=@"";_copyNetworkDetails.enabled=NO;
 _refresh.enabled=NO;_refresh.title=@"Scanning…";_statusHeading.stringValue=@"Scanning";_status.stringValue=@"Looking for nearby networks. This can take up to a minute.";
 [self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];
}
-(void)loadNetworks {
 AthControl::Response st{};if([self call:AthControl::Status request:nullptr response:&st])return;
 if(!(st.flags&2)){_status.stringValue=@"The scan did not complete. Try Refresh List again.";return;}
 [_rows removeAllObjects];[_networks removeAllItems];[_networks addItemWithTitle:@"Choose a network…"];_copyNetworkDetails.enabled=NO;
 for(uint32_t i=0;i<st.count;++i){AthControl::Request q{};AthControl::Response r{};q.index=i;
  if([self call:AthControl::BSS request:&q response:&r]||r.length<44)continue;
  NSData *data=[NSData dataWithBytes:r.data length:r.length];[_rows addObject:data];
  NSString *ssid=[[NSString alloc] initWithBytes:r.data+8 length:r.data[0] encoding:NSUTF8StringEncoding];
  if(!ssid.length)ssid=@"Hidden or binary SSID";
  NSString *security=[self securityForRow:r.data length:r.length];
  [_networks addItemWithTitle:[NSString stringWithFormat:@"%@  ·  %@  ·  Ch %u",ssid,security,r.data[40]]];
  _networks.itemArray.lastObject.representedObject=data;
 }
 _refresh.enabled=YES;_refresh.title=@"Scan again";
 _statusHeading.stringValue=st.count?@"Ready to connect":@"No networks found";
 _status.stringValue=st.count?@"Select a network and enter its password if needed. Passwords are stored in Keychain only when automatic reconnect is enabled.":@"The scan finished without finding networks. Try scanning again.";
 [self rebuildStatusMenu];
 if(!_autoConnectAttempted&&_remember.state==NSControlStateValueOn){
  NSArray<NSString *> *profiles=[self savedNetworks];BOOL nearby=NO;
  for(NSString *preferredSSID in profiles){NSInteger index=[self rowIndexForSSID:preferredSSID];if(index==NSNotFound)continue;nearby=YES;
   const uint8_t *b=static_cast<const uint8_t *>(_rows[(NSUInteger)index].bytes);
   if((b[40]>=52&&b[40]<=144)||(b[1]&&!b[41]))continue;
   if(b[1]){NSString *saved=[self savedPasswordForSSID:preferredSSID];if(!saved.length)continue;_password.stringValue=saved;}
   _autoConnectAttempted=YES;[self selectNetworkForSSID:preferredSSID];_statusHeading.stringValue=@"Reconnecting";_status.stringValue=[NSString stringWithFormat:@"Reconnecting to %@, your highest-priority available saved network…",preferredSSID];[self updateMenuProgress:_status.stringValue];[self connect:nil];return;
  }
  if(nearby){_statusHeading.stringValue=@"Saved network needs attention";_status.stringValue=@"A saved network is nearby, but its password is unavailable or its channel/security is unsupported. Choose another network or enter its password.";}
  else if(profiles.count){_statusHeading.stringValue=@"Waiting for saved network";_status.stringValue=@"None of your saved networks is in range. Scan again to retry automatic reconnect.";}
 }
 [self updateMenuProgress:_status.stringValue];
}
-(void)connect:(id)sender {
 (void)sender;if(_session)return;
 NSData *selectedData=_networks.selectedItem.representedObject;
 if(![selectedData isKindOfClass:NSData.class]||[_rows indexOfObjectIdenticalTo:selectedData]==NSNotFound)return;
 const uint8_t *b=(const uint8_t *)selectedData.bytes;
 uint8_t channel=b[40];if(channel>=52 && channel<=144){_statusHeading.stringValue=@"Channel not supported";_status.stringValue=@"This preview cannot connect on DFS channels. Use channels 1–13, 36–48, or 149–165.";return;}
 if(b[1]&&!b[41]){_statusHeading.stringValue=@"Security not supported";_status.stringValue=@"This driver needs WPA2-Personal with AES/CCMP pairwise. TKIP is supported only for the group key; WPA3, required PMF, TKIP-only pairwise, and enterprise networks are unsupported.";return;}
 AthControl::Response status{};if([self call:AthControl::Status request:nullptr response:&status])return;
 if(!(status.flags&1)){[self fail:[NSString stringWithFormat:@"The driver stopped: %s. Reload it or restart before connecting.",status.error]];return;}
 if(!(status.flags&2)){_statusHeading.stringValue=@"Still preparing";_status.stringValue=@"The initial scan is not complete yet. Wait, then scan again.";return;}
 _secure=b[1];
 _pendingSSID=[self ssidForRow:b];_passwordForSave=nil;_didSaveCredentials=NO;
 if(_secure){
  NSData *password=[_password.stringValue dataUsingEncoding:NSUTF8StringEncoding];
  if(password.length<8||password.length>63){_status.stringValue=@"WPA2 passwords must be 8 to 63 bytes long.";return;}
  if(_remember.state==NSControlStateValueOn)_passwordForSave=[_password.stringValue copy];
  uint8_t pmk[32]{},nonce[32]{};
  bool ok=CCKeyDerivationPBKDF(kCCPBKDF2,(const char *)password.bytes,password.length,b+8,b[0],kCCPRFHmacAlgSHA1,4096,pmk,32)==kCCSuccess;
  ok=ok && SecRandomCopyBytes(kSecRandomDefault,32,nonce)==errSecSuccess;
  ok=ok && _supplicant.begin(pmk,nonce,status.local,b+2,b+44,AthSecurity::le16(b+42));
  AthSecurity::wipe(pmk,32);AthSecurity::wipe(nonce,32);_password.stringValue=@"";
  if(!ok){_status.stringValue=@"Could not start WPA2 negotiation.";_pendingSSID=nil;_passwordForSave=nil;return;}
 }
 AthControl::Request q{};q.length=8+b[0];memcpy(q.data,b,q.length);
 kern_return_t r=[self call:AthControl::Connect request:&q response:nullptr];
 if(r){
  _supplicant.reset();AthControl::Response detail{};
  kern_return_t query=[self call:AthControl::Status request:nullptr response:&detail];
  if(!query && strcmp(detail.error,"none"))_status.stringValue=[NSString stringWithFormat:@"Could not start: %s (0x%x). Reload or restart if the runtime has stopped.",detail.error,r];
  else if(r==kIOReturnExclusiveAccess)_status.stringValue=@"A session is already active. Disconnect from the app that started it.";
  else if(r==kIOReturnNotReady)_status.stringValue=@"The interface is not ready. Enable its network service in macOS and wait for the scan.";
  else _status.stringValue=[NSString stringWithFormat:@"Could not start (0x%x). See the test-runtime.sh diagnostics.",r];
  _pendingSSID=nil;_passwordForSave=nil;
  return;
 }
 _failed=NO;_keysSubmitted=NO;_m4Sent=NO;_authorized=NO;_pendingLength=0;[self clearRekeyState];
 _session=YES;_connectedSSID=_pendingSSID;[self selectNetworkForSSID:_connectedSSID];_connect.enabled=NO;_networks.enabled=NO;_password.enabled=NO;_remember.enabled=NO;_refresh.enabled=NO;_statusHeading.stringValue=@"Connecting";_status.stringValue=@"Associating with the selected network…";[self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];
}
-(void)disconnect:(id)sender {
 (void)sender;if(!_session || _disconnecting)return;
 kern_return_t r=[self call:AthControl::Disconnect request:nullptr response:nullptr];
 if(r){_status.stringValue=[NSString stringWithFormat:@"Could not start disconnecting (0x%x). Wait for the current connection to finish.",r];return;}
 _disconnecting=YES;_disconnect.enabled=NO;
 _supplicant.reset();AthSecurity::wipe(_pending,sizeof(_pending));[self clearRekeyState];_pendingLength=0;_password.stringValue=@"";
 _statusHeading.stringValue=@"Disconnecting";_status.stringValue=@"Waiting for the device to confirm disconnection…";
 [self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];
}
-(void)tick:(NSTimer *)timer {
 (void)timer;if(_wakeRecoveryPending){[self recoverAfterWake];[self updateStatusIcon];return;}
 if(!_driver||_failed){[self updateStatusIcon];return;}
 AthControl::Response status{};kern_return_t statusResult=[self call:AthControl::Status request:nullptr response:&status];
 if(_waitingForDriver){
  if(statusResult==kIOReturnNotReady){
   if(NSProcessInfo.processInfo.systemUptime-_scanStartedAt>600){_waitingForDriver=NO;_refresh.enabled=YES;_refresh.title=@"Scan";_statusHeading.stringValue=@"Driver startup timed out";_status.stringValue=@"The Wi-Fi driver did not become ready within 10 minutes. Check the boot log before restarting.";[self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];}
   return;
  }
  if(statusResult){[self fail:@"Lost communication with the kext during driver startup."];return;}
  _waitingForDriver=NO;_connect.enabled=YES;_networks.enabled=YES;_password.enabled=YES;_remember.enabled=YES;_refresh.enabled=YES;_statusHeading.stringValue=@"Driver ready";_status.stringValue=@"The adapter is ready. Checking for nearby networks…";[self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];[self refresh:nil];return;
 }
 if(statusResult){[self fail:@"Lost communication with the kext."];return;}
 if(!(status.flags&1u)){
  [self beginRecoveryWithHeading:@"Wi-Fi adapter inactive" message:@"The kext is loaded, but the adapter service is inactive. Attempting a safe recovery…"];
  return;
 }
 if(_scanInProgress){
  if(status.flags&2){_scanInProgress=NO;[self loadNetworks];return;}
  if(NSProcessInfo.processInfo.systemUptime-_scanStartedAt>60){_scanInProgress=NO;_refresh.enabled=YES;_refresh.title=@"Scan again";_statusHeading.stringValue=@"Scan timed out";_status.stringValue=@"No scan result arrived within a minute. Try scanning again.";[self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];}
  return;
 }
 if(_waitingForInitialScan){
  if(status.flags&2){_waitingForInitialScan=NO;[self loadNetworks];return;}
  if(NSProcessInfo.processInfo.systemUptime-_scanStartedAt>60){_waitingForInitialScan=NO;_refresh.enabled=YES;_refresh.title=@"Scan again";_statusHeading.stringValue=@"Scan still preparing";_status.stringValue=@"The driver's first scan is taking longer than expected. Try scanning again shortly.";[self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];}
  return;
 }
 if(_disconnecting){
  if(status.flags&64)return;
  if(status.stage!=0){[self fail:@"Disconnect ended in an unexpected state. Check the diagnostics."];return;}
  _session=NO;_disconnecting=NO;_secure=NO;_keysSubmitted=NO;_m4Sent=NO;_authorized=NO;
  _connect.enabled=YES;_networks.enabled=YES;_password.enabled=YES;_remember.enabled=YES;_refresh.enabled=YES;_pendingSSID=nil;_passwordForSave=nil;_connectedSSID=nil;_menuShowsConnected=NO;
  _statusHeading.stringValue=@"Disconnected";_status.stringValue=@"Select a network and connect again. You do not need to reload the driver.";
  [self updateMenuProgress:_status.stringValue];[self rebuildStatusMenu];
  return;
 }
 if(!_session){[self updateMenuProgress:_status.stringValue];return;}
 if(status.stage==0&&!(status.flags&64)){
  _session=NO;_secure=NO;_keysSubmitted=NO;_m4Sent=NO;_authorized=NO;_disconnecting=NO;_menuShowsConnected=NO;_connectedSSID=nil;
  NSString *reason=status.error[0]&&strcmp(status.error,"none")?[[NSString alloc] initWithUTF8String:status.error]:@"The driver ended this connection attempt before WPA2 key exchange began. Try the connection once more; if it repeats, collect the Ath10k diagnostics.";
  [self fail:reason];return;
 }
 if(status.stage==11){NSString *reason=status.error[0]&&strcmp(status.error,"none")?[[NSString alloc] initWithUTF8String:status.error]:@"The access point rejected or timed out the connection.";[self fail:reason];return;}
 _disconnect.enabled=(status.flags&128) && (status.flags&4);
 if(_secure){
  for(unsigned i=0;i<4&&!_rekeyKeysReady&&!_rekeyResponsePending;++i){AthControl::Response rx{};
   if([self call:AthControl::ReceiveEAPOL request:nullptr response:&rx])break;
   auto result=_supplicant.receive(rx.data,rx.length);
   if(result==AthSecurity::Supplicant<MacCrypto>::Fatal){[self fail:[NSString stringWithUTF8String:_supplicant.error]];return;}
   if(result==AthSecurity::Supplicant<MacCrypto>::Keys){
    AthControl::Request keys{};keys.length=AthSecurity::Supplicant<MacCrypto>::keyPayloadLength;memcpy(keys.data,_supplicant.keys,keys.length);
    kern_return_t r=[self call:AthControl::InstallKeys request:&keys response:nullptr];
    AthSecurity::wipe(keys.data,sizeof(keys.data));AthSecurity::wipe(_supplicant.keys,sizeof(_supplicant.keys));
    if(r){[self fail:@"The kext rejected key installation."];return;}_keysSubmitted=YES;
   }
   if(result==AthSecurity::Supplicant<MacCrypto>::GroupKeys||result==AthSecurity::Supplicant<MacCrypto>::PairKeys){
    memcpy(_pendingRekeyKeys,_supplicant.keys,sizeof(_pendingRekeyKeys));
    _pendingRekeyOperation=result==AthSecurity::Supplicant<MacCrypto>::GroupKeys?AthControl::RekeyGroup:AthControl::RekeyPair;
    _rekeyKeysReady=YES;_rekeyResponsePending=YES;AthSecurity::wipe(_supplicant.keys,sizeof(_supplicant.keys));
   }
   if(result==AthSecurity::Supplicant<MacCrypto>::Keys||result==AthSecurity::Supplicant<MacCrypto>::Reply||
      result==AthSecurity::Supplicant<MacCrypto>::GroupKeys||result==AthSecurity::Supplicant<MacCrypto>::PairKeys){
    memcpy(_pending,_supplicant.output,_supplicant.outputLength);_pendingLength=_supplicant.outputLength;
   }
   if(_rekeyKeysReady)break;
  }
  if(_rekeyKeysReady){
   AthControl::Request rekey{};rekey.length=sizeof(_pendingRekeyKeys);memcpy(rekey.data,_pendingRekeyKeys,sizeof(_pendingRekeyKeys));
   kern_return_t r=[self call:_pendingRekeyOperation request:&rekey response:nullptr];
   AthSecurity::wipe(rekey.data,sizeof(rekey.data));
   if(!r){AthSecurity::wipe(_pendingRekeyKeys,sizeof(_pendingRekeyKeys));_pendingRekeyOperation=0;_rekeyKeysReady=NO;}
   else if(r!=kIOReturnBusy&&r!=kIOReturnNotReady){[self fail:@"The kext rejected the WPA2 key renewal."];return;}
  }
  BOOL maySend=_rekeyResponsePending?status.phase>=5:(!_keysSubmitted||status.phase>=3);
  if(_pendingLength&&!_rekeyKeysReady&&maySend){
   AthControl::Request tx{};tx.length=(uint32_t)_pendingLength;memcpy(tx.data,_pending,_pendingLength);
   kern_return_t r=[self call:AthControl::SendEAPOL request:&tx response:nullptr];
   if(!r){_pendingLength=0;if(_rekeyResponsePending)_rekeyResponsePending=NO;else if(_keysSubmitted)_m4Sent=YES;}
   else if(r!=kIOReturnBusy && r!=kIOReturnNotReady){[self fail:@"Failed to send the EAPOL response."];return;}
  }
  if(_m4Sent&&!_authorized&&!(status.flags&8)&&status.phase==3){
   kern_return_t r=[self call:AthControl::Authorize request:nullptr response:nullptr];if(!r)_authorized=YES;
  }
 }
 BOOL nowConnected=(status.flags&4)!=0;if(nowConnected!=_menuShowsConnected){_menuShowsConnected=nowConnected;[self rebuildStatusMenu];}
 if(status.flags&4){[self saveSuccessfulConnection];if(!_didSaveCredentials){_statusHeading.stringValue=@"Connected";_status.stringValue=@"The link is up. macOS is requesting network settings; keep this app open while you use the connection.";}}
 else {_statusHeading.stringValue=status.flags&32?@"Waiting for router":@"Connection progress";_status.stringValue=[self connectionProgressForStatus:status];}
 [self updateMenuProgress:_status.stringValue];
}
-(BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender{(void)sender;return NO;}
-(void)applicationWillTerminate:(NSNotification *)note{(void)note;[[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:self];[_timer invalidate];_supplicant.reset();AthSecurity::wipe(_pending,sizeof(_pending));[self clearRekeyState];if(_driver)IOServiceClose(_driver);if(_statusItem)[[NSStatusBar systemStatusBar] removeStatusItem:_statusItem];}
@end
int main(int argc,const char **argv){(void)argc;(void)argv;@autoreleasepool{[NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];Controller *c=[Controller new];NSApp.delegate=c;[NSApp run];}return 0;}
