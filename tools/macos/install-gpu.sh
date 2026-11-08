#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# The GTX 1060 (GP106) display and Metal stack for the Helios 300 on macOS 26 (25G332), next to the OpenCore-injected
# PG106.kext, from a macos_helios300 release: every file checked against manifest.json first (docs/MANIFEST.md).
#
#   PG106FB.kext, PG106Accel.kext   -> /Library/Extensions (root:wheel 755, kmutil load -p). OpenCore cannot inject
#                                      them: IOGraphicsFamily and IOAcceleratorFamily2 are only in the System KC.
#                                      A copy put there by hand is replaced (and kept in the backup).
#   NVMTLDriver.bundle              -> /Library/GPUBundles/NVMTLDriver.bundle (PG106Accel's MetalPluginName)
#   libnvmtl_translate.dylib        -> /Library/GPUBundles/nvmtl/libnvmtl_translate.dylib
#   NVK-Pascal (libvulkan_nouveau.dylib, nvk_icd.json) -> /Library/GPUBundles/nvmtl/
#   the per-app allow list          -> /Library/GPUBundles/nvmtl-allow.txt (written only when none exists; the plugin
#                                      gives the GP106 to the processes it names, every other one stays on the HD 630)
#
# Run as the logged-in user (it calls sudo itself), from the folder holding the release files and manifest.json
# (EFI/Helios300 on the stick, or a copy of it). MODE (default dry-run, which changes nothing):
#   MODE=dry-run   checks the Mac and every file against manifest.json, prints every command install would run
#   MODE=install   installs; restart afterwards (approve in System Settings > Privacy & Security if asked, restart again)
#   MODE=revert    removes everything install put there, rebuilds the auxiliary kernel collection from the kexts left in
#                  /Library/Extensions (each one checked in it; the previous one kept and put back on failure); restart
#   MODE=status    what is present, loaded and published now
# ONLY="PG106FB PG106Accel plugin xlate nvk allow" limits install to some parts (one change per boot).
# MANIFEST_SHA256=<sha256> refuses a manifest.json with another hash (the release's SHA256SUMS.txt has it).
# FORCE=1 skips the PG106Ready and macOS-build checks.
# The app list: /Library/GPUBundles/nvmtl-allow.txt, one process name per line (an app's CFBundleExecutable), "-name"
# denies, "#" comments, first match wins. One launch without a rule: NVMTL_ENABLE=1 /Applications/Foo.app/Contents/MacOS/Foo
# If the Mac does not come up: Shift+Enter in the OpenCore picker (safe mode, -x): PG106, PG106FB and PG106Accel stand
# down; then MODE=revert.
set -euo pipefail
MODE=${MODE:-dry-run}
ONLY=${ONLY:-"PG106FB PG106Accel plugin xlate nvk allow"}
SRC=$(pwd)
EXT=/Library/Extensions; GB=/Library/GPUBundles; NV=$GB/nvmtl; ALLOW=$GB/nvmtl-allow.txt
AUX=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
KB=/System/Library/KernelCollections/BootKernelExtensions.kc
KS=/System/Library/KernelCollections/SystemKernelExtensions.kc
STATE=/Library/Helios300/install-gpu
RECORD=$STATE/installed.txt
PINNED_BUILD=25G332

# stdin from /dev/null: run over SSH as `{ echo MODE=...; cat install-gpu.sh; } | hbmac`, the rest of this script is
# on stdin, and asroot (sudo -A) hands its stdin to the command, which would swallow the script (kmutil, ditto...)
R() { if declare -F asroot >/dev/null; then asroot "$@" </dev/null; else sudo "$@" </dev/null; fi; }
say() { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
die() { printf 'STOP: %s\n' "$*" >&2; exit 1; }
sha() { shasum -a 256 "$1" | awk '{print $1}'; }
live() { [ "$MODE" = install ] || [ "$MODE" = revert ]; }
run() { if live; then say "+ $*"; R "$@"; else say "would run: $*"; fi; }
want() { case " $ONLY " in *" $1 "*) return 0;; esac; return 1; }
ioprop() { ioreg -r -c "$1" -d 1 2>/dev/null | sed -n "s/^ *\"$2\" = //p" | head -1; }

