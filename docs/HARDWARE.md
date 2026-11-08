# Acer Predator Helios 300 G3-571 — hardware inventory

Read from the owner's machine under Linux (CachyOS, 2026-11-07); since 2026-11-08 the machine runs macOS 26.7.2
(25G332). What works there: [MACOS-STATUS.md](MACOS-STATUS.md).

| Component | ID | macOS driver |
|---|---|---|
| CPU i7-7700HQ (Kaby Lake) | — | native XCPM; CPUFriend + data from MacBookPro14,3 when spoofing a newer SMBIOS |
| iGPU HD 630 | 8086:591b | native on macOS 26.7.2 (Kaby Lake drivers, Metal 3) with WhateverGreen — owns the internal eDP panel; brightness needs `applbkl=1`, see "Brightness" below |
| dGPU GTX 1060 Mobile | 10de:1c20, sub 1025:118b, VBIOS 86.06.4F.00.08 | `PG106.kext` (OpenCore, firmware embedded) + `PG106FB.kext` (HDMI) and `PG106Accel.kext` (both /Library/Extensions), submodule: render offload like Linux PRIME, plus the HDMI port — see below |
| Audio | Realtek ALC255 (10ec:0255, sub 1025:118a); GP106 HDA 10de:10f1 (HDMI) | macOS 26 has no AppleHDA. Recommended: AppleHDA put back on the root volume + AppleALC layout-id 29, "dhinakg - Realtek ALC255 for Acer Predator G3-571", HDMI audio through NMHDA (option 2, `audio-applehda.sh`). Fallback: VoodooHDA 3.6.8 in /Library/Extensions, no HDMI audio (option 1, `audio-voodoohda.sh`). See "Audio on macOS 26" below |
| Ethernet | RTL8111 10ec:8168 | RealtekRTL8111 |
| Wi-Fi | Qualcomm Atheros QCA6174 168c:003e | `kexts/wifi/` (this repo): `Ath10kMac` + `AirportAth10k`, OpenCore-injected; country `IN` baked in at release (`Ath10kCountryCode`) |
| Bluetooth | Lite-On 04ca:3016 (QCA Rome), on xHCI USB 2 port 7 (HS07) | `kexts/bluetooth/` (this repo) + BlueToolFixup; `kexts/bluetooth/acpi/SSDT-HS07-UPC` marks HS07 internal (the DSDT has `_UPC` only on HS09). Works on macOS 26.7.2 (owner report); pairing, A2DP/HFP, sleep/wake to be tested |
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
  `PG106FB.kext` is its framebuffer. It must live in /Library/Extensions
  (IOGraphicsFamily is only in the System KC on 26.7.2, so OpenCore refuses
  it); it attaches under PG106 on 26.7.2, and a picture on an HDMI display
  has **not been reported yet**.
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
(`-wegnoegpu`) is not an option either. So the scripts below do OCLP's Modern Audio steps themselves. Two routes,
both reversible; use one, not both:

| | Option 2, `audio-applehda.sh` (recommended) | Option 1, `audio-voodoohda.sh` (fallback) |
|---|---|---|
| Driver | Apple's AppleHDA 600.2 + AppleALC layout 29 | VoodooHDA 3.6.8 |
| Sound | Apple's speaker processing from layout 29 (EQ, limiter, crossover) | raw signal; tuned gains (see "Option 1 sound") |
| HDMI audio | yes, NMHDA + PG106FB (monitor attached) | no |
| Changes | the root volume (a new, unsealed snapshot), a KDK in /Library/Developer/KDKs | one kext in /Library/Extensions |
| Needs | FileVault off, SIP 0x0A03, SecureBootModel Disabled, AMFIPass | SIP 0x1 |
| macOS update | removes it: run it again (the script is pinned to the build) | survives |

On 2026-11-08 option 1 was installed and worked, but the owner reported it sounds very bad. Option 2 is ready and has
not been run on the laptop yet.

