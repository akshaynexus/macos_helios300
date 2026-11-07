# AWDL / AirDrop on the QCA6174: research, feasibility, design, status

**Owner requirement:** AirDrop (and the other Continuity features that ride on AWDL, Apple
Wireless Direct Link) working on macOS 26 with the Helios 300's QCA6174 through
`AirportAth10k.kext`.

**State (2026-11-08): PAUSED by the owner — AirDrop/AWDL is deferred and not part of the
first release.** Research is done. The AWDL protocol engine
(`kexts/wifi/native/src/AirportAth10kAwdl.hpp`) is written, host-tested byte-exact against
OWL and simulated end to end. Nothing in the kext includes it yet: only the host test
builds it, so release builds carry no AWDL code. When the work resumes, wire it in behind
a build option that is off by default.

The radio binding and the macOS `awdl0` interface are **not** wired yet. No AWDL frame has
been exchanged by this card yet. Nothing works on macOS yet. Read "What blocks what" before
planning on AirDrop.

Hardware so far (Linux, OWL, against a macOS 27 Mac with AirDrop open to Everyone):
- **awdl-run1:** OWL never started. `iw dev wlan0 set type monitor` returned 0, but the
  interface stayed managed.
- **awdl-run2:**
  - A dedicated `mon0` monitor vif works, and OWL started and created `awdl0`.
  - No regulatory domain was set, so channel 44 is no-IR. OWL logged "Cannot inject frames
    on channel 44". Its MIF/PSF frames were queued but never radiated, and it saw 0 peers.
  - OpenDrop works with `setuptools<81`.
  - The run was cut off mid-way; the laptop was restored by hand.

Resume with awdl-run3:
- Run `iw reg set IN` first and restore the previous value at the end, or start on channel
  149/6.
- Make the restore survive a lost SSH session: run the experiment as a `systemd-run` unit
  with a timed restore.

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

## 5. Design of the two missing layers

### 5a. Radio binding (ath10k core → `Sink`)

The core today is a single STA vdev with its own state machines; AWDL adds a second
`Ath10kAwdlRadio` path, kept apart from the join/scan logic:

1. **RX passthrough.** `recordWMIBSS` (Ath10kBSS.hpp) ignores everything but beacons and
   probe responses. A new hook in the WMI management RX path hands action frames with
   category 127 / OUI `00:17:f2` and all data frames whose address 3 is the AWDL BSSID to
   the front end with the SNR (RSSI = SNR − 95 dBm as the core already reports) and the
   arrival time. The front end calls `Engine::receive`.
2. **TX.** A `transmit(frame, len)` that submits a caller-built 802.11 frame through the
   existing HTT management-TX descriptor on the AWDL vdev (action frames) or through HTT
   data TX in raw/native-wifi mode (data frames), with the completion reported back
   (`HTT_MGMT_TX_COMPLETION 0xe` is already decoded).
