# AWDL / AirDrop on the QCA6174: research, design, honest status

**Owner requirement:** AirDrop working natively (Finder and the Share Sheet) on macOS 26.7.2 with
the Helios 300's QCA6174 through `AirportAth10k.kext`.

**State (2026-11-08): research build, off by default, never run on hardware.** The whole thing is
behind the build option `AIRPORT_ATH10K_AWDL` (`AWDL=1` at build) and the boot-arg `ath10k_awdl=1`.
With the option off the kexts are byte-identical to a build without this work (`Ath10kMac`
`d938c785...`, `AirportAth10k` `74e2df96...`).

What exists, and what proves it (host means Linux unit tests, no radio, no Mac):

| Layer | State | Proof |
|---|---|---|
| Protocol engine (OWL port) | done | byte-exact against OWL vectors, two-node simulation |
| Owly's rules: shared/fixed placements, sequence intersection, ranking, peer timeout, A-MSDU bound, OWLY-2 echo, AWDL-18 early-SYN hold | done | host tests; the Fixed/Shared logic mirrors Owly's `node.rs`/`channel.rs` |
| Parsers against real iPhones | done | replay of Owly's three iPhone captures (`native/tests/test_native_awdl_capture.cpp`, T-AWDL-1/3 style): every action frame parses, the phone turns valid, schedule within 3 TU, 4,400+ data frames delivered |
| Radio binding, shared + exclusive placements (`Ath10kAwdl.hpp`, `Ath10kAwdlService.cpp`) | written | commands byte-checked, state machine against a firmware model; **no firmware has seen it** |
| `awdl0` on macOS 26 (`IO80211AWDLProtocol` subclass, hybrid BSD ifnet) | written | kc-audit vs 26.7.2 25G332; creation order against fakes; **never booted** |
| Feeding Apple's stack (peer manager RX, service records, neighbor entries) | written | layouts read from the 26.7.2 binaries; exercised only against fakes |
| AirDrop in Finder | **unknown** | needs a boot and a phone; see section 8 |

No AWDL frame has been exchanged by this card on any OS yet. Nothing in this document has run on the
Helios. Read section 8 before expecting AirDrop.

## 1. What AWDL needs from a Mac's Wi-Fi

AWDL (Stute et al., MobiCom '18; USENIX Sec '19) is Apple's ad-hoc link under AirDrop: 16 TU
availability windows, 64 TU slots, a 16-slot channel sequence, an elected sync master, and vendor
action frames (PSF, MIF) carrying TLVs. Data frames are ordinary 802.11 data frames on the AWDL BSSID
`00:25:00:ff:94:73` with an LLC/AWDL header in front of the IPv6 packet. AirDrop itself is all userland
(`sharingd`, `mDNSResponder`, `wifip2pd`/`airportd`) over link-local IPv6 on `awdl0`.

On a Mac the MAC-layer schedule is in the Broadcom firmware and the kernel side is the family's
`IO80211AWDLPeerManager`. A third-party driver has to be the firmware. That is what this code does.

## 2. Prior art, as of 2026-11

