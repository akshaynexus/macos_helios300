#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# test-stdin.sh: on Linux, run a tools/macos script the way the coordinator runs it on the Helios over SSH,
#     { echo MODE=...; cat script.sh; } | hbmac          (hbmac = ssh ... bash -s; asroot = sudo -A, passes stdin through)
# and check that no command the script runs reads the script's own stdin.
#
#   bash tools/macos/test-stdin.sh all                                 this repo's three scripts, every MODE
#   bash tools/macos/test-stdin.sh install-gpu.sh      [MODE...]       the release installer (dry-run install revert status)
#   bash tools/macos/test-stdin.sh audio-applehda.sh   [MODE...]       audio option 2 (dry-run install aux revert status)
#   bash tools/macos/test-stdin.sh audio-voodoohda.sh  [MODE...]       audio option 1 (dry-run install revert status)
#   bash tools/macos/test-stdin.sh dev-install <driver>/tools/macos/install-gpu.sh [MODE...]
#   bash tools/macos/test-stdin.sh gpu-apps    <driver>/tools/macos/gpu-apps.sh
# A path to another copy of install-gpu.sh / audio-*.sh (an older revision, say) is tested as that script.
# VERBOSE=1 prints each run's output.
#
# The sandbox: a fake asroot (a shell function, like hbmac's) that READS ALL OF ITS STDIN, logs what it was asked to
# run and runs nothing, except the fake kmutil (so a collection "built" as root exists and can be inspected); fake
# macOS tools on PATH: sw_vers, uname -r/-m, nvram, csrutil, ioreg, shasum, stat -f, id, chown, df -g, fdesetup,
# sysctl, system_profiler, log, xattr, plutil, osascript, and the ones that can read stdin (ditto, kmutil, codesign,
# hdiutil, curl, diskutil, pkgutil, defaults, PlistBuddy) reading all of it. kmutil create writes the bundle IDs of
# the kexts it was given with -p into the -A file and kmutil inspect prints them, so a collection that leaves a kext
# out is seen. Fake downloads, images, a KDK and a mounted root volume for the audio scripts; shasum maps their
# sha256 to the pinned one (sandbox only). /Library, /System/Library, /System/Volumes and /Applications are re-rooted
# into the sandbox (absolute paths only; $HOME/Library and paths inside other folders are left alone), and
# /usr/libexec/PlistBuddy is the fake. After the script a sentinel line is sent on the same stdin.
# PASS: the sentinel ran (so the script also ran to its end) and no fake read a byte of stdin. A command that
# swallowed the stream is named with the byte count it got. SIMULATION: nothing touches the real /Library.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
T=${1:?usage: test-stdin.sh all|install-gpu.sh|audio-applehda.sh|audio-voodoohda.sh|dev-install|gpu-apps [script] [MODE...]}; shift
if [ "$T" = all ]; then
	rc=0; for s in install-gpu.sh audio-applehda.sh audio-voodoohda.sh; do bash "$0" "$s" || rc=1; done
	[ "$rc" = 0 ] && echo "test-stdin all: PASS" || echo "test-stdin all: FAIL"; exit $rc
fi
case "$T" in
	dev-install) SCRIPT=${1:?dev-install needs the driver repo install-gpu.sh}; shift; KIND=dev ;;
	gpu-apps) SCRIPT=${1:?gpu-apps needs the driver repo gpu-apps.sh}; shift; KIND=apps ;;
	*) SCRIPT=$T; [ -f "$SCRIPT" ] || SCRIPT=$HERE/$T
	   case "$(basename "$T")" in
		*applehda*) KIND=applehda ;;
		*voodoohda*) KIND=voodoohda ;;
		*) KIND=release ;;
	   esac ;;
esac
[ -f "$SCRIPT" ] || { echo "test-stdin: no $SCRIPT"; exit 2; }
MODES=("$@")
if [ ${#MODES[@]} = 0 ]; then
	case "$KIND" in
		apps) MODES=(list add check remove) ;;
		applehda) MODES=(dry-run install aux revert status) ;;
		*) MODES=(dry-run install revert status) ;;
	esac
fi

