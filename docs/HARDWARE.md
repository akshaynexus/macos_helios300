# Acer Predator Helios 300 G3-571 — hardware inventory

Read from the owner's machine under Linux (CachyOS, 2026-11-07).

| Component | ID | macOS driver |
|---|---|---|
| CPU i7-7700HQ (Kaby Lake) | — | native XCPM; CPUFriend + data from MacBookPro14,3 when spoofing a newer SMBIOS |
| iGPU HD 630 | 8086:591b | WhateverGreen (+ OCLP root patches on macOS 14+) — owns the internal eDP panel |
| dGPU GTX 1060 Mobile | 10de:1c20, sub 1025:118b, VBIOS 86.06.4F.00.08 | `PG106.kext` + `PG106Accel.kext` (submodule): render offload only, like Linux PRIME — see below |
| Audio | Realtek ALC255 (10ec:0255, sub 1025:118a) | AppleALC layout-id 29 (AppleALC's ALC255 list: "dhinakg - Realtek ALC255 for Acer Predator G3-571"); macOS 26 also needs OCLP's AppleHDA root patch |
| Ethernet | RTL8111 10ec:8168 | RealtekRTL8111 |
| Wi-Fi | Qualcomm Atheros QCA6174 168c:003e | `kexts/wifi/` (this repo) |
| Bluetooth | Lite-On 04ca:3016 (QCA Rome), on xHCI USB 2 port 7 (HS07) | `kexts/bluetooth/` (this repo) + BlueToolFixup; `kexts/bluetooth/acpi/SSDT-HS07-UPC` marks HS07 internal (the DSDT has `_UPC` only on HS09) |
| Card reader | RTL8411B 10ec:5287 | RealtekCardReader |
| Touchpad | Synaptics SYNA7DB5 06CB:7DB7 (I2C HID) | VoodooI2C + VoodooI2CHID |
| Keyboard | PS/2 | VoodooPS2Controller |
| USB | xHCI 8086:a12f | USBToolBox + UTBMap |
| Storage | WD Blue SATA SSD | native |
| Webcam | Quanta 0408:a060 (UVC) | native |

dGPU power: ACPI `PG00` `_ON/_OFF` via `_PR3` on PEG0 (D3cold available).
The dGPU VBIOS is served by ACPI `_ROM` (no ROM chip).

## How the GTX 1060 is used on macOS vs Linux

Same Optimus shape as Linux PRIME render offload: the Intel GPU owns the
internal panel and the desktop; the GTX 1060 renders for the apps that ask
for it, and its frames are composited on the Intel side (IOSurfaces).

Differences from Linux with NVIDIA's driver on this laptop:
- **HDMI port:** physically wired to the GTX 1060 (its only output — no MUX,
  the panel can never be on the dGPU). On Linux NVIDIA drives it; on macOS
  PG106 has **no display driver yet**, so the HDMI port does not work.
- **Clocks:** no Pascal PMU firmware exists outside NVIDIA's driver, so the GPU
  stays at VBIOS boot clocks (like nouveau): much slower than on Linux.
- **App selection:** apps choosing the NVIDIA Metal device, the plugin's
  per-app allow list, or Vulkan via NVK — instead of `prime-run`.
- **Idle power-off:** same ACPI `PG00._OFF` mechanism; opt-in
  (`pg106_d3cold=1`) until tested on the machine.
