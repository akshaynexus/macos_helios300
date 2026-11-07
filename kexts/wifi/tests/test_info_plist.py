"""The kext matches exactly the Helios 300 card and reports one version everywhere."""
from pathlib import Path
import plistlib
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InfoPlistTests(unittest.TestCase):
    def setUp(self):
        self.plist = plistlib.loads((ROOT / 'Ath10kMac.kext/Contents/Info.plist').read_bytes())

    def test_matches_only_the_acer_card(self):
        personalities = self.plist['IOKitPersonalities']
        self.assertEqual(list(personalities), ['QCA6174-Acer-11ad-0807'])
        p = personalities['QCA6174-Acer-11ad-0807']
        self.assertNotIn('IOPCIMatch', p)  # would match any subsystem
        # IOPCIPrimaryMatch = device<<16 | vendor; IOPCISecondaryMatch = subdevice<<16 | subvendor.
        self.assertEqual(p['IOPCIPrimaryMatch'], '0x003E168C')
        self.assertEqual(p['IOPCISecondaryMatch'], '0x080711AD')
        self.assertEqual(p['IOClass'], 'Ath10kPCIDevice')
        self.assertEqual(p['IOProviderClass'], 'IOPCIDevice')
        self.assertIs(p['Ath10kOpenConnect'], False)

    def test_version_is_consistent(self):
        version = self.plist['CFBundleVersion']
        self.assertRegex(version, r'^\d+\.\d+\.\d+$')
        self.assertEqual(self.plist['CFBundleShortVersionString'], version)
        header = (ROOT / 'src/kext/Ath10kVersion.hpp').read_text()
        self.assertIn(f'#define ATH10K_VERSION "{version}"', header)
        kmod = (ROOT / 'src/kext/kmod_info.c').read_text()
        self.assertIn(f'"{version}"', kmod)
        for path in (ROOT / 'src').rglob('*.[ch]pp'):
            self.assertIsNone(re.search(r'"0\.0\.9\d"', path.read_text()), path)

    def test_no_private_kpi(self):
        self.assertNotIn('com.apple.kpi.private', self.plist['OSBundleLibraries'])


if __name__ == '__main__':
    unittest.main()
