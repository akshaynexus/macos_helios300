#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# cross-build-kext.sh — build a macOS 26 (x86_64) kext bundle on Linux.
#
# Compiles each source with clang against the macOS SDK's Kernel.framework
# (kernel code generation as Apple clang does for -mkernel/-fapple-kext),
# generates the _kmod_info TU Xcode would generate, links an MH_KEXT_BUNDLE
# with Apple's ld64 (cctools-port; LLVM's ld64.lld has no -kext), assembles
# the bundle and runs tools/kpi-audit.py over every imported symbol.
#
# Adapted from nvidia-macos-driver pascal/kext/scripts/cross-build-macos.sh.
#
# It proves the kext compiles, links, has _kmod_info and imports only symbols
# that the macOS 26 kernel (xnu export lists) or a declared family exports.
# It does NOT prove the kext loads or works — that needs macOS.
#
# Usage:
#   tools/cross-build-kext.sh --plist Info.plist --out build/X.kext \
#       [--exec NAME] [-I DIR]... [-D NAME[=VAL]]... [--cflags "..."] \
#       [--cxxflags "..."] [--allow FILE]... [--resources DIR] \
#       [--no-audit] SOURCE.{c,cpp}...
#
#   --plist      Info.plist copied into the bundle; CFBundleIdentifier and
#                CFBundleVersion feed _kmod_info; CFBundleExecutable names
#                the binary (unless --exec).
#   --out        bundle path (recreated).
#   -I / -D      extra include dirs / defines for every TU (repeatable;
#                use for generated headers outside the source tree).
#   --cflags     extra flags for every TU; --cxxflags only for C++ TUs.
#   --allow      kpi-audit allowlist for non-xnu families (repeatable,
#                format in tools/kpi-audit.py).
#   --resources  directory copied to Contents/Resources.
#   --kc-sibling binary of a kext built alongside whose exports resolve this
#                kext's imports in the real-collection check (repeatable).
#
# Environment:
#   MACOS_SDK   MacOSX26.x.sdk with Kernel.framework + usr/lib/libkmod.a
#   LD64        Apple ld64 (x86_64-apple-darwinNN-ld)
#   MACOS_MIN   deployment target (default 26.0)
#   XNU_CONFIG  xnu config/ directory with *.exports (default xnu-12377.1.9)
#   CLANG, NM   (default clang, llvm-nm)
#   HELIOS_REAL_KC  BootKernelExtensions.kc of the Mac that will run the kext.
#               When set, tools/kc-audit.py checks every import against the
#               collection's exports and every vtable against the real parent
#               layout (what kxld/OpenCore patch at injection). Unset: skipped.
#
# Exit: 0 built + audit passed; 1 failure; 77 tools missing.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CROSS="${HELIOS_CROSS:-$HOME/.local/share/helios300-cross}"
SDKS="$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs"
SDK="${MACOS_SDK:-$SDKS/MacOSX26.5.sdk}"
LD64="${LD64:-$CROSS/cctools/bin/x86_64-apple-darwin17-ld}"
MIN="${MACOS_MIN:-26.0}"
XNU_CONFIG="${XNU_CONFIG:-$CROSS/xnu-12377.1.9/config}"
CLANG="${CLANG:-clang}"
NM="${NM:-llvm-nm}"

skip() { echo "cross-build-kext: SKIP — $*"; exit 77; }
die() { echo "cross-build-kext: FAIL — $*"; exit 1; }

PLIST="" OUT="" EXEC="" RES="" AUDIT=1
EXTRA=() CXXEXTRA=() ALLOW=() SRCS=() KCSIBLING=()
while [ $# -gt 0 ]; do
	case "$1" in
	--plist) PLIST="$2"; shift 2 ;;
	--out) OUT="$2"; shift 2 ;;
	--exec) EXEC="$2"; shift 2 ;;
	--resources) RES="$2"; shift 2 ;;
	--allow) ALLOW+=(--allow "$2"); shift 2 ;;
	--cflags) read -r -a f <<<"$2"; EXTRA+=("${f[@]}"); shift 2 ;;
	--cxxflags) read -r -a f <<<"$2"; CXXEXTRA+=("${f[@]}"); shift 2 ;;
	--no-audit) AUDIT=0; shift ;;
	--kc-sibling) KCSIBLING+=(--sibling "$2"); shift 2 ;;
	-I) EXTRA+=(-I "$2"); shift 2 ;;
	-I*) EXTRA+=("$1"); shift ;;
	-D) EXTRA+=(-D "$2"); shift 2 ;;
	-D*) EXTRA+=("$1"); shift ;;
	-h|--help) sed -n '2,40p' "$0"; exit 0 ;;
	-*) die "unknown option $1" ;;
	*) SRCS+=("$1"); shift ;;
	esac
