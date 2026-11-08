#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""check-adhoc-signature.py — prove a Mach-O carries a valid ad-hoc code signature (release gate after rcodesign).

    python3 -I tools/release/check-adhoc-signature.py <Mach-O> [--bundle <the .kext/.bundle it is the main executable of>]

An ad-hoc signature (codesign -s -, rcodesign sign) is a CodeDirectory without a CMS signer: its whole meaning is
the hashes. `rcodesign verify` rejects every ad-hoc signature ("CMS error", and it says itself that it is unreliable),
so this recomputes them the way the kernel and dyld do: for every CodeDirectory in the LC_CODE_SIGNATURE superblob,
each page of the file up to codeLimit must hash to its code slot. With --bundle, special slot 1 must be the hash
of Contents/Info.plist and slot 3 of Contents/_CodeSignature/CodeResources, and CodeResources must seal every other
file of the bundle: hash2 = its SHA-256, or for nested code (air-runtime/libLLVM.dylib) cdhash = the hash of that
binary's own CodeDirectory, whose pages are checked the same way. The CS_ADHOC flag must be set. Exit 0 valid, 1 not.
"""
import argparse
import hashlib
import os
import plistlib
import struct
import sys

LC_CODE_SIGNATURE = 0x1D
CSMAGIC_EMBEDDED_SIGNATURE = 0xFADE0CC0
CSMAGIC_CODEDIRECTORY = 0xFADE0C02
CS_ADHOC = 0x2
HASHES = {1: hashlib.sha1, 2: hashlib.sha256, 3: hashlib.sha256}   # 3: SHA-256 truncated to hashSize (20)


def code_signature(blob):
    magic, _, _, _, ncmds = struct.unpack_from("<IiiII", blob, 0)
    if magic != 0xFEEDFACF:
        raise ValueError("not a 64-bit Mach-O")
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", blob, off)
        if cmd == LC_CODE_SIGNATURE:
            return struct.unpack_from("<II", blob, off + 8)
        off += size
    return None


def cdhashes(sig):
    """The cdhash of every CodeDirectory in an embedded signature: its hash type's digest of the CD blob, 20 bytes."""
    out = set()
    _, _, count = struct.unpack_from(">III", sig, 0)
    for i in range(count):
        typ, off = struct.unpack_from(">II", sig, 12 + 8 * i)
        cmagic, length = struct.unpack_from(">II", sig, off)
        if cmagic == CSMAGIC_CODEDIRECTORY:
            hf = HASHES.get(sig[off + 37])
            if hf:
                out.add(hf(sig[off:off + length]).digest()[:20])
    return out


def check_seal(bundle, main_exe):
    """Every file under Contents/ except the main executable, Info.plist and _CodeSignature must be in CodeResources
    files2 with the right hash."""
    problems = []
    with open(os.path.join(bundle, "Contents", "_CodeSignature", "CodeResources"), "rb") as fh:
        files2 = plistlib.load(fh).get("files2") or {}
    contents = os.path.join(bundle, "Contents")
    present = set()
    for root, _, files in os.walk(contents):
        for f in files:
            rel = os.path.relpath(os.path.join(root, f), contents)
            if rel.startswith("_CodeSignature/") or rel == "Info.plist" or os.path.join(contents, rel) == os.path.abspath(main_exe):
                continue
            present.add(rel)
    for rel in sorted(present - set(files2)):
        problems.append(f"{rel} is not sealed by CodeResources")
    for rel in sorted(set(files2) - present):
        problems.append(f"CodeResources lists {rel}, which is not in the bundle")
    for rel in sorted(present & set(files2)):
        ent, f = files2[rel], os.path.join(contents, rel)
        if isinstance(ent, dict) and "cdhash" in ent:
            nested = check(f)
            problems += [f"{rel}: {x}" for x in nested]
            with open(f, "rb") as fh:
                blob = fh.read()
            cs = code_signature(blob)
            if cs and ent["cdhash"] not in cdhashes(blob[cs[0]:cs[0] + cs[1]]):
                problems.append(f"{rel}: its cdhash is not the one CodeResources seals")
        else:
            want = ent.get("hash2") if isinstance(ent, dict) else None
            with open(f, "rb") as fh:
                if want != hashlib.sha256(fh.read()).digest():
                    problems.append(f"{rel}: SHA-256 differs from CodeResources")
    return problems


