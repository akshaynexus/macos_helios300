# Wi-Fi security work: Private Wi-Fi Address, WPA3-SAE/PMF, 802.1X Enterprise

**Status: PAUSED by the owner (2026-11-08), not in the first release.** Everything below is
compiled out of the shipped kexts by the build gate `AIRPORT_ATH10K_WPASEC` (default `0`).
With the gate off, both kexts join networks exactly as before this work started (WPA2-PSK,
CCMP pairwise, CCMP/TKIP group; the join path of `adff19e`/`2f7ff2a`). The host tests for the
new code build and run regardless of the gate; they do not ship.

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
  hardware MAC (`_vdevMAC == _wmiInit.mac`).
- Supplicant: `Supplicant::begin()` with six arguments is WPA2-PSK with the fixed station RSN
  (PRF-SHA1, HMAC-SHA1-128, descriptor version 2) and ignores message 1 key data as before. The
  front end sends the first 56 bytes of the key payload. The existing WPA2 vectors
  (`tests/security_vectors.inc`, `rekey_vectors.inc`) pass unchanged.
- Front end: `setHardwareAddress` still refuses a different MAC, capabilities advertise WPA2/CCMP/TKIP
  only, SAE/Enterprise joins are refused (`planJoin`).

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

## What is not done (resume here, gate on)

1. **Core `serviceConnection` (`Ath10kNetwork.cpp`)** — the state exists, the servicing does not:
   - external-auth Auth stage: do not auto-send Open System auth when `s.externalAuth`; transmit
     `s.authTx` through `buildExternalAuth` when `authTxPending`, reset `sinceMs` on each TX/RX,
     advance to Assoc when `authDone`;
   - MAC change at stage Scan before Start: run `buildMacChangeWMI` steps 1..3 on CE3 (VDEV_DELETE,
     VDEV_CREATE with `s.local`, ECHO `kMacChangeEchoCookie`), then `_vdevMAC = s.local`; do not
     honour cancel between delete and create; allowVdevDelete is already OR-ed with `macChange.step`;
   - PMF TX slot: send `s.pmfTx` (SA Query request/response) when idle like the spectrum report;
     set `lastTxPMF` in `submitNetworkPacket` and clear `pmfTx.inFlight` on completion; SA Query
     timing (`saQuery`: retry every 201 ms, fail after 1000 ms with `cancelRequested`);
   - enterprise: in `releaseRXBatch` deliver EAPOL frames other than EAPOL-Key (type 3) to the
     network interface before the port opens; in `outputPacket` let 0x888e frames out while
     `stage==Connected && enterprise && !portAuthorized` (via `submitNetworkPacket`).
2. **Front end (`AirportAth10kBridge.hpp`, `AirportAth10kInterface.cpp`, `AirportAth10k.cpp`)**:
   - `setHardwareAddress`: accept the private MAC (itlwm: `ifnet_set_lladdr`, `kIOMACAddress`),
     remember it in the session, pass it in the extended Connect (flag 16); `getHW_ADDR` keeps the
     permanent MAC; report the active MAC from Info [120..125];
   - SAE join: `planJoin` accepts `APPLE80211_AUTHTYPE_WPA3_SAE` (the WCL join request embeds an
     `apple80211_key` at 0x40: expect the password as cipher `APPLE80211_CIPHER_PWD`, up to 62 bytes
     from 0x50; unverified on the Mac), H2E when the AP's RSNXE has bit 5, drive
     `AthSAE::Session` through SendAuth/ReceiveAuth (commit, token retry, confirm), then
     `AuthResult 1`; station RSN from `buildStationRSN(group, 8, mfpc, mfpr, pmkid)`; supplicant
     `begin()` with AKM 8 and the SAE PMK/PMKID; keep a PMKSA cache per BSSID for `setCLEAR_PMKSA_CACHE`;
   - PMF policy (boot-arg, default: on when the AP is capable, required for WPA3) and the 81-byte key
     payload (`kKeyPayloadBytes`);
   - Enterprise: `planJoin` accepts the 802.1X upper-auth bits, extended Connect flag 8, PMK/MSK
     from `setCIPHER_KEY` (cipher 6/7/9, 32 bytes) starts the supplicant (AKM 1 or 5); hold
     ReceiveEAPOL polling until the PMK exists;
   - capabilities: advertise WPA3/PMF/802.1X to WCL only with the gate on; stop rewriting
     transition-mode RSN IEs to WPA2 when SAE is available.
3. **Tests**: core flow tests for SAE external auth, PMF deauth/SA Query, MAC change
   (`test_connection.cpp` harness); bridge tests for the three joins; the laptop trace comparison
   (random MAC VDEV sequence, PMF peer flags) against Linux ath10k.
4. **Docs**: move the three items out of the known-gaps lists in `kexts/wifi/README.md`,
   `docs/WIFI-NATIVE.md`, `docs/WIFI.md` when they work; record what remains Mac-only.

## Files

```
kexts/wifi/src/shared/Ath10kSHA256.hpp  Ath10kAES.hpp  Ath10kP256.hpp  Ath10kSAE.hpp
kexts/wifi/src/shared/Ath10kSecurity.hpp  Ath10kSupplicant.hpp  Ath10kControl.hpp
kexts/wifi/src/kext/Ath10kConnection.hpp  Ath10kUserClient.cpp  Ath10kBSS.hpp  Ath10kPCIDevice.{hpp,cpp}
kexts/wifi/tests/test_wpa3_crypto.cpp  test_wpa3_supplicant.cpp
kexts/wifi/native/src/AirportAth10kBridge.hpp (key payload size only)
```
