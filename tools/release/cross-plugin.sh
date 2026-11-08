#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# cross-plugin.sh <plugin source dir> <out dir> <version> — NVMTLDriver.bundle (Metal plugin) cross-built on Linux.
#
# The Linux equivalent of the driver repo's build/build_plugin_release.sh (xcrun clang on the build Mac):
#   clang -target x86_64-apple-macos15.0 -isysroot MacOSX26.5.sdk -bundle -fobjc-arc -O1 -DNVMTL_RELEASE=1
#   NVMTLDevice.m -framework Foundation Metal IOSurface IOKit CoreFoundation MetalFX, linked by Apple ld64
#   ld64.lld (see the note at the link) against the SDK's .tbd stubs. -Werror=unguarded-availability-new as on the Mac, so a
#   macOS 26-only API without an availability check stops the build (one plugin serves macOS 15 and 26).
# The shader library the plugin uses (nvconv.metal) is committed precompiled as nvconv_lib.h, so no Metal
# compiler is needed. Vulkan headers: KhronosGroup/Vulkan-Headers v1.4.363 (= VK_HEADER_VERSION of the
# Mesa tree NVK is built from), pinned by commit.
# Checks: Mach-O MH_BUNDLE x86_64, minos 15.0, and every undefined symbol bound to an SDK library that
# exports it (nvk/pascal/scripts/macho-check.py). The bundle is ad-hoc signed by release.sh afterwards.
# Exit: 0 built + checked, 1 failure, 77 tools missing.
set -euo pipefail
SRC="$(cd "${1:?plugin source dir}" && pwd)"; OUT="${2:?out dir}"; VER="${3:?version}"
SDK="${MACOS_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk}"
CACHE="${RELEASE_CACHE:-$HOME/.cache/helios300-release}"
MACHO_CHECK="${MACHO_CHECK:?set MACHO_CHECK to nvk/pascal/scripts/macho-check.py}"
VKH_URL=https://github.com/KhronosGroup/Vulkan-Headers
VKH_COMMIT=6802bb4733b63ed5efd3adb308a6c885ef180ea1   # tag v1.4.363
MIN=15.0
skip() { echo "cross-plugin: SKIP — $*"; exit 77; }
die() { echo "cross-plugin: FAIL — $*"; exit 1; }
[ -d "$SDK/System/Library/Frameworks/Metal.framework" ] || skip "macOS SDK with Metal.framework not found at $SDK"
python3 -c 'import yaml' 2>/dev/null || skip "PyYAML missing (macho-check.py reads the SDK .tbd stubs)"

VKH="$CACHE/Vulkan-Headers-$VKH_COMMIT"
if [ ! -f "$VKH/include/vulkan/vulkan_core.h" ]; then
	rm -rf "$VKH"; git init -q "$VKH"
	git -C "$VKH" fetch -q --depth 1 "$VKH_URL" "$VKH_COMMIT" && git -C "$VKH" checkout -q FETCH_HEAD || die "fetch Vulkan-Headers"
fi
[ "$(git -C "$VKH" rev-parse HEAD)" = "$VKH_COMMIT" ] || die "Vulkan-Headers cache is not at $VKH_COMMIT"

