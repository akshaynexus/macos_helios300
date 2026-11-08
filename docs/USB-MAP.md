# USB port map (Helios 300 G3-571)

The release currently ships `UTBDefault` + `SSDT-HS07-UPC`. `UTBDefault` keeps the first 15 ports, which are all
USB 2, so nothing reaches USB 3 speed. A real map fixes that.

`tools/usb-map/gen_utbmap.py` builds `UTBMap.kext` (code-less, needs USBToolBox.kext) from
`tools/usb-map/helios300-ports.json`:

```sh
python3 -I tools/usb-map/gen_utbmap.py OUTDIR
python3 -I -B -m unittest discover -s tools/usb-map
```

**State: generated from Linux and ACPI data, never booted.** It is not in the release yet. See
[what the map cannot know](#what-the-data-cannot-settle).

## What the data says

Sources, all from this laptop: the DSDT (`_UPC`, `_PLD`, `_STA`, `USRA()`, `SSPA()`) and `lsusb -t` under Linux.

Port numbers: `HSnn` = `nn`, `USR1/2` = 15/16, `SSnn` = 16 + `nn`. `SSPA()` returns 17 on this Sunrise Point-H
chipset, and Linux reports a 16-port USB 2 root hub.

| Port | DSDT says | Observed | In the map as |
|---|---|---|---|
| HS02 + SS02 | connectable, type 3 (USB 3 A), same `_PLD` | | USB 3 A |
| HS03 + SS03 + SS04 | connectable, type 0x0A (USB-C), same `_PLD` | | USB-C |
| HS05 | connectable, type 0 (USB 2 A) | | USB 2 A |
| HS06 | connectable, type 0 (USB 2 A) | | USB 2 A |
| HS07 | **not connectable** | Bluetooth 04ca:3016 | internal (255) |
| HS09 | connectable, type 0xFF | webcam 0408:a060 | internal (255) |
| HS04 | `_STA` returns 0 | | left out |
| HS01, HS08 | not connectable | | kept as candidates |
| SS01, SS05, SS06 | not connectable | | kept as candidates |
| HS10-14, USR1/2, SS07-10 | not connectable | | left out |

That is 14 ports, inside Apple's limit of 15.

The card reader is not USB: it is the Realtek RTL8411B on PCIe (`10ec:5287`), so it has no port here.

## What the data cannot settle

- ACPI shows four outside connectors, but two of them as USB 2 only. If the laptop has a second USB 3 Type-A socket,
  ACPI does not say which SS port is its USB 3 lane. HS07 shows this DSDT's port data is not fully reliable.
- So SS01, SS05 and SS06 are in the map as candidates, and any socket on a port left out would stop working
  under macOS.

## What to capture on the Mac

Do this once UTBMap.kext is installed (with `UTBDefault` the SS ports are not visible at all).

For **each outside socket**, with a USB 3 stick and then, if you have one, a USB 2 device:

```sh
ioreg -rlw0 -c AppleUSBHostPort | grep -E '^\+-o|"UsbConnector"|"port" =|"USB Product Name"|"Device Speed"'
```

Note which socket it was and which `HSnn`/`SSnn` shows the device. Also record, once, with nothing plugged in:

```sh
ioreg -rlw0 -c AppleUSBHostPort | grep -E '^\+-o|"UsbConnector"|"port" ='
kextstat | grep -iE 'USBToolBox|UTB'
```

Remove serial numbers before sharing. Check that Bluetooth and the webcam still show up, since both ports are
in the map.

Then in `helios300-ports.json`: set each socket's real `connector`, delete ports nothing lived on (candidates first),
and keep every entry that a device appeared on. `docs/MACOS-STATUS.md` gets the result.

Helios 300 Setup's "Copy diagnostics" does not collect this yet; the commands above are the capture until it does.
