#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# cross-xlate.sh <translator source dir> <out dir> — libnvmtl_translate.dylib (AIR -> SPIR-V) cross-built on Linux.
#
# The Linux equivalent of the driver repo's build/build_xlate_release.sh (cargo on the build Mac):
#   cargo build --release --locked --target x86_64-apple-darwin --manifest-path wrapper/Cargo.toml
# with rustup's x86_64-apple-darwin std and a linker wrapper: clang -target x86_64-apple-macos15.0
# -isysroot MacOSX26.5.sdk -fuse-ld=lld (the same linker as the plugin). Source paths are remapped
# (--remap-path-prefix) so the dylib names no build-machine path, the install name is
# @rpath/libnvmtl_translate.dylib (what install_name_tool -id sets on the Mac), debug stabs are stripped
# (strip -S), and nvk/pascal/scripts/macho-check.py proves every bind against the SDK .tbd stubs.
# Exit: 0 built + checked, 1 failure, 77 tools missing.
set -euo pipefail
SRC="$(cd "${1:?translator source dir}" && pwd)"; OUT="$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)"
SDK="${MACOS_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk}"
MACHO_CHECK="${MACHO_CHECK:?set MACHO_CHECK to nvk/pascal/scripts/macho-check.py}"
CARGO="${CARGO:-$HOME/.cargo/bin/cargo}"; RUSTC="${RUSTC:-$HOME/.cargo/bin/rustc}"
MIN=15.0
skip() { echo "cross-xlate: SKIP — $*"; exit 77; }
die() { echo "cross-xlate: FAIL — $*"; exit 1; }
[ -x "$CARGO" ] || skip "cargo not found at $CARGO"
lib="$("$RUSTC" --print target-libdir --target x86_64-apple-darwin 2>/dev/null)" && [ -d "$lib" ] ||
	skip "rust std for x86_64-apple-darwin missing (rustup target add x86_64-apple-darwin)"
command -v ld64.lld >/dev/null || skip "ld64.lld not found"
command -v llvm-strip >/dev/null || skip "llvm-strip not found"
[ -f "$SDK/usr/lib/libSystem.tbd" ] || skip "macOS SDK not found at $SDK"

# cargo hashes the source path into symbol names, so the same commit built under another path gives a
# different binary. Build from a fixed copy (build_xlate_release.sh keeps its R fixed for the same reason).
CACHE="${RELEASE_CACHE:-$HOME/.cache/helios300-release}"
FIXED="$CACHE/xlate-build"; rm -rf "$FIXED"; mkdir -p "$FIXED"; cp -R "$SRC" "$FIXED/src"; SRC="$FIXED/src"
WORK="$FIXED/work"; mkdir -p "$WORK/bin"
cat >"$WORK/bin/cc" <<EOF
#!/bin/sh
exec clang -target x86_64-apple-macos$MIN -isysroot "$SDK" -fuse-ld=lld -Qunused-arguments "\$@"
EOF
chmod +x "$WORK/bin/cc"
# A fresh target dir every time: git archive stamps files with the commit time and cargo reuses objects
# newer than their sources (build_xlate_release.sh has the same rule).
export CARGO_TARGET_X86_64_APPLE_DARWIN_LINKER="$WORK/bin/cc"
export CC_x86_64_apple_darwin="$WORK/bin/cc" AR_x86_64_apple_darwin=llvm-ar
export MACOSX_DEPLOYMENT_TARGET=$MIN SDKROOT="$SDK"
export RUSTFLAGS="--remap-path-prefix=$SRC=/src --remap-path-prefix=$HOME/.cargo=/cargo --remap-path-prefix=$HOME=/build -C link-arg=-Wl,-install_name,@rpath/libnvmtl_translate.dylib"
LOG="$OUT/libnvmtl_translate.cargo.log"
( cd "$SRC" && "$CARGO" build --release --locked --target x86_64-apple-darwin --manifest-path wrapper/Cargo.toml \
	--target-dir "$WORK/target" ) >"$LOG" 2>&1 || { grep -A6 '^error' "$LOG" | head -30; die "cargo build (log: $LOG)"; }
D="$OUT/libnvmtl_translate.dylib"
cp "$WORK/target/x86_64-apple-darwin/release/libnvmtl_translate.dylib" "$D"
llvm-strip -S "$D"
hdr="$(llvm-objdump --macho --private-header "$D")"
grep -q X86_64 <<<"$hdr" && grep -q DYLIB <<<"$hdr" || die "not an x86_64 MH_DYLIB"
llvm-objdump --macho --dylib-id "$D" | grep -q '@rpath/libnvmtl_translate.dylib' || die "install name is not @rpath/libnvmtl_translate.dylib"
left=$(strings -a "$D" | grep -cE "$HOME|/home/|/Users/" || true)
[ "$left" = 0 ] || { strings -a "$D" | grep -E "$HOME|/home/|/Users/" | sort -u | head -5; die "$left build-machine path(s) left in the dylib"; }
python3 "$MACHO_CHECK" "$D" "$SDK" >"$OUT/libnvmtl_translate.macho-check.txt" 2>&1 || { tail -20 "$OUT/libnvmtl_translate.macho-check.txt"; die "macho-check"; }
rm -rf "$FIXED"
echo "PASS Mach-O x86_64 MH_DYLIB, @rpath install name, no build paths, $(tail -1 "$OUT/libnvmtl_translate.macho-check.txt")"
echo "cross-xlate: OK — $D ($(sha256sum "$D" | cut -c1-16)…)"
