#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""kc_gate.py — the real-kernel-collection release gate (tools/release.sh --real-kc DIR).

    python3 -I tools/release/kc_gate.py --kc-dir DIR --stage <stage> --driver <driver tree> \
        --kc-audit tools/kc-audit.py --log dist/logs/kc-audit.log --out <result.json>

DIR holds BootKernelExtensions.kc and SystemKernelExtensions.kc copied from the Mac the release is for (macOS
26.7.2 25G332 on the Helios: ~/pascal-build/macos-26.7.2-25G332; Apple binaries, never committed or published —
only their SHA-256 goes into the manifest). Every kext is checked the way it will be loaded:

  OpenCore-injected  -> Boot collection only (OpenCore links against the boot KC; a dependency that only lives in
                        the System KC is "not found" at injection, which is how PG106FB failed on 2026-11-07)
      Ath10kMac, AirportAth10k (Ath10kMac's exports count, --sibling), QCARomePatchRAM: tools/kc-audit.py here
      PG106: nvidia-macos-driver's pascal/scripts/kc-preflight.sh
  /Library/Extensions -> Boot + System (the auxiliary KC links against both)
      PG106FB, PG106Accel: pascal/scripts/kc-preflight.sh

kc-audit checks every import, every Apple parent class size the kext was compiled with, and every vtable slot
against the real collections. NMHDA is a Lilu plugin (its Lilu imports resolve in Lilu, not in a KC) and is not
audited here.

The result goes to --out as JSON (release.kc_audit in the manifest) and every tool's full output to --log.
A staged artifact that is absent (a dry run that could not build it, or --skip) is "not-built", which is not a
gate failure by itself: make_manifest refuses a real release that lacks a required artifact.
Exit 0 every audited kext passed, 1 at least one failed, 2 the collections are missing.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

BOOT, SYSTEM = "BootKernelExtensions.kc", "SystemKernelExtensions.kc"
# (artifact, collections, sibling artifact) for the kexts audited with kc-audit.py directly
OWN = [
    ("Ath10kMac.kext", ["Boot"], None),
    ("AirportAth10k.kext", ["Boot"], "Ath10kMac.kext"),
    ("QCARomePatchRAM.kext", ["Boot"], None),
]
# (artifact, kc-preflight's name for it, collections) for nvidia-macos-driver's pascal/scripts/kc-preflight.sh
GPU = [
    ("PG106.kext", "PG106", ["Boot"]),
    ("PG106FB.kext", "PG106FB", ["Boot", "System"]),
    ("PG106Accel.kext", "PG106Accel", ["Boot", "System"]),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for b in iter(lambda: fh.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def exe_of(bundle):
    return os.path.join(bundle, "Contents", "MacOS", os.path.basename(bundle)[:-len(".kext")])


def last_verdict(text):
    lines = [x for x in text.splitlines() if re.match(r"^(PASS|FAIL)\b", x) or x.startswith("kc-audit:")]
    return lines[-1] if lines else (text.strip().splitlines() or ["(no output)"])[-1]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kc-dir", required=True)
    ap.add_argument("--stage", required=True)
    ap.add_argument("--driver", required=True)
    ap.add_argument("--kc-audit", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)

    kcs = {"Boot": os.path.join(a.kc_dir, BOOT), "System": os.path.join(a.kc_dir, SYSTEM)}
    if not all(os.path.isfile(p) for p in kcs.values()):
        print(f"kc_gate: FAIL {a.kc_dir} must hold {BOOT} and {SYSTEM} from the target Mac")
        return 2
    results, log = [], open(a.log, "w", encoding="utf-8")

    def record(name, cols, result, summary):
        results.append({"name": name, "collections": cols, "result": result, "summary": summary[:300]})

    for name, cols, sib in OWN:
        bundle = os.path.join(a.stage, name)
        if not os.path.isdir(bundle):
            record(name, cols, "not-built", "not in this build")
            continue
        cmd = [sys.executable, "-I", a.kc_audit]
        for c in cols:
            cmd += ["--kc", kcs[c]]
        cmd += ["--kext", exe_of(bundle), "--plist", os.path.join(bundle, "Contents", "Info.plist")]
        if sib and os.path.isdir(os.path.join(a.stage, sib)):
            cmd += ["--sibling", exe_of(os.path.join(a.stage, sib))]
        r = subprocess.run(cmd, capture_output=True, text=True)
        out = (r.stdout or "") + (r.stderr or "")
        log.write(f"== {name} against {'+'.join(cols)}: kc-audit.py rc={r.returncode}\n{out}\n")
        record(name, cols, "pass" if r.returncode == 0 else "fail", last_verdict(out))

    pre = os.path.join(a.driver, "pascal", "scripts", "kc-preflight.sh")
    gpu_staged = [(n, k, c) for n, k, c in GPU if os.path.isdir(os.path.join(a.stage, n))]
    for n, k, c in GPU:
        if (n, k, c) not in gpu_staged:
            record(n, c, "not-built", "not in this build")
    if gpu_staged and not os.path.isfile(pre):
        for n, k, c in gpu_staged:
            record(n, c, "fail", "this nvidia-macos-driver commit has no pascal/scripts/kc-preflight.sh; bump the submodule")
    elif gpu_staged:
        # kc-preflight.sh takes PG106, PG106FB, PG106Accel by position; a missing one is skipped by it ("not built").
        args = [os.path.join(a.stage, n) for n, _, _ in GPU]
        env = dict(os.environ, HELIOS_KC_DIR=a.kc_dir, KC_AUDIT=os.path.abspath(a.kc_audit))
        r = subprocess.run(["bash", pre, *args], capture_output=True, text=True, env=env)
        out = (r.stdout or "") + (r.stderr or "")
        log.write(f"== pascal/scripts/kc-preflight.sh rc={r.returncode}\n{out}\n")
        blocks, cur = {}, None
        for line in out.splitlines():
            m = re.match(r"^== (\S+) against ", line)
            if m:
                cur = m.group(1)
                blocks[cur] = []
            elif cur:
                blocks[cur].append(line)
        for n, k, c in gpu_staged:
            text = "\n".join(blocks.get(k, []))
            if k not in blocks:
                record(n, c, "fail", f"kc-preflight.sh did not audit it (rc {r.returncode}): {last_verdict(out)}")
            elif re.search(r"^PASS kc-audit", text, re.M) and not re.search(r"^FAIL", text, re.M):
                record(n, c, "pass", last_verdict(text))
            else:
                record(n, c, "fail", last_verdict(text))
    log.close()
    order = [n for n, _, _ in OWN] + [n for n, _, _ in GPU]
    results.sort(key=lambda x: order.index(x["name"]))
    for x in results:
        print(f"  {x['result']:<9} {x['name']:<22} [{'+'.join(x['collections'])}] {x['summary'][:110]}")
    failed = [x["name"] for x in results if x["result"] == "fail"]
    res = {
        "status": "fail" if failed else "pass",
        "kc_dir": os.path.basename(os.path.normpath(a.kc_dir)),
        "collections": [{"file": os.path.basename(p), "sha256": sha256(p)} for p in kcs.values()],
        "results": results,
        "log": "logs/" + os.path.basename(a.log),
    }
    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump(res, fh, indent=1)
    audited = sum(1 for x in results if x["result"] != "not-built")
    print(f"kc_gate: {'FAIL ' + ', '.join(failed) if failed else 'PASS'} ({audited} kexts audited against "
          f"{res['kc_dir']}; log {res['log']})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
