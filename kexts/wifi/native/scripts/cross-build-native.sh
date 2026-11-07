#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# cross-build-native.sh — build the native macOS 26 Wi-Fi pair on Linux:
#   Ath10kMac.kext      the ath10k core with Ath10kNativeFrontEnd=true (no
#                       Ethernet interface of its own; publishes Ath10kNativeReady)
#   AirportAth10k.kext  the IO80211/Skywalk front end over the core
# Output: kexts/wifi/build/native/. Uses tools/cross-build-kext.sh (clang
# -mkernel + Apple ld64 + KPI audit). The front end's imports are audited
# against the reconstructed IO80211/Skywalk/corecapture headers
# (native/include/Airport) and the core's own header. With HELIOS_REAL_KC set to
# the target Mac's BootKernelExtensions.kc, tools/kc-audit.py also checks both
# kexts' imports and vtable layouts against that collection. Nothing is signed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
OUT="${OUT:-$ROOT/build/native}"
NM="${NM:-llvm-nm}"
cd "$ROOT"

sh scripts/fetch-firmware.sh
python3 scripts/prepare-firmware.py --set kext

DEFS=(-D ATH10K_PAYLOAD_CACHE=1 -D ATH10K_RUNTIME_IRQ=1 -D ATH10K_RX_BATCH=1 -D ATH10K_RX_RESERVE=1
      -D ATH10K_MAX_WIDTH=80 -D ATH10K_MAX_NSS=2 -D ATH10K_AMPDU_LIMIT=64 -D ATH10K_AMSDU_LIMIT=3
      -D ATH10K_HT_VHT=1 -D ATH10K_DATA_TX_SLOTS=64
      -D AIRPORT_ATH10K_WPASEC="${WPASEC:-0}") # WPASEC=1 enables the paused security work (docs/WIFI-SECURITY.md)
CFLAGS="-fno-stack-check -Wframe-larger-than=4096 -Werror=frame-larger-than -Wno-nullability-completeness"
RES="$ROOT/build/kext-resources"
rm -rf "$RES"; mkdir -p "$RES"
cp firmware/LICENSE.QualcommAtheros_ath10k firmware/notice_ath10k_firmware-6.txt LICENSE "$RES/"
cp build/gen/kext/manifest.json "$RES/firmware-manifest.json"
mkdir -p "$OUT" "$ROOT/build/gen/native"

# 1. The core, with the native personality key set.
python3 -I - "$ROOT/Ath10kMac.kext/Contents/Info.plist" "$ROOT/build/gen/native/Ath10kMac-Info.plist" <<'PY'
import plistlib,sys
d=plistlib.load(open(sys.argv[1],'rb'))
for p in d['IOKitPersonalities'].values(): p['Ath10kNativeFrontEnd']=True
plistlib.dump(d,open(sys.argv[2],'wb'))
PY
"$REPO/tools/cross-build-kext.sh" --plist "$ROOT/build/gen/native/Ath10kMac-Info.plist" --out "$OUT/Ath10kMac.kext" \
	-I "$ROOT/build/gen/kext" -I "$ROOT/src/kext" "${DEFS[@]}" --cflags "$CFLAGS" \
	--allow "$ROOT/scripts/kext-families.allow" --resources "$RES" \
	src/kext/Ath10kUserClient.cpp src/kext/Ath10kNetwork.cpp src/kext/Ath10kRuntime.cpp \
	src/kext/Ath10kPower.cpp src/kext/Ath10kPCIDevice.cpp
"$NM" -m "$OUT/Ath10kMac.kext/Contents/MacOS/Ath10kMac" > "$OUT/Ath10kMac.nm.txt"
python3 -I scripts/check-kext-symbols.py --listing "$OUT/Ath10kMac.nm.txt"

# 2. The front end. Its allowlist names the reconstructed headers by absolute path.
sed -e "s|@AIRPORT@|$ROOT/native/include/Airport|g" -e "s|@CORE@|$ROOT/src/kext|g" \
	native/scripts/native-families.allow > "$ROOT/build/gen/native/families.allow"
mkdir -p "$OUT/AirportAth10k-resources"
cp LICENSE "$OUT/AirportAth10k-resources/"
cp native/NOTICE "$OUT/AirportAth10k-resources/NOTICE" 2>/dev/null || true
"$REPO/tools/cross-build-kext.sh" --plist native/AirportAth10k.kext/Contents/Info.plist --out "$OUT/AirportAth10k.kext" \
	-I "$ROOT/native/include" -I "$ROOT/native/src" -I "$ROOT/src/kext" -I "$ROOT/build/gen/kext" "${DEFS[@]}" \
	--cflags "$CFLAGS -Wno-unused-private-field -Wno-overloaded-virtual -Wno-invalid-offsetof" \
	--allow "$ROOT/build/gen/native/families.allow" --resources "$OUT/AirportAth10k-resources" \
	--kc-sibling "$OUT/Ath10kMac.kext/Contents/MacOS/Ath10kMac" \
	native/src/AirportAth10k.cpp native/src/AirportAth10kInterface.cpp native/src/AirportAth10kEthernetInterface.cpp
rm -rf "$OUT/AirportAth10k-resources"
"$NM" -m "$OUT/AirportAth10k.kext/Contents/MacOS/AirportAth10k" > "$OUT/AirportAth10k.nm.txt"
python3 -I scripts/check-kext-symbols.py --listing "$OUT/AirportAth10k.nm.txt"
echo "cross-build-native: OK — $OUT (Ath10kMac.kext + AirportAth10k.kext)"
