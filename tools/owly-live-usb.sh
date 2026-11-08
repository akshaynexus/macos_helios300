#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# owly-live-usb.sh: the cheapest proof of the radio side of AWDL on the Helios 300's QCA6174.
#
# Runs Owly (https://github.com/t4t5/owly, MIT) on a Linux live USB booted on the laptop,
# because Owly already does real AirDrop with iPhones in software. It answers the question
# the macOS driver cannot answer on its own:
#
#   Can this card (ath10k_pci, QCA6174, WLAN.RM.4.4.1) inject AWDL frames while associated,
#   hear the phone's frames and ACK its unicast?
#
# What it does not test: anything on macOS. It tells us whether the firmware can do it, so
# the macOS work has a known-good radio to aim at.
#
# Usage (as your normal user on the live USB, Wi-Fi joined with NetworkManager):
#   ./owly-live-usb.sh bundle DIR     on the build machine: make a tarball with owly, owlyd and packaging
#   ./owly-live-usb.sh setup [TAR]    on the live USB: install the bundle (or build from source with cargo)
#   ./owly-live-usb.sh test [shared|exclusive] [PHONE]   run `owly test-card` and write results here
#   ./owly-live-usb.sh report DIR     print and tar a result directory
#
# The live USB needs: NetworkManager and an AP on channel 6, 44 or 149 (shared mode). The iPhone
# must have AirDrop set to "Everyone for 10 Minutes" and be near. Exclusive mode needs no AP but
# turns Wi-Fi off while it runs.
#
# A CachyOS or Arch live USB matches this repo's build machine (glibc); on another distribution
# build from source with `setup` (needs Rust 1.88+ and libdbus-free cargo, about 4 minutes).
set -euo pipefail

COUNTRY="${COUNTRY:-IN}"                # the regulatory domain to set; 5 GHz is no-IR in the world domain
STATION="${STATION:-}"                  # default: first managed interface
OUT_BASE="${OUT_BASE:-$PWD}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OWLY_SRC_URL="${OWLY_SRC_URL:-https://github.com/t4t5/owly}"

die() { echo "owly-live-usb: $*" >&2; exit 1; }
need_root() { [ "$(id -u)" = 0 ] || die "this step needs root (sudo)"; }
station_iface() {
    if [ -n "$STATION" ]; then echo "$STATION"; return; fi
    iw dev 2>/dev/null | awk '$1=="Interface"{i=$2} $1=="type" && $2=="managed"{print i; exit}'
}

cmd_bundle() {
    local out="${1:?usage: bundle DIR}"
    mkdir -p "$out"
    local src="${OWLY_TREE:-$HOME/pascal-build/awdl2/owly-build}"
    [ -x "$src/target/release/owly" ] && [ -x "$src/target/release/owlyd" ] ||
        die "no release build in $src (cargo build --release --locked -p owlyd -p owly-cli)"
    local stage; stage="$(mktemp -d)"
    mkdir -p "$stage/owly/target/release"
    cp "$src/target/release/owly" "$src/target/release/owlyd" "$stage/owly/target/release/"
    cp -R "$src/packaging" "$stage/owly/"
    cp "$src/LICENSE" "$stage/owly/LICENSE.owly"
    tar -czf "$out/owly-live.tar.gz" -C "$stage" owly
    rm -rf "$stage"
    (cd "$out" && sha256sum owly-live.tar.gz)
    echo "bundle: $out/owly-live.tar.gz (copy it next to this script on the USB)"
}

cmd_setup() {
    need_root
    local tar="${1:-$HERE/owly-live.tar.gz}"
    local work=/opt/owly-live
    mkdir -p "$work"
    if [ -f "$tar" ]; then
        tar -xzf "$tar" -C "$work"
    else
        command -v cargo >/dev/null || die "no bundle at $tar and no cargo; run 'bundle' on the build machine, or install Rust 1.88+"
        command -v git >/dev/null || die "git needed to clone $OWLY_SRC_URL"
        git clone --depth 1 "$OWLY_SRC_URL" "$work/owly"
        (cd "$work/owly" && cargo build --release --locked -p owlyd -p owly-cli)
    fi
    (cd "$work/owly" && TARGET="$work/owly/target" sh packaging/install.sh owly)
    systemctl daemon-reload || true
    nmcli general reload conf 2>/dev/null || true
    # untested card: let owlyd run it, in the mode the test asks for
    mkdir -p /etc/owly
    cat > /etc/owly/radio.toml <<EOF
# written by owly-live-usb.sh
allow_untested = true
country = "$COUNTRY"
EOF
    rfkill unblock wifi || true
    echo "setup done: owly $(/usr/bin/owly --version 2>/dev/null || true)"
    echo "next: join a Wi-Fi network on channel 6, 44 or 149, then ./owly-live-usb.sh test shared"
}

# drivers.toml override for ath10k_pci: the '*' default is shared-mode, reconnect after teardown.
drivers_override() {
    local mode="$1"
    sudo tee /etc/owly/drivers.toml >/dev/null <<EOF
# written by owly-live-usb.sh for the QCA6174 test
[drivers.ath10k_pci]
status = "untested"
mode = "$mode"
reconnect_after_monitor = true
notes = "Helios 300 QCA6174, WLAN.RM.4.4.1, under test."
EOF
}

cmd_test() {
    local mode="${1:-shared}"; local phone="${2:-}"
    case "$mode" in shared|exclusive) ;; *) die "mode is shared or exclusive";; esac
    command -v owly >/dev/null || die "owly not installed; run setup first"
    local out; out="$OUT_BASE/awdl-live-$mode-$(date +%Y%m%d-%H%M%S)"; mkdir -p "$out"
    local iface; iface="$(station_iface)"; [ -n "$iface" ] || die "no managed Wi-Fi interface (is the card up?)"
    {
        echo "== kernel"; uname -a
        echo "== pci"; lspci -nnk 2>/dev/null | grep -A3 -i '168c:' || true
        echo "== ath10k"; (modinfo ath10k_pci 2>/dev/null | grep -E '^(filename|firmware:)') || true
        dmesg 2>/dev/null | grep -i -E 'ath10k.*(firmware|board|hardware|phy)' | head -20 || true
        echo "== interface $iface"; iw dev "$iface" info
        echo "== link"; iw dev "$iface" link
        echo "== phy"; iw phy | grep -E -A12 'Supported interface modes|valid interface combinations' | head -60
        echo "== reg"; iw reg get | head -10
    } > "$out/environment.txt" 2>&1 || true
    if [ "$mode" = shared ]; then
        local ch; ch="$(iw dev "$iface" info 2>/dev/null | awk '/channel/{print $2; exit}')"
        case "$ch" in 6|44|149) echo "station on channel $ch: shared mode can run" ;;
            *) die "station is on channel '$ch'; join an AP on 6, 44 or 149 first" ;;
        esac
    fi
    sudo tee /etc/owly/radio.toml >/dev/null <<EOF
