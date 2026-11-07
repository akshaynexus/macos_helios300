# Wi-Fi on macOS 26 (Helios 300 G3-571)

Card: Qualcomm Atheros QCA6174, PCI `168c:003e`, rev `0x32` (hw3.2), Lite-On subsystem
`11ad:0807`.

Driver: `kexts/wifi` (Ath10kMac) plus the Ath10k Connectivity menu-bar app.

## OpenCore checklist

- Kernel → Add: `Ath10kMac.kext` with `MinKernel` `25.0.0`. It has no Lilu dependency, so order doesn't matter.
- No AirportItlwm, itlwm or AirportBrcmFixup for this card. No OCLP wireless root patch.
- No boot args required. For debugging, use `keepsyms=1`.
- App in `/Applications`. Ad-hoc sign it once on the Mac (`kexts/wifi/README.md` → Install).

## Expected

- An Ethernet-style "Ath10k WLAN Adapter" in Network settings, with DHCP, DNS, VPN and IPv6 as usual.
- Scan and join from the app: open networks, and WPA2-PSK (CCMP, with CCMP or TKIP
  group) on 2.4 and 5 GHz. The app keeps passwords in the Keychain and rejoins the
  preferred network at login and after wake.

Not available:

- The system Wi-Fi menu and Settings.
- AirDrop, Continuity and Location via Wi-Fi. The plan for these is in `WIFI-NATIVE.md`.
- WPA3-only networks and networks that require PMF.
- Enterprise (802.1X) networks.

TX power is capped at 10 dBm, so range is lower than on Linux.

What was verified on Linux on this laptop, the owner test steps for macOS, and rollback
are all in [`kexts/wifi/README.md`](../kexts/wifi/README.md).
