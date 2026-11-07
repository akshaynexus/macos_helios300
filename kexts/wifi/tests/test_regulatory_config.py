import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('regconfig', root / 'scripts/configure-regulatory.py')
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)


class RegulatoryConfigTests(unittest.TestCase):
    def test_both_boards_and_atomic_invalid_input(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp) / 'Info.plist'
            p.write_bytes((root / 'Ath10kMac.kext/Contents/Info.plist').read_bytes())
            config.configure(p, 'BR', True, 7)
            obj = plistlib.loads(p.read_bytes())
            for name in ('QCA6174', 'QCA9377'):
                board = obj['IOKitPersonalities'][name]
                self.assertEqual((board['Ath10kCountryCode'], board['Ath10kIndoor'], board['Ath10kTxPowerDbm']), ('BR', True, 7))
                self.assertFalse(board['Ath10kOpenConnect'])
            before = p.read_bytes()
            for country, power in [('br', 7), ('ZZ', 7), ('BR', 0), ('BR', 11)]:
                with self.assertRaises(ValueError):
                    config.configure(p, country, True, power)
                self.assertEqual(p.read_bytes(), before)
