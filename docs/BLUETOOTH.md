# Bluetooth on macOS 26 (Helios 300 G3-571)

Controller: Lite-On `04ca:3016`, Qualcomm QCA6174A "Rome 3.2" over USB
(xHCI port HS07). It needs a RAM firmware patch on every cold boot.

Driver: `kexts/bluetooth` (QCARomePatchRAM). It uploads the firmware and
leaves the controller to macOS's own stack (`bluetoothd`).

## OpenCore checklist
- Kernel → Add, in this order:
  1. Lilu
  2. BlueToolFixup (BrcmPatchRAM ≥ 2.7.2, for Tahoe's `bluetoothd`)
  3. QCARomePatchRAM
  4. QCARomeHCIFix (optional; `MinKernel` `25.0.0`)

  Set `MinKernel` `21.0.0` for BlueToolFixup and QCARomePatchRAM.
- NVRAM `7C436110-AB2A-4BBB-A880-FE41995C9F82`: set
  `bluetoothExternalDongleFailed` = `00` and
  `bluetoothInternalControllerInfo` = 14 zero bytes. List both keys in Add
  and in Delete.
- USB map: HS07, connector type 255 ([USB-MAP.md](USB-MAP.md)).
- No BrcmBluetoothInjector, BrcmPatchRAM3 or IntelBluetooth* kexts.
- No boot args required.

## Expected
- System Settings › Bluetooth works.
- BR/EDR and LE pairing works.
- A2DP works.
- Sleep/wake works.

Not available: Handoff, AirDrop and other Continuity features (they need
Apple Broadcom firmware).

Not yet measured on macOS: HFP microphone audio and sleep/wake. QCARomeHCIFix
translates the Enhanced SCO command Linux marks broken on this chip and resets a
controller whose firmware died; it has not been loaded on macOS yet. Boot args:
`-qcahcioff`, `-qcahcinosco`, `-qcahcinowd`.

Details, the sleep/wake review, coexistence test plan, rollback, the owner test
steps for macOS and the Linux cross-check evidence are in [`kexts/bluetooth/README.md`](../kexts/bluetooth/README.md).
