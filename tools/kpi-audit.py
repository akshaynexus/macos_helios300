#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""kpi-audit.py — check every undefined symbol of a kext against what macOS exports.

Each import must be either
  * exported by an xnu KPI the kext declares in OSBundleLibraries
    (xnu config/<KPI>[.x86_64][.MacOSX].exports for com.apple.kpi.*), or
  * covered by an allowlist entry for a declared non-xnu family
    (IOUSBHostFamily, IONetworkingFamily, IOPCIFamily, ...).

Allowlist file format (one entry per line, '#' comments):
  sym <bundle-id> <mangled-symbol>     exact symbol
  re  <bundle-id> <python-regex>       full-match on the mangled symbol
  hdr <bundle-id> <header>             symbol must demangle to a class/function
                                       declared in <header> (path relative to
                                       Kernel.framework/Headers, or absolute):
                                       'vtable for C', 'C::gMetaClass',
                                       'C::metaClass', 'C::superClass' need
                                       'class/struct/namespace C' declared there; 'C::m(...)'
                                       also needs an 'm(' declaration in the
                                       same header; a C function 'f' needs 'f('.
                                       Constructors/destructors from
                                       OSDeclare*Structors(C) and C::_RESERVEDC<N>
                                       from OSMetaClassDeclareReserved*(C, N)
                                       are accepted when those macros are there.
The header check matches names, not full signatures.

