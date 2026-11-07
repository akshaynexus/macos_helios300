#!/bin/sh
# Read-only boot diagnostics; does not load, unload, reset or configure hardware.
set -u
cd "$(dirname "$0")/.."
date
echo '== Ath10kMac 0.0.89 boot diagnostics =='
binary="${ATH10K_TEST_KEXT:-build/out/Ath10kMac.kext}"
if [ -f "$binary/Contents/Info.plist" ]; then
    /usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$binary/Contents/Info.plist" || true
fi
if [ -f "$binary/Contents/MacOS/Ath10kMac" ]; then
    dwarfdump --uuid "$binary/Contents/MacOS/Ath10kMac" || true
    strings "$binary/Contents/MacOS/Ath10kMac" | grep -E 'post-chip-reset|BMI CE-fault|BMI ROM pre-submit|PLL %s|INTx prepared' || true
fi
echo '== Loaded kext =='
kmutil showloaded | grep -i ath10k || true
echo '== IORegistry, including CE pass count, BMI identity, MISC_IS and ROM evidence =='
ioreg -r -c Ath10kPCIDevice -l -w 0 || true
echo '== Kernel log =='
log show --last "${ATH10K_LOG_WINDOW:-1h}" --style compact --info --debug \
    --predicate 'process == "kernel" AND eventMessage CONTAINS[c] "Ath10kMac:"' || true
echo '== dmesg fallback =='
dmesg | grep -i Ath10kMac || true
