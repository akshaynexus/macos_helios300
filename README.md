# macos_helios300

macOS 26 (Tahoe, Intel) on the **Acer Predator Helios 300 G3-571** (2017):
i7-7700HQ, Intel HD 630, GeForce GTX 1060 Mobile (GP106M, Optimus, no MUX),
Qualcomm Atheros QCA6174 Wi-Fi + Bluetooth.

This repo holds the machine-specific drivers that do not exist elsewhere:

| Path | What | Licence | Status |
|---|---|---|---|
| `kexts/nvidia-macos-driver` (submodule, branch `pascal-gp106`) | `PG106.kext` (GTX 1060 Mobile, nouveau-derived; OpenCore-injected, firmware in the binary), `PG106FB.kext` (HDMI framebuffer) and `PG106Accel.kext` (Metal accelerator), both installed into /Library/Extensions | GPL-2.0 / PolyForm-NC (per tree) | PG106 runs on macOS 26.7.2 (bring-up to PG106Ready Yes); PG106FB attaches; HDMI picture and PG106Accel not yet tested |
| `kexts/wifi/` | QCA6174 (`168c:003e`, Acer `11ad:0807`) native Wi-Fi: `Ath10kMac.kext` core + `AirportAth10k.kext` IO80211 front end ([docs/WIFI.md](docs/WIFI.md), [docs/WIFI-NATIVE.md](docs/WIFI-NATIVE.md)) | GPL-3.0; imported Apple-ABI headers used with permission; firmware: Qualcomm licence | runs on macOS 26.7.2: Wi-Fi interface, power, scans, network list; joins (WPA2) and receives (hardware run 7), but sent nothing in run 7; the TX fix (19272e4) waits for run 8 |
| `kexts/bluetooth/` | QCA6174 Rome Bluetooth (`04ca:3016`) firmware uploader | see `kexts/bluetooth/LICENSE` | works on macOS 26.7.2 (owner report); pairing, A2DP/HFP, sleep/wake to be tested |
| `tools/` | cross-build helpers (Linux → macOS 26 kexts); `tools/release.sh` + `tools/release/` (release builder, manifest spec + schema, release gates); `tools/macos/` (post-install scripts run on the Mac: the GPU stack, audio options 1 and 2; `test-stdin.sh` runs them over a fake SSH pipe on Linux) | GPL-2.0-only | |
| `docs/` | hardware inventory, kext list, OpenCore notes | CC-BY-4.0 | |

The machine runs macOS 26.7.2 (25G332) with these drivers since 2026-11-08.
[docs/MACOS-STATUS.md](docs/MACOS-STATUS.md) is the one table of what works on
the hardware, what is built but untested, and what is paused; each driver's
README states exactly what was tested where.

Hardware facts come from the machine itself (`docs/HARDWARE.md`).

## Releases

A release is the set of built artifacts plus `manifest.json`, which says how each one goes into an OpenCore EFI
(Kernel → Add order, Min/MaxKernel, SSDTs, DeviceProperties, NVRAM, what must not be there) and gives its SHA-256.
The contract is [docs/MANIFEST.md](docs/MANIFEST.md).

```sh
tools/release.sh --version 0.1.1 --real-kc ~/pascal-build/macos-26.7.2-25G332   # cross-builds everything into dist/
```

`release.sh` builds from `git archive` exports of this repo and of the driver submodule's commit, and refuses a
release whose PG106 lacks its embedded firmware or whose kexts fail the audit against the target Mac's real kernel
collections. It publishes nothing; a reviewed `dist/` is uploaded as a GitHub pre-release by hand. v0.1.0 is
published (first test pre-release); the v0.1.1 procedure is at the end of [docs/MANIFEST.md](docs/MANIFEST.md).

[1401](https://github.com/akshaynexus/1401/tree/linux-port) (the owner's fork, Linux app) detects this laptop and
builds a macOS 26 OpenCore EFI and USB stick from the pinned release manifest.