# part | manifest artifact | what to take out of it -> target
PARTS=(
	"PG106FB|PG106FB.kext|PG106FB.kext|$EXT/PG106FB.kext"
	"PG106Accel|PG106Accel.kext|PG106Accel.kext|$EXT/PG106Accel.kext"
	"plugin|NVMTLDriver.bundle|NVMTLDriver.bundle|$GB/NVMTLDriver.bundle"
	"xlate|libnvmtl_translate.dylib|libnvmtl_translate.dylib|$NV/libnvmtl_translate.dylib"
	"nvk|NVK-Pascal|NVK-Pascal/libvulkan_nouveau.dylib|$NV/libvulkan_nouveau.dylib"
	"nvk|NVK-Pascal|NVK-Pascal/nvk_icd.json|$NV/nvk_icd.json"
)

WORK=$(mktemp -d "${TMPDIR:-/tmp}/install-gpu.XXXXXX")
cleanup() { rm -rf "$WORK" 2>/dev/null || R rm -rf "$WORK" 2>/dev/null || true; }
trap cleanup EXIT

# manifest.json -> tab-separated lines, read with JavaScript for Automation (on every Mac; python3 is not):
#   A <name> <file> <sha256> <install_path>
#   C <name> <path inside the bundle> <sha256>
manifest_lines() {
	osascript -l JavaScript - "$1" <<'JS'
ObjC.import('Foundation');
function run(argv) {
	var s = $.NSString.stringWithContentsOfFileEncodingError(argv[0], $.NSUTF8StringEncoding, null);
	if (!s || s.isNil()) throw new Error('cannot read ' + argv[0]);
	var m = JSON.parse(s.js), out = [];
	if (m.schema !== 1) throw new Error('manifest schema ' + m.schema + ' (want 1)');
	out.push(['M', m.release && m.release.version || '?', m.macos && m.macos.min_kernel || '', m.macos && m.macos.max_kernel || ''].join('\t'));
	(m.artifacts || []).forEach(function (a) {
		out.push(['A', a.name, a.file || '', a.sha256 || '', a.install_path || ''].join('\t'));
		var c = a.contents_sha256 || {};
		Object.keys(c).sort().forEach(function (k) { out.push(['C', a.name, k, c[k]].join('\t')); });
	});
	return out.join('\n');
}
JS
}

load_manifest() {
	[ -f "$SRC/manifest.json" ] || die "no manifest.json in $SRC: run this from the folder holding the release files"
	local msha; msha=$(sha "$SRC/manifest.json")
	say "manifest.json sha256 $msha"
	[ -z "${MANIFEST_SHA256:-}" ] || [ "$MANIFEST_SHA256" = "$msha" ] || die "manifest.json is not the one MANIFEST_SHA256 names"
	manifest_lines "$SRC/manifest.json" > "$WORK/manifest.tsv" || die "manifest.json does not parse"
	IFS=$'\t' read -r _ VERSION MINK MAXK < <(grep '^M' "$WORK/manifest.tsv")
	say "release $VERSION, macOS kernel $MINK .. $MAXK"
}
art_field() { # name field(2=file 3=sha 4=install_path)
	awk -F'\t' -v n="$1" -v f="$2" '$1 == "A" && $2 == n { print $(f + 1); exit }' "$WORK/manifest.tsv"
}

