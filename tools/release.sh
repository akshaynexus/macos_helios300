#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# release.sh — build every macos_helios300 release artifact from source on Linux and write dist/ + manifest.json.
#
#   tools/release.sh --version 0.1.1 --real-kc DIR [--dry-run] [--rev REV] [--driver-rev REV] [--driver-repo DIR]
#                    [--skip a,b] [--dist DIR] [--iaf2 FILE] [--wifi-country CC] [--no-kc-audit]
#
# What it does (docs/MANIFEST.md is the contract):
#   1. Exports this repo at REV (default HEAD) and nvidia-macos-driver at the submodule commit REV records
#      (or --driver-rev) with `git archive` into a fresh work dir. Uncommitted changes in either checkout are
#      never part of a release, and nothing is built inside a working tree.
#   2. Cross-builds each artifact in tools/release/spec.json with the Linux toolchain the repos already use
#      (clang + macOS 26.5 SDK + Apple ld64 from cctools-port or ld64.lld + xnu-12377 exports + rustup's
#      x86_64-apple-darwin target), each with its own checks (KPI audit, Mach-O checks, macho-check.py).
#   3. Release gates: PG106's 17 firmware files must be IN its binary (tools/release/check-pg106-firmware.py), and
#      with --real-kc DIR every OpenCore and /Library/Extensions kext is audited against that Mac's real kernel
#      collections (tools/release/kc_gate.py: tools/kc-audit.py for Wi-Fi/Bluetooth, the driver's
#      pascal/scripts/kc-preflight.sh for PG106/PG106FB/PG106Accel); result in dist/logs/kc-audit.log and in the
#      manifest (release.kc_audit). A failed audit stops a real release. A real release needs --real-kc (or
#      $HELIOS_KC_DIR); --no-kc-audit builds one without it and the manifest says kc_audit "skipped".
#   4. Ad-hoc signs bundles, apps and dylibs with rcodesign (apple-codesign), as `codesign -s -` does on a Mac
#      (PG106FB and PG106Accel load from /Library/Extensions; the plugin, its air-runtime/libLLVM.dylib and the
#      dylibs through dyld). Signing is not optional: without rcodesign the run stops before building (exit 77),
#      and a failed or unverifiable signature stops it (exit 1); nothing that installs into /Library ships unsigned.
#   5. tools/release/make_manifest.py zips each bundle deterministically, hashes everything, checks the result
#      against tools/release/manifest.schema.json and writes dist/manifest.json + dist/SHA256SUMS.txt.
#   6. tools/release/sign-manifest.sh signs manifest.json into dist/manifest.json.sig (Ed25519) with the key at
#      $HELIOS_SIGNING_KEY or ~/.config/helios300-release/manifest-ed25519, created there (0600, with a notice) if absent.
#      The app refuses an update whose manifest does not verify; a key other than the committed one is reported.
#
# It publishes nothing. A GitHub Actions runner has no macOS SDK, so this runs on the owner's Linux machine.
# --dry-run: a required artifact that fails to build is listed in the manifest (release.missing, dry_run=true)
# instead of stopping; 1401 refuses such a manifest unless it was pointed at it explicitly.
#
# --wifi-country CC (default IN, the owner's country; $HELIOS_WIFI_COUNTRY): written as Ath10kCountryCode into the
# built Ath10kMac.kext (tools/release/set-wifi-country.py) and recorded in the manifest. Not a boot-arg: 1401 keeps
# boot-args out of NVRAM -> Delete, so a boot-arg in a new config never reaches an installed Mac. "00" = world rules.
#
# Build keys for --skip: pg106 pg106fb nmhda bluetooth wifi ssdt pg106accel plugin xlate nvk licences scripts setupapp
# Environment: HELIOS_SIGNING_KEY (the manifest signing key), HELIOS_KC_DIR (= --real-kc), HELIOS_KC_AUDIT=skip (= --no-kc-audit; 1401 --build-from-source sets it
#   when HELIOS_KC_DIR is unset), HELIOS_WIFI_COUNTRY (= --wifi-country), MACOS_SDK, LD64, XNU_CONFIG,
#   HELIOS_CROSS (tools/cross-build-kext.sh defaults), HELIOS_IAF2 (= --iaf2), RELEASE_CACHE
#   (downloads + the NVK Mesa tree, default ~/.cache/helios300-release), PG106_NATIVE_BIN (NVK's build-machine
#   mesa_clc/vtn_bindgen2; built by build-nvk-linux.sh into the cache when unset).
# Exit: 0 dist/ written, 1 failure, 77 toolchain missing.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"
CROSS="${HELIOS_CROSS:-$HOME/.local/share/helios300-cross}"
export MACOS_SDK="${MACOS_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk}"
export LD64="${LD64:-$CROSS/cctools/bin/x86_64-apple-darwin17-ld}"
export XNU_CONFIG="${XNU_CONFIG:-$CROSS/xnu-12377.1.9/config}"
export RELEASE_CACHE="${RELEASE_CACHE:-$HOME/.cache/helios300-release}"
RCODESIGN="${RCODESIGN:-$(command -v rcodesign || echo "$HOME/.cargo/bin/rcodesign")}"