### Option 2 (recommended): AppleHDA back on the root volume — `tools/macos/audio-applehda.sh`

Step for step what OCLP 3.0.0's sys_patch does for Modern Audio (sources: OCLP 3.0.0-rc.5,
`sys_patch/patchsets/hardware/misc/modern_audio.py`, `sys_patch.py`, `utilities/kdk_merge.py`, `utilities/files.py`,
`kernelcache/kernel_collection/boot_system.py`, `mount/snapshot.py`, `sys_patch_helpers.py`), and nothing else:

- **Payload:** PatcherSupportPkg 2.0.5 `Universal-Binaries.dmg` (sha256
  `1c290781bc726759ff37a110ffcb7182a6c6fd3d8ebce112d134d482c8de52a3`, the GitHub release asset; encrypted with OCLP's
  public passphrase). The copy inside OpenCore-Patcher.app is used when it has that hash, otherwise the asset is
  downloaded. Folder "26.0 Beta 1": AppleHDA.kext 600.2 (DTSDKBuild 25A5279g), 115 files, binary
  `6bf19c385a1212160be8a01fd7903aaa0416407e0b52e949f49d04cee4c65de7`, tree hash
  `5ec3ce4a26b71d1384f9acb604f94c144c0f5f0c275a00965094221ce28ffc04`. These pins come from the image itself, decrypted
  and read on Linux on 2026-11-08: all 7 binaries match their CodeDirectory page hashes and carry Apple's own
  signatures. The bundles have no `_CodeSignature` seal, which is why AMFIPass is needed.
- **KDK:** `Kernel_Debug_Kit_26.7.2_build_25G332.dmg` from dortania/KdkSupportPkg (the exact build OCLP picks; sha256
  `9490bd9b5e16ff5cee8b7330c498d64f27a008aa91c5c70d686a00dc5978c555`, 1181052767 bytes). Its `KernelDebugKit.pkg`
  must be Apple-signed (`pkgutil --check-signature`) before it is installed under /Library/Developer/KDKs.
- **Checks** (OCLP detect.py): build 25G332; SIP bits 0x1, 0x2 and 0x800 (1401 writes `csr-active-config` 030A0000);
  FileVault off; Apple Secure Boot off (SecureBootModel Disabled); AMFIPass >= 1.2.1 loaded (1401 ships 1.4.1 with
  `-amfipassbeta`); not already patched (OCLP: revert first); Lilu + AppleALC loaded; alc-layout-id 29; 12 GiB free
  (14 GiB when the KDK still has to be downloaded).
- **Install, in order:** VoodooHDA (option 1) removed and an auxiliary collection without it built and published
  (both drivers would claim HDEF otherwise); KDK installed if missing; the root volume mounted read-write at
  `/System/Volumes/Update/mnt1`; `rsync -r -i -a <KDK>/System/Library/Extensions/ mnt1/System/Library/Extensions`;
  AppleHDA.kext copied to `mnt1/System/Library/Extensions` (`chmod -Rf 755`, `chown -Rf root:wheel`, copy verified);
  OCLP's record `mnt1/System/Library/CoreServices/OpenCore-Legacy-Patcher.plist` ("Modern Audio" → Overwrite System
  Volume → AppleHDA.kext from "26.0 Beta 1"); `kmutil create --allow-missing-kdk --volume-root
  /System/Volumes/Update/mnt1 --update-all --variant-suffix release`; RSRRepair from the app's payloads.dmg (OCLP
  installs it after a full rebuild; `RSRREPAIR=0` skips it); the auxiliary collection rebuilt against the new
  boot/system collections (below); `bless --folder mnt1/System/Library/CoreServices --bootefi --create-snapshot`;
  then that auxiliary collection is published. If kmutil or bless fails, nothing is blessed and the next boot is the
  sealed snapshot as before.
