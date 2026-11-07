# Release contract: `manifest.json`

A macos_helios300 release is a set of files plus one `manifest.json` that says what each file is and how it goes into an
OpenCore EFI for the **Acer Predator Helios 300 G3-571** on **macOS 26**. Installers (1401's Helios profile, or a person
editing `config.plist` by hand) apply the manifest literally and check every file against it. Nothing else about a
release is a contract.

- Schema: [`tools/release/manifest.schema.json`](../tools/release/manifest.schema.json) (JSON Schema 2020-12;
  `tools/release/schema_check.py` checks it without third-party modules).
- Static half: [`tools/release/spec.json`](../tools/release/spec.json). Edit this to change what a release contains.
- Built by: [`tools/release.sh`](../tools/release.sh), which writes `dist/` (git-ignored) and **publishes nothing**.

## Trust chain

1. The consumer pins the release tag and the **SHA-256 of `manifest.json`**, e.g. 1401's
   `p1401/helios300_release.json`.
2. `manifest.json` lists each file's `sha256` and `size`, and its `url` under
   `https://github.com/akshaynexus/macos_helios300/releases/download/v<version>/`.
3. For a zipped bundle, `contents_sha256` lists the SHA-256 of every file inside it. The consumer checks the
   extracted bundle again, so a zip with an extra or changed file is refused even if someone recomputed the zip's hash.

`SHA256SUMS.txt` in the release lists the same hashes, plus the manifest's own, for people checking by hand.

## Top level

| key | meaning |
|---|---|
| `schema` | `1`. A consumer refuses any other value. |
| `repo` | `akshaynexus/macos_helios300` |
| `release` | `version` (semver), `tag` (`v<version>`), `commit` (this repo), `sources.driver` (nvidia-macos-driver repo + commit), `built` (UTC), `built_on`, `pre_release`, `dirty` (always false, since builds come from `git archive`), `dry_run`, `missing` (artifacts a dry run could not build) |
| `machine.match` | `dmi_product` (`Predator G3-571`), `dmi_board` (`Sienta_KLS`), and `pci` / `usb` / `hda_codecs` IDs (lower-case hex, `required` true/false). Required: the GP106M `10de:1c20` with Acer subsystem `1025:118b`, and the HD 630 `8086:591b` (`1025:118a`). Reported but optional: QCA6174 `168c:003e` (`11ad:0807`), Bluetooth `04ca:3016`, ALC255 `10ec:0255` (`1025:118a`). |
| `macos` | `min_kernel` `25.0.0`, `max_kernel` `25.99.99`. The release targets macOS 26 Tahoe only; a consumer adds nothing for another macOS. |
| `artifacts[]` | one entry per release file, see below |
| `external_kexts[]` | kexts the release relies on but does not ship, with a minimum version: Lilu ≥ 1.6.0, AppleALC ≥ 1.9.0, BlueToolFixup from BrcmPatchRAM **≥ 2.7.2** (its macOS 26 bluetoothd patches; exactly one copy) |
| `config` | what the EFI needs besides the files, see below |
| `post_install[]` | steps after macOS is installed (`step`, `what`, `artifacts`) |

## `artifacts[]`

| key | meaning |
|---|---|
| `name` | bundle or file name (`PG106.kext`, `SSDT-PG106-HDAU.aml`, `NVK-Pascal`) |
| `kind` | `kext` · `ssdt` · `dylib` · `bundle` · `app` · `firmware-licence` |
| `file`, `sha256`, `size`, `url` | the release file. A directory is shipped as `<name>.zip`, deterministic (sorted entries, 1980-01-01 timestamps, 0755/0644 modes), with the bundle at the top: `PG106.kext/Contents/...` |
| `contents_sha256` | `{path inside the bundle: sha256}` for zips |
| `version`, `bundle_id` | from the built `Info.plist` (`CFBundleShortVersionString`, `CFBundleIdentifier`). For plain files, the release version. |
| `install` | `opencore` (EFI/OC), `library-extensions` (AuxKC, after install), `gpu-bundles` (`/Library/GPUBundles`), `applications`, `stick-only` (licences) |
| `install_path` | the destination for the non-OpenCore kinds |
| `opencore` | for kexts: the exact `Kernel → Add` entry, `BundlePath`, `ExecutablePath`, `PlistPath`, `Arch` (`x86_64`), `MinKernel`, `MaxKernel`, `Enabled`, `Comment`, plus `order`. Consumers insert entries **in `order`, after every kext they `require`**. |
| `acpi` | for SSDTs: the `ACPI → Add` entry (`Path`, `Enabled`, `Comment`, `order`) |
| `requires` | names that must load first: other artifacts or `external_kexts`. A consumer that cannot satisfy one leaves the artifact out and says so. |
| `conflicts` | kexts that must not be enabled together with this one (e.g. `itlwm.kext` for Ath10kMac) |
| `hardware` | the device(s) it is for. Added only when the scan shows one. |
| `boot_args_optional` | boot-args the artifact understands. **Never added by default**; documented for rollback and diagnostics. |
| `licence`, `licence_files`, `contains_firmware`, `status` | what applies to the file, and how far it has been tested |
| `source`, `built_by`, `signed` | tree (`self` / `driver`) and path; `cross` / `iasl` / `copy`; `ad-hoc (rcodesign)` or `unsigned` |
| `optional` | may be absent from a release (listed under `release.missing`) |

