"""The front end must start the BSD interface's output thread (hardware run 7).

tests/test_native_tx.cpp proves the TX path with AirportAth10kTx.hpp against a
model of IONetworkingFamily 3.4; this checks that AirportAth10k.cpp actually
wires that logic to the SPI: link-up/down go through setOutputThread, stop()
halts the thread before the core detaches, outputStart uses AirportTx::pull,
and (when a build exists) the binary imports start/stop/flushOutputThread.
"""
from pathlib import Path
import re
import shutil
import subprocess
import unittest

ROOT = Path(__file__).parents[1]
SOURCE = ROOT / 'native/src/AirportAth10k.cpp'
BINARY = ROOT / 'build/native/AirportAth10k.kext/Contents/MacOS/AirportAth10k'
SPI = ('__ZN18IONetworkInterface17startOutputThreadEj',
       '__ZN18IONetworkInterface16stopOutputThreadEj',
       '__ZN18IONetworkInterface16flushOutputQueueEj')


def body(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 0
    for i in range(brace, len(text)):
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        if depth == 0:
            return text[brace:i + 1]
    raise AssertionError(signature)


class NativeTxWiring(unittest.TestCase):
    def setUp(self):
        self.text = SOURCE.read_text()

    def test_spi_bound_by_mangled_name(self):
        for symbol in SPI:
            self.assertIn('__asm__("%s")' % symbol, self.text)

    def test_link_drives_output_thread(self):
        self.assertIn('setOutputThread(up);', body(self.text, 'void AirportAth10k::nativeLink(bool up)'))
        thread = body(self.text, 'void AirportAth10k::setOutputThread(bool up)')
        for call in ('airportStartOutputThread(', 'airportStopOutputThread(', 'airportFlushOutputQueue(', 'outputThread.link('):
            self.assertIn(call, thread)

    def test_stop_halts_before_detach(self):
        stop = body(self.text, 'void AirportAth10k::stop(IOService *provider)')
        self.assertLess(stop.index('setOutputThread(false)'), stop.index('nativeDetachInterface()'))

    def test_output_start_uses_pull(self):
        self.assertIn('AirportTx::pull(', body(self.text, 'IOReturn AirportAth10k::outputStart('))

    def test_binary_imports(self):
        nm = shutil.which('llvm-nm') or shutil.which('nm')
        if not BINARY.exists() or not nm:
            self.skipTest('no cross-built AirportAth10k')
        out = subprocess.run([nm, '-u', str(BINARY)], capture_output=True, text=True, check=True).stdout
        for symbol in SPI:
            self.assertTrue(re.search(r'\b%s\b' % re.escape(symbol), out), symbol)


if __name__ == '__main__':
    unittest.main()
