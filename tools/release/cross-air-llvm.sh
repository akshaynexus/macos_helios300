#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# cross-air-llvm.sh <out dir> — the Metal plugin's AIR reader, NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib,
# cross-built on Linux from upstream LLVM.
#
# Why: the plugin (driver repo plugin/NVMTLLibraryLoad.m, nvmtl_bitcode_reader_ready) dlopens
# air-runtime/libLLVM.dylib next to its binary and reads every Metal library's AIR bitcode in process through the
# LLVM C API only (dlsym by name: LLVMParseBitcodeInContext2, LLVMRunPasses, LLVMPrintModuleToString, the IR-walk
# functions, ...); the printed IR text goes to the translator. Without the dylib every shader that is not already in
# the cache fails to compile. No C++ ABI crosses the boundary, so any LLVM that exports those C functions and whose
# printed IR the translator reads will do. The translator's fixtures use LLVM 21+ syntax (captures(none), samesign,
# getelementptr inbounds nuw); upstream LLVM 23.1.4 reads Apple AIR (air64_v27, Metal on macOS 15.7) unpatched
# (tools/release/air-reader-check.c runs that on Linux).
#
# What: llvm-project 23.1.4 (llvmorg-23.1.4, the version of the cross toolchain and of the compiler-rt file the
# plugin build pins), release tarball SHA-256 pinned (its .sig verifies with LLVM release manager Tobias Hieta's key
# D574BD5D1D0E98895E3BF90044F2485E45D59042 from https://releases.llvm.org/release-keys.asc). Configured for
# x86_64-apple-macos15.0 against the macOS 26.5 SDK, clang + ld64.lld, CMake's NATIVE sub-build for tablegen:
#   - libLLVM.dylib only (LLVM_BUILD_LLVM_DYLIB), components Core, BitReader, IRReader, Passes, Support and what they
#     need; no code generator (LLVM_TARGETS_TO_BUILD empty: the plugin runs IR passes only), no tools;
#   - no zlib, zstd, libxml2, libedit, ffi, curl: the dylib links only /usr/lib/libSystem.B.dylib and
#     /usr/lib/libc++.1.dylib, so nothing else ships beside it (upstream NullMoth shipped Homebrew's LLVM + libzstd);
#   - BLAKE3 in portable C (LLVM_DISABLE_ASSEMBLY_FILES: CMake on a Linux host does not assemble its x86 .S files for
#     Darwin; BLAKE3 only hashes, it is not on the AIR path);
#   - install name @loader_path/libLLVM.dylib, local symbols stripped.
# The build takes ~3-5 min on 20 cores and is cached in $RELEASE_CACHE/air-llvm-<key> (key: tarball + this
# script), so a release rebuilds it only when one of them changes.
# Output: <out>/libLLVM.dylib and <out>/licenses/ (LLVM's LICENSE.TXT and the notices of code compiled into it).
# Exit: 0 ok, 1 failure, 77 tools missing.
set -euo pipefail
OUT="${1:?out dir}"
SDK="${MACOS_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk}"
CACHE="${RELEASE_CACHE:-$HOME/.cache/helios300-release}"
JOBS="${JOBS:-$(nproc)}"
LLVM_VER=23.1.4
TARBALL="llvm-project-$LLVM_VER.src.tar.xz"
TARBALL_SHA=819ecf7351f1a66f76b2d701d8942faaf5faeb324cb90c9d5b090de142434bb2
URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/$TARBALL"
MIN=15.0
skip() { echo "cross-air-llvm: SKIP — $*"; exit 77; }
die() { echo "cross-air-llvm: FAIL — $*"; exit 1; }
for t in clang clang++ ld64.lld llvm-ar llvm-ranlib llvm-nm llvm-strip llvm-install-name-tool llvm-objdump cmake ninja; do
	command -v "$t" >/dev/null || skip "$t missing"
done
[ -f "$SDK/usr/lib/libc++.tbd" ] || skip "no macOS SDK at $SDK"

SRCDIR="$CACHE/llvm-project-$LLVM_VER"; mkdir -p "$SRCDIR"
if [ ! -f "$SRCDIR/$TARBALL" ] || [ "$(sha256sum "$SRCDIR/$TARBALL" | cut -d' ' -f1)" != "$TARBALL_SHA" ]; then
	curl -fsSL --retry 2 -o "$SRCDIR/$TARBALL.part" "$URL" || die "download $URL"
	mv "$SRCDIR/$TARBALL.part" "$SRCDIR/$TARBALL"
fi
[ "$(sha256sum "$SRCDIR/$TARBALL" | cut -d' ' -f1)" = "$TARBALL_SHA" ] || die "$TARBALL sha256 is not $TARBALL_SHA"

