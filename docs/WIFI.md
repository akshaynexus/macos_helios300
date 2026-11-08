# Wi-Fi on macOS 26 (Helios 300 G3-571)

Card: Qualcomm Atheros QCA6174, PCI `168c:003e`, rev `0x32` (hw3.2), Lite-On subsystem
`11ad:0807`.

Driver: `kexts/wifi` — `Ath10kMac.kext` (the ath10k core) + `AirportAth10k.kext` (the native
IO80211 front end). Design: [`WIFI-NATIVE.md`](WIFI-NATIVE.md).

## OpenCore checklist

- Kernel → Add, in order: `Ath10kMac.kext`, then `AirportAth10k.kext`; `MinKernel` `25.0.0`.
  No Lilu dependency.
- No AirportItlwm, itlwm or AirportBrcmFixup for this card. No OCLP wireless root patch.
- No boot args required. `ath10k_cc=XX` picks the regulatory country, `ath10k_txpower=N`
  caps TX power (dBm). For debugging, `keepsyms=1 debug=0x100`.

## Expected

- A Wi-Fi interface in Network settings, the system Wi-Fi menu and Settings › Wi-Fi with
  the nearby networks; join by selecting the network and entering the password once
  (Keychain); DHCP, DNS, VPN and IPv6 as usual.
- Open networks and WPA2-PSK (CCMP, with CCMP or TKIP group) on 2.4 and 5 GHz; WPA2/WPA3
  transition networks through WPA2.
- TX power at the regulatory limit per channel (23 dBm on the owner's channel 44).

Not available: WPA3-only and PMF-required networks, Enterprise (802.1X), AirDrop/Continuity
(AWDL), 6 GHz.

What was verified on Linux on this laptop, the owner test steps for macOS, and rollback
are all in [`kexts/wifi/README.md`](../kexts/wifi/README.md).
