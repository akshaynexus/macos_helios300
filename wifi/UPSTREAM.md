# Upstream

`wifi/` starts from a snapshot of **Ath10kMac**, GPL-3.0 (see `LICENSE`):

| | |
|---|---|
| Upstream | https://github.com/thekunalthakur/Ath10kMac (fork of https://github.com/jonasw8/Ath10kMac) |
| Commit | `3954e87dbe4b6991ed3cd935818263caff444f35` (2026-10-28, "docs: explain QCA9377 board-profile porting") |
| Authors | Jonas Wander (jonasw8, original driver and app), Kunal Thakur (QCA9377 profile, TKIP group keys, DMA/sleep fixes) |
| Imported | 2026-11-07, `git archive` of that commit |

Not imported:

- `MacKernelSDK/` (submodule with a prebuilt `libkmod.a`). We build against the
  macOS 26.5 SDK's `Kernel.framework` and its `usr/lib/libkmod*.a`.
- `firmware/*.bin`, `firmware/prepared/`, `firmware/QCA9377/hw1.0/*.bin` (Qualcomm
  firmware blobs) and `src/kext/Ath10kPayload.hpp`, a 9 MB header generated from those
  blobs. The build fetches pinned linux-firmware files and checks their SHA-256 instead
  (`scripts/fetch-firmware.sh`).
- `.github/` (macOS GitHub Actions release workflow) and `SOURCE-HASHES-current.json`
  (hashes of the upstream tree, which no longer applies after our changes).

The snapshot is committed unchanged, apart from those exclusions, in the first `wifi:` commit,
so `git log -p -- wifi/` shows every change made here on top of upstream.

Protocol details added or changed here cite the Linux ath10k driver (ISC licence),
`drivers/net/wireless/ath/ath10k/` at torvalds/linux
`d48723711bae9b8cdc56719c448ba42fef28cbee` (2026-11-06).

Regulatory data (`regulatory/`) is upstream's: wireless-regdb (ISC) and Linux `ath/regd*.h`
(ISC), with sources in `regulatory/SOURCE.md`.
