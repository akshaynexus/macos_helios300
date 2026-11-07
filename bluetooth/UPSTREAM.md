# Upstream

`bluetooth/` started as a snapshot of **QCARomePatchRAM** by duckducktw:

- Repository: https://github.com/duckducktw/qca-rome-bt-macos
- Commit: `ea3d365e418e327d540a8d5a25827eff1d40b893` (2026-10-30,
  "Revert \"Info.plist: inject Apple IOBluetoothHostControllerUSBTransport ...\"")
- Licence: GPL-2.0-or-later (SPDX headers in every source file; upstream ships
  no separate licence text, so `LICENSE` here carries the GPL-2.0 text).
- Imported with `git archive ea3d365 | tar -x` into `bluetooth/`; the import
  commit is byte-identical to upstream except that `.github/` (upstream CI) is
  left out. Every later change is a separate commit in this repo, so
  `git diff <import-commit> -- bluetooth/` shows exactly what changed.

Firmware is not part of the snapshot: `scripts/gen_firmware_header.py` fetches
`qca/rampatch_usb_00000302.bin` and `qca/nvm_usb_00000302.bin` from
linux-firmware `d0039fd797d7776d89f00c22c45754914a96706e` and checks their
SHA-256 (licence and notices in `firmware/`).

Protocol reference: Linux `drivers/bluetooth/btusb_main.c`
(`btusb_setup_qca*`, GPL-2.0).