- **PG106FB and PG106Accel after the rebuild:** they load from the auxiliary collection
  (`/Library/KernelCollections/AuxiliaryKernelExtensions.kc`, on the Data volume), and an auxiliary collection is
  linked against one pair of boot/system collections. `--update-all` on the mounted root makes new ones, so the old
  auxiliary collection would not fit them. OCLP's `clean_auxiliary_kc` says the same about its revert. The script
  therefore builds one for each snapshot, with the command form in the driver repo's `docs/MULTI-GPU.md` (verified on
  macOS 26):
  `kmutil create -n aux --volume-root / --allow-missing-kdk -a x86_64 -B <boot kc> -S <system kc> --kernel <kernel>
  --repository /Library/Extensions -p <each kext> -A <file> -z`. It checks each build with `kmutil inspect`. Both
  collections are kept in `/Library/Helios300/audio-applehda` (`aux-sealed.kc`, `aux-patched.kc`, with the list of
  kexts each was built from), and the one matching what boots is published. The replaced file is kept as
  `aux-previous.kc`, and the very first one as `aux-original.kc`. If PG106FB or PG106Accel is missing after a restart
  (`MODE=status` says so), `MODE=aux` rebuilds the collection against the running collections; restart. If that
  build fails, the fallback is the route of the first install: `sudo kmutil load -p
  /Library/Extensions/PG106FB.kext` (and PG106Accel), then restart. After the patched boot, also run
  `pascal/scripts/kc-preflight.sh` against copies of the new collections. They are rebuilt from the same 25G332
  kexts, so the class layouts should not change, but this checks it.
- **Revert:** `MODE=revert` runs `bless --mount /System/Volumes/Update/mnt1 --bootefi --last-sealed-snapshot` and
  publishes `aux-sealed.kc` again, but only if /Library/Extensions has not changed since it was built. If it has, run
  `MODE=aux` after the restart. AppleHDA, the merged KDK files and the record exist only in the patched snapshot. The
  KDK stays (`REMOVE_KDK=1` deletes it), and so does RSRRepair, as with OCLP. From Recovery (OpenCore picker → Space →
  Recovery → Utilities > Terminal; FileVault is off, so the volumes are mounted):
  `bless --mount "/Volumes/Macintosh HD" --bootefi --last-sealed-snapshot`, then
  `D=$(ls -d /Volumes/*Data | head -1); cp "$D/Library/Helios300/audio-applehda/aux-sealed.kc" "$D/Library/KernelCollections/AuxiliaryKernelExtensions.kc"`,
  `reboot`. The same lines are printed at the end of `MODE=revert`.
- **Caveats:** FileVault must stay off while patched. A macOS update boots a new sealed snapshot without AppleHDA, and
  macOS offers full-size updates (no deltas) while the seal is broken. After an update, the script's KDK pin
  (`KDK_BUILD`, `KDK_URL`, `KDK_SHA`, `KDK_SIZE`: the exact build from KdkSupportPkg) must be bumped before it runs
  again. It refuses any other build. Never let OCLP's own patcher (or its auto-patch login agent) apply its patch
  set.
- Run it as the logged-in user, from a copy of the script: `MODE=dry-run` (no change; downloads the KDK into
  `~/Library/Caches/helios300-audio` and test-builds the auxiliary collection), `MODE=install`, restart,
  `MODE=status`. Over SSH: `{ echo MODE=dry-run; cat tools/macos/audio-applehda.sh; } | ~/.config/helios300-ssh/hbmac`.
- Tested on Linux (2026-11-08) in a user-namespace sandbox with a fake macOS tree, the real payload image and
  macOS-tool stand-ins. It covered dry-run (nothing changed), install (step order, the auxiliary collection published
  for the new boot collection, VoodooHDA out), patched-boot status, `MODE=aux`, revert, and the stops (FileVault,
  SIP, old AMFIPass, no AppleALC, kmutil or aux build failing before bless). The real tools are untested until the
  laptop runs it.

