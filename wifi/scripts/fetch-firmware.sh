#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# fetch-firmware.sh — download the pinned Qualcomm ath10k firmware the kext embeds.
#
# The blobs are not committed (Qualcomm licence: firmware/LICENSE.QualcommAtheros_ath10k).
# Every file comes from one fixed linux-firmware commit and must match the SHA-256
# listed below, so a build is reproducible and a tampered or updated file fails loudly.
#
#   linux-firmware tag 20261019 = commit a2dcb924f0897368b0b4f305cbf9808cdda90ce0
#   (the same release the Helios 300's Linux install runs: linux-firmware-atheros 1:20261019-1)
#
# Usage: scripts/fetch-firmware.sh [DEST]      (default: build/firmware)
# Offline: LINUX_FIRMWARE_DIR=/path/to/linux-firmware checkout copies instead of
# downloading (still checksum-verified).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=${1:-$ROOT/build/firmware}
COMMIT=a2dcb924f0897368b0b4f305cbf9808cdda90ce0
BASE=${LINUX_FIRMWARE_URL:-https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$COMMIT}

# path-in-linux-firmware  sha256
FILES='
ath10k/QCA6174/hw3.0/board-2.bin 66e83dde1c9af535df1fcd17c72971a96a263357300e921b358d35a353227d60
ath10k/QCA6174/hw3.0/firmware-6.bin 04d3bad5efa3f9fbe3ba53fd3e25fa9b0585ed227eea8111303b4e08861f979d
ath10k/QCA9377/hw1.0/board-2.bin 0fdcc7838f478da81704de88f7b33e28862110c6d5decf7818543f8e37e6cd98
ath10k/QCA9377/hw1.0/board.bin 127d35d82edb46278f30c448cbca664d755ff0d5fed57b649959cdbc4208c768
ath10k/QCA9377/hw1.0/firmware-6.bin 8f8b002fccfe81d42238f27dd1f56d189604f180bd4772c7c8e75ae1fef16f01
'

if command -v sha256sum >/dev/null 2>&1; then sha() { sha256sum "$1" | cut -d' ' -f1; }
else sha() { shasum -a 256 "$1" | cut -d' ' -f1; }; fi

echo "$FILES" | while read -r path want; do
	[ -n "$path" ] || continue
	out=$DEST/$path
	if [ -f "$out" ] && [ "$(sha "$out")" = "$want" ]; then continue; fi
	mkdir -p "$(dirname "$out")"
	if [ -n "${LINUX_FIRMWARE_DIR:-}" ]; then
		cp "$LINUX_FIRMWARE_DIR/$path" "$out.part"
	else
		curl -fsSL --retry 3 -o "$out.part" "$BASE/$path"
	fi
	got=$(sha "$out.part")
	if [ "$got" != "$want" ]; then
		rm -f "$out.part"
		echo "fetch-firmware: SHA-256 mismatch for $path: got $got, want $want" >&2
		exit 1
	fi
	mv "$out.part" "$out"
	echo "fetch-firmware: $path OK"
done
echo "fetch-firmware: all files verified in $DEST (linux-firmware $COMMIT)"
