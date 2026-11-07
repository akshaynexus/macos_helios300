#!/bin/sh
# Build three independent bundles; never install or load either automatically.
set -eu
cd "$(dirname "$0")/.."
mkdir -p experiments
for slots in 16 32 64; do
    make clean
    make kext TX_SLOTS="$slots" AMPDU_LIMIT="${AMPDU_LIMIT:-64}" AMSDU_LIMIT="${AMSDU_LIMIT:-3}"
    destination="experiments/tx$slots"
    mkdir -p "$destination"
    rm -rf "$destination/Ath10kMac.kext"
    cp -R build/out/Ath10kMac.kext "$destination/"
    printf 'TX_SLOTS=%s AMPDU_LIMIT=%s AMSDU_LIMIT=%s\n' "$slots" "${AMPDU_LIMIT:-64}" "${AMSDU_LIMIT:-3}" > "$destination/config.txt"
done
printf '%s\n' 'Ready: experiments/tx16, experiments/tx32 and experiments/tx64. Install only one at a time and reboot between tests.'
