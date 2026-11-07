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
2. **Polling, not callbacks.** The front end polls the core every 100 ms on the IO80211 work
   queue, under the same gate as the WCL commands, so the session logic is single-threaded
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
5. **Sleep/wake** stays the core's: after wake it reports "restart required"; the session
   posts driver-unavailable, asks for the guarded restart, and the rematched core brings a
   fresh front end which WCL re-enumerates.

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

Only macOS can verify (not done): that WCL accepts the capability bits and event sequence,
DHCP over the Skywalk interface, sleep/wake, Settings details. The reconstructed headers are
now proven against 26.7.2; another point release can still move slots, so rerun the build
with that release's collection before installing it.

## Not implemented

WPA3-SAE/PMF, Enterprise and the Private Wi-Fi Address are in progress behind the build gate
`AIRPORT_ATH10K_WPASEC` (off in releases; paused by the owner): [`WIFI-SECURITY.md`](WIFI-SECURITY.md).

WPA3-SAE / PMF, Enterprise, WEP, Private Wi-Fi Address (random MAC), AWDL/AirDrop/Continuity,
P2P/hotspot, 6 GHz, 160 MHz, active/directed probing, roaming between BSSIDs of one SSID (WCL
asks for a new join; the core disconnects and reconnects).
