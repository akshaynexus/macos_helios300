#!/bin/bash
# Audio option 1 for the Helios 300 on macOS 26.7.2 (25G332): VoodooHDA 3.6.8 in /Library/Extensions, no AppleHDA,
# no root-volume change. See docs/HARDWARE.md "Audio on macOS 26".
#
# Run on the Mac. MODE selects what happens (default: dry-run, which changes nothing):
#   MODE=dry-run   checks + downloads/verifies the kext into a temp dir, prints what install would do
#   MODE=install   copies VoodooHDA.kext to /Library/Extensions (root:wheel 755) and loads it with kmutil
#   MODE=revert    removes /Library/Extensions/VoodooHDA.kext (+ prefPane) and rebuilds the auxiliary collection
#   MODE=status    what is installed/loaded now (after the reboot)
# NODES=1 (with dry-run/install) writes the ALC255 pin fallback into the kext's NodesToPatch and re-signs it ad hoc;
# use it only if the first boot with the stock kext has no headphone output or no microphone.
# Over SSH: { echo MODE=dry-run; cat tools/macos/audio-voodoohda.sh; } | ~/.config/helios300-ssh/hbmac
set -euo pipefail
MODE=${MODE:-dry-run}
NODES=${NODES:-0}

# Pinned: CloverHackyColor/VoodooHDA GitHub release "Release368" (2026-09-18), VoodooHDA 3.6.8, built with the
# macOS 26.5 SDK on 25G83, ad-hoc signed. It carries AppleALC's pin tables and is newer than the 3.0.3 in
# chris1111/VoodooHDA-Tahoe.
ZIP_URL="https://github.com/CloverHackyColor/VoodooHDA/releases/download/Release368/VoodooHDA.kext-368.zip"
ZIP_SHA=9aaa01b245f4827b2de5edfdc028700b9bc8823f03bfb1daf280b8571f88adb0
BIN_SHA=932a7c9e46a65fdab7c8729f599c1094b568ae4fc7f17dad9818c83e833a0973
PLIST_SHA=21b886b5449b9a0739422b6720ea2bc04fdcc477f79e1e3cece021033a51bf1a
DEST=/Library/Extensions/VoodooHDA.kext
PREFPANE_SYS=/Library/PreferencePanes/VoodooHDA.prefPane
PREFPANE_USER="$HOME/Library/PreferencePanes/VoodooHDA.prefPane"
BUILD_WANT=25G332

