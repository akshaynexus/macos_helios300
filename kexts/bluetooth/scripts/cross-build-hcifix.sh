#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build QCARomeHCIFix.kext (Lilu plugin) for macOS 26 x86_64 on Linux. A Lilu plugin starts from
# kmod_info (Lilu's plugin_start.cpp), which tools/cross-build-kext.sh's generated kmod_info cannot
# do, so this is the same recipe as tools/release/cross-nmhda.sh, against the macOS 26.5 SDK.
# Usage: cross-build-hcifix.sh [out dir] (default build/cross). Environment as cross-build-kext.sh,
# plus RELEASE_CACHE for the pinned Lilu SDK (default ~/.cache/helios300-release).
# Exit: 0 built and audited, 1 failure, 77 tools missing.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
TOOLS="$REPO/tools"
OUT="${1:-$HERE/build/cross}"
SRC="$HERE/kext/QCARomeHCIFix"
CROSS="${HELIOS_CROSS:-$HOME/.local/share/helios300-cross}"
SDK="${MACOS_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk}"
LD64="${LD64:-$CROSS/cctools/bin/x86_64-apple-darwin17-ld}"
XNU_CONFIG="${XNU_CONFIG:-$CROSS/xnu-12377.1.9/config}"
CACHE="${RELEASE_CACHE:-$HOME/.cache/helios300-release}"
MIN=26.0
LILU_VER=1.7.2
LILU_ZIP="Lilu-$LILU_VER-DEBUG.zip"
LILU_URL="https://github.com/acidanthera/Lilu/releases/download/$LILU_VER/$LILU_ZIP"
LILU_SHA=e95df95d82e8b151047abf359ecd9c853755c59293e19dfcc3907da8fbd8cf89

skip() { echo "cross-build-hcifix: SKIP — $*"; exit 77; }
die() { echo "cross-build-hcifix: FAIL — $*"; exit 1; }
[ -x "$LD64" ] || skip "Apple ld64 not found at $LD64"
[ -f "$SDK/usr/lib/libkmod.a" ] || skip "macOS SDK not found at $SDK"
[ -d "$XNU_CONFIG" ] || skip "xnu config not found at $XNU_CONFIG"
command -v clang >/dev/null && command -v llvm-nm >/dev/null || skip "clang/llvm-nm missing"
SDK_VER="$(grep -oE '"Version":"[0-9.]+"' "$SDK/SDKSettings.json" | head -1 | cut -d'"' -f4)"
KF="$SDK/System/Library/Frameworks/Kernel.framework/Headers"

mkdir -p "$CACHE"
if [ ! -f "$CACHE/$LILU_ZIP" ] || [ "$(sha256sum "$CACHE/$LILU_ZIP" | cut -d' ' -f1)" != "$LILU_SHA" ]; then
	curl -fsSL -o "$CACHE/$LILU_ZIP.part" "$LILU_URL" || die "download $LILU_URL"
	[ "$(sha256sum "$CACHE/$LILU_ZIP.part" | cut -d' ' -f1)" = "$LILU_SHA" ] || { rm -f "$CACHE/$LILU_ZIP.part"; die "$LILU_ZIP sha256 mismatch"; }
	mv "$CACHE/$LILU_ZIP.part" "$CACHE/$LILU_ZIP"
fi

BUNDLE="$OUT/QCARomeHCIFix.kext"
WORK="$OUT/.hcifix-work"; rm -rf "$WORK" "$BUNDLE"; mkdir -p "$WORK/lilu" "$BUNDLE/Contents/MacOS"
python3 -I -m zipfile -e "$CACHE/$LILU_ZIP" "$WORK/lilu" >/dev/null
L="$WORK/lilu/Lilu.kext"; LSDK="$L/Contents/Resources"
[ -f "$LSDK/Headers/plugin_start.hpp" ] && [ -f "$LSDK/Library/plugin_start.cpp" ] || die "no Lilu SDK in $LILU_ZIP"
plist_get() { python3 -I -c 'import plistlib,sys; print(plistlib.load(open(sys.argv[1],"rb"))[sys.argv[2]])' "$SRC/Info.plist" "$1"; }
BID="$(plist_get CFBundleIdentifier)"; BVER="$(plist_get CFBundleVersion)"

COMMON=(-target "x86_64-apple-macos$MIN" -isysroot "$SDK" -c -O2 -g0 -mkernel -nostdinc -fno-builtin -fno-common
	-fno-stack-protector -mllvm --disable-atexit-based-global-dtor-lowering
	-I"$KF" -I"$LSDK" -I"$HERE/src/core" -include "$TOOLS/xnu-cross-compat.h"
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -D__ACIDANTHERA_MAC_SDK=260000
	-DPRODUCT_NAME=QCARomeHCIFix "-DMODULE_VERSION=$BVER"
	-Wall -Wno-unused-parameter -Wno-deprecated-declarations -Wno-inconsistent-missing-override)
