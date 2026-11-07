# awdl-run2 — OWL on the QCA6174 under Linux, against a Mac's AirDrop

**Question.** Can this card (ath10k, WLAN.RM.4.4.1) receive and send AWDL frames well enough to
join an AWDL cluster: hear a Mac's action frames (PSF/MIF), synchronise to it, hop the channel
sequence, and be seen by it? This is the hardware floor for any kext work; nothing on macOS can
be verified before it.

## Owner steps (on the Mac, macOS 27 is fine: the air protocol is the same)

1. Wi-Fi and Bluetooth on. The Mac may stay joined to the home AP.
2. Finder › AirDrop (⇧⌘R). Set "Allow me to be discovered by" to **Everyone** and keep the
   window in front for the whole run (about 5 minutes). That keeps the Mac's AWDL master/slave
   engine running and broadcasting action frames on 6/44/149.
3. When the script prints "opendrop receive", drag any small photo onto the device that appears
   in the AirDrop window that is *not* another Apple device (it carries the laptop's host name).
   If nothing appears, do nothing; that is a result too.
4. Report the printed SUMMARY block and attach `~/pascal-build/awdl-run2/log/*.txt`
   (MACs are already hashed).

## What the script does

Checks SSH is on Ethernet, installs `libev`/`cmake`, builds OWL at commit `0ee3263` with its
submodules, puts `wlan0` into (passive) monitor mode by hand, runs `owl -N -c 44` for 90 s and
`owl -N -c 6` for 90 s with trace logging, optionally OpenDrop `find`/`receive` during the
channel-44 phase, then restores NetworkManager and managed mode. Exit code 2 = SSH not on
Ethernet (nothing touched); 3 = build problem; 4 = monitor mode refused.

## Expected signals

| Line in the summary / log | Meaning |
|---|---|
| `log/before.txt` has no "active monitor" line | expected: this card ACKs nothing in monitor mode, so the Mac retransmits unicast to us up to 7×; discovery still works |
| `add peer mac#…` within the first 20 s of a phase | **PASS for RX**: the Mac's AWDL action frames are received and parsed on this channel |
| `switch channel to N (slot S)` lines | OWL follows the merged channel sequence: `iw set channel` works fast enough on ath10k |
| `election`/`master` lines naming a peer | sync + election with the Mac's cluster |
| `awdl0 neighbours ≥ 1` | IPv6 neighbour for the Mac on awdl0 (data-plane address learned) |
| `opendrop find` lists a device | mDNS over AWDL reached the Mac and back (bidirectional data frames) |
| the Mac shows the laptop in the AirDrop window, a file lands in `~/pascal-build/awdl-run2/` | **full PASS**: TX of action + data frames works without ACKs |

Any `add peer` on at least one channel is enough to continue with the kext design. "Peers
added 0" on both channels with the AirDrop window open means either the Mac stayed on a
different channel (it prefers 149 when on a 5 GHz AP; a channel-149 pass is the next run) or
the ath10k monitor RX filter drops vendor action frames, which would change the kext design
(a P2P-device vdev + management RX instead of monitor).

## Changes from run 1

Run 1 failed before OWL started: `iw dev wlan0 set type monitor` returned 0 but the interface
stayed managed (NetworkManager/wpa_supplicant still owned it), so libpcap saw no radiotap DLT
and OWL exited ("could not initialize core"). Run 2 adds a dedicated `mon0` monitor vif on
the phy with wlan0 down, checks `type monitor` before starting OWL (exit 4 otherwise), builds
OWL with `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (CMake 4), installs `setuptools<81` for
OpenDrop on Python 3.14, uses `asroot timeout N …`, and routes every log through the MAC
hash with SSID lines stripped. Invocation: `export __HB_DIR; export -f asroot; bash run.sh`.
