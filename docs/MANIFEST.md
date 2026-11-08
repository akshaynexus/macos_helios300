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
| `release` | `version` (semver), `tag` (`v<version>`), `commit` (this repo), `sources.driver` (nvidia-macos-driver repo + commit), `built` (UTC), `built_on`, `pre_release`, `dirty` (always false, since builds come from `git archive`), `dry_run`, `missing` (artifacts a dry run could not build), `kc_audit` (the real-collection gate, see below: `status` `pass` / `fail` (dry runs only) / `skipped`, `kc_dir`, the SHA-256 of each collection, one `results[]` row per kext with the collections it was checked against) |
| `machine.match` | `dmi_product` (`Predator G3-571`), `dmi_board` (`Sienta_KLS`), and `pci` / `usb` / `hda_codecs` IDs (lower-case hex, `required` true/false). Required: the GP106M `10de:1c20` with Acer subsystem `1025:118b`, and the HD 630 `8086:591b` (`1025:118a`). Reported but optional: QCA6174 `168c:003e` (`11ad:0807`), Bluetooth `04ca:3016`, ALC255 `10ec:0255` (`1025:118a`). |
| `macos` | `min_kernel` `25.0.0`, `max_kernel` `25.99.99`. The release targets macOS 26 Tahoe only; a consumer adds nothing for another macOS. |
| `artifacts[]` | one entry per release file, see below |
| `external_kexts[]` | kexts the release relies on but does not ship, with a minimum version: Lilu ≥ 1.6.0, AppleALC ≥ 1.9.0, BlueToolFixup from BrcmPatchRAM **≥ 2.7.2** (its macOS 26 bluetoothd patches; exactly one copy) |
| `config` | what the EFI needs besides the files, see below |
| `post_install[]` | steps after macOS is installed, see below |

## `artifacts[]`

| key | meaning |
|---|---|
| `name` | bundle or file name (`PG106.kext`, `SSDT-PG106-HDAU.aml`, `NVK-Pascal`) |
| `kind` | `kext` · `ssdt` · `dylib` · `bundle` · `app` · `firmware-licence` · `script` (a post-install shell script, run on the Mac) · `doc` (read on the Mac: `POST-INSTALL.md`) |
| `file`, `sha256`, `size`, `url` | the release file. A directory is shipped as `<name>.zip`, deterministic (sorted entries, 1980-01-01 timestamps, 0755/0644 modes), with the bundle at the top: `PG106.kext/Contents/...` |
| `contents_sha256` | `{path inside the bundle: sha256}` for zips |
| `version`, `bundle_id` | from the built `Info.plist` (`CFBundleShortVersionString`, `CFBundleIdentifier`). For plain files, the release version. |
| `install` | `opencore` (EFI/OC), `library-extensions` (AuxKC, after install), `gpu-bundles` (`/Library/GPUBundles`), `applications`, `stick-only` (licences, scripts, `POST-INSTALL.md`) |
| `install_path` | the destination for the non-OpenCore kinds |
| `opencore` | for kexts: the exact `Kernel → Add` entry, `BundlePath`, `ExecutablePath`, `PlistPath`, `Arch` (`x86_64`), `MinKernel`, `MaxKernel`, `Enabled`, `Comment`, plus `order`. Consumers insert entries **in `order`, after every kext they `require`**. |
| `acpi` | for SSDTs: the `ACPI → Add` entry (`Path`, `Enabled`, `Comment`, `order`) |
| `requires` | names that must load first: other artifacts or `external_kexts`. A consumer that cannot satisfy one leaves the artifact out and says so. |
| `conflicts` | kexts that must not be enabled together with this one (e.g. `itlwm.kext` for Ath10kMac) |
| `hardware` | the device(s) it is for. Added only when the scan shows one. |
| `boot_args_optional` | boot-args the artifact understands. **Never added by default**; documented for rollback and diagnostics. |
| `licence`, `licence_files`, `contains_firmware`, `status` | what applies to the file, and how far it has been tested |
| `source`, `built_by`, `signed` | tree (`self` / `driver`) and path; `cross` / `iasl` / `copy` / `generated` (written by release.sh from the spec: `POST-INSTALL.md`); `ad-hoc (rcodesign)` or `unsigned` |
| `optional` | may be absent from a release (listed under `release.missing`) |
| `third_party` | code from elsewhere inside a bundle: `path` in the bundle, `what`, `licence`, `notices` (licence files in the bundle; make_manifest refuses the bundle if any is absent), `source_url`, `source_sha256` |
| `personality` | values baked into the built kext's `IOKitPersonalities` at release time, e.g. Ath10kMac `{"Ath10kCountryCode": {"string": "IN"}}` (`release.sh --wifi-country`) |

