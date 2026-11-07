#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# cross-build-app.sh — build "Ath10k Connectivity.app" (Objective-C++/Cocoa) for
# macOS 13+ x86_64 on Linux: clang -target x86_64-apple-macos13.0 against the
# macOS 26.5 SDK, linked by Apple ld64 against the SDK's .tbd stubs.
#
# Differences from scripts/build-connectivity.sh (the macOS-host build):
#   * deployment target 13.0 (was 11.0): the app's only @available checks are for
#     macOS 13, so they fold to compile-time true and no compiler-rt
#     ___isPlatformVersionAtLeast is needed (Linux has no Apple compiler-rt);
#   * no app icon (generate-app-icon.swift + iconutil exist only on macOS);
#   * not signed. Run `codesign --force --sign - "Ath10k Connectivity.app"` on the
#     Mac before first launch (README "Install").
#
# Usage: scripts/cross-build-app.sh [OUTDIR]     (default build/release)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/release}"
CROSS="${HELIOS_CROSS:-$HOME/.local/share/helios300-cross}"
SDK="${MACOS_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk}"
LD64="${LD64:-$CROSS/cctools/bin/x86_64-apple-darwin17-ld}"
CLANGXX="${CLANGXX:-clang++}"
NM="${NM:-llvm-nm}"
MIN=13.0
APP="$OUT/Ath10k Connectivity.app"
[ -x "$LD64" ] || { echo "cross-build-app: SKIP — no ld64 at $LD64"; exit 77; }
[ -d "$SDK/System/Library/Frameworks/Cocoa.framework" ] || { echo "cross-build-app: SKIP — no SDK at $SDK"; exit 77; }
SDK_VER="$(grep -oE '"Version":"[0-9.]+"' "$SDK/SDKSettings.json" | head -1 | cut -d'"' -f4)"

WORK="$OUT/.app.obj"
rm -rf "$APP" "$WORK"; mkdir -p "$WORK" "$APP/Contents/MacOS" "$APP/Contents/Resources"
"$CLANGXX" -target "x86_64-apple-macos$MIN" -isysroot "$SDK" -std=c++17 -fobjc-arc -O2 \
	-Wall -Wextra -Werror -Wno-deprecated-declarations -c "$ROOT/app/Main.mm" -o "$WORK/Main.o"
"$LD64" -arch x86_64 -platform_version macos "$MIN" "$SDK_VER" -syslibroot "$SDK" -dead_strip \
	-o "$APP/Contents/MacOS/Ath10kConnectivity" "$WORK/Main.o" \
	-framework Cocoa -framework IOKit -framework Security -framework ServiceManagement \
	-lobjc -lc++ -lSystem
rm -rf "$WORK"
# Everything must come from a system dylib/framework: no compiler-rt or other
# toolchain-only symbol may be left for dyld to fail on.
if "$NM" -u "$APP/Contents/MacOS/Ath10kConnectivity" | grep -E '___isPlatformVersionAtLeast|___isOSVersionAtLeast|___clang_|___builtin'; then
	echo "cross-build-app: FAIL — toolchain runtime symbols left undefined"; exit 1
fi
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>br.com.jonasw8.Ath10kConnectivity</string>
<key>CFBundleName</key><string>Ath10k Connectivity</string>
<key>CFBundleExecutable</key><string>Ath10kConnectivity</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.1.13</string>
<key>CFBundleVersion</key><string>9</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>LSUIElement</key><true/>
</dict></plist>
PLIST
cp "$ROOT/LICENSE" "$APP/Contents/Resources/LICENSE"
echo "cross-build-app: OK — $APP (unsigned; ad-hoc sign on the Mac)"
