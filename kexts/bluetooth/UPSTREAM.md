# Upstream

`kexts/bluetooth/` (originally `bluetooth/`) started as a snapshot of **QCARomePatchRAM** by duckducktw:

- Repository: https://github.com/duckducktw/qca-rome-bt-macos
- Commit: `ea3d365e418e327d540a8d5a25827eff1d40b893` (2026-10-30,
  "Revert \"Info.plist: inject Apple IOBluetoothHostControllerUSBTransport ...\"")
- Licence: GPL-2.0-or-later (SPDX headers in every source file; upstream ships
  no separate licence text, so `LICENSE` here carries the GPL-2.0 text).
- Imported with `git archive ea3d365 | tar -x` into `bluetooth/` (moved to `kexts/bluetooth/` later); the import
  commit is byte-identical to upstream except that `.github/` (upstream CI) is
  left out. Every later change is a separate commit in this repo, so
  `git diff fdde1f8 -- bluetooth/` plus `git log --follow kexts/bluetooth` show
  exactly what changed.

Firmware is not part of the snapshot: `scripts/gen_firmware_header.py` fetches
`qca/rampatch_usb_00000302.bin` and `qca/nvm_usb_00000302.bin` from
linux-firmware `d0039fd797d7776d89f00c22c45754914a96706e` and checks their
SHA-256 (licence and notices in `firmware/`).

Protocol reference: Linux `drivers/bluetooth/btusb_main.c`
(`btusb_setup_qca*`, GPL-2.0).

Not from that snapshot:

- `kext/QCARomeHCIFix/` is new code in this repo. Its choice of hook points follows IntelBTPatcher's Tahoe
  port (OpenIntelWireless/IntelBluetoothFirmware); no code is taken from it. It builds against the Lilu 1.7.2
  SDK and links its `plugin_start.cpp`, as NMHDA does; that SDK is fetched and sha256-checked at build time.
- `src/core/qca_hci_filter.c` follows Linux's behaviour (`btusb_qca_reset()`,
  `HCI_QUIRK_BROKEN_ENHANCED_SETUP_SYNC_CONN`, `hci_setup_sync_conn`) and the Bluetooth Core specification's
  command layouts; it is not derived from Linux source.