### Artifacts and OpenCore order (spec.json)

| order | artifact | install | MinKernel..MaxKernel | requires |
|---|---|---|---|---|
| 100 | `PG106.kext`, GTX 1060 Mobile (GP106M); its 17 NVIDIA firmware files are built **into the binary** (an injected kext has no bundle on disk for `OSKextRequestResource`; the copies in `Resources` are only a fallback for a `/Library/Extensions` install) | opencore | 25.0.0.. | none |
| 120 | `NMHDA.kext`, GP106 HDMI audio (Lilu plugin) | opencore | 24.0.0.. | Lilu, AppleALC |
| 130 | `QCARomePatchRAM.kext`, QCA Rome Bluetooth firmware | opencore | 21.0.0.. | Lilu, BlueToolFixup |
| 140 | `Ath10kMac.kext`, QCA6174 core (firmware + Acer board data) | opencore | 25.0.0..25.99.99 | none |
| 145 | `AirportAth10k.kext`, native Wi-Fi front end (IO80211/Skywalk) | opencore | 25.0.0..25.99.99 | Ath10kMac |
| ACPI 100 | `SSDT-PG106-HDAU.aml`, GP106 HDA function 01:00.1 visible from boot | opencore | | |
| ACPI 110 | `SSDT-HS07-UPC.aml`, internal Bluetooth port HS07 (`_UPC` type 0xFF) | opencore | | |
| | `PG106FB.kext`, the HDMI framebuffer (links IOGraphicsFamily, only in the System KC on 26.7.2: OpenCore refuses it, "Dependency com.apple.iokit.IOGraphicsFamily was not found") | library-extensions | | PG106 |
| | `PG106Accel.kext` (links IOAcceleratorFamily2, only in the System KC, so OpenCore cannot inject it) | library-extensions | | PG106 |
| | `NVMTLDriver.bundle`, Metal plugin (optional), with its AIR reader `Contents/MacOS/air-runtime/libLLVM.dylib` (upstream LLVM 23.1.4, see `third_party`) | gpu-bundles | | PG106Accel, translator, NVK |
| | `libnvmtl_translate.dylib`, AIR→SPIR-V (optional) | gpu-bundles `nvmtl/` | | |
| | `NVK-Pascal`: `libvulkan_nouveau.dylib` + `nvk_icd.json` (optional) | gpu-bundles `nvmtl/` | | |
| | `NVIDIA-firmware-LICENCE.txt`, `Mesa-license.rst` | stick-only | | |
| | `audio-voodoohda.sh`, `audio-applehda.sh`, `install-gpu.sh`, from `tools/macos/`; `gpu-apps.sh` from the driver's `tools/macos/` | stick-only (`EFI/Helios300`) | | |
| | `POST-INSTALL.md`: every `post_install` step in order with its commands, and every file on the stick (`tools/release/post_install_doc.py`) | stick-only (`EFI/Helios300`) | | |

## `post_install[]`

What to do on the Mac once macOS 26 runs, in `order`. A consumer copies every non-OpenCore artifact and
`manifest.json` to `EFI/Helios300` on the stick and lists the steps in its README; it never runs them.

