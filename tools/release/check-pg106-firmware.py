#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""check-pg106-firmware.py — prove the GP106 firmware is built INTO PG106's binary (release gate).

    python3 -I tools/release/check-pg106-firmware.py --kext PG106.kext --driver <nvidia-macos-driver tree>

PG106 is injected by OpenCore, and an OpenCore-injected kext has no bundle on disk, so OSKextRequestResource can
never serve Contents/Resources (macOS 26.7.2, first boot 2026-11-07). The firmware must therefore be in the Mach-O
itself (nvidia-macos-driver pascal/kext/scripts/gen-firmware-embed.py, table pg106_fw_embedded). Staging the files
into Resources proves nothing about that, so this checks the binary:

  1. every file pinned in <driver>/pascal/kext/firmware.sha256 exists in <driver>/research/firmware with that
     SHA-256 (the exact linux-firmware bytes);
  2. each file's bytes occur, unmodified, in Contents/MacOS/PG106, and so does its pinned SHA-256 string (the
     table row's sha256 field);
  3. the binary exports _pg106_fw_embedded and _pg106_fw_embedded_count, and the count stored in the binary equals
     the number of pinned files (17 for the GP106: ACR, SEC2, GR and the four sw_* lists).

Exit 0 pass (last line "PASS ..."), 1 fail. Prints one line per file.
"""
import argparse
import hashlib
import os
import struct
import sys

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2


def macho_symbols(blob):
    """{name: n_value} of the defined external symbols, and the sections [(addr, size, fileoff)]."""
    magic, _, _, _, ncmds = struct.unpack_from("<IiiII", blob, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit("check-pg106-firmware: FAIL PG106 is not a 64-bit Mach-O")
    off, sections, syms = 32, [], {}
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", blob, off)
        if cmd == LC_SEGMENT_64:
            nsects = struct.unpack_from("<I", blob, off + 64)[0]
            for i in range(nsects):
                s = off + 72 + i * 80
                addr, sz, fileoff = struct.unpack_from("<QQI", blob, s + 32)
                sections.append((addr, sz, fileoff))
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff, _ = struct.unpack_from("<IIII", blob, off + 8)
            for i in range(nsyms):
                strx, ntype, _, _, value = struct.unpack_from("<IBBHQ", blob, symoff + i * 16)
                if ntype & 0x0E == 0x0E:   # N_SECT: defined in a section
                    end = blob.index(b"\0", stroff + strx)
                    syms[blob[stroff + strx:end].decode("utf-8", "replace")] = value
        off += size
    return syms, sections


def read_u32(blob, sections, addr):
    for base, size, fileoff in sections:
        if base <= addr < base + size and fileoff:
            return struct.unpack_from("<I", blob, fileoff + addr - base)[0]
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kext", required=True, help="the built PG106.kext")
    ap.add_argument("--driver", required=True, help="the nvidia-macos-driver tree it was built from")
    a = ap.parse_args(argv)

    pins_path = os.path.join(a.driver, "pascal", "kext", "firmware.sha256")
    exe = os.path.join(a.kext, "Contents", "MacOS", "PG106")
    if not os.path.isfile(pins_path):
        print(f"check-pg106-firmware: FAIL {pins_path} missing: this driver commit does not embed the firmware "
              "(gen-firmware-embed.py); bump the nvidia-macos-driver submodule")
        return 1
    pins = []
    for line in open(pins_path, encoding="utf-8"):
        line = line.strip()
        if line and not line.startswith("#"):
            digest, path = line.split()
            pins.append((digest.lower(), path))
    try:
        with open(exe, "rb") as fh:
            blob = fh.read()
    except OSError as e:
        print(f"check-pg106-firmware: FAIL no PG106 binary: {e}")
        return 1
    bad = 0
    for digest, path in pins:
        src = os.path.join(a.driver, "research", "firmware", path)
        try:
            with open(src, "rb") as fh:
                data = fh.read()
        except OSError:
            print(f"  FAIL {path}: not in research/firmware")
            bad += 1
            continue
        if hashlib.sha256(data).hexdigest() != digest:
            print(f"  FAIL {path}: research/firmware copy does not match the pinned sha256")
            bad += 1
        elif data not in blob:
            print(f"  FAIL {path}: {len(data)} bytes NOT in the PG106 binary")
            bad += 1
        elif digest.encode() not in blob:
            print(f"  FAIL {path}: bytes present but its table row (sha256 {digest[:16]}) is not")
            bad += 1
        else:
            print(f"  ok   {path}: {len(data)} bytes embedded, sha256 {digest[:16]}")
    syms, sections = macho_symbols(blob)
    if "_pg106_fw_embedded" not in syms or "_pg106_fw_embedded_count" not in syms:
        print("  FAIL the binary has no _pg106_fw_embedded / _pg106_fw_embedded_count table")
        bad += 1
    else:
        count = read_u32(blob, sections, syms["_pg106_fw_embedded_count"])
        if count != len(pins):
            print(f"  FAIL pg106_fw_embedded_count is {count}, {len(pins)} files are pinned")
            bad += 1
        else:
            print(f"  ok   pg106_fw_embedded_count = {count}")
    if bad or not pins:
        print(f"check-pg106-firmware: FAIL {bad} problem(s); {len(pins)} pinned files")
        return 1
    print(f"check-pg106-firmware: PASS {len(pins)} of {len(pins)} firmware files embedded in PG106 "
          f"({hashlib.sha256(blob).hexdigest()[:16]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
