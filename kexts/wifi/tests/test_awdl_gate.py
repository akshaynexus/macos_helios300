#!/usr/bin/env python3
"""The AWDL/AirDrop research path stays behind AIRPORT_ATH10K_AWDL (off by default)."""
import os
import re
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


class AwdlGate(unittest.TestCase):
    def read(self, rel):
        with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
            return f.read()

    def test_default_off(self):
        control = self.read("src/shared/Ath10kControl.hpp")
        self.assertRegex(control, r"#ifndef AIRPORT_ATH10K_AWDL\n#define AIRPORT_ATH10K_AWDL 0")
        for script in ("native/scripts/cross-build-native.sh",):
            text = self.read(script)
            self.assertIn('AIRPORT_ATH10K_AWDL="${AWDL:-0}"', text)
            self.assertIn('if [ "${AWDL:-0}" = 1 ]', text)  # the AWDL sources only when asked for
            self.assertIn("native/src/AirportAth10kAwdlDriver.cpp", text.split('if [ "${AWDL:-0}" = 1 ]')[1])
        self.assertNotIn("AWDL=1", self.read("../../tools/release.sh"))

    def test_sources_are_gated(self):
        for rel in ("src/kext/Ath10kAwdlService.cpp", "src/kext/Ath10kAwdl.hpp",
                    "native/src/AirportAth10kAwdlDriver.cpp", "native/src/AirportAth10kAwdlDriver.hpp"):
            self.assertIn("AIRPORT_ATH10K_AWDL", self.read(rel), rel)
        wmi = self.read("src/kext/Ath10kWMI.hpp")
        self.assertEqual(len(re.findall(r"#if AIRPORT_ATH10K_AWDL", wmi)), 3)

    def test_gate_guards_every_core_hook(self):
        text = self.read("src/kext/Ath10kPCIDevice.cpp") + self.read("src/kext/Ath10kNetwork.cpp") + self.read("src/kext/Ath10kUserClient.cpp") + self.read("src/kext/Ath10kPower.cpp")
        hooks = [l for l in text.splitlines() if "_awdl" in l or "serviceAwdl" in l or "awdlControl" in l or "awdlOperationBase" in l]
        self.assertTrue(hooks)
        # each hook line sits in a gated block: strip gated blocks and find none left
        stripped = re.sub(r"#if AIRPORT_ATH10K_AWDL.*?#endif", "", text, flags=re.S)
        self.assertNotIn("serviceAwdl", stripped)
        self.assertNotIn("_awdl", stripped)
        self.assertNotIn("awdlControl", stripped)


if __name__ == "__main__":
    unittest.main()
