# QCA6174 Wi-Fi for macOS 26 on the Helios 300 (Ath10kMac + AirportAth10k)

Two kexts make the **Qualcomm Atheros QCA6174** in the Acer Predator Helios 300 G3-571
(PCI `168c:003e`, rev `0x32`/hw3.2, Lite-On subsystem `11ad:0807`) a Wi-Fi card for macOS 26:

| Kext | Role |
|---|---|
| `Ath10kMac.kext` | the ath10k core: PCI/CE, BMI, firmware boot, HTC, WMI-TLV, HTT, MLME (scan, join, keys, power, sleep). A port of [Ath10kMac](https://github.com/thekunalthakur/Ath10kMac) (GPL-3.0, `UPSTREAM.md`) with protocol facts from Linux ath10k. |
| `AirportAth10k.kext` (`native/`) | the native front end: an `IO80211Controller` with an IO80211/Skywalk infrastructure interface, so macOS sees a Wi-Fi card. It ports the macOS 26 (WCL) front end of [laobamac/itlwm](https://github.com/laobamac/itlwm) onto the core's control ABI and runs the WPA2 supplicant in the kernel. |

With both loaded the card appears in the **system Wi-Fi menu and Settings › Wi-Fi**, networks
are chosen and joined by macOS, passwords live in the Keychain, and the interface is listed
as Wi-Fi in Network settings.

> **Status: six boots on macOS 26.7.2 (25G332) on the Helios 300.** The card appears as a
> Wi-Fi interface, powers on, scans and lists networks; joins failed through run 6 (fixed in
> d39e247, not yet run on the Mac), and the in-place sleep/wake (a3da2e8) has not run either.
> Run history: [`docs/WIFI-NATIVE.md`](../../docs/WIFI-NATIVE.md#macos-run-history-2672-25g332-opencore);
> sanitized logs: [`evidence/macos/`](evidence/macos/). The IO80211/Skywalk layouts come from
> reconstructed headers checked against the real 26.7.2 collections (kc-audit); another
> point release needs that check again.

## What works (by design) and what does not

| Feature | State | Where it was checked |
|---|---|---|
| Match exactly this card (`168c:003e` + subsystem `11ad:0807`) | done | core Info.plist, `probe()`; `tests/test_info_plist.py` |
| Board calibration and firmware = what Linux loads here (board-2.bin `…11ad,subsystem-device=0807`, WLAN.RM.4.4.1-00309) | done | crc32 = Linux debugfs (`evidence/`); `tests/test_payload.py` |
| Boot: BMI, OTP, board, firmware, HTC, WMI init | code + host tests | matches the Linux trace (`evidence/README.md`) |
| Wi-Fi menu / Settings › Wi-Fi: scan, network list with security and RSSI | code + host tests | macOS only |
| Join open networks and WPA2-Personal (CCMP pairwise, CCMP or TKIP group), WPA2/WPA3 transition networks via WPA2-PSK | code + host tests (handshake vectors, in-kernel supplicant) | macOS only |
| Hidden networks (join by name once the beacon was seen) | code | macOS only |
| Link state, RSSI, channel/width, NSS, TX power, country in the menu and `wdutil`/`airport` style tools | code | macOS only |
| DHCP / IP, VPN, IPv6 | macOS networking over the Wi-Fi interface | macOS only |
| Station power save | requested after each association; Settings "power mode" is mirrored | macOS only |
| Sleep/wake | Linux ath10k's power-off suspend: before sleep the connection and scan are taken down (bounded 0.5 s), the target is reset and DMA-quiesced before D3; on wake the core reloads the firmware in place (PCI D0 + bus mastering, rings rebuilt, BMI/firmware/HTC/WMI, vdev) while the front end and its IO80211 interface stay attached; WCL gets driver-unavailable, then driver-available, rescans and auto-joins. A failed reload falls back to the guarded restart (terminate + rematch); `ath10k_wake_reinit=0` always uses it | host tests (`test_wake_reinit`: 10 cycles, DMA model; `test_power`; bridge); macOS only for the hardware |
| TX power | regulatory limit per channel from wireless-regdb (US 5.15-5.25 GHz: 23 dBm), firmware clamps to the board calibration; cap with `ath10k_txpower=<dBm>` | host tests |
| Regulatory | EEPROM regdomain `0x6c` (world) as on Linux; `ath10k_cc=XX` or `Ath10kCountryCode` picks the country | host tests |
| Private Wi-Fi Address (per-network MAC) | done: WCL sets the address before each join; the BSD interface takes it at once and the core recreates vdev 0 with it (VDEV_DELETE/CREATE + ECHO barrier) before VDEV_START, as Linux ath10k does for a MAC change | host tests (`test_connection`, `test_control`, bridge); macOS only for the join |
| WPA3-SAE only, required PMF (802.11w), WEP, TKIP pairwise, WPA1, Enterprise (802.1X), AP/IBSS, AWDL/AirDrop, 6 GHz, 160 MHz | **not supported** | see below and `docs/WIFI-SECURITY.md` |
| Active and directed scans | probe requests (40 ms dwell) on channels the regulatory rules let the host transmit on (non-DFS, no NO-IR, or a beacon already heard there, like Linux beacon hints), passive 110 ms elsewhere; WCL's channel lists and SSIDs go to the firmware; cached results are posted at once. World domain: ~3.5 s per full sweep (was ~19 s); `ath10k_cc=IN` probes 5 GHz from the first scan | host tests (`test_wmi_scan`, `test_native_bridge`); macOS only for timing |

### Not supported, and why

- **WPA3-SAE / PMF.** SAE commit/confirm authentication frames, the IGTK KDE and BIP in the
  supplicant, and IGTK installation in firmware are all missing; the core builds Open System
  authentication only. Transition networks still join through WPA2-PSK (the front end
  advertises them to WCL as WPA2, like AirportItlwm). SAE-only and PMF-required networks are
  refused when macOS asks to join them.
- **Enterprise.** WCL would hand over a PMK/MSK after EAP; the core has no 802.1X port control.
- **AWDL / AirDrop / Continuity.** Needs a second (P2P) virtual interface and action-frame
  TX; not started.
- **Paused work.** SAE/PMF/802.1X are being implemented behind the build gate
  `AIRPORT_ATH10K_WPASEC` (off in releases): status and resume plan in
  [`docs/WIFI-SECURITY.md`](../../docs/WIFI-SECURITY.md). (The Private Wi-Fi Address is shipped.)
- **Unsupported networks** (WPA3-SAE only, PMF required, WPA1/TKIP-only pairwise, WEP, 802.1X)
  are refused with an 802.11 status in the association result, not as a driver error.

## Layout

```
kexts/wifi/
  src/kext/            the core (Ath10kPCIDevice : IOEthernetController)
  src/shared/          control ABI (Ath10kControl.hpp) and the WPA2 supplicant (header-only)
  native/src/          AirportAth10kNub (provider nub owning the IO80211 work queue, after AirportItlwm's
                       IOPCIEDeviceWrapper), AirportAth10k (IO80211Controller), AirportAth10kInterface (IO80211InfraProtocol),
                       AirportAth10kEthernetInterface (BSD shim), AirportAth10kBridge.hpp (pure logic:
                       WCL request mapping, scan-result conversion, join state machine, supplicant driver),
                       AirportAth10kCrypto.hpp (SHA-1/HMAC/AES for the kernel), AirportWCL.hpp (imported)
  native/include/Airport/  reconstructed IO80211/Skywalk/corecapture headers (imported, see native/NOTICE)
  native/tests/        host tests for the crypto policy and the bridge
  native/scripts/      cross-build-native.sh, native-families.allow (KPI audit)
  app/                 the menu-bar app of the Ethernet-debug variant (developers only)
```

How the two kexts fit: the core publishes `Ath10kNativeReady` once its firmware runtime is up;
`AirportAth10k` matches on that property, attaches its IO80211 interface and BSD interface, and
hands the core an output queue gated on the core's work loop. Received frames go from the
core straight into the BSD interface. Control (scan, join, keys, status) uses the same
fixed-size control ABI the debug app used, called in-kernel; the front end polls it every
100 ms on the IO80211 work queue and turns the core's state into WCL messages (scan results,
association, first beacon, link status, connect complete, deauth, LQM).

## Build (on Linux)

Requirements: clang, llvm-nm, the macOS 26.5 SDK, Apple ld64 (cctools-port) and the
xnu-12377 `config/` export lists at the shared defaults in `tools/cross-build-kext.sh`.

```sh
kexts/wifi/scripts/cross-build.sh          # → kexts/wifi/build/native/{Ath10kMac,AirportAth10k}.kext
kexts/wifi/tests/run-tests.sh              # host unit tests (ASan/UBSan), ~15 min
```

The build fetches the pinned firmware (linux-firmware `a2dcb924`, SHA-256 checked), generates
the embedded payload, compiles with `clang -mkernel` against `MacOSX26.5.sdk`, links with ld64
(`-kext`), and audits every import: the core's 513 against xnu-12377 KPIs and
IONetworking/IOPCI headers; the front end's 836 against xnu, IONetworking and the reconstructed
IO80211Family (218 symbols), IOSkywalkFamily (79) and corecapture (4) declarations plus the
core's own exports (`native/scripts/native-families.allow`). That audit proves the symbols
exist in the headers. With `HELIOS_REAL_KC=/path/to/BootKernelExtensions.kc` (copied from
the target Mac: `/System/Library/KernelCollections/BootKernelExtensions.kc`) the build also
runs `tools/kc-audit.py`, which checks every import against the collection's exports, the
size of every Apple class the kext subclasses against the real `classSize`, and every
inherited vtable slot against the real parent layout, i.e. what OpenCore's vtable patching
will do at boot. Give it both collections, colon-separated
(`BootKernelExtensions.kc:SystemKernelExtensions.kc`), for the exact macOS build you will boot.

`cross-build.sh --ethernet-debug` builds the developer variant (core as an Ethernet-style
interface + the "Ath10k Connectivity" app, `build/release/`). It is a debugging aid for the
core without the IO80211 stack and **not an install option for users**.

## Install (OpenCore, macOS 26)

Back up a working EFI first and try the kexts from a USB EFI before touching the internal one.
Keep Ethernet (RTL8111) or a USB adapter available.

1. Copy `Ath10kMac.kext` and `AirportAth10k.kext` to `EFI/OC/Kexts/`.
2. `config.plist` → `Kernel` → `Add`, **in this order** (the front end links the core by bundle id):
   1. `Ath10kMac.kext` — `BundlePath` `Ath10kMac.kext`, `ExecutablePath` `Contents/MacOS/Ath10kMac`,
      `PlistPath` `Contents/Info.plist`, `Arch` `x86_64`, `MinKernel` `25.0.0`, `MaxKernel` `25.99.99`, `Enabled`.
   2. `AirportAth10k.kext` — `BundlePath` `AirportAth10k.kext`, `ExecutablePath` `Contents/MacOS/AirportAth10k`,
      `PlistPath` `Contents/Info.plist`, same `Arch`/`MinKernel`/`MaxKernel`, `Enabled`.
   Neither depends on Lilu. They must not be combined with AirportItlwm, itlwm or any other
   driver for `168c:003e`. No OCLP wireless root patch is involved.
3. Boot-args: none required. Useful ones:
   - `ath10k_cc=IN` (two-letter wireless-regdb country; default `00` world rules) and
     `ath10k_txpower=<1..30>` (cap in dBm; default: the regdb limit per channel).
   - For the first boots: `keepsyms=1 debug=0x100` so a panic names the function.
   - Safe mode (`-x`, macOS safe boot / OpenCore Shift+Enter): `Ath10kMac`, `AirportAth10k`
     (nub and controller) and `QCARomePatchRAM` each log one line and do not attach, so a
     bad build is always bypassed from the boot menu. No boot-arg of ours is needed for it.
   - Kill switches: `-airportath10k_off` disables the whole Wi-Fi stack (Ath10kMac declines in
     probe: no firmware, no sleep notification, no front end; use it to tell whether a sleep or
     boot problem is Wi-Fi). `-airportath10k_frontoff` keeps the core and skips only AirportAth10k.
     `ath10k_wake_reinit=0`: after wake use the guarded restart instead of the in-place reload.
   - Diagnostics for the front end: `ath10k_native_delay=<seconds>`
     makes AirportAth10k return from start() at once and run its whole bring-up N seconds
     later, so the system reaches login and SSH first; every bring-up stage logs one
     `AirportAth10k: step N …` line (`log stream --predicate 'sender == "AirportAth10k"'`),
     and the last line before a hang names the stage. The same state is mirrored into the
     registry, so no verbose console is needed: on the nub `AirportAth10kLastStep` and
     `AirportAth10kLastMessage`, on the controller `AirportAth10kJoinState`
     (offline/ready/joining/connected/leaving/restarting), `AirportAth10kLastError` and
     `AirportAth10kLastDisconnectReason` (802.11 reason code; 255 = driver-initiated),
     `AirportAth10kLastScanMs` / `AirportAth10kLastJoinMs` (last core scan; last successful join,
     WCL request to authorized port), `AirportAth10kScanCount`, `AirportAth10kCachedNetworks`:
     `ioreg -l -w0 | grep AirportAth10k`. Nothing is logged from the poll (20 ms while a scan or
     join runs, 100 ms otherwise). The core adds `Ath10kWMIScanProbedChannels`,
     `Ath10kWMIScanBeaconHints` and `Ath10kWMIScanRequestChannels`.
   - `pg106_hda=0`-style GPU args are unrelated; the core needs no `-v`.
4. AppleVTD (`Kernel › Quirks › DisableIoMapper`): the core allocates DMA through the
   system mapper when one is active and works without it; leave your existing setting. If
   the log shows `Ath10kMac: DMA mapper` failures, try the other setting once.
5. Kexts injected by OpenCore need no signature. Do not put them in `/Library/Extensions`.
6. Country: `ath10k_cc`, or set `Ath10kCountryCode` in the core's personality
   (`python3 kexts/wifi/scripts/configure-regulatory.py --country IN`, then rebuild).

### Rollback

Set both entries' `Enabled` to `false` (or boot the backup EFI). The card then stays
unclaimed, as before the kexts. There is nothing else to remove.

## Owner test steps (macOS only)

Record the results of each step. With any report, send
`sh kexts/wifi/scripts/test-runtime.sh > ~/Desktop/ath10k-runtime.txt` (read-only: kext
properties and network status; remove SSIDs and MACs before sharing) and
`log show --last 10m --predicate 'sender == "Ath10kMac" OR sender == "AirportAth10k"' > ~/Desktop/wifi-log.txt`.

1. **Load.** `kmutil showloaded | grep -i -e ath10k -e airportath10k` lists both.
   `ioreg -r -c Ath10kPCIDevice -l -w0 | grep -E 'Ath10k(PreparedBoardKey|DriverVersion|BoardUploadStatus|NativeReady)'`
   shows the board key `…11ad,subsystem-device=0807`, `0.1.1`, a successful board upload and
   `Ath10kNativeReady = Yes`. `ioreg -r -c AirportAth10k -l -w0 | head -40` shows the front end
   and `ifconfig` an `en` interface with the card's MAC.
2. **Menu.** The Wi-Fi menu-bar item and Settings › Wi-Fi show the card on; within about 4 s
   the first scan lists 2.4 and 5 GHz networks with lock icons and signal bars (later menus fill
   from the cache at once). `AirportAth10kLastScanMs` on the controller has the last scan's duration.
   `networksetup -listallhardwareports` lists it as "Wi-Fi".
3. **Join the home AP** (WPA2, 5 GHz channel 44) from the menu, entering the password once.
   Expect the handshake to finish within 5 s, then DHCP: `ifconfig en<n>` shows an address,
   `ping 1.1.1.1` and web browsing work. Settings › Wi-Fi › Details shows channel 44, 80 MHz,
   RSSI and the country.
4. **Private Wi-Fi Address.** Leave it on (macOS default). The kernel log shows
   `AirportAth10k: station MAC now …` followed by `Ath10kMac: STATION MAC change begin/complete`
   at the join; `ifconfig en1` shows that address while associated.
5. **Join a 2.4 GHz network** (phone hotspot, WPA2) and an **open** network if available;
   switch between networks from the menu.
6. **Sleep/wake.** Close the lid for 1 minute, open it: within about 10 s the firmware is
   reloaded in place and macOS rejoins the saved network. `ioreg -l -w0 | grep -E
   'AirportAth10k(LastSleep|LastWake|WakeMs|WakeResult)|Ath10kPowerWake'` shows
   `AirportAth10kWakeResult = "reinit"`. Repeat 3 times.
7. **Power save.** While connected, `ioreg -l | grep Ath10kStationPowerSave` shows the request
   applied. A 10-minute `ping -i 1` should show no drops.
8. **Reboot twice** and check that both kexts load and the saved network joins automatically.
9. **Stability**: one hour of normal use, then report any panic (Console › panic reports).

If macOS panics in `IO80211Controller`/`IO80211InfraInterface` methods at load, the class
layout of that 26.x release differs from the reconstructed headers; send the panic log with
the exact build number (`sw_vers`), which is the input for a header fix.

## Licences

- Code: GPL-3.0 (`LICENSE`), from Ath10kMac (Jonas Wander, Kunal Thakur) for the core.
- `native/`: GPL-3.0 for the front end written here; imported Apple-ABI headers and
  `AirportWCL.hpp` from laobamac/itlwm, used with the author's permission (`native/NOTICE`,
  `UPSTREAM.md`).
- Linux-derived protocol facts: ISC (Linux ath10k), cited where used.
- Regulatory data: ISC (`regulatory/`).
- Firmware: Qualcomm redistribution licence (`firmware/LICENSE.QualcommAtheros_ath10k` and
  `notice_ath10k_firmware-6.txt`), fetched at build time and shipped only inside the built
  core kext, with the licence alongside.

Upstream's original README (Dell/QCA9377 notes, Portuguese section) is kept in `README.upstream.md`.