### ALC255 layout 29

AppleALC's `Resources/ALC255/Info.plist` (master, which is 1.9.9; layout 29 has been unchanged since PR #531, 2020)
lists layout 29 as "dhinakg - Realtek ALC255 for Acer Predator G3-571", this exact laptop. It was built from layout 27
with Owod's microphone fix. None of the other Acer entries is a Predator G3 (A515-54G, A51x, A315-56, AN515-52,
A715-42G), so layout 29 stays; spec.json and the 1401 profile are unchanged. What it sets (PinConfigs ConfigData +
Platforms29):

- 0x14 internal speakers `0x90170110`, EAPD on; path 0x14 ← mixer 0x0C ← DAC 0x02.
- 0x21 headphone jack `0x04211020`; path 0x21 ← 0x0D ← DAC 0x03. AppleHDA mutes the speakers on jack sense (the
  headphone auto-switch).
- Internal microphone: the digital mic on 0x12 (path ADC 0x08 ← 0x23 ← 0x12; its pin keeps the BIOS default). 0x1B
  `0x90A00130` is configured but not on a path.
- Jack input 0x19 `0x04811020` (Line In, association 2 with the headphone); path ADC 0x08 ← 0x23 ← 0x19.
- Layout29.xml has `SignalProcessing`/`SoftwareDSP` blocks with EQ filters, a limiter and a crossover for the internal
  speakers. VoodooHDA has nothing like this, which is the main reason option 2 sounds better.

The PCI `layout-id` reads 7 by design: AppleALC reports 7 to the system and keeps the real one in `alc-layout-id`
(29).

### Headset microphone (combo jack)

Not known yet on this board: test it after option 2. Linux treats this Acer ALC255 board as a combo jack without mic
presence detection. Its pin quirk "Acer" (0x12 `0x90a601c0`, 0x14 `0x90171120`, 0x21 `0x02211030`) selects
`ALC255_FIXUP_ACER_MIC_NO_PRESENCE`: pin 0x19 becomes a headset mic `0x01a1913c`, and `alc_fixup_headset_mode_alc255`
switches the jack between headphone and headset (CTIA/OMTP) mode with codec coefficients. Layout 29 makes 0x19 a
Line In, and AppleHDA does none of the headset-mode switching, so a 4-pole headset may play fine while its mic stays
silent or hums.

Test: plug in a CTIA (iPhone-type) headset, then check System Settings → Sound → Input and the input meter. If there
is no headset input, or it is silent, try these verbs once by hand. They need AppleALC's verb interface (boot-arg
`alcverbs=1`) and `hda-verb` (from ALCPlugFix-Swift, not shipped or pinned here). The values are Linux's
`alc_headset_mode_mic_in` + `alc_headset_mode_ctia` for 0x10ec0255:

```
hda-verb 0x19 SET_PIN_WIDGET_CONTROL 0x24          # 0x19 input with 80% mic bias
hda-verb 0x20 SET_COEF_INDEX 0x06; hda-verb 0x20 SET_PROC_COEF 0x6100   # MIC2 Vref gate normal
hda-verb 0x20 SET_COEF_INDEX 0x45; hda-verb 0x20 SET_PROC_COEF 0xd489   # CTIA headset mode (OMTP: 0xe489)
hda-verb 0x20 SET_COEF_INDEX 0x1b; hda-verb 0x20 SET_PROC_COEF 0x0c2b
hda-verb 0x57 SET_COEF_INDEX 0x03; hda-verb 0x57 SET_PROC_COEF 0x8ea6
```

If that brings the mic, it has to be made persistent (on every plug, boot and wake), the job ALCPlugFix does. That
is a separate decision for the owner (ALCPlugFix-Swift as a LaunchDaemon with these verbs). Nothing is installed
for it now. With option 1, `NODES=1` makes 0x19 a Mic In (`0x04A1102F`), and VoodooHDA gives Mic In pins their bias
itself.