# Check one artifact (zip or single file) and stage it under $WORK/x/<name>. Returns 1 if not in this release.
verify() {
	local name=$1 file want got
	file=$(art_field "$name" 2); want=$(art_field "$name" 3)
	[ -n "$file" ] || { say "  $name: not in this release (skipped)"; return 1; }
	[ -f "$SRC/$file" ] || die "$file is listed in manifest.json but not in $SRC"
	got=$(sha "$SRC/$file")
	[ "$got" = "$want" ] || die "$file sha256 $got, manifest.json says $want"
	mkdir -p "$WORK/x"
	case "$file" in
	*.zip)
		rm -rf "$WORK/x/$name"
		ditto -x -k "$SRC/$file" "$WORK/x" </dev/null || die "cannot unzip $file"
		[ -d "$WORK/x/$name" ] || die "$file does not hold $name/ at its top"
		local n=0 k h p
		while IFS=$'\t' read -r _ _ p h; do
			[ -f "$WORK/x/$name/$p" ] || die "$name: $p is in manifest.json but not in $file"
			[ "$(sha "$WORK/x/$name/$p")" = "$h" ] || die "$name: $p does not match its manifest.json sha256"
			n=$((n + 1))
		done < <(awk -F'\t' -v n="$name" '$1 == "C" && $2 == n' "$WORK/manifest.tsv")
		k=$(find "$WORK/x/$name" -type f | wc -l | tr -d ' ')
		[ "$n" -gt 0 ] && [ "$k" = "$n" ] || die "$name: $k files in $file, manifest.json lists $n"
		find "$WORK/x/$name" -type l | grep -q . && die "$name: $file holds a symlink"
		say "  $name: $file sha256 ok, $n inner files ok"
		;;
	*)
		cp "$SRC/$file" "$WORK/x/$name"
		say "  $name: $file sha256 ok"
		;;
	esac
	return 0
}

csr_value() {
	local raw; raw=$(nvram csr-active-config 2>/dev/null | sed 's/^csr-active-config[[:space:]]*//') || true
	[ -n "$raw" ] || { echo 0; return; }
	printf '%b' "$(printf '%s' "$raw" | sed 's/%\([0-9a-fA-F][0-9a-fA-F]\)/\\x\1/g')" | od -An -tu4 | tr -d ' \n'
}

preflight() {
	step "macOS"
	local build; build=$(sw_vers -buildVersion)
	say "build $build, Darwin $(uname -r), $(uname -m)"
	[ "$(uname -m)" = x86_64 ] || die "not an Intel Mac"
	case "$(uname -r)" in 25.*) ;; *) die "this release is for macOS 26 (Darwin 25), not $(uname -r)";; esac
	[ "$build" = "$PINNED_BUILD" ] || [ "${FORCE:-0}" = 1 ] || die "the kexts were checked against $PINNED_BUILD's collections; this is $build (FORCE=1 to go on)"
	local csr; csr=$(csr_value)
	say "csr-active-config $(printf '0x%x' "$csr")"
	[ $((csr & 1)) -ne 0 ] || die "SIP: CSR_ALLOW_UNTRUSTED_KEXTS (0x1) is not set; the ad-hoc kexts would not load (1401 sets 030A0000)"
	step "PG106 (PG106FB and PG106Accel attach to it)"
	local ready; ready=$(ioprop PG106 PG106Ready)
	say "PG106Ready = ${ready:-absent}, PG106Stage = $(ioprop PG106 PG106Stage), PG106Error = $(ioprop PG106 PG106Error)"
	[ "$ready" = Yes ] || [ "${FORCE:-0}" = 1 ] || die "PG106 is not up (PG106Ready is not Yes): fix that first (FORCE=1 to go on)"
	step "Conflicts"
	for k in NVRM NVRMFB NVAccel NVRMAGDC; do
		[ ! -e "$EXT/$k.kext" ] || die "$EXT/$k.kext is installed: the NVRM stack must not be on this machine (manifest config.forbidden)"
	done
	say "no NVRM stack"
	step "Release files in $SRC"
	load_manifest
	HAVE=" "
	local p a
	for p in PG106FB PG106Accel plugin xlate nvk; do
		want "$p" || continue
		a=$(for it in "${PARTS[@]}"; do IFS='|' read -r q n _ _ <<< "$it"; [ "$q" = "$p" ] && echo "$n" && break; done)
		if verify "$a"; then HAVE="$HAVE$p "; fi
	done
}

has() { case "$HAVE" in *" $1 "*) return 0;; esac; return 1; }

