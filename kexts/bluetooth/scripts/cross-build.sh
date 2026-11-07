#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build QCARomePatchRAM.kext for macOS 26 x86_64 on Linux with the shared
# tools/cross-build-kext.sh (clang + macOS 26.5 SDK + Apple ld64 + KPI audit).
# Output: build/cross/QCARomePatchRAM.kext. Exit codes as cross-build-kext.sh.
# QCA_FIRMWARE_CACHE: where the pinned linux-firmware files are kept (sha256-checked on every use);
# release.sh points it at RELEASE_CACHE so a fresh export does not download them again.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$HERE/build/cross}"
GEN="$HERE/build/gen"
[ -f "$GEN/qca_rome_firmware.h" ] ||
	python3 -I "$HERE/scripts/gen_firmware_header.py" "$GEN/qca_rome_firmware.h" --cache "${QCA_FIRMWARE_CACHE:-$HERE/build/firmware-cache}"
RES="$OUT/.resources"; rm -rf "$RES"; mkdir -p "$RES"
cp "$HERE/firmware/LICENSE.QualcommAtheros_ath10k" "$HERE/firmware/NOTICE.qca" "$HERE/LICENSE" "$RES/"
"$REPO/tools/cross-build-kext.sh" \
	--plist "$HERE/kext/QCARomePatchRAM/Info.plist" \
	--out "$OUT/QCARomePatchRAM.kext" \
	-I "$HERE/src/core" -I "$GEN" -I "$HERE/kext/QCARomePatchRAM" \
	--cflags "-Wextra -Werror" \
	--allow "$HERE/kext/QCARomePatchRAM/kpi-allow.txt" \
	--resources "$RES" \
	"$HERE/kext/QCARomePatchRAM/QCARomePatchRAM.cpp" \
	"$HERE/src/core/qca_rome.c" "$HERE/src/core/qca_lifecycle.c"
rm -rf "$RES"