| key | meaning |
|---|---|
| `step`, `order`, `what` | id, position, and what the step does in words (enough to do it by hand) |
| `artifacts` | release files the step uses; every name is a spec artifact |
| `script` | the artifact that does it (one of `artifacts`) |
| `run` | the exact command lines: `check` (dry run, changes nothing), `install`, `revert`, `status`. The scripts call `sudo` themselves; run them as the logged-in user |
| `cwd` | where to run it when the script needs the files next to it |
| `group`, `choice` | alternatives: do exactly one step of a group. `audio`: choice 2 `audio-applehda` (AppleHDA on the root volume, AppleALC layout 29, HDMI audio through NMHDA; recommended, listed first), choice 1 `audio-voodoohda` (VoodooHDA in /Library/Extensions, reversible, no speaker EQ, no HDMI audio; the fallback) |
| `recommended` | `true` on the step of a group to take unless it cannot be used (`audio-applehda`) |
| `reboot` | restart after `install` / `revert` |
| `needs` | what must be true first (the script checks it and stops otherwise), e.g. FileVault off for option 2 |
| `rerun_after_macos_update` | a macOS update undoes it (option 2: the update boots a new sealed snapshot without AppleHDA) |
| `notes` | anything else the person needs: limits, the way back |

The 0.1.1 steps: `gpu` (`install-gpu.sh`: PG106FB + PG106Accel into /Library/Extensions, the Metal plugin + NVK into
/Library/GPUBundles with the per-app allow list `/Library/GPUBundles/nvmtl-allow.txt`; every zip and its inner files
checked against `manifest.json` before anything is unzipped), then **one** of `audio-applehda` (recommended) / `audio-voodoohda`.

## `config`

Values that are not JSON types are written `{"data": "<hex>"}`, `{"string": …}`, `{"integer": …}` or `{"boolean": …}`.

| key | release 0.1 content |
|---|---|
| `boot_args` | none required |
| `nvram_add` / `nvram_delete` | `7C436110-AB2A-4BBB-A880-FE41995C9F82`: `bluetoothExternalDongleFailed` = `00`, `bluetoothInternalControllerInfo` = 14 zero bytes (BrcmPatchRAM ≥ 2.7.0). Both are also listed under Delete so they get written. |
| `device_properties` | `hda-gfx` = `onboard-2` on `PciRoot(0x0)/Pci(0x1,0x0)/Pci(0x0,0x0)` (GP106) and `/Pci(0x0,0x1)` (its HDA). WhateverGreen gives the HD 630 `onboard-1`. `applbkl` = `01000000` on `PciRoot(0x0)/Pci(0x2,0x0)` (HD 630): WhateverGreen skips its AppleBacklight panel patch while a discrete GPU is visible, so without it there is no brightness slider (docs/HARDWARE.md "Brightness"; 1401 4b9a322 writes the same). |
| `usb_ports` | informational: HS07 internal (255) for the Bluetooth. `SSDT-HS07-UPC.aml` implements it, since the DSDT has a `_UPC` only on HS09. A full UTBMap needs a USBToolBox mapping run on the machine. |
| `forbidden` | kexts `NVRM` `NVRMFB` `NVAccel` `NVRMAGDC` (the Turing+ NullMoth stack) and the other Bluetooth stacks; boot-arg `-wegnoegpu`; `disable-gpu` on the GP106 path; `SSDT-Disable_GPU_PEG0.aml`. PG106 drives the GTX 1060, so nothing may hide or power it off. |

## How a release is built (`tools/release.sh`)

```sh
tools/release.sh --version 0.1.1 --real-kc ~/pascal-build/macos-26.7.2-25G332   # real: submodule driver, every gate
tools/release.sh --version 0.1.1 --dry-run --real-kc ~/pascal-build/macos-26.7.2-25G332 \
    --driver-repo ../nvidia-macos-driver --driver-rev origin/pascal-gp106          # try a newer driver commit
```