VERSION="" DRY=0 REV=HEAD DRIVER_REV="" DRIVER_REPO="" SKIP="," DIST="$ROOT/dist" IAF2="${HELIOS_IAF2:-}"
KC_DIR="${HELIOS_KC_DIR:-}" NO_KC=0 WIFI_CC="${HELIOS_WIFI_COUNTRY:-IN}"
[ "${HELIOS_KC_AUDIT:-}" != skip ] || NO_KC=1
while [ $# -gt 0 ]; do
	case "$1" in
	--version) VERSION="$2"; shift 2 ;;
	--dry-run) DRY=1; shift ;;
	--rev) REV="$2"; shift 2 ;;
	--driver-rev) DRIVER_REV="$2"; shift 2 ;;
	--driver-repo) DRIVER_REPO="$2"; shift 2 ;;
	--skip) SKIP=",$2,"; shift 2 ;;
	--dist) DIST="$2"; shift 2 ;;
	--iaf2) IAF2="$2"; shift 2 ;;
	--real-kc) KC_DIR="$2"; shift 2 ;;
	--no-kc-audit) NO_KC=1; shift ;;
	--wifi-country) WIFI_CC="$2"; shift 2 ;;
	-h|--help) sed -n '2,/^# Exit:/p' "$0"; exit 0 ;;
	*) echo "release.sh: unknown argument $1" >&2; exit 1 ;;
	esac
done
die() { echo "release.sh: FAIL — $*" >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] || die "--version X.Y.Z is required"
[[ "$WIFI_CC" =~ ^[A-Za-z0-9]{2}$ ]] || die "--wifi-country takes a two-letter wireless-regdb code (or 00)"
WIFI_CC="${WIFI_CC^^}"
if [ -n "$KC_DIR" ]; then
	[ "$NO_KC" = 0 ] || die "--real-kc and --no-kc-audit exclude each other"
	[ -d "$KC_DIR" ] || die "--real-kc: no directory $KC_DIR"
	KC_DIR="$(cd "$KC_DIR" && pwd)"
	[ -f "$KC_DIR/BootKernelExtensions.kc" ] && [ -f "$KC_DIR/SystemKernelExtensions.kc" ] ||
		die "--real-kc $KC_DIR must hold BootKernelExtensions.kc and SystemKernelExtensions.kc from the target Mac"
elif [ "$NO_KC" = 0 ] && [ "$DRY" = 0 ]; then
	die "a real release is audited against the target Mac's kernel collections: pass --real-kc DIR (or set HELIOS_KC_DIR), or --no-kc-audit to build one without that gate (recorded in the manifest)"
fi
[ -f "$MACOS_SDK/usr/lib/libkmod.a" ] || { echo "release.sh: SKIP — no macOS SDK at $MACOS_SDK"; exit 77; }
[ -x "$LD64" ] || { echo "release.sh: SKIP — no Apple ld64 at $LD64"; exit 77; }
[ -d "$XNU_CONFIG" ] || { echo "release.sh: SKIP — no xnu config at $XNU_CONFIG"; exit 77; }
for t in clang llvm-nm llvm-objdump ld64.lld iasl git python3 openssl; do command -v "$t" >/dev/null || { echo "release.sh: SKIP — $t missing"; exit 77; }; done
# What installs into /Library (kmutil, dyld in WindowServer and apps) must be signed; install-gpu.sh copies it as is.
SIGN_KEYS=(pg106fb pg106accel plugin xlate nvk setupapp)
for k in "${SIGN_KEYS[@]}"; do
	[[ "$SKIP" == *",$k,"* ]] && continue
	[ -x "$RCODESIGN" ] || { echo "release.sh: SKIP — rcodesign (apple-codesign) missing: PG106FB, PG106Accel and the userspace bundles may not ship unsigned (cargo install apple-codesign, or set RCODESIGN)"; exit 77; }
	break