### Artifacts and OpenCore order (spec.json)

| order | artifact | install | MinKernel..MaxKernel | requires |
|---|---|---|---|---|
| 100 | `PG106.kext`, GTX 1060 Mobile (GP106M), with 17 NVIDIA firmware files in `Resources` | opencore | 25.0.0.. | none |
| 110 | `PG106FB.kext`, the HDMI framebuffer | opencore | 25.0.0.. | PG106.kext |
| 120 | `NMHDA.kext`, GP106 HDMI audio (Lilu plugin) | opencore | 24.0.0.. | Lilu, AppleALC |
| 130 | `QCARomePatchRAM.kext`, QCA Rome Bluetooth firmware | opencore | 21.0.0.. | Lilu, BlueToolFixup |
| 140 | `Ath10kMac.kext`, QCA6174 core (firmware + Acer board data) | opencore | 25.0.0..25.99.99 | none |
| 145 | `AirportAth10k.kext`, native Wi-Fi front end (IO80211/Skywalk) | opencore | 25.0.0..25.99.99 | Ath10kMac |
| ACPI 100 | `SSDT-PG106-HDAU.aml`, GP106 HDA function 01:00.1 visible from boot | opencore | | |
| ACPI 110 | `SSDT-HS07-UPC.aml`, internal Bluetooth port HS07 (`_UPC` type 0xFF) | opencore | | |
| | `PG106Accel.kext` (links IOAcceleratorFamily2, so OpenCore cannot inject it) | library-extensions | | PG106 |
| | `NVMTLDriver.bundle`, Metal plugin (optional) | gpu-bundles | | PG106Accel, translator, NVK |
| | `libnvmtl_translate.dylib`, AIR→SPIR-V (optional) | gpu-bundles `nvmtl/` | | |
| | `NVK-Pascal`: `libvulkan_nouveau.dylib` + `nvk_icd.json` (optional) | gpu-bundles `nvmtl/` | | |
| | `NVIDIA-firmware-LICENCE.txt`, `Mesa-license.rst` | stick-only | | |

## `config`

Values that are not JSON types are written `{"data": "<hex>"}`, `{"string": …}`, `{"integer": …}` or `{"boolean": …}`.

| key | release 0.1 content |
|---|---|
| `boot_args` | none required |
| `nvram_add` / `nvram_delete` | `7C436110-AB2A-4BBB-A880-FE41995C9F82`: `bluetoothExternalDongleFailed` = `00`, `bluetoothInternalControllerInfo` = 14 zero bytes (BrcmPatchRAM ≥ 2.7.0). Both are also listed under Delete so they get written. |
| `device_properties` | `hda-gfx` = `onboard-2` on `PciRoot(0x0)/Pci(0x1,0x0)/Pci(0x0,0x0)` (GP106) and `/Pci(0x0,0x1)` (its HDA). WhateverGreen gives the HD 630 `onboard-1`. |
| `usb_ports` | informational: HS07 internal (255) for the Bluetooth. `SSDT-HS07-UPC.aml` implements it, since the DSDT has a `_UPC` only on HS09. A full UTBMap needs a USBToolBox mapping run on the machine. |
| `forbidden` | kexts `NVRM` `NVRMFB` `NVAccel` `NVRMAGDC` (the Turing+ NullMoth stack) and the other Bluetooth stacks; boot-arg `-wegnoegpu`; `disable-gpu` on the GP106 path; `SSDT-Disable_GPU_PEG0.aml`. PG106 drives the GTX 1060, so nothing may hide or power it off. |

## How a release is built (`tools/release.sh`)

```sh
tools/release.sh --version 0.1.0              # real: driver at the submodule commit, every required artifact must build
tools/release.sh --version 0.1.0 --dry-run \
    --driver-repo ../nvidia-macos-driver --driver-rev origin/pascal-gp106   # try a newer driver commit
```

- Sources: `git archive` of this repo at `--rev` (default `HEAD`), and of nvidia-macos-driver at the commit the
  `kexts/nvidia-macos-driver` submodule records (a real release refuses any other driver commit; bump the submodule
  first). Uncommitted changes are never in a release.
- Toolchain, all on Linux: clang/LLVM 23, the macOS 26.5 SDK (xtool), Apple ld64-956.6 (cctools-port), ld64.lld,
  xnu-12377.1.9 export lists, rustup `x86_64-apple-darwin`, iasl, rcodesign (apple-codesign 0.29). A GitHub runner
  has no macOS SDK, so the script runs on the owner's machine. No workflow is provided.
