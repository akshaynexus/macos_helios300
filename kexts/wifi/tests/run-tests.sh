#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
ASAN_OPTIONS=detect_leaks=0 python3 tests/test-user-access.py
python3 -m unittest discover -s tests -p 'test_*.py' -v
test_tmp_dir=$(mktemp -d)
trap 'rm -rf "$test_tmp_dir"' EXIT HUP INT TERM
mkdir -p "$test_tmp_dir/IOKit"
cp tests/mock_iolib.h "$test_tmp_dir/IOKit/IOLib.h"
cat > "$test_tmp_dir/IOKit/IOTypes.h" <<'EOF'
#pragma once
#include <cstdint>
using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
EOF
# The method extraction below uses textual boundaries: renaming/reordering methods
# requires updating those boundaries; missing markers intentionally fail loudly.
# HTC_ONLY=1 is useful when only the wire parser changed.
if [ "${HTC_ONLY:-0}" = 1 ]; then
    "${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$test_tmp_dir" -Isrc/kext tests/test_htc_ready.cpp -o "$test_tmp_dir/htc"
    ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/htc"
    "${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$test_tmp_dir" -Isrc/kext tests/test_htc_service.cpp -o "$test_tmp_dir/htc-service"
    ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/htc-service"
    echo 'HTC READY/service ABI tests passed (ASan/UBSan).'
    exit 0
fi
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_bmi_framing.cpp -o "$test_tmp_dir/framing"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/framing"
echo 'BMI framing tests passed (ASan/UBSan).'
python3 tests/extract-production.py "$test_tmp_dir/driver_methods.inc"
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_wake.cpp -o "$test_tmp_dir/wake"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/wake"
echo 'Wake ownership regression passed (actual wake/release methods; already-awake, asleep, MMIO failure and timeout).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_control.cpp -o "$test_tmp_dir/control"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/control"
echo 'Control API tests passed (production gated handler, lengths, ownership, key reinstall denial and port authorization).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_recovery.cpp -o "$test_tmp_dir/recovery"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/recovery"
echo 'Recovery/Info API regression passed (actual methods, host mocks; native termination/rematching not exercised).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_power.cpp -o "$test_tmp_dir/power"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/power"
echo 'Power notification/gate regression passed (actual methods, sleep before PCI off, both wake orders, duplicate events, cancellation, quarantine and notifier removal).'

for target in runtime_irq rx_reserve dma_policy rx_rate irq_fallback target_init_irq pll pll_memory; do
    "${CXX:-c++}" -std=c++17 -O2 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$test_tmp_dir" -Isrc/kext "tests/test_${target}.cpp" -o "$test_tmp_dir/$target"
    ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/$target"
    echo "Production $target regression passed (ASan/UBSan; host mocks)."
done
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DATH10K_PAYLOAD_CACHE=0 -I"$test_tmp_dir" -Isrc/kext tests/test_dma_policy.cpp -o "$test_tmp_dir/dma-uncached"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/dma-uncached"
echo 'Uncached payload fallback passed (actual production DMA helper).'
for target in regulatory wmi_reg_caps security_disconnect channel_switch dma_radio station_maintenance bmi_identity ap_maintenance linux_parity firmware_memory wmi_notifications runtime_power firmware_stats fix085 fix086 fix086_runtime rate_recovery reset_sequence; do
    "${CXX:-c++}" -std=c++17 -pthread "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
        -I"$test_tmp_dir" -Isrc/kext "tests/test_${target}.cpp" -o "$test_tmp_dir/$target"
    ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/$target"
done
for width in 20 40 80; do
    for nss in 1 2; do
        "${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
            -DATH10K_MAX_WIDTH="$width" -DATH10K_MAX_NSS="$nss" -I"$test_tmp_dir" -Isrc/kext tests/test_capability_limits.cpp -o "$test_tmp_dir/capability-limits"
        ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/capability-limits"
    done
done
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DATH10K_HT_VHT=0 -I"$test_tmp_dir" -Isrc/kext tests/test_capability_limits.cpp -o "$test_tmp_dir/capability-disabled"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/capability-disabled"
if [ "${WORKER_ONLY:-0}" = 1 ]; then
    "${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$test_tmp_dir" -Isrc/kext tests/test_wmi_runtime.cpp -o "$test_tmp_dir/wmi-runtime"
    ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/wmi-runtime"
    echo 'Final runtime lifecycle regression passed (actual methods; host mocks).'
    exit 0
