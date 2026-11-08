# Helios 300 Setup

The post-install app for the Acer Predator Helios 300 G3-571 on macOS 26.7.2 (25G332). It replaces running the
`tools/macos` scripts one by one. Open it after macOS is installed. The release scripts are still its engine,
and still ship next to it for advanced users.

**Status: cross-built and unit-tested on Linux only. Nothing in it has run on a Mac yet.**

## What it does

- **Setup**: the release's `post_install` steps in order, each with a live status: Not installed, Installed,
  Needs restart, Failed or Not available (with the reason). The order is: OpenCore to the internal disk, then the
  GTX 1060 stack one part per boot (PG106FB, then the Metal userspace, then PG106Accel), one audio option, then
  sleep.
  - Each step has three buttons: **Check** (the dry run), **Install** and **Undo**. Install is enabled only after
    a passing Check in this boot, against the same manifest.
  - **One driver change per restart.** The app, and its CLI, refuse a second one.
  - Steps that need a restart offer *Restart now*. The app then reopens itself after login, through a LaunchAgent
    it removes again.
  - The script output shows live in the log.
- **Status**: one-glance health from the manifest's `health` table: startup source, Wi-Fi (the network name is
  hidden unless asked), Bluetooth, GTX 1060, HDMI, brightness, audio and sleep. *Copy diagnostics* copies a
  sanitized report with no MAC addresses, serial numbers, UUIDs, network name or local addresses.
- **GPU apps**: tick the apps that render on the GTX 1060 (render offload, like prime-run), or run one once on it.
  Both go through the release's `gpu-apps.sh`.
- **Updates**: works like Sparkle.
  - It checks GitHub for a newer macos_helios300 release (pre-releases follow the installed channel, with a toggle)
    and shows the release notes plus which files changed.
  - Every file is downloaded and verified against the manifest (sha256, size, and every file inside each zip),
    then applied: **Install next step**, **Update all** (one driver change per restart), *Remind me later*, or
    *Skip this version*.
  - *Roll back last update* restores the `.prev` kexts and the `config.plist` backup.
  - A daily check is optional and only posts a notification; it never installs anything.
  - Updating third-party kexts from Dortania's build repo is optional and off by default.
- **Help**: safe mode (Shift+Enter in the OpenCore picker), the Recovery commands, and the BIOS boot order. The
  text comes from the manifest's `app_help`.

## Architecture

