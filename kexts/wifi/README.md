# QCA6174 Wi-Fi for macOS 26 on the Helios 300 (Ath10kMac)

`Ath10kMac.kext` drives the **Qualcomm Atheros QCA6174** in the Acer Predator Helios 300
G3-571: PCI `168c:003e`, rev `0x32` (hw3.2), Lite-On subsystem `11ad:0807`. The
**Ath10k Connectivity** menu-bar app scans, joins networks and runs the WPA2 handshake.

It is a port of [Ath10kMac](https://github.com/thekunalthakur/Ath10kMac) (GPL-3.0; see
`UPSTREAM.md`) adapted to this laptop. The protocol layers come from Linux ath10k:
PCI/CE, BMI, HTC, WMI-TLV, HTT.

> **Status: built and unit-tested on Linux, cross-checked against Linux ath10k on this
> laptop, never loaded on macOS.** Nothing here can boot macOS, so everything under
> "Only macOS can verify" below is unverified.

## What macOS sees

The kext is an **Ethernet-style interface** (`IOEthernetController`, shown as "Ath10k WLAN
Adapter"). macOS networking treats it like a wired port, so DHCP, DNS, IPv6 and VPNs all
work as usual. The **system Wi-Fi menu, Wi-Fi Settings, AirDrop, Continuity and Location
via Wi-Fi do not**: they need Apple's private IO80211/Skywalk stack. Status, feasibility
and plan for that are in [`docs/WIFI-NATIVE.md`](../../docs/WIFI-NATIVE.md).

## Feature status

| Feature | State | Where it was checked |
|---|---|---|
| Match exactly this card (`168c:003e` + subsystem `11ad:0807`) | done | Info.plist (`IOPCIPrimaryMatch` + `IOPCISecondaryMatch`), `probe()` refuses other subsystems; `tests/test_info_plist.py` |
| Board calibration = what Linux loads on this laptop | done | board-2.bin entry `…11ad,subsystem-device=0807`, crc32 `58f39f4d` = Linux debugfs `fw_checksums` (`evidence/`); `tests/test_payload.py`, `test_board_flow.cpp` |
| Firmware WLAN.RM.4.4.1-00309 (same as Linux), embedded | done | crc32 of firmware/athwlan/otp = Linux debugfs |
| Boot: BMI, OTP, board, firmware, HTC, WMI init | code + host tests | sequence and parameters match the Linux trace (`evidence/README.md`) |
| WMI command sizes vs Linux on this AP (VDEV_START, PEER_CREATE, PEER_ASSOC, VDEV_UP, INIT, ECHO, VDEV_CREATE, SET_REGDOMAIN) | equal | `tests/test_helios_trace_parity.cpp` |
| Scan 2.4 + 5 GHz (passive), hidden SSIDs | code + host tests | macOS only |
| Open networks | code + host tests | macOS only |
| WPA2-PSK, CCMP pairwise, CCMP or TKIP group | code + host tests (crypto vectors) | macOS only |
| Owner's AP (RSN CCMP/CCMP, AKM PSK + FT-PSK, no PMF) | accepted: PSK is selected, FT is not used | `supportsWPA2()` logic; macOS only |
| DHCP / IP | macOS networking (Ethernet-style) | macOS only |
| Sleep/wake | association is not kept; after wake the app restarts the adapter and rejoins the preferred saved network | macOS only |
| Station power save | requested by the app on every association (default on, menu "Power Saving") | macOS only |
| Regulatory | EEPROM regdomain `0x6c` (world) as on Linux; country selectable; TX power ceiling 10 dBm (see below) | host tests |
| Link stats | RSSI/SNR, channel, width, NSS, beacon misses in the app; tx/rx counters on the interface | macOS only |
| WPA3-SAE, required PMF (802.11w), TKIP pairwise, WPA1, Enterprise, AP mode | **not supported** | see "Not supported" |

### Not supported, and why

- **WPA3-SAE / PMF.** The firmware advertises MFP and Linux uses it. Here it would need
  three pieces: SAE commit/confirm authentication frames (the kext only builds Open System
  auth, `Ath10kConnection.hpp:772`); the IGTK KDE and BIP-CMAC in the supplicant
  (`src/shared/Ath10kSupplicant.hpp`); and installing the IGTK into the firmware with
  peer PMF flags. That is a large change that can only be debugged on the Mac, so it was
  not started blind. WPA2/WPA3 transition networks still work through WPA2-PSK. WPA3-only
  networks and networks that require PMF are refused with a clear message.
- **TKIP pairwise and WPA1**: the upstream supplicant is CCMP-only for pairwise keys.
- **802.11r FT**: not used. The owner's AP also offers plain PSK, which is selected.

### TX power

Upstream caps the transmit power at **10 dBm** (`Ath10kTxPowerDbm` 1-10). Linux on this
laptop allows 23 dBm (US 5.15-5.25 GHz client limit) and lets the firmware clamp to the
board's calibrated limits. The cap is kept because raising it is an RF change nobody has
measured. It costs range compared with Linux.

## Build (on Linux)

Requirements: clang, llvm-nm, the macOS 26.5 SDK, Apple ld64 (cctools-port), and the xnu-12377
`config/` export lists. The paths are the shared defaults in `tools/cross-build-kext.sh`.

```sh
kexts/wifi/scripts/cross-build.sh          # kext + app → kexts/wifi/build/release/
kexts/wifi/tests/run-tests.sh              # host unit tests (ASan/UBSan), ~10 min
```

`cross-build.sh` runs these steps:

1. Fetch the pinned firmware: linux-firmware tag 20261019 = commit `a2dcb924`, SHA-256 checked.
2. Generate `build/gen/kext/Ath10kPayload.hpp`. It holds QCA6174 firmware, OTP and the Acer
   and Dell board data, and refuses any input whose checksum differs.
3. Compile with `clang -mkernel` against `MacOSX26.5.sdk` `Kernel.framework`.
4. Link `-kext -static` with ld64 and the SDK's `libkmod`/`libkmodc++`, with a generated
   `_kmod_info`.
5. Audit imports: all 512 resolve, to xnu-12377 KPI exports or to IONetworking/IOPCI
   symbols declared in SDK headers.
6. Check for weak or stack-probe imports.
7. Cross-build the app: `-target x86_64-apple-macos13.0`, linked against SDK `.tbd` stubs.

The firmware is not in git. The kext embeds it, and the bundle's `Contents/Resources`
carries the Qualcomm licence and notice. On a Mac the `Makefile` still works
(`make kext`, `make app`), using the same pinned fetch.

## Install (OpenCore, macOS 26)

Back up a working EFI first, and try it from a USB EFI before touching the internal one.

1. Copy `Ath10kMac.kext` to `EFI/OC/Kexts/`.
2. Under `config.plist` → `Kernel` → `Add`, add an entry. Ath10kMac depends on no other
   added kext (no Lilu), so it can go anywhere after the kexts your setup needs; placing
   it last is fine.
   - `Arch` `x86_64`, `BundlePath` `Ath10kMac.kext`, `ExecutablePath` `Contents/MacOS/Ath10kMac`,
     `PlistPath` `Contents/Info.plist`, `Enabled` `true`, `MinKernel` `25.0.0`, `MaxKernel` `25.99.99`.
   - It must **not** be combined with AirportItlwm or any other driver for `168c:003e`.
3. The kext needs no code signature when OpenCore injects it. Do not install it in
   `/Library/Extensions`.
4. App: copy `Ath10k Connectivity.app` to `/Applications`, then on the Mac run
   ```sh
   xattr -dr com.apple.quarantine "/Applications/Ath10k Connectivity.app"
   codesign --force --sign - "/Applications/Ath10k Connectivity.app"
   ```
   Linux cannot sign Mach-O binaries. An ad-hoc signature is enough for a locally run app.
   The app needs no root and installs nothing system-wide. It talks to the kext through
   its user client, which accepts the local console user. Every request has a fixed size
   and a version check, and is re-checked against the opening task (`src/kext/Ath10kUserClient.cpp`).
5. Optional: set the country. India is likely for this laptop, but **choose your actual
   country**. Run this before building, then rebuild:
   `python3 kexts/wifi/scripts/configure-regulatory.py --country IN`.
   The default `00` uses world rules.

### Rollback

Set the kext's `Enabled` to `false` in `config.plist`, or boot the backup EFI, and delete the
app. A USB Wi-Fi adapter or Ethernet (RTL8111) keeps the machine online meanwhile.

## Owner test steps (macOS only)

Record the results. With any report, send the output of `sh kexts/wifi/scripts/test-runtime.sh > ~/Desktop/ath10k-runtime.txt`.
The script is read-only and collects kext properties and network status. Remove SSIDs and MACs before sharing.

1. **Load.** `kextstat | grep ath10k` (or `kmutil showloaded | grep ath10k`), then
   `ioreg -r -c Ath10kPCIDevice -l -w0 | grep -E 'Ath10k(PreparedBoardKey|PreparedBoardSelection|DriverVersion|BoardUploadStatus)'`.
   Expect the board key `…11ad,subsystem-device=0807`, `board-api2-exact`, `0.1.0` and a
   successful board upload.
   `log show --last 5m --predicate 'sender == "Ath10kMac"'` should reach "deferred target
   probe" → HTC ready → WMI ready, with no "profile-mismatch".
2. **Scan.** Launch the app and click Scan. Both 2.4 and 5 GHz networks should be listed.
3. **Join the home AP**, a WPA2 network on 5 GHz channel 44. Then check that
   `ifconfig` shows an `enX` with an IP address from DHCP, and that `ping 1.1.1.1` and
   web browsing work.
4. **Join a 2.4 GHz network** (phone hotspot, WPA2) and an **open** network, if available.
5. **Sleep/wake.** Close the lid for 1 minute, then open it. The app should show
   "Restoring Wi-Fi after wake" and rejoin within about 30 s. Repeat 3 times.
6. **Power save.** While connected, `ioreg -l | grep Ath10kStationPowerSave` should show the
   request applied. Run a 10-minute `ping -i 1` and compare the latency with the
   menu's "Power Saving" off.
7. **Reboot twice** and check that the kext loads both times and the app auto-joins the preferred network.
8. **Stability**: one hour of normal use, then report any panic (Console → panic reports).

## Licences

- Code: GPL-3.0 (`LICENSE`), from Ath10kMac (Jonas Wander, Kunal Thakur).
- Linux-derived protocol facts: ISC (Linux ath10k), cited where used.
- Regulatory data: ISC (`regulatory/`).
- Firmware: Qualcomm redistribution licence (`firmware/LICENSE.QualcommAtheros_ath10k` and
  `notice_ath10k_firmware-6.txt`). It is fetched at build time and shipped only inside the
  built kext, with the licence alongside.

Upstream's original README (Dell/QCA9377 notes, Portuguese section) is kept in `README.upstream.md`.