- **Release gates.** A real release stops (no manifest) when any of these fails; a dry run records it.
  - PG106's firmware: `tools/release/check-pg106-firmware.py` checks that each of the 17 files pinned in the
    driver's `pascal/kext/firmware.sha256` is byte for byte in `Contents/MacOS/PG106`, together with its table row
    (the pinned SHA-256 string), and that `_pg106_fw_embedded_count` in the binary is 17. Staging the files into
    `Resources` is not enough: an OpenCore-injected kext cannot read them (macOS 26.7.2, first boot).
  - Real kernel collections (`--real-kc DIR`, or `$HELIOS_KC_DIR`): `DIR` holds `BootKernelExtensions.kc` and
    `SystemKernelExtensions.kc` copied from the target Mac (Apple binaries: local only, never committed or
    uploaded; only their SHA-256 goes into the manifest). `tools/release/kc_gate.py` runs `tools/kc-audit.py` for
    Ath10kMac, AirportAth10k (with Ath10kMac as sibling) and QCARomePatchRAM against the **Boot** collection
    (OpenCore injects into it, so a dependency that only lives in the System KC must fail here), and the driver's
    `pascal/scripts/kc-preflight.sh` for PG106 (Boot) and PG106FB + PG106Accel (Boot + System, the auxiliary KC
    links against both): every import, every Apple parent class size, every vtable slot. Output:
    `dist/logs/kc-audit.log`; result: `release.kc_audit`. A driver commit without `kc-preflight.sh` fails the gate.
    NMHDA (a Lilu plugin) is not audited. A real release without `--real-kc` is refused unless `--no-kc-audit`
    (or `HELIOS_KC_AUDIT=skip`, which 1401 `--build-from-source` sets when `HELIOS_KC_DIR` is unset); the manifest
    then says `kc_audit.status` `skipped`.
