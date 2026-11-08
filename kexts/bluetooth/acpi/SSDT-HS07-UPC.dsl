/*
 * SPDX-License-Identifier: GPL-2.0-only
 * SSDT-HS07-UPC — mark the Helios 300's internal Bluetooth port (xHCI 8086:a12f, USB 2 port 7 = HS07,
 * Lite-On 04ca:3016 on it per lsusb -t) as connectable and internal (_UPC type 0xFF).
 *
 * Acer's DSDT does have a _UPC on HS07, and it says "not connectable" (Return (UPCN)). That is wrong for this
 * port (the Bluetooth controller sits on it per lsusb -t). An SSDT cannot replace a method that already exists, so
 * this file's _UPC is a duplicate: expect ACPICA to report AE_ALREADY_EXISTS and keep the DSDT's. Whether macOS
 * applied it is unmeasured (kexts/bluetooth README). It is not needed once UTBMap.kext (docs/USB-MAP.md) sets
 * UsbConnector 255 on HS07. An earlier version of this comment said HS07 has no _UPC; that was wrong.
 */
DefinitionBlock ("", "SSDT", 2, "HELIOS", "HS07UPC", 0x00001000)
{
    External (_SB_.PCI0.XHC_.RHUB.HS07, DeviceObj)

    Scope (_SB.PCI0.XHC.RHUB.HS07)
    {
        Method (_UPC, 0, NotSerialized)  // _UPC: USB Port Capabilities
        {
            Return (Package (0x04)
            {
                One,    // connectable
                0xFF,   // proprietary connector: internal
                Zero,
                Zero
            })
        }
    }
}
