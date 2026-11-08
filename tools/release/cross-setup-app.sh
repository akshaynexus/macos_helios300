#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# cross-setup-app.sh <self tree> <driver tree> <out dir> <version> — "Helios 300 Setup.app" cross-built on Linux.
#
#   1. swift format lint (apps/HeliosSetup/.swift-format) and swift test of the core (HeliosCore) on Linux: a broken
#      app does not ship (SKIP_TESTS=1 skips both; release.sh never does). swift-crypto (Package.resolved pins it) is
#      a Linux-only dependency for those tests; the Mac build uses CryptoKit and links nothing from it.
#   2. swift build -c release for x86_64-apple-macosx (minos 26.0) with the darwin Swift SDK xtool installed
#      (~/.config/swiftpm/swift-sdks/darwin.artifactbundle; SwiftPM's native build system: the default one cannot
#      find the macOS platform in that SDK). Products: HeliosSetup (the AppKit app) and helios-setup (the CLI).
#   3. The bundle: Contents/MacOS/{HeliosSetup,helios-setup}, Info.plist, Resources/ with the release scripts
#      (tools/macos/*.sh of the self tree, gpu-apps.sh of the driver tree, checked against the manifest at run time),
#      spec.json, the PolyForm licence for gpu-apps.sh and NOTICE.txt.
#   4. Checks: both Mach-O x86_64 executables, minos 26.0, every undefined symbol bound to an SDK library
#      (the driver's nvk/pascal/scripts/macho-check.py).
# The sources are copied to a fixed cache path first ($RELEASE_CACHE/setup-app/src), so paths in the binaries do not
# depend on release.sh's temp dir and SwiftPM's build cache is reused. JOBS (default nproc) builds in parallel.
# Signing is release.sh's (the nested helios-setup first, then the bundle); SIGN=1 does the same here for a local build.
# Exit: 0 built + checked, 1 failure, 77 tools missing.
set -euo pipefail
SELF="$(cd "${1:?self tree}" && pwd)"; DRV="$(cd "${2:?driver tree}" && pwd)"; OUT="${3:?out dir}"; VER="${4:?version}"
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"; LOG="$OUT/setup-app"
CACHE="${RELEASE_CACHE:-$HOME/.cache/helios300-release}/setup-app"
JOBS="${JOBS:-$(nproc)}"
SDKB="${SWIFT_DARWIN_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle}"
SDK="${MACOS_SDK:-$SDKB/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk}"
MACHO_CHECK="${MACHO_CHECK:-$DRV/nvk/pascal/scripts/macho-check.py}"
APP_NAME="Helios 300 Setup.app"
BUNDLE_ID=org.helios300.setup
MIN=26.0
skip() { echo "cross-setup-app: SKIP — $*"; exit 77; }
die() { echo "cross-setup-app: FAIL — $*"; exit 1; }
command -v swift >/dev/null || skip "swift (the Swift toolchain) not found"
[ -d "$SDKB" ] || skip "no darwin Swift SDK at $SDKB (xtool setup installs it)"
command -v llvm-objdump >/dev/null || skip "llvm-objdump not found"
command -v rsync >/dev/null || skip "rsync not found"
[ -d "$SELF/apps/HeliosSetup" ] || die "no apps/HeliosSetup in $SELF"
[ -f "$DRV/tools/macos/gpu-apps.sh" ] || die "no tools/macos/gpu-apps.sh in the driver tree"

