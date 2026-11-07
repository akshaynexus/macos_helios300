#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Turn the built artifacts in <stage> into release files + manifest.json in <dist> (docs/MANIFEST.md).

    python3 -I tools/release/make_manifest.py --spec tools/release/spec.json --stage <dir> --dist dist \
        --version 0.1.0 --self-commit <sha> --driver-commit <sha> [--driver-repo akshaynexus/nvidia-macos-driver] \
        [--dirty] [--dry-run] [--signed NAME]...

<stage>/<artifact name> is what release.sh built for each spec entry (a bundle directory, a directory of files for
NVK-Pascal, or a single file). Each becomes one release file:
  - a directory  -> <name>.zip, deterministic (sorted entries, fixed 1980-01-01 timestamps, 0755/0644 modes), so
    the same build gives the same bytes and the same sha256;
  - a file       -> copied as is.
The manifest records each file's sha256 and size, the sha256 of every file inside a zip (contents_sha256: the
consumer re-checks the extracted bundle), the bundle's CFBundleIdentifier/CFBundleShortVersionString from the built
Info.plist, and the release URL. A required artifact that was not built stops the release; with --dry-run it is
listed under release.missing instead and the manifest is marked dry_run (a consumer refuses such a manifest unless
it was pointed at it explicitly). The result is checked against manifest.schema.json before it is written.
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import plistlib
import shutil
import stat
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import schema_check  # noqa: E402

ZIP_DATE = (1980, 1, 1, 0, 0, 0)
SPEC_ONLY = {"about"}   # spec keys that never reach the manifest


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def file_name(name):
    """Release file name: spaces become '-' (the schema allows [A-Za-z0-9_.+-])."""
    return name.replace(" ", "-")


