#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Audio option 1 (fallback) for the Helios 300 on macOS 26.7.2 (25G332): VoodooHDA 3.6.8 in /Library/Extensions, no
# AppleHDA, no root-volume change, no HDMI audio. Option 2 (audio-applehda.sh) is the recommended route: it sounds
# better because AppleALC layout 29 brings Apple's speaker EQ, limiter and crossover, which VoodooHDA has no
# equivalent of. See docs/HARDWARE.md "Audio on macOS 26".
#
# Run on the Mac. MODE selects what happens (default: dry-run, which changes nothing):
#   MODE=dry-run   checks + downloads/verifies the kext into a temp dir, applies TUNE/NODES there, prints the plan
#   MODE=install   copies VoodooHDA.kext to /Library/Extensions (root:wheel 755) and rebuilds the auxiliary collection
#                  with it (falls back to kmutil load -p); also the way to change TUNE on an installed copy. Restart.
#   MODE=revert    removes /Library/Extensions/VoodooHDA.kext (+ prefPane) and rebuilds the auxiliary collection
#   MODE=status    what is installed/loaded now, with the installed copy's tuning values
# TUNE=1 (default) writes the sound profile below into the kext's Info.plist and re-signs it ad hoc; TUNE=0 installs
# the release's Info.plist unchanged (sha256-pinned). Values: VHDA_PCM (default 90), VHDA_REC (75), VHDA_INHIBIT (true).
# NODES=1 also writes the ALC255 pin fallback into NodesToPatch; use it only if the headphones or a microphone are
# missing with the BIOS pins.
# Over SSH: { echo MODE=dry-run; cat tools/macos/audio-voodoohda.sh; } | ~/.config/helios300-ssh/hbmac
set -euo pipefail
MODE=${MODE:-dry-run}
NODES=${NODES:-0}
TUNE=${TUNE:-1}
VHDA_PCM=${VHDA_PCM:-90}
VHDA_REC=${VHDA_REC:-75}
VHDA_INHIBIT=${VHDA_INHIBIT:-true}

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
EXT=/Library/Extensions
AUX=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
KB=/System/Library/KernelCollections/BootKernelExtensions.kc
KS=/System/Library/KernelCollections/SystemKernelExtensions.kc
STATE=/Library/Helios300/audio-voodoohda

