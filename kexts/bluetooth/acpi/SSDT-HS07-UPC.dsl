/*
 * SPDX-License-Identifier: GPL-2.0-only
 * SSDT-HS07-UPC — mark the Helios 300's internal Bluetooth port (xHCI 8086:a12f, USB 2 port 7 = HS07,
 * Lite-On 04ca:3016 on it per lsusb -t) as connectable and internal (_UPC type 0xFF).
 *
 * Acer's DSDT gives only HS09 (the webcam) a _UPC; HS07 has none, so macOS has no connector type for the
 * Bluetooth controller and treats it as an external dongle (kexts/bluetooth/README.md, "USB map"). With the
 * USBToolBox + UTBDefault map 1401's engine builds (no per-port dictionary), macOS takes the port type from
 * ACPI, so this one method is the whole fix; no other port is touched. The package is the same one the DSDT's
 * own GUPC(One) returns for HS09. It is the hardware's true description, so it applies to every OS.
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
