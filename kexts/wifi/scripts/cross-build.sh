#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# cross-build.sh — build the Wi-Fi kexts for macOS 26 x86_64 on Linux.
#
# Default: the native pair (native/scripts/cross-build-native.sh) in build/native/:
#   Ath10kMac.kext (core, native personality) + AirportAth10k.kext (IO80211 front end).
#
# --ethernet-debug: the developer/debug variant — the core as an Ethernet-style
# interface plus the "Ath10k Connectivity" menu-bar app in build/release/. Not
# for users: macOS shows it as a wired port and the system Wi-Fi menu stays empty.
#
# Ethernet-debug steps:
#   1. scripts/fetch-firmware.sh      pinned linux-firmware blobs, SHA-256 checked
#   2. scripts/prepare-firmware.py    build/gen/kext/Ath10kPayload.hpp (QCA6174 only)
#   3. tools/cross-build-kext.sh      clang -mkernel against MacOSX26.5.sdk, ld64 -kext,
#                                     generated _kmod_info, KPI audit vs xnu-12377 +
#                                     IONetworking/IOPCI headers (scripts/kext-families.allow)
#   4. weak/stack-probe import check  scripts/check-kext-symbols.py (upstream's rule)
#   5. scripts/cross-build-app.sh     the menu-bar app (skipped with --kext-only)
#
# Nothing here signs anything: Linux cannot codesign. OpenCore-injected kexts
# need no signature; the app must be ad-hoc signed on the Mac (README).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
OUT="${OUT:-$ROOT/build/release}"
NM="${NM:-llvm-nm}"
cd "$ROOT"
if [ "${1:-}" != "--ethernet-debug" ]; then
	exec bash native/scripts/cross-build-native.sh
fi
shift

sh scripts/fetch-firmware.sh
python3 scripts/prepare-firmware.py --set kext

# Upstream Makefile defaults (Makefile KFLAGS): 64 TX slots, 2x2 VHT80, payload cache on.
DEFS=(-D ATH10K_PAYLOAD_CACHE=1 -D ATH10K_RUNTIME_IRQ=1 -D ATH10K_RX_BATCH=1 -D ATH10K_RX_RESERVE=1
      -D ATH10K_MAX_WIDTH=80 -D ATH10K_MAX_NSS=2 -D ATH10K_AMPDU_LIMIT=64 -D ATH10K_AMSDU_LIMIT=3
      -D ATH10K_HT_VHT=1 -D ATH10K_DATA_TX_SLOTS=64)
RES="$ROOT/build/kext-resources"
rm -rf "$RES"; mkdir -p "$RES"
cp firmware/LICENSE.QualcommAtheros_ath10k firmware/notice_ath10k_firmware-6.txt LICENSE "$RES/"
cp build/gen/kext/manifest.json "$RES/firmware-manifest.json"

mkdir -p "$OUT"
"$REPO/tools/cross-build-kext.sh" --plist Ath10kMac.kext/Contents/Info.plist --out "$OUT/Ath10kMac.kext" \
	-I "$ROOT/build/gen/kext" -I "$ROOT/src/kext" "${DEFS[@]}" \
	--cflags "-fno-stack-check -Wframe-larger-than=4096 -Werror=frame-larger-than -Wno-nullability-completeness" \
	--allow "$ROOT/scripts/kext-families.allow" --resources "$RES" \
	src/kext/Ath10kUserClient.cpp src/kext/Ath10kNetwork.cpp src/kext/Ath10kRuntime.cpp \
	src/kext/Ath10kPower.cpp src/kext/Ath10kPCIDevice.cpp

"$NM" -m "$OUT/Ath10kMac.kext/Contents/MacOS/Ath10kMac" > "$OUT/Ath10kMac.nm.txt"
python3 -I scripts/check-kext-symbols.py --listing "$OUT/Ath10kMac.nm.txt"

if [ "${1:-}" != "--kext-only" ]; then
	bash scripts/cross-build-app.sh "$OUT"
fi
echo "cross-build: OK — $OUT"
