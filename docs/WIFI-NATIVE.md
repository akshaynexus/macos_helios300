# Native macOS Wi-Fi for the QCA6174: feasibility and plan

**Goal (owner):** the system Wi-Fi menu and Settings, standard joins with passwords
stored in the Keychain, and Location and other system services seeing Wi-Fi.

**Today:** `kexts/wifi` is an Ethernet-style interface with its own menu-bar app.

**Verdict:** possible on macOS 26 without OCLP root patches, but only by writing a new
IO80211 "front end" against reverse-engineered Apple ABI. Expect weeks of work with the Mac
in the loop. It is fragile across 26.x point releases. It is **not implemented in this
repo yet**, and nothing in it could be verified from Linux.

Sources were read in July-November 2026 state. Paths are relative to each cloned repo:
OpenIntelWireless/itlwm `53c51c2` (2024-06-09), laobamac/itlwm `7204c69` (2026-11-01),
and dortania/OpenCore-Legacy-Patcher `a1c74ce` (2026-11-03).

## How the working Intel driver does it

AirportItlwm, the Intel itlwm front end, changed shape at macOS 14:

| macOS | Apple family it links | Front-end classes | Who dispatches apple80211 commands | WPA handshake |
|---|---|---|---|---|
| ≤ 13 | `IO80211Family` (≤ 11) / `IO80211FamilyLegacy` (12-13) | `AirportItlwm : IO80211Controller`, `IO80211Interface` | driver, `apple80211Request` switch (`AirportItlwm/AirportSTAIOCTL.cpp:16-210`) | Apple's RSN supplicant: EAPOL passed up, keys come back via `setCIPHER_KEY` |
| 14 | `IO80211Family` 1.5.0 + `IOSkywalkFamily` + `corecapture` (`AirportItlwm-Sonoma-Info.plist:56-76`) | `IO80211Controller` + `IO80211InfraProtocol` (Skywalk interface chain) (`AirportItlwmSkywalkInterface.hpp:14`) | Apple's family calls ~150 per-command virtual `getX`/`setX` methods | **driver** (net80211). PSK from `setASSOCIATE` (`AirportItlwmSkywalkInterface.cpp:141-148`) |
| 15.2+ / 26 (laobamac fork, `AIRPORT_WCL`) | same stock families (`AirportItlwm/WCL/Info.plist:56-76`) | adds the `WCL` command path | `setWCL_SCAN_REQ`/`setWCL_ASSOCIATE`, events posted as messages (`AirportItlwm/WCL/WCL.cpp:208-540`) | driver |

Upstream itlwm has no Sequoia or Tahoe target. laobamac/itlwm is the only public native
Tahoe front end; v2.4.0 is still alpha.

- **Claimed working:** scan, join, network switching and private address on 26.x.
- **Not working:** WPA3-only networks, required PMF, AWDL, MLO.
- **ABI breaks between point releases.** Issue #47 on 26.2 fails to load with
  `Failed to patch symbol __ZN17IO80211Controller27_RESERVEDIO80211Controller5Ev`. The
  successful reports are on 26.7.1.

OCLP's wireless root patches (`modern_wireless.py`, `legacy_wireless.py`) restore
`IO80211FamilyLegacy` and 13.x userspace for **Broadcom** cards, and Atheros40 for
legacy wireless. OCLP detects the card by `IOName`, so QCA6174 is not covered. It would
need spoofing plus SIP `0x803`, AMFI off and SecureBootModel disabled, and OCLP 3.0 (the
first with Tahoe support) is still a release candidate. **We should not build on that path.**

## What a native front end for this driver needs

1. **IO80211 class declarations for macOS 26.x.** None exist in the SDK. The only public
   reconstructions are zxystd/laobamac headers (`include/Airport/macOS26/*.h`), and they
   are marked "All rights reserved". The repo itself is GPLv2-only, which cannot be
   combined with this GPL-3.0 tree. So we need **our own declarations**, derived from the
   real binary. dortania/KdkSupportPkg hosts macOS 26 Kernel Debug Kits (latest listed:
   26.6.2, build 25G83). They contain `IO80211Family.kext` with symbols, from which the
   vtables and exports can be dumped. This has to be redone for each 26.x the owner runs.
2. **A front-end class pair**: `AirportAth10k : IO80211Controller` plus a Skywalk
   `IO80211InfraProtocol` interface. It must implement the command set airportd uses:
   SCAN_REQ and SCAN_RESULT, ASSOCIATE and DISASSOCIATE, SSID, BSSID, RSSI, CHANNEL, STATE,
   RATE, OP_MODE and PHY_MODE, POWER, COUNTRY_CODE, CARD_CAPABILITIES, CIPHER_KEY and
   RSN_IE, plus link, scan-done and assoc events.
3. **The WPA2 supplicant in the kernel.** On 14+ the family hands the driver the PSK and
   expects the driver to run the 4-way handshake. Ours runs in the app today
   (`src/shared/Ath10kSupplicant.hpp`, header-only C++). It needs PBKDF2/HMAC-SHA1 and
   AES key unwrap on libkern crypto. That is feasible, and it would also let the
   Ethernet-mode kext rejoin after wake without the app.
4. **Mapping onto our MLME** (`Ath10kConnection.hpp`). Our core already scans, selects a
   BSS, associates and installs keys. The front end would translate airportd requests
   into the existing `Connect`/`Scan`/`InstallKeys` operations instead of the app's.
5. **A Mac-in-the-loop bring-up.** Expect early kernel panics from vtable mismatches.
   That needs a USB EFI and `keepsyms=1 debug=0x100`, with panic logs sent back after
   each iteration.

## Plan

| Step | Work | Can be done from Linux? |
|---|---|---|
| 0 | Fetch the 26.x KDK matching the owner's build, dump the `IO80211Family`/`IOSkywalkFamily` symbols and vtables, and write our own headers plus an import audit list | yes |
| 1 | Move the supplicant into the kernel (both modes benefit) and host-test it against the existing crypto vectors | yes |
| 2 | Front end: controller + infra interface + command handlers over the existing core, as a separate kext target `AirportAth10k`, with the Ethernet+app build kept as the default | compile and audit only |
| 3 | First boots on the Helios 300 from USB EFI: panics, then scan, join and DHCP | **macOS only** |
| 4 | Sleep/wake, roaming, power and Location | **macOS only** |

**Honest timeline:** steps 0-2 are weeks of focused work. Step 3 is open-ended and depends on how
fast the owner can run test builds and send panic logs back. The Ethernet+app driver in
`kexts/wifi` is the usable path until then.