# __builtin_cpu_supports needs ___cpu_model, which Apple clang takes from compiler-rt (libclang_rt.osx.a)
# without being asked. Upstream clang on Linux has no darwin compiler-rt, so the same source is compiled
# here: LLVM compiler-rt lib/builtins/cpu_model/x86.c + cpu_model.h at llvmorg-23.1.4 (Apache-2.0 WITH
# LLVM-exception), SHA-256 pinned.
CRT_TAG=llvmorg-23.1.4
CRT="$CACHE/compiler-rt-$CRT_TAG"; mkdir -p "$CRT"
for f in "x86.c 2f4ae90693c03fd5d0741d2b8b68edac5fed132a6fc6990ec540b0e1499e87ed" \
         "cpu_model.h 0b2f65e64cca9bf4287eec344c6802cdf850c6d6c48fb4c1a7bdd787974af733"; do
	set -- $f
	if [ ! -f "$CRT/$1" ] || [ "$(sha256sum "$CRT/$1" | cut -d' ' -f1)" != "$2" ]; then
		curl -fsSL -o "$CRT/$1.part" "https://raw.githubusercontent.com/llvm/llvm-project/$CRT_TAG/compiler-rt/lib/builtins/cpu_model/$1" || die "fetch compiler-rt $1"
		[ "$(sha256sum "$CRT/$1.part" | cut -d' ' -f1)" = "$2" ] || { rm -f "$CRT/$1.part"; die "compiler-rt $1 sha256 mismatch"; }
		mv "$CRT/$1.part" "$CRT/$1"
	fi
done

B="$OUT/NVMTLDriver.bundle"; rm -rf "$B"; mkdir -p "$B/Contents/MacOS"
LOG="$OUT/NVMTLDriver.build.log"
clang -target "x86_64-apple-macos$MIN" -isysroot "$SDK" -O2 -c -fvisibility=hidden "$CRT/x86.c" -o "$OUT/cpu_model.o" >"$LOG" 2>&1 ||
	{ cat "$LOG"; die "compile compiler-rt cpu_model/x86.c"; }
# Linker: ld64.lld. Apple ld64-956.6 from cctools-port (the kexts' linker) aborts on this object with a
# libstdc++ assertion (std::vector<ld::Fixup>::operator[] out of range) once every symbol resolves; the
# NVK dylib links with it, this bundle does not. ld64.lld links it, and macho-check below proves the binds.
command -v ld64.lld >/dev/null || skip "ld64.lld (LLVM lld) not found"
( cd "$SRC" && clang -target "x86_64-apple-macos$MIN" -isysroot "$SDK" -fuse-ld=lld \
	-bundle -fobjc-arc -O1 -DNVMTL_RELEASE=1 -Wno-protocol -Wno-incomplete-implementation \
	-Werror=unguarded-availability-new -I"$VKH/include" -o "$B/Contents/MacOS/NVMTLDriver" NVMTLDevice.m "$OUT/cpu_model.o" \
	-framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation -framework MetalFX ) \
	>>"$LOG" 2>&1 || { grep -E ' error: |^ld: |Undefined|referenced from' "$LOG" | head -30; die "compile/link (log: $LOG)"; }
echo "PASS compile + link NVMTLDriver ($(grep -c ': warning:' "$LOG" || true) warnings)"
cat >"$B/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>NVMTLDriver</string>
	<key>CFBundleIdentifier</key>
	<string>com.nullmoth.NVMTLDriver</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>NVMTLDriver</string>
	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleShortVersionString</key>
	<string>$VER</string>
	<key>CFBundleVersion</key>
	<string>$VER</string>
</dict>
</plist>
PLIST
BIN="$B/Contents/MacOS/NVMTLDriver"
hdr="$(llvm-objdump --macho --private-header "$BIN")"
grep -q "X86_64" <<<"$hdr" && grep -q "BUNDLE" <<<"$hdr" || die "not an x86_64 MH_BUNDLE"
minos="$(llvm-objdump --macho --all-headers "$BIN" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/&&!d{print $2; d=1}')"
[ "$minos" = "$MIN" ] || die "minos $minos, want $MIN"
echo "PASS Mach-O x86_64 MH_BUNDLE, minos $minos"
python3 "$MACHO_CHECK" "$BIN" "$SDK" >"$OUT/NVMTLDriver.macho-check.txt" 2>&1 || { tail -20 "$OUT/NVMTLDriver.macho-check.txt"; die "macho-check"; }
tail -1 "$OUT/NVMTLDriver.macho-check.txt"
echo "cross-plugin: OK — $B ($(sha256sum "$BIN" | cut -c1-16)…)"
