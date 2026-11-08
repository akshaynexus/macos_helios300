# Native macOS Wi-Fi for the QCA6174: design, install and verification

**Goal (owner):** the card appears in macOS exactly as a Wi-Fi card: the system Wi-Fi menu
and Settings › Wi-Fi, network selection and joining by macOS, passwords in the Keychain, the
interface shown as Wi-Fi in Network settings.

**State (2026-11-08):** implemented as `AirportAth10k.kext` over the `Ath10kMac.kext` core in
`kexts/wifi/native/`, cross-built for macOS 26 and host-tested. **Never loaded on macOS.**
Install and owner test steps: [`kexts/wifi/README.md`](../kexts/wifi/README.md).

## How it is built

macOS 15.2+/26 moved Wi-Fi control into WCL (the Wi-Fi Core Layer in `IO80211Family`): the
driver subclasses `IO80211Controller` and `IO80211InfraProtocol`, receives scan/join
requests as `setWCL_SCAN_REQ`/`setWCL_ASSOCIATE` and reports back with `postMessage`
events (scan result, association, first beacon, link status, connect complete, deauth,
LQM). The only public driver doing this is laobamac/itlwm (Intel), whose structure and
reconstructed class headers are reused here (used with the author's permission, see
`kexts/wifi/native/NOTICE`).

```
 airportd / WCL (IO80211Family 1.5.0, IOSkywalkFamily, corecapture)
        │ setWCL_* / get*                        ▲ postMessage(ScanResult, AssocComplete, …)
        ▼                                         │
 AirportAth10k.kext      AirportAth10k : IO80211Controller
                         AirportAth10kInterface : IO80211InfraProtocol
                         AirportAth10kEthernetInterface : IOEthernetInterface   (BSD en<n>)
                         AirportBridge::Session  — pure logic, host-tested:
                           WCL request → control ABI, BSS cache → WCL beacons,
                           join state machine, in-kernel WPA2 supplicant (PMK from WCL)
        │ controlRequest() (Status/Info/Scan/BSS/Connect/EAPOL/InstallKeys/Authorize/…)
        │ nativeAttachInterface(), output queue gated on the core's loop, inputPacket()
        ▼
 Ath10kMac.kext          Ath10kPCIDevice (ath10k core): firmware boot, WMI/HTT, MLME, keys, power
        ▼
 QCA6174 (168c:003e)
```

Decisions:

1. **The core is untouched as a protocol engine.** The native personality key
   `Ath10kNativeFrontEnd` only stops it from attaching its own Ethernet interface; it publishes
   `Ath10kNativeReady` when the firmware runtime is up and the front end matches on that.
   The control ABI the menu-bar app used is now called in-kernel (`controlRequest` takes an
   `OSObject` owner), extended with the BSS details WCL needs (capabilities, interval, rates,
   HT/VHT elements) and the country code.
2. **Polling, not callbacks.** The front end polls the core every 100 ms (20 ms while a scan
   or join runs) on the IO80211 work queue, under the same gate as the WCL commands, so the session logic is single-threaded
   and the core gained no new locks. EAPOL frames reach the kernel supplicant the same way the
   app received them.
3. **Supplicant in the kernel.** `src/shared/Ath10kSupplicant.hpp` is unchanged; it gets a
   software crypto policy (`AirportAth10kCrypto.hpp`: SHA-1, HMAC-SHA1, AES-128 decrypt for
   the RFC 3394 unwrap, `read_random` for nonces) because the kernel exports no single-block
   AES outside `com.apple.kpi.unsupported`. WCL hands over the PMK for Personal networks, so
   no PBKDF2 runs in the kernel.
4. **Scan results are rebuilt from the core's BSS cache** (the core keeps parsed fields, not
   raw beacons): SSID, rates, DS, HT/VHT, RSN, power constraint. Transition-mode RSN IEs are
   rewritten to plain WPA2-PSK for WCL, as AirportItlwm does. `BEACON_INFO` polls get a
   synthesized beacon with an advancing timestamp.
