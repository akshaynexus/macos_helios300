# macos_helios300

macOS 26 (Tahoe, Intel) on the **Acer Predator Helios 300 G3-571** (2017):
i7-7700HQ, Intel HD 630, GeForce GTX 1060 Mobile (GP106M, Optimus, no MUX),
Qualcomm Atheros QCA6174 Wi-Fi + Bluetooth.

This repo holds the machine-specific drivers that do not exist elsewhere:

| Path | What | Licence | Status |
|---|---|---|---|
| `kexts/nvidia-macos-driver` (submodule, branch `pascal-gp106`) | `PG106.kext` (GTX 1060 Mobile, nouveau-derived) and `PG106Accel.kext` (Metal accelerator) | GPL-2.0 / PolyForm-NC (per tree) | cross-built, not yet run on macOS |
| `kexts/wifi/` | QCA6174 (`168c:003e`, Acer `11ad:0807`) native Wi-Fi: `Ath10kMac.kext` core + `AirportAth10k.kext` IO80211 front end ([docs/WIFI.md](docs/WIFI.md), [docs/WIFI-NATIVE.md](docs/WIFI-NATIVE.md)) | GPL-3.0; imported Apple-ABI headers used with permission; firmware: Qualcomm licence | cross-built for macOS 26, host-tested, cross-checked against Linux ath10k; not yet run on macOS |
| `kexts/bluetooth/` | QCA6174 Rome Bluetooth (`04ca:3016`) firmware uploader | see `kexts/bluetooth/LICENSE` | in progress |
| `tools/` | cross-build helpers (Linux → macOS 26 kexts); `tools/release.sh` + `tools/release/` (release builder, manifest spec + schema) | GPL-2.0-only | |
| `docs/` | hardware inventory, kext list, OpenCore notes | CC-BY-4.0 | |

Nothing here has been validated on macOS on this machine yet; each driver's
README states exactly what was tested where (Linux cross-checks on the same
laptop, mocks, or hardware).

Hardware facts come from the machine itself (`docs/HARDWARE.md`).

## Releases

A release is the set of built artifacts plus `manifest.json`, which says how each one goes into an OpenCore EFI
(Kernel → Add order, Min/MaxKernel, SSDTs, DeviceProperties, NVRAM, what must not be there) and gives its SHA-256.
The contract is [docs/MANIFEST.md](docs/MANIFEST.md).

```sh
tools/release.sh --version 0.1.0 --dry-run     # cross-builds everything on Linux into dist/ (git-ignored)
```

`release.sh` builds from `git archive` exports of this repo and of the driver submodule's commit. It publishes
nothing; a reviewed `dist/` is uploaded as a GitHub pre-release by hand. No release has been published yet.

[1401](https://github.com/akshaynexus/1401/tree/linux-port) (the owner's fork, Linux app) detects this laptop and
builds a macOS 26 OpenCore EFI and USB stick from the pinned release manifest.

