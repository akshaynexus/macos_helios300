#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# test-stdin.sh: on Linux, run a tools/macos script the way the coordinator runs it on the Helios over SSH,
#     { echo MODE=...; cat script.sh; } | hbmac          (hbmac = ssh ... bash -s; asroot = sudo -A, passes stdin through)
# and check that no command the script runs reads the script's own stdin.
#
#   bash tools/macos/test-stdin.sh install-gpu.sh  [MODE...]      this repo's release installer (default modes: dry-run install revert status)
#   bash tools/macos/test-stdin.sh dev-install <driver>/tools/macos/install-gpu.sh [MODE...]
#   bash tools/macos/test-stdin.sh gpu-apps    <driver>/tools/macos/gpu-apps.sh
#
# The sandbox: a fake asroot (a shell function, like hbmac's) that READS ALL OF ITS STDIN, logs what it was asked to
# run and runs nothing; fake macOS tools (sw_vers, uname -r/-m, nvram, csrutil, ioreg, shasum, osascript, ditto, kmutil,
# codesign, hdiutil, curl, stat -f, id, chown) on PATH, the ones that can read stdin (ditto, kmutil, codesign, hdiutil,
# curl) reading all of it; a fake release folder with a manifest.json; /Library re-rooted into the sandbox.
# After the script a sentinel line is sent on the same stdin. PASS: the sentinel ran and no fake read a byte of stdin.
# A command that swallowed the stream is named with the byte count it got. SIMULATION: nothing touches /Library.
set -uo pipefail
T=${1:?usage: test-stdin.sh install-gpu.sh|dev-install|gpu-apps [script] [MODE...]}; shift
HERE=$(cd "$(dirname "$0")" && pwd)
case "$T" in
	install-gpu.sh) SCRIPT=$HERE/install-gpu.sh; KIND=release ;;
	dev-install) SCRIPT=${1:?dev-install needs the driver repo install-gpu.sh}; shift; KIND=dev ;;
	gpu-apps) SCRIPT=${1:?gpu-apps needs the driver repo gpu-apps.sh}; shift; KIND=apps ;;
	*) SCRIPT=$T; KIND=release ;;