done
[ -n "$PLIST" ] && [ -f "$PLIST" ] || die "--plist missing"
[ -n "$OUT" ] || die "--out missing"
[ "${#SRCS[@]}" -gt 0 ] || die "no sources"

command -v "$CLANG" >/dev/null || skip "clang not found"
command -v "$NM" >/dev/null || skip "llvm-nm not found (set NM)"
[ -x "$LD64" ] || skip "Apple ld64 not found at $LD64 (set LD64)"
KF="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[ -d "$KF/IOKit" ] && [ -f "$SDK/usr/lib/libkmod.a" ] ||
	skip "macOS SDK with Kernel.framework not found at $SDK (set MACOS_SDK)"
SDK_VER="$(grep -oE '"Version":"[0-9.]+"' "$SDK/SDKSettings.json" | head -1 | cut -d'"' -f4)"

plist_get() {
	python3 -I -c 'import plistlib,sys; d=plistlib.load(open(sys.argv[1],"rb")); print(d.get(sys.argv[2]) or "")' "$PLIST" "$1"
}
BID="$(plist_get CFBundleIdentifier)"
BVER="$(plist_get CFBundleVersion)"; [ -n "$BVER" ] || BVER="$(plist_get CFBundleShortVersionString)"
[ -n "$EXEC" ] || EXEC="$(plist_get CFBundleExecutable)"
[ -n "$BID" ] && [ -n "$BVER" ] && [ -n "$EXEC" ] || die "Info.plist lacks CFBundleIdentifier/CFBundleVersion/CFBundleExecutable"
case "$BVER" in *[!0-9.]*) die "CFBundleVersion '$BVER' must be numeric (kmod/kxld parse it)" ;; esac

WORK="$(dirname "$OUT")/.$(basename "$OUT" .kext).obj"
rm -rf "$OUT" "$WORK"; mkdir -p "$WORK" "$OUT/Contents/MacOS"

# xnu-cross-compat.h fixes the SDK's fortify-trap asm for upstream clang.
# Upstream LLVM lowers llvm.global_dtors to __cxa_atexit on Mach-O; the kernel
# exports no __cxa_atexit, so keep destructors in __mod_term_func (run by
# libkmod's _stop) with --disable-atexit-based-global-dtor-lowering.
COMMON=(
	-target "x86_64-apple-macos$MIN" -isysroot "$SDK"
	-c -O2 -g0 -mkernel -nostdinc -fno-builtin -fno-common -fno-stack-protector
	-mllvm --disable-atexit-based-global-dtor-lowering
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
	"${EXTRA[@]}"
	-I "$KF"
	-include "$HERE/xnu-cross-compat.h"
	-Wall -Wno-unused-parameter -Wno-deprecated-declarations
	-Wno-inconsistent-missing-override
)
# Translation units compile in parallel (JOBS, default nproc). Object names, and so the link order, are the
# same as a serial build. Each compiler's output goes to its own log, printed in source order.
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
fail=0 n=0 pids=() srcs=() logs=()
for src in "${SRCS[@]}"; do
	n=$((n + 1))
	obj="$WORK/$n-$(basename "${src%.*}").o"
	case "$src" in
	*.cpp|*.cc) lang=(-x c++ -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti -nostdinc++ "${CXXEXTRA[@]}") ;;
	*.c) lang=(-x c -std=gnu11) ;;
	*) echo "FAIL $src: unknown source type"; fail=1; continue ;;
	esac
	while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n || true; done
	"$CLANG" "${COMMON[@]}" "${lang[@]}" "$src" -o "$obj" >"$obj.log" 2>&1 &
	pids+=("$!"); srcs+=("$src"); logs+=("$obj.log")
