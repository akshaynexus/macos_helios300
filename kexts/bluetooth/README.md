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

| What | Where verified | Result |
|---|---|---|
| Our core uploads rampatch + NVM to the real chip | Helios, Linux userspace, chip unpatched after a cold boot (`tools/linux/qca_rome_usb.c` = same core as the kext) | **works**: patch `0x111`→`0x3e8`, status `0x20`→`0xe0`, 18 bulk packets, 384 ms. Linux btusb then saw a patched controller (no re-download), hci0 powered, a scan found devices (`evidence/cold1-core-upload.txt`) |
| Our transfers vs Linux btusb's | source review (`btusb_main.c` 3566–3972) + golden replay of usbmon captures from this laptop | **identical**: Linux's own cold-boot download (usbmon) and our core's are the same 23 requests: setup fields, lengths, SHA-256 of the header and all 18 bulk packets, and a ~20 ms DFU delay. Our core adds one read-only final status read (`evidence/cold2-linux-download.txt`; `tests/golden/linux-cold-download.trace`) |
| Which events lose the patch | Helios, Linux | kept: warm reboot, USB port reset, deauthorize/authorize, USB port power-off. Lost: cold boot (power-off), firmware crash. Suspend-to-RAM: not measured (Linux S3 was blocked by the NVIDIA driver) |
| Firmware = what Linux loads | Helios `/usr/lib/firmware/qca/` | byte-identical (`evidence/firmware-hashes.txt`) |
| Lifecycle (start, retries, sleep/wake, unplug) | host tests with a simulated chip | pass (ASan/UBSan, gcc + clang) |
| Kext builds for macOS 26 x86_64 | Linux cross-build: MacOSX26.5 SDK, Apple ld64, KPI audit against xnu-12377 | MH_KEXT_BUNDLE; all 314 imports resolve |
| **Loads on macOS; controller in System Settings; pairing, audio, sleep/wake** | **macOS only** | **not tested yet**; see [Owner test steps](#owner-test-steps-macos-26) |

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
2. **NVRAM → Add** under `7C436110-AB2A-4BBB-A880-FE41995C9F82`. BrcmPatchRAM
   ≥ 2.7.0 requires these on all macOS versions. Also list both keys under
   NVRAM → Delete for that GUID so they get written:
   - `bluetoothExternalDongleFailed`: Data `00`
   - `bluetoothInternalControllerInfo`: Data `0000000000000000000000000000`
3. **USB map.** USBToolBox/UTBMap must include the BT port: xHCI 8086:a12f,
   USB 2 port 7 (HS07) on this laptop, connector type 255 (internal).
   Otherwise macOS treats the controller as an external dongle.
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
- **Risk: hands-free (HFP/HSP) microphone audio.** Linux marks this
  controller `HCI_QUIRK_BROKEN_ENHANCED_SETUP_SYNC_CONN`: it advertises
  Enhanced Setup Synchronous Connection, but the command "doesn't seem to work
  with HSP/HFP mSBC" (`btusb_setup_qca`). The Helios's Linux logs
  "...advertised, but not supported." on every init.
  - If Apple's stack uses that command, headset-mic calls may fail. A2DP is
    not affected.
  - A fix would mean editing the HCI Read Local Supported Commands reply on
    its way to bluetoothd, with a Lilu/IOUSBHostPipe hook like IntelBTPatcher.
    This is not done.
  - Test step 6 checks it.
- **Firmware crash.** The Linux logs on this laptop show two controller
  crashes (`Opcode 0x0c24 failed: -112`), and each time the chip lost its
  patch. Linux recovers by resetting the USB device. On macOS, if bluetoothd
  resets the device, it re-enumerates and the kext patches the new instance.
  Without a reset, the controller stays dead until the next sleep/wake or
  reboot.

### Linux Rome quirks and what macOS needs
| Linux (btusb, `BTUSB_QCA_ROME` + `BTUSB_WIDEBAND_SPEECH` for 04ca:3016) | Meaning | macOS |
|---|---|---|
| `setup_on_usb = btusb_setup_qca` | rampatch + NVM download | **this kext** |
| `HCI_QUIRK_BROKEN_ENHANCED_SETUP_SYNC_CONN` | don't use HCI 0x043D for SCO | not applied; HFP risk above |
| `HCI_QUIRK_SIMULTANEOUS_DISCOVERY` | Linux may scan LE and BR/EDR together | Linux scheduler hint; nothing needed |
| `HCI_QUIRK_WIDEBAND_SPEECH_SUPPORTED` | Linux may offer mSBC | Linux feature flag; nothing needed |
| `hdev->set_bdaddr = btusb_set_bdaddr_ath3012` | vendor command to change BD_ADDR | used only when user space sets an address. The chip has a valid public address (Linux reports no "missing options"); nothing needed |
| `hdev->shutdown = btusb_shutdown_qca` | HCI_Reset on power-off | bluetoothd resets the controller itself |
| `hdev->reset = btusb_qca_reset` | USB reset on command timeout | up to bluetoothd; see firmware crash above |
| `btusb_check_needs_reset_resume` | reset-resume for listed Dell models | not this laptop |
| btqca.c (`HCI_QUIRK_USE_BDADDR_PROPERTY` etc.) | QCA UART/SoC parts | not used by USB Rome |

The descriptors are the standard Bluetooth USB layout: interface 0 has the
event and ACL endpoints; interface 1 has isochronous SCO with alternate
settings 0–5 (0/9/17/25/33/49-byte packets). Remote wakeup and LPM are
supported (`evidence/lsusb-v-04ca-3016.txt`).

## Rollback

- Add the boot arg `-qcarpoff`, or disable the kext in OpenCore's
  config.plist. The chip then stays at ROM and Bluetooth does not work.
  Nothing else is affected.
- To remove the kext, delete `QCARomePatchRAM.kext` from `EFI/OC/Kexts` and
  its Kernel → Add entry. Keep BlueToolFixup only if another Bluetooth kext
  needs it.
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
     Sound, then make a FaceTime audio call or record in Voice Memos. If the
     call drops or has no audio, that is the Enhanced-SCO quirk above.
     Capture `log show --last 10m --predicate 'process == "bluetoothd"'`.
7. **Sleep/wake.** With a device connected, sleep for 1 min, then 15 min,
   then overnight. After each wake:
   - The device should reconnect, and ioreg should show `Wakes` incremented
     and `State = "ready"`.
   - The log shows `power off`/`power on`, then a run with either
     `rampatch=0 nvm=0` (the chip kept power) or `rampatch=1` (it lost power
     and was repaired). Both are fine; note which one you see.
8. **Report** the outputs of steps 1–7: `log show ... QCARomePatchRAM`, the
   ioreg lines and the System Information Bluetooth page. Remove BD_ADDRs
   first.

## Building

```sh
make test          # host tests (ASan/UBSan): core, lifecycle + chip simulation, golden replay
make kext          # Linux -> macOS 26 cross-build: build/cross/QCARomePatchRAM.kext
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
- `tests/`: unit, simulation and golden tests; `tests/support/sha256.c`
- `tools/linux/`: `qca_rome_usb.c` (our core over usbdevfs) and
  `usbmon_pcap.c` (usbmon → pcap)
- `tools/usbmon_reduce.py`: pcap → trace, with no firmware bytes or HCI
  payloads
- `scripts/`: firmware header generator, build scripts
- `firmware/`: Qualcomm licence and notices

Licence: GPL-2.0-or-later (`LICENSE`). Firmware: Qualcomm Atheros licence
(`firmware/`).
