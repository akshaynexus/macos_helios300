#!/bin/sh
# Regenerates awdl_vectors.inc from OWL's own C code (seemoo-lab/owl, GPL-3.0, pinned commit)
# driven by gen.c with a fixed clock. Needs git, gcc, python3 and network access; writes only
# ../awdl_vectors.inc. The vectors pin our AirportAth10kAwdl.hpp port to OWL's wire format.
set -eu
cd "$(dirname "$0")"
OWL_COMMIT=0ee3263f944ea721c355f826aa90d636d012a512
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
git clone -q https://github.com/seemoo-lab/owl "$tmp/owl"
git -C "$tmp/owl" checkout -q "$OWL_COMMIT"
git -C "$tmp/owl" submodule update -q --init radiotap
sed 's/^uint64_t clock_time_us() {/uint64_t orig_clock_time_us() {/' "$tmp/owl/src/state.c" > "$tmp/state_noclock.c"
gcc -std=gnu11 -D_GNU_SOURCE -DRADIOTAP_SUPPORT_OVERRIDES -I"$tmp/owl/src" -I"$tmp/owl/radiotap" -o "$tmp/gen" gen.c "$tmp/state_noclock.c" \
    $(ls "$tmp"/owl/src/*.c | grep -v '/state\.c$') "$tmp/owl/radiotap/radiotap.c"
"$tmp/gen" > "$tmp/vectors.txt"
python3 to_inc.py "$tmp/vectors.txt" > ../awdl_vectors.inc
echo "wrote ../awdl_vectors.inc from OWL $OWL_COMMIT"
