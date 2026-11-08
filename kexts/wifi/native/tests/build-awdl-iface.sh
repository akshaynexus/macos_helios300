#!/bin/bash
# Builds and runs test_native_awdl_iface: the production AWDL driver source against
# tests/awdl-fakes (compiled from a copy so quote includes resolve to the fakes).
# Usage: build-awdl-iface.sh <output dir>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$1";mkdir -p "$OUT/awdl-src"
SRC="$HERE/../src"
for f in AirportAth10kAwdlDriver.cpp AirportAth10kAwdlDriver.hpp AirportAth10kAwdlSession.hpp AirportAth10kAwdl.hpp AirportAth10kRegistration.hpp;do cp "$SRC/$f" "$OUT/awdl-src/";done
cp "$HERE"/awdl-fakes/AirportAth10k.hpp "$HERE"/awdl-fakes/AirportAth10kDefines.hpp "$OUT/awdl-src/"
cp "$HERE/../../src/shared/Ath10kControl.hpp" "$OUT/awdl-src/"
cp "$HERE/test_native_awdl_iface.cpp" "$OUT/awdl-src/"
mkdir -p "$OUT/fakes/Airport/macOS26"
cp -R "$HERE/awdl-fakes/." "$OUT/fakes/"
cp "$HERE/../include/Airport/macOS26/IO80211AWDLProtocol.h" "$OUT/fakes/Airport/macOS26/"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -Wno-unused-private-field -fsanitize=address,undefined \
   -DAIRPORT_ATH10K_AWDL=1 -DAIRPORT_ATH10K_AWDL_TEST -I"$OUT/awdl-src" -I"$OUT/fakes" \
   "$OUT/awdl-src/test_native_awdl_iface.cpp" "$OUT/awdl-src/AirportAth10kAwdlDriver.cpp" -o "$OUT/awdl-iface"
ASAN_OPTIONS=detect_leaks=0 "$OUT/awdl-iface"
