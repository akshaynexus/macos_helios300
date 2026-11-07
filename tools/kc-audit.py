#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""kc-audit.py — check a cross-built kext against a real macOS kernel collection.

Given BootKernelExtensions.kc (an MH_FILESET) from the Mac that will run the
kext, verify what kxld/OpenCore will do at injection time:

  1. every undefined symbol of the kext is defined by a fileset entry whose
     bundle id the kext's OSBundleLibraries names (com.apple.kpi.* map to
     com.apple.kernel), or by a sibling binary built with it (--sibling);
     pad slots (_RESERVED<Class><N>) that only live in vtables are resolved by
     vtable patching and are accepted when the parent slot holds a method;
  2. every vtable the kext defines (__ZTV<Class>) is laid out like its
     parent's vtable in the collection: for each slot the kext does not
     override, the symbol the kext names must be the symbol the parent's
     slot holds. OpenCore ("OCAK: Failed to patch symbol X for vtable Y")
     refuses a kext whose slot lands on a parent pad slot; any other name
     mismatch means the reconstructed header's virtual order differs from the
     binary and the family would call the wrong method, so both fail here.

Only symbol names and slot indices are read from the collection; nothing from
it is written anywhere. --dump CLASS prints a class's real vtable.

Exit 0 pass, 1 findings, 2 usage. Kext vtable slots in the collection are
chained fixups in the x86_64 kernel-cache format (target = low 30 bits,
relative to the collection base); the kernel's own vtables hold plain
addresses. Both are what BootKernelExtensions.kc uses on Intel Macs.
"""
import argparse
import os
import plistlib
import re
import struct
import subprocess
import sys

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xb
LC_FILESET_ENTRY = 0x80000035
PURE = "___cxa_pure_virtual"


def cstr(buf, off):
    return buf[off:buf.index(b"\0", off)].decode("utf-8", "replace")


def pad_slot(name):
    return bool(re.match(r"^__ZN\d+\w+?_RESERVED\w+\d+Ev$", name)) or "_RESERVED" in name


class Collection:
    def __init__(self, path):
        self.f = open(path, "rb").read()
        f = self.f
        magic, _, _, ft, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", f, 0)
        if magic != 0xFEEDFACF or ft != 12:
            raise SystemExit("kc-audit: %s is not an MH_FILESET kernel collection" % path)
        self.entries, self.segs = {}, []
        off = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", f, off)
            if cmd == LC_FILESET_ENTRY:
                vmaddr, fileoff, nameoff, _ = struct.unpack_from("<QQII", f, off + 8)
                self.entries[cstr(f, off + nameoff)] = (vmaddr, fileoff)
            elif cmd == LC_SEGMENT_64:
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", f, off + 24)
                self.segs.append((vmaddr, vmsize, fileoff))
            off += size
        self.base = min(s[0] for s in self.segs)
        self.defined = {}   # symbol -> (address, bundle)
        self.by_addr = {}   # address -> symbol
        for bundle, (_, fileoff) in self.entries.items():
            self.load_symbols(bundle, fileoff)

    def load_symbols(self, bundle, fileoff):
        f = self.f
        ncmds = struct.unpack_from("<I", f, fileoff + 16)[0]
        off = fileoff + 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", f, off)
            if cmd == LC_SYMTAB:
                symoff, nsyms, stroff, _ = struct.unpack_from("<IIII", f, off + 8)
                for i in range(nsyms):
                    n_strx, n_type, _, _, n_value = struct.unpack_from("<IBBHQ", f, symoff + 16 * i)
                    if n_type & 0x0E and n_value:
                        name = cstr(f, stroff + n_strx)
                        self.defined.setdefault(name, (n_value, bundle))
                        self.by_addr.setdefault(n_value, name)
            off += size

    def file_offset(self, vmaddr):
        for sva, svs, sfo in self.segs:
            if sva <= vmaddr < sva + svs:
                return sfo + (vmaddr - sva)
        return None

    def pointer(self, vmaddr):
        off = self.file_offset(vmaddr)
        if off is None:
            return None
        raw = struct.unpack_from("<Q", self.f, off)[0]
        if raw == 0:
            return 0
        if raw >> 32 == 0xFFFFFF80:
            return raw  # the kernel's own vtables hold plain kernel addresses
        return self.base + (raw & 0x3FFFFFFF)  # chained fixup, x86_64 kernel-cache format

    def vtable(self, cls):
        """Slot symbols after the two header words, until the next symbol or a null."""
        entry = self.defined.get("__ZTV%d%s" % (len(cls), cls))
        if not entry:
            return None
        va, slots, i = entry[0], [], 2
        while True:
            addr = va + 8 * i
            if i > 2 and addr in self.by_addr:
                break
            target = self.pointer(addr)
            if not target:
                break
            slots.append(self.by_addr.get(target, "?%x" % target))
            i += 1
        return slots


class Kext:
    """A linked MH_KEXT_BUNDLE (x86_64): symbols and relocation-driven vtable slots."""

    def __init__(self, path):
        self.f = open(path, "rb").read()
        f = self.f
        magic, _, _, ft, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", f, 0)
        if magic != 0xFEEDFACF or ft != 11:
            raise SystemExit("kc-audit: %s is not an MH_KEXT_BUNDLE" % path)
        self.segs, symtab, dysym = [], None, None
        off = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", f, off)
            if cmd == LC_SEGMENT_64:
                vmaddr, vmsize, fileoff, _ = struct.unpack_from("<QQQQ", f, off + 24)
                self.segs.append((vmaddr, vmsize, fileoff))
            elif cmd == LC_SYMTAB:
                symtab = struct.unpack_from("<IIII", f, off + 8)
            elif cmd == LC_DYSYMTAB:
                dysym = struct.unpack_from("<18I", f, off + 8)
            off += size
        symoff, nsyms, stroff, _ = symtab
        self.syms, self.defined, self.undefined, self.by_addr = [], {}, [], {}
        for i in range(nsyms):
            n_strx, n_type, _, _, n_value = struct.unpack_from("<IBBHQ", f, symoff + 16 * i)
            name = cstr(f, stroff + n_strx)
            self.syms.append(name)
            if n_type & 0x0E:
                self.defined[name] = n_value
                self.by_addr.setdefault(n_value, name)
            elif n_type & 0x01 and not (n_type & 0xE0):
                self.undefined.append(name)
        # ld64 -kext keeps every relocation in the "local" table, external ones
        # included; r_address is relative to the first segment (vmaddr 0).
        self.relocs = {}
        for table in ((dysym[14], dysym[15]), (dysym[12], dysym[13])):
            for r in range(table[1]):
                r_address, info = struct.unpack_from("<iI", f, table[0] + 8 * r)
                if (info >> 25) & 3 != 3:
                    continue
                if (info >> 27) & 1:
                    self.relocs[r_address] = self.syms[info & 0xFFFFFF]
                else:
                    self.relocs[r_address] = self.by_addr.get(self.read64(r_address), "?%x" % self.read64(r_address))

    def read64(self, vmaddr):
        for sva, svs, sfo in self.segs:
            if sva <= vmaddr < sva + svs:
                return struct.unpack_from("<Q", self.f, sfo + vmaddr - sva)[0]
        return 0

    def classes(self):
        return [m.group(1) for n in self.defined for m in [re.match(r"^__ZTV\d+(\w+)$", n)] if m]

    def vtable(self, cls):
        va, slots, i = self.defined["__ZTV%d%s" % (len(cls), cls)], [], 2
        while True:
            addr = va + 8 * i
            if i > 2 and addr in self.by_addr:
                break
            if addr in self.relocs:
                slots.append(self.relocs[addr])
            elif self.read64(addr) == 0:
                break
            else:  # the kext's own method: ld64 left an absolute address, no relocation
                slots.append(self.by_addr.get(self.read64(addr), "?%x" % self.read64(addr)))
            i += 1
        return slots

    def parents(self):
        """Classes whose gMetaClass the kext references: the direct superclasses."""
        return [m.group(1) for n in self.undefined for m in [re.match(r"^__ZN\d+(\w+?)10gMetaClassE$", n)] if m]


def demangle(names):
    stripped = [n[1:] if n.startswith("_") else n for n in names]
    try:
        out = subprocess.run(["llvm-cxxfilt"], input="\n".join(stripped) + "\n",
                             capture_output=True, text=True, check=True).stdout.splitlines()
        if len(out) == len(names):
            return dict(zip(names, out))
    except (OSError, subprocess.CalledProcessError):
        pass
    return dict(zip(names, stripped))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kc", required=True, help="BootKernelExtensions.kc of the target Mac")
    ap.add_argument("--kext", help="the kext's Mach-O binary")
    ap.add_argument("--plist", help="its Info.plist (OSBundleLibraries)")
    ap.add_argument("--sibling", action="append", default=[], help="binary of another kext built alongside (exports count as resolved)")
    ap.add_argument("--dump", action="append", default=[], metavar="CLASS", help="print CLASS's vtable from the collection")
    a = ap.parse_args()
    kc = Collection(a.kc)
    print("kc-audit: %s: %d fileset entries, base 0x%x" % (os.path.basename(a.kc), len(kc.entries), kc.base))
    for cls in a.dump:
        slots = kc.vtable(cls)
        if slots is None:
            print("no vtable for %s" % cls)
            continue
        names = demangle(slots)
        print("## %s: %d slots" % (cls, len(slots)))
        for i, s in enumerate(slots):
            print("%4d %s" % (i, names[s]))
    if not a.kext:
        return 0
    if not a.plist:
        ap.error("--plist is required with --kext")
    kext = Kext(a.kext)
    libs = plistlib.load(open(a.plist, "rb")).get("OSBundleLibraries", {})
    allowed = {b for b in libs if not b.startswith("com.apple.kpi.")}
    if any(b.startswith("com.apple.kpi.") for b in libs):
        allowed.add("com.apple.kernel")
    sibling_defs = set()
    for s in a.sibling:
        sibling_defs |= set(Kext(s).defined)
    findings = []
    # 1. imports
    vtable_symbols = set()
    for cls in kext.classes():
        vtable_symbols |= set(kext.vtable(cls))
    for name in sorted(set(kext.undefined)):
        if name in sibling_defs:
            continue
        entry = kc.defined.get(name)
        if entry and entry[1] in allowed:
            continue
        if entry:
            findings.append("import %s is exported by %s, not in OSBundleLibraries" % (name, entry[1]))
        elif pad_slot(name) and name in vtable_symbols:
            continue  # resolved by vtable patching if the parent slot holds a method (checked below)
        else:
            findings.append("import %s is not defined in the collection" % name)
    # 2. vtables
    parents = kext.parents()
    for cls in kext.classes():
        ours = kext.vtable(cls)
        best, best_score = None, -1
        for p in parents:
            theirs = kc.vtable(p)
            if theirs is None:
                continue
            score = sum(1 for i, s in enumerate(ours[:len(theirs)]) if s == theirs[i])
            if score > best_score:
                best, best_score = p, score
        if best is None:
            findings.append("vtable %s: no parent vtable found in the collection (candidates: %s)" % (cls, ", ".join(parents) or "none"))
            continue
        theirs = kc.vtable(best)
        print("  vtable %s: %d slots, parent %s has %d slots in the collection" % (cls, len(ours), best, len(theirs)))
        if len(ours) < len(theirs):
            findings.append("vtable %s has %d slots, parent %s has %d" % (cls, len(ours), best, len(theirs)))
        names = demangle(list(set(ours) | set(theirs)))
        for i, mine in enumerate(ours):
            if i >= len(theirs):
                break
            real = theirs[i]
            if mine in kext.defined or mine == PURE or mine == real:
                continue
            if pad_slot(mine) and not pad_slot(real):
                continue  # kext built against older headers: OpenCore patches it
            if pad_slot(real):
                findings.append("vtable %s slot %d: kext names %s but the parent slot is the pad %s (OpenCore refuses this)"
                                % (cls, i, names.get(mine, mine), names.get(real, real)))
            else:
                findings.append("vtable %s slot %d: kext names %s but the parent slot holds %s (header slot order differs)"
                                % (cls, i, names.get(mine, mine), names.get(real, real)))
    if findings:
        print("FAIL: %d finding(s) against %s:" % (len(findings), os.path.basename(a.kc)))
        for x in findings:
            print("   ", x)
        return 1
    print("PASS kc-audit: %d imports resolve and %d vtable(s) match the collection" % (len(set(kext.undefined)), len(kext.classes())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