done

SELF_COMMIT="$(git -C "$ROOT" rev-parse --verify "$REV^{commit}")" || die "no commit $REV"
SUB_COMMIT="$(git -C "$ROOT" ls-tree "$SELF_COMMIT" kexts/nvidia-macos-driver | awk '{print $3}')"
DRIVER_REPO="${DRIVER_REPO:-$ROOT/kexts/nvidia-macos-driver}"
DRIVER_COMMIT="$(git -C "$DRIVER_REPO" rev-parse --verify "${DRIVER_REV:-$SUB_COMMIT}^{commit}")" ||
	die "driver commit ${DRIVER_REV:-$SUB_COMMIT} not in $DRIVER_REPO (fetch it there, or pass --driver-repo)"
if [ -n "$DRIVER_REV" ] && [ "$DRIVER_COMMIT" != "$SUB_COMMIT" ]; then
	echo "release.sh: NOTE driver $DRIVER_COMMIT is not the submodule commit $SUB_COMMIT recorded at ${SELF_COMMIT:0:12}"
	[ "$DRY" = 1 ] || die "a real release builds the driver at the submodule commit; bump the submodule first (or use --dry-run)"
fi
[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ] ||
	echo "release.sh: NOTE this checkout has uncommitted changes; they are NOT in the release (built from ${SELF_COMMIT:0:12})"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/helios300-release.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
SELF="$WORK/self"; DRV="$WORK/driver"; STAGE="$WORK/stage"; LOGS="$DIST/logs"
mkdir -p "$SELF" "$DRV" "$STAGE"
git -C "$ROOT" archive "$SELF_COMMIT" | tar -x -C "$SELF"
git -C "$DRIVER_REPO" archive "$DRIVER_COMMIT" | tar -x -C "$DRV"
rm -rf "$DIST"; mkdir -p "$DIST" "$LOGS"
echo "release.sh: v$VERSION from macos_helios300 ${SELF_COMMIT:0:12}, nvidia-macos-driver ${DRIVER_COMMIT:0:12}$([ "$DRY" = 1 ] && echo ' (DRY RUN)')"

MISSING=() SIGNED=() FAILED=0
skipped() { [[ "$SKIP" == *",$1,"* ]]; }
# step <key> <artifact names...> -- <command...>: starts the build as a background job with its own log.
# Steps are independent (own build dir, own staged names, own log), so they all run at once; collect_steps waits
# for them in the order they were started and records results, so output and the manifest don't depend on timing.
# A job also keeps errexit: a function called as `f || rc=$?` runs with errexit off for every command in it, so a
# failing build step would be reported as ok. A job's own `set -e` still applies.
STEP_KEYS=() STEP_PIDS=() STEP_NAMES=()
step() {
	local key="$1"; shift; local names=()
	while [ "$1" != "--" ]; do names+=("$1"); shift; done; shift
	if skipped "$key"; then
		for n in "${names[@]}"; do MISSING+=(--missing-why "$n=skipped (--skip $key)"); done
		echo "  skip   $key"; return 0
	fi
	( set -e; "$@" ) >"$LOGS/$key.log" 2>&1 &
	STEP_KEYS+=("$key"); STEP_PIDS+=("$!"); STEP_NAMES+=("${names[*]}")
}
collect_steps() {
	local i key log rc n
	for i in "${!STEP_PIDS[@]}"; do
		key="${STEP_KEYS[$i]}" log="$LOGS/${STEP_KEYS[$i]}.log" rc=0
		wait "${STEP_PIDS[$i]}" || rc=$?
		if [ "$rc" = 0 ]; then
			echo "  ok     $key  ($(tail -1 "$log" | cut -c1-110))"
		else
			echo "  FAIL   $key  rc=$rc ($(tail -1 "$log" | cut -c1-110)); log: $log"
			for n in ${STEP_NAMES[$i]}; do MISSING+=(--missing-why "$n=build failed (rc $rc, logs/$key.log)"); done
			FAILED=1
		fi
	done
	echo "release.sh: ${#STEP_PIDS[@]} builds finished in $((SECONDS - BUILD_START)) s (run in parallel)"
}
put() { [ -e "$1" ] || { echo "release.sh: expected $1" >&2; return 1; }; rm -rf "$STAGE/$2"; cp -R "$1" "$STAGE/$2"; }

