# Acer Predator Helios 300 G3-571 — hardware inventory

Read from the owner's machine under Linux (CachyOS, 2026-11-07).

| Component | ID | macOS driver |
|---|---|---|
| CPU i7-7700HQ (Kaby Lake) | — | native XCPM; CPUFriend + data from MacBookPro14,3 when spoofing a newer SMBIOS |
| iGPU HD 630 | 8086:591b | native on macOS 26.7.2 (Kaby Lake drivers, Metal 3) with WhateverGreen — owns the internal eDP panel; brightness needs `applbkl=1`, see "Brightness" below |
| dGPU GTX 1060 Mobile | 10de:1c20, sub 1025:118b, VBIOS 86.06.4F.00.08 | `PG106.kext` + `PG106Accel.kext` (submodule): render offload only, like Linux PRIME — see below |
| Audio | Realtek ALC255 (10ec:0255, sub 1025:118a) | macOS 26 has no AppleHDA: VoodooHDA 3.6.8 in /Library/Extensions (option 1) or AppleHDA put back + AppleALC layout-id 29, "dhinakg - Realtek ALC255 for Acer Predator G3-571" (option 2); see "Audio on macOS 26" below |
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

## Brightness (internal panel, macOS 26.7.2)

The CMN N156HCE-EAA panel on the HD 630 needs, besides SSDT-PNLF (`\_SB.PCI0.GFX0.PNLF`, `_UID` 16: the
Skylake/Kaby Lake profile, PWM max 0x56C) and `-igfxblt` (WhateverGreen's backlight-registers fix for Kaby Lake on
macOS 13.4+), the IGPU property `applbkl` = `<01000000>`. WhateverGreen's default (`applbkl` "detect") patches
AppleBacklight only on IGPU-only machines: it switches the patch off when any discrete GPU is visible, and the GTX
1060 stays visible here for PG106. Without the patch AppleIntelPanelA has no `F16T<panel>` profile for this panel,
AppleBacklightDisplay gets `bklt` but no `brightness` parameter, and macOS shows no brightness slider (first macOS 26
boots, 2026-11-08). 1401's Helios profile writes it (`helios-backlight`); by hand: DeviceProperties > Add >
`PciRoot(0x0)/Pci(0x2,0x0)` > `applbkl` Data `01000000` (or the boot-arg `applbkl=1`).

