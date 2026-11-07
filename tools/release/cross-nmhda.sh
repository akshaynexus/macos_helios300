#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# cross-nmhda.sh <NMHDA source dir> <out dir> — NMHDA.kext (Lilu plugin) cross-built on Linux.
#
# The Linux equivalent of kexts/NMHDA/build.sh (which runs xcrun clang++ on a Mac):
#   - headers: acidanthera MacKernelSDK at the commit PG106 pins (3f750085) + the Lilu SDK
#     (Contents/Resources of Lilu 1.7.2's DEBUG release: plugin_start.hpp/.cpp, Headers/);
#   - clang -mkernel -fapple-kext with --disable-atexit-based-global-dtor-lowering (as PG106);
#   - _kmod_info comes from nmhda.cpp itself (start/stop = NMHDA_kern_start/stop, which Lilu's
#     plugin_start.cpp defines with ADDPR());
#   - Apple ld64 -kext -static with the macOS SDK's libkmod/libkmodc++. build.sh also links
#     -lcc_kext (compiler builtins); the link fails if a builtin is needed, which is the check;
#   - KPI audit (tools/kpi-audit.py): xnu exports for the com.apple.kpi.* it declares, every symbol
#     exported by that Lilu binary for as.vit9696.Lilu, IOPCIFamily by header.
# Environment as tools/cross-build-kext.sh (MACOS_SDK, LD64, XNU_CONFIG), plus
#   RELEASE_CACHE  download cache (default ~/.cache/helios300-release)
# Exit: 0 built + audited, 1 failure, 77 tools missing.
set -euo pipefail
SRC="$(cd "${1:?NMHDA source dir}" && pwd)"; OUT="${2:?out dir}"
HERE="$(cd "$(dirname "$0")" && pwd)"; TOOLS="$(cd "$HERE/.." && pwd)"
CROSS="${HELIOS_CROSS:-$HOME/.local/share/helios300-cross}"
SDK="${MACOS_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk}"
LD64="${LD64:-$CROSS/cctools/bin/x86_64-apple-darwin17-ld}"
XNU_CONFIG="${XNU_CONFIG:-$CROSS/xnu-12377.1.9/config}"
CACHE="${RELEASE_CACHE:-$HOME/.cache/helios300-release}"
MIN=15.0   # build.sh: -mmacosx-version-min=15.0 (one NMHDA for macOS 15 and 26)
LILU_VER=1.7.2
LILU_ZIP="Lilu-$LILU_VER-DEBUG.zip"
LILU_URL="https://github.com/acidanthera/Lilu/releases/download/$LILU_VER/$LILU_ZIP"
LILU_SHA=e95df95d82e8b151047abf359ecd9c853755c59293e19dfcc3907da8fbd8cf89
MKSDK_URL=https://github.com/acidanthera/MacKernelSDK
MKSDK_COMMIT=3f750085caa17ec3a7880f11c11bf4f48cd6a164

skip() { echo "cross-nmhda: SKIP — $*"; exit 77; }
die() { echo "cross-nmhda: FAIL — $*"; exit 1; }
[ -x "$LD64" ] || skip "Apple ld64 not found at $LD64"
[ -f "$SDK/usr/lib/libkmod.a" ] || skip "macOS SDK not found at $SDK"
[ -d "$XNU_CONFIG" ] || skip "xnu config not found at $XNU_CONFIG"
command -v clang >/dev/null && command -v llvm-nm >/dev/null || skip "clang/llvm-nm missing"
SDK_VER="$(grep -oE '"Version":"[0-9.]+"' "$SDK/SDKSettings.json" | head -1 | cut -d'"' -f4)"

mkdir -p "$CACHE"
if [ ! -f "$CACHE/$LILU_ZIP" ] || [ "$(sha256sum "$CACHE/$LILU_ZIP" | cut -d' ' -f1)" != "$LILU_SHA" ]; then
	curl -fsSL -o "$CACHE/$LILU_ZIP.part" "$LILU_URL" || die "download $LILU_URL"
	[ "$(sha256sum "$CACHE/$LILU_ZIP.part" | cut -d' ' -f1)" = "$LILU_SHA" ] || { rm -f "$CACHE/$LILU_ZIP.part"; die "$LILU_ZIP sha256 mismatch"; }
	mv "$CACHE/$LILU_ZIP.part" "$CACHE/$LILU_ZIP"
fi
MK="$CACHE/MacKernelSDK-$MKSDK_COMMIT"
if [ ! -d "$MK/Headers" ]; then
	rm -rf "$MK"; git init -q "$MK"
	git -C "$MK" fetch -q --depth 1 "$MKSDK_URL" "$MKSDK_COMMIT" && git -C "$MK" checkout -q FETCH_HEAD || die "fetch MacKernelSDK"