esac
[ -f "$SCRIPT" ] || { echo "test-stdin: no $SCRIPT"; exit 2; }
MODES=("$@"); [ ${#MODES[@]} -gt 0 ] || { [ "$KIND" = apps ] && MODES=(list add check remove) || MODES=(dry-run install revert status); }

S=$(mktemp -d "${TMPDIR:-/tmp}/test-stdin.XXXXXX"); trap 'rm -rf "$S"' EXIT
mkdir -p "$S/bin" "$S/rel" "$S/stage"
SW=$S/swallowed; ROOT=$S/asroot.log; : > "$SW"; : > "$ROOT"

# reader: a fake for a command that can read stdin (a prompt, a script on stdin); it reads ALL of it and reports it.
# plain: a fake for a command that never reads stdin when given its operands (shasum FILE, ioreg, sw_vers ...).
reader() { # name body...
	local n=$1; shift
	{ printf '#!/bin/bash\n_in=$(cat); [ -z "$_in" ] || printf "%%s read %%s bytes of stdin\\n" "%s $*" "${#_in}" >> "%s"\n' "$n" "$SW"
	  printf '%s\n' "$@"; } > "$S/bin/$n"
	chmod +x "$S/bin/$n"
}
plain() { local n=$1; shift; { printf '#!/bin/bash\n'; printf '%s\n' "$@"; } > "$S/bin/$n"; chmod +x "$S/bin/$n"; }
plain sw_vers 'echo 25G332'
plain uname 'case "${1:-}" in -r) echo 25.6.0;; -m) echo x86_64;; *) exec /usr/bin/uname "$@";; esac'
plain nvram 'case "${1:-}" in csr-active-config) printf "csr-active-config\t%%7f%%00%%00%%00\n";; boot-args) printf "boot-args\t-v keepsyms=1\n";; esac'
plain csrutil 'echo "System Integrity Protection status: unknown (Custom Configuration)."'
plain ioreg 'case " $* " in *" PG106 "*) printf "    \"PG106Ready\" = Yes\n    \"PG106Stage\" = \"ready\"\n";; esac'
plain shasum 'shift 2; sha256sum "$@"'
plain stat 'echo "root:wheel 755"'
plain id 'echo 0'
plain chown 'exit 0'
reader ditto 'if [ "$1" = -x ]; then unzip -q -o "$3" -d "$4" </dev/null; else cp -R "$1" "$2"; fi'
reader kmutil 'exit 0'
reader codesign 'exit 0'
reader hdiutil 'exit 0'
reader curl 'exit 0'
# osascript -l JavaScript - manifest.json: the script's JS (on its heredoc stdin, so not counted) turned manifest.json
# into tab-separated lines; this does the same in Python
cat > "$S/bin/osascript" <<EOF
#!/bin/bash
cat >/dev/null   # the JavaScript, from the script's own here-document
exec python3 -I -c '
import json, sys
m = json.load(open(sys.argv[1]))
print("\t".join(["M", m["release"]["version"], m["macos"]["min_kernel"], m["macos"]["max_kernel"]]))
for a in m["artifacts"]:
    print("\t".join(["A", a["name"], a["file"], a["sha256"], a["install_path"]]))
    for k in sorted(a.get("contents_sha256", {})):
        print("\t".join(["C", a["name"], k, a["contents_sha256"][k]]))
' "\${@: -1}"
EOF
chmod +x "$S/bin/osascript"

# the release folder: PG106FB.kext, PG106Accel.kext, NVMTLDriver.bundle, NVK-Pascal (zips), the translator (a file)
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
# The script runs with /Library and /System/Library re-rooted under the sandbox (a copy; the original is untouched),
# so the modes that read state there (revert: the install record, the backups) get somewhere to read it from.
R0=$S/root
sed -e "s#/System/Library/#@SYSLIB@#g" -e "s#/Library/#$R0/Library/#g" -e "s#@SYSLIB@#$R0/System/Library/#g" \
	"$SCRIPT" > "$S/script.sh"
mkdir -p "$R0/Library/Extensions/PG106FB.kext/Contents/MacOS" "$R0/Library/GPUBundles" "$R0/Library/KernelCollections" \
	"$R0/System/Library/KernelCollections" "$R0/Library/Helios300/install-gpu" "$R0/Library/Helios300/gpu-backup/20261108-000000"
printf '%s\n' "$R0/Library/Extensions/PG106FB.kext" "$R0/Library/GPUBundles/nvmtl-allow.txt" > "$R0/Library/Helios300/install-gpu/installed.txt"
touch "$R0/Library/Helios300/gpu-backup/20261108-000000/PG106FB.kext.absent" "$R0/Library/Helios300/gpu-backup/20261108-000000/AuxiliaryKernelExtensions.kc.absent"

fail=0
for mode in "${MODES[@]}"; do
	: > "$SW"; : > "$ROOT"
	case "$KIND" in
		release) prelude="MODE=$mode; FORCE=1; cd '$S/rel'" ;;
		dev) prelude="MODE=$mode; SRC='$S/stage'; FORCE=1" ;;
		apps) prelude="NVMTL_ALLOW='$S/nvmtl-allow.txt'; set -- $mode $( [ "$mode" = list ] || echo SandboxApp )" ;;
	esac
	out=$( { printf '%s\n' "$prelude"
	         printf '%s\n' 'asroot() { local _in; _in=$(cat); [ -z "$_in" ] || printf "asroot %s read %s bytes of stdin\n" "$1" "${#_in}" >> "'"$SW"'"; printf "%s\n" "$*" >> "'"$ROOT"'"; }'
	         cat "$S/script.sh"
	         printf '%s\n' 'echo STDIN-SENTINEL-REACHED' ; } | env PATH="$S/bin:$PATH" TMPDIR="$S" bash -s 2>&1 )
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
done
[ "$fail" = 0 ] && echo "test-stdin: PASS ($SCRIPT)" || echo "test-stdin: FAIL ($SCRIPT)"
exit $fail
