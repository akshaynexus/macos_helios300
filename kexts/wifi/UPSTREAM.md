# Upstream

`kexts/wifi/` (originally `wifi/`) starts from a snapshot of **Ath10kMac**, GPL-3.0 (see `LICENSE`):

| | |
|---|---|
| Upstream | https://github.com/thekunalthakur/Ath10kMac (fork of https://github.com/jonasw8/Ath10kMac) |
| Commit | `3954e87dbe4b6991ed3cd935818263caff444f35` (2026-10-28, "docs: explain QCA9377 board-profile porting") |
| Authors | Jonas Wander (jonasw8, original driver and app), Kunal Thakur (QCA9377 profile, TKIP group keys, DMA/sleep fixes) |
| Imported | 2026-11-07, `git archive` of that commit |

Not imported:

- `MacKernelSDK/` (submodule with a prebuilt `libkmod.a`). We build against the
  macOS 26.5 SDK's `Kernel.framework` and its `usr/lib/libkmod*.a`.
- `firmware/*.bin`, `firmware/prepared/`, `firmware/QCA9377/hw1.0/*.bin` (Qualcomm
  firmware blobs) and `src/kext/Ath10kPayload.hpp`, a 9 MB header generated from those
  blobs. The build fetches pinned linux-firmware files and checks their SHA-256 instead
  (`scripts/fetch-firmware.sh`).
- `.github/` (macOS GitHub Actions release workflow) and `SOURCE-HASHES-current.json`
  (hashes of the upstream tree, which no longer applies after our changes).

The snapshot is committed unchanged, apart from those exclusions, in the first `wifi:` commit,
so `git log --follow -p -- kexts/wifi/` (or `-- wifi/` before the move) shows every change made here on top of upstream.

Protocol details added or changed here cite the Linux ath10k driver (ISC licence),
`drivers/net/wireless/ath/ath10k/` at torvalds/linux
`d48723711bae9b8cdc56719c448ba42fef28cbee` (2026-11-06).

Regulatory data (`regulatory/`) is upstream's: wireless-regdb (ISC) and Linux `ath/regd*.h`
(ISC), with sources in `regulatory/SOURCE.md`.

## Native front end (`native/`): laobamac/itlwm

`kexts/wifi/native/` (AirportAth10k.kext) ports the macOS 26 (Tahoe, WCL) front end of
**laobamac/itlwm** onto the ath10k core:

| | |
|---|---|
| Upstream | https://github.com/laobamac/itlwm (fork of https://github.com/OpenIntelWireless/itlwm) |
| Commit | `7204c69d1bfa7f5cc0f7864e08fb03e55d0ec29e` (2026-11-01, "Merge pull request #36 … wpa3-transition-and-amsdu") |
| Authors | 钟先耀 / zxystd (AirportItlwm, Skywalk interface, reconstructed Apple headers), laobamac (macOS 15.2/26 WCL path and macOS 26 headers) |
| Imported | 2026-11-08 |

Imported verbatim (original notices kept): `include/Airport/**` → `native/include/Airport/`
(macOS 15 variants dropped), `AirportItlwm/WCL/WCL.hpp` → `native/src/AirportWCL.hpp`.
Rewritten over our core but following upstream's structure: `AirportItlwmV2.cpp`,
`AirportItlwmSkywalkInterface.cpp`, `WCL/WCL.cpp`, `AirportItlwmEthernetInterface.cpp` →
`native/src/AirportAth10k*.cpp`. Not imported: itlwm/itl80211 (Intel HAL, OpenBSD net80211),
`MacKernelSDK`, the AWDL/legacy (≤ macOS 14) front ends.

Licence: those files carry "All rights reserved" inside a GPL-2.0 repository. They are
**used with the author's permission per the repository owner (2026-11-08)**, who
collaborates with laobamac; see `native/NOTICE`. Everything written here is GPL-3.0.


## WPA3 supplicant: hostap (hostapd/wpa_supplicant)

`src/shared/Ath10kSAE.hpp` (SAE, IEEE 802.11-2020 12.4), the AKM/KDF generalisation of
`src/shared/Ath10kSupplicant.hpp` (KDF-SHA256 PTK, AES-128-CMAC key MIC, IGTK KDE, RSNXE)
and the PMF rules in the core follow the structure, constant-time choices and test vectors of
**hostap** (`src/common/sae.c`, `dragonfly.c`, `wpa_common.c`, `src/rsn_supp/wpa.c`,
`wpa_supplicant/sme.c` for SA Query, `src/common/common_module_tests.c` for the Annex J.10
vectors):

| | |
|---|---|
| Upstream | https://w1.fi/hostap.git (Jouni Malinen and contributors) |
| Licence | BSD-3-Clause (hostap `COPYING`/`README`) |
| Used | 2026-11-08, as the reference for a fresh C++ implementation on the kernel-safe
`Ath10kSHA256.hpp`/`Ath10kAES.hpp`/`Ath10kP256.hpp`; no hostap source is copied |

The bignum/ECC code is our own (Montgomery arithmetic, Jacobian P-256, Montgomery ladder) and
is checked against NIST points and an independent affine implementation in
`tests/test_wpa3_crypto.cpp`.
