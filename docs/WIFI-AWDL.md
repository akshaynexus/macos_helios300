# AWDL / AirDrop on the QCA6174: research, feasibility, design, status

**Owner requirement:** AirDrop (and the other Continuity features that ride on AWDL, Apple
Wireless Direct Link) working on macOS 26 with the Helios 300's QCA6174 through
`AirportAth10k.kext`.

**State (2026-11-08): research build, off by default, never run on hardware.**
`AIRPORT_ATH10K_AWDL=0` (the default, `AWDL=1` at build) keeps the release builds free of any AWDL
code. With `AWDL=1` the kexts carry the radio binding and the `awdl0` interface, enabled at
run time with the boot-arg `ath10k_awdl=1`. Build: `AWDL=1 bash kexts/wifi/scripts/cross-build.sh`.
With the option off the kexts are byte-identical to a build without this work (checked for this
commit: `Ath10kMac` `d938c785...`, `AirportAth10k` `74e2df96...`, each equal with and without
the AWDL source).

What exists and what proves it:

| Layer | State | Proof |
|---|---|---|
| Protocol engine | done (OWL port) | byte-exact against OWL vectors, two-node simulation |
| Radio binding in the core (`Ath10kAwdl.hpp`, `Ath10kAwdlService.cpp`, ops 32-35) | written | firmware commands checked byte for byte; state machine against a firmware model (`native/tests/test_native_awdl_radio.cpp`) |
| `awdl0` on macOS 26 (`AirportAth10kAwdlDriver.*`, `IO80211AWDLProtocol.h`) | written | kc-audit against 26.7.2 25G332: imports, 568-slot vtable, class sizes; creation order against fakes (`test_native_awdl_iface.cpp`) |
| Data path, IPv6 link-local | hybrid path written (section 5c) | fakes only |
| On the air / AirDrop | **nothing tested** | laptop off for about a week; the coordinator runs the checklist in section 7 |

No AWDL frame has been exchanged by this card on any OS yet. Nobody has `awdl0` on the WCL
(1200.x) family with a non-Apple card, so this is research; read section 8 before expecting
AirDrop to work.

Earlier hardware runs (Linux, OWL, against a macOS 27 Mac with AirDrop open to Everyone):
- **awdl-run1:** OWL never started. `iw dev wlan0 set type monitor` returned 0, but the
  interface stayed managed.
- **awdl-run2:** the `mon0` monitor vif worked and OWL created `awdl0`, but no regulatory
  domain was set: channel 44 was no-IR ("Cannot inject frames on channel 44"), nothing
  radiated, 0 peers. OpenDrop works with `setuptools<81`. The run was cut off mid-way.
- **awdl-run3 (optional):** needs a Linux live USB now that CachyOS is erased: `iw reg set IN`
  first, restore the previous value at the end, run under a `systemd-run` unit with a timed
  restore. It would tell us whether this firmware radiates injected frames at all before
  a macOS boot is spent on it.

## 1. What AWDL is, and who does the work on a Mac