install_all() {
	local bk; bk="$STATE/backup-$(date +%Y%m%d-%H%M%S)"
	step "Backup of what is there now -> $bk"
	run mkdir -p "$bk"
	local it p n s t
	for it in "${PARTS[@]}"; do
		IFS='|' read -r p n s t <<< "$it"; has "$p" || continue
		[ -e "$t" ] && run ditto "$t" "$bk/$(basename "$t")"
	done
	step "Install"
	run mkdir -p "$NV"
	run chown root:wheel "$GB" "$NV"
	run chmod 755 "$GB" "$NV"
	: > "$WORK/record"
	for it in "${PARTS[@]}"; do
		IFS='|' read -r p n s t <<< "$it"; has "$p" || continue
		run rm -rf "$t"
		run ditto "$WORK/x/$s" "$t"
		run chown -R root:wheel "$t"
		case "$t" in
			*.kext|*.bundle|*.dylib) run chmod -R 755 "$t" ;;
			*) run chmod 644 "$t" ;;
		esac
		echo "$t" >> "$WORK/record"
	done
	if want allow; then
		if [ -f "$ALLOW" ]; then say "allow list kept: $ALLOW"
		else
			default_allow > "$WORK/nvmtl-allow.txt"
			run cp "$WORK/nvmtl-allow.txt" "$ALLOW"
			run chown root:wheel "$ALLOW"
			run chmod 644 "$ALLOW"
			echo "$ALLOW" >> "$WORK/record"
		fi
	fi
	# the record of what this install put there (revert removes exactly that)
	if live; then
		R mkdir -p "$STATE"
		cat "$RECORD" "$WORK/record" 2>/dev/null | sort -u > "$WORK/record.all" || true
		R cp "$WORK/record.all" "$RECORD"
		R chmod 644 "$RECORD"
	else
		say "would record: $(tr '\n' ' ' < "$WORK/record")-> $RECORD"
	fi
	step "Kernel extensions: kmutil load -p (macOS may ask for approval in System Settings > Privacy & Security)"
	for p in PG106FB PG106Accel; do
		has "$p" || continue
		if live; then
			say "+ kmutil load -p $EXT/$p.kext"
			R kmutil load -p "$EXT/$p.kext" 2>&1 | sed 's/^/  /' || say "  kmutil load -p $p returned an error: if it asks for approval, allow it in System Settings and restart"
		else
			say "would run: kmutil load -p $EXT/$p.kext"
		fi
	done
	say ""
	say "Restart. If System Settings > Privacy & Security reports blocked system software, allow it and restart again."
	say "Then: MODE=status bash install-gpu.sh. Backup of the previous files: $bk. Allow list: $ALLOW"
}