fi
security_libs=""
if [ "$(uname -s)" != Darwin ]; then security_libs="-lcrypto"; fi
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -Wno-deprecated-declarations -fsanitize=address,undefined \
    tests/test_security.cpp $security_libs -o "$test_tmp_dir/security"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/security"
echo 'WPA2 supplicant tests passed (independent crypto vectors, MIC, GTK unwrap, retries, RSN mismatch and no key reinstall).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_rx_pipeline.cpp -o "$test_tmp_dir/rx-pipeline"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/rx-pipeline"
echo 'RX pipeline tests passed (atomic A-MSDU, replay, host/firmware reorder, wrap, gap/fragment timeouts, CCMP defrag, raw decap, 40/80MHz and 1/2SS negotiation).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_network_rx.cpp -o "$test_tmp_dir/network-rx"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/network-rx"
echo 'Network RX integration passed (actual production methods, FIRST-only Native WiFi metadata, aggregate security/integrity, replay, firmware and host ordering, EAPOL and allocation failure).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_rx_security_diagnostics.cpp -o "$test_tmp_dir/rx-security"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/rx-security"
echo 'RX security diagnostics passed (all 65536 first-header FC values preserve acceptance; FIRST-only metadata, categorized rejection, no PN commit and no IV export).'
if [ "${PERFORMANCE_ONLY:-0}" = 1 ]; then
    for target in rx-setup rx-refill connection; do
        source_name=$(printf '%s' "$target" | tr '-' '_')
        "${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
            -I"$test_tmp_dir" -Isrc/kext "tests/test_${source_name}.cpp" -o "$test_tmp_dir/$target"
        ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/$target"
        echo "Performance regression $target passed (ASan/UBSan)."
    done
    exit 0
fi
if [ "${CONNECTION_ONLY:-0}" = 1 ]; then
    "${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$test_tmp_dir" -Isrc/kext tests/test_connection.cpp -o "$test_tmp_dir/connection"
    ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/connection"
    echo 'Open/WPA2 connection and disconnect tests passed (3 reconnect cycles, TX drain, key removal, peer unmap/delete, stopped/ECHO gates, missing acknowledgements, replay checks).'
    exit 0
fi
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_board_flow.cpp -o "$test_tmp_dir/flow"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/flow"
echo 'Board flow tests passed (actual method bodies, simulated CE0/ROM, ASan/UBSan).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_otp_flow.cpp -o "$test_tmp_dir/otp"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/otp"
echo 'OTP flow tests passed (actual method bodies; simulated ROM, no execution of binary).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_firmware_flow.cpp -o "$test_tmp_dir/firmware"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/firmware"
echo 'WLAN download/readback tests passed (actual methods, simulated ROM and RAM).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_qca9377_flow.cpp -o "$test_tmp_dir/qca9377"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/qca9377"
echo 'QCA9377 profile/board/OTP/WLAN regressions passed (actual production methods, independent simulated ROM loader).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_bmi_transport.cpp -o "$test_tmp_dir/bmi-transport"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/bmi-transport"
echo 'BMI transport regression passed (actual method; RX before TX, descriptor-first completion, bounce DMA, timeout, no retry).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_bmi_rom.cpp -o "$test_tmp_dir/bmi-rom"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/bmi-rom"