Check after the boot: `ioreg -lw0 -r -c AppleIntelPanelA | grep -o F16Txxxx` (WhateverGreen's profile is there) and
`ioreg -lw0 -r -c AppleBacklightDisplay | grep -o '"brightness"={[^}]*}'`.

## Audio on macOS 26

macOS 26 removed AppleHDA.kext, so AppleALC (a patcher for AppleHDA) and NMHDA (which adds the GP106 HDMI audio
function to AppleHDAController's device table) have nothing to work on. OpenCore Legacy Patcher's root patcher
refuses this machine: next to "Miscellaneous: Modern Audio" it lists "Graphics: Nvidia Web Drivers" for the GTX 1060
and requires nvda_drv. The Web Drivers must never be installed (PG106 owns the GPU), and hiding the GPU from OCLP
(`-wegnoegpu`) is not an option either. Two routes, both reversible:

**Option 1: VoodooHDA, no root-volume change** — `tools/macos/audio-voodoohda.sh`

- CloverHackyColor/VoodooHDA GitHub release `Release368` (2026-09-18): VoodooHDA 3.6.8, macOS 26.5 SDK, built on
  25G83, ad-hoc signed. `VoodooHDA.kext-368.zip` sha256 `9aaa01b245f4827b2de5edfdc028700b9bc8823f03bfb1daf280b8571f88adb0`;
  binary `932a7c9e46a65fdab7c8729f599c1094b568ae4fc7f17dad9818c83e833a0973`, Info.plist
  `21b886b5449b9a0739422b6720ea2bc04fdcc477f79e1e3cece021033a51bf1a`. Not chris1111/VoodooHDA-Tahoe: it packages
  VoodooHDA 3.0.3 (2025), without the AppleALC pin tables. Not v3.7.1-alpha: its OpenCore-injectable `VoodooHDA_BL`
  compiles Apple's IOAudioFamily classes in under their own names (IOAudioDevice, IOAudioEngine, ...), which would
  collide with the real IOAudioFamily the moment AppleUSBAudio, AppleGFXHDA or AudioAUUC load it (USB headsets).
- It links IOAudioFamily (740.1, compat 1.0, only in SystemKernelExtensions.kc on 25G332) and IOGraphicsFamily
  (600, ditto), so OpenCore cannot inject it (same as PG106FB): /Library/Extensions, root:wheel 755, `kmutil load -p`,
  restart. 1401 builds `csr-active-config` 030A0000: 0x1 (untrusted kexts) and 0x200 (no approval prompt); the script checks.
  The prefPane is optional and not installed.
- Config: none. Keep AppleALC: VoodooHDA 3.6.8 matches `IONameMatch` HDAS, which stays in the device's
  `compatible` list after AppleALC renames it HDEF, and it takes its pin table from `layout-id`. With AppleALC loaded
  that is 7, which has no ALC255 entry, so VoodooHDA reads the codec's BIOS pin defaults. Without AppleALC it would be
  the injected 29, and AppleALC layout 29 puts the headphone jack (0x21, out) and the jack input (0x19, in) in the
  same association 2: VoodooHDA disables an association with mixed directions ("wrong direction"), i.e. no 3.5 mm
  output (the symptom in CloverHackyColor/VoodooHDA#21, whose own cause was AMD controller matching).
- If headphones or the microphone do not work on the stock kext, `NODES=1` writes this ALC255 pin set into the
  kext's `NodesToPatch` (applied after the layout/BIOS pins, before associations are parsed) and re-signs it ad hoc:
  0x14 speaker `0x90170110` (assoc 1 seq 0), 0x21 headphone `0x0421101F` (assoc 1 seq 15: speaker muted while
  headphones are in), 0x12 internal mic `0x90A60120` (assoc 2 seq 0; node 18 is the internal-mic path in AppleALC's
  Platforms29), 0x19 mic jack `0x04A1102F` (assoc 2 seq 15). Pin values from layout 29's ConfigData
  (0x14 0x90170110, 0x19 0x04811020, 0x1B 0x90A00130, 0x21 0x04211020, EAPD on 0x14), re-associated.
- HDMI audio: no. VoodooHDA 3.6.8's personalities match HDAS and AMD (1002:aaxx) HDMI functions only, not the
  GP106's 10de:10f1, and NMHDA needs AppleHDAController.
- Revert: `MODE=revert` (removes the kext, rebuilds the auxiliary collection), restart. If the system does not come
  up: safe mode from the OpenCore picker (Shift+Enter; /Library/Extensions kexts are not loaded), then `MODE=revert`.

**Option 2: AppleHDA back on the root volume, as OCLP's Modern Audio does** — `tools/macos/audio-applehda.sh`

- Source: OCLP 3.0.0's PatcherSupportPkg 2.0.5 `Universal-Binaries.dmg` in OpenCore-Patcher.app (sha256
  `1c290781bc726759ff37a110ffcb7182a6c6fd3d8ebce112d134d482c8de52a3`, the release asset; encrypted with OCLP's
  public passphrase), folder "26.0 Beta 1": AppleHDA.kext 600.2 (re-signed by Dortania's CI; the script prints its
  tree hash). KDK: `Kernel_Debug_Kit_26.7.2_build_25G332.dmg` from dortania/KdkSupportPkg (exact build, sha256
  `9490bd9b5e16ff5cee8b7330c498d64f27a008aa91c5c70d686a00dc5978c555`), installed under /Library/Developer/KDKs.
- Steps (OCLP sys_patch): SIP 0x1/0x2/0x800, FileVault off, SecureBootModel Disabled, AMFIPass; mount the root
  volume at /System/Volumes/Update/mnt1; rsync the KDK's /System/Library/Extensions in; AppleHDA.kext to
  /System/Library/Extensions (755, root:wheel); /System/Library/CoreServices/OpenCore-Legacy-Patcher.plist;
  `kmutil create --allow-missing-kdk --volume-root /System/Volumes/Update/mnt1 --update-all --variant-suffix release`;
  RSRRepair; `bless --folder .../CoreServices --bootefi --create-snapshot`.
- Gives HDMI audio too: NMHDA's targets (AppleHDAController probe/start, the Kepler 10DE:0E0A... table entries) are
  in AppleHDA 600.2; AppleALC layout 29 drives the ALC255.
- Revert: `MODE=revert` = `bless --mount /System/Volumes/Update/mnt1 --bootefi --last-sealed-snapshot`, restart;
  from Recovery: `bless --mount "/Volumes/Macintosh HD" --bootefi --last-sealed-snapshot`. A macOS update drops the
  patch (run it again after one).