R() { if declare -F asroot >/dev/null; then asroot "$@"; else sudo "$@"; fi; }
say() { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
die() { printf 'STOP: %s\n' "$*" >&2; exit 1; }
sha() { shasum -a 256 "$1" | awk '{print $1}'; }
run() { if [ "$MODE" = install ] || [ "$MODE" = revert ]; then say "+ $*"; R "$@"; else say "would run: $*"; fi; }

csr_value() {
	# csr-active-config is little-endian; nvram prints it as %xx escapes or raw bytes.
	local raw; raw=$(nvram csr-active-config 2>/dev/null | sed 's/^csr-active-config[[:space:]]*//') || true
	[ -n "$raw" ] || { echo 0; return; }
	printf '%b' "$(printf '%s' "$raw" | sed 's/%\([0-9a-fA-F][0-9a-fA-F]\)/\\x\1/g')" | od -An -tu4 | tr -d ' \n'
}

hda_props() {
	# The PCH HD Audio device (8086:a171, PciRoot(0x0)/Pci(0x1f,0x3)): ACPI name HDAS, renamed HDEF by AppleALC.
	local n
	for n in HDEF HDAS; do ioreg -rlw0 -n "$n" -d1 2>/dev/null; done | grep -E '^\+-o |"(compatible|layout-id|alc-layout-id|voodoo-layout-id)"' || true
}

preflight() {
	step "macOS"
	local build; build=$(sw_vers -buildVersion)
	say "build $build ($(uname -m))"
	[ "$(uname -m)" = x86_64 ] || die "not an Intel Mac"
	[ "$build" = "$BUILD_WANT" ] || say "WARNING: checked against $BUILD_WANT only"

	step "SIP"
	local csr; csr=$(csr_value)
	say "csr-active-config = $(printf '0x%x' "$csr") ($(csrutil status 2>/dev/null | head -1))"
	[ $((csr & 0x1)) -ne 0 ] || die "CSR_ALLOW_UNTRUSTED_KEXTS (0x1) is not set: an ad-hoc signed kext cannot load from /Library/Extensions"
	if [ $((csr & 0x200)) -ne 0 ]; then say "CSR_ALLOW_UNAPPROVED_KEXTS (0x200) set: no approval prompt expected"
	else say "0x200 not set: approve VoodooHDA under System Settings > Privacy & Security after the load"; fi

	step "Conflicts"
	if [ -e /System/Library/Extensions/AppleHDA.kext ]; then
		die "AppleHDA.kext is on the root volume (option 2 is installed): revert it first (audio-applehda.sh MODE=revert); AppleHDA and VoodooHDA would both claim HDAS"
	fi
	say "AppleHDA.kext: absent (expected on macOS 26)"
	for k in "$PREFPANE_SYS" "$PREFPANE_USER" "$DEST"; do
		if [ -e "$k" ]; then say "already present: $k ($(defaults read "$k/Contents/Info" CFBundleShortVersionString 2>/dev/null || echo '?'))"; fi
	done
	ls -d /Library/Extensions/*HDA*.kext /Library/Extensions/*ALC*.kext 2>/dev/null | grep -v "^$DEST\$" | sed 's/^/other audio kext in \/Library\/Extensions: /' || true
	say "loaded:"; kmutil showloaded 2>/dev/null | grep -E 'Lilu|AppleALC|NMHDA|IOAudioFamily|VoodooHDA|WhateverGreen' | sed 's/^/  /' || true

	step "Device"
	hda_props | sed 's/^/  /'
	if ! hda_props | grep '"compatible"' | grep 'pci8086,a171' | grep -q '"HDAS"'; then
		say "WARNING: 'HDAS' is not in the PCH audio device's compatible list; VoodooHDA 3.6.8 matches IONameMatch=HDAS"
	fi
	say "layout-id: AppleALC reports 7 to the system; VoodooHDA's built-in AppleALC table has no ALC255 layout 7,"
	say "so it uses the codec's BIOS pin defaults. Do not disable AppleALC for this option: layout-id would then be"
	say "the injected 29, whose pins VoodooHDA cannot use (headphone 0x21 and jack 0x19 share association 2"
	say "with opposite directions; VoodooHDA disables such an association)."
}

stage() {
	step "Fetch and verify VoodooHDA 3.6.8 (Release368)"
	WORK=$(mktemp -d "${TMPDIR:-/tmp}/vhda.XXXXXX")
	curl -fsSL --retry 2 -o "$WORK/v.zip" "$ZIP_URL" || die "download failed: $ZIP_URL"
	[ "$(sha "$WORK/v.zip")" = "$ZIP_SHA" ] || die "zip sha256 $(sha "$WORK/v.zip") != pinned $ZIP_SHA"
	say "zip sha256 OK ($ZIP_SHA)"
	(cd "$WORK" && ditto -x -k v.zip x)
	K="$WORK/x/VoodooHDA.kext"
	[ -d "$K" ] || die "no VoodooHDA.kext in the zip"
	xattr -cr "$K"   # the zip's __MACOSX metadata, restored as xattrs by ditto
	[ "$(sha "$K/Contents/MacOS/VoodooHDA")" = "$BIN_SHA" ] || die "binary sha256 differs from the pinned build"
	[ "$(sha "$K/Contents/Info.plist")" = "$PLIST_SHA" ] || die "Info.plist sha256 differs from the pinned build"
	codesign --verify --strict "$K" || die "codesign --verify failed on the release kext"
	say "binary + Info.plist sha256 OK; signature valid (ad hoc): $(codesign -dv "$K" 2>&1 | grep -E '^Identifier|^Signature' | tr '\n' ' ')"
	say "version $(defaults read "$K/Contents/Info" CFBundleShortVersionString); OSBundleLibraries:"
	/usr/libexec/PlistBuddy -c 'Print :OSBundleLibraries' "$K/Contents/Info.plist" | sed 's/^/  /'
	say "(IOAudioFamily 740.1 and IOGraphicsFamily 600 are in SystemKernelExtensions.kc only: this kext cannot be"
	say " OpenCore-injected; /Library/Extensions -> auxiliary collection is the route)"
	if [ "$NODES" = 1 ]; then nodes_patch "$K"; fi
	step "kmutil print-diagnostics on a root:wheel copy in a temp dir"
	local d; d=$(mktemp -d "${TMPDIR:-/tmp}/vhda-diag.XXXXXX")
	R ditto "$K" "$d/VoodooHDA.kext"; R chown -R root:wheel "$d/VoodooHDA.kext"; R chmod -R 755 "$d/VoodooHDA.kext"
	R kmutil print-diagnostics -p "$d/VoodooHDA.kext" 2>&1 | sed 's/^/  /' | head -25 || true
	R rm -rf "$d"
}

# ALC255 pins, from AppleALC layout 29 (dhinakg, this laptop) re-grouped the way VoodooHDA (FreeBSD hdaa) expects:
#   0x14 (20) internal speaker  0x90170110  assoc 1 seq 0
#   0x21 (33) headphone jack    0x0421101F  assoc 1 seq 15 -> speaker muted while headphones are plugged in
#   0x12 (18) internal mic      0x90A60120  assoc 2 seq 0  (Platforms29 records the internal mic on node 18)
#   0x19 (25) mic jack          0x04A1102F  assoc 2 seq 15 -> internal mic switched off while a mic is plugged in
nodes_patch() {
	local k=$1 pl="$1/Contents/Info.plist" base=":IOKitPersonalities:VoodooHDA:NodesToPatch"
	step "NodesToPatch fallback (ALC255, codec address 0)"
	/usr/libexec/PlistBuddy -c "Delete $base" "$pl" >/dev/null 2>&1 || true
	/usr/libexec/PlistBuddy -c "Add $base array" "$pl"
	local i=0 n c
	for nc in 20:0x90170110:speaker 33:0x0421101F:headphone 18:0x90A60120:internal-mic 25:0x04A1102F:mic-jack; do
		n=${nc%%:*}; c=${nc#*:}; c=${c%%:*}
		/usr/libexec/PlistBuddy -c "Add $base:$i dict" -c "Add $base:$i:Codec integer 0" -c "Add $base:$i:Node integer $n" \
			-c "Add $base:$i:Config integer $((c))" -c "Add $base:$i:Comment string ${nc##*:}" "$pl"
		i=$((i + 1))
	done
	plutil -lint "$pl" >/dev/null || die "patched Info.plist does not lint"
	codesign -f -s - "$k" 2>&1 | sed 's/^/  /'
	codesign --verify --strict "$k" || die "re-signing failed"
	/usr/libexec/PlistBuddy -c "Print $base" "$pl" | sed 's/^/  /'
}

install_kext() {
	step "Install into /Library/Extensions"
	if [ -e "$DEST" ]; then run rm -rf "$DEST"; fi
	run ditto "$K" "$DEST"
	run chown -R root:wheel "$DEST"
	run chmod -R 755 "$DEST"
	if [ "$MODE" = install ]; then
		[ "$(sha "$DEST/Contents/MacOS/VoodooHDA")" = "$BIN_SHA" ] || die "installed binary differs"
		[ "$(stat -f %Su:%Sg "$DEST")" = root:wheel ] || die "ownership not root:wheel"
	fi
	step "Load (adds it to the auxiliary kernel collection; same procedure as PG106FB)"
	run kmutil load -p "$DEST"
	say "If kmutil says approval is needed: System Settings > Privacy & Security > Allow, then restart."
	say "Then restart either way, and run MODE=status."
}

revert() {
	step "Revert option 1"
	for k in "$DEST" "$PREFPANE_SYS" "$PREFPANE_USER"; do
		if [ -e "$k" ]; then run rm -rf "$k"; fi
	done
	# Rebuild the auxiliary collection from what is left in /Library/Extensions (PG106FB, PG106Accel), the
	# command OpenCore Legacy Patcher uses for an aux-only rebuild.
	run kmutil create --allow-missing-kdk --new aux \
		--boot-path /System/Library/KernelCollections/BootKernelExtensions.kc \
		--system-path /System/Library/KernelCollections/SystemKernelExtensions.kc || say "aux rebuild failed; the restart rebuilds it (kernelmanagerd)"
	say "Restart. If the machine does not reach the desktop with VoodooHDA: boot safe mode (OpenCore picker,"
	say "Shift+Enter; safe mode does not load /Library/Extensions kexts) and run MODE=revert there."
}

status() {
	step "Status"
	say "installed: $( [ -e "$DEST" ] && defaults read "$DEST/Contents/Info" CFBundleShortVersionString || echo no)"
	kmutil showloaded 2>/dev/null | grep -E 'VoodooHDA|IOAudioFamily|AppleALC|NMHDA|PG106' | sed 's/^/  /' || true
	ioreg -rw0 -c VoodooHDADevice -d1 2>/dev/null | head -5 || true
	say "audio engines:"; ioreg -rw0 -c IOAudioEngine -l 2>/dev/null | grep -E '"IOAudioEngineDescription"|"IOAudioEngineState"' | sed 's/^/  /' | head -10 || true
	say "Core Audio devices:"; system_profiler SPAudioDataType 2>/dev/null | sed -n '1,40p'
	say "VoodooHDA log:"; log show --last boot --predicate 'senderImagePath CONTAINS "VoodooHDA" OR eventMessage CONTAINS "VoodooHDA"' 2>/dev/null | tail -25 || true
}

case "$MODE" in
	dry-run) preflight; stage; install_kext; say; say "dry run: nothing was changed (temp dir $WORK)";;
	install) preflight; stage; install_kext;;
	revert) revert;;
	status) status;;
	*) die "MODE must be dry-run, install, revert or status";;
esac
