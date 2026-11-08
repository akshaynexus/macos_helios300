#!/bin/bash
# Audio option 2 for the Helios 300 on macOS 26.7.2 (25G332): put AppleHDA.kext back on the root volume, step for step
# what OpenCore Legacy Patcher 3.0.0's sys_patch does for its "Miscellaneous: Modern Audio" set, and nothing else.
# OCLP itself refuses to patch this machine: it also lists "Graphics: Nvidia Web Drivers" for the GTX 1060 and
# requires nvda_drv; the Web Drivers must never be installed (PG106 owns the GPU). See docs/HARDWARE.md.
#
# Run on the Mac. MODE (default dry-run, which changes nothing persistent):
#   MODE=dry-run   all checks; mounts OCLP's payload read-only, hashes AppleHDA.kext; prints every command install runs
#   MODE=install   (KDK install if missing), mount root rw, KDK merge, AppleHDA.kext, patch record, kmutil, snapshot
#   MODE=revert    boot the last sealed (Apple) snapshot again: AppleHDA, the KDK merge and the record are gone
#   MODE=status    what is booted and loaded now
# RSRREPAIR=0 skips OCLP's RSRRepair install (default 1, as OCLP does after a full KC rebuild).
# Over SSH: { echo MODE=dry-run; cat tools/macos/audio-applehda.sh; } | ~/.config/helios300-ssh/hbmac
set -euo pipefail
MODE=${MODE:-dry-run}
RSRREPAIR=${RSRREPAIR:-1}

APP=/Applications/OpenCore-Patcher.app
# OCLP 3.0.0 bundles PatcherSupportPkg 2.0.5 (constants.py). Universal-Binaries.dmg is that release's asset, sha256
# from github.com/dortania/PatcherSupportPkg/releases/tag/2.0.5; the image is encrypted with OCLP's public passphrase
# "password" (sys_patch/utilities/dmg_mount.py). modern_audio.py: AppleHDA.kext from "26.0 Beta 1".
UB_SHA=1c290781bc726759ff37a110ffcb7182a6c6fd3d8ebce112d134d482c8de52a3
UB_URL=https://github.com/dortania/PatcherSupportPkg/releases/download/2.0.5/Universal-Binaries.dmg
PSP_VER=2.0.5
SRC_REL="26.0 Beta 1/System/Library/Extensions/AppleHDA.kext"
# The KDK OCLP picks for 25G332 (exact build match in https://dortania.github.io/KdkSupportPkg/manifest.json).
KDK_BUILD=25G332
KDK_URL=https://github.com/dortania/KdkSupportPkg/releases/download/25G332/Kernel_Debug_Kit_26.7.2_build_25G332.dmg
KDK_SHA=9490bd9b5e16ff5cee8b7330c498d64f27a008aa91c5c70d686a00dc5978c555
KDK_DIR=/Library/Developer/KDKs
MNT=/System/Volumes/Update/mnt1
RECORD=System/Library/CoreServices/OpenCore-Legacy-Patcher.plist

