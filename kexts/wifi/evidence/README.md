# Linux cross-check evidence (Helios 300 G3-571, CachyOS, kernel 7.3.1)

Captured 2026-11-07/08 over SSH on Ethernet, on the owner's laptop running Linux
`ath10k_pci` with linux-firmware 20261019. MAC addresses, BSSIDs and SSIDs are replaced
with `<mac>` / `<ssid>`. No key material is recorded: the trace print formats carry WMI
command ids and lengths only, never buffers.

| File | What it shows |
|---|---|
| `linux-lspci-02.00.0.txt` | `168c:003e` rev `0x32`, subsystem **Lite-On `11ad:0807`**, MSI 1/8, ASPM L1 on, L1 substates capable but off, BAR0 2 MB |
| `linux-dmesg-ath10k.txt` | `qca6174 hw3.2 target 0x05030000 chip_id 0x00340aff sub 11ad:0807`; firmware `WLAN.RM.4.4.1-00309-` api 6 (features wowlan, ignore-otp, mfp); board api 2; EEPROM regdomain `0x6c` (world), later changed to US by the AP's country IE |
| `linux-debugfs-fw_checksums.txt` | crc32 of what Linux actually loaded. `board 58f39f4d` is the selected board entry, `board-N.bin d2863f91` is the whole board-2.bin |
| `linux-debugfs-wmi_services.txt` | firmware WMI services (STA power save, GTK offload, WOW, NLO, beacon-miss offload, …) |
| `linux-dmidecode.txt` | Acer Predator G3-571, BIOS V1.22. SMBIOS type 248 has no BDF variant string |
| `linux-iw-phy.txt` | phy capabilities: HT40/VHT80, 2x2, ciphers, interface modes, per-channel limits |
| `linux-scan-security-summary.txt` | security of the visible APs (RSN only): CCMP/CCMP, AKM PSK (+ FT-PSK on two), no PMF capability bits |
| `linux-ath10k-load-trace.txt` | `rmmod ath10k_pci; modprobe ath10k_pci` traced with tracefs (`ath10k_log_*`, `ath10k_wmi_cmd`, `ath10k_wmi_event`), first 20 s after probe, ids resolved to Linux `wmi-tlv.h` names. Noisy per-packet lines are dropped. A command repeated 2-3 times at the same timestamp is one command retried while waiting for HTC credits (the tracepoint is in `ath10k_wmi_cmd_send_nowait`) |

## Facts the kext depends on, as observed

- **Board key.** Linux builds `bus=pci,vendor=168c,device=003e,subsystem-vendor=11ad,subsystem-device=0807`
  (no `variant=`: "wrong smbios bdf ext type length (11)") and finds an exact match in
  `QCA6174/hw3.0/board-2.bin`. That 8124-byte entry has Linux crc32 `58f39f4d`, equal to the
  debugfs `board` checksum. `scripts/prepare-firmware.py` selects the same entry, and
  `tests/test_payload.py` asserts the crc.
- **Firmware.** firmware-6.bin crc32 `0793bcf2`; its athwlan IE (681821 B) crc32 `9986d09a`
  and OTP IE (24429 B) crc32 `3871d397` match debugfs exactly. These are the bytes the kext embeds.
- **Boot order (trace +0.20 s … +0.35 s).** BMI get target info (version `0x05030000`). OTP run
  with param `0x10` for the board id, which returns board id 0 and is ignored. PLL setup through
  SOC registers `0xaf4`/`0x1018`/`0x914`/`0x1014`/`0x820`. Host-interest writes. Board written to
  `0x401fc0` (8124 B), `0x400858` set. OTP run with param 0, result 3: "calibration mode otp".
  Firmware download (Linux uses CE7 "fast diag"; the kext uses BMI LZ, which ath10k also supports).
  BMI done. HTC ready with 2 credits of 1792 B. Services HTT Data (ul 4 / dl 1) and WMI (ul 3 / dl 2).
- **WMI bring-up.** SERVICE_AVAILABLE, SERVICE_READY → INIT (220 B) → READY → ECHO →
  PKTLOG_DISABLE → SCAN_PROB_REQ_OUI → RESMGR_ADAPTIVE_OCS → SCAN_CHAN_LIST (1052 B) →
  PDEV_SET_REGDOMAIN → VDEV_CREATE (STA) → STA power-save params / WMM / U-APSD → START_SCAN
  (+6.2 s) → VDEV_START → PEER_ASSOC → VDEV_UP → INSTALL_KEY (+10.55 s).
  Linux initialises the firmware twice at probe: a first boot, PDEV_SUSPEND, then a full boot
  on interface up.