- Pinned downloads (cached in `~/.cache/helios300-release`, SHA-256 or commit checked): Lilu 1.7.2 DEBUG (its SDK,
  for NMHDA), MacKernelSDK `3f750085`, Vulkan-Headers v1.4.363 (`6802bb47`), LLVM compiler-rt `cpu_model/x86.c`
  at llvmorg-23.1.4, open-gpu-kernel-modules at the driver's submodule commit (NVK's NVRM headers), linux-firmware
  for the Wi-Fi and Bluetooth firmware (each kext's own pinned fetch), Mesa `17ca6174` (NVK's prepare-mesa.sh).

| artifact | how it is built on Linux | checks |
|---|---|---|
| PG106, PG106FB | `pascal/kext/scripts/cross-build-macos.sh`, `pascal/fb/scripts/cross-build-macos.sh` (clang `-mkernel`, ld64 `-kext`) | MH_KEXT_BUNDLE, `_kmod_info`, KPI audit; all firmware staged (0 missing) |
| PG106Accel | `kexts/PG106Accel/cross-build.sh` | host tests, KPI + IOAcceleratorFamily2 audit, vtable slots. Needs the macOS 26 IOAcceleratorFamily2 reference binary, which is not in git (`--iaf2`) |
| NMHDA | `tools/release/cross-nmhda.sh`: Lilu SDK + MacKernelSDK, ld64 `-kext` with the SDK's libkmod. No `cc_kext` builtins are needed (the link would fail). | KPI audit incl. every Lilu export |
| QCARomePatchRAM, Ath10kMac + AirportAth10k | their own `scripts/cross-build.sh` (`tools/cross-build-kext.sh`). Only the native Wi-Fi pair ships; the Ethernet-style core and its app are developer-only. | KPI audit, symbol checks |
| SSDTs | `iasl -vs -ve` | 0 errors |
| NVMTLDriver.bundle | `tools/release/cross-plugin.sh`: clang `-bundle -fobjc-arc`, minos 15.0, `-Werror=unguarded-availability-new`, **ld64.lld** (cctools ld64-956.6 aborts on this object with a libstdc++ assertion), compiler-rt `cpu_model` for `__builtin_cpu_supports`. `nvconv.metal` is committed precompiled (`nvconv_lib.h`), so no Metal compiler is needed. | macho-check.py: every bind exported by an SDK `.tbd`, 0 strong lookups |
| libnvmtl_translate.dylib | `tools/release/cross-xlate.sh`: `cargo build --locked --target x86_64-apple-darwin`, clang + lld linker, `@rpath` install name, paths remapped, built from a fixed path | macho-check.py, no build-machine paths |
| NVK-Pascal | `nvk/pascal/scripts/cross-build-nvk-macos.sh` (Mesa + NAK; `build-nvk-linux.sh` makes the build-machine tools) | its own macho check |

Ad-hoc signatures (`rcodesign sign`, what `codesign -s -` does): PG106Accel, the plugin, the translator, NVK. Kexts
injected by OpenCore need none.

**Everything is cross-built and nothing has run on a Mac.** Before a release that is not marked pre-release, do one
Xcode verification build of each artifact on a Mac and compare it with the cross-built one. `pascal/kext/scripts/build-macos.sh`,
`kexts/PG106Accel/build.sh`, `kexts/NMHDA/build.sh`, the driver repo's `build/build_plugin_release.sh` and
`build/build_xlate_release.sh`, and `nvk/pascal/scripts/build-nvk-macos.sh` are the Mac builds. Every release is
`pre_release: true` until then.

## Who builds what

Only this repo builds. `tools/release.sh` produces every file an EFI for this laptop needs from this project, plus
`manifest.json`, and those are what a GitHub (pre-)release carries. Consumers assemble; they never build:

- **Prebuilt (the default).** 1401 downloads `manifest.json` from the release assets (the tag pinned in its
  `p1401/helios300_release.json`, else the latest release, pre-releases included), downloads every listed file,
  checks each SHA-256 (and `contents_sha256` inside bundles), and unpacks them into the EFI in the `opencore.order`.
  It needs no SDK or toolchain. `--release-dir DIR` / `--manifest PATH` points it at a local copy (offline, or before
  a release is published), and downloads are cached for offline rebuilds.
- **Build from source (opt-in, `1401 build ... --build-from-source [--ref <tag|commit>]`).** 1401 clones this repo at
  the ref, runs `git submodule update --init`, and runs *this* `tools/release.sh --version <tag version or
  0.0.0-src.<commit>> --dist <dir>`. The resulting `dist/manifest.json` goes through the same checks and assembly as
  a download. release.sh exits 77 when the cross toolchain is missing, and 1401 then points to the section above.
  The PG106Accel audit needs the macOS 26 IOAcceleratorFamily2 reference binary, which cannot be in git: set
  `HELIOS_IAF2=<file>` (release.sh reads it as `--iaf2`), or that required artifact fails and so does the build.

## Publishing (coordinator, after review)

`tools/release.sh` stops at `dist/`. To publish: review `dist/manifest.json` and `dist/logs/`, then create release
`v<version>` (pre-release) with every file in `dist/` except `logs/` uploaded under its `file` name, and set the
tag + manifest SHA-256 in 1401's `p1401/helios300_release.json`.