"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_htc_ready.cpp -o "$test_tmp_dir/htc"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/htc"
echo 'HTC READY parser tests passed (bounds, trailers, invalid resources, ASan/UBSan).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_htc_service.cpp -o "$test_tmp_dir/htc-service"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/htc-service"
echo 'HTC service ABI tests passed (frame builders, responses, endpoint bounds, ASan/UBSan).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_boot_flow.cpp -o "$test_tmp_dir/boot"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/boot"
echo 'Boot tests passed (actual methods; simulated CE1/firmware/reset, no hardware).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_htc_flow.cpp -o "$test_tmp_dir/htc-flow"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/htc-flow"
echo 'HTC control/handshake tests passed (actual methods, simulated CE0/CE1, ASan/UBSan).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_wmi_bootstrap.cpp -o "$test_tmp_dir/wmi-bootstrap"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/wmi-bootstrap"
echo 'WMI bootstrap tests passed (actual CE2 methods, parser, credits and reset ownership).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_wmi_init.cpp -o "$test_tmp_dir/wmi-init"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/wmi-init"
echo 'WMI INIT tests passed (real CE3 methods, READY and CE1/CE2 credits, reset ownership).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_wmi_runtime.cpp -o "$test_tmp_dir/wmi-runtime"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/wmi-runtime"
echo 'WMI runtime tests passed (actual receiver/lifecycle methods, simulated CE and worker/PM ordering).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_wmi_echo.cpp -o "$test_tmp_dir/wmi-echo"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/wmi-echo"
echo 'WMI ECHO tests passed (actual timer, CE3 TX, response correlation, credits and teardown).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_wmi_vdev.cpp -o "$test_tmp_dir/wmi-vdev"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/wmi-vdev"
echo 'WMI VDEV_CREATE tests passed (actual timer, ECHO gating, CE3 transport, credits and teardown).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_wmi_channels.cpp -o "$test_tmp_dir/wmi-channels"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/wmi-channels"
echo 'WMI channel-list tests passed (actual timer/CE3, VDEV gating, credits, timeout and teardown).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_wmi_scan.cpp -o "$test_tmp_dir/wmi-scan"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/wmi-scan"
echo 'WMI START_SCAN tests passed (actual TX/timer, correlated events, transactional credits, timeouts and teardown).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_wmi_bss.cpp -o "$test_tmp_dir/wmi-bss"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/wmi-bss"
echo 'WMI BSS tests passed (actual receive path, bounded cache, binary SSID, malformed RF and credits).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_wmi_radio.cpp -o "$test_tmp_dir/wmi-radio"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/wmi-radio"
echo 'WMI radio tests passed (TX/RX ordering, wire ABI, credits, timeout and DMA quarantine).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_wmi_channel_info.cpp -o "$test_tmp_dir/wmi-channel-info"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/wmi-channel-info"
echo 'WMI channel-info tests passed (38 channels, optional fields, invalid TLVs, transactional credits).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_rx_setup.cpp -o "$test_tmp_dir/rx-setup"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/rx-setup"
echo 'RX setup tests passed (filter barrier, HTT ABI/version, ring DMA, scan gating, failures and exhaustion).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_rx_refill.cpp -o "$test_tmp_dir/rx-refill"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/rx-refill"
echo 'HTT ordered RX refill tests passed (completion gating, 2200 cycles, DMA failure and malformed indications).'
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_connection.cpp -o "$test_tmp_dir/connection"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/connection"
echo 'Open STA connection and traffic tests passed (actual state machine, simulated CE/WMI/HTT).'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_reconnect_090.cpp -o "$test_tmp_dir/reconnect-090"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/reconnect-090"

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
    -I"$test_tmp_dir" -Isrc/kext tests/test_tx_watchdog.cpp -o "$test_tmp_dir/tx-watchdog"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/tx-watchdog"

# Same production connection/DMA lifecycle with the larger transmit window.
"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DATH10K_DATA_TX_SLOTS=32 -I"$test_tmp_dir" -Isrc/kext tests/test_connection.cpp -o "$test_tmp_dir/connection32"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/connection32"
echo '32-slot connection/traffic and Block ACK tests passed.'

"${CXX:-c++}" -std=c++17 "${ATH10K_HOST_TEST_OPT:--O2}" -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DATH10K_DATA_TX_SLOTS=16 -I"$test_tmp_dir" -Isrc/kext tests/test_connection.cpp -o "$test_tmp_dir/connection16"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/connection16"
echo '16/32/64-slot connection/traffic regressions passed (64 is default).'

"${CXX:-c++}" -std=c++17 -O1 -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined -I"$test_tmp_dir" -Isrc/kext tests/test_review_regressions.cpp -o "$test_tmp_dir/review-regressions"
ASAN_OPTIONS=detect_leaks=0 "$test_tmp_dir/review-regressions"
