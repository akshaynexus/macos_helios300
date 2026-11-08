# macOS 26.7.2 Wi-Fi evidence (sanitized)

Logs from the owner's Helios 300 (macOS 26.7.2 25G332, OpenCore) for the native Wi-Fi
runs, in the order of the run history in [`docs/WIFI-NATIVE.md`](../../../../docs/WIFI-NATIVE.md).

Sanitized with [`sanitize.py`](sanitize.py): every MAC address (AP BSSIDs, the card's
address, the Private Wi-Fi Address) is `02:00:00:00:00:NN`, numbered by first appearance
across the whole set (the same address has the same number in every file); SSIDs are
`SSID-A`, `SSID-B`; host name, user name and private IPv4 addresses are replaced. The
script's mapping file (`.sanitize-map.json`) holds the real values and is never committed.
Panic reports are reduced to their header line and the plain-text panic string.

| File | Run | What it shows |
|---|---|---|
| `run2-panic-ionetworkcontroller-start.txt`, `run3-…` | 2, 3 (front end injected) | Page fault (CR2 0x658) in `ml_thread_policy` from `IONetworkController::start`: `IO80211WorkQueue::getThread()` is NULL on 1200.13.1. Fixed by `AirportAth10kNub` (d1ab92a). |
| `delayed-bringup-boot-first.txt`, `delayed-bringup-boot-stream.txt` | `ath10k_native_delay=90` boot | Bring-up deferred past login; `log stream` of AirportAth10k/IO80211Family/Ath10kMac while the front end comes up (1f5ffc5). |
| `shutdown-panic-registrationinfo-zone-1.txt`, `-2.txt` | nub build, shutdown/reboot | "not in the expected zone early.kalloc.288, found in kalloc.type.var5.384" in `IOSkywalkNetworkInterface::deregisterNetworkInterface`: the family typed-frees RegistrationInfo blocks it never allocated. Fixed in ac7d33a. |
| `run5-join-private-mac-refused.txt` | 5 | Scan and list work; `APPLE80211_IOC_ASSOCIATE` fails 0xe00002c7: WCL's `SET_MAC_ADDRESS` (Private Wi-Fi Address) was refused. Fixed in 36dc4f4/32be721. |
| `run6-join-extended-bss-info.txt` | 6 | The core associates; WCL's `setCurrentBSS` reads `WCL_EXTENDED_BSS_INFO` right after `JOIN_ASSOC_COMPLETE`, gets 0xe00002d8 (NotReady), leaves with "Update Bss fail" (statusCode 1, extendedCode 3), the password prompt returns; registry snapshot of the core after the leave. Fixed in d39e247. |

Run 1 (OpenCore refused AirportAth10k's vtable) is described in WIFI-NATIVE.md; its OpenCore
log was not kept. The two hangs of 2026-11-08 (`-wegnoegpu` + PG106 in /L/E, and the first
AirportAth10k-injected boot) left no readable log (the log archive was corrupt), so nothing
of them is here.