S=$(mktemp -d "${TMPDIR:-/tmp}/test-stdin.XXXXXX"); trap 'rm -rf "$S"' EXIT
mkdir -p "$S/bin" "$S/rel" "$S/stage" "$S/home" "$S/cache" "$S/shamap" "$S/dl" "$S/images"
SW=$S/swallowed; ROOT=$S/asroot.log; : > "$SW"; : > "$ROOT"
R0=$S/root

# reader: a fake for a command that can read stdin (a prompt, a script on stdin); it reads ALL of it and reports it.
# plain: a fake for a command that never reads stdin when given its operands (shasum FILE, ioreg, sw_vers ...).
# The body comes on stdin; the fakes find the sandbox through FAKE_S.
reader() { { printf '#!/bin/bash\n_in=$(cat); [ -z "$_in" ] || printf "%%s read %%s bytes of stdin\\n" "%s $*" "${#_in}" >> "$FAKE_S/swallowed"\n' "$1"; cat; } > "$S/bin/$1"; chmod +x "$S/bin/$1"; }
plain() { { printf '#!/bin/bash\n'; cat; } > "$S/bin/$1"; chmod +x "$S/bin/$1"; }
plain sw_vers <<<'echo 25G332'
plain uname <<<'case "${1:-}" in -r) echo 25.6.0;; -m) echo x86_64;; *) exec /usr/bin/uname "$@";; esac'
# csr-active-config 0x0a7f: 0x1 0x2 0x800 (what audio option 2 needs) and 0x200
plain nvram <<<'case "${1:-}" in csr-active-config) printf "csr-active-config\t%%7f%%0a%%00%%00\n";; boot-args) printf "boot-args\t-v keepsyms=1\n";; esac'
plain csrutil <<<'echo "System Integrity Protection status: unknown (Custom Configuration)."'
plain ioreg <<<'case " $* " in *" PG106 "*) printf "    \"PG106Ready\" = Yes\n    \"PG106Stage\" = \"ready\"\n";; esac'
# shasum -a 256 FILE: the real sha256, or the pinned one a sandbox stand-in maps to ($S/shamap/<real> holds it)
plain shasum <<'EOF'
shift 2
[ $# -gt 0 ] || { sha256sum | sed 's/ .*/  -/'; exit; }
for f; do
	[ -f "$f" ] || { echo "shasum: $f: No such file or directory" >&2; exit 1; }
	h=$(sha256sum "$f" | cut -d' ' -f1); [ -f "$FAKE_S/shamap/$h" ] && h=$(cat "$FAKE_S/shamap/$h")
	printf '%s  %s\n' "$h" "$f"
done
EOF
plain stat <<'EOF'
[ "${1:-}" = -f ] || exec /usr/bin/stat "$@"
fmt=$2; f=${3:-}
[ -e "$f" ] || { echo "stat: $f: stat: No such file or directory" >&2; exit 1; }
fmt=${fmt//%Su/root}; fmt=${fmt//%Sg/wheel}; fmt=${fmt//%Lp/755}; fmt=${fmt//%Sm/Nov  8 00:00:00 2026}
fmt=${fmt//%z/$(/usr/bin/stat -c %s "$f")}
printf '%s\n' "$fmt"
EOF
plain id <<<'echo 0'
plain chown <<<'exit 0'
plain df <<<'printf "Filesystem 1G-blocks Used Available Capacity Mounted on\n/dev/disk1s5s1 466 120 300 29%% /\n"'
plain fdesetup <<<'echo "FileVault is Off."'
plain sysctl <<<'exit 1'
plain system_profiler <<<'echo "Audio:"'
plain log <<<'exit 0'
plain xattr <<<'exit 0'
reader ditto <<<'if [ "$1" = -x ]; then unzip -q -o "$3" -d "$4" </dev/null; else cp -R "$1" "$2"; fi'
reader codesign <<<'exit 0'
reader diskutil <<'EOF'
case "$*" in
	"info -plist /") printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>APFSSnapshot</key><true/><key>DeviceIdentifier</key><string>disk1s5s1</string></dict></plist>\n';;
	"info /") printf '   Device Identifier:         disk1s5s1\n   Sealed:                    Yes\n';;
esac
EOF
reader pkgutil <<'EOF'
case "${1:-}" in --check-signature) printf 'Package "KernelDebugKit.pkg":\n   Status: signed by Apple\n';; --pkg-info) exit 1;; esac
EOF
# PlistBuddy -c CMD... FILE: Print :a:b reads the plist; Set/Add/Delete do nothing
reader PlistBuddy <<'EOF'
exec python3 -I -c '
import plistlib, sys
a = sys.argv[1:]; f = a[-1]; cmds = [a[i + 1] for i in range(len(a) - 1) if a[i] == "-c"]
try: d = plistlib.load(open(f, "rb"))
except Exception: sys.exit("Error Reading File: " + f)
for c in cmds:
    if not c.startswith("Print"): continue
    v = d
    for k in [k for k in c[5:].strip().replace("\\ ", " ").split(":") if k]:
        if not isinstance(v, dict) or k not in v: sys.exit("Print: Entry, \"%s\", Does Not Exist" % c[5:].strip())
        v = v[k]
    print(("Dict {\n" + "".join("    %s = %s\n" % kv for kv in v.items()) + "}") if isinstance(v, dict) else str(v).lower() if isinstance(v, bool) else v)