b_pg106() {
	BUILD_DIR="$WORK/b/pg106" PG106_MACOS_SDK="$MACOS_SDK" PG106_LD64="$LD64" PG106_XNU_CONFIG="$XNU_CONFIG" \
		bash "$DRV/pascal/kext/scripts/cross-build-macos.sh"
	# PG106 is OpenCore-injected: no bundle on disk, so OSKextRequestResource cannot serve Contents/Resources. Every
	# pinned firmware file (17: ACR, SEC2, GR + sw_*) must be in the binary itself, byte for byte.
	python3 -I "$HERE/release/check-pg106-firmware.py" --kext "$WORK/b/pg106/PG106.kext" --driver "$DRV"
	put "$WORK/b/pg106/PG106.kext" PG106.kext
}
b_pg106fb() {
	[ -f "$DRV/pascal/fb/scripts/cross-build-macos.sh" ] || { echo "pascal/fb is not in driver ${DRIVER_COMMIT:0:12}"; return 1; }
	BUILD_DIR="$WORK/b/pg106fb" PG106_MACOS_SDK="$MACOS_SDK" PG106_LD64="$LD64" PG106_XNU_CONFIG="$XNU_CONFIG" \
		bash "$DRV/pascal/fb/scripts/cross-build-macos.sh"
	put "$WORK/b/pg106fb/PG106FB.kext" PG106FB.kext
}
b_nmhda() { bash "$HERE/release/cross-nmhda.sh" "$DRV/kexts/NMHDA" "$WORK/b/nmhda" && put "$WORK/b/nmhda/NMHDA.kext" NMHDA.kext; }
b_bluetooth() {
	QCA_FIRMWARE_CACHE="$RELEASE_CACHE/qca-firmware" MACOS_SDK="$MACOS_SDK" LD64="$LD64" XNU_CONFIG="$XNU_CONFIG" \
		bash "$SELF/kexts/bluetooth/scripts/cross-build.sh" "$WORK/b/bt"
	put "$WORK/b/bt/QCARomePatchRAM.kext" QCARomePatchRAM.kext
}
b_wifi() {
	# The native pair only (kexts/wifi README): Ath10kMac (core) then AirportAth10k (IO80211/Skywalk front end).
	# The Ethernet-style core + Connectivity app (cross-build.sh --ethernet-debug) is developer-only and not shipped.
	( cd "$SELF/kexts/wifi" && MACOS_SDK="$MACOS_SDK" LD64="$LD64" XNU_CONFIG="$XNU_CONFIG" bash scripts/cross-build.sh )
	local out="$SELF/kexts/wifi/build/native"
	put "$out/Ath10kMac.kext" Ath10kMac.kext
	put "$out/AirportAth10k.kext" AirportAth10k.kext
	# The regulatory country travels in the injected kext's personality (Ath10kCountryCode), not as a boot-arg.
	python3 -I "$HERE/release/set-wifi-country.py" --plist "$STAGE/Ath10kMac.kext/Contents/Info.plist" \
		--regdb "$SELF/kexts/wifi/src/kext/Ath10kRegulatoryData.hpp" --country "$WIFI_CC"
	echo "wifi: Ath10kMac.kext + AirportAth10k.kext (native), country $WIFI_CC"
}
b_ssdt() {
	mkdir -p "$WORK/b/ssdt"; cp "$DRV/pascal/fb/acpi/SSDT-PG106-HDAU.dsl" "$WORK/b/ssdt/"
	( cd "$WORK/b/ssdt" && iasl -vs -ve SSDT-PG106-HDAU.dsl )
	put "$WORK/b/ssdt/SSDT-PG106-HDAU.aml" SSDT-PG106-HDAU.aml
	cp "$SELF/kexts/bluetooth/acpi/SSDT-HS07-UPC.dsl" "$WORK/b/ssdt/"
	( cd "$WORK/b/ssdt" && iasl -vs -ve SSDT-HS07-UPC.dsl )
	put "$WORK/b/ssdt/SSDT-HS07-UPC.aml" SSDT-HS07-UPC.aml
	echo "SSDTs: PG106-HDAU $(sha256sum "$STAGE/SSDT-PG106-HDAU.aml" | cut -c1-16), HS07-UPC $(sha256sum "$STAGE/SSDT-HS07-UPC.aml" | cut -c1-16) (iasl $(iasl -v 2>/dev/null | grep -o 'version [0-9]*' | head -1))"
}
b_pg106accel() {
	local iaf2="${IAF2:-$DRIVER_REPO/research/prior-art/meteor-lake-xe/research/IOAcceleratorFamily2-recovery-reference.macho}"
	[ -f "$iaf2" ] || { echo "no macOS 26 IOAcceleratorFamily2 reference binary (not in git; pass --iaf2)"; return 1; }
	BUILD_DIR="$WORK/b/accel" PGA_MACOS_SDK="$MACOS_SDK" PGA_LD64="$LD64" PGA_XNU_CONFIG="$XNU_CONFIG" PGA_IAF2="$iaf2" \
		bash "$DRV/kexts/PG106Accel/cross-build.sh" | tee "$WORK/accel.out"
	local k; k="$(sed -n 's/^cross-build: OK — \(.*\)$/\1/p' "$WORK/accel.out" | tail -1)"
	put "$k" PG106Accel.kext
}
AIR_LLVM_VERSION=23.1.4   # tools/release/cross-air-llvm.sh
b_plugin() {
	MACHO_CHECK="$DRV/nvk/pascal/scripts/macho-check.py" bash "$HERE/release/cross-plugin.sh" "$DRV/plugin" "$WORK/b/plugin" "$VERSION"
	# The plugin's in-process AIR reader (plugin/NVMTLLibraryLoad.m dlopens Contents/MacOS/air-runtime/libLLVM.dylib):
	# without it every shader not already in the cache fails to compile. Upstream LLVM, cross-built (cached).
	bash "$HERE/release/cross-air-llvm.sh" "$WORK/b/air-llvm"
	python3 -I "$HERE/release/check-air-llvm.py" --dylib "$WORK/b/air-llvm/libLLVM.dylib" --plugin "$DRV/plugin" \
		--llvm-version "$AIR_LLVM_VERSION" ${HELIOS_AIR_HOST_LIBLLVM:+--host-libllvm "$HELIOS_AIR_HOST_LIBLLVM"}
	local b="$WORK/b/plugin/NVMTLDriver.bundle"
	mkdir -p "$b/Contents/MacOS/air-runtime" "$b/Contents/Resources/air-runtime"
	cp "$WORK/b/air-llvm/libLLVM.dylib" "$b/Contents/MacOS/air-runtime/libLLVM.dylib"
	cp "$WORK/b/air-llvm/licenses/"* "$b/Contents/Resources/air-runtime/"
	put "$b" NVMTLDriver.bundle
	echo "plugin: NVMTLDriver.bundle with air-runtime/libLLVM.dylib (LLVM $AIR_LLVM_VERSION, $(sha256sum "$b/Contents/MacOS/air-runtime/libLLVM.dylib" | cut -c1-16))"
}
b_xlate() { MACHO_CHECK="$DRV/nvk/pascal/scripts/macho-check.py" bash "$HERE/release/cross-xlate.sh" "$DRV/translator" "$WORK/b/xlate" && put "$WORK/b/xlate/libnvmtl_translate.dylib" libnvmtl_translate.dylib; }
b_nvk() {
	# The Mesa tree is prepared per driver commit (prepare-mesa.sh applies that commit's patches).
	local w="$RELEASE_CACHE/nvk-${DRIVER_COMMIT:0:12}"
	# The NVRM backend compiled into the same library needs NVIDIA's open-gpu-kernel-modules headers, a submodule
	# of the driver repo (git archive leaves submodules out). Use the commit the driver tree pins.
	local og og_dir
	og="$(git -C "$DRIVER_REPO" ls-tree "$DRIVER_COMMIT" third_party/open-gpu-kernel-modules | awk '{print $3}')"
	[ -n "$og" ] || { echo "driver ${DRIVER_COMMIT:0:12} pins no third_party/open-gpu-kernel-modules"; return 1; }
	og_dir="$RELEASE_CACHE/open-gpu-kernel-modules-$og"
	if [ ! -d "$og_dir/src/common/sdk/nvidia/inc" ]; then
		rm -rf "$og_dir.part"; mkdir -p "$og_dir.part"
		if git -C "$DRIVER_REPO/third_party/open-gpu-kernel-modules" cat-file -e "$og^{commit}" 2>/dev/null; then
			git -C "$DRIVER_REPO/third_party/open-gpu-kernel-modules" archive "$og" | tar -x -C "$og_dir.part"
		else
			git init -q "$og_dir.part/.g" && git -C "$og_dir.part/.g" fetch -q --depth 1 https://github.com/NVIDIA/open-gpu-kernel-modules "$og" &&
				git -C "$og_dir.part/.g" archive FETCH_HEAD | tar -x -C "$og_dir.part" && rm -rf "$og_dir.part/.g"
		fi
		mv "$og_dir.part" "$og_dir"
	fi
	rmdir "$DRV/third_party/open-gpu-kernel-modules" 2>/dev/null || true
	ln -sfn "$og_dir" "$DRV/third_party/open-gpu-kernel-modules"
	# prepare-mesa.sh links pascal/bridge and pascal/core into the cached tree; those links point into the
	# work dir of the run that prepared it, which is gone by now (or was killed). Re-point them at this run's export.
	local pg="$w/mesa/src/nouveau/vulkan/nvkmd/pg106"
	if [ -d "$pg" ]; then
		ln -sfn "$DRV/pascal/bridge" "$pg/bridge"
		ln -sfn "$DRV/pascal/core" "$pg/core"
	fi
	local nvrm="$w/mesa/src/nouveau/vulkan/nvkmd/nvrm"
	[ ! -d "$nvrm" ] || ln -sfn "$og_dir" "$nvrm/open-gpu-kernel-modules"
	echo "open-gpu-kernel-modules ${og:0:12} -> $og_dir"
	if [ -z "${PG106_NATIVE_BIN:-}" ] && [ ! -x "$w/native/bin/mesa_clc" ]; then
		sh "$DRV/nvk/pascal/scripts/build-nvk-linux.sh" "$w"
	fi
	PG106_MACOS_SDK="$MACOS_SDK" PG106_CCTOOLS="$(dirname "$LD64")" sh "$DRV/nvk/pascal/scripts/cross-build-nvk-macos.sh" "$w"
	[ -f "$w/out/libvulkan_nouveau.dylib" ] || { echo "NVK cross build produced no libvulkan_nouveau.dylib"; return 1; }
	mkdir -p "$STAGE/NVK-Pascal"
	cp "$w/out/libvulkan_nouveau.dylib" "$w/out/nvk_icd.json" "$STAGE/NVK-Pascal/"
	echo "NVK-Pascal: $(sha256sum "$STAGE/NVK-Pascal/libvulkan_nouveau.dylib" | cut -c1-16)"
}
b_scripts() {
	# Post-install scripts (run on the Mac; docs/MANIFEST.md post_install): this repo's tools/macos installers, the
	# driver's gpu-apps.sh (which apps get the GTX 1060), and POST-INSTALL.md generated from spec.json's post_install.
	local f src n=0
	for f in self:audio-voodoohda.sh self:audio-applehda.sh self:install-gpu.sh driver:gpu-apps.sh; do
		case "$f" in self:*) src=$SELF/tools/macos/${f#self:};; driver:*) src=$DRV/tools/macos/${f#driver:};; esac
		f=${f#*:}
		[ -f "$src" ] || { echo "tools/macos/$f missing in the ${src#"$WORK/"} tree"; return 1; }
		bash -n "$src"
		put "$src" "$f"; chmod 0755 "$STAGE/$f"; n=$((n + 1))
	done
	python3 -I "$SELF/tools/release/post_install_doc.py" --spec "$SELF/tools/release/spec.json" --version "$VERSION" \
		--out "$STAGE/POST-INSTALL.md"
	echo "scripts: $n post-install scripts + POST-INSTALL.md"
}
b_setupapp() {
	# Helios 300 Setup.app (apps/HeliosSetup): swift test of its core first (never skipped here), then the macOS build.
	SKIP_TESTS=0 MACHO_CHECK="$DRV/nvk/pascal/scripts/macho-check.py" \
		bash "$HERE/release/cross-setup-app.sh" "$SELF" "$DRV" "$WORK/b/setupapp" "$VERSION"
	put "$WORK/b/setupapp/Helios 300 Setup.app" "Helios 300 Setup.app"
}
b_licences() {
	cp "$DRV/licenses/NVIDIA-firmware-LICENCE.txt" "$STAGE/NVIDIA-firmware-LICENCE.txt"
	cp "$DRV/licenses/Mesa-license.rst" "$STAGE/Mesa-license.rst"
	echo "licences copied"
}

BUILD_START=$SECONDS
step pg106 PG106.kext -- b_pg106
step pg106fb PG106FB.kext -- b_pg106fb
step nmhda NMHDA.kext -- b_nmhda
step bluetooth QCARomePatchRAM.kext -- b_bluetooth
step wifi Ath10kMac.kext AirportAth10k.kext -- b_wifi
step ssdt SSDT-PG106-HDAU.aml SSDT-HS07-UPC.aml -- b_ssdt
step pg106accel PG106Accel.kext -- b_pg106accel
step plugin NVMTLDriver.bundle -- b_plugin
step xlate libnvmtl_translate.dylib -- b_xlate
step nvk NVK-Pascal -- b_nvk
step licences NVIDIA-firmware-LICENCE.txt Mesa-license.rst -- b_licences
step scripts audio-voodoohda.sh audio-applehda.sh install-gpu.sh gpu-apps.sh POST-INSTALL.md -- b_scripts
step setupapp "Helios 300 Setup.app" -- b_setupapp
collect_steps

# Real-collection gate (before signing: the audit reads what was linked). A real release stops on any finding.
KC_ARG=()
if [ -n "$KC_DIR" ]; then
	if python3 -I "$HERE/release/kc_gate.py" --kc-dir "$KC_DIR" --stage "$STAGE" --driver "$DRV" \
		--kc-audit "$HERE/kc-audit.py" --log "$LOGS/kc-audit.log" --out "$WORK/kc-audit.json"; then
		echo "  ok     kc-audit  (against $KC_DIR)"
	else
		rc=$?
		[ -f "$WORK/kc-audit.json" ] || die "kc-audit could not run (rc $rc)"
		[ "$DRY" = 1 ] || die "kc-audit found problems (rc $rc; $LOGS/kc-audit.log): OpenCore/kmutil would refuse or misload a kext; nothing written"
		echo "  FAIL   kc-audit  rc=$rc ($LOGS/kc-audit.log); DRY RUN goes on, the manifest records it"
		FAILED=1
	fi
	KC_ARG=(--kc-audit "$WORK/kc-audit.json")
else
	echo "  skip   kc-audit  (no --real-kc$([ "$NO_KC" = 1 ] && echo ', --no-kc-audit'); the manifest says skipped)"
fi

# Ad-hoc signatures (codesign -s - on a Mac). OpenCore injection needs none; kmutil (PG106FB, PG106Accel in
# /Library/Extensions) and dyld (the plugin, its libLLVM, the dylibs) accept ad-hoc ones. Every one is required:
# a failed signing, or a signature that does not verify afterwards, stops the release. `rcodesign verify` rejects
# every ad-hoc signature (it says itself it is unreliable), so tools/release/check-adhoc-signature.py recomputes the
# CodeDirectory page hashes and, for a bundle, the Info.plist and CodeResources slots and every sealed file.
signed_ok() {   # <Mach-O> [<bundle it is the main executable of>]
	python3 -I "$HERE/release/check-adhoc-signature.py" "$1" ${2:+--bundle "$2"} >>"$LOGS/signatures.log" 2>&1
}
sign_one() {   # <artifact name> <path to sign> <main Mach-O> [bundle]
	local n="$1" p="$2" log="$LOGS/sign-${1// /-}.log"
	"$RCODESIGN" sign "$p" >>"$log" 2>&1 || die "rcodesign could not sign $n ($(tail -1 "$log"); $log); nothing ships unsigned"
	signed_ok "$3" "${4:-}" || die "$n: the signature does not verify ($(tail -1 "$LOGS/signatures.log"))"
	SIGNED+=(--signed "$n"); echo "  signed $n (ad-hoc, rcodesign; verified)"
}
for n in PG106FB PG106Accel; do
	[ -e "$STAGE/$n.kext" ] && sign_one "$n.kext" "$STAGE/$n.kext" "$STAGE/$n.kext/Contents/MacOS/$n" "$STAGE/$n.kext"
done
if [ -e "$STAGE/NVMTLDriver.bundle" ]; then
	# The nested libLLVM first (its own identifier), then the bundle, whose CodeResources seals it.
	"$RCODESIGN" sign --binary-identifier org.llvm.libLLVM "$STAGE/NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib" \
		>"$LOGS/sign-NVMTLDriver.bundle.log" 2>&1 || die "rcodesign could not sign air-runtime/libLLVM.dylib ($LOGS/sign-NVMTLDriver.bundle.log)"
	# check-adhoc-signature.py --bundle also verifies the nested libLLVM's own pages and its cdhash in CodeResources
	sign_one NVMTLDriver.bundle "$STAGE/NVMTLDriver.bundle" "$STAGE/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver" "$STAGE/NVMTLDriver.bundle"
fi
[ -e "$STAGE/libnvmtl_translate.dylib" ] && sign_one libnvmtl_translate.dylib "$STAGE/libnvmtl_translate.dylib" "$STAGE/libnvmtl_translate.dylib"
if [ -f "$STAGE/NVK-Pascal/libvulkan_nouveau.dylib" ]; then
	"$RCODESIGN" sign --binary-identifier libvulkan_nouveau "$STAGE/NVK-Pascal/libvulkan_nouveau.dylib" >"$LOGS/sign-nvk.log" 2>&1 ||
		die "rcodesign could not sign NVK-Pascal/libvulkan_nouveau.dylib ($LOGS/sign-nvk.log)"
	signed_ok "$STAGE/NVK-Pascal/libvulkan_nouveau.dylib" || die "NVK-Pascal: the signature does not verify ($(tail -1 "$LOGS/signatures.log"))"
	SIGNED+=(--signed NVK-Pascal); echo "  signed NVK-Pascal (ad-hoc, rcodesign; verified)"
fi

if [ -e "$STAGE/Helios 300 Setup.app" ]; then
	# The nested CLI first (its own identifier), then the bundle, whose CodeResources seals it by cdhash.
	"$RCODESIGN" sign --binary-identifier org.helios300.setup.cli "$STAGE/Helios 300 Setup.app/Contents/MacOS/helios-setup" \
		>"$LOGS/sign-setupapp.log" 2>&1 || die "rcodesign could not sign Helios 300 Setup.app's helios-setup ($LOGS/sign-setupapp.log)"
	sign_one "Helios 300 Setup.app" "$STAGE/Helios 300 Setup.app" "$STAGE/Helios 300 Setup.app/Contents/MacOS/HeliosSetup" \
		"$STAGE/Helios 300 Setup.app"
fi

DRY_ARG=(); [ "$DRY" = 1 ] && DRY_ARG=(--dry-run)
python3 -I "$HERE/release/make_manifest.py" --spec "$SELF/tools/release/spec.json" --schema "$SELF/tools/release/manifest.schema.json" \
	--stage "$STAGE" --dist "$DIST" --version "$VERSION" --self-commit "$SELF_COMMIT" --driver-commit "$DRIVER_COMMIT" \
	--wifi-country "$WIFI_CC" --driver-tree "$DRV" "${KC_ARG[@]}" "${DRY_ARG[@]}" "${SIGNED[@]}" "${MISSING[@]}" || exit 1
bash "$HERE/release/sign-manifest.sh" "$DIST/manifest.json" || die "the manifest could not be signed"
[ "$FAILED" = 0 ] || echo "release.sh: some builds failed (see $LOGS); the manifest lists them under release.missing"
echo "release.sh: dist/ ready: $DIST (nothing was published)"