### HDMI audio (option 2 only): NMHDA + PG106FB

The chain, with what was checked against AppleHDA 600.2 from the payload (Linux, 2026-11-08):

1. The GP106 HDA function 01:00.1 is visible from boot (`SSDT-PG106-HDAU`; seen on 26.7.2 as `pci10de,10f1`).
2. NMHDA (OpenCore, after Lilu and AppleALC) hooks `AppleHDAController::probe`/`start` and
   `IOHDACodecDevice::executeVerb`, and rewrites the Kepler entries of AppleHDAController's device table for each
   NVIDIA audio function. All three symbols and the Kepler IDs 10de:0e0a, 0e0b, 0e1b, 0e1a and 0e08 are present in
   AppleHDA 600.2's AppleHDAController/IOHDAFamily, so NMHDA's targets exist. AppleHDA's `AppleHDAHDMI_DPDriver`
   personality drives the HDMI codec (10de:0084).
3. The pairing: `hda-gfx` = `onboard-2` on both 01:00.0 and 01:00.1 (spec.json DeviceProperties; WhateverGreen gives
   the HD 630 `onboard-1`). PG106FB copies it onto itself and adds `audio-codec-info` `00 01 07 00` (codec 0, AFG 1,
   pin NID 4 + SOR 3), `connector-type` 0x800 and `av-signal-type` 8. After a modeset it writes the monitor's ELD to
   the HDA codec and answers `kConnectionEnableAudio`/`kConnectionAudioStreaming`.
4. The HDMI output appears in System Settings → Sound → Output with a monitor attached and lit by PG106FB.

Not run yet: PG106FB's modeset and ELD path have never run on the Mac, and NMHDA has never run with AppleHDA. PG106FB
loads late (auxiliary collection), after AppleHDA. If no HDMI output appears, unplug and replug the cable once, then
check `MODE=status`: `debug.nmhda`, the AppleHDAController instances, PG106FB's `hda-gfx`/`audio-codec-info`, the HDAU
`hda-gfx`, and the PG106FB log line `audio: AppleHDA pairing published`.

### Option 1 (fallback): VoodooHDA, no root-volume change — `tools/macos/audio-voodoohda.sh`

- CloverHackyColor/VoodooHDA GitHub release `Release368` (2026-09-18): VoodooHDA 3.6.8, macOS 26.5 SDK, built on
  25G83, ad-hoc signed. `VoodooHDA.kext-368.zip` sha256 `9aaa01b245f4827b2de5edfdc028700b9bc8823f03bfb1daf280b8571f88adb0`;
  binary `932a7c9e46a65fdab7c8729f599c1094b568ae4fc7f17dad9818c83e833a0973`, Info.plist
  `21b886b5449b9a0739422b6720ea2bc04fdcc477f79e1e3cece021033a51bf1a` (source: commit 8c7e8e2). Not
  chris1111/VoodooHDA-Tahoe: it packages VoodooHDA 3.0.3 (2025), without the AppleALC pin tables. Not v3.7.1-alpha:
  its OpenCore-injectable `VoodooHDA_BL` compiles Apple's IOAudioFamily classes in under their own names
  (IOAudioDevice, IOAudioEngine, ...), which would collide with the real IOAudioFamily the moment AppleUSBAudio,
  AppleGFXHDA or AudioAUUC load it (USB headsets).
- It links IOAudioFamily (740.1, compat 1.0, only in SystemKernelExtensions.kc on 25G332) and IOGraphicsFamily
  (600, ditto), so OpenCore cannot inject it (same as PG106FB): /Library/Extensions, root:wheel 755, then the
  auxiliary collection rebuilt with it (same `kmutil create -n aux` form as option 2; `kmutil load -p` as the
  fallback), restart. 1401 builds `csr-active-config` 030A0000: 0x1 (untrusted kexts) and 0x200 (no approval
  prompt); the script checks. The prefPane is optional and not installed.