def deterministic_zip(src_dir, top, out_path):
    """Zip src_dir as <top>/... with sorted entries and fixed metadata. Returns {relative path: sha256}."""
    contents = {}
    entries = []
    for root, dirs, files in os.walk(src_dir):
        dirs.sort()
        rel_root = os.path.relpath(root, src_dir)
        for d in dirs:
            p = os.path.join(root, d)
            if os.path.islink(p):
                raise SystemExit(f"make_manifest: symlink {p} in a bundle is not supported")
            entries.append((os.path.normpath(os.path.join(top, rel_root, d)) + "/", None))
        for f in sorted(files):
            p = os.path.join(root, f)
            if os.path.islink(p):
                raise SystemExit(f"make_manifest: symlink {p} in a bundle is not supported")
            entries.append((os.path.normpath(os.path.join(top, rel_root, f)), p))
    entries.sort(key=lambda e: e[0])
    with zipfile.ZipFile(out_path + ".part", "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        root_info = zipfile.ZipInfo(top + "/", ZIP_DATE)
        root_info.external_attr = (stat.S_IFDIR | 0o755) << 16
        z.writestr(root_info, b"")
        for arc, src in entries:
            info = zipfile.ZipInfo(arc, ZIP_DATE)
            info.create_system = 3   # Unix, so the modes below are honoured by unzip/ditto
            if src is None:
                info.external_attr = (stat.S_IFDIR | 0o755) << 16
                z.writestr(info, b"")
                continue
            mode = 0o755 if os.stat(src).st_mode & 0o111 else 0o644
            info.external_attr = (stat.S_IFREG | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(src, "rb") as fh:
                data = fh.read()
            z.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
            contents[os.path.relpath(arc, top)] = hashlib.sha256(data).hexdigest()
    os.replace(out_path + ".part", out_path)
    return contents


def bundle_info(path):
    plist = os.path.join(path, "Contents", "Info.plist")
    if not os.path.isfile(plist):
        return None, None
    with open(plist, "rb") as fh:
        d = plistlib.load(fh)
    return d.get("CFBundleIdentifier"), d.get("CFBundleShortVersionString") or d.get("CFBundleVersion")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--spec", required=True)
    ap.add_argument("--schema", default=os.path.join(HERE, "manifest.schema.json"))
    ap.add_argument("--stage", required=True)
    ap.add_argument("--dist", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--self-commit", required=True)
    ap.add_argument("--driver-commit", required=True)
    ap.add_argument("--driver-repo", default="akshaynexus/nvidia-macos-driver")
    ap.add_argument("--driver-ref", default="pascal-gp106")
    ap.add_argument("--dirty", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--signed", action="append", default=[], help="artifact names release.sh ad-hoc signed")
    ap.add_argument("--missing-why", action="append", default=[], help="NAME=reason for an artifact release.sh could not build")
    a = ap.parse_args(argv)

    with open(a.spec) as fh:
        spec = json.load(fh)
    why_missing = dict(x.split("=", 1) for x in a.missing_why)
    os.makedirs(a.dist, exist_ok=True)
    tag = "v" + a.version
    artifacts, missing = [], []
    for art in spec["artifacts"]:
        name = art["name"]
        src = os.path.join(a.stage, name)
        if not os.path.exists(src):
            reason = why_missing.get(name, "not built")
            if art.get("optional"):
                print(f"make_manifest: {name} not in this release (optional): {reason}")
                missing.append({"name": name, "why": "optional, " + reason})
                continue
            if not a.dry_run:
                raise SystemExit(f"make_manifest: required artifact {name} was not built ({reason}); no manifest written")
            print(f"make_manifest: DRY RUN - required {name} missing: {reason}")
            missing.append({"name": name, "why": reason})
            continue
        out = dict(art)
        if os.path.isdir(src):
            fname = file_name(name) + ".zip"
            out["contents_sha256"] = deterministic_zip(src, name, os.path.join(a.dist, fname))
            bid, ver = bundle_info(src)
            if art.get("bundle_id") and bid != art["bundle_id"]:
                raise SystemExit(f"make_manifest: {name} was built with CFBundleIdentifier {bid}, the spec says {art['bundle_id']}")
            if bid:
                out["bundle_id"] = bid
            out["version"] = ver or a.version
        else:
            fname = file_name(name)
            shutil.copyfile(src, os.path.join(a.dist, fname))
            out["version"] = a.version
        path = os.path.join(a.dist, fname)
        out["file"] = fname
        out["sha256"] = sha256_file(path)
        out["size"] = os.path.getsize(path)
        out["url"] = spec["url_template"].format(version=a.version, file=fname)
        if art["kind"] in ("kext", "bundle", "app", "dylib") and art["built_by"] == "cross":
            out["signed"] = "ad-hoc (rcodesign)" if name in a.signed else "unsigned"
        artifacts.append(out)
        print(f"make_manifest: {fname:<34} {out['sha256'][:16]}  {out['size']:>10} B  {out.get('version')}")

    manifest = {k: v for k, v in spec.items() if k not in SPEC_ONLY and k != "url_template"}
    manifest["about"] = "macos_helios300 release manifest; contract: docs/MANIFEST.md"
    manifest["release"] = {
        "version": a.version, "tag": tag, "commit": a.self_commit, "dirty": bool(a.dirty),
        "pre_release": True, "dry_run": bool(a.dry_run),
        "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "built_on": f"{platform.system()} {platform.machine()} (cross toolchain, see docs/MANIFEST.md)",
        "sources": {"self": {"repo": spec["repo"], "commit": a.self_commit},
                    "driver": {"repo": a.driver_repo, "commit": a.driver_commit, "ref": a.driver_ref}},
    }
    if missing:
        manifest["release"]["missing"] = missing
    manifest["artifacts"] = artifacts
    with open(a.schema) as fh:
        errors = schema_check.check(json.load(fh), manifest)
    if errors:
        for e in errors:
            print("make_manifest: schema:", e)
        raise SystemExit("make_manifest: the manifest does not match manifest.schema.json; nothing written")
    mpath = os.path.join(a.dist, "manifest.json")
    with open(mpath + ".part", "w") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=False)
        fh.write("\n")
    os.replace(mpath + ".part", mpath)
    names = sorted(x["file"] for x in artifacts) + ["manifest.json"]
    with open(os.path.join(a.dist, "SHA256SUMS.txt"), "w") as fh:
        for n in names:
            fh.write(f"{sha256_file(os.path.join(a.dist, n))}  {n}\n")
    print(f"make_manifest: {mpath} ({len(artifacts)} artifacts, sha256 {sha256_file(mpath)}){' DRY RUN' if a.dry_run else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
