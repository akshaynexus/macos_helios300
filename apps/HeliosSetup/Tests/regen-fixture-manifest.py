#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regenerate Tests/HeliosCoreTests/Fixtures/manifest-next.json: the manifest the current tools/release/make_manifest.py
writes (with config_delta, update_groups, health, ...) for the files of the real release in dist/ (v0.1.0, unzipped
back into a stage: the zips are deterministic) plus this checkout's scripts, POST-INSTALL.md and a placeholder app.

    python3 -I apps/HeliosSetup/Tests/regen-fixture-manifest.py [--dist dist] [--driver kexts/nvidia-macos-driver]

The app's tests then diff it against the real manifest-0.1.0.json, so a make_manifest change that the app would
misread fails `swift test`.
"""
import argparse
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
REL = os.path.join(ROOT, "tools", "release")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dist", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--driver", default=os.path.join(ROOT, "kexts", "nvidia-macos-driver"))
    a = ap.parse_args()
    with open(os.path.join(a.dist, "manifest.json")) as fh:
        old = json.load(fh)
    with tempfile.TemporaryDirectory() as t:
        stage, out = os.path.join(t, "stage"), os.path.join(t, "out")
        os.makedirs(stage)
        for art in old["artifacts"]:
            src = os.path.join(a.dist, art["file"])
            if art["file"].endswith(".zip"):
                with zipfile.ZipFile(src) as z:
                    z.extractall(stage)
                    for info in z.infolist():   # extractall drops the modes; the zips are rebuilt from them
                        os.chmod(os.path.join(stage, info.filename), (info.external_attr >> 16) & 0o777)
            else:
                shutil.copy(src, os.path.join(stage, art["name"]))
        # 0.1.0 predates the baked Wi-Fi country (release.sh --wifi-country); give it the default one.
        plist = os.path.join(stage, "Ath10kMac.kext", "Contents", "Info.plist")
        with open(plist, "rb") as fh:
            info = plistlib.load(fh)
        for pers in info.get("IOKitPersonalities", {}).values():
            pers.setdefault("Ath10kCountryCode", "IN")
        with open(plist, "wb") as fh:
            plistlib.dump(info, fh)
        # 0.1.0's plugin predates its air-runtime: placeholders for the third_party files the spec now requires.
        with open(os.path.join(REL, "spec.json")) as fh:
            spec = json.load(fh)
        for art in spec["artifacts"]:
            for tp in art.get("third_party") or []:
                for rel in [tp["path"], *tp["notices"]]:
                    path = os.path.join(stage, art["name"], rel)
                    if os.path.isdir(os.path.join(stage, art["name"])) and not os.path.exists(path):
                        os.makedirs(os.path.dirname(path), exist_ok=True)
                        with open(path, "w") as fh:
                            fh.write("fixture placeholder\n")
        for name in ("audio-voodoohda.sh", "audio-applehda.sh", "install-gpu.sh"):
            shutil.copy(os.path.join(ROOT, "tools", "macos", name), os.path.join(stage, name))
        shutil.copy(os.path.join(a.driver, "tools", "macos", "gpu-apps.sh"), os.path.join(stage, "gpu-apps.sh"))
        subprocess.run([sys.executable, "-I", os.path.join(REL, "post_install_doc.py"), "--spec", os.path.join(REL, "spec.json"),
                        "--version", "0.1.1", "--out", os.path.join(stage, "POST-INSTALL.md")], check=True)
        app = os.path.join(stage, "Helios 300 Setup.app", "Contents")
        os.makedirs(os.path.join(app, "MacOS"))
        with open(os.path.join(app, "Info.plist"), "w") as fh:
            fh.write('<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict>'
                     "<key>CFBundleIdentifier</key><string>org.helios300.setup</string>"
                     "<key>CFBundleShortVersionString</key><string>0.1.1</string></dict></plist>\n")
        with open(os.path.join(app, "MacOS", "HeliosSetup"), "w") as fh:
            fh.write("placeholder\n")
        # 0.1.0 shipped PG106FB unsigned; a release now signs everything that installs into /Library (release.sh).
        signed = [x["name"] for x in old["artifacts"] if x.get("signed")] + ["Helios 300 Setup.app"]
        cmd = [sys.executable, "-I", os.path.join(REL, "make_manifest.py"), "--spec", os.path.join(REL, "spec.json"),
               "--stage", stage, "--dist", out, "--version", "0.1.1", "--self-commit", "0" * 40, "--driver-commit", "1" * 40,
               "--driver-tree", a.driver, "--dry-run"] + [x for n in signed for x in ("--signed", n)]
        subprocess.run(cmd, check=True)
        shutil.copy(os.path.join(out, "manifest.json"), os.path.join(HERE, "HeliosCoreTests", "Fixtures", "manifest-next.json"))
    print("wrote Fixtures/manifest-next.json")


if __name__ == "__main__":
    main()
