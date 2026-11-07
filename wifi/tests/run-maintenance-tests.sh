#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_tmp_dir=$(mktemp -d)
trap 'rm -rf "$test_tmp_dir"' EXIT HUP INT TERM
mkdir -p "$test_tmp_dir/IOKit"
cp tests/mock_iolib.h "$test_tmp_dir/IOKit/IOLib.h"
cat > "$test_tmp_dir/IOKit/IOTypes.h" <<'EOF'
#pragma once
#include <cstdint>
using UInt8=uint8_t;using UInt16=uint16_t;using UInt32=uint32_t;using UInt64=uint64_t;
EOF
python3 tests/extract-production.py "$test_tmp_dir/driver_methods.inc"
for test_case in control recovery station_maintenance channel_switch review_regressions ap_maintenance linux_parity firmware_memory wmi_notifications runtime_power firmware_stats fix085 fix086 fix086_runtime pll irq_fallback target_init_irq runtime_irq rate_recovery reset_sequence; do
    "${CXX:-c++}" -std=c++17 -pthread -O1 -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
        -I"$test_tmp_dir" -Isrc/kext "tests/test_$test_case.cpp" -o "$test_tmp_dir/$test_case"
    ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/$test_case"
done
for slots in 16 32 64; do
    "${CXX:-c++}" -std=c++17 -pthread -O1 -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
        -DATH10K_DATA_TX_SLOTS="$slots" -I"$test_tmp_dir" -Isrc/kext tests/test_connection.cpp -o "$test_tmp_dir/connection$slots"
    ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/connection$slots"
done
python3 tests/test-kernel-stack.py
ASAN_OPTIONS=detect_leaks=0 python3 tests/test-user-access.py
echo 'Final-source maintenance, CSA, control/Info, connection 16/32/64 and stack/access regressions passed.'