' "$@"
EOF
# defaults read <path without .plist> <key>
reader defaults <<'EOF'
[ "${1:-}" = read ] || exit 0
exec python3 -I -c '
import plistlib, sys
try: print(plistlib.load(open(sys.argv[1] + ".plist", "rb"))[sys.argv[2]])
except Exception: sys.exit(1)
' "$2" "$3"
EOF
# plutil: -extract KEY raw - (the data on stdin, piped by the script: not counted), -lint, -p
plain plutil <<'EOF'
case "${1:-}" in
	-extract) exec python3 -I -c 'import plistlib, sys; d = plistlib.loads(sys.stdin.buffer.read()); v = d[sys.argv[1]]; print(str(v).lower() if isinstance(v, bool) else v)' "$2";;
	-lint) exit 0;;
	-p) cat "$2";;
esac
EOF
reader kmutil <<'EOF'
case "${1:-}" in
	showloaded) for k in as.vit9696.Lilu:1.7.1 as.vit9696.AppleALC:1.9.6 com.nullmoth.NMHDA:1.0.0 com.dhinakg.AMFIPass:1.4.1 org.helios300.pg106:0.1.0; do
		printf '   70    0 0xffffff7f80000000 0x1000     0x1000     %s (%s) 00000000-0000-0000-0000-000000000000 <8 6 5 3 2 1>\n' "${k%%:*}" "${k#*:}"; done;;
	create) out=""; ids=()
		while [ $# -gt 0 ]; do
			case "$1" in
				-A) out=$2; shift;;
				-p) id=$("$FAKE_S/bin/PlistBuddy" -c 'Print :CFBundleIdentifier' "$2/Contents/Info.plist" </dev/null) || { echo "kmutil: $2: no Info.plist" >&2; exit 1; }
				    ids+=("$id"); shift;;
			esac; shift
		done
		[ -z "$out" ] || { for i in "${ids[@]}"; do printf '%s (1.0)\n' "$i"; done; } > "$out";;
	inspect) while [ $# -gt 0 ] && [ "$1" != -A ]; do shift; done; cat "${2:?}";;
esac
exit 0
EOF
# hdiutil attach ... -mountpoint M ... IMAGE: the sandbox's image of that name copied to M; detach: nothing
reader hdiutil <<'EOF'
[ "${1:-}" = attach ] || exit 0
m=""; for ((i = 1; i <= $#; i++)); do [ "${!i}" = -mountpoint ] && { j=$((i + 1)); m=${!j}; }; done
img=$FAKE_S/images/$(basename "${!#}")
[ -n "$m" ] && [ -d "$img" ] || { echo "hdiutil: attach failed" >&2; exit 1; }
cp -R "$img/." "$m/"
EOF
# curl -o OUT URL: the sandbox's download of that name, or HTTP 404
reader curl <<'EOF'
o=""; u=""; while [ $# -gt 0 ]; do case "$1" in -o) o=$2; shift;; -*) ;; *) u=$1;; esac; shift; done
f=$FAKE_S/dl/$(basename "$u"); [ -n "$o" ] && [ -f "$f" ] || exit 22
cp "$f" "$o"
EOF
# osascript -l JavaScript - manifest.json: the script's JS (on its heredoc stdin, so not counted) turned manifest.json
# into tab-separated lines; this does the same in Python
plain osascript <<'EOF'
cat >/dev/null   # the JavaScript, from the script's own here-document
exec python3 -I -c '
import json, sys
m = json.load(open(sys.argv[1]))
print("\t".join(["M", m["release"]["version"], m["macos"]["min_kernel"], m["macos"]["max_kernel"]]))
for a in m["artifacts"]:
    print("\t".join(["A", a["name"], a["file"], a["sha256"], a["install_path"]]))
    for k in sorted(a.get("contents_sha256", {})):
        print("\t".join(["C", a["name"], k, a["contents_sha256"][k]]))
' "${@: -1}"
EOF

# The release folder: PG106FB.kext, PG106Accel.kext, NVMTLDriver.bundle, NVK-Pascal (zips), the translator (a file)
python3 -I - "$S/rel" "$S/stage" <<'PY'
import hashlib, json, os, sys, zipfile
rel, stage = sys.argv[1], sys.argv[2]
def h(p): return hashlib.sha256(open(p, 'rb').read()).hexdigest()
arts = []
def zipped(name, files, install):
    zp = os.path.join(rel, name + '.zip'); inner = {}
    with zipfile.ZipFile(zp, 'w') as z:
        for rp, data in files.items():
            z.writestr(name + '/' + rp, data); inner[rp] = hashlib.sha256(data).hexdigest()
    arts.append({'name': name, 'file': name + '.zip', 'sha256': h(zp), 'install_path': install, 'contents_sha256': inner})
zipped('PG106FB.kext', {'Contents/Info.plist': b'<plist/>', 'Contents/MacOS/PG106FB': b'fb'}, '/Library/Extensions')
zipped('PG106Accel.kext', {'Contents/Info.plist': b'<plist/>', 'Contents/MacOS/PG106Accel': b'acc'}, '/Library/Extensions')
zipped('NVMTLDriver.bundle', {'Contents/Info.plist': b'<plist/>', 'Contents/MacOS/NVMTLDriver': b'plug'}, '/Library/GPUBundles')
zipped('NVK-Pascal', {'libvulkan_nouveau.dylib': b'nvk', 'nvk_icd.json': b'{}'}, '/Library/GPUBundles/nvmtl')
p = os.path.join(rel, 'libnvmtl_translate.dylib'); open(p, 'wb').write(b'xlate')
arts.append({'name': 'libnvmtl_translate.dylib', 'file': 'libnvmtl_translate.dylib', 'sha256': h(p), 'install_path': '/Library/GPUBundles/nvmtl'})
json.dump({'schema': 1, 'release': {'version': '0.0-test'}, 'macos': {'min_kernel': '25.0', 'max_kernel': '25.99'},
           'artifacts': arts}, open(os.path.join(rel, 'manifest.json'), 'w'))
# the developer installer takes unpacked artifacts (SRC=)
for d in ('PG106FB.kext/Contents/MacOS', 'PG106Accel.kext/Contents/MacOS', 'NVMTLDriver.bundle/Contents/MacOS', 'NVK-Pascal'):
    os.makedirs(os.path.join(stage, d), exist_ok=True)
for f in ('PG106FB.kext/Contents/MacOS/PG106FB', 'PG106Accel.kext/Contents/MacOS/PG106Accel', 'NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver',
          'libnvmtl_translate.dylib', 'NVK-Pascal/libvulkan_nouveau.dylib', 'NVK-Pascal/nvk_icd.json'):
    open(os.path.join(stage, f), 'wb').write(b'x')
PY
printf 'allow\n' > "$S/nvmtl-allow.txt"

# The re-rooted Mac: /Library/Extensions holds PG106FB (recorded as installed by install-gpu.sh), VoodooHDA and one
# other third-party kext; an auxiliary collection; a KDK; the root volume "mounted" at /System/Volumes/Update/mnt1;
# the audio downloads and the PatcherSupportPkg image; their sha256 mapped to the scripts' pins.
pin() { sed -n "s/^$1=\([0-9a-f]*\).*/\1/p" "$HERE/$2" | head -1; }
python3 -I - "$S" "$R0" "$(pin ZIP_SHA audio-voodoohda.sh)" "$(pin BIN_SHA audio-voodoohda.sh)" "$(pin PLIST_SHA audio-voodoohda.sh)" \
	"$(pin UB_SHA audio-applehda.sh)" "$(pin HDA_BIN audio-applehda.sh)" <<'PY'
import hashlib, os, plistlib, sys, zipfile
S, R0, zip_sha, bin_sha, plist_sha, ub_sha, hda_bin = sys.argv[1:]
def put(p, data):
    os.makedirs(os.path.dirname(p), exist_ok=True); open(p, 'wb').write(data); return data
def pmap(data, pinned):
    if pinned: open(os.path.join(S, 'shamap', hashlib.sha256(data).hexdigest()), 'w').write(pinned)
def kext(path, ident, binary=b'bin', extra=None):
    pl = plistlib.dumps(dict({'CFBundleIdentifier': ident, 'CFBundleShortVersionString': '1.0', 'CFBundleVersion': '1.0'}, **(extra or {})))
    put(path + '/Contents/Info.plist', pl); put(path + '/Contents/MacOS/' + os.path.basename(path)[:-5], binary); return pl
ext = R0 + '/Library/Extensions'
kext(ext + '/PG106FB.kext', 'org.helios300.pg106fb')
kext(ext + '/Other.kext', 'com.example.Other')
vbin = b'VoodooHDA 3.6.8 stand-in'
vpl = kext(ext + '/VoodooHDA.kext', 'org.voodoo.driver.VoodooHDA', vbin, {
    'CFBundleShortVersionString': '3.6.8', 'OSBundleLibraries': {'com.apple.iokit.IOAudioFamily': '1.1'},
    'IOKitPersonalities': {'VoodooHDA': {'MixerValues': {'PCM': 95, 'Rec': 95}, 'InhibitCache': False}}})
pmap(vbin, bin_sha); pmap(vpl, plist_sha)
put(R0 + '/Library/KernelCollections/AuxiliaryKernelExtensions.kc', b'old aux\n')
# the release zip (the same kext), its sha256 mapped to ZIP_SHA
zp = os.path.join(S, 'dl', 'VoodooHDA.kext-368.zip')
with zipfile.ZipFile(zp, 'w') as z:
    z.writestr('VoodooHDA.kext/Contents/Info.plist', vpl); z.writestr('VoodooHDA.kext/Contents/MacOS/VoodooHDA', vbin)
pmap(open(zp, 'rb').read(), zip_sha)
# PatcherSupportPkg's Universal-Binaries.dmg (verified, in the cache) and what it mounts as
pmap(put(S + '/cache/Universal-Binaries.dmg', b'Universal-Binaries stand-in\n'), ub_sha)
hda = S + '/images/Universal-Binaries.dmg/26.0 Beta 1/System/Library/Extensions/AppleHDA.kext'
hbin = b'AppleHDA 600.2 stand-in'
put(hda + '/Contents/Info.plist', plistlib.dumps({'CFBundleIdentifier': 'com.apple.driver.AppleHDA', 'CFBundleVersion': '600.2'}))
pmap(put(hda + '/Contents/MacOS/AppleHDA', hbin), hda_bin)
# an installed KDK for 25G332, and the root volume at the update mount point (already holding what install copies)
sv = plistlib.dumps({'ProductBuildVersion': '25G332'})
kdk = R0 + '/Library/Developer/KDKs/KDK_26.7.2_25G332.kdk'
put(kdk + '/System/Library/CoreServices/SystemVersion.plist', sv)
put(kdk + '/System/Library/Extensions/System.kext/PlugIns/Libkern.kext/Libkern', b'libkern')
mnt = R0 + '/System/Volumes/Update/mnt1'
put(mnt + '/System/Library/CoreServices/SystemVersion.plist', sv)
put(mnt + '/System/Library/Extensions/System.kext/PlugIns/Libkern.kext/Libkern', b'libkern')
put(mnt + '/System/Library/Extensions/AppleHDA.kext/Contents/Info.plist', open(hda + '/Contents/Info.plist', 'rb').read())
put(mnt + '/System/Library/Extensions/AppleHDA.kext/Contents/MacOS/AppleHDA', hbin)
# audio option 2's saved collection for the sealed snapshot (what revert puts back)
put(R0 + '/Library/Helios300/audio-applehda/aux-sealed.kc', b'sealed aux\n')
put(R0 + '/Library/Helios300/audio-applehda/aux-sealed.lext', b'')
PY

# The script runs with absolute /Library, /System/Library, /System/Volumes and /Applications paths re-rooted under
# the sandbox (a copy; the original is untouched), so the modes that read state there get somewhere to read it from.
B='(^|[^A-Za-z0-9_.$}/-])'
sed -E -e "s#$B/System/Library/#\1@SYSLIB@#g" -e "s#$B/System/Volumes/#\1@SYSVOL@#g" \
	-e "s#$B/Library/#\1$R0/Library/#g" -e "s#$B/Applications/#\1$R0/Applications/#g" \
	-e "s#@SYSLIB@#$R0/System/Library/#g" -e "s#@SYSVOL@#$R0/System/Volumes/#g" \
	-e "s#/usr/libexec/PlistBuddy#$S/bin/PlistBuddy#g" "$SCRIPT" > "$S/script.sh"
mkdir -p "$R0/Library/GPUBundles" "$R0/System/Library/KernelCollections" "$R0/System/Library/Extensions" \
	"$R0/Library/Helios300/install-gpu" "$R0/Library/Helios300/gpu-backup/20261108-000000" "$R0/Library/PreferencePanes"
printf '%s\n' "$R0/Library/Extensions/PG106FB.kext" "$R0/Library/GPUBundles/nvmtl-allow.txt" > "$R0/Library/Helios300/install-gpu/installed.txt"
touch "$R0/Library/Helios300/gpu-backup/20261108-000000/PG106FB.kext.absent" "$R0/Library/Helios300/gpu-backup/20261108-000000/AuxiliaryKernelExtensions.kc.absent"

fail=0
for mode in "${MODES[@]}"; do
	: > "$SW"; : > "$ROOT"
	case "$KIND" in
		release) prelude="MODE=$mode; FORCE=1; cd '$S/rel'" ;;
		dev) prelude="MODE=$mode; SRC='$S/stage'; FORCE=1" ;;
		apps) prelude="NVMTL_ALLOW='$S/nvmtl-allow.txt'; set -- $mode $( [ "$mode" = list ] || echo SandboxApp )" ;;
		applehda) prelude="MODE=$mode; FORCE=1; DL=0; CACHE='$S/cache'" ;;
		voodoohda) prelude="MODE=$mode" ;;
	esac
	# asroot: reads its stdin, logs, runs only the fake kmutil (with /dev/null: what asroot read is already counted)
	out=$( { printf '%s\n' "$prelude"
	         printf '%s\n' 'asroot() { local _in; _in=$(cat); [ -z "$_in" ] || printf "asroot %s read %s bytes of stdin\n" "$1" "${#_in}" >> "'"$SW"'"; printf "%s\n" "$*" >> "'"$ROOT"'"; case "$1" in kmutil|*/kmutil) shift; "'"$S"'/bin/kmutil" "$@" </dev/null;; esac; }'
	         cat "$S/script.sh"
	         printf '%s\n' 'echo STDIN-SENTINEL-REACHED' ; } | env PATH="$S/bin:$PATH" TMPDIR="$S" HOME="$S/home" FAKE_S="$S" bash -s 2>&1 )
	rc=$?
	reached=no; grep -q '^STDIN-SENTINEL-REACHED$' <<< "$out" && reached=yes
	nroot=$(wc -l < "$ROOT" | tr -d ' ')
	if [ "$reached" = yes ] && [ ! -s "$SW" ]; then
		echo "PASS $KIND MODE=$mode: the line after the script ran, nothing read the script's stdin ($nroot asroot calls)"
	else
		fail=1
		echo "FAIL $KIND MODE=$mode: sentinel reached: $reached (exit $rc); stdin readers:"
		sed 's/^/    /' "$SW"; [ -s "$SW" ] || echo "    (none; the script stopped:)"; printf '%s\n' "$out" | tail -5 | sed 's/^/    | /'
	fi
	[ "${VERBOSE:-0}" = 1 ] && printf '%s\n' "$out" | sed 's/^/    > /'
done
[ "$fail" = 0 ] && echo "test-stdin: PASS ($SCRIPT)" || echo "test-stdin: FAIL ($SCRIPT)"
exit $fail