- **Wi-Fi country** (`--wifi-country CC`, default `IN`, the owner's choice; `$HELIOS_WIFI_COUNTRY`):
  `tools/release/set-wifi-country.py` writes `Ath10kCountryCode` into the built Ath10kMac's personality and the
  manifest records it (`personality`). It is not a boot-arg on purpose: 1401 leaves `boot-args` out of NVRAM →
  Delete (policy 2c), so a boot-arg added to a new config.plist never reaches a Mac that already has boot-args
  in NVRAM; the kext OpenCore injects carries the value on every boot. `ath10k_cc=XX` still overrides it; `00` =
  world rules. Someone in another country builds with their own code.
- **Post-install scripts** (`--skip scripts` key): `audio-voodoohda.sh`, `audio-applehda.sh`, `install-gpu.sh` (this repo's
  `tools/macos/`) and `gpu-apps.sh` (the driver's `tools/macos/`) copied as release files after `bash -n`;
  `POST-INSTALL.md` generated from `spec.json` by `tools/release/post_install_doc.py` (so it matches the manifest).
  `tools/macos/test-stdin.sh` is a Linux test and does not ship.
- **The Metal plugin's AIR reader** (part of the `plugin` step): `NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib`.
  The plugin (driver `plugin/NVMTLLibraryLoad.m`) dlopens it and reads every Metal library's AIR bitcode in process
  through the LLVM **C API only** (56 functions resolved by name), then hands the printed IR to the translator;
  without it every shader not already in the cache fails to compile. `tools/release/cross-air-llvm.sh` builds it
  from upstream llvm-project **23.1.4** (the cross toolchain's LLVM; the translator reads LLVM 21+ IR syntax:
  `captures(none)`, `samesign`, `getelementptr inbounds nuw`). The release tarball is SHA-256 pinned
  (`819ecf73…434bb2`), and its `.sig` verifies with LLVM release manager Tobias Hieta's key `D574BD5D…45D59042`. The build:
  x86_64 macOS 15.0 against the 26.5 SDK, clang + ld64.lld, components Core/BitReader/IRReader/Passes/Support, no
  code generator, no zlib/zstd (it links only libSystem and libc++), install name `@loader_path/libLLVM.dylib`,
  about 15-25 min once, cached in `$RELEASE_CACHE/air-llvm-<key>`. Licence: Apache-2.0 WITH LLVM-exception, notices
  in `Contents/Resources/air-runtime/` (LLVM, BLAKE3, regex); recorded in the artifact's `third_party`.
  Gate `tools/release/check-air-llvm.py`: (1) the Mach-O exports every C function the plugin resolves (read
  from the plugin source, optional ones included); (2) the 17 AIR modules of the plugin's own Apple-compiled
  `nvconv.metallib` (`nvconv_lib.h`, `air64_v27-apple-macosx15.7.0`) go through the plugin's exact sequence
  (`tools/release/air-reader-check.c`: parse with an error-severity diagnostic handler, the simdgroup-matrix
  unroll pipeline, both pointer-canonicalization pipelines, print) against the host's libLLVM **of the same
  version** (`llvm-config` must say 23.1.4, or `HELIOS_AIR_HOST_LIBLLVM=<libLLVM.so>`). The Mach-O itself cannot
  run on Linux; its first load is on the Mac (`log stream` shows `library: in-memory bitcode reader ready`).
  Upstream LLVM needed no Apple patch for this AIR. AIR from a newer Metal compiler than the sample's
  (macOS 15.7) is only proven by running real apps on macOS 26.

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
| PG106, PG106FB | `pascal/kext/scripts/cross-build-macos.sh`, `pascal/fb/scripts/cross-build-macos.sh` (clang `-mkernel`, ld64 `-kext`) | MH_KEXT_BUNDLE, `_kmod_info`, KPI audit; PG106: 17 firmware files embedded in the binary (`check-pg106-firmware.py`) |
| PG106Accel | `kexts/PG106Accel/cross-build.sh` | host tests, KPI + IOAcceleratorFamily2 audit, vtable slots. Needs the macOS 26 IOAcceleratorFamily2 reference binary, which is not in git (`--iaf2`) |
| NMHDA | `tools/release/cross-nmhda.sh`: Lilu SDK + MacKernelSDK, ld64 `-kext` with the SDK's libkmod. No `cc_kext` builtins are needed (the link would fail). | KPI audit incl. every Lilu export |
| QCARomePatchRAM, Ath10kMac + AirportAth10k | their own `scripts/cross-build.sh` (`tools/cross-build-kext.sh`). Only the native Wi-Fi pair ships; the Ethernet-style core and its app are developer-only. | KPI audit, symbol checks |
| SSDTs | `iasl -vs -ve` | 0 errors |
| NVMTLDriver.bundle | `tools/release/cross-plugin.sh`: clang `-bundle -fobjc-arc`, minos 15.0, `-Werror=unguarded-availability-new`, **ld64.lld** (cctools ld64-956.6 aborts on this object with a libstdc++ assertion), compiler-rt `cpu_model` for `__builtin_cpu_supports`. `nvconv.metal` is committed precompiled (`nvconv_lib.h`), so no Metal compiler is needed. | macho-check.py: every bind exported by an SDK `.tbd`, 0 strong lookups |
| libnvmtl_translate.dylib | `tools/release/cross-xlate.sh`: `cargo build --locked --target x86_64-apple-darwin`, clang + lld linker, `@rpath` install name, paths remapped, built from a fixed path | macho-check.py, no build-machine paths |
| NVK-Pascal | `nvk/pascal/scripts/cross-build-nvk-macos.sh` (Mesa + NAK; `build-nvk-linux.sh` makes the build-machine tools) | its own macho check |

Ad-hoc signatures (`rcodesign sign`, what `codesign -s -` does): PG106FB and PG106Accel (kmutil loads them from
/Library/Extensions), the plugin and its nested `air-runtime/libLLVM.dylib`, the translator, NVK. Kexts injected by
OpenCore need none. **Signing is a gate:** without rcodesign release.sh stops before building (exit 77) unless all of
pg106fb, pg106accel, plugin, xlate and nvk are `--skip`ped. A failed signing, or a signature that does not verify
afterwards, stops it (exit 1). `rcodesign verify` rejects every ad-hoc signature, so
`tools/release/check-adhoc-signature.py` recomputes the CodeDirectory page hashes and, for a bundle, the Info.plist
and CodeResources slots and the seal of every file in it, including the nested libLLVM's own signature and cdhash.
Results go to `dist/logs/signatures.log`. make_manifest refuses an unsigned artifact that installs into /Library. `install-gpu.sh` installs the files exactly as shipped.

**Everything is cross-built; nothing has been built with Xcode on a Mac.** Several artifacts run on the owner's Mac
([MACOS-STATUS.md](MACOS-STATUS.md)). Before a release that is not marked pre-release, do one
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

## v0.1.1: the exact procedure

v0.1.0's EFI cannot be fixed in place: its config lists `PG106FB.kext` under Kernel → Add, which OpenCore refuses,
its PG106 reads firmware from `Resources`, which an injected kext cannot do, and the Wi-Fi country, brightness and
post-install scripts are missing. Each step below is done by the person named; agents prepare, the coordinator
publishes.

1. **Build with the gates** (owner's Linux machine, after the coordinator has bumped `kexts/nvidia-macos-driver`
   to the reviewed GPU build and committed it):
   ```sh
   git -C kexts/nvidia-macos-driver log -1 --oneline        # the commit the release will build
   tools/release.sh --version 0.1.1 --real-kc ~/pascal-build/macos-26.7.2-25G332 --iaf2 <IOAcceleratorFamily2 reference>
   ```
   It must end with `dist/ ready` and every `ok` line, including `ok kc-audit`. Any FAIL stops here.
2. **Reviewer pass** (not the person who built it): `dist/manifest.json` has `release.dry_run` false,
   `release.kc_audit.status` `pass` with six `pass` rows (Ath10kMac, AirportAth10k, QCARomePatchRAM on Boot;
   PG106 on Boot; PG106FB, PG106Accel on Boot + System), `release.sources.driver.commit` = the submodule commit,
   NVMTLDriver.bundle present with `Contents/MacOS/air-runtime/libLLVM.dylib` in `contents_sha256` and `third_party`
   recorded (`dist/logs/plugin.log`: `check-air-llvm: PASS`), every cross-built /Library artifact `signed`
   `ad-hoc (rcodesign)`,
   no `release.missing` except optional pieces, PG106FB and PG106Accel `install` `library-extensions` and `signed`
   `ad-hoc (rcodesign)`, Ath10kMac `personality.Ath10kCountryCode` = `IN`, `config.device_properties` with
   `applbkl`, and `post_install` with the `gpu` step and the two audio choices. Read `dist/logs/pg106.log` for
   `check-pg106-firmware: PASS 17 of 17` and `dist/logs/kc-audit.log`. `sha256sum -c SHA256SUMS.txt` in `dist/`.
3. **Publish the pre-release** (coordinator): tag `v0.1.1` at the commit in `release.commit`, GitHub release marked
   pre-release, upload every file in `dist/` except `logs/` under its `file` name. Download `manifest.json` back
   and compare its SHA-256 with `dist/manifest.json`.
4. **Pin it in 1401**: `p1401/helios300_release.json` → `"tag": "v0.1.1"`, `"manifest_sha256": <that SHA-256>`;
   `python3 -m unittest -q tests.test_helios300` green; commit.
5. **Build the stick** with that 1401 (`1401 build ...` for the Helios, then write the stick). Check the stick's
   `EFI/OC/config.plist`: no `PG106FB.kext` under Kernel → Add, PG106/NMHDA/QCARomePatchRAM/Ath10kMac/AirportAth10k
   present, `applbkl` on `PciRoot(0x0)/Pci(0x2,0x0)`; `EFI/Helios300` holds the PG106FB/PG106Accel zips, the
   scripts, `manifest.json` and `README.txt`.
6. **Regenerate the internal EFI** (the disk's EFI partition, not only the stick): it still has v0.1.0's config,
   whose Kernel → Add lists PG106FB, which OpenCore refuses. Back up the old `EFI/`, then copy the new `EFI/OC`
   from the stick (or a fresh 1401 build) over it. Then on macOS: `EFI/Helios300` → `install-gpu.sh` (the `gpu`
   step; until it ships, by hand as its `what` says; it replaces the PG106FB copied into /Library/Extensions by
   hand earlier), then one audio option. Boot-args are not
   reset by 1401 (NVRAM → Delete leaves them alone): the Wi-Fi country no longer needs one, and any diagnostic
   boot-arg set earlier (`pg106_printf=1`, `pg106_selftest=2`, `ath10k_native_delay`) stays until removed with
   `sudo nvram boot-args="…"`.