AWDL (Stute et al., MobiCom '18 "One Billion Apples' Secret Sauce"; USENIX Sec '19 "A Billion
Open Interfaces for Eve and Mallory") is Apple's ad-hoc protocol under AirDrop, AirPlay,
Sidecar, Continuity Camera and the rest:

- **Frames.** Vendor-specific 802.11 action frames (category 127, OUI `00:17:f2`, type 8)
  carry TLVs: synchronisation parameters, election parameters (v1 and v2), the channel
  sequence, service parameters, HT capabilities, data-path state, the host name (ARPA) and
  the version. Two subtypes: PSF (periodic sync frame, every 110 TU as master, 440 TU as
  slave) and MIF (master indication frame, mid availability window). Data frames are normal
  802.11 data frames to the AWDL BSSID `00:25:00:ff:94:73` with an LLC/SNAP + 8-byte AWDL
  header carrying IPv6; A-MSDU is used.
- **Time.** Availability windows (AW) of 16 TU; four AWs form an extended AW of 64 TU; the
  channel sequence has 16 slots of one EAW each, so the sequence repeats every 1.05 s. A node
  is on the slot's channel for the whole EAW. Social channels are 6, 44 and 149; a node also
  lists its infrastructure channel so that it stays reachable while associated.
- **Election and sync.** Every node advertises a metric and a counter; the node with the
  highest (counter, metric) is the master, others synchronise their AW counter/phase to the
  master's sync parameters (a sync tree with a maximum height). Masters send PSFs more often.
- **Discovery and transfer.** mDNS over the AWDL IPv6 link-local addresses (`_airdrop._tcp`
  with a Bloom-filter-based pre-discovery over BLE), then HTTPS on a mutual-TLS connection
  for AirDrop itself. All of that is userspace on a Mac (`sharingd`, `mDNSResponder`,
  `wifip2pd`/`awdld`, `airportd`); the kernel and the firmware only have to provide the
  `awdl0` interface and the AWDL MAC-layer schedule.

**On Apple's own hardware the AWDL engine (sync, election, channel hopping, action-frame
TX/RX in the availability windows) runs in the Broadcom firmware.** The kernel driver only
passes parameters down and events up. That is visible in the reconstructed IO80211 headers:
about 100 `APPLE80211_IOC_AWDL_*` ioctls (sync params, election ID/metric, channel sequence,
presence mode, master channel, BSSID, OOB action-frame requests, peer traffic registration,
sync frame template, AF TX mode, …) and `IO80211VirtualInterface::postPeerPresence/
postPeerAbsence/postSyncStateChanged/postNewMasterElected`. There is no mac80211-style
"inject this frame now" interface; a third-party driver must therefore *be* the firmware
for this purpose. That is what the engine below does.

## 2. Prior art on third-party drivers (all negative so far)