- **Owly** (https://github.com/t4t5/owly, MIT): the first AWDL proven with iPhones in software, on
  Linux, on Intel, MediaTek, Atheros and Realtek cards, including real AirDrop. QCA6174/ath10k is not
  listed (the `*` entry says it needs monitor-mode injection while associated). It runs in two
  placements (see section 3 and 5a); this project adopts them. `SPEC.md`, `conformance.md` and its
  capture corpus are the reference used here. Its **MIT notice is in `kexts/wifi/native/NOTICE`**.
- **Bananz0/AirportItlwm-Tahoe-AWDL** (https://github.com/Bananz0/AirportItlwm-Tahoe-AWDL, GPL-2.0+):
  Intel 9560 on the *legacy* (OCLP-restored Ventura) IO80211Family. It feeds received action frames to
  Apple's peer manager through `bpfTapInput(DLT 12)`, posts availability-window messages, extracts service
  TLVs from OOB frames, and dequeues TX with `dequeueTxPackets`. Reached "AirDrop session start", no
  complete transfer. Its findings on the legacy family informed section 5d; **only structure and
  semantics were reused, no code.** On the native 26.x family the same entry points exist and were
  re-derived from the 26.7.2 binaries; offsets differ.
- **itlwm issue 1062 / kgp-macPro's AirportItlwm-Tahoe** (GPL-2.0): `awdl0` as `IO80211P2PInterface` on
  the legacy family, wifip2pd attached, AirPlay and Screen Mirroring working, AirDrop not. Dequeue path
  blocked until Bananz0 found the per-AC budgets (their conclusion, adopted there, not used here since this
  driver is hybrid BSD).
- **OCLP fork (InsanelyMac thread 362042)** restores the legacy Modern Wireless stack
  (`IO80211FamilyLegacy`, `IOSkywalkFamily` downgrade, `WiFiPeerToPeer`, `wifip2pd`) by root patch. It
  does nothing for a third-party AWDL radio; on Intel it reaches `awdl0` and AirPlay, not AirDrop. It is
  not applicable to this native-family driver.
- Nobody has AWDL on the native macOS 26 family with a non-Apple card.

## 3. What the QCA6174 can do

Evidence: `kexts/wifi/evidence/linux-iw-phy.txt`, the firmware (`WLAN.RM.4.4.1-00309`, WMI op 4, HTT 3,
feature bits `WOWLAN_SUPPORT IGNORE_OTP_RESULT MFP_SUPPORT`, **no `RAW_MODE_SUPPORT`**), Linux `ath10k`.
Interface combinations list `managed + monitor` on one channel and `managed + P2P-client` on two.

- **Verified against Linux ath10k source (2026-11-08):** monitor vdev type is `WMI_VDEV_TYPE_MONITOR = 4`;
  the HTT TX descriptor flags `0x05` for data are `RAW | HDR_PRESENT | NO_ENCRYPT` (`PKT_TYPE` 0 in bits
  7:5) and `0x65` for action frames (`PKT_TYPE` 3, `ATH10K_HW_TXRX_MGMT`), ext-tid 16 (non-QoS) and 17
  (management). The two values the first pass took from memory were correct.
- **Unknown until a run:** whether RM.4.4.1 radiates a monitor vdev's raw frames while associated.
  Linux reaches the air differently in shared mode: mac80211 finds a local address in the injected
  frame's addr2 and sends it through the *station* vdev (Native Wi-Fi TX, no RAW). This driver keeps to
  the monitor vdev (RAW). They are not the same firmware path, so the Linux test below gives evidence for
  the card, and may need the driver to switch to the station-vdev path if RAW fails.

## 4. The engine: `native/src/AirportAth10kAwdl.hpp`

A header-only, allocation-free port of OWL (SEEMOO, GPL-3.0, commit `0ee3263`, attribution in the
file) of everything platform-independent, extended with Owly's placements and rules (MIT, attribution
in the file):

- `kPresenceFixed` (Owly's `Presence::Fixed`) and `kPresenceShare` (`Presence::Share`): the radio stays on
  one channel; shared advertises only the sync master's slots on that channel (AWDL-12), keeps its previous
  sequence if the master never names it (rather than announce it is never available), ranks masters counter
  first; fixed advertises every slot and ranks metric first, as Apple devices do.
- `kPresenceOwl` keeps OWL's original behaviour for the byte-exact vector tests.
- Peers are valid after a MIF and a Version TLV; a valid peer silent for 5 s is dropped; data frames count as
  hearing it (AWDL-17). A-MSDUs are bounded to 16 subframes (AWDL-14).
- The Session layer (`AirportAth10kAwdlSession.hpp`) adds: placement follows the station (below); OWLY-2 (an
  ICMPv6 echo to each valid peer every 3 s, replies swallowed); AWDL-18 (a TCP SYN from a not-yet-valid peer
  held up to 2 s and delivered after its neighbor entry); the macOS stack's service records (section 5d).
  PSFs carry HT capabilities as Owly's spec requires (OWL's did not; vectors unaffected).

Differences from OWL and Owly are stated in the file headers.

## 5. What is implemented (everything needs `AWDL=1` and `ath10k_awdl=1`)

### 5a. Radio binding (core): placements

Owly's two placements, chosen by the Session from the station link:

| Station state | Placement | Radio |
|---|---|---|
| Joined on channel 6, 44 or 149 | **Shared** | monitor vdev (id 1) with the station's MAC, on the station's channel definition (center and phy copied, as Linux `ath10k_monitor_vdev_start`). No channel changes. The station's own receive path ACKs the peer's unicast (Owly's design). |
| Not joined | **Exclusive/fixed** | monitor vdev on one social channel (default 6, `ath10k_awdl_ch=`), no ACKs from the chip |
| Joined on another channel | none | AWDL off and `awdl0` not running; waits |

- awdl0's MAC is the station's MAC in every placement (AWDL-2 needs a stable address, and shared mode needs
  this address so the station ACKs).
- The state machine moves between placements through a full tear-down and restart. A join in progress, a
  disconnect, a CSA, or a roam stops the radio and the session restarts it when the station is stable.
- The core (`awdlPlan`) enforces it: shared runs only while the station is `Connected` and not in a channel
  switch, on the same channel; exclusive only while the station vdev is idle.
- TX: HTT `TX_FRM` on the monitor vdev, one frame at a time on CE4, serialized against the station's
  management TX (the core's single-owner path), and held off while the station's data slots are outstanding
  so the two never batch in the completion stream. Under heavy Wi-Fi traffic AWDL frames wait for those
  slots to drain.
- Exclusive placement has no ACK path (Owly's IBSS/hidden-AP is not implemented; the Linux test says
  whether the card can do it). Discovery works; unicast from peers is retried on air.

### 5b. `awdl0` on macOS 26 (IO80211Family 1200.13.1)

Read from the 26.7.2 collection (symbol tables and vtables via `tools/kc-audit.py`; only names, slots
and offsets were used). `awdl0` on 26.x is an `IO80211AWDLProtocol` subclass on `IO80211VirtualInterface`,
not the legacy `IO80211P2PInterface`. The family creates the peer manager (`IO80211AWDLPeerManager`,
1100+ methods) for role 6. This driver's interface answers `setVIRTUAL_IF_CREATE` for role 6.

The 63 pure-virtual AWDL handlers (`IO80211AWDLProtocol`), checked against what
`AppleBCMWLANProximityInterface` does with each on 26.7.2:

- Answered from the engine (sync params, election ID/metric, master channel, sync state, presence, BSSID,
  channel, peer cache size): 23, as before.
- Now implemented: `SYNCHRONIZATION_CHANNEL_SEQUENCE` set+get (stored), `OOB_REQUEST`, `OOB_AUTO_REQUEST`
  (section 5d), `SECONDARY_MASTER_CHANNEL` get.
- Accepted and counted, no firmware to apply them to (`AirportAth10kAwdlIoctls` in the registry shows what a
  real run calls): `EXTENSION_STATE_MACHINE`, `ELECTION_RSSI_THRESHOLDS`, `ELECTION_PARAMS`, `PEER_CACHE_CONTROL`,
  `SYNC_FRAME_TEMPLATE`, `PEER_TRAFFIC_REGISTRATION`, `AF_TX_MODE`, `MIN_RATE`, encryption keys/type,
  `TXCAL_PERIOD`, advertisers, `DFSP`, `OOB_SEC_PAYLOAD`, `LLW`, `TX_RATE`, `P2P_COEX_PROTECTION`, `PSF_DWELL`,
  `CONFIG`, `RTG_PEER_STATS_CONFIG`.
- Refused (return Unsupported) because the driver does not keep the data: `COMPANION_SKYWALK_IF_CREATE`
  and the getters of those parameters.
- **Unknown:** which of these `wifip2pd` and the peer manager need to succeed. The registry counter settles
  that on the first boot.

### 5c. Data path and addressing

The same hybrid as `en1`: `AirportAth10kAwdlNet` is an `IOEthernetController` whose BSD interface has
the prefix `awdl`, Wi-Fi sub-family and the output pull model, passed to
`IO80211VirtualInterface::prepareBSDInterface` and `peerToPeerConfigureIfnet` (the family's own routine that
sets the peer-to-peer `eflags` for role 6). Output: mbufs -> the engine's unicast/multicast queues -> sent in
the peer's availability window. Input: frames the engine delivers -> `inputPacket`.

This is **not** the native Skywalk data path (`IOSkywalkTxSubmissionQueue`, `dequeueTxPackets`) that Broadcom
uses. IP traffic does not need that path: the ifnet is a normal ifnet to `sharingd` and `mDNSResponder`, and
output never passes through the flow queues traced in the legacy family. Whether the kernel's AWDL
special-casing (IFEF_AWDL, `peerToPeerConfigureIfnet`) behaves with a hybrid interface is unknown.

Neighbor entries (`NET-2`): `nd6_alt_node_present/absent` (kernel, the API Apple's AWDL path uses) create the
peer's link-local neighbor entry from its AWDL address. IPv6 link-local autoconfiguration on `awdl0` itself:
left to the normal macOS path, not verified.

### 5d. Feeding Apple's stack

`ath10k_awdl_stack=<mask>` turns each feed off (default 7):

- bit 0: **Peer manager RX.** Every received AWDL action frame goes to the family's
  `IO80211VirtualInterface::bpfTapInput(mbuf, DLT 12, meta, 0x48)`, the same path
  `AppleBCMWLANProximityInterface::handleActionFrame_rx` uses on 26.7.2 (it copies into a family buffer,
  `IO80211Controller::allocFramePool`, created at controller start). Metadata (source at 0, channel at
  0x0c, rate 0x28, RSSI 0x2c, MAC time 0x30, size 0x48) read from the binary; `isValidAF` requires it.
  The peer manager gets to see peers, service records and Service Response TLVs (type 2) from them.
- bit 1: **Neighbors** as above.
- bit 2: **Service records from OOB.** The family hands us `apple80211_awdl_oob_request` for the MI frame it
  builds (`updatePrimaryPayloadMI`); the frame pointer is at +0x30 and the length at +0x28 (read from
  Broadcom's handler; the reconstructed header's inline `data[]` is older). Service Response (2) and ARPA (16)
  TLVs go into our MIFs. One-shot unicast OOB frames are transmitted.
- bit 3 (default off): `IO80211VirtualInterface::setLinkState` on bring-up.

Not done: the availability-window-start message the family's `reportDataPathEvents` handles (index 0 of the
first switch; Bananz0 posts message 41 on the legacy family; on 26.7.2 its handler reads the interface state,
not verified further), and any TX through Apple's flow queues.

## 6. What macOS needs, and how sure we are

From the 26.7.2 binaries:

- The family creates `IO80211AWDLPeerManager` and drives it. `wifip2pd` configures it via ioctls that go
  through `IO80211PeerManager::ioctl_internal` to the 63 handlers; the peer manager wants `enableAWDL`
  (`interfaceStateChange`) and gates action-frame input on its own state (`+0x3ee0` bit 1, `isValidAF`).
  Frame feed is `bpfTapInput` DLT 12. All confirmed in disassembly.
- Service records: `IO80211AWDLPeerManager::updatePrimaryPayloadMI` hands the MI frame down via OOB (confirmed);
  received Service Response TLVs reach it through the action frame feed (the parser is in the peer manager).

From research (not verifiable on Linux; no macOS userland binaries here):

- `sharingd` needs `awdl0` with AWDL capability; `mDNSResponder` publishes `_airdrop._tcp` over D2D/awdl0; AirDrop
  is then HTTPS to the peer's link-local address port 8770 (Owly SPEC; TLS is user-level).
- Whether `sharingd` accepts this non-Broadcom awdl0 at all is **unknown** until it runs. It may refuse the
  whole path (capability checks, Continuity gating); the Intel attempts reached AirDrop only in isolated reports.
- BLE AirDrop advertisement (the wake-up): on a sending Mac, `sharingd` advertises Apple manufacturer data with TLV
  type 5 (Owly SPEC BLE-1..5). The Rome chip's BLE works per the owner; never tested here.

## 7. On-device test steps (coordinator installs; one change per boot; known-good fallback)

Install the `AWDL=1` kexts and keep a fallback boot. Then, in order:

1. **`ath10k_awdl=1 ath10k_awdl_stack=0`** (radio only, nothing into Apple's stack):
   - Station must still join as before.
   - `ifconfig awdl0` shows the interface, `ioreg -l | grep AirportAth10kAwdl` shows
     `AirportAth10kAwdlState` 2 and `AirportAth10kAwdlPlacement` 2 (shared, when on channel 6/44/149).
   - Log: `log stream --predicate 'sender == "AirportAth10k"' | grep -i awdl`. Peers appear as `awdl: peer added`.
2. **Peers visible while joined** on an AP on 6/44/149: an iPhone/Mac with AirDrop open gives peers; a Mac
   sees us with `sudo log stream --predicate 'process == "wifip2pd"'`.
3. **`ath10k_awdl_stack=7`** (default): repeat, and check `AirportAth10kAwdlIoctls` (which handlers get
   called), `AirportAth10kAwdlFedFrames`, `AirportAth10kAwdlOob`. Then **Finder AirDrop / Share Sheet**.
4. `ifconfig awdl0` for `fe80::` (if no address: note it, `sudo ifconfig awdl0 inet6 -autoconf` and report).
5. A file each way. `wdutil info`, `netstat -I awdl0`, `AirportAth10kAwdl*` registry keys, and panics.
6. Join/leave paths: joining another channel stops AWDL (state leaves Running, no panic); returning restarts.

Recovery: `-airportath10k_frontoff` disables the front end; or drop the boot-arg.

**Linux live USB test (the cheapest radio proof, do this first if possible):** `tools/owly-live-usb.sh`. On a
Linux live USB booted on the laptop: `./owly-live-usb.sh bundle DIR` here, copy `owly-live.tar.gz` next to the script,
`sudo ./owly-live-usb.sh setup`, then `./owly-live-usb.sh test shared` (Wi-Fi joined on 6/44/149, iPhone AirDrop
Everyone). It runs `owly test-card` (a photo each way) and tells you which placement this card can do. Then
`test exclusive`. The result decides whether the macOS shared mode has a working radio to match.

## 8. What can still fail, bluntly

Unproven: everything on hardware. Specifically, in order of likely failure:

1. **The firmware may not radiate monitor-vdev RAW frames while a station is joined**, or may refuse the monitor
   vdev next to a joined station. The Linux test distinguishes "card can't" from "this driver can't".
2. **First-boot behaviour of the hybrid** `IO80211VirtualInterface`: `prepareBSDInterface` and the peer manager may
   reach Skywalk state that does not exist for a hybrid interface (the previous pass flagged this). Panic is possible
   on first boot; keep the fallback.
3. **Apple's userland may simply not use it.** `sharingd`/`wifip2pd` may refuse a non-Broadcom interface, or expect
   handlers we refuse, or the peer manager's data path. The Intel forks never got reliable AirDrop on the legacy
   stack; this is the native one, which nobody has tried.
4. **Timing.** The core has ms ticks and a host-side MAC; AWDL's 3 TU guard tolerates a few ms, but shared mode has
   no hardware-assisted schedule. Owly works in software, so this is not fatal, but it is unmeasured here.
5. AirDrop-specific: `sharingd` Contacts-Only needs Apple identity validation; the "Everyone" path is the first target.
6. Coexistence limit: AWDL TX waits for outstanding station data TX; AirDrop while the Wi-Fi link is busy will be slow.

Not implemented: `IOSkywalkTxSubmissionQueue` native rings, Owly's exclusive ACK via IBSS/hidden AP, ADDBA responses
(Owly accepts only on cards that ACK aggregates), channel hopping across social channels.