3. **Channel.** A monitor vdev (`WMI_VDEV_TYPE_MONITOR`, created once, `VDEV_START`/
   `VDEV_RESTART` on `setChannel`) while the STA vdev is idle or on the same channel; when
   associated on another channel, the engine's sequence is pinned to the infra channel
   (OWL's known limitation) so that peers that list that channel are still reached. The
   P2P-device vdev + firmware MCC variant is the upgrade path once the monitor variant is
   proven; its ACK behaviour is the thing to measure.
4. **Timing.** `tick()` is scheduled from an `IOTimerEventSource` on the core's work loop
   with the next-due time the engine returns (≤ 16 TU ahead); `receive()` runs in the WMI
   RX path under the same gate. Dead time per channel switch (vdev restart ≈ 2–5 ms on
   ath10k) is inside the 64 TU slot.

The control-ABI additions (`AwdlRadioStart/Stop/SetChannel/Tx` and an RX ring the front
end polls like EAPOL frames) are deferred until the concurrent WPA3/Enterprise work in the
core files is at a stable point (coordination note in the commit history).

### 5b. macOS: `awdl0` on IO80211Family 1200.x (WCL)

What is known:

- The macOS 26 `IO80211Controller` (reconstructed V2 header) has the virtual-interface
  slots: `createVirtualInterface(ether_addr*, uint)`, `attachVirtualInterface(
  IO80211VirtualInterface**, ether_addr*, uint, bool)`, `detachVirtualInterface`,
  `enableVirtualInterface`/`disableVirtualInterface`, plus `getActionFramePoolCapacity`,
  `getPostOffice`/`CreatePostOffice` (private, pinned by name), `setChannelSequenceList(
  apple80211_awdl_sync_channel_sequence*)` and `isAwdlAssistedDiscoveryEnabled`.
- The owner's Apple-silicon Mac (macOS 27.2, IO80211Family 1200.13.1, AppleBCMWLANCore)
  lists `awdl0`, `llw0`, `nan0`, `ap1`, `anpi0/1` — the interface set WCL creates for a
  Broadcom card; `awdl0` is still a virtual interface of the controller.
- In the legacy model `awdl0` is an `IO80211P2PInterface` (role AWDL = 6) whose ioctls
  (`apple80211_ioctl.h`, `APPLE80211_IOC_AWDL_*`) carry the parameters, and the data path
  is `requestPacketTx` → `dequeueTxPackets`/`outputPacket` on the virtual interface and
  `inputPacket` for RX; peers are announced with `postPeerPresence/Absence`.

What is not known yet, and is being extracted from Apple's macOS 26 kernelcache (symbol and
string inventory only, nothing copied into the repo): which class WCL instantiates for the
AWDL role on 1200.x (`IO80211P2PInterface` vs a Skywalk-based peer-to-peer interface), the
exact WCL command set for AWDL (if `setWCL_*` setters exist for the AWDL ioctls), and the TX
dequeue contract that the Intel attempt could not find. The front-end plan is: implement
`createVirtualInterface`/`attachVirtualInterface` for role AWDL, answer the AWDL ioctls from
the engine's state (sync params, election ID/metric, BSSID, channel sequence, master
channel, sync state, presence mode, peer cache size, capabilities), post peer presence /
sync state / master elected from the engine's events, bridge `awdl0` RX/TX to
`Engine::send`/`Sink::data`, and keep the whole thing behind a boot-arg (`ath10k_awdl=1`)
until the first macOS run shows the vtables match.

## 6. What blocks what

| Step | State | Blocked by |
|---|---|---|
| Protocol engine | done, host-verified | — |
| Card hears and syncs to a Mac on Linux | run 2: awdl0 up, no frames radiated (no-IR channel) | awdl-run3 with a regulatory domain set |
| Radio binding in the core | designed | wifisec's in-flight core edits; then hardware (Linux core-trace parity, as for the join path) |
| `awdl0` on macOS 26 | designed from headers | the kernelcache symbol inventory; the first macOS load (vtable match) |
| AirDrop transfer | — | everything above plus Apple's userspace accepting a non-Broadcom `awdl0` (unknown; the Intel thread suggests sharingd works once the interface and link-local address exist) |

A different Wi-Fi card does **not** remove any of these steps except the ACK weakness: the
only cards whose firmware runs AWDL are Apple-flashed Broadcom ones, for which Apple's own
driver exists.

## 7. Owner test steps

Linux (when resumed): awdl-run3 (run 2 plus a regulatory domain), staged the way
`kexts/wifi/scripts/awdl-run2/` is.

macOS (after the binding and the interface land): with `ath10k_awdl=1`,
1. `ifconfig awdl0` exists after boot, `UP` with an `fe80::` address;
2. `log stream --predicate 'sender == "AirportAth10k"' | grep -i awdl` shows peers added
   when a Mac/iPhone opens AirDrop nearby;
3. Finder › AirDrop on the Helios lists the other device; the other device lists the
   Helios; a photo transfers in each direction;
4. report `wdutil info` (AWDL section) and the log with any failure.
