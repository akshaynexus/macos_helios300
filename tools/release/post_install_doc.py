#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""POST-INSTALL.md for the stick (EFI/Helios300), generated from spec.json's post_install and artifacts.

    python3 -I tools/release/post_install_doc.py --spec tools/release/spec.json --version 0.1.1 --out <stage>/POST-INSTALL.md

Every post_install step in `order`: the alternatives of a group named once, each step's exact commands (check,
install, revert, status), where to run them, what must be true first, its notes. Then every file that goes on the
stick (each artifact that is not OpenCore-injected), with what it is. release.sh writes it as the POST-INSTALL.md
artifact, so it always matches the manifest it ships with; 1401 copies it into EFI/Helios300 with the rest.
"""
import argparse
import json
import os
import sys

DOC_NAME = "POST-INSTALL.md"
WHERE = {"library-extensions": "installed into /Library/Extensions by its step",
         "gpu-bundles": "installed into /Library/GPUBundles by its step",
         "applications": "installed into /Applications by its step",
         "stick-only": "stays on the stick"}


def render(spec, version):
    steps = sorted(spec.get("post_install") or [], key=lambda st: st.get("order", 0))
    arts = {a["name"]: a for a in spec["artifacts"]}
    out = [f"# macos_helios300 {version}: after macOS 26 is installed",
           "",
           f"For the {spec['machine']['name']}. Generated from the release's `manifest.json` (its `post_install`); "
           "the manifest next to this file is the authority for every file's SHA-256, source and licence.",
           "",
           "OpenCore cannot inject these. Mount the stick's EFI partition (`sudo diskutil mount <disk>s1`) or copy "
           "`EFI/Helios300` to your home folder, open Terminal in that folder, and run each script with `bash` as "
           "your own user: the scripts call `sudo` themselves. `MODE=dry-run` changes nothing and prints what "
           "`MODE=install` would do: run it first, every time. Make one change per restart.",
           "",
           "If macOS does not come up after a step: Shift+Enter in the OpenCore picker (safe mode, `-x`); the "
           "macos_helios300 kexts stand down there. Then run that step's revert command.",
           "",
           "## Steps, in order",
           ""]
    said = set()
    for st in steps:
        g = st.get("group")
        if g and g not in said:
            n = sum(1 for x in steps if x.get("group") == g)
            out += [f"**{g.capitalize()}: do ONE of these {n} options.**", ""]
            said.add(g)
        head = f"### {st['step']}"
        if st.get("choice"):
            head += f" (option {st['choice']}{', recommended' if st.get('recommended') else ''})"
        out += [head, "", st["what"] + ".", ""]
        run = st.get("run") or {}
        if st.get("script"):
            if st.get("cwd"):
                out += [f"Run in: {st['cwd']}.", ""]
            out.append("```sh")
            for k in ("check", "install", "revert", "status"):
                if run.get(k):
                    out.append(f"{run[k]:<44}# {k}")
            out += ["```", ""]
        files = [a for a in st["artifacts"] if a != st.get("script")]
        if files:
            out += ["Files: " + ", ".join(f"`{arts[a]['name']}`" for a in files) + ".", ""]
        if st.get("needs"):
            out += ["Needs: " + "; ".join(st["needs"]) + ".", ""]
        tail = []
        if st.get("reboot"):
            tail.append("Restart after install or revert.")
        if st.get("rerun_after_macos_update"):
            tail.append("A macOS update undoes it: run it again after every update.")
        if tail:
            out += [" ".join(tail), ""]
        for note in st.get("notes") or []:
            out.append(f"- {note}")
        if st.get("notes"):
            out.append("")
    out += ["## Files on the stick", "", "| File | What | Where it goes |", "|---|---|---|"]
    for a in spec["artifacts"]:
        if a.get("install") == "opencore":
            continue
        fname = a["name"].replace(" ", "-") + ("" if a["kind"] in ("script", "doc", "firmware-licence", "dylib") and "." in a["name"] else ".zip")
        if a["name"] == DOC_NAME:
            what = "this file"
        else:
            what = a["status"].split("; ")[0].replace("|", "/")
        where = WHERE.get(a["install"], a["install"])
        if a["install"] != "stick-only" and a.get("install_path"):
            where = f"installed to `{a['install_path']}` by its step"
        out.append(f"| `{fname}` | {a['kind']}: {what} | {where} |")
    out += ["", "Contract and sources: https://github.com/akshaynexus/macos_helios300/blob/main/docs/MANIFEST.md", ""]
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--spec", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    with open(a.spec) as fh:
        spec = json.load(fh)
    text = render(spec, a.version)
    with open(a.out + ".part", "w", encoding="utf-8") as fh:
        fh.write(text)
    os.replace(a.out + ".part", a.out)
    print(f"post_install_doc: {a.out} ({len(spec.get('post_install') or [])} steps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
