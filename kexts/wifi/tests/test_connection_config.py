import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('config', ROOT / 'scripts/configure-open-network.py')
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)

class ConnectionConfigTests(unittest.TestCase):
    def test_explicit_configuration_and_disable(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'Info.plist'
            p.write_bytes((ROOT / 'Ath10kMac.kext/Contents/Info.plist').read_bytes())
            config.configure(p, 'Rede teste', '02:11:22:33:44:55', 5)
            obj = plistlib.loads(p.read_bytes())
            self.assertEqual(obj['CFBundleIdentifier'], 'br.com.jonasw8.driver.ath10kmac')
            for personality in obj['IOKitPersonalities'].values():
                self.assertTrue(personality['Ath10kOpenConnect'])
                self.assertEqual(personality['Ath10kConnectSSID'], b'Rede teste')
                self.assertEqual(personality['Ath10kConnectBSSID'], bytes.fromhex('021122334455'))
                self.assertEqual(personality['Ath10kConnectPowerHalfDbm'], 10)
            config.configure(p, disable=True)
            for personality in plistlib.loads(p.read_bytes())['IOKitPersonalities'].values():
                self.assertFalse(personality['Ath10kOpenConnect'])
                self.assertNotIn('Ath10kConnectSSID', personality)

    def test_invalid_input_does_not_modify_plist(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'Info.plist'
            original = (ROOT / 'Ath10kMac.kext/Contents/Info.plist').read_bytes()
            p.write_bytes(original)
            for ssid, bssid, power in [('', '02:11:22:33:44:55', 10),
                ('é'*17, '02:11:22:33:44:55', 10), ('TEST', 'ff:ff:ff:ff:ff:ff', 10),
                ('TEST', '00:00:00:00:00:00', 10), ('TEST', 'x', 10),
                ('TEST', '02:11:22:33:44:55', 21)]:
                with self.assertRaises(ValueError):
                    config.configure(p, ssid, bssid, power)
                self.assertEqual(p.read_bytes(), original)
