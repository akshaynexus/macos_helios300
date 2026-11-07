# Acer Predator Helios 300 G3-571 — hardware inventory

Read from the owner's machine under Linux (CachyOS, 2026-11-07).

| Component | ID | macOS driver |
|---|---|---|
| CPU i7-7700HQ (Kaby Lake) | — | native XCPM; CPUFriend + data from MacBookPro14,3 when spoofing a newer SMBIOS |
| iGPU HD 630 | 8086:591b | WhateverGreen (+ OCLP root patches on macOS 14+) — owns the internal eDP panel |
| dGPU GTX 1060 Mobile | 10de:1c20, sub 1025:118b, VBIOS 86.06.4F.00.08 | `PG106.kext` + `PG106Accel.kext` (this repo); HDMI only, no MUX |
| Audio | Realtek ALC255 (10ec:0255, sub 1025:118a) | AppleALC (layout to be found) |
| Ethernet | RTL8111 10ec:8168 | RealtekRTL8111 |
| Wi-Fi | Qualcomm Atheros QCA6174 168c:003e | `wifi/` (this repo) |
| Bluetooth | Lite-On 04ca:3016 (QCA Rome) | `bluetooth/` (this repo) + BlueToolFixup |
| Card reader | RTL8411B 10ec:5287 | RealtekCardReader |
| Touchpad | Synaptics SYNA7DB5 06CB:7DB7 (I2C HID) | VoodooI2C + VoodooI2CHID |
| Keyboard | PS/2 | VoodooPS2Controller |
| USB | xHCI 8086:a12f | USBToolBox + UTBMap |
| Storage | WD Blue SATA SSD | native |
| Webcam | Quanta 0408:a060 (UVC) | native |

dGPU power: ACPI `PG00` `_ON/_OFF` via `_PR3` on PEG0 (D3cold available).
The dGPU VBIOS is served by ACPI `_ROM` (no ROM chip).
