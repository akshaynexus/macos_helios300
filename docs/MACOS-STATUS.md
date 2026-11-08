# macOS status on the Helios 300 G3-571

One table, kept honest: **works** means seen on this laptop under macOS, with where it is written down. Anything
only host-tested, cross-checked against Linux or reported second-hand says so. Machine: macOS 26.7.2 (25G332),
OpenCore EFI from 1401 with macos_helios300 v0.1.0 plus hand-installed newer builds, as of 2026-11-08.
Update this page in the same commit as the evidence.

## Works on the hardware

| What | Evidence |
|---|---|
| macOS 26.7.2 boots through OpenCore; HD 630 drives the internal panel natively (Kaby Lake drivers, Metal 3) | first boots, 2026-11-07/08 |
| Brightness slider, with `applbkl` = `01000000` on the HD 630 (WhateverGreen's panel patch forced on) | [HARDWARE.md](HARDWARE.md) "Brightness"; 1401 4b9a322, in the 0.1.1 manifest |
| PG106: OpenCore-injected, firmware embedded, GTX 1060 bring-up to `PG106Ready` Yes; with `pg106_selftest=2` the copy-engine and GR self-tests pass | driver repo `research/hardware/helios300/macos-runs/README.md` (build b72926bd, boots 2-3). The later logging builds hung (boot 5); the fix (driver e65024b) has not booted yet |
| Safe-mode stand-down: Shift+Enter (`-x`) in the OpenCore picker boots with PG106 not attached | macos-runs README, boot 5. Ath10kMac, AirportAth10k and QCARomePatchRAM have the same stand-down (in the code; not separately observed) |
| PG106FB from /Library/Extensions attaches under PG106 | macos-runs README, build 94f086ec, boots 2-3 |
| Wi-Fi interface (`en1`, type Wi-Fi), radio power, scans, the network list, Private Wi-Fi Address; the core associates | [WIFI-NATIVE.md](WIFI-NATIVE.md), hardware runs 4-6 |
| Bluetooth (QCARomePatchRAM + BlueToolFixup) | **owner report** (2026-11-08); no log kept. Pairing, A2DP/HFP and sleep/wake have not been tested one by one ([kexts/bluetooth/README.md](../kexts/bluetooth/README.md)) |
| The GP106's HDMI audio function 01:00.1 visible from boot (`SSDT-PG106-HDAU`) | macos-runs README, boot 1 (ioreg `pci10de,10f1`) |

## Built, not yet confirmed on the hardware

| What | State |
|---|---|
| Wi-Fi joins to completion (DHCP, traffic) | fix for the run-6 failure in d39e247; its hardware run is pending |
| Wi-Fi sleep/wake with the in-place firmware reload | a3da2e8, host-tested (10 cycles); not run on macOS |
| Wi-Fi country `IN` from the kext (`Ath10kCountryCode`, baked in by `release.sh --wifi-country`) | new in 0.1.1; until then only the boot-arg `ath10k_cc=IN` did it, and boot-args in a new config do not reach an installed Mac |
| HDMI picture through PG106FB | not reported yet |
| PG106Accel, the Metal plugin (NVMTLDriver.bundle), NVK-Pascal | cross-built and audited; never loaded on macOS; installed by `tools/macos/install-gpu.sh` (the `gpu` post-install step) |
| The plugin's AIR reader, `air-runtime/libLLVM.dylib` (upstream LLVM 23.1.4, new in 0.1.1; without it every uncached shader failed) | Linux: exports checked, and the same LLVM version reads the plugin's own Apple AIR (17 modules) through the plugin's call sequence. Never loaded on macOS: look for `library: in-memory bitcode reader ready` |
| Audio option 1 (fallback), `tools/macos/audio-voodoohda.sh` (VoodooHDA 3.6.8 in /Library/Extensions; no HDMI audio) | installed with the stock Info.plist on 2026-11-08: "Speaker (Analog)" and "Microphone (Digital)" at 48 kHz, but it sounds very bad (owner). The `TUNE` profile (PCM 90, Rec 75, InhibitCache) is not run yet; the main cause, no speaker EQ/limiter, is only fixed by option 2 |
| Audio option 2 (recommended), `tools/macos/audio-applehda.sh` (AppleHDA 600.2 on the root volume as OCLP's Modern Audio, layout 29; FileVault off; run again after every macOS update) | payload, AppleHDA and KDK pinned; every step sandbox-tested on Linux (fake macOS tree, the real payload image); not run on the laptop yet |
| NMHDA (GP106 HDMI audio) | needs AppleHDA, so only after option 2; its hook targets are present in that AppleHDA 600.2 (checked on Linux); never run with it |
| GPU idle power-off (`pg106_d3cold=1`), GPU sleep/wake | never run under macOS |
| USB: every port at USB 3 | only `UTBDefault` + `SSDT-HS07-UPC`; a USBToolBox map is still to be made |

## Paused or not possible

| What | Why |
|---|---|
| Wi-Fi WPA3-SAE/PMF and 802.1X Enterprise | paused by the owner (2026-11-08), compiled out by `AIRPORT_ATH10K_WPASEC=0` ([WIFI-SECURITY.md](WIFI-SECURITY.md)) |
| GTX 1060 reclocking | no Pascal PMU firmware outside NVIDIA's driver: the GPU stays at VBIOS boot clocks, as with nouveau |
| The internal panel on the GTX 1060 | not wired: no MUX, the panel is on the HD 630 only |