5. **Sleep/wake** follows Linux ath10k's power-off suspend/resume (no WoWLAN) and AirportItlwm's
   IO80211 power handling, without terminating anything. Before sleep the core's priority
   notification takes the connection down (disconnect/cancel: vdev down, keys, peer delete,
   vdev stop, barrier) and stops the scan on the normal runtime, bounded to 0.5 s, refusing
   new radio work meanwhile; then it stops HTC/WMI/CE, masks IRQs and resets the target with
   bus mastering off (`endWMIRuntime` → `quiesceTargetAfterBMI`), so nothing can DMA in D3.
   PM takes the controller to state 0: the session posts link-down and driver-unavailable.
   On wake (`kIOMessageSystemHasPoweredOn` with the device on) the probe thread reloads in
   place (`runWakeReinit`): only if every DMA owner the release path trusts is clear and no
   quarantine is set, it re-enables memory decode and bus mastering (the config space
   IOPCIFamily restored has them off), drops the runtime IRQ source, unhooks MMIO, resets
   every runtime service state (connection with keys wiped, scan keeping its generation, BSS
   cache keeping beacon hints, RX setup, echo/vdev/radio/channel-list/scan, WMI events and
   runtime, runtime power, IRQ flags, control owner), frees and reallocates the CE rings, then
   `probeTarget` (cold reset, BMI, board/OTP/firmware, HTC, WMI, regulatory) exactly as at the
   first start; the runtime timer then recreates the vdev (hardware MAC; a Private Wi-Fi
   Address is reapplied at the next join) and scans. Info state bit 8 tells the front end not
   to request a restart meanwhile; when the runtime returns the session posts ResetInterface +
   DriverAvailable and WCL rescans and auto-joins. Any failure ends the partial runtime and
   leaves "restart required" for the guarded restart (terminate + rematch, which still
   depends on the clean teardown below); an unverified reset leaves the DMA quarantine
   (no free, no restart). `ath10k_wake_reinit=0` restores the old restart-on-wake.
6. **The Skywalk registration blocks are the driver's** (`AirportAth10kRegistration.hpp`).
   IOSkywalkFamily 1.0 dereferences `mExpansionData->fRegistrationInfo` and
   `mExpansionData2->fRegistrationInfo` during the BSD attach without a NULL check, allocates
   them itself only on the `registerNetworkInterface` path (which IO80211 drivers never take),
   and typed-frees them in `IOSkywalkNetworkInterface::stop/free` and
   `IOSkywalkEthernetInterface::free`. A block from `IOMalloc` is in another zone, and the
   free panicked at every reboot/shutdown ("not in the expected zone early.kalloc.288, found
   in kalloc.type.var5.384", 2026-11-07; the wake restart hit the same path, which is why the
   screen stayed black). `AirportAth10kInterface::stop()`/`free()` take the blocks back
   (pointer cleared, `IOFree` with the allocating size) before the family runs; both family
   frees skip a NULL pointer. The host suite pins the ownership rules
   (`native/tests/test_native_registration.cpp`).

## OpenCore

Kernel › Add order: `Ath10kMac.kext`, then `AirportAth10k.kext`; `MinKernel` `25.0.0`,
`MaxKernel` `25.99.99`. No Lilu, no AirportItlwm/itlwm/Brcm kexts for this card, no OCLP
root patch. Boot-args: none required; `ath10k_cc=XX`, `ath10k_txpower=N` (see the README),
`keepsyms=1 debug=0x100` for the first boots. AppleVTD: keep the current `DisableIoMapper`
setting; the core supports both.

## Verification

Can be done from Linux (done):

- `kexts/wifi/tests/run-tests.sh`: the core suite plus `native/tests/test_native_crypto.cpp`
  (FIPS-180/RFC 2202/FIPS-197 vectors, the supplicant over the kernel policy) and
  `native/tests/test_native_bridge.cpp` (WCL request mapping, scan-result conversion, the
  full WPA2 join with key install against a simulated core, link loss, leave, scan-first
  join, rejection, timeout, abort, offline/restart, power).
- `kexts/wifi/scripts/cross-build.sh`: both kexts link with ld64 and every import resolves
  to xnu-12377 KPIs or to a class/method declared in the family headers (836 imports for the
  front end). WCL request/event layouts are pinned by `static_assert`s in `AirportWCL.hpp`.