# 1. the sources at a fixed path
SRC="$CACHE/src"
mkdir -p "$SRC"
mkdir -p "$SRC/apps"
rsync -a --delete --exclude .build "$SELF/apps/HeliosSetup/" "$SRC/apps/HeliosSetup/"
mkdir -p "$SRC/tools/release" "$SRC/tools/macos"
cp "$SELF"/tools/release/{spec.json,sign-manifest.sh,manifest-ed25519.pub} "$SRC/tools/release/"
cp "$SELF"/tools/macos/*.sh "$SRC/tools/macos/"
cd "$SRC/apps/HeliosSetup"

# 2. tests on Linux (the core and its fixtures; tests read ../../tools/release/{spec.json,sign-manifest.sh,manifest-ed25519.pub}
#    and ../../tools/macos/*.sh)
if [ "${SKIP_TESTS:-0}" != 1 ]; then
	if swift format --version >/dev/null 2>&1; then
		swift format lint --strict --configuration .swift-format -r Sources Tests >"$LOG.format.log" 2>&1 ||
			{ head -20 "$LOG.format.log"; die "swift format lint (apps/HeliosSetup/.swift-format; swift format format -i fixes it)"; }
		echo "PASS swift format lint"
	fi
	swift test -j "$JOBS" --scratch-path "$CACHE/linux" >"$LOG.test.log" 2>&1 || { tail -30 "$LOG.test.log"; die "swift test (log: $LOG.test.log)"; }
	echo "PASS swift test: $(grep -Eo 'Executed [0-9]+ tests, with 0 failures' "$LOG.test.log" | tail -1)"
fi

# 3. the macOS build
swift build -c release -j "$JOBS" --swift-sdk darwin --triple x86_64-apple-macosx --build-system native \
	--scratch-path "$CACHE/darwin" -Xswiftc -file-prefix-map -Xswiftc "$SRC=/HeliosSetup" >"$LOG.build.log" 2>&1 ||
	{ grep -E ' error: ' "$LOG.build.log" | sort -u | head -30; die "swift build (log: $LOG.build.log)"; }
BIN="$CACHE/darwin/x86_64-apple-macosx/release"
echo "PASS swift build ($(grep -c ' warning: ' "$LOG.build.log" || true) warnings)"

# 4. the bundle
APP="$OUT/$APP_NAME"; rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN/HeliosSetup" "$BIN/helios-setup" "$APP/Contents/MacOS/"
chmod 0755 "$APP/Contents/MacOS/"*
for f in "$SELF"/tools/macos/audio-voodoohda.sh "$SELF"/tools/macos/audio-applehda.sh "$SELF"/tools/macos/install-gpu.sh \
	"$DRV/tools/macos/gpu-apps.sh"; do
	bash -n "$f"
	cp "$f" "$APP/Contents/Resources/"
done
chmod 0755 "$APP/Contents/Resources/"*.sh
cp "$SELF/tools/release/spec.json" "$APP/Contents/Resources/spec.json"
cp "$DRV/LICENSE" "$APP/Contents/Resources/PolyForm-Noncommercial-1.0.0.txt"
cat >"$APP/Contents/Resources/NOTICE.txt" <<'NOTICE'
Helios 300 Setup (macos_helios300 apps/HeliosSetup): GPL-2.0-only.
Contents/Resources/audio-voodoohda.sh, audio-applehda.sh, install-gpu.sh: macos_helios300 tools/macos, GPL-2.0-only.
Contents/Resources/gpu-apps.sh: nvidia-macos-driver tools/macos, PolyForm-Noncommercial-1.0.0
  (PolyForm-Noncommercial-1.0.0.txt next to it).
The manifest checks follow 1401's p1401/helios300.py (akshaynexus/1401); the OpenCore install and update flow
take their shape from OpenCore Legacy Patcher (support/install.py, support/updates.py; BSD-3-Clause,
Copyright (c) 2020-2025 Dhinak G, Mykola Grymalyuk and contributors); no OCLP code is included.
Source: https://github.com/akshaynexus/macos_helios300
NOTICE
cat >"$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleDisplayName</key>
	<string>Helios 300 Setup</string>
	<key>CFBundleExecutable</key>
	<string>HeliosSetup</string>
	<key>CFBundleIdentifier</key>
	<string>$BUNDLE_ID</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>Helios 300 Setup</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>$VER</string>
	<key>CFBundleVersion</key>
	<string>$VER</string>
	<key>LSApplicationCategoryType</key>
	<string>public.app-category.utilities</string>
	<key>LSMinimumSystemVersion</key>
	<string>$MIN</string>
	<key>NSAppleEventsUsageDescription</key>
	<string>Helios 300 Setup asks the system to restart when you click Restart now, and asks for the administrator password for the steps that change the Mac.</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSHumanReadableCopyright</key>
	<string>macos_helios300 contributors. GPL-2.0-only.</string>
	<key>NSPrincipalClass</key>
	<string>NSApplication</string>
</dict>
</plist>
PLIST

# 5. checks
for exe in HeliosSetup helios-setup; do
	f="$APP/Contents/MacOS/$exe"
	hdr="$(llvm-objdump --macho --private-header "$f")"
	grep -q X86_64 <<<"$hdr" && grep -q EXECUTE <<<"$hdr" || die "$exe is not an x86_64 executable"
	minos="$(llvm-objdump --macho --all-headers "$f" | awk '/LC_BUILD_VERSION/{b=1} b && /minos/ && !d {print $2; d=1}')"
	[ "$minos" = "$MIN" ] || die "$exe minos $minos, want $MIN"
	if [ -f "$MACHO_CHECK" ] && python3 -c 'import yaml' 2>/dev/null; then
		python3 "$MACHO_CHECK" "$f" "$SDK" >"$LOG.$exe.macho-check.txt" 2>&1 || { tail -20 "$LOG.$exe.macho-check.txt"; die "macho-check $exe"; }
	else
		skip "macho-check.py or PyYAML missing (every symbol must be checked against the SDK)"
	fi
done
echo "PASS Mach-O: HeliosSetup + helios-setup, x86_64, minos $MIN, every symbol bound to the SDK"

if [ "${SIGN:-0}" = 1 ]; then
	RCODESIGN="${RCODESIGN:-$(command -v rcodesign || echo "$HOME/.cargo/bin/rcodesign")}"
	"$RCODESIGN" sign --binary-identifier "$BUNDLE_ID.cli" "$APP/Contents/MacOS/helios-setup" >"$LOG.sign.log" 2>&1 || die "rcodesign helios-setup"
	"$RCODESIGN" sign "$APP" >>"$LOG.sign.log" 2>&1 || die "rcodesign the bundle"
	python3 -I "$SELF/tools/release/check-adhoc-signature.py" "$APP/Contents/MacOS/HeliosSetup" --bundle "$APP" || die "signature"
	echo "PASS signed (ad-hoc, rcodesign; verified)"
fi
echo "cross-setup-app: OK — $APP ($(sha256sum "$APP/Contents/MacOS/HeliosSetup" | cut -c1-16)…)"
