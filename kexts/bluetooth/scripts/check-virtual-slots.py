#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""check-virtual-slots.py: the virtual methods QCARomeHCIFix calls on classes it does not subclass.

kc-audit.py checks the vtables of classes a kext subclasses. It cannot see a call such as
device->reset() on an Apple object, which is compiled to a fixed vtable offset from the SDK header. If
the running macOS has that method in another slot, the plugin would call the wrong function, silently.
So compare the slot clang assigns from the SDK with the slot the real kernel collection has.
"""
import argparse
import os
import re
import subprocess
import sys

CALLS = [
    ("IOUSBHostDevice", "reset()", "reset"),
    ("IOUSBHostDevice", "getDeviceDescriptor()", "getDeviceDescriptor"),
    ("IOUSBHostIOSource", "getDevice() const", "getDevice"),  # clang dumps this one under IOUSBHostPipe
    ("IOUSBHostPipe", "getEndpointDescriptor(", "getEndpointDescriptor"),
]
SOURCE = """#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/usb/IOUSBHostIOSource.h>
void f(IOUSBHostDevice *d, IOUSBHostPipe *p, IOUSBHostIOSource *s)
{ d->reset(); d->getDeviceDescriptor(); p->getEndpointDescriptor(); s->getDevice(); }
"""


def sdk_slots(args):
    src = os.path.join(args.work, "slots.cpp")
    with open(src, "w") as f:
        f.write(SOURCE)
    kf = os.path.join(args.sdk, "System/Library/Frameworks/Kernel.framework/Headers")
    out = subprocess.run(
        ["clang", "-target", "x86_64-apple-macos26.0", "-isysroot", args.sdk, "-c", "-mkernel", "-nostdinc",
         "-nostdinc++", "-I" + kf, "-include", os.path.join(args.tools, "xnu-cross-compat.h"), "-DKERNEL",
         "-DKERNEL_PRIVATE", "-DDRIVER_PRIVATE", "-DAPPLE", "-DNeXT", "-x", "c++", "-std=c++17", "-fapple-kext",
         "-fno-rtti", "-fno-exceptions", "-Wno-deprecated-declarations", "-Wno-inconsistent-missing-override",
         "-Xclang", "-fdump-vtable-layouts", src, "-o", os.path.join(args.work, "slots.o")],
        capture_output=True, text=True)
    if out.returncode:
        sys.exit("check-virtual-slots: clang failed\n" + out.stderr[-2000:])
    slots, cls = {}, None
    # Each class's "Vtable for" table includes the two header words, so a method's slot is its index - 2.
    for line in out.stdout.splitlines():
        m = re.match(r"Vtable for '(\w+)'", line)
        if m:
            cls = m.group(1)
            continue
        if not line.strip():
            cls = None
        m = re.match(r"\s*(\d+) \| .*?\b(IOUSBHost\w+::\w+\(.*)$", line)
        if cls and m:
            slots.setdefault(cls, {})[m.group(2)] = int(m.group(1)) - 2
    return slots


def real_slots(args, cls):
    cmd = [sys.executable, "-I", os.path.join(args.tools, "kc-audit.py")]
    for kc in args.kc:
        cmd += ["--kc", kc]
    out = subprocess.run(cmd + ["--dump", cls], capture_output=True, text=True).stdout
    return {m.group(2).strip(): int(m.group(1)) for m in re.finditer(r"^\s*(\d+) (\S.*)$", out, re.M)}


def find(table, owner_hint, method):
    return [v for k, v in table.items() if method in k and k.split("::")[0].startswith("IOUSBHost")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tools", required=True)
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--kc", action="append", required=True)
    args = ap.parse_args()
    sdk = sdk_slots(args)
    bad = 0
    for cls, sig, name in CALLS:
        want = find(sdk.get("IOUSBHostPipe" if cls == "IOUSBHostIOSource" else cls, {}), cls, sig)
        have = find(real_slots(args, cls), cls, sig)
        if len(want) != 1 or len(have) != 1 or want != have:
            print("FAIL %s::%s: SDK slot %s, real collection %s" % (cls, name, want, have))
            bad = 1
        else:
            print("PASS %s::%s slot %d" % (cls, name, want[0]))
    return bad


if __name__ == "__main__":
    sys.exit(main())