R() { if declare -F asroot >/dev/null; then asroot "$@"; else sudo "$@"; fi; }
say() { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
die() { printf 'STOP: %s\n' "$*" >&2; exit 1; }
sha() { shasum -a 256 "$1" | awk '{print $1}'; }
live() { [ "$MODE" = install ] || [ "$MODE" = revert ]; }
run() { if live; then say "+ $*"; R "$@"; else say "would run: $*"; fi; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/applehda.XXXXXX")
UB_MNT="$WORK/Universal-Binaries"; PL_MNT="$WORK/payloads"
cleanup() {
	hdiutil detach "$UB_MNT" >/dev/null 2>&1 || true
	hdiutil detach "$PL_MNT" >/dev/null 2>&1 || true
}
trap cleanup EXIT

csr_value() {
	local raw; raw=$(nvram csr-active-config 2>/dev/null | sed 's/^csr-active-config[[:space:]]*//') || true
	[ -n "$raw" ] || { echo 0; return; }
	printf '%b' "$(printf '%s' "$raw" | sed 's/%\([0-9a-fA-F][0-9a-fA-F]\)/\\x\1/g')" | od -An -tu4 | tr -d ' \n'
}
root_dev() {   # OCLP mount.py: diskutil info / -> DeviceIdentifier, minus the snapshot suffix
	local info id; info=$(diskutil info -plist /)
	id=$(printf '%s' "$info" | plutil -extract DeviceIdentifier raw -)
	if [ "$(printf '%s' "$info" | plutil -extract APFSSnapshot raw - 2>/dev/null)" = true ]; then id=${id%s*}; fi
	echo "$id"
}
tree_sha() { (cd "$1" && find . -type f | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f"; done) | shasum -a 256 | awk '{print $1}'; }

preflight() {
	step "macOS"
	BUILD=$(sw_vers -buildVersion); XNU=$(uname -r)
	say "build $BUILD, Darwin $XNU, $(uname -m)"
	[ "$(uname -m)" = x86_64 ] || die "not an Intel Mac"
	[ "$BUILD" = "$KDK_BUILD" ] || die "this script is pinned to $KDK_BUILD (KDK and checks); running $BUILD"

	step "OCLP's validation for Modern Audio (sys_patch/patchsets/detect.py)"
	local csr; csr=$(csr_value); say "csr-active-config $(printf '0x%x' "$csr"); $(csrutil status 2>/dev/null | head -1)"
	for b in 0x1:CSR_ALLOW_UNTRUSTED_KEXTS 0x2:CSR_ALLOW_UNRESTRICTED_FS 0x800:CSR_ALLOW_UNAUTHENTICATED_ROOT; do
		[ $((csr & ${b%%:*})) -ne 0 ] || die "SIP: ${b#*:} (${b%%:*}) is not set (OCLP root_patch_sip_ventura)"
	done
	say "SIP bits 0x1 0x2 0x800: set"
	fdesetup status | grep -q "FileVault is Off" || die "FileVault is on (OCLP refuses root patching with FileVault)"
	say "FileVault: off"
	local pol; pol=$(nvram 94B73556-2197-4702-82A8-3E1337DAFBFB:AppleSecureBootPolicy 2>/dev/null | awk '{print $2}') || true
	say "AppleSecureBootPolicy: ${pol:-absent} (must be absent or %00: SecureBootModel Disabled)"
	case "${pol:-}" in ""|"%00"|"%00%00%00%00") ;; *) die "Apple Secure Boot is active";; esac
	local loaded; loaded=$(kmutil showloaded 2>/dev/null || true)
	if printf '%s' "$loaded" | grep -q com.dhinakg.AMFIPass; then say "AMFI: AMFIPass loaded ($(printf '%s' "$loaded" | grep com.dhinakg.AMFIPass | grep -o '([^)]*)' | head -1)); OCLP requires >= 1.2.1"
	elif nvram boot-args 2>/dev/null | grep -q amfi_get_out_of_my_way; then say "AMFI: amfi_get_out_of_my_way"
	else die "AMFI is enforcing: AppleHDA's HAL plug-in (OCLP-signed) would not load in coreaudiod; OCLP requires AMFIPass or AMFI off"; fi
	if [ "$(diskutil info -plist / | plutil -extract APFSSnapshot raw - 2>/dev/null)" != true ]; then die "/ is not an APFS snapshot"; fi
	ROOTDEV=$(root_dev); say "root volume: /dev/$ROOTDEV ($(diskutil apfs list 2>/dev/null | grep -m1 'Snapshot Sealed' | sed 's/^ *//'))"
	local free; free=$(df -g / | awk 'NR==2{print $4}'); say "free space: ${free} GiB"; [ "$free" -ge 12 ] || die "less than 12 GiB free (KDK ~4 GiB + merge)"

	step "Conflicts"
	[ ! -e /Library/Extensions/VoodooHDA.kext ] || die "VoodooHDA is installed (option 1): revert it first (audio-voodoohda.sh MODE=revert)"
	if [ -e "/$RECORD" ]; then say "OCLP patch record already on /:"; plutil -p "/$RECORD" | sed 's/^/  /'; fi
	if [ -e /System/Library/Extensions/AppleHDA.kext ]; then say "NOTE: AppleHDA.kext already on the booted snapshot"; fi
	say "OCLP would also relocate /Library/Extensions kexts older than 2021-10-01 (none of ours); this script does not:"
	find /Library/Extensions -maxdepth 1 -name '*.kext' ! -newermt 2021-10-01 2>/dev/null | sed 's/^/  old: /' || true
	say "loaded: $(printf '%s' "$loaded" | grep -oE 'as\.vit9696\.(Lilu|AppleALC|WhateverGreen)|com\.nullmoth\.NMHDA|org\.helios300\.[a-z0-9]+' | sort -u | tr '\n' ' ')"
	if [ -f /Library/LaunchAgents/com.dortania.opencore-legacy-patcher.auto-patch.plist ]; then
		say "NOTE: OCLP's auto-patch agent is installed; it may offer its own (refused) patch at login. Leave it alone."
	fi
}

payload() {
	step "OpenCore-Patcher.app and its PatcherSupportPkg payload"
	[ -d "$APP" ] || die "$APP not found"
	say "app: $(defaults read "$APP/Contents/Info" CFBundleShortVersionString 2>/dev/null) $(codesign -dv "$APP" 2>&1 | grep -E '^TeamIdentifier' || true)"
	codesign --verify --strict "$APP" || die "$APP fails codesign --verify (its sealed resources include the payload images)"
	say "app signature (sealed resources) OK"
	UB_DMG="$APP/Contents/Resources/Universal-Binaries.dmg"
	[ -f "$UB_DMG" ] || UB_DMG=$(find "$APP/Contents" -name Universal-Binaries.dmg -print -quit)
	[ -f "$UB_DMG" ] || die "Universal-Binaries.dmg not in the app"
	local got; got=$(sha "$UB_DMG")
	[ "$got" = "$UB_SHA" ] || die "Universal-Binaries.dmg sha256 $got != PatcherSupportPkg $PSP_VER asset $UB_SHA ($UB_URL)"
	say "Universal-Binaries.dmg = PatcherSupportPkg $PSP_VER release asset ($UB_SHA)"
	mkdir -p "$UB_MNT"
	printf 'password' | hdiutil attach -readonly -noverify -nobrowse -stdinpass -mountpoint "$UB_MNT" "$UB_DMG" >/dev/null || die "cannot mount Universal-Binaries.dmg"
	SRC="$UB_MNT/$SRC_REL"
	[ -d "$SRC" ] || die "no '$SRC_REL' in the payload"
	say "AppleHDA.kext $(defaults read "$SRC/Contents/Info" CFBundleVersion) from '$SRC_REL'"
	say "  files: $(find "$SRC" -type f | wc -l | tr -d ' '), tree sha256 (sorted 'shasum -a 256' lines): $(tree_sha "$SRC")"
	say "  binary sha256: $(sha "$SRC/Contents/MacOS/AppleHDA")"
	say "  signature: $(codesign -dv "$SRC" 2>&1 | grep -E '^(Authority|Signature|TeamIdentifier)' | head -2 | tr '\n' ' ')"
	CUSTOM_SIG=false; [ -e "$UB_MNT/.signed" ] && CUSTOM_SIG=true
	say "  payload .signed marker: $CUSTOM_SIG (OCLP's 'Custom Signature')"
	OCLP_VER=$(defaults read "$APP/Contents/Info" CFBundleShortVersionString 2>/dev/null || echo 3.0.0)
	COMMIT_URL=$(/usr/libexec/PlistBuddy -c 'Print :Github:Commit\ URL' "$APP/Contents/Info.plist" 2>/dev/null || echo "")
}

kdk() {
	step "Kernel Debug Kit $KDK_BUILD (OCLP kdk_handler: /Library/Developer/KDKs, exact build first)"
	KDK=$(find "$KDK_DIR" -maxdepth 1 -name "*_$KDK_BUILD.kdk" -type d 2>/dev/null | head -1 || true)
	if [ -n "$KDK" ] && [ "$(defaults read "$KDK/System/Library/CoreServices/SystemVersion" ProductBuildVersion 2>/dev/null)" = "$KDK_BUILD" ] \
		&& [ -f "$KDK/System/Library/Extensions/System.kext/PlugIns/Libkern.kext/Libkern" ]; then
		say "installed: $KDK"
		if pkgutil --pkg-info "com.apple.pkg.KDK.$KDK_BUILD" >/dev/null 2>&1; then
			local miss; miss=$(pkgutil --files "com.apple.pkg.KDK.$KDK_BUILD" | grep '^System/Library/Extensions' | while IFS= read -r f; do [ -e "$KDK/$f" ] || echo "$f"; done | head -3)
			[ -z "$miss" ] || die "KDK incomplete (macOS updates delete KDK files), missing e.g. $miss; remove it and rerun"
			say "KDK complete against its pkg receipt"
		fi
		return
	fi
	say "not installed: install downloads $KDK_URL (1181052767 bytes, sha256 $KDK_SHA) and runs its KernelDebugKit.pkg"
	live || { KDK="$KDK_DIR/KDK_26.7.2_$KDK_BUILD.kdk (after install)"; return; }
	local dmg="$WORK/KDK.dmg" km="$WORK/kdkmnt"
	curl -fL --retry 2 -o "$dmg" "$KDK_URL" || die "KDK download failed"
	[ "$(sha "$dmg")" = "$KDK_SHA" ] || die "KDK sha256 mismatch"
	mkdir -p "$km"; hdiutil attach -readonly -nobrowse -mountpoint "$km" "$dmg" >/dev/null || die "cannot mount KDK"
	R installer -pkg "$km/KernelDebugKit.pkg" -target / || { hdiutil detach "$km" >/dev/null; die "KDK install failed"; }
	hdiutil detach "$km" >/dev/null || true; rm -f "$dmg"
	kdk
}

mount_root() {
	step "Mount the root volume read-write at $MNT (OCLP mount.py)"
	if [ -f "$MNT/System/Library/CoreServices/SystemVersion.plist" ]; then say "already mounted"; return; fi
	run /sbin/mount -o nobrowse -t apfs "/dev/$ROOTDEV" "$MNT"
	if live; then
		[ "$(defaults read "$MNT/System/Library/CoreServices/SystemVersion" ProductBuildVersion)" = "$BUILD" ] \
			|| die "mounted root is not $BUILD: a macOS update is staged; finish or cancel it first"
		say "sanity: $MNT is $BUILD"
	fi
}

install_all() {
	mount_root
	step "Merge the KDK's /System/Library/Extensions (OCLP kdk_merge.py)"
	local merged=""
	if [ -f "/$RECORD" ] && [ "$(/usr/libexec/PlistBuddy -c 'Print :Kernel\ Debug\ Kit\ Used' "/$RECORD" 2>/dev/null)" = "$KDK" ] \
		&& [ -f "$MNT/System/Library/Extensions/System.kext/PlugIns/Libkern.kext/Libkern" ]; then merged=yes; say "same KDK already merged; skipped"; fi
	if [ -z "$merged" ]; then
		run /usr/bin/rsync -r -i -a "$KDK/System/Library/Extensions/" "$MNT/System/Library/Extensions"
		if live; then [ -f "$MNT/System/Library/Extensions/System.kext/PlugIns/Libkern.kext/Libkern" ] || die "KDK merge failed (no Libkern on $MNT)"; fi
	fi

	step "AppleHDA.kext -> $MNT/System/Library/Extensions (OCLP utilities/files.py install_new_file)"
	if [ -e "$MNT/System/Library/Extensions/AppleHDA.kext" ]; then run /bin/rm -R "$MNT/System/Library/Extensions/AppleHDA.kext"; fi
	run /bin/cp -R "$SRC" "$MNT/System/Library/Extensions"
	run /bin/chmod -Rf 755 "$MNT/System/Library/Extensions/AppleHDA.kext"
	run /usr/sbin/chown -Rf root:wheel "$MNT/System/Library/Extensions/AppleHDA.kext"
	if live; then [ "$(tree_sha "$MNT/System/Library/Extensions/AppleHDA.kext")" = "$(tree_sha "$SRC")" ] || die "copied AppleHDA.kext differs from the payload"; fi

	step "Patch record $MNT/$RECORD (OCLP sys_patch_helpers.generate_patchset_plist)"
	local rec="$WORK/OpenCore-Legacy-Patcher.plist" xnu_mm; xnu_mm=$(uname -r | cut -d. -f1-2)
	cat > "$rec" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>OpenCore Legacy Patcher</key><string>v$OCLP_VER</string>
	<key>PatcherSupportPkg</key><string>v$PSP_VER</string>
	<key>Time Patched</key><string>$(LC_ALL=C date '+%B %d, %Y @ %H:%M:%S')</string>
	<key>Commit URL</key><string>$COMMIT_URL</string>
	<key>Kernel Debug Kit Used</key><string>$KDK</string>
	<key>Metal Library Used</key><string>Not applicable</string>
	<key>OS Version</key><string>$xnu_mm ($BUILD)</string>
	<key>Custom Signature</key><$CUSTOM_SIG/>
	<key>Modern Audio</key>
	<dict>
		<key>Overwrite System Volume</key>
		<dict>
			<key>/System/Library/Extensions</key>
			<dict><key>AppleHDA.kext</key><string>26.0 Beta 1</string></dict>
		</dict>
	</dict>
</dict>
</plist>
EOF
	plutil -lint "$rec" >/dev/null || die "record does not lint"
	if [ -e "$MNT/$RECORD" ]; then run /bin/rm "$MNT/$RECORD"; fi
	run /bin/cp "$rec" "$MNT/System/Library/CoreServices/"
	live || plutil -p "$rec" | sed 's/^/  /'

	step "Rebuild the boot + system kernel collections (OCLP kernelcache/kernel_collection/boot_system.py)"
	run /usr/bin/kmutil create --allow-missing-kdk --volume-root "$MNT" --update-all --variant-suffix release

	if [ "$RSRREPAIR" = 1 ]; then
		step "RSRRepair (OCLP sys_patch_helpers.install_rsr_repair_binary, from the app's payloads.dmg)"
		local pdmg; pdmg=$(find "$APP/Contents" -name payloads.dmg -print -quit)
		if [ -n "$pdmg" ]; then
			mkdir -p "$PL_MNT"
			if printf 'password' | hdiutil attach -readonly -noverify -nobrowse -stdinpass -mountpoint "$PL_MNT" "$pdmg" >/dev/null; then
				if [ -x "$PL_MNT/Tools/RSRRepair" ]; then run "$PL_MNT/Tools/RSRRepair" --install || say "RSRRepair install failed (OCLP logs and continues)"
				else say "no Tools/RSRRepair in payloads.dmg; skipped"; fi
			else say "cannot mount payloads.dmg; RSRRepair skipped"; fi
		else say "payloads.dmg not found; RSRRepair skipped"; fi
	fi

	step "Create and bless a new snapshot (OCLP mount/snapshot.py, Intel form)"
	run /usr/sbin/bless --folder "$MNT/System/Library/CoreServices" --bootefi --create-snapshot
	run /sbin/umount "$MNT" || true
	say "Restart. Then MODE=status."
}

revert() {
	step "Revert: boot Apple's last sealed snapshot (OCLP sys_patch.py _unpatch_root_vol)"
	ROOTDEV=$(root_dev); BUILD=$(sw_vers -buildVersion)
	mount_root
	run /usr/sbin/bless --mount "$MNT" --bootefi --last-sealed-snapshot
	run /sbin/umount "$MNT" || true
	say "Restart. AppleHDA.kext, the merged KDK files and the patch record live only in the patched snapshot."
	say "From Recovery (if the patched system does not boot): Terminal ->"
	say "  bless --mount \"/Volumes/Macintosh HD\" --bootefi --last-sealed-snapshot   (name as in 'diskutil list')"
}

status() {
	step "Status"
	diskutil apfs list 2>/dev/null | grep -E 'Snapshot (Name|Sealed)' | head -4 | sed 's/^ */  /'
	if [ -f "/$RECORD" ]; then plutil -p "/$RECORD" | sed 's/^/  /'; else say "  no OCLP patch record on /"; fi
	say "AppleHDA on /: $( [ -e /System/Library/Extensions/AppleHDA.kext ] && defaults read /System/Library/Extensions/AppleHDA.kext/Contents/Info CFBundleVersion || echo absent)"
	kmutil showloaded 2>/dev/null | grep -E 'AppleHDA|IOHDAFamily|DspFuncLib|AppleALC|NMHDA|PG106' | sed 's/^/  /' || true
	sysctl -n debug.nmhda 2>/dev/null | sed 's/^/  nmhda: /' || true
	ioreg -rlw0 -n HDEF -d1 2>/dev/null | grep -E '"(alc-layout-id|layout-id)"' | sed 's/^/  /' || true
	system_profiler SPAudioDataType 2>/dev/null | sed -n '1,40p'
}

case "$MODE" in
	dry-run) preflight; payload; kdk; install_all; say; say "dry run: nothing persistent was changed";;
	install) preflight; payload; kdk; install_all;;
	revert) revert;;
	status) status;;
	*) die "MODE must be dry-run, install, revert or status";;
esac