KEY="$( { echo "$TARBALL_SHA"; sha256sum "$0" | cut -d' ' -f1; echo "$SDK"; } | sha256sum | cut -c1-16)"
W="$CACHE/air-llvm-$KEY"
if [ ! -f "$W/done/libLLVM.dylib" ]; then
	rm -rf "$W"; mkdir -p "$W"
	echo "cross-air-llvm: building libLLVM $LLVM_VER for x86_64-apple-macos$MIN in $W (cached afterwards)"
	tar -xJf "$SRCDIR/$TARBALL" -C "$W" "llvm-project-$LLVM_VER.src/llvm" "llvm-project-$LLVM_VER.src/cmake" \
		"llvm-project-$LLVM_VER.src/third-party" "llvm-project-$LLVM_VER.src/libc"   # libc: header-only utilities LLVM 23 includes
	S="$W/llvm-project-$LLVM_VER.src"
	TC="$W/darwin-x86_64.cmake"
	cat >"$TC" <<EOF
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_OSX_ARCHITECTURES x86_64)
set(CMAKE_OSX_SYSROOT "$SDK")
set(CMAKE_OSX_DEPLOYMENT_TARGET $MIN)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-apple-macos$MIN)
set(CMAKE_CXX_COMPILER_TARGET x86_64-apple-macos$MIN)
set(CMAKE_AR "$(command -v llvm-ar)")
set(CMAKE_RANLIB "$(command -v llvm-ranlib)")
set(CMAKE_INSTALL_NAME_TOOL "$(command -v llvm-install-name-tool)")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
	# -ffile-prefix-map: no build-machine path in the binary.
	cmake -S "$S/llvm" -B "$W/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_FLAGS="-ffile-prefix-map=$S=llvm-project-$LLVM_VER" -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$S=llvm-project-$LLVM_VER" \
		-DLLVM_HOST_TRIPLE=x86_64-apple-darwin24.0.0 -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-apple-darwin24.0.0 \
		-DLLVM_TARGETS_TO_BUILD= -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=OFF \
		-DLLVM_DYLIB_COMPONENTS="Core;BitReader;IRReader;Passes;Support" \
		-DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
		-DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_FFI=OFF -DLLVM_ENABLE_CURL=OFF \
		-DLLVM_ENABLE_HTTPLIB=OFF -DLLVM_ENABLE_Z3_SOLVER=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_OCAMLDOC=OFF \
		-DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
		-DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF -DLLVM_INSTALL_UTILS=OFF -DLLVM_INCLUDE_UTILS=ON \
		-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF -DLLVM_APPEND_VC_REV=OFF \
		-DLLVM_DISABLE_ASSEMBLY_FILES=ON \
		>"$W/configure.log" 2>&1 || { tail -40 "$W/configure.log"; die "cmake configure (log: $W/configure.log)"; }
	ninja -C "$W/build" -j "$JOBS" LLVM >"$W/build.log" 2>&1 || { grep -E "error|FAILED" "$W/build.log" | head -30; die "ninja LLVM (log: $W/build.log)"; }
	mkdir -p "$W/done/licenses"
	D="$W/build/lib/libLLVM.dylib"; [ -f "$D" ] || D="$(ls "$W"/build/lib/libLLVM*.dylib | head -1)"
	cp -L "$D" "$W/done/libLLVM.dylib"
	llvm-install-name-tool -id @loader_path/libLLVM.dylib "$W/done/libLLVM.dylib"
	llvm-strip -x "$W/done/libLLVM.dylib"
	# Licences: LLVM's (Apache-2.0 WITH LLVM-exception; third-party/siphash is under it too), and the notices of the
	# two third-party pieces LLVM Support compiles into the library: BLAKE3 (CC0-1.0 OR Apache-2.0) and Henry
	# Spencer's regex (BSD-style).
	cp "$S/llvm/LICENSE.TXT" "$W/done/licenses/LLVM-LICENSE.txt"
	cp "$S/llvm/lib/Support/BLAKE3/LICENSE" "$W/done/licenses/BLAKE3-LICENSE.txt"
	cp "$S/llvm/lib/Support/COPYRIGHT.regex" "$W/done/licenses/regex-COPYRIGHT.txt"
	echo "$LLVM_VER $TARBALL_SHA" >"$W/done/VERSION"
fi

# Checks on the Mach-O itself (it cannot run on Linux; tools/release/check-air-llvm.py checks the exports).
B="$W/done/libLLVM.dylib"
hdr="$(llvm-objdump --macho --private-headers "$B")"
grep -q "MH_MAGIC_64.*X86_64.*DYLIB" <<<"$hdr" || die "libLLVM.dylib is not an x86_64 MH_DYLIB"
grep -A6 "LC_BUILD_VERSION" <<<"$hdr" | grep -q "minos $MIN$" || die "libLLVM.dylib minos is not $MIN"
libs="$(llvm-objdump --macho --dylibs-used "$B" | tail -n +2 | awk '{print $1}' | sort -u | tr '\n' ' ')"
for l in $libs; do
	case "$l" in @loader_path/libLLVM.dylib|/usr/lib/libSystem.B.dylib|/usr/lib/libc++.1.dylib) ;; *) die "libLLVM.dylib links $l (only libSystem and libc++ may ship)";; esac
done
! grep -aq "$W" "$B" || die "libLLVM.dylib contains the build path $W"
mkdir -p "$OUT"; rm -rf "$OUT/licenses"
cp "$B" "$OUT/libLLVM.dylib"; cp -R "$W/done/licenses" "$OUT/licenses"
echo "cross-air-llvm: OK — libLLVM $LLVM_VER x86_64 macOS $MIN, $(stat -c %s "$OUT/libLLVM.dylib") B, links: $libs($(sha256sum "$OUT/libLLVM.dylib" | cut -c1-16))"
