"""Measure real callback/reset code and exercise it on a minimum host stack.

This complements ASan (large user stacks), not a native macOS kext build.
Makefile applies the same frame budget to every production kernel TU in Clang.
"""
from pathlib import Path
import os, shlex, subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
class KernelStackTests(unittest.TestCase):
    def test_control_and_resets_have_bounded_frames(self):
        source=(ROOT/'src/kext/Ath10kUserClient.cpp').read_text()
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp);(folder/'IOKit').mkdir()
            (folder/'IOKit/IOTypes.h').write_text('#pragma once\n#include <cstdint>\nusing UInt8=uint8_t;using UInt16=uint16_t;using UInt32=uint32_t;using UInt64=uint64_t;\n')
            (folder/'control_method.inc').write_text(source[source.index('IOReturn Ath10kPCIDevice::controlAction('):])
            for opt in ('-O0','-O2'):
                for report in folder.glob('*.su'):report.unlink()
                target=folder/('stack'+opt[-1])
                cmd=shlex.split(os.environ.get('CXX','c++'))+[
                    '-std=c++17',opt,'-pthread','-fstack-usage',
                    '-Wall','-Wextra','-Werror','-Wframe-larger-than=4096',
                    '-I'+str(folder),'-I'+str(ROOT/'src/kext'),
                    str(ROOT/'tests/test_kernel_stack.cpp'),'-o',str(target)]
                build=subprocess.run(cmd,capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                run=subprocess.run([str(target)],capture_output=True,text=True)
                self.assertEqual(run.returncode,0,run.stderr)
                print(run.stdout.strip())
                rows=[]
                for report in folder.glob('*.su'):
                    for line in report.read_text().splitlines():
                        parts=line.rsplit('\t',2)
                        if len(parts)==3:rows.append((parts[0],int(parts[1]),parts[2]))
                self.assertTrue(any('controlAction' in name for name,_,_ in rows))
                for name,size,kind in rows:
                    self.assertLessEqual(size,4096,(opt,name,size))
                    self.assertNotEqual(kind,'dynamic',(opt,name,kind))
                print(opt, 'maximum measured frame:', max(n for _,n,_ in rows),'bytes')
