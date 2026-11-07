import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location('symbols', Path(__file__).parents[1] / 'scripts/check-kext-symbols.py')
symbols = importlib.util.module_from_spec(spec)
spec.loader.exec_module(symbols)

class SymbolTests(unittest.TestCase):
    def test_captured_probe(self):
        row = '                 (undefined) weak external ____chkstk_darwin (dynamically looked up)'
        self.assertEqual(symbols.rejected_symbols(row), [row.strip()])
    def test_defined_probe_and_kernel_import(self):
        self.assertEqual(symbols.rejected_symbols('000000a670 (__TEXT,__text) non-external ___chkstk_darwin\n (undefined) external _IOLog (dynamically looked up)'), [])
    def test_other_weak_and_strong_probe(self):
        self.assertEqual(len(symbols.rejected_symbols('(undefined) weak external _optional\n(undefined) external ___stack_chk_fail\n(undefined) external ___chkstk_darwin')), 3)

if __name__ == '__main__':
    unittest.main()