- **itlwm / AirportItlwm (Intel).** The legacy front end (macOS ≤ 14 ABI) implements the
  AWDL ioctls as stubs that accept parameters; it never had an AWDL engine. On macOS 26 the
  WCL front end (laobamac's V2 path that AirportAth10k is ported from) has **no virtual
  interface code at all**: `awdl0` is not created.
- **OpenIntelWireless/itlwm issue #1062** (Tahoe): the most thorough attempt. With the
  OCLP-restored *legacy* IO80211FamilyLegacy and a patched Ventura AirportItlwm, the author
  reached: `awdl0` created as `IO80211P2PInterface` (role 6), `wifip2pd` attached, link
  state active, IPv6 link-local assigned, Apple's AWDL action frames received on channel 6
  and decoded, a management frame submitted through the Intel firmware. Never reached: a
  usable TX data path (the queued AWDL packets are not exposed through the legacy dequeue
  paths), any synchronised transmit, any AirDrop transfer beyond ~4 isolated one-time
  reports across all users. Conclusion in that thread: interface creation is solvable, the
  hard part is the scheduled MAC layer and the Apple-private TX/RX handshake.
- **Nobody has `awdl0` on the native macOS 26 (WCL, IO80211Family 1200.x) family** with a
  non-Apple card. This project is on that family, so there is no example to copy.

## 3. What the QCA6174 and WLAN.RM.4.4.1 can do (from Linux ath10k, this laptop)

Evidence: `kexts/wifi/evidence/linux-iw-phy.txt`, `linux-debugfs-wmi_services.txt`, the
firmware container (`firmware-6.bin`: version `WLAN.RM.4.4.1-00309`, WMI op version 4 = TLV,
HTT 3, feature bits `WOWLAN_SUPPORT IGNORE_OTP_RESULT MFP_SUPPORT`; **`NO_P2P` is not set**),
Linux `ath10k` at `d48723711bae`.

| Need | Firmware/driver fact | Verdict |
|---|---|---|
| Receive vendor action frames | management RX via `WMI_MGMT_RX` with channel, SNR, rate (no TSF in the TLV event); Linux delivers all management frames | yes (host timestamps the frame on arrival; AWDL's 16 TU granularity tolerates ms jitter) |
| Transmit arbitrary action frames | management TX via `HTT_H2T_MSG_TYPE_MGMT_TX` (the path the core already uses for auth/assoc; `WMI_SERVICE_MGMT_TX_WMI` is not advertised); any 802.11 frame body | yes |
| Transmit data frames to the AWDL BSSID | monitor/injection on Linux uses `ATH10K_HW_TXRX_RAW` through HTT TX; in the kext the same HTT TX with a monitor vdev, or native-wifi TX on a P2P-device vdev | yes, to be proven on hardware |
| Second virtual interface | `P2P-device`, `P2P-client`, `P2P-GO`, `monitor`, `mesh` listed; combinations allow `managed + P2P-client` on **2 channels** (firmware MCC), `managed + monitor` on 1 channel | yes |
| Fast channel changes | `remain_on_channel` via a one-channel WMI scan (Linux ROC); a monitor vdev follows `VDEV_RESTART`; both are ~ms operations | yes for 64 TU slots, with a few ms of dead time per switch |
| TSF access | `inc_tsf`/`dec_tsf` vdev params exist; no TSF read in the TLV management RX event | not needed: the engine keeps its own clock like OWL |
| ACKing frames addressed to us while not associated (active monitor) | **not advertised** on this card (`iw phy` lists no "active monitor") | peers retransmit unicast up to 7×; discovery still works; throughput suffers. A STA/P2P-device vdev with a firmware peer entry per AWDL peer would ACK; to be tested |
| Concurrent infrastructure link on another channel | MCC with a P2P-client vdev (2 channels); with monitor only 1 channel | AWDL fully concurrent only when the engine's sequence stays on the infra channel, or with the P2P vdev variant |

Nothing fundamental blocks AWDL on this card. What a Broadcom card has and this one lacks is
the firmware-side schedule; it has to be done on the host with ms precision. The one hardware
weakness is the missing active-monitor ACK, which a different card (ath9k, or a Broadcom
with Apple firmware) would fix, but it degrades, not prevents, AirDrop.

## 4. The engine: `native/src/AirportAth10kAwdl.hpp`

A header-only, allocation-free port of OWL (SEEMOO, GPL-3.0, commit `0ee3263`,
attribution in the file) of everything platform-independent: wire helpers, the frame
builders, the TLV parsers, sync, election, channel sequence, a fixed 32-entry peer table,
the transmit schedule (availability windows, unicast guard 3 TU, multicast guard 16 TU, the
multicast EAWs 0 and 10) and an `Engine` with OWL's timers (channel switch at every AW, PSF
every `psf_interval`, MIF mid-EAW, peer clean-up + election every second, unicast/multicast
queues) driven by `tick(now)`, `receive(frame)` and `send(ethernet)` over a `Sink`
(`setChannel`, `transmit`, `peerAdded/Removed`, `syncChanged`, `data`). Times are integer
microseconds; frames are plain 802.11 (no radiotap). Differences from OWL are listed at the
top of the file.

Verified on the host (`tests/run-tests.sh` → `native/tests/test_native_awdl.cpp`, ASan/UBSan):

- PSF, MIF and data frames are **byte-exact** against frames produced by OWL's own C code
  with the same state and clock (`native/tests/awdl_vectors.inc`, regenerated by
  `awdl-vectors/generate.sh`); parse results (peer fields, channel sequence), election
  outcomes, sync updates and schedule values are identical to OWL's.
- OWL's unit tests (sync, election, peers) ported and passing.
- Two engines on a shared simulated medium: election (the better metric wins, the other
  becomes a slave at height 1 with the slave PSF interval), synchronisation within 3 TU,
  unicast and multicast data delivered only in the right windows, loopback, drop to
  unknown peers, master loss and re-election after the 2 s timeout.
- Truncated frames, RSSI filter with grace, buffer limits, foreign management, A-MSDU.
- Compiles under the kext cross-build flags with only `memcpy`, `strlen`, `bzero` imports.

Not in the engine (as in OWL): dynamic channel-sequence adaptation (static sequence on the
master channel; OWL's `awdl_chanseq_init_idle` is available), AWDL encryption/security
TLVs, the service parameter bitmask (service discovery is mDNS over IPv6 and needs nothing
from the engine), NAN sync.

## 5. What is implemented

Everything in this section is compiled only with `AIRPORT_ATH10K_AWDL=1`.

### 5a. Radio binding (ath10k core)

`src/kext/Ath10kAwdl.hpp` (pure logic, host-tested) and `src/kext/Ath10kAwdlService.cpp` (the CE
glue). The state machine owns a **monitor vdev, id 1**, created with the type the Linux
monitor mode uses:

- Bring-up: `VDEV_CREATE` (type 4, monitor), `VDEV_START` (20 MHz HT on the chosen channel,
  the regulatory limit as power), `VDEV_UP`. Each command is one CE3 transaction with the
  completion rule the core's other WMI users apply; `VDEV_START`/`RESTART`/`STOP` also wait
  for the WMI event. Events for vdev 1 are consumed before the core's parser, which rejects
  anything else as unsolicited. A missing reply fails the radio after 3 s, never the station.
- Channel retune (`AwdlSetChannel`): `DOWN`, `RESTART`, `UP` (three round trips, the same
  sequence as the station's channel switch). Not used in the default static mode.
- Frames: AWDL action frames arrive via the WMI management-RX event (filtered: category
  127, OUI 00:17:f2, type 8); data frames with address 3 = AWDL BSSID via HTT RX (raw MPDU,
  FCS trimmed, or native WiFi), both timestamped on arrival. TX goes through HTT `TX_FRM` with
  the vdev id in the descriptor, type MGMT for action frames and RAW for data, one frame in
  flight at a time from an 8-entry queue. Frames travel through typed in-kernel calls
  (`nativeAwdlTx/Rx`), since a control `Request` carries at most 512 bytes.
- Lifecycle: control ops 32-35 (`AwdlRadioStart/Stop/SetChannel/Status`). A firmware reload
  resets the radio; the front end asks again.

**Firmware limits, stated plainly.**

| Limit | Consequence | Degraded behaviour |
|---|---|---|
| The core runs one WMI command at a time and the station path owns the rings | No AWDL while the station vdev is joining or joined; the radio tears down on a join and is restarted when idle | AirDrop only when not associated (Option-click the Wi-Fi menu and Disconnect, or no saved network in range) |
| No host-side MCC implementation; a monitor vdev cannot follow the infrastructure channel | Same as above | Concurrent infrastructure + AWDL is future work |
| Timing resolution: the core's runtime tick (1-10 ms) and the 3-command retune | A 64 TU slot can lose a few ms; AW alignment is approximate | **Static channel sequence on one social channel (default 6)**: this node is on air for the peers' slots on that channel only. 6 is chosen because it is TX-legal in the world domain; `ath10k_awdl_ch=44|149` needs the regulatory domain to allow TX there |
| No firmware timestamp in the management-RX event | The host stamps frames on arrival | Sync error of a few ms instead of microseconds; AWDL's 16 TU windows tolerate it |
| No active monitor on this card (`iw phy`) | Unicast frames addressed to us are not ACKed by the firmware | Peers retry up to 7 times; discovery works, throughput suffers |
| Injected monitor-vdev frames | Whether WLAN.RM.4.4.1 radiates them from a monitor vdev without a peer entry is unproven | Run 3 (Linux) or the on-device checklist below settles it |
| 6 GHz, A-MSDU/A-MPDU TX, 802.11 fragmentation | Not implemented | Single MSDU frames only |

### 5b. `awdl0` on macOS 26 (IO80211Family 1200.13.1)

Read from the 26.7.2 collection (symbol tables and vtables, `tools/kc-audit.py --dump`; nothing
copied beyond names and offsets):

- `awdl0` is a subclass of **`IO80211AWDLProtocol`** (568 vtable slots, classSize 0x130, no
  ivars) on **`IO80211VirtualInterface`** (505 slots, 0x130), itself an
  `IO80211SkywalkInterface`, not the legacy `IO80211P2PInterface`. The class has **63 pure
  virtual slots** (505-567): the `getAWDL_*/setAWDL_*` ioctl handlers.
- Apple's Broadcom driver creates it from `setVIRTUAL_IF_CREATE` (the Infra interface's slot
  556) → `AppleBCMWLANCore::createProxInterface`: `withOptions` →
  `IO80211AWDLProtocol` ctor, `init(controller, mac, 6, name)` (role 6 = AWDL, the role WCL
  asks for), `attach`, `initRegistrationInfo`, then its own `start` → a native
  `IOSkywalkEthernetInterface::registerEthernetInterface` with rings. Peer management is in the
  family (`IO80211AWDLPeerManager`, ~1000 methods), which the base class creates for role 6.
- This driver: `AirportAth10kAwdlInterface : IO80211AWDLProtocol` (role 6, interface id 2, 23
  handlers answered from the engine or stored, the other 40 refused as Unsupported rather than
  reading a guessed layout), `setVIRTUAL_IF_CREATE` answered for role 6 with the BSD name and
  MAC, created at bring-up when `ath10k_awdl=1`.
- `include/Airport/macOS26/IO80211AWDLProtocol.h` is generated from those slots. kc-audit
  proves it at every `AWDL=1` build: `AirportAth10kAwdlInterface` 568 slots match the parent,
  `IO80211VirtualInterface`/`IO80211AWDLProtocol` sizes 0x130, all imports exported.

### 5c. Data path and addressing

Same hybrid as the infrastructure interface, not a Skywalk nexus: `AirportAth10kAwdlNet` is a
second `IOEthernetController` (provider: the main controller, same IO80211 work queue) whose
BSD interface `AirportAth10kAwdlBsd` has the name prefix `awdl`, sub-family Wi-Fi and the
output pull model (the same SPI that gave en1 its flowswitch). The Skywalk side goes through
`IO80211VirtualInterface::prepareBSDInterface` in the gate and `peerToPeerConfigureIfnet`
(IFEF_AWDL and the peer-to-peer ifnet parameters).

Output: mbufs → `outputStart` → the engine's unicast/multicast queues → transmitted in the
peer's availability window. Input: frames the engine delivers → `inputPacket`. The link is
active while the radio runs. IPv6 link-local: the interface comes up with the AWDL MAC
(locally administered); assignment of the `fe80::` address is left to the normal macOS
autoconfiguration. Not verified. AirDrop uses mDNS/TLS over that link-local address.

Not implemented: native Skywalk rings (`IOSkywalkTxSubmissionQueue` and friends), which is
what `IO80211VirtualInterface` is built for on Broadcom; delivering received AWDL action frames
to Apple's `IO80211AWDLPeerManager` (its `actionFrameInput` takes `IO80211Buffer` objects
from a family pool; the IO80211Buffer ownership contract is not yet known), so Apple's
stack does not see peers; peers appear only in this driver's log and registry.

## 6. What blocks what

| Step | State | Blocked by |
|---|---|---|
| Protocol engine | done, host-verified | — |
| Radio binding in the core | written, host-verified against a firmware model | the first run on real firmware (does a monitor vdev start next to the idle station vdev, do frames radiate) |
| `awdl0` on macOS 26 | written, ABI audited against 26.7.2 | first boot (`ifconfig awdl0`), vtables are proven, behaviour is not |
| Skywalk nexus and Apple peer manager feed | not implemented | a first `awdl0` boot, and the IO80211Buffer contract |
| AirDrop transfer | — | everything above plus Apple's userspace accepting a non-Broadcom `awdl0` (section 8) |

A different Wi-Fi card does **not** remove any of these except the ACK weakness: the
only cards whose firmware runs AWDL are Apple-flashed Broadcom ones, for which Apple's own
driver exists.

## 7. Owner test steps

Build with `AWDL=1`, install, boot with `ath10k_awdl=1` (plus `ath10k_awdl_ch=6` if another channel was tried). The coordinator installs; one change per boot.

**Before anything else**: with the join path unaffected, Wi-Fi must still join as before (`ath10k_awdl` not set: identical builds apart from the gated code paths). Check with `ath10k_awdl=1` first that the station still joins and that disconnecting returns to idle.

1. **awdl0 exists:** `ifconfig awdl0` shows the interface, `UP`, and a `fe80::` address appears within seconds of the link going active. If there is no address: `sudo ifconfig awdl0 inet6 -autoconf` and report; this is the IPv6 link-local item.
2. **Radio up:** `ioreg -l -w0 | grep -E 'AirportAth10kAwdl'` shows `AirportAth10kAwdlState` 2 (Running); the log has `awdl: awdl0 started`, no `awdl:` failure lines. `log stream --predicate 'sender == "AirportAth10k"' | grep -i awdl`.
3. **Peers appear** (Wi-Fi disconnected, a Mac or iPhone with AirDrop open nearby on 2.4 GHz): `awdl: peer added <mac> name '<name>'` within a few seconds. Report `AirportAth10kAwdlPeers` and any `awdl-*` failure strings from `AirportAth10kLastMessage`.
4. **Peer sees us:** on the other device, AirDrop should list "helios300" if discovery works over mDNS; AWDL peers should be visible with `sudo log stream --predicate 'process == "wifip2pd"'` on a Mac.
5. **Finder › AirDrop** on the Helios lists the other device; a small file in each direction.
6. **Stop paths:** join a Wi-Fi network (the log shows AWDL tearing down; `AirportAth10kAwdlState` leaves Running; join succeeds as without AWDL) and disconnect (AWDL comes back).
7. Report `wdutil info` (AWDL section), `netstat -I awdl0`, the log, and the first failing step.

Things to record even if everything fails: which firmware errors appear (`awdl-vdev-start-rejected`, `awdl-firmware-timeout`), whether `VDEV_START` was accepted at all, and any panic log.

## 8. What can still fail (plainly)

- The firmware rejects a monitor vdev next to the idle station vdev, or never radiates injected frames from it, or drops other-BSS frames: the core reports `awdl-firmware-timeout` / nothing received. A Linux `awdl-run3` tells this apart from the macOS side.
- `IO80211VirtualInterface` is built around native Skywalk rings. The hybrid BSD interface is unproven for a virtual interface; `prepareBSDInterface` may reach IOSkywalk state that only exists after `registerEthernetInterface`. The result would be a panic on the first boot; `-airportath10k_frontoff` and removing the boot-arg are the recovery. **Boot with a known-good fallback available.**
- Apple's AWDL userland (`wifip2pd`, `sharingd`, `mDNSResponder`) was designed against the Broadcom firmware and IO80211AWDLPeerManager events. `sharingd` may refuse a non-Broadcom card, the family may not feed our interface, `wifip2pd` may immediately reconfigure it through ioctls we refuse (40 are `Unsupported`), or the Continuity entitlements for AWDL use may gate features. The Intel thread suggests the discovery path can work; AirDrop transfers worked only in isolated reports there.
- The static channel sequence means long discovery times and missed peers; peers that do not revisit channel 6 are unreachable. Concurrent AWDL + Wi-Fi is not implemented; AWDL needs the Wi-Fi disassociated.
- AWDL link-layer encryption is not implemented (OWL has none; AirDrop's payload is TLS over the link), but other Continuity features may assume it.