fi
[ "$(git -C "$MK" rev-parse HEAD)" = "$MKSDK_COMMIT" ] || die "MacKernelSDK cache is not at $MKSDK_COMMIT"

WORK="$OUT/.nmhda-work"; rm -rf "$WORK" "$OUT/NMHDA.kext"; mkdir -p "$WORK/lilu" "$OUT/NMHDA.kext/Contents/MacOS"
python3 -I -m zipfile -e "$CACHE/$LILU_ZIP" "$WORK/lilu" >/dev/null
L="$WORK/lilu/Lilu.kext"; LSDK="$L/Contents/Resources"
[ -f "$LSDK/Headers/plugin_start.hpp" ] && [ -f "$LSDK/Library/plugin_start.cpp" ] || die "no Lilu SDK in $LILU_ZIP"

BID="$(python3 -I -c 'import plistlib,sys; print(plistlib.load(open(sys.argv[1],"rb"))["CFBundleIdentifier"])' "$SRC/Info.plist")"
BVER="$(python3 -I -c 'import plistlib,sys; d=plistlib.load(open(sys.argv[1],"rb")); print(d.get("CFBundleVersion") or d["CFBundleShortVersionString"])' "$SRC/Info.plist")"
FLAGS=(-target "x86_64-apple-macos$MIN" -c -O2 -g0 -mkernel -nostdinc -nostdinc++ -fapple-kext -fno-builtin -fno-common
       -fno-stack-protector -mllvm --disable-atexit-based-global-dtor-lowering
       -I"$MK/Headers" -I"$LSDK"
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DPRODUCT_NAME=NMHDA "-DMODULE_VERSION=$BVER"
       -std=c++17 -fno-rtti -fno-exceptions -Wall -Wno-unused-parameter)
clang++ "${FLAGS[@]}" "$SRC/nmhda.cpp" -o "$WORK/nmhda.o" || die "compile nmhda.cpp"
echo "PASS compile nmhda.cpp"
# Lilu's bundled startup source carries its own conversion and shadow warnings (build.sh).
clang++ "${FLAGS[@]}" -Wno-sign-conversion -Wno-shadow "$LSDK/Library/plugin_start.cpp" -o "$WORK/plugin_start.o" || die "compile plugin_start.cpp"
echo "PASS compile plugin_start.cpp (Lilu $LILU_VER SDK)"
# nmhda.cpp carries its own KMOD_EXPLICIT_DECL(com.nullmoth.NMHDA, ..., NMHDA_kern_start, NMHDA_kern_stop),
# so no _kmod_info TU is generated here (the check after the link makes sure it is there).
BIN="$OUT/NMHDA.kext/Contents/MacOS/NMHDA"
"$LD64" -arch x86_64 -platform_version macos "$MIN" "$SDK_VER" -syslibroot "$SDK" -kext -static -o "$BIN" \
	"$WORK/nmhda.o" "$WORK/plugin_start.o" -L"$SDK/usr/lib" -lkmodc++ -lkmod \
	2> >(grep -v -e "directory not found" -e "was built for newer macOS version" >&2) || die "link (a compiler builtin from cc_kext would show here)"
cp "$SRC/Info.plist" "$OUT/NMHDA.kext/Contents/Info.plist"
[ "$(od -An -tu4 -j12 -N4 "$BIN" | tr -d ' ')" = 11 ] || die "not MH_KEXT_BUNDLE"
llvm-nm "$BIN" | grep -q " _kmod_info$" || die "no _kmod_info"
echo "PASS link MH_KEXT_BUNDLE, macOS $MIN, SDK $SDK_VER"
llvm-nm -u "$BIN" | sed 's/^ *U //' | sort -u >"$WORK/imports.txt"
llvm-nm -gU "$L/Contents/MacOS/Lilu" | awk '{print "sym as.vit9696.Lilu " $3}' >"$WORK/lilu.allow"
python3 -I "$TOOLS/kpi-audit.py" --xnu-config "$XNU_CONFIG" --sdk-headers "$SDK/System/Library/Frameworks/Kernel.framework/Headers" \
	--plist "$OUT/NMHDA.kext/Contents/Info.plist" --imports "$WORK/imports.txt" --allow "$WORK/lilu.allow" \
	--allow "$HERE/nmhda-families.allow" || die "KPI audit"
rm -rf "$WORK"
echo "cross-nmhda: OK — $OUT/NMHDA.kext ($(sha256sum "$BIN" | cut -c1-16)…)"
