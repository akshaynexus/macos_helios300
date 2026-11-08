# Wi-Fi security work: WPA3-SAE/PMF, 802.1X Enterprise (Private Wi-Fi Address shipped)

**Update (hardware run 5):** Private Wi-Fi Address left this list. WCL sets it before every
join, so it is required for any join on macOS 26; the station-MAC-only extended Connect
(flags 16) is accepted in every build and the core's vdev recreation at the Scan stage is
implemented (`serviceMacChangeOnly`) and host-tested. The rest below is still gated.

**Status: code complete behind the gate, host-tested, not yet run on the laptop (2026-11-08).**
Everything below is compiled out of the shipped kexts by the build gate
`AIRPORT_ATH10K_WPASEC` (default `0`). With the gate off, both kexts join networks exactly as
before this work started (WPA2-PSK, CCMP pairwise, CCMP/TKIP group; the join path of
`adff19e`/`2f7ff2a`). The gate stays off for releases until the steps in
[On-device test](#on-device-test) pass on the laptop.

## The gate

`AIRPORT_ATH10K_WPASEC` is defined (default 0) in `kexts/wifi/src/shared/Ath10kControl.hpp` and
`Ath10kSecurity.hpp`, so the core, the front end and the tests agree. The build scripts pass it
from the environment:

```sh
kexts/wifi/scripts/cross-build.sh                 # release: WPASEC=0, nothing new compiled in
WPASEC=1 kexts/wifi/scripts/cross-build.sh        # development: new control ops and core paths
WPASEC=1 OUT=/tmp/native-on bash kexts/wifi/native/scripts/cross-build-native.sh
c++ ... -DAIRPORT_ATH10K_WPASEC=1 kexts/wifi/tests/test_control.cpp   # host tests with the gate on
```

What the gate off guarantees (checked by `tests/test_control.cpp`, `test_connection.cpp`,
`test_security.cpp`, `native/tests/test_native_crypto.cpp`, `native/tests/test_native_bridge.cpp`):

- Control ABI: operations above `RadioOptions` (SendAuth/ReceiveAuth/AuthResult) are rejected;
  the extended Connect (index bit 2) returns `kIOReturnUnsupported`; InstallKeys/Rekey accept the
  56-byte payload only; Info stays 116 bytes with capability bit 1024 clear; the BSS record is
  unchanged (no RSNXE extension).
- Core: management frames with the Protected bit are dropped as before (mask `0x470f`);
  `pmf`, `externalAuth`, `enterprise` and `stationRSNLength` stay zero, so BSS selection,
  PEER_ASSOC flags and the association request are the WPA2-PSK ones; the station MAC is the
  hardware MAC unless WCL set a Private Wi-Fi Address (the flags-16 extended Connect, shipped
  in every build, recreates vdev 0 with it).
- Supplicant: `Supplicant::begin()` with six arguments is WPA2-PSK with the fixed station RSN
  (PRF-SHA1, HMAC-SHA1-128, descriptor version 2) and ignores message 1 key data as before. The
  front end sends the first 56 bytes of the key payload. The existing WPA2 vectors
  (`tests/security_vectors.inc`, `rekey_vectors.inc`) pass unchanged.
- Front end: `setHardwareAddress` accepts a changed MAC (Private Wi-Fi Address, shipped since
  hardware run 5: `ifnet_set_lladdr` at once, the core's vdev at the next join); capabilities
  advertise WPA2/CCMP/TKIP only; SAE/Enterprise/PMF-required joins are refused with an 802.11
  status (`planJoin`/`refusalStatus`: 43, 31); scan results show the WPA2 view of a transition
  network; PMK/MSK `setCIPHER_KEY` is refused.

## What is done (all on `main`, gate-independent host tests)

| Piece | Where | Tests |
|---|---|---|
| SHA-256, HMAC-SHA256, 802.11 KDF-SHA256, HKDF | `src/shared/Ath10kSHA256.hpp` | FIPS 180-4, RFC 4231, RFC 5869 (`tests/test_wpa3_crypto.cpp`) |
| AES-128 encrypt/decrypt, AES-CMAC, BIP-CMAC-128 | `src/shared/Ath10kAES.hpp` | FIPS 197 C.1, RFC 4493, BIP framing (wlantest's AAD) |
| Constant-time P-256 (Montgomery, Jacobian, ladder) | `src/shared/Ath10kP256.hpp` | NIST points 1G..4G, (n-1)G, nG, independent Python check |
| SAE group 19: H&P and H2E PWE, commit/confirm, tokens, password identifiers, KCK/PMK/PMKID | `src/shared/Ath10kSAE.hpp` | IEEE 802.11-2020 Annex J.10 vectors (hostap's copy), two-station exchanges |
| Supplicant for AKMs 1/2/5/6/8: KDF-SHA256 PTK, CMAC MIC, versions 0/3, PMKID check, RSNXE, IGTK KDE, 81-byte key payload | `src/shared/Ath10kSupplicant.hpp`, `Ath10kSecurity.hpp` (parseRSN/rsnAcceptable/buildStationRSN) | `tests/test_wpa3_supplicant.cpp` against an OpenSSL authenticator; old WPA2 tests unchanged |
| Core (gated): extended Connect (AKM, PMF, external auth, enterprise, station MAC, station RSN/RSNXE), SendAuth/ReceiveAuth/AuthResult, IGTK install, PMF rules in management RX (protected unicast, BIP-verified broadcast, unprotected deauth → SA Query trigger, robust action drop, SA Query response), PEER_ASSOC PMF flag, BSS RSNXE, frame builders (external auth, SA Query, VDEV delete/create/echo for a MAC change) | `src/kext/Ath10kConnection.hpp`, `Ath10kUserClient.cpp`, `Ath10kBSS.hpp`, `Ath10kPCIDevice.*` | compile with the gate on; `test_control.cpp` runs with the gate on |

References used (cited in the code): Linux ath10k `mac.c` (`ath10k_set_key` returns 1 for
AES_CMAC so BIP is software; `ath10k_peer_assoc_h_crypto` PMF flag with `ATH10K_FW_FEATURE_MFP_SUPPORT`,
which our firmware-6 carries), `wmi.c` (`ath10k_wmi_rx_is_decrypted`: PMF frames arrive decrypted with the
Protected bit and CCMP header kept for action/deauth/disassoc), `htt_tx.c` (MIC room for protected
action frames), `ath10k_add_interface`/`ath10k_remove_interface` (MAC change = vdev delete/create);
mac80211 `rx.c` (`ieee80211_drop_unencrypted_mgmt`); wpa_supplicant `sme.c` (SA Query 201/1000 TU);
hostap `sae.c`/`dragonfly.c`/`wpa_common.c` (BSD, see `kexts/wifi/UPSTREAM.md`); laobamac/itlwm
`AirportItlwmV2.cpp` (MAC change: `ifnet_set_lladdr` + re-init) and `WCL.cpp` (PMK/MSK via
`setCIPHER_KEY` for enterprise).

## What the gate-on build does

Join paths (`native/src/AirportAth10kBridge.hpp`, `planJoin`/`planSecurity`, `AirportWCL.hpp`
`classifyLegacyAuth`):

- **WPA3-SAE** (`APPLE80211_AUTHTYPE_WPA3_SAE`, AKM 8). WCL's join request carries the password
  in the `apple80211_key` at 0x40 (`APPLE80211_CIPHER_PWD`, up to 64 bytes: read from
  `WCLJoinRequest::checkValidationForApple80211Key`, 26.7.2). The core stops at the Auth stage
  (`externalAuth`); the session drives `AthSAE::Session` through `SendAuth`/`ReceiveAuth`:
  Commit, Anti-Clogging Token retry (status 76, raw for H&P, the token container for H2E),
  Confirm, `AuthResult 1`, then the association with the station RSN
  (`buildStationRSN(group, 8, mfpc, mfpr, pmkid)`) and, with H2E (AP RSNXE bit 5), RSNXE
  {244,1,0x20}. Group 19 only; a rejected or different group fails the join. Reason 23 goes
  to WCL so macOS treats it as a credentials problem.
- **Transition networks** (AKM 2 + 8, MFPC set, MFPR clear). Scan results now show the RSN as it
  is plus the RSNXE (no `selectWPA2TransitionMode` rewrite). A join with a password runs SAE, a
  join with a PMK runs PSK (with PMF: the AP is capable).
- **802.1X** (`WPA`/`WPA2`/`SHA256_8021X`/`WPA3_ENTERPRISE`, AKM 1 or 5). The core raises the data
  link at association; EAP frames pass to and from `eapolclient` while the port is closed,
  EAPOL-Key frames stay in the core's queue until a key exists. The PMK/MSK arrives through
  `setCIPHER_KEY` (cipher 6/7/9, 32 bytes, or a 64-byte MSK whose first 32 bytes are the PMK);
  the session then starts the 4-way handshake. The join completes when the port opens.
- **PMF** (`airportath10k_pmf`): 1 (default) capable when the AP is, required when it requires
  it, SAE needs an MFPC AP; 2 required on every MFPC AP; 0 off (SAE and PMF-required networks
  refused with status 31). BIP-CMAC-128 in software like Linux ath10k. The 81-byte key payload
  carries the IGTK.
- **PMKSA cache** (8 entries, 12 h, `setCLEAR_PMKSA_CACHE`): SAE and 802.1X joins that have a live
  entry for BSSID+AKM associate with Open System authentication and the PMKID; if the AP refuses
  (status 53 or any failure before association) the entry is dropped and full authentication
  runs once.
- `getCARD_CAPABILITIES` adds byte 6 bit 0 (MFP capable); `EXTENDED_BSS_INFO` reports the station
  RSN the join built. `getWCL_EXTENDED_BSS_INFO` keeps the fixed WPA2 RSN when the join used none.

Core (`src/kext/Ath10kConnection.hpp`, `Ath10kNetwork.cpp`, `Ath10kUserClient.cpp`): the
external-auth stage, the PMF TX slot with SA Query timing (retry 201 TU, fail after 1000 TU,
power save leaves first), protected management TX (host-built CCMP header and PN, firmware
encrypts: HTT flags 0x61, `IEEE80211_KEY_FLAG_GENERATE_IV_MGMT` in Linux), PMKSA joins
(SAE AKM with a PMKID in the RSN), and the 802.1X data path (`releaseRXBatch`, `outputPacket`;
ordinary packets are dropped while the port is closed so they cannot stall EAP behind them).

Host tests: `tests/test_wpasec.cpp` (production `controlAction`, `serviceConnection`,
`receiveStationManagement`, `buildHTTPacket` and the real front-end session against a simulated
AP and firmware), `tests/test_network_rx.cpp` and `tests/test_native_tx.cpp` with the gate on
(production `releaseRXBatch`/`outputPacket` for 802.1X), `native/tests/test_native_bridge.cpp`
with the gate on (run by `tests/run-tests.sh`). Coverage: SAE H&P and H2E with Anti-Clogging
Tokens, group 19 rejection and mismatch, wrong password, the 4-way handshake with the IGTK
against an OpenSSL authenticator, PMF deauth with SA Query both ways, BIP-CMAC with replay,
unprotected robust frame drop, SA Query timeout, EAPOL pass-through with a 64-byte MSK handover,
PMKSA caching with the status 53 fallback, transition networks.

## On-device test

Build the gate-on pair and boot it through the usual run procedure (`docs/WIFI-NATIVE.md`):

```sh
cd kexts/wifi
WPASEC=1 OUT=$HOME/pascal-build/wpasec-work/k1 \
  HELIOS_REAL_KC=$HOME/pascal-build/macos-26.7.2-25G332/BootKernelExtensions.kc:$HOME/pascal-build/macos-26.7.2-25G332/SystemKernelExtensions.kc \
  bash scripts/cross-build.sh
```

After each join, on the Mac:

```sh
ioreg -rn AirportAth10k | grep -E 'AirportAth10k(JoinState|LastError|LastJoinMs|LastDisconnectReason|PMKSACount)'
sudo log show --last 3m --predicate 'eventMessage CONTAINS "WCLJoinRequest" OR eventMessage CONTAINS "AirportAth10k" OR eventMessage CONTAINS "Ath10kMac:"' | tail -60
wdutil info | sed -n '/WIFI/,/NETWORK/p'
```

The first line `WCLJoinRequest: lowerAuth = ..., upperAuth = ..., key = ...` in the log is the first
thing to read: it says which AKM and key type airportd really chose (see "Unverified on the Mac").

1. **WPA3-only network** (WPA3-Personal, PMF required; an AP or `hostapd` with `sae_pwe=2` for
   H2E if available). Expect `AirportAth10kJoinState` connected, `AirportAth10kLastError` none,
   Security "WPA3 Personal" in System Settings and `wdutil info`, working DHCP/ping (needs the
   pending TX fix). `AirportAth10kPMKSACount` is 1. Disable and re-enable Wi-Fi: the rejoin uses
   PMKSA caching (no SAE frames in a capture), still connected. Wrong password: Settings asks
   for the password again, `LastError` names `sae-no-response` (or `sae-confirm-rejected`).
2. **Transition network** (WPA2/WPA3): joined with WPA3 and the menu shows WPA3 unless macOS
   chooses otherwise; with the join key type being a PMK (see the first log line) it joins as
   WPA2-PSK with PMF negotiated. Both must end up connected.
3. **Enterprise**: a WPA2-Enterprise network with PEAP-MSCHAPv2 (or EAP-TLS with a profile). The
   prompt appears, EAP completes, the link comes up and DHCP runs. `AirportAth10kPMKSACount`
   is 1. Toggle Wi-Fi: reconnects without another credentials prompt (PMKSA) or with a fresh
   EAP exchange if the AP refuses the cached key.
4. **PMF** on the WPA3 network: `sudo tcpdump -I -i en1 'wlan type mgt'` (Monitor mode to be
   tried from a second device when possible). The association shows MFPC/MFPR in the RSN; a
   deauth from a sniffer or a spoofed frame must not drop the link when protected (a reason 6
   or 7 triggers SA Query instead).

Gate-off regression: run the release pair as before on a WPA2-PSK network; the join and TX
checks of hardware run 8 must not change.

### Unverified on the Mac

- airportd's actual choice for SAE: `upperAuth` with bit 12 and cipher `APPLE80211_CIPHER_PWD` is
  what Apple's Broadcom join adaptor accepts ("Failed to program the password"), but a host-side
  supplicant may have to be enabled by a capability bit no one has found yet. If airportd
  refuses or sends a PMK only, read `WCLJoinRequest` and `getCARD_CAPABILITIES` (KC) next.
- Capability byte 6 bit 0 (MFP) is what laobamac's list names; Apple's own flags gate WPA3 per
  feature (SAE-PK, OCV). If airportd rejects WPA3 networks, these are the first suspect.
- WCL's handling of the LinkStatus/ConnectComplete timing for Enterprise (the data link is
  up at association, ConnectComplete at the end of the 4-way handshake).
- Firmware: Linux sends `pdev_param pmf_qos=1` at start (`ath10k_start`); the core does not,
  because no sequence point exists for it. If protected management TX does not get a response
  from the AP, add it first.
- SAE crypto runs on the IO80211 work queue during the join (milliseconds on the laptop; watch
  for a watchdog complaint in the log).

## Files

```
kexts/wifi/src/shared/Ath10kSHA256.hpp  Ath10kAES.hpp  Ath10kP256.hpp  Ath10kSAE.hpp
kexts/wifi/src/shared/Ath10kSecurity.hpp  Ath10kSupplicant.hpp  Ath10kControl.hpp
kexts/wifi/src/kext/Ath10kConnection.hpp  Ath10kNetwork.cpp  Ath10kUserClient.cpp  Ath10kBSS.hpp  Ath10kPCIDevice.{hpp,cpp}
kexts/wifi/native/src/AirportAth10kBridge.hpp  AirportWCL.hpp  AirportAth10kInterface.{hpp,cpp}  AirportAth10k.cpp
kexts/wifi/tests/test_wpasec.cpp  test_wpa3_crypto.cpp  test_wpa3_supplicant.cpp  test_network_rx.cpp  test_native_tx.cpp
kexts/wifi/native/tests/test_native_bridge.cpp
```
