#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""check-air-llvm.py — release gate for the Metal plugin's AIR reader (NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib).

    python3 -I tools/release/check-air-llvm.py --dylib <libLLVM.dylib> --plugin <driver>/plugin --llvm-version 23.1.4 \
        [--host-libllvm /usr/lib/libLLVM.so.23.1]

1. Exports: every LLVM C function the plugin resolves with dlsym (read from plugin/NVMTLLibraryLoad.m and
   plugin/nvmtl_air_reader.h:
   NV_LLVM_LOAD(...), dlsym(library, "...") and NVMTL_LLVM_WALK_NAMES) must be exported by the Mach-O. A missing
   required one disables the reader (every uncached shader fails); a missing optional one silently disables the
   simdgroup-matrix unroll or the pointer canonicalization, so all of them are required here.
2. AIR: the bitcode modules of the plugin's own precompiled Metal library (plugin/nvconv_lib.h, Apple metal
   compiler output) go through the plugin's exact call sequence (tools/release/air-reader-check.c: parse with an
   error-severity diagnostic handler, the unroll pipeline NVMTL_AIR_UNROLL_PIPELINE from the source, the two
   canonicalization pipelines, print) against libLLVM of the SAME version built for Linux. The macOS dylib cannot
   run on Linux, so this proves the LLVM version and the call sequence, and check 1 proves the binary's surface.
   The host library must report exactly --llvm-version (llvm-config), or be named with --host-libllvm.
Exit 0 pass, 1 fail, 77 no host compiler/LLVM to run check 2.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def plugin_symbols(src):
    names = set(re.findall(r"NV_LLVM_LOAD\((LLVM\w+)\)", src))
    names |= set(re.findall(r'dlsym\(library,\s*"(LLVM\w+)"\)', src))
    m = re.search(r"#define NVMTL_LLVM_WALK_NAMES((?:[^\n]*\\\n)*[^\n]*)", src)
    if m:
        names |= set(re.findall(r'"(LLVM\w+)"', m.group(1)))
    return names


def air_modules(header, out_dir):
    body = header.split("[] = {", 1)[1].split("};", 1)[0]
    blob = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))
    if blob[:4] != b"MTLB":
        raise SystemExit("check-air-llvm: FAIL nvconv_lib.h is not a metallib")
    paths = []
    for m in re.finditer(re.escape(b"\xde\xc0\x17\x0b"), blob):   # bitcode wrapper, as the metallib stores modules
        o = m.start()
        _, _, off, size, _ = struct.unpack_from("<5I", blob, o)
        if blob[o + off:o + off + 4] != b"BC\xc0\xde":
            continue
        p = os.path.join(out_dir, f"nvconv-{len(paths)}.air.bc")
        with open(p, "wb") as fh:
            fh.write(blob[o:o + off + size])
        paths.append(p)
    return paths


def host_libllvm(version):
    try:
        got = subprocess.run(["llvm-config", "--version"], capture_output=True, text=True, check=True).stdout.strip()
        libdir = subprocess.run(["llvm-config", "--libdir"], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None, "llvm-config not found"
    if got != version:
        return None, f"the host LLVM is {got}, not {version} (pass --host-libllvm with a {version} libLLVM.so)"
    major, minor = version.split(".")[:2]
    # the soname of this exact major.minor (a host may carry several LLVMs side by side), then the older names
    for name in (f"libLLVM.so.{major}.{minor}", f"libLLVM-{major}.so", f"libLLVM.so.{major}"):
        if os.path.isfile(os.path.join(libdir, name)):
            return os.path.join(libdir, name), None
    return None, f"no libLLVM.so.{major}.{minor} in {libdir}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dylib", required=True)
    ap.add_argument("--plugin", required=True, help="the driver tree's plugin/ directory")
    ap.add_argument("--llvm-version", required=True)
    ap.add_argument("--host-libllvm")
    a = ap.parse_args(argv)
    with open(os.path.join(a.plugin, "NVMTLLibraryLoad.m"), encoding="utf-8") as fh:
        src = fh.read()
    # the IR walks, their dlsym names and the pipelines moved to a plain-C header the Linux harness shares
    # (driver plugin/nvmtl_air_reader.h, plugin/tests/air_xlate_check.py); older plugin trees have none
    reader_h = os.path.join(a.plugin, "nvmtl_air_reader.h")
    if os.path.isfile(reader_h):
        with open(reader_h, encoding="utf-8") as fh:
            src += "\n" + fh.read()
    want = plugin_symbols(src)
    if len(want) < 20:
        print(f"check-air-llvm: FAIL only {len(want)} LLVM symbols found in NVMTLLibraryLoad.m; the plugin's loader changed shape")
        return 1
    nm = subprocess.run(["llvm-nm", "-gU", "--defined-only", "-j", a.dylib], capture_output=True, text=True)
    if nm.returncode:
        print(f"check-air-llvm: FAIL llvm-nm {a.dylib}: {nm.stderr.strip()}")
        return 1
    have = {s[1:] if s.startswith("_") else s for s in nm.stdout.split()}
    missing = sorted(want - have)
    if missing:
        print(f"check-air-llvm: FAIL libLLVM.dylib does not export {len(missing)} function(s) the plugin resolves: {', '.join(missing)}")
        return 1
    print(f"  ok   exports: all {len(want)} LLVM C functions the plugin dlsyms ({len(have)} exported)")

    pipe = re.search(r'#define NVMTL_AIR_UNROLL_PIPELINE "([^"]+)"', src)
    if not pipe:
        print("check-air-llvm: FAIL NVMTL_AIR_UNROLL_PIPELINE not found in NVMTLLibraryLoad.m")
        return 1
    lib, why = (a.host_libllvm, None) if a.host_libllvm else host_libllvm(a.llvm_version)
    cc = shutil.which("cc") or shutil.which("clang")
    if not lib or not cc:
        print(f"check-air-llvm: SKIP AIR parse: {why or 'no host C compiler'}")
        return 77
    with tempfile.TemporaryDirectory(prefix="air-check-") as t:
        with open(os.path.join(a.plugin, "nvconv_lib.h"), encoding="utf-8") as fh:
            mods = air_modules(fh.read(), t)
        if not mods:
            print("check-air-llvm: FAIL no AIR bitcode module in nvconv_lib.h")
            return 1
        exe = os.path.join(t, "air-reader-check")
        r = subprocess.run([cc, "-O1", "-o", exe, os.path.join(HERE, "air-reader-check.c"), "-ldl"], capture_output=True, text=True)
        if r.returncode:
            print("check-air-llvm: FAIL compiling air-reader-check.c:\n" + r.stderr[-2000:])
            return 1
        r = subprocess.run([exe, lib, pipe.group(1), *mods], capture_output=True, text=True)
        out = r.stdout + r.stderr
        sys.stdout.write("".join(f"  {x}\n" for x in out.splitlines() if not x.startswith("  ok")))
        if r.returncode:
            print(f"check-air-llvm: FAIL the AIR sample did not go through LLVM {a.llvm_version} ({lib})")
            return 1
    print(f"check-air-llvm: PASS exports + {len(mods)} Apple AIR modules (nvconv.metallib) parsed, unrolled, canonicalized "
          f"and printed by LLVM {a.llvm_version} through the plugin's C API sequence ({os.path.basename(lib)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
