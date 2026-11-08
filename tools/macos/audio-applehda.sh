#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Audio option 2 (recommended) for the Helios 300 on macOS 26.7.2 (25G332): put AppleHDA.kext back on the root
# volume, step for step what OpenCore Legacy Patcher 3.0.0's sys_patch does for its "Miscellaneous: Modern Audio"
# set, and nothing else. AppleALC layout 29 then drives the ALC255 (with the layout's speaker EQ/limiter) and NMHDA
# gives HDMI audio on the GTX 1060's HDA function. OCLP itself refuses this machine: it also lists "Graphics: Nvidia
# Web Drivers" for the GTX 1060 and wants nvda_drv. The Web Drivers must never be installed (PG106 owns the GPU).
# See docs/HARDWARE.md "Audio on macOS 26".
#
# Run on the Mac as the logged-in user (it uses sudo, or asroot over hbmac). MODE (default dry-run):
#   MODE=dry-run   every check and every download (sha256-verified into $CACHE); the payload mounted read-only and
#                  hashed; the KDK package's signature checked; the auxiliary collection test-built in a temp dir;
#                  every command install would run is printed. Nothing on the root volume or in /Library changes.
#   MODE=install   option 1 (VoodooHDA) out first; KDK installed if missing; root volume mounted rw at
#                  /System/Volumes/Update/mnt1; KDK merge; AppleHDA.kext; OCLP's patch record; kmutil boot+system
#                  collections; RSRRepair; the auxiliary collection (PG106FB, PG106Accel) rebuilt against the NEW
#                  boot/system collections; new snapshot blessed. Restart.
#   MODE=aux       rebuild the auxiliary collection from /Library/Extensions against the RUNNING boot/system
#                  collections and publish it (a boot where PG106FB/PG106Accel did not load). Restart.
#   MODE=revert    bless Apple's last sealed snapshot and put back the auxiliary collection built for it. Restart.
#                  The same from Recovery: printed at the end of revert (revert_help).
#   MODE=status    what is booted and loaded: AppleHDA, layout 29, NMHDA and the HDMI pairing, PG106FB/PG106Accel.
# Options: RSRREPAIR=0 skips OCLP's RSRRepair (default 1, as OCLP after a full KC rebuild). DL=0: dry-run skips the
# downloads. CACHE=<dir> (default ~/Library/Caches/helios300-audio) keeps the verified downloads for install.
# REMOVE_KDK=1 (revert) deletes the KDK too. FORCE=1 skips the "already patched" / "not sealed" stops.
# Over SSH: { echo MODE=dry-run; cat tools/macos/audio-applehda.sh; } | ~/.config/helios300-ssh/hbmac
set -euo pipefail
MODE=${MODE:-dry-run}
RSRREPAIR=${RSRREPAIR:-1}
DL=${DL:-1}
FORCE=${FORCE:-0}
REMOVE_KDK=${REMOVE_KDK:-0}
CACHE=${CACHE:-$HOME/Library/Caches/helios300-audio}

APP=/Applications/OpenCore-Patcher.app
# OCLP 3.0.0 (rc.5 too) bundles PatcherSupportPkg 2.0.5 (constants.py patcher_support_pkg_version). Its
# Universal-Binaries.dmg is the release asset below (sha256 = GitHub's asset digest), encrypted with OCLP's public
# passphrase "password" (sys_patch/utilities/dmg_mount.py). modern_audio.py takes AppleHDA.kext from "26.0 Beta 1".
# The app's copy is used when its sha256 matches; otherwise the asset is downloaded.
PSP_VER=2.0.5
UB_URL=https://github.com/dortania/PatcherSupportPkg/releases/download/$PSP_VER/Universal-Binaries.dmg
UB_SHA=1c290781bc726759ff37a110ffcb7182a6c6fd3d8ebce112d134d482c8de52a3
UB_SIZE=853972480
SRC_REL="26.0 Beta 1/System/Library/Extensions/AppleHDA.kext"
# AppleHDA.kext 600.2 (DTSDKBuild 25A5279g) in that image: 115 files, Apple's own signatures on the 7 binaries (no
# bundle seal). Tree hash = sha256 of the sorted "shasum -a 256 ./path" lines, computed from the decrypted image
# (each binary matched its CodeDirectory page hashes). The image sha256 is the real pin; these catch a wrong folder.
HDA_VER=600.2
HDA_TREE=5ec3ce4a26b71d1384f9acb604f94c144c0f5f0c275a00965094221ce28ffc04
HDA_BIN=6bf19c385a1212160be8a01fd7903aaa0416407e0b52e949f49d04cee4c65de7
# The KDK OCLP picks for 25G332 (exact build in https://dortania.github.io/KdkSupportPkg/manifest.json).
KDK_BUILD=25G332
KDK_URL=https://github.com/dortania/KdkSupportPkg/releases/download/25G332/Kernel_Debug_Kit_26.7.2_build_25G332.dmg
KDK_SHA=9490bd9b5e16ff5cee8b7330c498d64f27a008aa91c5c70d686a00dc5978c555
KDK_SIZE=1181052767
KDK_DIR=/Library/Developer/KDKs
AMFIPASS_MIN=1.2.1   # OCLP constants.amfipass_compatibility_version
MNT=/System/Volumes/Update/mnt1
RECORD=System/Library/CoreServices/OpenCore-Legacy-Patcher.plist
EXT=/Library/Extensions
AUX=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
KCDIR=System/Library/KernelCollections
STATE=/Library/Helios300/audio-applehda
VOODOO=$EXT/VoodooHDA.kext

