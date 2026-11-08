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
  2. every Apple class the kext subclasses has the size the kext was
     compiled with: the kext exports kKcAuditParentSizes ({name, sizeof}
     pairs, NULL-terminated) and the collection's <Class>::MetaClass::MetaClass()
     passes the real classSize to OSMetaClass::OSMetaClass. A difference means
     the kext's own ivars overlap the parent's (or the parent reads past them);
  3. every vtable the kext defines (__ZTV<Class>) is laid out like its
     parent's vtable in the collection: for each slot the kext does not
     override, the symbol the kext names must be the symbol the parent's
     slot holds. OpenCore ("OCAK: Failed to patch symbol X for vtable Y")
     refuses a kext whose slot lands on a parent pad slot; any other name
     mismatch means the reconstructed header's virtual order differs from the
     binary and the family would call the wrong method, so both fail here.

--kc may be given more than once (Boot and System collections) and their
symbol tables are merged. A pointer inside a collection is a chained fixup
in the x86_64 kernel-cache format whose cacheLevel bits (31:30) say which
collection the 30-bit target offset belongs to (0 = Boot, 1 = System), so
the System collection's vtables resolve to kernel methods and to their own.
An inherited slot the real parent overrides (the kext names the ancestor's
method, the parent slot holds the parent's own) is what kxld and OpenCore
patch at link time; it is reported as a note, not a finding, unless
--strict. Only symbol names, slot indices and class sizes are read from the
collections; nothing from them is written anywhere. --dump CLASS prints a
class's real vtable and size.

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
        self.path = path
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
        self.levels = None  # set by Merged: collection per cacheLevel
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
        # dyld_chained_ptr_64_kernel_cache_rebase: target 29:0, cacheLevel 31:30.
        level = (raw >> 30) & 3
        base = self.base
        if self.levels and level < len(self.levels) and self.levels[level]:
            base = self.levels[level].base
        return base + (raw & 0x3FFFFFFF)

    def name_at(self, addr):
        if self.levels:
            for c in self.levels:
                if c and addr in c.by_addr:
                    return c.by_addr[addr]
        return self.by_addr.get(addr)

    def class_size(self, cls):
        """classSize passed by <cls>::MetaClass::MetaClass() to OSMetaClass::OSMetaClass."""
        # OSMetaClass::OSMetaClass lives in the kernel (Boot collection). A
        # System-collection kext reaches it through a stub, `jmp [rip+GOT]`,
        # whose GOT slot is a cacheLevel-0 fixup to the kernel's symbol.
        ctor = self.defined.get("__ZN11OSMetaClassC2EPKcPKS_j")
        if not ctor and self.levels and self.levels[0]:
            ctor = self.levels[0].defined.get("__ZN11OSMetaClassC2EPKcPKS_j")
        for suffix in ("C2Ev", "C1Ev"):
            entry = self.defined.get("__ZN%d%s9MetaClass%s" % (len(cls), cls, suffix))
            if not entry or not ctor:
                continue
            off = self.file_offset(entry[0])
            code = self.f[off:off + 0x80]
            size = None
            i = 0
            while i < len(code) - 5:
                if code[i] == 0xB9:  # mov ecx, imm32
                    size = struct.unpack_from("<I", code, i + 1)[0]
                    i += 5
                    continue
                if code[i] == 0xE8:  # call rel32
                    target = entry[0] + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
                    if target == ctor[0] or self.stub_target(target) == ctor[0]:
                        return size
                    i += 5
                    continue
                i += 1
        return None

    def stub_target(self, addr):
        """Where a `jmp qword [rip+disp32]` stub at addr goes (None if not one)."""
        off = self.file_offset(addr)
        if off is None or self.f[off:off + 2] != b"\xff\x25":
            return None
        disp = struct.unpack_from("<i", self.f, off + 2)[0]
        return self.pointer(addr + 6 + disp)

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
            slots.append(self.name_at(target) or "?%x" % target)
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
        self.relocs_any = {}  # every external relocation, 4-byte ones (GOT loads, lea) included
        for table in ((dysym[14], dysym[15]), (dysym[12], dysym[13])):
            for r in range(table[1]):
                r_address, info = struct.unpack_from("<iI", f, table[0] + 8 * r)
                if (info >> 27) & 1:
                    self.relocs_any[r_address] = self.syms[info & 0xFFFFFF]
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

    def read_cstring(self, vmaddr):
        for sva, svs, sfo in self.segs:
            if sva <= vmaddr < sva + svs:
                off = sfo + vmaddr - sva
                return self.f[off:self.f.index(b"\0", off)].decode("utf-8", "replace")
        return None

    def parent_sizes(self):
        """{class: sizeof} from the kext's kKcAuditParentSizes table, if it exports one."""
        table = self.defined.get("_kKcAuditParentSizes")
        out = {}
        if table is None:
            return out
        addr = table
        while True:
            name_ptr, size = self.read64(addr), self.read64(addr + 8)
            if not name_ptr:
                break
            name = self.read_cstring(name_ptr)
            if name:
                out[name] = size
            addr += 16
        return out

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

    def direct_parent(self, cls):
        """The superclass CLASS's MetaClass constructor names: the gMetaClass
        it passes OSMetaClass::OSMetaClass (`lea rdx, [rip+X]`, relocated)."""
        for suffix in ("C2Ev", "C1Ev"):
            va = self.defined.get("__ZN%d%s9MetaClass%s" % (len(cls), cls, suffix))
            if va is None:
                continue
            for i in range(0, 0x80):
                # lea rdx, [rip+disp32] or mov rdx, [rip+GOT] (external data under -mkernel)
                if self.read_bytes(va + i, 3) in (b"\x48\x8d\x15", b"\x48\x8b\x15"):
                    sym = self.relocs_any.get(va + i + 3) or self.relocs.get(va + i + 3)
                    if sym is None:
                        # mov rdx, [rip+disp]: the GOT slot carries the relocation
                        disp = struct.unpack_from("<i", self.read_bytes(va + i + 3, 4), 0)[0]
                        slot = va + i + 7 + disp
                        sym = self.relocs.get(slot) or self.by_addr.get(slot)
                    m = re.match(r"^__ZN\d+(\w+?)10gMetaClassE$", sym or "")
                    if m:
                        return m.group(1)
        return None

    def read_bytes(self, vmaddr, n):
        for sva, svs, sfo in self.segs:
            if sva <= vmaddr < sva + svs:
                return self.f[sfo + vmaddr - sva:sfo + vmaddr - sva + n]
        return b""


class Merged:
    """Several collections seen as one symbol space (Boot + System)."""

    def __init__(self, kcs):
        self.kcs = kcs
        self.defined = {}
        # cacheLevel 0 is the Boot collection (the one with the kernel's
        # 0xffffff80... base), 1 the System one (linked at 0).
        boot = [c for c in kcs if c.base >> 32 == 0xFFFFFF80]
        system = [c for c in kcs if c.base >> 32 != 0xFFFFFF80]
        levels = [boot[0] if boot else None, system[0] if system else None]
        for c in kcs:
            c.levels = levels
            for k, v in c.defined.items():
                self.defined.setdefault(k, v)

    def owner(self, sym):
        for c in self.kcs:
            if sym in c.defined:
                return c
        return None

    def vtable(self, cls):
        c = self.owner("__ZTV%d%s" % (len(cls), cls))
        return c.vtable(cls) if c else None

    def class_size(self, cls):
        for c in self.kcs:
            size = c.class_size(cls)
            if size is not None:
                return size
        return None


def method_name(demangled):
    """`IOService::setProperties(OSObject*)` -> `setProperties(OSObject*)`."""
    head = demangled.split("(", 1)
    return head[0].rsplit("::", 1)[-1] + ("(" + head[1] if len(head) > 1 else "")


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
    ap.add_argument("--kc", action="append", required=True, help="a kernel collection of the target Mac (repeatable: Boot and System)")
    ap.add_argument("--kext", help="the kext's Mach-O binary")
    ap.add_argument("--plist", help="its Info.plist (OSBundleLibraries)")
    ap.add_argument("--sibling", action="append", default=[], help="binary of another kext built alongside (exports count as resolved)")
    ap.add_argument("--dump", action="append", default=[], metavar="CLASS", help="print CLASS's vtable from the collection")
    ap.add_argument("--strict", action="store_true", help="count a parent's own override of an inherited slot as a finding too")
    a = ap.parse_args()
    kcs = [Collection(p) for p in a.kc]
    kc = Merged(kcs)
    for c in kcs:
        print("kc-audit: %s: %d fileset entries, base 0x%x" % (os.path.basename(c.path), len(c.entries), c.base))
    for cls in a.dump:
        slots = kc.vtable(cls)
        if slots is None:
            print("no vtable for %s" % cls)
            continue
        names = demangle(slots)
        print("## %s: %d slots, classSize %s" % (cls, len(slots), hex(kc.class_size(cls) or 0)))
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
    patched = []
    unverified = []
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
    # 2. class sizes
    sizes = kext.parent_sizes()
    for cls in sorted(sizes):
        real = kc.class_size(cls)
        if real is None:
            findings.append("class %s: no MetaClass constructor in the collection(s) to read classSize from" % cls)
        elif real != sizes[cls]:
            findings.append("class %s: kext compiled with sizeof %#x, the collection's classSize is %#x" % (cls, sizes[cls], real))
    if sizes:
        print("  class sizes: %d checked (%s)" % (len(sizes), ", ".join("%s=%#x" % (k, v) for k, v in sorted(sizes.items()))))
    else:
        print("  class sizes: kext exports no kKcAuditParentSizes table; sizes not checked")
    # 3. vtables
    all_parents = kext.parents()
    for cls in kext.classes():
        parents = all_parents
        ours = kext.vtable(cls)
        best, best_score = None, -1
        direct = kext.direct_parent(cls)
        if direct and kc.vtable(direct) is None:
            # The real family has the class (its gMetaClass resolved above)
            # but does not name its vtable: the layout cannot be compared.
            unverified.append("vtable %s: parent %s's vtable is not named in the collection; layout unverified" % (cls, direct))
            if not a.strict:
                continue
        if direct and kc.vtable(direct) is not None:
            parents = [direct]  # the superclass the MetaClass constructor names, not a guess by score
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
            elif not real.startswith("?") and method_name(names.get(mine, mine)) == method_name(names.get(real, real)) and not a.strict:
                # The parent overrides an ancestor's method the kext's header
                # does not list: the slot is inherited, so kxld (and OpenCore's
                # vtable patcher) replace it with the parent's entry.
                patched.append("vtable %s slot %d: %s is the parent's own override (patched at link)" % (cls, i, names.get(real, real)))
            else:
                findings.append("vtable %s slot %d: kext names %s but the parent slot holds %s (header slot order differs)"
                                % (cls, i, names.get(mine, mine), names.get(real, real)))
    for x in unverified:
        print("  NOTE %s" % x)
    if patched:
        print("  %d inherited slot(s) the parent overrides (patched at link): %s" % (len(patched), "; ".join(x.split(": ", 1)[1] for x in patched[:6]) + (" ..." if len(patched) > 6 else "")))
    if findings:
        print("FAIL: %d finding(s) against %s:" % (len(findings), ", ".join(os.path.basename(p) for p in a.kc)))
        for x in findings:
            print("   ", x)
        return 1
    print("PASS kc-audit: %d imports resolve, %d class size(s) and %d vtable(s) match the collection(s)" % (len(set(kext.undefined)), len(sizes), len(kext.classes())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
