# Test fixtures

Real captures from the Helios 300 (macOS 26.7.2 25G332), sanitized or already free of personal data:

| file | from |
| --- | --- |
| `ioreg-PG106-notready.txt`, `kmutil-showloaded.txt`, `nvram-boot-args.txt` | the 2026-11-07 macOS evidence capture (`ioreg -r -c PG106`, `kmutil showloaded`, `nvram boot-args`) |
| `ioreg-AirportAth10k-run7.txt`, `ioreg-Ath10kPCIDevice-run7.txt`, `ioreg-tree-run6.txt` | `kexts/wifi/evidence/macos/run7-join-tx-dead.txt` and `run6-join-extended-bss-info.txt` (sanitized by `kexts/wifi/evidence/macos/sanitize.py`) |
| `config-internal.plist` | the internal EFI's `config.plist` (2026-11-07); PlatformInfo serial, MLB, ROM and SystemUUID replaced with placeholders |
| `manifest-0.1.0.json` | the real v0.1.0 release manifest (`dist/manifest.json`) |
| `dortania-latest-subset.json` | four products of Dortania's build-repo `latest.json` (2026-11-08) |

Generated:

- `manifest-next.json`: the current `make_manifest.py` output for the 0.1.0 files plus this checkout's scripts and
  a placeholder app (`../regen-fixture-manifest.py`; rerun it after a spec.json change).

Synthetic: the Mac was off when the app was written, so these follow the tools' documented output format
(no real capture yet):

- `pmset-g-*.txt`, `sp-audio.txt`, `sp-bluetooth.txt` (made-up addresses);
- `diskutil-*.plist`, `nvram-boot-path-*.txt` (made-up UUIDs);
- `github-releases.json`.

Replace them with real captures from the Mac when it is back: `pmset -g`, `system_profiler -detailLevel mini
SPAudioDataType SPBluetoothDataType` (sanitize the addresses), `diskutil list -plist internal physical`,
`diskutil info -plist disk0s1`, `nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:boot-path`.