Exit 0 = every import resolved; 1 = at least one unresolved (listed).
"""
import argparse
import os
import plistlib
import re
import subprocess
import sys

KPI_FILES = {
    "com.apple.kpi.bsd": "BSDKernel",
    "com.apple.kpi.iokit": "IOKit",
    "com.apple.kpi.libkern": "Libkern",
    "com.apple.kpi.mach": "Mach",
    "com.apple.kpi.unsupported": "Unsupported",
    "com.apple.kpi.dsep": "MACFramework",
    "com.apple.kpi.private": "Private",
}


def load_exports(cfg, base):
    syms = set()
    for suf in ("", ".x86_64", ".x86_64.MacOSX", ".MacOSX"):
        f = os.path.join(cfg, base + suf + ".exports")
        if os.path.exists(f):
            for line in open(f, encoding="utf-8", errors="replace"):
                s = line.split(":")[0].strip()
                if s and not s.startswith("#"):
                    syms.add(s)
    return syms


def demangle(names):
    """Mach-O symbol -> demangled text (C symbols: name without the '_')."""
    if not names:
        return {}
    # Mach-O prefixes every symbol with '_'; strip it and demangle Itanium.
    stripped = [n[1:] if n.startswith("_") else n for n in names]
    for tool in ("llvm-cxxfilt", "c++filt"):
        try:
            out = subprocess.run([tool], input="\n".join(stripped) + "\n",
                                 capture_output=True, text=True, check=True).stdout.splitlines()
            break
        except (OSError, subprocess.CalledProcessError):
            out = None
    if not out or len(out) != len(names):
        return {n: s for n, s in zip(names, stripped)}
    return dict(zip(names, out))


def parse_demangled(d):
    """Return (class or None, member or None)."""
    m = re.match(r"^(?:vtable|typeinfo|VTT) for (?:\w+::)*(\w+)$", d)
    if m:
        return m.group(1), None
    m = re.match(r"^(?:[\w:]+::)?(\w+)::(\w+|~\w+)(?:\(.*)?$", d)
    if m:
        cls, mem = m.group(1), m.group(2)
        if mem in ("gMetaClass", "metaClass", "superClass"):
            return cls, None
        return cls, mem
    m = re.match(r"^(\w+)(?:\(.*)?$", d)
    if m:
        return None, m.group(1)
    return None, None


def header_has(text, cls, mem):
    if cls and not re.search(r"\b(?:class|struct|namespace)\s+(?:\w+\s+)*" + re.escape(cls) + r"\s*(?::|\{|\n\s*[:{])", text):
        return False
    if cls and mem:
        # Members the SDK declares only through libkern macros:
        # OSDeclare{Default,Abstract,Final}Structors(C) -> C::C(), C::~C();
        # OSMetaClassDeclareReserved{Unused,Used}(C, N) -> C::_RESERVEDC<N>().
        c = re.escape(cls)
        if mem in (cls, "~" + cls) and re.search(
                r"\bOSDeclare(?:Default|Abstract|Final)?Structors\w*\s*\(\s*" + c + r"\s*[,)]", text):
            return True
        m = re.fullmatch("_RESERVED" + c + r"(\d+)", mem)
        if m and re.search(r"\bOSMetaClassDeclareReserved(?:Unused|Used)\w*\s*\(\s*" + c +
                           r"\s*,\s*" + m.group(1) + r"\s*\)", text):
            return True
    if mem:
        name = mem.lstrip("~")
        if not re.search(r"[\s~*&]" + re.escape(name) + r"\s*\(", text):
            return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xnu-config", required=True)
    ap.add_argument("--sdk-headers", required=True, help="Kernel.framework/Headers")
    ap.add_argument("--plist", required=True)
    ap.add_argument("--imports", required=True)
    ap.add_argument("--allow", action="append", default=[])
    a = ap.parse_args()

    libs = plistlib.load(open(a.plist, "rb")).get("OSBundleLibraries", {})
    if "com.apple.kpi.private" in libs:
        print("FAIL: kext declares com.apple.kpi.private (Apple-only)")
        return 1

    declared, undeclared = {}, {}
    for bundle, base in KPI_FILES.items():
        for s in load_exports(a.xnu_config, base):
            (declared if bundle in libs else undeclared).setdefault(s, bundle)

    exact, regexes, headers = {}, [], []
    for path in a.allow:
        for ln, line in enumerate(open(path, encoding="utf-8"), 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split(None, 2)
            if len(parts) != 3 or parts[0] not in ("sym", "re", "hdr"):
                print(f"FAIL: {path}:{ln}: bad allowlist line")
                return 1
            kind, bundle, arg = parts
            if bundle not in libs:
                print(f"FAIL: {path}:{ln}: {bundle} is not in OSBundleLibraries")
                return 1
            if kind == "sym":
                exact[arg] = bundle
            elif kind == "re":
                regexes.append((re.compile(arg), bundle))
            else:
                hp = arg if os.path.isabs(arg) else os.path.join(a.sdk_headers, arg)
                if not os.path.exists(hp):
                    print(f"FAIL: {path}:{ln}: header {hp} not found")
                    return 1
                headers.append((open(hp, encoding="utf-8", errors="replace").read(), bundle, arg))

    syms = [l.strip() for l in open(a.imports) if l.strip()]
    pending = [s for s in syms if s not in declared and s not in exact
               and not any(r.fullmatch(s) for r, _ in regexes)]
    dem = demangle(pending)
    counts, bad = {}, []
    for s in syms:
        b = declared.get(s) or exact.get(s)
        how = ""
        if not b:
            for r, rb in regexes:
                if r.fullmatch(s):
                    b = rb
                    break
        if not b and headers:
            cls, mem = parse_demangled(dem.get(s, s))
            if cls or mem:
                for text, hb, hname in headers:
                    if header_has(text, cls, mem):
                        b, how = hb, hname
                        break
        if b:
            counts[b] = counts.get(b, 0) + 1
        else:
            hint = f" (exported by undeclared {undeclared[s]})" if s in undeclared else ""
            bad.append(f"{s}  [{dem.get(s, s)}]{hint}")
    for k, n in sorted(counts.items()):
        print(f"  {k}: {n}")
    unused = sorted(set(libs) - set(counts) - {"as.vit9696.Lilu"})
    if unused:
        print(f"  note: declared but unused: {', '.join(unused)}")
    if bad:
        print(f"FAIL: {len(bad)} import(s) not exported by any declared KPI/family:")
        for s in bad:
            print("   ", s)
        return 1
    print(f"PASS KPI audit: all {len(syms)} imports resolve to declared KPIs/families")
    return 0


if __name__ == "__main__":
    sys.exit(main())