# stdin from /dev/null: over hbmac the script itself is on stdin, and asroot hands it to the command
R() { if declare -F asroot >/dev/null; then asroot "$@" </dev/null; else sudo "$@" </dev/null; fi; }
say() { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
die() { printf 'STOP: %s\n' "$*" >&2; exit 1; }
warn() { printf 'WARNING: %s\n' "$*"; }
sha() { shasum -a 256 "$1" | awk '{print $1}'; }
live() { [ "$MODE" = install ] || [ "$MODE" = revert ] || [ "$MODE" = aux ]; }
run() { if live; then say "+ $*"; R "$@"; else say "would run: $*"; fi; }
ver_ge() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | head -1)" = "$2" ]; }   # $1 >= $2
plist_get() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null || true; }
tree_sha() { (cd "$1" && find . -type f | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f"; done) | shasum -a 256 | awk '{print $1}'; }
LOADED=$(kmutil showloaded 2>/dev/null || true)
# here-strings and awk without an early exit: under pipefail a reader that quits early fails the writer (SIGPIPE)
is_loaded() { grep -q " $1 (" <<<"$LOADED"; }
loaded_ver() { awk -v id=" $1 (" 'v == "" && (i = index($0, id)) { v = substr($0, i + length(id)); sub(/\).*/, "", v) } END { print v }' <<<"$LOADED"; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/applehda.XXXXXX")
UB_MNT="$WORK/Universal-Binaries"; PL_MNT="$WORK/payloads"; KDK_MNT="$WORK/kdkmnt"
cleanup() {
	local m; for m in "$UB_MNT" "$PL_MNT" "$KDK_MNT"; do hdiutil detach "$m" >/dev/null 2>&1 || true; done
	R rm -rf "$WORK" 2>/dev/null || true
}
trap cleanup EXIT

csr_value() {
	local raw; raw=$(nvram csr-active-config 2>/dev/null | sed 's/^csr-active-config[[:space:]]*//') || true
	[ -n "$raw" ] || { echo 0; return; }
	printf '%b' "$(printf '%s' "$raw" | sed 's/%\([0-9a-fA-F][0-9a-fA-F]\)/\\x\1/g')" | od -An -tu4 | tr -d ' \n'
}
root_dev() {   # OCLP mount.py: diskutil info / -> DeviceIdentifier, minus the snapshot suffix (disk1s1s1 -> disk1s1)
	local info id; info=$(diskutil info -plist /)
	id=$(printf '%s' "$info" | plutil -extract DeviceIdentifier raw -)
	if [ "$(printf '%s' "$info" | plutil -extract APFSSnapshot raw - 2>/dev/null)" = true ]; then id=${id%s*}; fi
	echo "$id"
}
sealed() {   # "Yes", "Broken" (changed root, e.g. a patched snapshot), "No", or "" when diskutil does not say
	local s; s=$(diskutil info / 2>/dev/null | awk -F': *' '/^ *Sealed:/ && v == "" {v = $2} END {print v}')
	[ -n "$s" ] || s=$(diskutil apfs list 2>/dev/null | awk -F': *' '/Snapshot Sealed:/ && v == "" {v = $2} END {print v}')
	echo "$s"
}

# Downloads: verified into $CACHE (as the user), reused by install. Path on stdout, messages on stderr.
fetch() {   # url sha256 size name
	local url=$1 want=$2 size=$3 f="$CACHE/$4"
	mkdir -p "$CACHE"
	if [ -f "$f" ] && [ "$(sha "$f")" = "$want" ]; then say "  cached: $f (sha256 OK)" >&2; echo "$f"; return; fi
	say "  downloading $url ($size bytes) -> $f" >&2
	curl -fL --retry 3 -C - -o "$f.part" "$url" </dev/null >&2 || die "download failed: $url"
	[ "$(stat -f %z "$f.part")" = "$size" ] || die "$4: size $(stat -f %z "$f.part") != $size"
	[ "$(sha "$f.part")" = "$want" ] || { rm -f "$f.part"; die "$4: sha256 mismatch (file removed)"; }
	mv "$f.part" "$f"; say "  sha256 OK ($want)" >&2; echo "$f"
}

preflight() {
	step "macOS"
	BUILD=$(sw_vers -buildVersion)
	say "build $BUILD, Darwin $(uname -r), $(uname -m)"
	[ "$(uname -m)" = x86_64 ] || die "not an Intel Mac"
	[ "$BUILD" = "$KDK_BUILD" ] || die "this script is pinned to $KDK_BUILD (KDK and checks); running $BUILD. After a macOS update the KDK pin must be updated first (docs/HARDWARE.md)"

	step "OCLP's validation for Modern Audio (sys_patch/patchsets/detect.py)"
	local csr; csr=$(csr_value); say "csr-active-config $(printf '0x%x' "$csr"); $(csrutil status 2>/dev/null | head -1)"
	for b in 0x1:CSR_ALLOW_UNTRUSTED_KEXTS 0x2:CSR_ALLOW_UNRESTRICTED_FS 0x800:CSR_ALLOW_UNAUTHENTICATED_ROOT; do
		[ $((csr & ${b%%:*})) -ne 0 ] || die "SIP: ${b#*:} (${b%%:*}) is not set (OCLP root_patch_sip_ventura)"
	done
	say "SIP bits 0x1 0x2 0x800: set"
	fdesetup status 2>/dev/null | grep -q "FileVault is Off" || die "FileVault is on (or still decrypting): turn it off and wait until 'fdesetup status' says Off"
	say "FileVault: off"
	local pol; pol=$(nvram 94B73556-2197-4702-82A8-3E1337DAFBFB:AppleSecureBootPolicy 2>/dev/null | awk '{print $2}') || true
	say "AppleSecureBootPolicy: ${pol:-absent} (must be absent or %00: SecureBootModel Disabled)"
	case "${pol:-}" in ""|"%00"|"%00%00%00%00") ;; *) die "Apple Secure Boot is active (SecureBootModel must be Disabled)";; esac
	local amfi; amfi=$(loaded_ver com.dhinakg.AMFIPass)
	if [ -n "$amfi" ]; then
		ver_ge "$amfi" "$AMFIPASS_MIN" || die "AMFIPass $amfi loaded; OCLP needs >= $AMFIPASS_MIN"
		say "AMFI: AMFIPass $amfi loaded (>= $AMFIPASS_MIN)"
	elif nvram boot-args 2>/dev/null | grep -q amfi_get_out_of_my_way; then say "AMFI: amfi_get_out_of_my_way"
	else die "AMFI is enforcing: AppleHDA's HAL plug-in would not load in coreaudiod; OCLP requires AMFIPass (-amfipassbeta) or AMFI off"; fi
	[ "$(diskutil info -plist / | plutil -extract APFSSnapshot raw - 2>/dev/null)" = true ] || die "/ is not an APFS snapshot"
	ROOTDEV=$(root_dev); SEALED=$(sealed)
	say "root volume /dev/$ROOTDEV; booted snapshot sealed: ${SEALED:-?}"
	[ -d "$MNT" ] || die "$MNT does not exist"

	step "Already patched?"
	if [ -e "/$RECORD" ]; then
		say "OCLP patch record on /:"; plutil -p "/$RECORD" | sed 's/^/  /'
		[ "$FORCE" = 1 ] || die "this snapshot is already patched (MODE=status). To patch again, as OCLP: MODE=revert, restart, MODE=install"
	elif [ "${SEALED:-}" = Broken ] || [ "${SEALED:-}" = No ]; then
		[ "$FORCE" = 1 ] || die "the booted snapshot is not sealed ($SEALED) and has no OCLP record (something else changed the root volume); FORCE=1 to go on"
	elif [ -z "${SEALED:-}" ]; then warn "diskutil does not report the seal; no OCLP record, so taken as unpatched"
	else say "no: sealed, no record"; fi

	step "Audio stack AppleHDA needs (OpenCore-injected)"
	local k; for k in as.vit9696.Lilu as.vit9696.AppleALC com.nullmoth.NMHDA; do say "  $k: $(loaded_ver "$k")"; done
	is_loaded as.vit9696.Lilu && is_loaded as.vit9696.AppleALC || die "Lilu and AppleALC must be loaded (OpenCore Kernel > Add): without AppleALC AppleHDA has no ALC255 layout"
	is_loaded com.nullmoth.NMHDA || warn "NMHDA is not loaded: the ALC255 will work, HDMI audio will not"
	local lid; lid=$(ioreg -rlw0 -n HDEF -d1 2>/dev/null | awk -F'= ' '/"alc-layout-id"/ && v == "" {v = $2} END {print v}')
	say "  HDEF alc-layout-id: ${lid:-absent} (want <1d000000> = 29; layout-id reads 7 by design, AppleALC reports 7)"
	[ "${lid:-}" = "<1d000000>" ] || warn "alc-layout-id is not 29: check DeviceProperties PciRoot(0x0)/Pci(0x1f,0x3)"

	step "Free space"
	local free need=12; free=$(df -g / | awk 'NR==2{print $4}')
	[ -f "$CACHE/$(basename "$KDK_URL")" ] || [ -n "$(kdk_find)" ] || need=$((need + 2))
	say "free: ${free} GiB (need $need: KDK ~4 GiB installed + its download + the merge + the new snapshot)"
	[ "$free" -ge "$need" ] || die "less than $need GiB free"

	step "/Library/Extensions (they go into the auxiliary collection)"
	ls -1d "$EXT"/*.kext 2>/dev/null | sed 's/^/  /' || say "  (none)"
	local old; old=$(find "$EXT" -maxdepth 1 -name '*.kext' ! -newermt 2021-10-01 2>/dev/null || true)
	[ -z "$old" ] || say "NOTE: OCLP would move these pre-2021-10 kexts to /Library/Relocated Extensions; this script leaves them: $old"
	[ ! -f /Library/LaunchAgents/com.dortania.opencore-legacy-patcher.auto-patch.plist ] \
		|| say "NOTE: OCLP's auto-patch agent is installed; if it offers to patch at login, decline (it would want the Web Drivers)"
}

payload() {
	step "PatcherSupportPkg $PSP_VER Universal-Binaries.dmg"
	UB_DMG=""; OCLP_VER=3.0.0; COMMIT_URL=""; SRC=""; CUSTOM_SIG=false
	if [ -d "$APP" ]; then
		OCLP_VER=$(defaults read "$APP/Contents/Info" CFBundleShortVersionString 2>/dev/null || echo 3.0.0)
		COMMIT_URL=$(/usr/libexec/PlistBuddy -c 'Print :Github:Commit\ URL' "$APP/Contents/Info.plist" 2>/dev/null || echo "")
		say "OpenCore-Patcher.app $OCLP_VER (only its payload images are used, never its patcher)"
		local d; d=$(find "$APP/Contents" -name Universal-Binaries.dmg -print -quit 2>/dev/null || true)
		if [ -n "$d" ] && [ "$(sha "$d")" = "$UB_SHA" ]; then UB_DMG=$d; say "app copy = release asset ($UB_SHA)"
		elif [ -n "$d" ]; then say "app copy's sha256 differs from PatcherSupportPkg $PSP_VER: using the release asset"
		else say "no Universal-Binaries.dmg in the app: using the release asset"; fi
	else say "OpenCore-Patcher.app not installed: release asset; RSRRepair will be skipped"; fi
	if [ -z "$UB_DMG" ]; then
		if [ "$MODE" = dry-run ] && [ "$DL" = 0 ] && [ ! -f "$CACHE/Universal-Binaries.dmg" ]; then
			say "DL=0: not downloading $UB_URL; payload checks skipped"; return
		fi
		UB_DMG=$(fetch "$UB_URL" "$UB_SHA" "$UB_SIZE" Universal-Binaries.dmg)
	fi
	mkdir -p "$UB_MNT"
	hdiutil attach -readonly -noverify -nobrowse -mountpoint "$UB_MNT" -passphrase password "$UB_DMG" </dev/null >/dev/null \
		|| die "cannot mount Universal-Binaries.dmg"
	SRC="$UB_MNT/$SRC_REL"
	[ -d "$SRC" ] || die "no '$SRC_REL' in the payload"
	local v t; v=$(defaults read "$SRC/Contents/Info" CFBundleVersion); t=$(tree_sha "$SRC")
	say "AppleHDA.kext $v from '$SRC_REL': $(find "$SRC" -type f | wc -l | tr -d ' ') files, tree $t"
	[ "$v" = "$HDA_VER" ] || die "AppleHDA $v, expected $HDA_VER"
	[ "$(sha "$SRC/Contents/MacOS/AppleHDA")" = "$HDA_BIN" ] || die "AppleHDA binary sha256 differs from the pinned $HDA_BIN"
	if [ "$t" = "$HDA_TREE" ]; then say "  tree hash = pinned"
	else warn "tree hash differs from $HDA_TREE (computed on Linux from the same sha256-pinned image): report this, the copy itself is verified"; fi
	say "  binary signature: $(codesign -dv "$SRC/Contents/MacOS/AppleHDA" 2>&1 | grep -E '^Authority' | head -1)"
	[ -e "$UB_MNT/.signed" ] && CUSTOM_SIG=true
	say "  payload .signed marker: $CUSTOM_SIG (the record's 'Custom Signature')"
}

kdk_find() {   # OCLP kdk_handler: /Library/Developer/KDKs, exact build, Libkern present
	local k; k=$(find "$KDK_DIR" -maxdepth 1 -name "*_$KDK_BUILD.kdk" -type d 2>/dev/null | head -1 || true)
	if [ -n "$k" ] && [ "$(defaults read "$k/System/Library/CoreServices/SystemVersion" ProductBuildVersion 2>/dev/null)" = "$KDK_BUILD" ] \
		&& [ -f "$k/System/Library/Extensions/System.kext/PlugIns/Libkern.kext/Libkern" ]; then echo "$k"; fi
}

kdk() {
	step "Kernel Debug Kit $KDK_BUILD"
	KDK=$(kdk_find)
	if [ -n "$KDK" ]; then
		say "installed: $KDK"
		if pkgutil --pkg-info "com.apple.pkg.KDK.$KDK_BUILD" >/dev/null 2>&1; then
			local miss; miss=$(pkgutil --files "com.apple.pkg.KDK.$KDK_BUILD" | grep 'System/Library/Extensions/.*\.kext/Contents/MacOS/[^/]*$' \
				| while IFS= read -r f; do [ -e "/$f" ] || [ -e "$KDK/$f" ] || echo "$f"; done | head -3 || true)
			[ -z "$miss" ] && say "  complete against its package receipt" \
				|| warn "KDK binaries missing against its receipt (macOS updates delete KDK files), e.g. $miss: delete $KDK and rerun"
		fi
		return
	fi
	if [ "$MODE" = dry-run ] && [ "$DL" = 0 ] && [ ! -f "$CACHE/$(basename "$KDK_URL")" ]; then
		say "not installed; DL=0: not downloading $KDK_URL"; KDK="$KDK_DIR/KDK_26.7.2_$KDK_BUILD.kdk (after install)"; return
	fi
	say "not installed: $KDK_URL"
	local dmg; dmg=$(fetch "$KDK_URL" "$KDK_SHA" "$KDK_SIZE" "$(basename "$KDK_URL")")
	mkdir -p "$KDK_MNT"; hdiutil attach -readonly -nobrowse -mountpoint "$KDK_MNT" "$dmg" </dev/null >/dev/null || die "cannot mount the KDK image"
	[ -f "$KDK_MNT/KernelDebugKit.pkg" ] || die "no KernelDebugKit.pkg in the KDK image"
	local sig; sig=$(pkgutil --check-signature "$KDK_MNT/KernelDebugKit.pkg" 2>&1 || true)
	printf '%s\n' "$sig" | sed -n '1,4p' | sed 's/^/  /'
	grep -q 'Status: signed' <<<"$sig" && grep -q 'Apple' <<<"$sig" || die "KernelDebugKit.pkg is not signed by Apple"
	if live; then
		run /usr/sbin/installer -pkg "$KDK_MNT/KernelDebugKit.pkg" -target / || die "KDK install failed"
		hdiutil detach "$KDK_MNT" >/dev/null || true
		KDK=$(kdk_find); [ -n "$KDK" ] || die "the KDK install did not produce $KDK_DIR/*_$KDK_BUILD.kdk"
		say "installed: $KDK"
	else
		say "would run: installer -pkg KernelDebugKit.pkg -target /"
		hdiutil detach "$KDK_MNT" >/dev/null || true
		KDK="$KDK_DIR/KDK_26.7.2_$KDK_BUILD.kdk (after install)"
	fi
}

# ---- the auxiliary collection (PG106FB, PG106Accel and any other /Library/Extensions kext) ----
# An aux collection is linked against one pair of boot/system collections. kmutil --update-all on the mounted root
# makes new ones, so the aux built for the sealed snapshot does not fit them (OCLP's clean_auxiliary_kc says the same
# for its revert), and it lives on the Data volume, outside both snapshots. So one is built per snapshot, both are kept
# in $STATE, and the one matching what boots is published. Command form as the driver repo's docs/MULTI-GPU.md
# (verified on macOS 26): kmutil create -n aux ... -p <each kext> -A <file> -z, kmutil inspect, copy to $AUX.
aux_kexts() { find "$EXT" -maxdepth 1 -name '*.kext' ! -name VoodooHDA.kext 2>/dev/null | LC_ALL=C sort; }
aux_manifest() { local k; aux_kexts | while IFS= read -r k; do echo "$(tree_sha "$k") $k"; done; }
aux_build() {   # boot-kc system-kc kernel out -> 0 built, 2 nothing to build, 1 failed
	local boot=$1 sys=$2 kern=$3 out=$4 k ids=""
	local a=(create -n aux --volume-root / --allow-missing-kdk -a x86_64 -B "$boot")
	[ -s "$sys" ] && a+=(-S "$sys")
	[ -f "$kern" ] && a+=(--kernel "$kern")
	a+=(--repository "$EXT")
	while IFS= read -r k; do a+=(-p "$k"); ids="$ids $(plist_get "$k/Contents/Info.plist" CFBundleIdentifier)"; done < <(aux_kexts)
	[ -n "$ids" ] || { say "  no kexts in $EXT besides VoodooHDA: no auxiliary collection needed"; return 2; }
	a+=(-A "$out" -z)
	say "  kmutil ${a[*]}"
	if ! R kmutil "${a[@]}" > "$WORK/aux-build.log" 2>&1; then sed 's/^/    /' "$WORK/aux-build.log" | tail -15; return 1; fi
	if ! R kmutil inspect -a x86_64 -A "$out" > "$WORK/aux-inspect.txt" 2>&1; then sed 's/^/    /' "$WORK/aux-inspect.txt" | tail -5; return 1; fi
	for k in $ids; do grep -q "$k" "$WORK/aux-inspect.txt" || { say "  $k is not in the built collection"; return 1; }; done
	say "  built and inspected: $(wc -c < "$out" | tr -d ' ') bytes, contains$ids"
	R kmutil inspect -a x86_64 -A "$out" --show-prelink-info 2>/dev/null | grep -o '_PrelinkBundlePath[^;]*;[^;]*' | head -4 | sed 's/^/    /' || true
}
aux_publish() {   # file
	R mkdir -p "$STATE"
	if [ -f "$AUX" ]; then
		[ -f "$STATE/aux-original.kc" ] || R cp -p "$AUX" "$STATE/aux-original.kc"
		R cp -p "$AUX" "$STATE/aux-previous.kc"
	fi
	R cp "$1" "$AUX"; R chown root:wheel "$AUX"; R chmod 644 "$AUX"
	say "  published $AUX (previous: $STATE/aux-previous.kc, first one replaced: $STATE/aux-original.kc)"
}
aux_save() {   # file name: a copy + the /Library/Extensions manifest it was built from
	R mkdir -p "$STATE"; R cp "$1" "$STATE/$2.kc"
	aux_manifest > "$WORK/$2.lext"; R cp "$WORK/$2.lext" "$STATE/$2.lext"
}

voodoo_out() {
	step "Option 1 out first (VoodooHDA and AppleHDA would both claim HDEF)"
	local v=0 p; for p in "$VOODOO" /Library/PreferencePanes/VoodooHDA.prefPane "$HOME/Library/PreferencePanes/VoodooHDA.prefPane"; do
		if [ -e "$p" ]; then v=1; run rm -rf "$p"; fi; done
	if is_loaded org.voodoo.driver.VoodooHDA; then say "VoodooHDA is loaded now; it goes away with the restart"; fi
	[ "$v" = 1 ] || say "VoodooHDA not installed"
	step "Auxiliary collection for the CURRENT (sealed) snapshot, without VoodooHDA"
	local rc=0; aux_build "/$KCDIR/BootKernelExtensions.kc" "/$KCDIR/SystemKernelExtensions.kc" /System/Library/Kernels/kernel "$WORK/aux-sealed.kc" || rc=$?
	case $rc in
		0) if live; then
			aux_save "$WORK/aux-sealed.kc" aux-sealed; say "  kept as $STATE/aux-sealed.kc (MODE=revert puts it back)"
			if [ "$v" = 1 ]; then aux_publish "$WORK/aux-sealed.kc"; fi
		   else say "  dry run: test build only (install keeps it as $STATE/aux-sealed.kc, and publishes it if VoodooHDA was there)"; fi;;
		2) if [ "$v" = 1 ]; then run rm -f "$AUX"; fi;;
		*) if live; then
			[ "$v" = 1 ] && say "VoodooHDA.kext was removed: reinstall it with audio-voodoohda.sh MODE=install if you stop here"
			die "the auxiliary collection did not build (log above); nothing on the root volume was changed"
		   else warn "the auxiliary collection test build failed (log above): install would stop here"; fi;;
	esac
}

mount_root() {
	step "Mount the root volume read-write at $MNT (OCLP mount.py)"
	if [ -f "$MNT/System/Library/CoreServices/SystemVersion.plist" ]; then say "already mounted"
	else run /sbin/mount -o nobrowse -t apfs "/dev/$ROOTDEV" "$MNT" || die "cannot mount /dev/$ROOTDEV at $MNT"; fi
	if live; then
		[ "$(defaults read "$MNT/System/Library/CoreServices/SystemVersion" ProductBuildVersion)" = "$BUILD" ] \
			|| die "the mounted root is not $BUILD: a macOS update is staged; finish or cancel it first"
		say "sanity: $MNT is $BUILD"
	fi
}

install_all() {
	voodoo_out
	mount_root
	local libkern="$MNT/System/Library/Extensions/System.kext/PlugIns/Libkern.kext/Libkern"
	step "Merge the KDK's /System/Library/Extensions (OCLP kdk_merge.py)"
	if [ -f "/$RECORD" ] && [ "$(plist_get "/$RECORD" 'Kernel\ Debug\ Kit\ Used')" = "$KDK" ] && [ -f "$libkern" ]; then
		say "same KDK already merged; skipped"
	elif live; then
		say "+ rsync -r -i -a $KDK/System/Library/Extensions/ $MNT/System/Library/Extensions"
		R /usr/bin/rsync -r -i -a "$KDK/System/Library/Extensions/" "$MNT/System/Library/Extensions" > "$WORK/kdk-merge.log" || die "KDK merge failed"
		R mkdir -p "$STATE"; R cp "$WORK/kdk-merge.log" "$STATE/kdk-merge.log"
		say "  $(wc -l < "$WORK/kdk-merge.log" | tr -d ' ') items (log: $STATE/kdk-merge.log)"
		[ -f "$libkern" ] || die "KDK merge failed (no Libkern on $MNT)"
	else say "would run: rsync -r -i -a $KDK/System/Library/Extensions/ $MNT/System/Library/Extensions"; fi

	step "AppleHDA.kext -> $MNT/System/Library/Extensions (OCLP utilities/files.py install_new_file + fix_permissions)"
	if live && [ -e "$MNT/System/Library/Extensions/AppleHDA.kext" ]; then run /bin/rm -R "$MNT/System/Library/Extensions/AppleHDA.kext"; fi
	run /bin/cp -R "${SRC:-<payload>/$SRC_REL}" "$MNT/System/Library/Extensions" || die "copy failed"
	run /bin/chmod -Rf 755 "$MNT/System/Library/Extensions/AppleHDA.kext"
	run /usr/sbin/chown -Rf root:wheel "$MNT/System/Library/Extensions/AppleHDA.kext"
	if live; then [ "$(tree_sha "$MNT/System/Library/Extensions/AppleHDA.kext")" = "$(tree_sha "$SRC")" ] || die "copied AppleHDA.kext differs from the payload"; say "  copy verified"; fi

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
	<key>Patched By</key><string>macos_helios300 tools/macos/audio-applehda.sh (OCLP's Modern Audio set only)</string>
</dict>
</plist>
EOF
	plutil -lint "$rec" >/dev/null || die "record does not lint"
	if live && [ -e "$MNT/$RECORD" ]; then run /bin/rm "$MNT/$RECORD"; fi
	run /bin/cp "$rec" "$MNT/System/Library/CoreServices/"
	live || plutil -p "$rec" | sed 's/^/  /'

	step "Rebuild the boot + system kernel collections (OCLP kernelcache/kernel_collection/boot_system.py)"
	if live; then
		say "+ kmutil create --allow-missing-kdk --volume-root $MNT --update-all --variant-suffix release"
		if ! R /usr/bin/kmutil create --allow-missing-kdk --volume-root "$MNT" --update-all --variant-suffix release > "$WORK/kmutil.log" 2>&1; then
			tail -25 "$WORK/kmutil.log"
			die "kmutil failed; nothing is blessed, the next boot is still the sealed snapshot (no revert needed)"
		fi
		tail -8 "$WORK/kmutil.log" | sed 's/^/  /'; R cp "$WORK/kmutil.log" "$STATE/kmutil-update-all.log"
	else say "would run: kmutil create --allow-missing-kdk --volume-root $MNT --update-all --variant-suffix release"; fi

	if [ "$RSRREPAIR" = 1 ]; then
		step "RSRRepair (OCLP sys_patch_helpers.install_rsr_repair_binary, from the app's payloads.dmg)"
		local pdmg=""; if [ -d "$APP" ]; then pdmg=$(find "$APP/Contents" -name payloads.dmg -print -quit 2>/dev/null || true); fi
		if [ -n "$pdmg" ]; then
			mkdir -p "$PL_MNT"
			if hdiutil attach -readonly -noverify -nobrowse -mountpoint "$PL_MNT" -passphrase password "$pdmg" </dev/null >/dev/null; then
				if [ -x "$PL_MNT/Tools/RSRRepair" ]; then run "$PL_MNT/Tools/RSRRepair" --install || say "RSRRepair install failed (OCLP logs and continues)"
				else say "no Tools/RSRRepair in payloads.dmg; skipped"; fi
			else say "cannot mount payloads.dmg; RSRRepair skipped"; fi
		else say "payloads.dmg not found; RSRRepair skipped"; fi
	fi

	step "Auxiliary collection for the NEW boot/system collections (PG106FB, PG106Accel)"
	local auxok=0 rc=0
	if live; then
		aux_build "$MNT/$KCDIR/BootKernelExtensions.kc" "$MNT/$KCDIR/SystemKernelExtensions.kc" "$MNT/System/Library/Kernels/kernel" "$WORK/aux-patched.kc" || rc=$?
		case $rc in 0) auxok=1;; 2) auxok=2;; *) warn "the aux build against the new collections failed: PG106FB/PG106Accel will not load after the restart; run MODE=aux then";; esac
	else say "would build it against $MNT/$KCDIR/{Boot,System}KernelExtensions.kc (the command above, other -B/-S) and publish it after bless"; fi

	step "Create and bless a new snapshot (OCLP mount/snapshot.py, Intel form)"
	run /usr/sbin/bless --folder "$MNT/System/Library/CoreServices" --bootefi --create-snapshot \
		|| die "bless failed: the next boot is still the sealed snapshot; $AUX was not touched"
	if [ "$auxok" = 1 ]; then aux_save "$WORK/aux-patched.kc" aux-patched; aux_publish "$WORK/aux-patched.kc"
	elif [ "$auxok" = 2 ]; then run rm -f "$AUX"; fi
	run /sbin/umount "$MNT" || true
	say ""; say "Restart. Then MODE=status (AppleHDA, layout 29, NMHDA, PG106FB/PG106Accel)."
	say "If the patched system does not come up: OpenCore picker -> Recovery -> Terminal (MODE=revert prints the lines; docs/HARDWARE.md has them too)."
}

revert_help() {
	cat <<'EOF'
From Recovery (OpenCore picker: Space -> Recovery -> Utilities > Terminal; FileVault is off, so the volumes are mounted):
  diskutil list                                    # the system volume, normally "Macintosh HD"
  bless --mount "/Volumes/Macintosh HD" --bootefi --last-sealed-snapshot
  D=$(ls -d /Volumes/*Data | head -1)             # normally "/Volumes/Macintosh HD - Data"
  cp "$D/Library/Helios300/audio-applehda/aux-sealed.kc" "$D/Library/KernelCollections/AuxiliaryKernelExtensions.kc"
  reboot
(The cp only matters for PG106FB/PG106Accel; without it run MODE=aux once macOS is up, and restart.)
EOF
}

revert() {
	step "Revert: boot Apple's last sealed snapshot (OCLP sys_patch.py _unpatch_root_vol)"
	ROOTDEV=$(root_dev); BUILD=$(sw_vers -buildVersion)
	if [ ! -e "/$RECORD" ]; then
		say "no OCLP patch record on the booted snapshot (sealed: $(sealed))"
		[ "$FORCE" = 1 ] || { say "nothing to revert (FORCE=1 blesses the last sealed snapshot anyway)"; return; }
	fi
	mount_root
	run /usr/sbin/bless --mount "$MNT" --bootefi --last-sealed-snapshot || die "bless --last-sealed-snapshot failed"
	run /sbin/umount "$MNT" || true
	step "Auxiliary collection for the sealed snapshot"
	if [ -f "$STATE/aux-sealed.kc" ] && [ -f "$STATE/aux-sealed.lext" ]; then
		aux_manifest > "$WORK/now.lext"
		if cmp -s "$WORK/now.lext" "$STATE/aux-sealed.lext"; then aux_publish "$STATE/aux-sealed.kc"
		else say "/Library/Extensions changed since it was built: after the restart run MODE=aux, restart again"; diff "$STATE/aux-sealed.lext" "$WORK/now.lext" | sed 's/^/  /' || true; fi
	else say "no saved aux-sealed.kc: after the restart run MODE=aux if PG106FB/PG106Accel did not load"; fi
	if [ "$REMOVE_KDK" = 1 ]; then
		step "Remove the KDK"
		local k; k=$(kdk_find)
		if [ -n "$k" ]; then run rm -rf "$k"; run pkgutil --forget "com.apple.pkg.KDK.$KDK_BUILD" || true; else say "no KDK $KDK_BUILD installed"; fi
	else say "The KDK stays in $KDK_DIR (REMOVE_KDK=1 deletes it; the next install would download it again)."; fi
	say ""; say "Restart. AppleHDA.kext, the merged KDK files and the patch record live only in the patched snapshot."
	say "VoodooHDA (option 1) is not put back; audio-voodoohda.sh MODE=install does that."
	say ""; revert_help
}

aux_mode() {
	step "Auxiliary collection from $EXT against the running boot/system collections"
	local rc=0; aux_build "/$KCDIR/BootKernelExtensions.kc" "/$KCDIR/SystemKernelExtensions.kc" /System/Library/Kernels/kernel "$WORK/aux.kc" || rc=$?
	case $rc in
		0) aux_publish "$WORK/aux.kc"
		   if [ -e "/$RECORD" ]; then aux_save "$WORK/aux.kc" aux-patched; else aux_save "$WORK/aux.kc" aux-sealed; fi;;
		2) run rm -f "$AUX";;
		*) die "the build failed (log above); $AUX left as it was. Fallback: sudo kmutil load -p $EXT/PG106FB.kext and $EXT/PG106Accel.kext, restart";;
	esac
	say "Restart, then MODE=status."
}

status() {
	step "Root volume"
	say "  build $(sw_vers -buildVersion); booted snapshot sealed: $(sealed)"
	if [ -f "/$RECORD" ]; then plutil -p "/$RECORD" | sed 's/^/  /'; else say "  no OCLP patch record on / (not patched)"; fi
	say "  AppleHDA on /: $( [ -e /System/Library/Extensions/AppleHDA.kext ] && defaults read /System/Library/Extensions/AppleHDA.kext/Contents/Info CFBundleVersion || echo absent)"
	step "Loaded"
	printf '%s\n' "$LOADED" | grep -E 'AppleHDA|IOHDAFamily|DspFuncLib|AppleALC|Lilu|NMHDA|AMFIPass|VoodooHDA|pg106|PG106' | sed 's/^ *[0-9]* *[0-9]* *0x[0-9a-f]* *0x[0-9a-f]* *0x[0-9a-f]* */  /; s/ [0-9A-F-]\{36\}.*//' || true
	if is_loaded org.voodoo.driver.VoodooHDA; then warn "VoodooHDA is loaded next to AppleHDA: audio-voodoohda.sh MODE=revert, restart"; fi
	step "PG106FB / PG106Accel (auxiliary collection)"
	local k id miss=0
	for k in "$EXT/PG106FB.kext" "$EXT/PG106Accel.kext"; do
		[ -d "$k" ] || continue; id=$(plist_get "$k/Contents/Info.plist" CFBundleIdentifier)
		if is_loaded "$id"; then say "  $id: loaded"; else say "  $id: NOT loaded"; miss=1; fi
	done
	[ "$miss" = 0 ] || say "  -> MODE=aux, restart (the aux collection was built for other boot/system collections)"
	say "  $AUX: $(stat -f '%Sm' "$AUX" 2>/dev/null || echo absent); boot KC: $(stat -f '%Sm' "/$KCDIR/BootKernelExtensions.kc" 2>/dev/null)"
	step "ALC255 (HDEF)"
	ioreg -rlw0 -n HDEF -d1 2>/dev/null | grep -E '"(alc-layout-id|layout-id)"' | sed 's/^ */  /' || true
	ioreg -rlw0 -c IOHDACodecDevice 2>/dev/null | grep -E '"IOHDACodecVendorID"' | sed 's/^ */  codec /' || true
	step "HDMI audio (NMHDA + PG106FB pairing; the HDMI device appears with a monitor attached)"
	sysctl -n debug.nmhda 2>/dev/null | sed 's/^/  nmhda: /' || say "  debug.nmhda: absent (NMHDA not loaded)"
	ioreg -rw0 -c AppleHDAController -d1 2>/dev/null | grep -E '^\+-o' | sed 's/^/  /' | head -4 || true
	ioreg -rlw0 -c PG106FB -d1 2>/dev/null | grep -E '"(hda-gfx|audio-codec-info|connector-type|av-signal-type)"' | sed 's/^ */  PG106FB /' || say "  PG106FB not attached"
	local h; h=$({ ioreg -rlw0 -n HDAU -d1; ioreg -rlw0 -n pci10de,10f1 -d1; } 2>/dev/null | awk '/"hda-gfx"/ && v == "" {v = $0} END {print v}')
	if [ -n "$h" ]; then say "  HDAU ${h#"${h%%[! ]*}"}"; else say "  HDAU: no hda-gfx (DeviceProperties PciRoot(0x0)/Pci(0x1,0x0)/Pci(0x0,0x1))"; fi
	step "Core Audio"
	system_profiler SPAudioDataType 2>/dev/null | sed -n '1,60p'
	say ""; say "Combo jack: plug in a headset (CTIA, iPhone-type) and check System Settings > Sound > Input;"
	say "docs/HARDWARE.md 'Headset microphone' if there is no headset input."
}

case "$MODE" in
	dry-run) preflight; payload; kdk; install_all; say; say "dry run: nothing on the system was changed (downloads, if any, are in $CACHE)";;
	install) preflight; payload; [ -n "$SRC" ] || die "no payload"; kdk; install_all;;
	aux) aux_mode;;
	revert) revert;;
	status) status;;
	*) die "MODE must be dry-run, install, aux, revert or status";;
esac
