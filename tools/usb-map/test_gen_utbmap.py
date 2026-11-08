#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for gen_utbmap.py and the Helios port table. Run: python3 -I -B -m unittest discover -s tools/usb-map"""
import json
import os
import plistlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_utbmap  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def table(ports, pci_id="0xa12f8086"):
    return {"controller": {"pci_id": pci_id}, "ports": ports}


def port(name, number, connector=3):
    return {"name": name, "port": number, "connector": connector}


class HeliosTable(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(HERE, "helios300-ports.json")) as f:
            self.table = json.load(f)
        self.ports = {p["name"]: p for p in self.table["ports"]}

    def test_within_apple_limit(self):
        self.assertLessEqual(len(self.table["ports"]), gen_utbmap.MAX_PORTS)

    def test_internal_devices(self):
        self.assertEqual((self.ports["HS07"]["port"], self.ports["HS07"]["connector"]), (7, 255))
        self.assertEqual((self.ports["HS09"]["port"], self.ports["HS09"]["connector"]), (9, 255))

    def test_numbering_matches_the_dsdt(self):
        # HSnn = nn; SSnn = 16 + nn (SSPA() = 17 on this PCH).
        for name, p in self.ports.items():
            n = int(name[2:])
            self.assertEqual(p["port"], n if name.startswith("HS") else 16 + n, name)

    def test_every_acpi_connector_is_mapped(self):
        # ports the DSDT calls connectable, and the two internal devices
        for name in ("HS02", "SS02", "HS03", "SS03", "SS04", "HS05", "HS06", "HS07", "HS09"):
            self.assertIn(name, self.ports)

    def test_disabled_port_is_absent(self):
        # HS04's _STA returns 0 in the DSDT
        self.assertNotIn("HS04", self.ports)

    def test_candidates_are_labelled(self):
        for name, p in self.ports.items():
            self.assertIn(p["evidence"], ("acpi", "observed", "candidate"), name)
            self.assertTrue(p["what"], name)

    def test_plist_shape(self):
        info = gen_utbmap.build_plist(self.table)
        per = info["IOKitPersonalities"]["Controller-0"]
        self.assertEqual(per["IOClass"], "USBToolBox")
        self.assertEqual(per["IOProviderClass"], "IOPCIDevice")
        self.assertEqual(per["IOPCIPrimaryMatch"], "0xa12f8086")
        self.assertEqual(info["OSBundleLibraries"], {"com.dhinakg.USBToolBox.kext": "1.0.0"})
        merge = per["IOProviderMergeProperties"]
        self.assertEqual(merge["port-count"], struct.pack("<I", 22))
        self.assertEqual(merge["ports"]["HS07"]["port"], b"\x07\x00\x00\x00")
        self.assertEqual(merge["ports"]["SS02"]["port"], b"\x12\x00\x00\x00")
        self.assertEqual(merge["ports"]["HS07"]["UsbConnector"], 255)

    def test_cli_writes_a_kext_plist_can_read(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(gen_utbmap.main([d]), 0)
            with open(os.path.join(d, "UTBMap.kext", "Contents", "Info.plist"), "rb") as f:
                info = plistlib.load(f)
            self.assertEqual(info["CFBundleName"], "UTBMap")
            self.assertEqual(len(info["IOKitPersonalities"]["Controller-0"]["IOProviderMergeProperties"]["ports"]), 14)


class Refusals(unittest.TestCase):
    def assertRefuses(self, t, text):
        with self.assertRaises(gen_utbmap.TableError) as c:
            gen_utbmap.build_plist(t)
        self.assertIn(text, str(c.exception))

    def test_over_the_limit(self):
        self.assertRefuses(table([port("P%d" % i, i) for i in range(1, 17)]), "macOS allows 15")

    def test_exactly_the_limit_is_fine(self):
        info = gen_utbmap.build_plist(table([port("P%d" % i, i) for i in range(1, 16)]))
        self.assertEqual(len(info["IOKitPersonalities"]["Controller-0"]["IOProviderMergeProperties"]["ports"]), 15)

    def test_empty(self):
        self.assertRefuses(table([]), "no ports")

    def test_bad_controller(self):
        self.assertRefuses(table([port("A", 1)], pci_id="8086:a12f"), "pci_id")

    def test_repeated_name(self):
        self.assertRefuses(table([port("A", 1), port("A", 2)]), "repeated port name")

    def test_repeated_number(self):
        self.assertRefuses(table([port("A", 1), port("B", 1)]), "repeated port number")

    def test_bad_connector(self):
        self.assertRefuses(table([port("A", 1, 7)]), "connector")

    def test_bad_number(self):
        self.assertRefuses(table([port("A", 0)]), "port number")

    def test_cli_reports_bad_table(self):
        with tempfile.TemporaryDirectory() as d:
            bad = os.path.join(d, "bad.json")
            with open(bad, "w") as f:
                f.write("{not json")
            self.assertEqual(gen_utbmap.main(["--ports", bad, d]), 1)
            self.assertFalse(os.path.exists(os.path.join(d, "UTBMap.kext")))


if __name__ == "__main__":
    unittest.main()