```
            ┌──────────────── HeliosSetup (AppKit, macOS only) ──────────────┐
            │ SetupPage  StatusPage  GPUAppsPage  UpdatesPage  HelpSheet      │
            └───────────────┬────────────────────────────────────────────────┘
 helios-setup (CLI) ────────┤
                            ▼
 ┌──────────────────────────── HeliosCore (Foundation only; Linux + macOS) ──────────────────────────┐
 │ Engine (user side): loadRelease · overview · perform · checkForUpdates · download · gpuApps        │
 │   │  changes go through PrivilegedRunner ──► `helios-setup _root …` ──► RootOps (root side)         │
 │   ▼                                                                                                │
 │ ManifestModel   Codable manifest/spec (post_install + app, health, update_groups, config_delta)    │
 │ ManifestCheck   1401's check_manifest / blob / covers + install-gpu.sh's zip check                  │
 │ Probes          ioreg · kmutil · pmset · nvram · system_profiler · diskutil parsers + evaluator    │
 │ StepPlanner     checklist order · status · RestartGuard (one driver change/boot) · DryRunRule       │
 │ Installer       runs post_install `run` lines as written (stdin /dev/null, MODE/ONLY in env)       │
 │ EFIManager      ESP find/mount · backup · staged copy · verify · swap · restore · ocvalidate        │
 │ ConfigMerger    config_delta → config.plist, never dropping the user's keys                        │
 │ Updater         GitHub releases · manifest diff · download + verify (file + inner zip)             │
 │ UpdatePlanner   units (update_groups) · batches (one restart each)                                 │
 │ UpdateApplier   applies a unit; Rollback records + RollbackExecutor                                │
 │ StateStore      /Library/Application Support/Helios300/state.plist (root) + the user's state       │
 │ Ports           CommandRunner · FileSystem · HTTPClient · Clock · PrivilegedRunner (+ fakes)       │
 └────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

Every side effect goes through a protocol in `Ports.swift`, so the whole core runs in `swift test` on Linux with
fakes. Root work runs in a separate process: the user side starts `helios-setup _root --user U --home H <command>`,
through `osascript … with administrator privileges` from the GUI, `sudo` from the CLI, or directly when already
root. That process checks every rule again (the release and its scripts, one driver change per restart). It
writes the change to the system state *before* making it, then makes it. No password is ever seen or stored.

The scripts in `Contents/Resources` must match the release manifest's sha256 before they run, and they run in
the release folder:

```
stdin </dev/null   env -i HOME USER PATH MODE=… ONLY=…   bash install-gpu.sh
```

They never get their own body on stdin.

### Where things come from (one source of truth)

Nothing about steps, scripts, commands, restarts, probes, update groups or OpenCore changes is hard-coded in
Swift. All of it is in `tools/release/spec.json`, which `make_manifest.py` copies into every release's
`manifest.json`:

| spec.json | used for |
| --- | --- |
| `post_install[]` `order`, `group`, `choice`, `recommended`, `run`, `reboot`, `needs`, `notes` | the checklist and the exact commands |
| `post_install[].app` `title`, `driver_change`, `undo_note`, `parts[]` (`env`, `artifacts`, `reboot`), `detect` (`installed`, `active`, `available` probes, `failed_hint`) | names, restart rules, live status |
| `health[]` | the Status page |
| `update_groups[]` (`always` / `abi-change` with `abi_from`) | updates that go in one restart |
| artifact `requires_restart` (default: opencore and library-extensions) | one driver change per restart for updates |
| `config_delta` (computed by make_manifest from `opencore`/`acpi` entries and `config`) | config.plist changes |
| `app_help` | the Help sheet |
| `third_party_kexts` | Dortania products and bundle IDs (optional updates) |

Probe kinds: `file`, `kext`, `ioreg`, `pmset`, `bootarg`, `nvram`, `profiler`, `filevault-off`, `boot-source`, `ip`,
`ssid`, `build`. Modifiers: `equals`, `contains`, `negate`, `format: band|info`, `private`, `why_not`
(`Probes.swift` has the table).

### Adding a step

1. Add it to `post_install` in `tools/release/spec.json`. Give it a `run` with `check`, `install`, `revert` and
   `status` lines in the grammar `Installer.swift` documents (`[VAR=value] bash script.sh`, `sudo program …`,
   `helios-setup …`), an `app.title`, and `app.detect` probes. If the step is installed in parts, give it
   `app.parts`.
2. If it has a script, add that script as a spec artifact, and to release.sh's `b_scripts`.
3. Run `python3 -I apps/HeliosSetup/Tests/regen-fixture-manifest.py` and `swift test`. The tests check:
   - every command parses;
   - every script is an artifact;
   - each step's `build` probe equals its script's pin;
   - spec and manifest list the same steps.

No Swift change is needed. A new probe kind is the only exception: it needs a case in `ProbeEvaluator` and in
the schema enum.

## Build, test, run

```
cd apps/HeliosSetup
swift test -j $(nproc)                                     # the core, on Linux (86 tests)
swift format lint --strict -r Sources Tests                # the committed .swift-format
SIGN=1 bash ../../tools/release/cross-setup-app.sh ../.. ../../kexts/nvidia-macos-driver /tmp/app 0.1.1
```

`cross-setup-app.sh` does the following:

1. Copies the sources to `~/.cache/helios300-release/setup-app/src`, a fixed path, so builds are reproducible and
   cached.
2. Runs `swift format lint` and `swift test`.
3. Builds with `swift build --swift-sdk darwin --triple x86_64-apple-macosx --build-system native`. This uses the
   darwin Swift SDK that xtool installs; SwiftPM's default build system cannot find the macOS platform in it.
4. Assembles `Helios 300 Setup.app`.
5. Checks both Mach-O files: x86_64, minos 26.0, and every symbol bound to the SDK (the driver's
   `macho-check.py`).

`tools/release.sh` runs it as step `setupapp`, signs the nested CLI and then the bundle with rcodesign, and
verifies the signature with `check-adhoc-signature.py`.

CLI (`Helios 300 Setup.app/Contents/MacOS/helios-setup`, also over SSH; root steps ask through sudo, or run directly
under `asroot`):

```
helios-setup status [--json] [--show-ssid]      helios-setup plan
helios-setup check|install|undo <step>          helios-setup efi status|check|install|restore [--source EFI]
helios-setup update --check [--prereleases]     helios-setup update --apply      helios-setup rollback
helios-setup gpu-apps list|add APP|remove APP|run [--log] APP
helios-setup third-party --check|--apply PRODUCT  helios-setup diagnostics      helios-setup release DIR
```

## Credits and licences

- The app and the `tools/macos` scripts it carries are GPL-2.0-only. `gpu-apps.sh`, from nvidia-macos-driver, is
  PolyForm-Noncommercial-1.0.0; its licence text ships next to it in `Contents/Resources`.
- Manifest verification ports 1401's `p1401/helios300.py` (`check_manifest`, `Release.blob`, `Release.covers`,
  `_vkey`). The semantics are kept identical, so both sides verify the same way.
- The OpenCore install and the update flow follow OpenCore Legacy Patcher's `support/install.py`
  (`diskutil list -plist`, mount, copy, unmount) and `support/updates.py` (check, then offer). OCLP is BSD-3-Clause,
  © 2020-2025 Dhinak G, Mykola Grymalyuk and contributors. No OCLP code is included.
- Config validation is OpenCore's own `ocvalidate`, run only when it matches the EFI's OpenCore version. New
  Kernel → Add entries carry every key of OpenCore's Sample.plist.