# stdin from /dev/null: over hbmac the script itself is on stdin, and asroot hands it to the command
R() { if declare -F asroot >/dev/null; then asroot "$@" </dev/null; else sudo "$@" </dev/null; fi; }
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
	local o; for o in "$EXT"/*HDA*.kext "$EXT"/*ALC*.kext; do
		[ -e "$o" ] && [ "$o" != "$DEST" ] && say "other audio kext in $EXT: $o"; done
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
	curl -fsSL --retry 2 -o "$WORK/v.zip" "$ZIP_URL" </dev/null || die "download failed: $ZIP_URL"
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
	if [ "$TUNE" = 1 ]; then tune_patch "$K"; fi
	if [ "$NODES" = 1 ]; then nodes_patch "$K"; fi
	if [ "$TUNE" = 1 ] || [ "$NODES" = 1 ]; then
		plutil -lint "$K/Contents/Info.plist" >/dev/null || die "patched Info.plist does not lint"
		codesign -f -s - "$K" 2>&1 | sed 's/^/  /'
		codesign --verify --strict "$K" || die "re-signing failed"
		say "Info.plist changed, kext re-signed ad hoc (binary unchanged: $BIN_SHA)"
	fi
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
	/usr/libexec/PlistBuddy -c "Print $base" "$pl" | sed 's/^/  /'
}

# Why the stock 3.6.8 sounds bad on this laptop, and what TUNE changes (docs/HARDWARE.md "Option 1 sound"):
# - No speaker processing. AppleHDA with layout 29 runs Apple's DSP chain for the internal speakers (EQ filters,
#   limiter, crossover: layout29.xml SignalProcessing/SoftwareDSP); VoodooHDA sends the raw signal, so the small
#   speakers sound thin and harsh, and full-scale content drives them into distortion. Not fixable here: option 2.
# - Gain: the ALC255's only output gain stage is the DAC amp (NID 0x02, 0.75 dB steps, 0 dB max); VoodooHDA sets it
#   to Main x PCM (VoodooHDADevice.cpp audioCtlOssMixerSet), PCM 95 = near 0 dB at full volume, no headroom.
#   PCM 90 keeps ~4 dB more headroom at the top of the slider (VHDA_PCM).
# - DMA: stream buffers are mapped cached unless InhibitCache (allocateDmaMemory); the release turns it on only for
#   its HDMI personality. Uncached stream buffers are the usual cure for crackle/static (VHDA_INHIBIT).
# - Recording: Rec 95 is the release's capture level; 75 (the driver's own table default, Tables.c) lowers the
#   DMIC's hiss and clipping (VHDA_REC).
# Unchanged on purpose: Boost 1 (a x1 multiply, no effect), Noise 0 (no noise gate), Vectorize, iMix 95 (the
# playback mixer path, must not be 0), iGain 0 (analog loopback muted), the volume-curve "fixes" (they only remap
# the slider). Mixer values are read once at load: a change needs MODE=install and a restart.
tune_patch() {
	local pl="$1/Contents/Info.plist" base=":IOKitPersonalities:VoodooHDA"
	step "Sound profile (TUNE=1): PCM $VHDA_PCM, Rec $VHDA_REC, InhibitCache $VHDA_INHIBIT"
	case "$VHDA_PCM$VHDA_REC" in *[!0-9]*) die "VHDA_PCM/VHDA_REC must be numbers 0-100";; esac
	[ "$VHDA_PCM" -ge 50 ] && [ "$VHDA_PCM" -le 100 ] || die "VHDA_PCM $VHDA_PCM: use 50-100"
	[ "$VHDA_REC" -le 100 ] || die "VHDA_REC $VHDA_REC: use 0-100"
	case "$VHDA_INHIBIT" in true|false) ;; *) die "VHDA_INHIBIT must be true or false";; esac
	/usr/libexec/PlistBuddy -c "Set $base:MixerValues:PCM $VHDA_PCM" -c "Set $base:MixerValues:Rec $VHDA_REC" \
		-c "Set $base:InhibitCache $VHDA_INHIBIT" "$pl"
	/usr/libexec/PlistBuddy -c "Print $base:MixerValues" -c "Print $base:InhibitCache" "$pl" | sed 's/^/  /'
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
	step "Auxiliary kernel collection with it (kmutil create, as audio-applehda.sh / docs/MULTI-GPU.md)"
	local rc=0; aux_rebuild || rc=$?
	if [ "$rc" != 0 ]; then
		step "Fallback: kmutil load -p (kernelmanagerd adds it to the auxiliary collection; the first install's route)"
		run kmutil load -p "$DEST"
		say "If kmutil says approval is needed: System Settings > Privacy & Security > Allow, then restart."
	fi
	say "Restart, and run MODE=status."
}

# The auxiliary collection from every kext in /Library/Extensions against the running boot/system collections:
# kmutil create -n aux ... -p <each> -A <file> -z, kmutil inspect, then publish (previous copy kept in $STATE).
aux_rebuild() {
	local k ids="" a=(create -n aux --volume-root / --allow-missing-kdk -a x86_64 -B "$KB")
	[ -s "$KS" ] && a+=(-S "$KS")
	[ -f /System/Library/Kernels/kernel ] && a+=(--kernel /System/Library/Kernels/kernel)
	a+=(--repository "$EXT")
	for k in "$EXT"/*.kext; do
		[ -d "$k" ] || continue
		if [ "$k" = "$DEST" ] && [ "$MODE" = revert ]; then continue; fi
		a+=(-p "$k"); ids="$ids $(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$k/Contents/Info.plist" 2>/dev/null)"
	done
	if [ "$MODE" = install ] && [ ! -d "$DEST" ]; then a+=(-p "$DEST"); ids="$ids org.voodoo.driver.VoodooHDA"; fi
	if [ -z "$ids" ]; then run rm -f "$AUX"; say "no kexts left in $EXT: auxiliary collection removed"; return 0; fi
	local W2; W2=$(mktemp -d "${TMPDIR:-/tmp}/vhda-aux.XXXXXX")
	a+=(-A "$W2/aux.kc" -z)
	if [ "$MODE" != install ] && [ "$MODE" != revert ]; then say "would run: kmutil ${a[*]}; kmutil inspect; publish to $AUX"; return 0; fi
	say "+ kmutil ${a[*]}"
	if ! R kmutil "${a[@]}" > "$W2/log" 2>&1; then sed 's/^/    /' "$W2/log" | tail -15; R rm -rf "$W2"; return 1; fi
	R kmutil inspect -a x86_64 -A "$W2/aux.kc" > "$W2/inspect" 2>&1 || { tail -5 "$W2/inspect"; R rm -rf "$W2"; return 1; }
	for k in $ids; do grep -q "$k" "$W2/inspect" || { say "  $k missing from the built collection"; R rm -rf "$W2"; return 1; }; done
	R mkdir -p "$STATE"
	if [ -f "$AUX" ]; then R cp -p "$AUX" "$STATE/aux-previous.kc"; fi
	R cp "$W2/aux.kc" "$AUX"; R chown root:wheel "$AUX"; R chmod 644 "$AUX"
	say "  published $AUX with$ids (previous: $STATE/aux-previous.kc)"
	R rm -rf "$W2"
}

revert() {
	step "Revert option 1"
	for k in "$DEST" "$PREFPANE_SYS" "$PREFPANE_USER"; do
		if [ -e "$k" ]; then run rm -rf "$k"; fi
	done
	# Rebuild the auxiliary collection from what is left in /Library/Extensions (PG106FB, PG106Accel).
	step "Auxiliary kernel collection without it"
	aux_rebuild || say "aux rebuild failed (log above): PG106FB/PG106Accel may still be built with VoodooHDA; after the restart, sudo kmutil load -p them"
	say "Restart. If the machine does not reach the desktop with VoodooHDA: boot safe mode (OpenCore picker,"
	say "Shift+Enter; safe mode does not load /Library/Extensions kexts) and run MODE=revert there."
}

status() {
	step "Status"
	say "installed: $( [ -e "$DEST" ] && defaults read "$DEST/Contents/Info" CFBundleShortVersionString || echo no)"
	if [ -e "$DEST" ]; then
		say "tuning: $(/usr/libexec/PlistBuddy -c 'Print :IOKitPersonalities:VoodooHDA:MixerValues' "$DEST/Contents/Info.plist" 2>/dev/null | tr -s ' \n' ' ')" \
			"InhibitCache $(/usr/libexec/PlistBuddy -c 'Print :IOKitPersonalities:VoodooHDA:InhibitCache' "$DEST/Contents/Info.plist" 2>/dev/null)" \
			"($( [ "$(sha "$DEST/Contents/Info.plist")" = "$PLIST_SHA" ] && echo 'release Info.plist' || echo 'changed by TUNE/NODES'))"
	fi
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