CXX=(-x c++ -std=c++17 -nostdinc++ -fapple-kext -fno-rtti -fno-exceptions)
grep -qF "KMOD_EXPLICIT_DECL($BID, \"$BVER\"," "$SRC/QCARomeHCIFix.cpp" || die "KMOD_EXPLICIT_DECL does not match Info.plist ($BID $BVER)"
clang "${COMMON[@]}" "${CXX[@]}" -Wextra -Werror -Wno-unused-parameter \
	"$SRC/QCARomeHCIFix.cpp" -o "$WORK/hcifix.o" || die "compile QCARomeHCIFix.cpp"
echo "PASS compile QCARomeHCIFix.cpp"
clang "${COMMON[@]}" -x c -std=gnu11 -Wextra -Werror "$HERE/src/core/qca_hci_filter.c" -o "$WORK/filter.o" || die "compile qca_hci_filter.c"
echo "PASS compile qca_hci_filter.c"
# Lilu's bundled startup source carries its own conversion and shadow warnings.
clang "${COMMON[@]}" "${CXX[@]}" -Wno-sign-conversion -Wno-shadow "$LSDK/Library/plugin_start.cpp" -o "$WORK/plugin_start.o" || die "compile plugin_start.cpp"
echo "PASS compile plugin_start.cpp (Lilu $LILU_VER SDK)"

BIN="$BUNDLE/Contents/MacOS/QCARomeHCIFix"
"$LD64" -arch x86_64 -platform_version macos "$MIN" "$SDK_VER" -syslibroot "$SDK" -kext -static -o "$BIN" \
	"$WORK/hcifix.o" "$WORK/filter.o" "$WORK/plugin_start.o" -L"$SDK/usr/lib" -lkmodc++ -lkmod \
	2> >(grep -v -e "directory not found" -e "was built for newer macOS version" >&2) || die "link"
cp "$SRC/Info.plist" "$BUNDLE/Contents/Info.plist"
mkdir -p "$BUNDLE/Contents/Resources"; cp "$HERE/LICENSE" "$BUNDLE/Contents/Resources/"
[ "$(od -An -tu4 -j12 -N4 "$BIN" | tr -d ' ')" = 11 ] || die "not MH_KEXT_BUNDLE"
SYMS="$(llvm-nm "$BIN")"
grep -q " _kmod_info$" <<<"$SYMS" || die "no _kmod_info"
echo "PASS link MH_KEXT_BUNDLE, macOS $MIN, SDK $SDK_VER"

llvm-nm -u "$BIN" | sed 's/^ *U //' | sort -u >"$WORK/imports.txt"
llvm-nm -gU "$L/Contents/MacOS/Lilu" | awk '{print "sym as.vit9696.Lilu " $3}' >"$WORK/lilu.allow"
python3 -I "$TOOLS/kpi-audit.py" --xnu-config "$XNU_CONFIG" --sdk-headers "$KF" --plist "$BUNDLE/Contents/Info.plist" \
	--imports "$WORK/imports.txt" --allow "$WORK/lilu.allow" --allow "$HERE/kext/QCARomePatchRAM/kpi-allow.txt" || die "KPI audit"

if [ -n "${HELIOS_REAL_KC:-}" ]; then
	KCARGS=()
	IFS=: read -r -a kcs <<<"$HELIOS_REAL_KC"
	for kc in "${kcs[@]}"; do [ -f "$kc" ] || die "HELIOS_REAL_KC: $kc not found"; KCARGS+=(--kc "$kc"); done
	# kc-audit reads a thin kext; the Lilu release binary is universal.
	llvm-lipo -thin x86_64 "$L/Contents/MacOS/Lilu" -output "$WORK/Lilu.x86_64" || cp "$L/Contents/MacOS/Lilu" "$WORK/Lilu.x86_64"
	python3 -I "$TOOLS/kc-audit.py" "${KCARGS[@]}" --kext "$BIN" --plist "$BUNDLE/Contents/Info.plist" \
		--sibling "$WORK/Lilu.x86_64" || die "real-collection audit (kc-audit.py)"
	python3 -I "$HERE/scripts/check-virtual-slots.py" --tools "$TOOLS" --sdk "$SDK" --work "$WORK" "${KCARGS[@]}" || die "virtual slots differ from the SDK"
else
	echo "note: HELIOS_REAL_KC unset — imports/vtables not checked against a real kernel collection"
fi
rm -rf "$WORK"
echo "cross-build-hcifix: OK — $BUNDLE ($(sha256sum "$BIN" | cut -c1-16)…)"