- Keep AppleALC: VoodooHDA 3.6.8 matches `IONameMatch` HDAS, which stays in the device's `compatible` list after
  AppleALC renames it HDEF, and it takes its pin table from `layout-id`. With AppleALC loaded that is 7, which has no
  ALC255 entry, so VoodooHDA reads the codec's BIOS pin defaults. Without AppleALC it would be the injected 29, and
  layout 29 puts the headphone jack (0x21, out) and the jack input (0x19, in) in the same association 2: VoodooHDA
  disables an association with mixed directions ("wrong direction"), i.e. no 3.5 mm output (the symptom in
  CloverHackyColor/VoodooHDA#21, whose own cause was AMD controller matching).
- If headphones or the microphone do not work with the BIOS pins, `NODES=1` writes this ALC255 pin set into the
  kext's `NodesToPatch` (applied after the layout/BIOS pins, before associations are parsed): 0x14 speaker
  `0x90170110` (assoc 1 seq 0), 0x21 headphone `0x0421101F` (assoc 1 seq 15: speakers muted while headphones are
  in), 0x12 internal mic `0x90A60120` (assoc 2 seq 0; node 18 is the internal-mic path in AppleALC's Platforms29),
  0x19 mic jack `0x04A1102F` (assoc 2 seq 15). Pin values from layout 29's ConfigData, re-associated.
- HDMI audio: no. VoodooHDA 3.6.8's personalities match HDAS and AMD (1002:aaxx) HDMI functions only, not the
  GP106's 10de:10f1, and NMHDA needs AppleHDAController.
- Revert: `MODE=revert` removes the kext and rebuilds the auxiliary collection without it (PG106FB and PG106Accel
  stay), restart. If the system does not come up: safe mode from the OpenCore picker (Shift+Enter; /Library/Extensions
  kexts are not loaded), then `MODE=revert`.

#### Option 1 sound

Why the stock 3.6.8 sounds bad here, from its source (Release368, commit 8c7e8e2), most likely cause first:

1. **No speaker processing.** AppleHDA with layout 29 runs Apple's DSP chain on the internal speakers (EQ, limiter,
   crossover). VoodooHDA sends the raw signal, so the small speakers sound thin and harsh, and loud passages distort.
   Nothing in VoodooHDA can fix this; option 2 does. A system-wide software EQ is the only partial workaround.
2. **No headroom.** The ALC255's only output gain stage is the DAC amp (NID 0x02, 0.75 dB steps, 0 dB at the top).
   VoodooHDA sets it to Main × PCM (`audioCtlOssMixerSet`), and the release's PCM 95 leaves it near 0 dB at full
   volume.
3. **Cached stream DMA.** The stream buffers are mapped cached unless `InhibitCache` (`allocateDmaMemory`); the
   release turns it on only for its HDMI personality. Uncached buffers are the usual cure for crackle and static.
4. **Recording level.** `Rec` 95 makes the digital mic hiss and clip; the driver's own table default is 75.

`TUNE=1` (the default for install) writes PCM 90 (~4 dB more headroom; `VHDA_PCM`), Rec 75 (`VHDA_REC`) and
InhibitCache true (`VHDA_INHIBIT`) into the analog personality, then re-signs ad hoc. The binary stays the pinned one.
Left alone on purpose: Boost 1 (a ×1 multiply, no effect), Noise 0 (no noise gate), Vectorize, iMix 95 (the playback
mixer path, must not be 0), iGain 0 (analog loopback muted), and the "volume fix" flags (they only reshape the slider
curve). VoodooHDA reads these values once at load, so a change means `MODE=install` with the new values, then a
restart. `TUNE=0 MODE=install` puts the release's Info.plist back (its sha256 is pinned), and `MODE=status` shows
which one is installed. In Audio MIDI Setup, pick the highest bit depth at 48 kHz; the default can be 16-bit.
