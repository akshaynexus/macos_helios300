# QCARomePatchRAM — Bluetooth for the Helios 300 on macOS 26

This is the firmware loader for the Helios 300 G3-571's Bluetooth: Lite-On
`04ca:3016`, a Qualcomm QCA6174A "Rome 3.2" USB HCI controller (ROM `0x302`).
It does for this chip what BrcmPatchRAM3 does for Broadcom chips. It uploads
Qualcomm's rampatch and NVM, then hands the controller to macOS's own
Bluetooth stack: `bluetoothd`, patched by BlueToolFixup to accept a non-Apple
controller. All Bluetooth functions come from Apple's stack; there is no
third-party app or daemon.

It is based on duckducktw's QCARomePatchRAM (`UPSTREAM.md`), reworked to stay
resident, to survive sleep and unplug, and to be tested against the real chip.

## Status (2026-11-08)

**On macOS 26.7.2 (25G332) the owner reports that Bluetooth works** with this kext
OpenCore-injected (macos_helios300 v0.1.0 EFI). That is an owner report, not a
logged test: pairing, A2DP and HFP audio, and sleep/wake have not been tested
one by one yet ([Owner test steps](#owner-test-steps-macos-26)).

| What | Where verified | Result |
|---|---|---|
| Our core uploads rampatch + NVM to the real chip | Helios, Linux userspace, chip unpatched after a cold boot (`tools/linux/qca_rome_usb.c` = same core as the kext) | **works**: patch `0x111`→`0x3e8`, status `0x20`→`0xe0`, 18 bulk packets, 384 ms. Linux btusb then saw a patched controller (no re-download), hci0 powered, a scan found devices (`evidence/cold1-core-upload.txt`) |
| Our transfers vs Linux btusb's | source review (`btusb_main.c` 3566–3972) + golden replay of usbmon captures from this laptop | **identical**: Linux's own cold-boot download (usbmon) and our core's are the same 23 requests: setup fields, lengths, SHA-256 of the header and all 18 bulk packets, and a ~20 ms DFU delay. Our core adds one read-only final status read (`evidence/cold2-linux-download.txt`; `tests/golden/linux-cold-download.trace`) |
| Which events lose the patch | Helios, Linux | kept: warm reboot, USB port reset, deauthorize/authorize, USB port power-off. Lost: cold boot (power-off), firmware crash. Suspend-to-RAM: not measured (Linux S3 was blocked by the NVIDIA driver) |
| Firmware = what Linux loads | Helios `/usr/lib/firmware/qca/` | byte-identical (`evidence/firmware-hashes.txt`) |
| Lifecycle (start, retries, sleep/wake, unplug, failure paths) | host tests with a simulated chip | pass (ASan/UBSan, gcc + clang) |
| HCI filter: SCO translation, answer matching, command watchdog | host tests (`tests/test_hci_filter.cpp`), mutation-checked | pass. **Never loaded on macOS** |
| Kexts build for macOS 26 x86_64 | Linux cross-build: MacOSX26.5 SDK, Apple ld64, KPI audit against xnu-12377, kc-audit and virtual-slot check against the 26.7.2 collections | pass, both kexts |
| Loads on macOS, Bluetooth usable | Helios, macOS 26.7.2 | **works (owner report, 2026-11-08)**; no log of the run is kept here |
| **Pairing, A2DP/HFP audio, sleep/wake** | **macOS only** | **not tested yet**; see [Owner test steps](#owner-test-steps-macos-26) |

## How it works

1. **Match.** The kext matches `IOUSBHostDevice` with `idVendor 0x04ca`,
   `idProduct 0x3016`, IOProbeScore 4000. It uses its own `IOMatchCategory`,
   so other drivers and the userspace stack can still open the device.
2. **Setup run.** The run happens on the kext's own workloop, 10 ms after
   `start`.
   - It opens the device. If the device is unconfigured, the kext configures
     it with interface matching on, and leaves it configured.
   - It runs the portable state machine in `src/core/qca_rome.c`:
     - Vendor IN `0x09` reads the version. Only ROM `0x302` with the generic
       NVM (version flag not `0x80xx`) is accepted, before anything is
       written.
     - Vendor IN `0x05` reads the status.
     - If the rampatch bit `0x80` is clear, it sends
       `rampatch_usb_00000302.bin`: the 28-byte header via vendor OUT `0x01`,
       then a 20 ms pause, then the body over bulk OUT EP `0x02` in 4 KB
       chunks.
     - It reads the version again.
     - If the NVM bit `0x40` is clear, it sends `nvm_usb_00000302.bin` the
       same way (4-byte header).
     - The final status must show `0xc0`.
   - The interface and bulk pipe are opened only if a download is due, and
     before its DFU header, so a failure can't leave EP2 switched to DFU.
3. **Hand-over.** After a successful run on macOS 12+:
   - The kext renames the device to `Bluetooth USB Host Controller` and
     re-registers it. bluetoothd uses that name to choose the USB transport.
     IntelBluetoothFirmware does the same on Monterey+ (its commit
     `eb7e3e1`).
   - It publishes the result in ioreg: `QCARomePatchRAM-*` properties on the
     device, and `State`/`Runs`/`Wakes` on the driver.
   - It closes the device.
4. **Stays resident.** The kext joins the power tree.
   - **Wake.** When power returns after a sleep, the same run happens 800 ms
     later. If the chip kept its patch, that run is four control reads with
     no writes. If the chip lost power without re-enumerating, the run does a
     full download.
     Power events are only posted to the kext's workloop, so the PM thread
     never waits for a run. If bluetoothd holds the device during a wake
     check, the check is skipped.
   - **Retries.** Transient USB failures are retried up to 4 times, with
     250/500/1000 ms between attempts, and again on every wake. Permanent
     errors (wrong chip, firmware mismatch) are not retried, except for one
     attempt per wake in case the error came from a garbage read.
   - **Unplug or unload.** `willTerminate` (which never waits) aborts the
     control requests and the bulk pipe; the run ends quickly and closes the
     device, and `stop()` then tears down. A re-enumerated device gets a new
     instance.

   These decisions live in `src/core/qca_lifecycle.c` and are host-tested.
   The upstream one-shot `QCARomePatchRAM-Attempted` flag is gone.

### Why no transport personality injection
On macOS 12+ the USB HCI transport is `bluetoothd` in userspace, and
BlueToolFixup's patches (vendor check, chipset check) are in `bluetoothd`.
BrcmBluetoothInjector's personalities pointed Apple's *kernel* transports
(`BroadcomBluetoothHostControllerUSBTransport`,
`IOBluetoothHostControllerUSBTransport`) at third-party devices. BrcmPatchRAM
documents that injector for 10.11–11 only, and says not to use it with
BlueToolFixup on macOS 12 or newer. Upstream QCARomePatchRAM added such an
injection and then reverted it (`ea3d365`).

This kext injects nothing. Instead it applies the device name that
IntelBluetoothFirmware applies on Tahoe. Whether bluetoothd then adopts the
controller can only be confirmed on macOS (test step 4).

## macOS 26 requirements (OpenCore)

1. **Kexts, in this order under Kernel → Add:**
   1. `Lilu.kext` (current release)
   2. `BlueToolFixup.kext` from **BrcmPatchRAM ≥ 2.7.2**. 2.7.2's changelog
      says "Added `bluetoothd` patches for macOS 26". That is the chipset-check
      patch for 15.4+, applied for Sequoia and later, including Tahoe
      (BrcmPatchRAM commit `4fdd4d4`). `MinKernel` `21.0.0`.
   3. `QCARomePatchRAM.kext` (this one). `MinKernel` `21.0.0`,
      `ExecutablePath` `Contents/MacOS/QCARomePatchRAM`.
   4. `QCARomeHCIFix.kext` (optional, see below). `MinKernel` `25.0.0`,
      `ExecutablePath` `Contents/MacOS/QCARomeHCIFix`.
2. **NVRAM → Add** under `7C436110-AB2A-4BBB-A880-FE41995C9F82`. BrcmPatchRAM
   ≥ 2.7.0 requires these on all macOS versions. Also list both keys under
   NVRAM → Delete for that GUID so they get written:
   - `bluetoothExternalDongleFailed`: Data `00`
   - `bluetoothInternalControllerInfo`: Data `0000000000000000000000000000`
3. **USB map.** USBToolBox/UTBMap must include the BT port: xHCI 8086:a12f,
   USB 2 port 7 (HS07) on this laptop, connector type 255 (internal).
   Otherwise macOS treats the controller as an external dongle. The DSDT calls
   HS07 not connectable, and `acpi/SSDT-HS07-UPC.dsl` cannot replace that method,
   so `UTBMap` ([docs/USB-MAP.md](../../docs/USB-MAP.md)) is the proper fix.
4. **Do not install** BrcmPatchRAM3, BrcmBluetoothInjector, BrcmFirmwareData,
   IntelBluetoothFirmware, IntelBTPatcher or IntelBluetoothInjector. Install
   only one BlueToolFixup.
5. **Boot args.** None are required. Optional:
   - `-qcarpprobe`: read-only, no download. Use it first if you want to be
     cautious.
   - `-qcarpoff`: the kext does not attach.
   - `-x` (macOS safe mode): the kext logs one line and does not attach, like the
     Wi-Fi kexts.
   - `-qcarpnoname`: no rename (diagnostics).
   - `-btlfxallowanyaddr` (BlueToolFixup): only if the log says "Third Party
     Dongle has the same address as the internal module".

A warm reboot from Linux into macOS leaves the chip patched. The kext then
only reads the status, which is expected and correct.

## What to expect, and the limits

These come from Apple's stack on a non-Apple controller; the kext cannot
change them.

- **Expected:**
  - The controller appears in System Settings › Bluetooth, and the menu-bar
    toggle works.
  - BR/EDR and LE pairing works (keyboards, mice, game controllers).
  - A2DP stereo audio works.
  - Toggle off/on and sleep/wake work.
- **Not available:** Handoff, Universal Clipboard, AirDrop, Continuity Camera,
  Sidecar wireless, Unlock with Apple Watch. These need Apple's Broadcom
  firmware and its vendor HCI extensions.
- **Hands-free (HFP/HSP) microphone audio: unmeasured.** Linux marks this
  controller `HCI_QUIRK_BROKEN_ENHANCED_SETUP_SYNC_CONN`: it advertises
  Enhanced Setup Synchronous Connection, but the command "doesn't seem to work
  with HSP/HFP mSBC" (`btusb_setup_qca`). The Helios's Linux logs
  "...advertised, but not supported." on every init. QCARomeHCIFix (below)
  covers this if Apple's stack sends that command. A2DP is not affected.
- **Firmware crash.** The Linux logs on this laptop show two controller
  crashes (`Opcode 0x0c24 failed: -112`), and each time the chip lost its
  patch. Linux recovers by resetting the USB device; QCARomeHCIFix does the
  same (below).

### Linux Rome quirks and what macOS needs
| Linux (btusb, `BTUSB_QCA_ROME` + `BTUSB_WIDEBAND_SPEECH` for 04ca:3016) | Meaning | macOS |
|---|---|---|
| `setup_on_usb = btusb_setup_qca` | rampatch + NVM download | **this kext** |
| `HCI_QUIRK_BROKEN_ENHANCED_SETUP_SYNC_CONN` | don't use HCI 0x043D for SCO | QCARomeHCIFix sends 0x0428 instead |
| `HCI_QUIRK_SIMULTANEOUS_DISCOVERY` | Linux may scan LE and BR/EDR together | Linux scheduler hint; nothing needed |
| `HCI_QUIRK_WIDEBAND_SPEECH_SUPPORTED` | Linux may offer mSBC | Linux feature flag; nothing needed |
| `hdev->set_bdaddr = btusb_set_bdaddr_ath3012` | vendor command to change BD_ADDR | used only when user space sets an address. The chip has a valid public address (Linux reports no "missing options"); nothing needed |
| `hdev->shutdown = btusb_shutdown_qca` | HCI_Reset on power-off | bluetoothd resets the controller itself |
| `hdev->reset = btusb_qca_reset` | USB reset after 5 command timeouts | QCARomeHCIFix, same rule, bounded |
| `btusb_check_needs_reset_resume` | reset-resume for listed Dell models | not this laptop |
| btqca.c (`HCI_QUIRK_USE_BDADDR_PROPERTY` etc.) | QCA UART/SoC parts | not used by USB Rome |

The descriptors are the standard Bluetooth USB layout: interface 0 has the
event and ACL endpoints; interface 1 has isochronous SCO with alternate
settings 0–5 (0/9/17/25/33/49-byte packets). Remote wakeup and LPM are
supported (`evidence/lsusb-v-04ca-3016.txt`).

## QCARomeHCIFix: SCO translation and crash recovery

A second, separate kext, `QCARomeHCIFix.kext`: a Lilu plugin that hooks
`IOUSBHostDevice::deviceRequest` and the three `IOUSBHostIOSource::io`
overloads (the places IntelBTPatcher hooks on Tahoe), only ever acting on
04ca:3016. It is separate so a Lilu or hook problem cannot stop the firmware
load, which is known to work. The decisions are in `src/core/qca_hci_filter.c`
and host-tested. **It has never been loaded on macOS.**

Kernel path on 26.7.2 (read from the real collection): HCI commands from
bluetoothd arrive through `AppleUSBHostUserClient::ControlRequest` at the 7-argument
`deviceRequest` that every overload funnels into. Events come back through
`IOUSBHostPipe::io`, which ends up in `IOUSBHostIOSource::io`. BlueToolFixup only
patches `bluetoothd` in user space, so the two do not overlap.

**Does macOS actually send 0x043D?** Not known. The closest stack on disk is
iOS 26's `bluetoothd`, whose HCI command-name table has the legacy
`SETUP_SYNCHRONOUS_CNCT` and `ACCEPT_SYNCHRONOUS_CNCT` and **no** enhanced setup
(it does have `ENHANCED_FLUSH`, so the table is not simply old). If macOS 26 is the
same, the translator never fires and was not needed: it costs nothing and the
`TranslatedSetup` counter stays 0. Whether HFP works then rests on other things.
The on-device test reads that counter.

**Translation (default on, `-qcahcinosco` turns it off).** 0x043D/0x043E become
0x0428/0x0429 only when the legacy command says the same thing:

| Air coding | Host coding | Legacy Voice_Setting |
|---|---|---|
| CVSD / µ-law / A-law | linear PCM 8 or 16 bit, or µ-law/A-law | built from the fields (CVSD + 16-bit PCM = 0x0060) |
| transparent, or mSBC | transparent (host does the coding) | 0x0003 (BlueZ's `BT_VOICE_TRANSPARENT`) |
| mSBC | linear PCM (a codec in the controller) | refused |

Refused means passed through unchanged and counted, so a case the legacy command
cannot express behaves exactly as it does today. The same goes for a vendor codec,
a non-HCI data path, or TX/RX that differ. The Command Status is rewritten back to
the enhanced opcode. Legacy commands the host sends itself are matched in order and
left alone. Nothing is translated until an event has been seen from the controller,
since an unproven event hook could not rewrite the answer. The Read Local Supported
Commands bit is deliberately not hidden: nobody knows how the stack reacts.

**Crash recovery (default on, `-qcahcinowd` turns it off).** Mirrors
`btusb_qca_reset()`: five HCI commands unanswered for 2 s (10 s for HCI_Reset) lead
to a USB reset. `IOUSBHostDevice::reset` re-enumerates the device and QCARomePatchRAM's
new instance re-patches it. Two more cases are handled:

- Two unanswered commands and then 30 s of silence also count, because bluetoothd may
  stop sending after a timeout and five would never come.
- Any event from the controller clears the count. One lost answer never resets.
  Host_Number_Of_Completed_Packets and vendor commands are not timed, and nothing is
  timed until the event hook is proven.

Bounded: 3 resets inside 10 minutes, then none until reboot
(`RecoveryExhausted` = Yes), with a 30 s cooldown after each. The two crashes in
`evidence/linux-dmesg.txt` were 15 minutes apart, a rate this budget never spends.

Other boot args: `-qcahcioff` (plugin does not load), `-qcahcidbg`. Under `-x`
Lilu does not start it. Counters: `ioreg -l -w0 | grep -A14 '"QCARomeHCIFix"'`.

## Sleep and wake (review, 2026-11-08)

Read against the code and the host tests; never run on macOS. Lid close is an
ordinary system sleep here: IOKit delivers `setPowerState` the same way.

- **Fixed:** a permanent error read while the machine was going to sleep and
  back (a garbage read at the worst moment) left the controller dead until the
  next wake. Now the usual one attempt per wake follows it.
- **Fixed:** a wake check skipped because bluetoothd holds the device counted
  as a success in `Successes`. It now counts as neither.
- **Power loss** is detected by reading the status, not assumed. A chip that lost
  power without re-enumerating gets a full download; one that re-enumerated gets a new
  instance. If bluetoothd holds the device the check is skipped, so the case
  it cannot see is a chip that lost power, did not re-enumerate, and is held
  open. A device that lost power should not survive USB resume, and if it did,
  the watchdog above catches the dead controller.
- **`-x`:** `probe` declines before attaching, so there is no power-tree entry and
  no wake path. QCARomeHCIFix does not start either.
- **Unknown until measured:** whether bluetoothd holds the device across sleep,
  and what the chip does in S3 (Linux S3 was blocked on this laptop).

## A2DP and Wi-Fi coexistence (2.4 GHz)

Bluetooth and Wi-Fi share the radio on the QCA6174A. Coexistence is firmware-level
between the two parts: this driver neither sees nor sets it, and Linux has no
Rome-USB setting for it either. So it is something to measure, not to code:

1. A2DP to headphones for 10 minutes, Wi-Fi joined to a **2.4 GHz** network and
   idle. Listen for dropouts.
2. Same, with continuous Wi-Fi traffic (a large download).
3. Same, on **5 GHz**, as the comparison: it should be clean, and if it is not, the
   cause is not coexistence.
4. An HFP call during the 2.4 GHz traffic (Bluetooth reserves slots for voice, so
   expect Wi-Fi throughput to fall).
5. A BLE or BR/EDR mouse during 2.4 GHz traffic: note any cursor lag.

Report what was connected, the band, and the audio codec. Wi-Fi TX is not confirmed
yet ([MACOS-STATUS](../../docs/MACOS-STATUS.md)), so step 2 waits for it.

## Rollback

- Add the boot arg `-qcarpoff`, or disable the kext in OpenCore's
  config.plist. The chip then stays at ROM and Bluetooth does not work.
  Nothing else is affected.
- To remove the kext, delete `QCARomePatchRAM.kext` from `EFI/OC/Kexts` and
  its Kernel → Add entry. Keep BlueToolFixup only if another Bluetooth kext
  needs it.
- QCARomeHCIFix alone: boot arg `-qcahcioff`, or disable its Kernel → Add
  entry. The firmware loader does not depend on it.
- The kext writes nothing persistent: no NVRAM, no chip flash. The patch
  lives in the controller's RAM until power is lost.

## Owner test steps (macOS 26)

Use the cross-built `build/cross/QCARomePatchRAM.kext`. For a release, sign it
ad hoc on a Mac with `codesign -f -s - QCARomePatchRAM.kext`; OpenCore
injection does not need a signature.

1. **First boot, probe only.** Add the kexts and NVRAM keys above, plus the
   boot arg `-qcarpprobe`. Power off first, then boot. In Terminal run:
   ```sh
   log show --last boot --predicate 'sender == "QCARomePatchRAM"' --info | head -40
   ioreg -l -w0 | grep -E 'QCARomePatchRAM|"State"|"Runs"'
   ```
   Expect `probe-only done ... rom 0x00000302 patch 0x111 ... status 0x20`
   (any status without `0x80` means unpatched) and
   `QCARomePatchRAM-Result = "ok"`.
2. **Real upload.** Remove `-qcarpprobe` and cold boot again.
   - The log should show `firmware ready in ~400 ms: rom 0x00000302 patch
     0x111->0x3e8 status 0x20->0xe0 (rampatch=1 nvm=1, 18 bulk packets)`,
     then `device named "Bluetooth USB Host Controller"`.
   - In ioreg, the device should show `QCARomePatchRAM-StatusFinal = 0xe0`
     and the driver `State = "ready"`.
3. **Controller visible.**
   - System Settings › Bluetooth shows the switch.
   - System Information › Bluetooth lists a controller. Note the Chipset /
     Vendor ID (expected Qualcomm / `0x04CA`), firmware version and
     "Transport: USB".
   - Toggle Bluetooth off and on 3 times from the menu bar. It must come back
     every time.
4. **If there is no controller:**
   - Run `log show --last boot --predicate 'process == "bluetoothd"' | grep -iE 'transport|dongle|chipset|vendor|error' | head -60`
     and `ioreg -p IOUSB -l -w0 | grep -A30 'Bluetooth USB Host Controller'`.
   - Check that BlueToolFixup loaded (`kextstat | grep -i bluetoolfixup`),
     that the NVRAM keys are set (`nvram -p | grep -i bluetooth`), and that
     HS07 is in the USB map.
   - Then try once with `-qcarpnoname` and compare.
5. **Pairing.** Pair a classic BT keyboard or mouse (BR/EDR) and an LE device
   (LE mouse, watch or fitness band). Use each one for 5 minutes.
6. **Audio.** Pair headphones.
   - A2DP: play music for 10 minutes and check the codec (Option-click the
     Bluetooth menu: AAC/SBC).
   - HFP: choose the headphones as the *input* device in System Settings ›
     Sound, then make a FaceTime audio call or record in Voice Memos for a
     minute. Then record the counters:
     `ioreg -l -w0 | grep -A14 '"QCARomeHCIFix"'`.
     - `TranslatedSetup` > 0 and the call works: the fix is doing its job.
     - `TranslatedSetup` = 0 and the call works: macOS uses the legacy command and
       no fix was needed.
     - The call fails: capture
       `log show --last 10m --predicate 'process == "bluetoothd" OR sender == "QCARomeHCIFix"'`
       and repeat once with `-qcahcinosco` to compare.
7. **Sleep/wake.** With a device connected, sleep for 1 min, then 15 min,
   then overnight. After each wake:
   - The device should reconnect, and ioreg should show `Wakes` incremented
     and `State = "ready"`.
   - The log shows `power off`/`power on`, then a run with either
     `rampatch=0 nvm=0` (the chip kept power) or `rampatch=1` (it lost power
     and was repaired). Both are fine; note which one you see, and whether the
     log says `wake check skipped`.
   - Close the lid, wait a minute, open it: same checks.
   - From sleep, press a key on a paired Bluetooth keyboard: note whether the
     laptop wakes. If not, that is USB wake or Apple's stack, not the firmware.
8. **Toggle.** Turn Bluetooth off and on 3 times with a device connected, and
   check `EventsSeen` keeps rising and `CommandTimeouts` stays 0 in the ioreg
   counters.
9. **Report** the outputs of steps 1–8: `log show ... QCARomePatchRAM`, the
   ioreg lines and the System Information Bluetooth page. Remove BD_ADDRs
   first.

## Building

```sh
make test          # host tests (ASan/UBSan): core, lifecycle + chip simulation, HCI filter, golden replay
make kext          # Linux -> macOS 26 cross-build: both kexts in build/cross/
make linux-tools   # static build/qca-rome-usb and build/usbmon-pcap for the laptop
make kext-macos    # on a Mac: Xcode + MacKernelSDK (upstream script)
```
`make kext` uses the repo's `tools/cross-build-kext.sh`:
- clang `-mkernel` against the MacOSX26.5.sdk `Kernel.framework`, with
  `-mllvm --disable-atexit-based-global-dtor-lowering`
- a generated `_kmod_info`
- cctools ld64 `-kext -static` with `libkmodc++`/`libkmod`
- `tools/kpi-audit.py`: every import must be in the xnu-12377 KPI exports,
  and IOUSBHostFamily imports in the SDK headers
  (`kext/QCARomePatchRAM/kpi-allow.txt`)

`QCARomeHCIFix.kext` is a Lilu plugin, so it has its own recipe
(`scripts/cross-build-hcifix.sh`, the same one as NMHDA's), against the pinned
Lilu 1.7.2 SDK. With `HELIOS_REAL_KC` set it also runs
`scripts/check-virtual-slots.py`: the plugin makes virtual calls on Apple
objects, which `kc-audit` cannot see, so their slots are compared with the
real collection.

Firmware is fetched at build time from linux-firmware
`d0039fd797d7776d89f00c22c45754914a96706e` and checked by size and SHA-256
(`scripts/gen_firmware_header.py`); it is not committed. The built kext embeds
it, so the Qualcomm Atheros licence and notices ship in `Contents/Resources/`
(`firmware/LICENSE.QualcommAtheros_ath10k`, `firmware/NOTICE.qca`). The
licence allows unmodified binary redistribution for use with Qualcomm
Atheros chipsets.

| file | bytes | sha256 |
|---|---|---|
| rampatch_usb_00000302.bin | 68644 | f0d15f6d7c4ce17270c951287222699c0909bea9028ecb84d2e8be6fa364691e |
| nvm_usb_00000302.bin | 1998 | 9ee2cff5bd51523b65c941b72ecf05a8b5e7b9e280c79fa08c70ade7205088a5 |

## Evidence

Everything was collected on this laptop under Linux and is in `evidence/`
(index in `evidence/README.md`); MACs, BD_ADDRs and the hostname are
redacted. The golden traces that `make test` replays are in `tests/golden/`.

## Layout
- `src/core/`: portable C (state machine and lifecycle policy), shared by the
  kext, the tests and the Linux tool
- `kext/QCARomePatchRAM/`: IOKit driver, Info.plist, KPI allowlist
- `kext/QCARomeHCIFix/`: the Lilu plugin (hooks only; decisions are in
  `src/core/qca_hci_filter.c`)
- `tests/`: unit, simulation and golden tests; `tests/support/sha256.c`
- `tools/linux/`: `qca_rome_usb.c` (our core over usbdevfs) and
  `usbmon_pcap.c` (usbmon → pcap)
- `tools/usbmon_reduce.py`: pcap → trace, with no firmware bytes or HCI
  payloads
- `scripts/`: firmware header generator, build scripts
- `firmware/`: Qualcomm licence and notices

Licence: GPL-2.0-or-later (`LICENSE`). Firmware: Qualcomm Atheros licence
(`firmware/`).
