#!/bin/sh
set -u

echo "== loaded kext =="
date
kmutil showloaded | grep -i ath10k || true

echo
echo "== Ath10kMac logs =="
date
log show --last "${ATH10K_LOG_WINDOW:-1d}" --style compact --info --debug \
  --predicate 'process == "kernel" AND eventMessage CONTAINS[c] "Ath10kMac:"' || true

echo
echo "== IORegistry =="
date
ioreg -r -c Ath10kPCIDevice -l -w 0 || true
