# Evidence: Bluetooth (04ca:3016) cross-checks on the Helios 300 under Linux

All collected on the owner's Acer Predator Helios 300 G3-571 running CachyOS,
kernel 7.3.1-1-cachyos, linux-firmware 20261019, 2026-11-07/08. The scripts
were sent over SSH (`~/.config/helios300-ssh/hb`). MAC addresses/BD_ADDRs and
the hostname are redacted. HCI events and ACL data were never recorded in
these files (usbmon_reduce.py prints only vendor control transfers, bulk
EP 0x02 payload hashes, HCI command opcodes and counts).

| File | What it shows |
|---|---|
| `lsusb-v-04ca-3016.txt` | full descriptors (root): class E0/01/01, bcdUSB 2.01, Full Speed, 1 configuration; interface 0: EP 0x81 interrupt IN (16), 0x82 bulk IN (64), **0x02 bulk OUT (64)**; interface 1: isochronous 0x83/0x03, alt settings 0–5; remote wakeup; LPM; no strings (iProduct 0) |
| `firmware-hashes.txt` | the laptop's `/usr/lib/firmware/qca/{rampatch,nvm}_usb_00000302.bin` are byte-identical to what the kext embeds |
| `linux-dmesg.txt` | Linux's own view: `QCA: patch rome 0x302 build 0x3e8, firmware rome 0x302 build 0x111`, `using NVM file: qca/nvm_usb_00000302.bin`; download at a boot after power-off, none after a warm reboot; two firmware crashes (`Opcode 0x0c24 failed: -112`), each needing a re-download |
| `persistence-exp1-reset-deauthorize.txt` | our core's probe: patch kept across a USB port reset (`USBDEVFS_RESET`) and `authorized` 0→1 (status `0xe0`, patch `0x3e8` each time) |
| `persistence-exp2-port-power-off.txt` | patch kept across a root-hub port power-off (`usb1-port7/disable` 1→0, device disconnected and re-enumerated as dev 4); Linux btusb re-setup found it patched |
| `s3-attempt-aborted.txt` | the one approved `rtcwake -m mem` attempt; the NVIDIA proprietary driver failed `pci_pm_suspend` (-5), so no S3 happened: **S3 behaviour not measured** |
| `cold1-core-upload.txt` | **real-hardware run of our core** after a cold boot with btusb held off: chip at ROM (patch `0x111`, status `0x20`) → our upload (18 bulk packets, 384 ms, 20.1/20.8 ms DFU delays) → patch `0x3e8`, status `0xe0`; Linux btusb then bound without downloading, hci0 powered, scan found devices |
| `cold1-core-upload.usbmon.trace` | the same run recorded independently by usbmon (identical to the tool's own trace; also `tests/golden/core-cold-download.trace`) |
| `cold2-linux-download.*` | Linux btusb's own download after a second cold boot, captured with usbmon, and the diff against our core (present once cold-2 was run) |

Measured patch persistence (chip status `0xe0` = patched, `0x20` = ROM only):

| Event | Patch kept? |
|---|---|
| warm reboot (Linux → Linux, 12 s) | yes (no download at next boot) |
| USB port reset (USBDEVFS_RESET) | yes |
| deauthorize / authorize | yes |
| root-hub port power-off + on (device re-enumerates) | yes |
| power-off ≥ 15 s (cold boot) | **no** (patch `0x111`, status `0x20`) |
| controller firmware crash (HCI command timeout) | **no** (Linux reset + re-download) |
| suspend to RAM (S3) | not measured |

The macOS kext handles every row: it runs on every device instance (cold boot,
re-enumeration) and on every wake (S3 either way), and does nothing when the
chip is already patched.