done
for i in "${!pids[@]}"; do
	src="${srcs[$i]}"; log="${logs[$i]}"
	if wait "${pids[$i]}"; then
		cat "$log"; echo "PASS compile $(basename "$src")"
	else
		cat "$log"; echo "FAIL compile $src"; fail=1
	fi
	rm -f "$log"
done
[ "$fail" = 0 ] || die "compile failed"

# _kmod_info: Xcode generates this TU for kext targets; without it kmutil
# silently leaves the kext out of the kernel collection. _start/_stop come
# from libkmod (c_start.o); _realmain/_antimain = 0 as for IOKit-only kexts.
# The struct is spelled out (KMOD_EXPLICIT_DECL stringifies a token, which
# would mangle bundle IDs containing '-').
cat >"$WORK/kmod_info.c" <<KMOD
#include <mach/mach_types.h>
#include <mach/kmod.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) kmod_info_t kmod_info = {
	0, KMOD_INFO_VERSION, -1U, { "$BID" }, { "$BVER" }, -1, 0, 0, 0, 0, _start, _stop
};
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = 0;
KMOD
"$CLANG" "${COMMON[@]}" -x c -std=gnu11 "$WORK/kmod_info.c" -o "$WORK/0-kmod_info.o" ||
	die "compile kmod_info.c"
echo "PASS compile kmod_info.c ($BID $BVER)"

BIN="$OUT/Contents/MacOS/$EXEC"
"$LD64" -arch x86_64 -platform_version macos "$MIN" "$SDK_VER" -syslibroot "$SDK" \
	-kext -static -o "$BIN" "$WORK"/*.o \
	-L"$SDK/usr/lib" -lkmodc++ -lkmod 2> >(grep -v -e "directory not found" -e "libkmod.*was built for newer macOS version" >&2) ||
	die "link"
cp "$PLIST" "$OUT/Contents/Info.plist"
if [ -n "$RES" ]; then mkdir -p "$OUT/Contents/Resources"; cp -R "$RES"/. "$OUT/Contents/Resources/"; fi
rm -rf "$WORK"

filetype="$(od -An -tu4 -j12 -N4 "$BIN" | tr -d ' ')"
[ "$filetype" = 11 ] || die "Mach-O filetype $filetype, want 11 (MH_KEXT_BUNDLE)"
echo "PASS link MH_KEXT_BUNDLE x86_64, macOS $MIN, SDK $SDK_VER"
"$NM" "$BIN" | grep -q " _kmod_info$" || die "no _kmod_info (kmutil would skip the kext)"
echo "PASS _kmod_info present; $("$NM" "$BIN" | grep -c 'gMetaClass' || true) metaclass symbols"

if [ "$AUDIT" = 1 ]; then
	[ -d "$XNU_CONFIG" ] || skip "built, but KPI audit needs xnu config at $XNU_CONFIG (set XNU_CONFIG)"
	"$NM" -u "$BIN" | sed 's/^ *U //' | sort -u >"$OUT.imports.txt"
	python3 -I "$HERE/kpi-audit.py" --xnu-config "$XNU_CONFIG" --sdk-headers "$KF" \
		--plist "$OUT/Contents/Info.plist" --imports "$OUT.imports.txt" "${ALLOW[@]}" ||
		die "KPI audit"
fi
if [ -n "${HELIOS_REAL_KC:-}" ]; then
	[ -f "$HELIOS_REAL_KC" ] || die "HELIOS_REAL_KC=$HELIOS_REAL_KC not found"
	python3 -I "$HERE/kc-audit.py" --kc "$HELIOS_REAL_KC" --kext "$BIN" --plist "$OUT/Contents/Info.plist" ${KCSIBLING[@]+"${KCSIBLING[@]}"} ||
		die "real-collection audit (kc-audit.py)"
else
	echo "note: HELIOS_REAL_KC unset — imports/vtables not checked against a real kernel collection"
fi
echo "cross-build-kext: OK — $OUT ($(sha256sum "$BIN" | cut -c1-16)…)"
