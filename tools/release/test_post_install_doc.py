#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The stick kit: POST-INSTALL.md lists every post_install step with its commands and every stick file; release.sh
stages every script/doc artifact the spec names; make_manifest accepts them (kind doc, built_by generated).

    python3 -I -B -m unittest discover -s tools/release -p 'test_*.py'
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import post_install_doc  # noqa: E402

with open(os.path.join(HERE, "spec.json")) as fh:
    SPEC = json.load(fh)


class Doc(unittest.TestCase):
    def setUp(self):
        self.doc = post_install_doc.render(SPEC, "0.1.1")

    def test_every_step_with_its_commands_in_order(self):
        last = -1
        for st in sorted(SPEC["post_install"], key=lambda s: s["order"]):
            at = self.doc.index(f"### {st['step']}")
            self.assertGreater(at, last, st["step"])
            last = at
            for k in ("check", "install", "revert", "status"):
                if (st.get("run") or {}).get(k):
                    self.assertIn(st["run"][k], self.doc)
        self.assertIn("**Audio: do ONE of these 2 options.**", self.doc)
        self.assertIn("### audio-applehda (option 2, recommended)", self.doc)

    def test_every_stick_file_is_listed(self):
        table = self.doc[self.doc.index("## Files on the stick"):]
        for a in SPEC["artifacts"]:
            if a["install"] == "opencore":
                self.assertNotIn(f"`{a['name']}", table, a["name"])
            else:
                self.assertRegex(table, r"\| `" + re.escape(a["name"]) + r"(\.zip)?` \|", a["name"])

    def test_gpu_apps_ships_with_the_gpu_step(self):
        gpu = next(s for s in SPEC["post_install"] if s["step"] == "gpu")
        self.assertIn("gpu-apps.sh", gpu["artifacts"])
        self.assertIn("sudo bash gpu-apps.sh add /Applications/Foo.app", self.doc)
        self.assertNotIn("test-stdin.sh", self.doc)


class Release(unittest.TestCase):
    def test_release_sh_stages_every_script_and_doc(self):
        with open(os.path.join(ROOT, "tools", "release.sh")) as fh:
            sh = fh.read()
        line = next(x for x in sh.splitlines() if x.startswith("step scripts "))
        staged = set(line.split(" -- ")[0].split()[2:])
        want = {a["name"] for a in SPEC["artifacts"] if a["kind"] in ("script", "doc")}
        self.assertEqual(staged, want)
        for a in SPEC["artifacts"]:
            if a["kind"] == "script" and a["source"]["tree"] == "self":
                self.assertTrue(os.path.isfile(os.path.join(ROOT, a["source"]["path"])), a["source"]["path"])

    def test_make_manifest_accepts_the_kit(self):
        with tempfile.TemporaryDirectory() as t:
            stage, dist = os.path.join(t, "stage"), os.path.join(t, "dist")
            os.makedirs(stage)
            for a in SPEC["artifacts"]:
                if a["kind"] == "script":
                    with open(os.path.join(stage, a["name"]), "w") as fh:
                        fh.write("#!/bin/bash\n")
            post_install_doc.main(["--spec", os.path.join(HERE, "spec.json"), "--version", "0.1.1",
                                   "--out", os.path.join(stage, "POST-INSTALL.md")])
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "make_manifest.py"), "--spec", os.path.join(HERE, "spec.json"),
                                "--stage", stage, "--dist", dist, "--version", "0.1.1", "--self-commit", "0" * 40,
                                "--driver-commit", "1" * 40, "--dry-run"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            with open(os.path.join(dist, "manifest.json")) as fh:
                m = json.load(fh)
            doc = next(a for a in m["artifacts"] if a["name"] == "POST-INSTALL.md")
            self.assertEqual((doc["kind"], doc["built_by"], doc["install"], doc["file"]), ("doc", "generated", "stick-only", "POST-INSTALL.md"))
            self.assertIn("gpu-apps.sh", [a["file"] for a in m["artifacts"]])


if __name__ == "__main__":
    unittest.main()