# written by owly-live-usb.sh
allow_untested = true
country = "$COUNTRY"
EOF
    drivers_override "$mode"
    sudo systemctl restart owlyd 2>/dev/null || true
    sleep 2
    echo "== owly doctor (saved in $out/doctor.txt)"
    owly doctor | tee "$out/doctor.txt" || true
    local args=(--out "$out" --watch 120)
    [ -n "$phone" ] && args+=(--phone "$phone")
    echo "== owly test-card ($mode). Open AirDrop on the iPhone, Everyone for 10 Minutes; follow the prompts."
    set +e
    owly test-card "${args[@]}" 2>&1 | tee "$out/test-card.log"
    local rc=${PIPESTATUS[0]}
    set -e
    journalctl -u owlyd --no-pager -n 300 > "$out/owlyd.log" 2>/dev/null || true
    dmesg 2>/dev/null | grep -i ath10k | tail -60 > "$out/dmesg-ath10k.txt" || true
    cmd_report "$out"
    echo "owly test-card exit code: $rc"
}

cmd_report() {
    local out="${1:?usage: report DIR}"
    echo
    echo "=== Reading the result ($out)"
    echo "owly-test-*.json is Owly's report; share the tarball with the agent."
    if ls "$out"/owly-test-*.json >/dev/null 2>&1; then
        grep -hE '"(status|mode|channel)"|"step"|"result"' "$out"/owly-test-*.json | head -40 || true
    fi
    echo
    cat <<'EOF'
What the outcomes mean:
  - shared works (peers seen, photos both ways): this firmware can inject while associated, hear the
    phone's AWDL frames and ACK them. The macOS driver's shared mode has a working radio to match.
  - shared: test-card fails at "wait for peers" / doctor says it cannot add a monitor interface:
    the firmware or ath10k cannot do monitor+station injection here. Try exclusive.
  - exclusive works but shared does not: fall back to exclusive mode (Wi-Fi off while AirDrop is on);
    we then implement the same in the macOS driver (ad-hoc/hidden-AP ACK).
  - nothing works: no monitor injection on this firmware at all. Stop here and report; a different
    card or firmware is the fix.
Keep the "Probe" and "Link" sections of test-card.log: they name the mode, channel and firmware.
EOF
    tar -czf "$out.tar.gz" -C "$(dirname "$out")" "$(basename "$out")"
    echo "wrote $out.tar.gz"
}

case "${1:-}" in
    bundle) shift; cmd_bundle "$@";;
    setup)  shift; cmd_setup "$@";;
    test)   shift; cmd_test "$@";;
    report) shift; cmd_report "$@";;
    *) sed -n '2,/^set -euo/p' "$0" | sed -e 's/^# \{0,1\}//' -e '$d'; exit 2;;
esac