- `tools/kc-audit.py` (run by the cross-build when `HELIOS_REAL_KC` points at the target
  Mac's `BootKernelExtensions.kc`): every import must be exported by a collection kext the
  plist depends on, and every vtable slot the kext inherits must name the symbol the real
  parent vtable holds at that index, which is the rule kxld/OpenCore apply at injection.
  Checked against macOS 26.7.2 (25G332, IO80211Family 1200.13.1, IOSkywalkFamily 1.0,
  corecapture 1.0.4): 835 imports resolve, `AirportAth10k` (463 slots),
  `AirportAth10kInterface` (668) and `AirportAth10kEthernetInterface` (340) match.

First real boot (v0.1.0, 26.7.2): the core loaded, booted the firmware, set the regulatory
domain and scanned; OpenCore refused the front end ("Failed to patch symbol
`IO80211Controller::attachVirtualInterface` for vtable `AirportAth10k`"). Two causes, both
caught by the audit now: laobamac's macOS 26 `IO80211ControllerV2.h` carried two asm-aliased
`_RESERVEDIONetworkController6/7` declarations for a MacKernelSDK that lacked
`allocatePacketNoWait`/`setHardwareAssists`; the 26.5 SDK declares both, so every
IO80211Controller slot was shifted by two and `attachVirtualInterface` landed on the pad
`_RESERVEDIO80211Controller0`. And `IONetworkInterface::setInterfaceSubType` (private SPI
AirportItlwm calls) is not exported by 26.7.2's IONetworkingFamily.

Second and third real boots (26.7.2, front end injected): kernel panic in
`IONetworkController::start`, page fault on `thread->mutex` (CR2 0x658) from
`ml_thread_policy`. `IONetworkController::start` compares `getWorkLoop()` with the
provider's and, when they differ, calls `ml_thread_policy(getWorkLoop()->getThread(), …)`;
`IO80211WorkQueue::getThread()` in 1200.13.1 returns NULL. AirportItlwm never takes that
path because its provider (`IOPCIEDeviceWrapper`) returns the same work queue; our
provider was the core with its own loop. Fixed by `AirportAth10kNub`, a provider nub
between the core and the controller that owns the IO80211 work queue and returns it from
`getWorkLoop()`, exactly the AirportItlwm topology. The real class sizes were read from the
collection's `MetaClass` constructors while chasing this; every subclassed Apple class
matches (`IO80211Controller` 0x128, `IO80211InfraProtocol` 0x130, `IOEthernetInterface` 0x148,
`IONetworkController` 0x110, `IOEthernetController` 0x118) and the build now checks them
(`kKcAuditParentSizes`), against both the Boot and the System collection.

Fourth real boot (26.7.2, nub build): no panic; nub, controller, IO80211 interface and BSD
interface published, configd registered an IEEE80211 interface, `networksetup` listed
"Wi-Fi (en1)", WCL went to driver-available after our `DriverAvailable` message. But
airportd found no Apple80211 interface ("getifaddrs … count[0]") and power stayed
"Off (forced)". Three causes, fixed:
- `ifconfig -v en1` said `type: Ethernet`: the Wi-Fi ifnet sub-family was missing because the
  `IONetworkInterface::setInterfaceSubType(3)` call had been dropped after a mis-mangled
  audit entry (`__ZN19…` for an 18-letter class); it is back with the real symbol.
- No `IFEF_TXSTART` and no Skywalk flowswitch agent (the Realtek `en0` and the owner's real
  Mac have both): the front end used the legacy output queue. It now uses the output pull
  model like AirportItlwm, `configureOutputPullModel` + `outputStart` (private SPI bound by
  mangled name; the SDK shows `outputStart`'s vtable slot as `_RESERVEDIONetworkController2`),
  with the core signalling the interface's output thread when its gated queue drains.
- The IO80211 interface's power state stayed 0 (`setPowerState powerStateOrdinal[0]`): the
  core's "on" power state exported no `kIOPMPowerOn` to children, so neither the nub nor the
  controller could reach state 1. The core now outputs `kIOPMPowerOn`.

Fifth real boot (26.7.2): Wi-Fi power on, scanning and the network list work; joining failed
with `kIOReturnUnsupported` from `APPLE80211_IOC_ASSOCIATE`. WCL's join manager first sends
`APPLE80211_IOC_SET_MAC_ADDRESS` with the network's Private Wi-Fi Address
(`IO80211InfraInterface::setMacAddress` → `setHardwareAddress`), and the front end refused a
changed address, so the join never reached `setWCL_ASSOCIATE`. Private Wi-Fi Address is now a
release feature: the front end changes the BSD address at once and remembers it; the join's
Connect carries it (extended block, flags 16, accepted in every build); the core recreates
vdev 0 with it at the Scan stage (VDEV_DELETE, VDEV_CREATE, ECHO barrier, cancel waits for the
vdev) before VDEV_START, as Linux ath10k does for a MAC change. The remaining non-GOOD IOCs seen
during a join are the family's own (`PLATFORM_CONFIG`, `VIRTUAL_IF_PARENT`), `SSID`/`BSSID`
while unassociated, and WCL configuration sets AirportItlwm also leaves unsupported; `MCS` now
answers.

Sixth real boot (26.7.2, core 4535d42b, front end 2d2a4880): en1 is Wi-Fi, power on, networks
listed, the Private Wi-Fi Address set; every join failed and macOS asked for the password again.
The core associated (WMI commands up to stage 10, connected-open, then torn down by the leave), but
WCL's join manager, right after `JOIN_ASSOC_COMPLETE`, reads `APPLE80211_IOC_WCL_EXTENDED_BSS_INFO`
in `setCurrentBSS`, and the front end answered `kIOReturnNotReady` because it only reported the
BSS once the 4-way handshake had authorized the port: "Fail to get EXTENDED_BSS_INFO" → "leave
network due to <Update Bss fail>" → join failure (statusCode 1, extendedCode 3). AirportItlwm
answers from net80211 RUN (associated). Fixed, with the rest of the join and scan path (hardware
run 7 pending):
- EXTENDED_BSS_INFO, BSS_INFO, BEACON_INFO, CHANNEL, RSSI, RATE, MCS and TXPOWER answer from
  AssocComplete on; the BSD link follows the authorized port (DHCP no longer starts before the
  handshake); ConnectComplete after the keys are installed and the port is open.
- Join: setWCL_ASSOCIATE waits for the core to be idle (disconnects/cancels a previous
  connection, cancels a running scan), scans the candidate's channel by SSID when the core's BSS
  cache lacks the BSSID ("configured-bss-not-found", reported in the response now), retries a
  Connect the core is not ready for instead of failing, and is accepted while a leave completes.
- Scans: never refused as busy. Cached results (heard in the last 30 s) are posted at once,
  results stream while the core scans, overlapping requests merge, the core's own boot scan is
  adopted, a request during a join/leave is answered from the cache after 1.5 s. The core probes
  actively (wildcard + directed SSID, rate IEs as mac80211 builds them, 40 ms dwell) on channels
  the regulatory rules allow it to transmit on, passive 110 ms elsewhere (was 500 ms passive
  everywhere, ~19 s per sweep); world domain lifts NO-IR per channel once a beacon was heard on it
  (Linux cfg80211 beacon hints), which re-sends the firmware channel list once.
- RSSI: a BSS heard on an adjacent channel during the sweep keeps its last on-channel signal
  (cfg80211 `signal_valid`); before, the last (weakest, leaked) reading won, which is how WCL
  picked a -87 dBm 2.4 GHz BSS over the AP's 5 GHz channel 44.
- The front end polls every 20 ms while a scan or join runs (100 ms otherwise) and immediately
  after each WCL command; nothing in the join path waits on a fixed delay.
- `AirportAth10kLastError` could point into a stack response after a rejected Connect (the
  registry mirror then read dead stack); errors are copied now.
- Registry: `AirportAth10kLastScanMs`, `AirportAth10kLastJoinMs`, `AirportAth10kScanCount`,
  `AirportAth10kCachedNetworks`.

Sleep/wake (the build after d39e247): in-place firmware reload on wake, decision 5 above.
`tests/test_wake_reinit.cpp` runs the actual power and reload methods through 10 sleep/wake
cycles with a join before each, against a device model whose DMA engine aborts if any ring
or buffer is freed or reallocated while the target still holds it; it covers a failed
reload (guarded-restart fallback, then a clean reload on the next wake), a teardown that
does not finish before the reset, a double wake, a sleep during a reload and a quarantine.
Registry: `AirportAth10kLastSleep`, `AirportAth10kLastWake`, `AirportAth10kWakeMs`,
`AirportAth10kWakeResult` on the controller; `Ath10kPowerWakeResult`/`WakeDetail`/`WakeMs`/
`WakeReinits`/`WakeReinitFailures` on the core. `-airportath10k_off` now disables the core
too (whole stack), so a sleep problem can be attributed with one boot-arg.

Only macOS can verify (not done): that WCL accepts the capability bits and event sequence,
DHCP over the Skywalk interface, sleep/wake, Settings details. The reconstructed headers are
now proven against 26.7.2; another point release can still move slots, so rerun the build
with that release's collection before installing it.

## Not implemented

WPA3-SAE/PMF, Enterprise and the Private Wi-Fi Address are in progress behind the build gate
`AIRPORT_ATH10K_WPASEC` (off in releases; paused by the owner): [`WIFI-SECURITY.md`](WIFI-SECURITY.md).

WPA3-SAE / PMF, Enterprise, WEP, Private Wi-Fi Address (random MAC), AWDL/AirDrop/Continuity,
P2P/hotspot, 6 GHz, 160 MHz, roaming between BSSIDs of one SSID (WCL
asks for a new join; the core disconnects and reconnects).