def check(path, bundle=None):
    with open(path, "rb") as fh:
        blob = fh.read()
    cs = code_signature(blob)
    if not cs:
        return ["no LC_CODE_SIGNATURE"]
    dataoff, datasize = cs
    sig = blob[dataoff:dataoff + datasize]
    magic, _, count = struct.unpack_from(">III", sig, 0)
    if magic != CSMAGIC_EMBEDDED_SIGNATURE:
        return ["LC_CODE_SIGNATURE does not point at an embedded signature"]
    problems, cds = [], 0
    for i in range(count):
        typ, off = struct.unpack_from(">II", sig, 12 + 8 * i)
        if typ != 0 and not 0x1000 <= typ < 0x1005:
            continue
        cmagic, _, version, flags, hash_off, _, n_special, n_code, code_limit, hsize, htype, _, page_log = \
            struct.unpack_from(">IIIIIIIIIBBBB", sig, off)
        if cmagic != CSMAGIC_CODEDIRECTORY:
            continue
        cds += 1
        if version >= 0x20300:
            limit64 = struct.unpack_from(">Q", sig, off + 56)[0]
            code_limit = limit64 or code_limit
        hf = HASHES.get(htype)
        if not hf:
            problems.append(f"CodeDirectory {typ:#x}: unknown hash type {htype}")
            continue
        if not flags & CS_ADHOC:
            problems.append(f"CodeDirectory {typ:#x}: not ad-hoc (flags {flags:#x})")
        page = 1 << page_log if page_log else code_limit
        want_slots = (code_limit + page - 1) // page
        if n_code != want_slots:
            problems.append(f"CodeDirectory {typ:#x}: {n_code} code slots for {code_limit} bytes")
        base = off + hash_off
        bad = 0
        for p in range(n_code):
            got = hf(blob[p * page:min((p + 1) * page, code_limit)]).digest()[:hsize]
            if got != sig[base + p * hsize:base + (p + 1) * hsize]:
                bad += 1
        if bad:
            problems.append(f"CodeDirectory {typ:#x}: {bad} of {n_code} page hashes do not match the file")
        if bundle:
            for slot, rel in ((1, "Contents/Info.plist"), (3, "Contents/_CodeSignature/CodeResources")):
                if n_special < slot:
                    problems.append(f"CodeDirectory {typ:#x}: no special slot {slot} ({rel})")
                    continue
                f = os.path.join(bundle, rel)
                if not os.path.isfile(f):
                    problems.append(f"{rel} missing")
                    continue
                with open(f, "rb") as fh:
                    h = hf(fh.read()).digest()[:hsize]
                if h != sig[base - slot * hsize:base - (slot - 1) * hsize]:
                    problems.append(f"CodeDirectory {typ:#x}: special slot {slot} is not the hash of {rel}")
    if not cds:
        problems.append("no CodeDirectory in the signature")
    if bundle and not problems:
        problems += check_seal(bundle, path)
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("macho")
    ap.add_argument("--bundle")
    a = ap.parse_args(argv)
    try:
        problems = check(a.macho, a.bundle)
    except (OSError, ValueError, struct.error) as e:
        problems = [str(e)]
    name = os.path.relpath(a.macho, os.path.dirname(a.bundle)) if a.bundle else os.path.basename(a.macho)
    if problems:
        print(f"check-adhoc-signature: FAIL {name}: " + "; ".join(problems))
        return 1
    print(f"check-adhoc-signature: ok {name} (ad-hoc, every page hash matches"
          f"{', Info.plist, CodeResources and every bundle file sealed' if a.bundle else ''})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