# The auxiliary collection without the recorded kexts, as audio-applehda.sh's aux_build/aux_publish (kept inline: this
# script runs piped over SSH and from the release folder, with nothing to source). kmutil create links in only the
# kexts named with -p (--repository just resolves dependencies), so every kext that stays in /Library/Extensions
# (VoodooHDA, any other third-party one) gets its own -p, and each bundle ID must show up in kmutil inspect.
aux_keep() { # the kexts in $EXT that revert leaves there
	local k; for k in "$EXT"/*.kext; do
		[ -d "$k" ] || continue
		grep -qxF "$k" "$RECORD" 2>/dev/null && continue
		echo "$k"
	done
}
aux_build() { # out -> 0 built and checked, 2 no kext stays (no collection), 1 failed
	local out=$1 k id ids="" a=(create -n aux --volume-root / --allow-missing-kdk -a x86_64 -B "$KB")
	[ -s "$KS" ] && a+=(-S "$KS")
	[ -f /System/Library/Kernels/kernel ] && a+=(--kernel /System/Library/Kernels/kernel)
	a+=(--repository "$EXT")
	while IFS= read -r k; do
		id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$k/Contents/Info.plist" 2>/dev/null </dev/null) || id=""
		[ -n "$id" ] || { say "  $k has no CFBundleIdentifier"; return 1; }
		a+=(-p "$k"); ids="$ids $id"
	done < <(aux_keep)
	[ -n "$ids" ] || { say "  no other kext in $EXT: no auxiliary collection needed"; return 2; }
	a+=(-A "$out" -z)
	say "+ kmutil ${a[*]}"
	if ! R kmutil "${a[@]}" > "$WORK/aux-build.log" 2>&1; then sed 's/^/    /' "$WORK/aux-build.log" | tail -15; return 1; fi
	if ! R kmutil inspect -a x86_64 -A "$out" > "$WORK/aux-inspect.txt" 2>&1; then sed 's/^/    /' "$WORK/aux-inspect.txt" | tail -5; return 1; fi
	for id in $ids; do grep -qF "$id" "$WORK/aux-inspect.txt" || { say "  $id is not in the built collection"; return 1; }; done
	say "  built and inspected, contains$ids"
}
aux_publish() { # file -> 0 published; 1 not published ($AUX as it was: kept, or put back from aux-previous.kc)
	local had=0
	R mkdir -p "$STATE"
	if [ -f "$AUX" ]; then
		R cp -p "$AUX" "$STATE/aux-previous.kc" || { say "  cannot keep $AUX as $STATE/aux-previous.kc: not replaced"; return 1; }
		had=1
	fi
	if R cp "$1" "$AUX" && R chown root:wheel "$AUX" && R chmod 644 "$AUX" && R cmp -s "$1" "$AUX"; then
		say "  published $AUX (previous: $STATE/aux-previous.kc)"; return 0
	fi
	if [ "$had" = 1 ]; then
		R cp -p "$STATE/aux-previous.kc" "$AUX" && say "  publishing failed: $AUX put back from $STATE/aux-previous.kc" \
			|| say "  publishing failed and the restore too: sudo cp -p $STATE/aux-previous.kc $AUX"
	else R rm -f "$AUX"; say "  publishing failed: the partial $AUX removed (there was none before)"; fi
	return 1
}

revert_all() {
	step "Remove what install-gpu.sh installed ($RECORD)"
	[ -f "$RECORD" ] || die "no $RECORD: nothing was installed by this script"
	local t kexts=0 auxrc=0
	grep -q '\.kext$' "$RECORD" && kexts=1
	if [ "$kexts" = 1 ]; then
		# Built before anything is removed: if it fails, revert stops with the machine as it was.
		step "Auxiliary kernel collection without them, from the kexts that stay in $EXT"
		mkdir -p "$WORK/kc"
		aux_build "$WORK/kc/aux.kc" || auxrc=$?
		[ "$auxrc" != 1 ] || die "the auxiliary collection without them did not build (above): nothing removed, $AUX left as it was"
	fi
	step "Remove the files"
	while IFS= read -r t; do
		[ -n "$t" ] || continue
		case "$t" in "$EXT"/*|"$GB"/*) ;; *) say "  skipped $t (outside $EXT and $GB)"; continue;; esac
		if [ -e "$t" ]; then run rm -rf "$t"; else say "  $t: already gone"; fi
	done < "$RECORD"
	[ -d "$NV" ] && [ -z "$(ls -A "$NV" 2>/dev/null)" ] && run rmdir "$NV"
	if [ "$kexts" = 1 ]; then
		step "Publish the auxiliary collection"
		if [ "$auxrc" = 2 ]; then
			if [ -f "$AUX" ]; then R mkdir -p "$STATE"; run cp -p "$AUX" "$STATE/aux-previous.kc"; fi
			run rm -f "$AUX"
		elif ! aux_publish "$WORK/kc/aux.kc"; then
			say "The files are removed but $AUX still holds PG106FB/PG106Accel: run MODE=revert again (the record is kept)."
			die "auxiliary collection not replaced"
		fi
	fi
	R rm -f "$RECORD"
	say "Removed. Restart. The pre-install copies (if any) stay in $STATE/backup-*; the previous collection: $STATE/aux-previous.kc."
}

status() {
	step "macOS"
	say "build $(sw_vers -buildVersion), Darwin $(uname -r); boot-args: $(nvram boot-args 2>/dev/null | cut -f2- || echo '-')"
	step "Loaded"
	kmutil showloaded </dev/null 2>/dev/null | grep -E 'org\.helios300\.pg106|com\.nullmoth\.PG106Accel' | sed 's/^/  /' \
		|| say "  none of PG106, PG106FB, PG106Accel"
	step "Published (ioreg)"
	local k v
	for k in PG106Ready PG106Stage PG106Error PG106FirmwareEmbedded PG106WakeResult PG106LastSleep; do
		v=$(ioprop PG106 "$k"); [ -z "$v" ] || say "  PG106 $k = $v"; done
	for k in PG106FBSelfCheck PG106FBConnected PG106FBModeCount PG106FBAccelerated; do
		v=$(ioprop PG106FB "$k"); [ -z "$v" ] || say "  PG106FB $k = $v"; done
	ioreg -r -c PG106FB -d 1 2>/dev/null | grep -q . || say "  PG106FB: not attached"
	for k in PG106AccelState PG106AccelDisplay PG106AccelFramePath MetalPluginName; do
		v=$(ioprop PG106Accel "$k"); [ -z "$v" ] || say "  PG106Accel $k = $v"; done
	ioreg -r -c PG106Accel -d 1 2>/dev/null | grep -q . || say "  PG106Accel: not attached"
	step "Files"
	local it p n s t bin
	for it in "${PARTS[@]}"; do
		IFS='|' read -r p n s t <<< "$it"
		if [ -e "$t" ]; then
			bin=$t; [ -d "$t" ] && bin=$(find "$t/Contents/MacOS" -maxdepth 1 -type f 2>/dev/null | head -1)
			say "  $t  $([ -f "$bin" ] && sha "$bin" | cut -c1-16)  $(stat -f '%Su:%Sg %Lp' "$t")"
		else say "  $t  (absent)"; fi
	done
	if [ -f "$ALLOW" ]; then say "  $ALLOW: $(grep -cvE '^[[:space:]]*(#|$)' "$ALLOW") rules"; else say "  $ALLOW  (absent)"; fi
	[ -f "$RECORD" ] && say "  installed by this script: $(tr '\n' ' ' < "$RECORD")" || say "  nothing recorded as installed by this script"
	say "Plugin log: log show --last 10m --style compact --predicate 'eventMessage CONTAINS \"NVMTL\"'"
}

default_allow() {
	cat <<'EOF'
# /Library/GPUBundles/nvmtl-allow.txt: the processes that get the GTX 1060 (GP106) as a Metal device (render offload,
# like prime-run on Linux: frames are copied to the Intel HD 630, which drives the internal panel). Every process not
# listed stays on the Intel GPU. Written by install-gpu.sh when no list exists; edit it as root (0644 root:wheel).
#
# One rule per line, the first match wins:
#   name    the process name (an app's CFBundleExecutable, not its display name) gets the GTX 1060
#   -name   never gets it
#   *       every process (not recommended: system UI processes would start on the dGPU)
# Needs no line: mtlprobe, nvmtltest, nvmtlrender, pg106_offload, and any process started with NVMTL_ENABLE=1.
# WindowServer is decided by the driver: it gets the GP106 only for the HDMI screen.
#
# System processes that stay on the Intel GPU even if a "*" line is added below them:
-loginwindow
-Dock
-WindowManager
-SystemUIServer
-ControlCenter
-NotificationCenter
-Finder
-Spotlight
-QuickLookUIService
-universalaccessd
-com.apple.WebKit.GPU
-photoanalysisd
-mediaanalysisd
-ScreenSaverEngine
#
# Apps, one per line (the executable name in Foo.app/Contents/MacOS/):
EOF
}

# The whole dispatch reads /dev/null (see R): bash has read this case statement before it runs, and resumes reading
# the script (or what an SSH caller sent after it) from its own stdin afterwards.
case "$MODE" in
	dry-run|install) preflight; install_all ;;
	revert) revert_all ;;
	status) status ;;
	*) die "MODE=dry-run|install|revert|status" ;;
esac </dev/null
