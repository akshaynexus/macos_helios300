#!/bin/bash
# awdl-run1: can the Helios 300's QCA6174 (ath10k, WLAN.RM.4.4.1) take part in AWDL at all?
#
# Runs SEEMOO's OWL (userspace AWDL, GPL-3.0, pinned commit) on the laptop's Linux with
# wlan0 in monitor mode, on channel 44 and then channel 6, with a Mac's AirDrop window
# open nearby. Optionally runs OpenDrop ("find" and "receive") over OWL's awdl0.
#
# Run through hb; uses `asroot` only. Self-contained: builds OWL under ~/pascal-build/awdl-run1.
# Takes the Wi-Fi link down for ~4 minutes; SSH must be on Ethernet (checked first).
# Restores NetworkManager/managed mode at the end (also on Ctrl-C / error).
# Logs: ~/pascal-build/awdl-run1/log/*.txt with every MAC address replaced by a hash.
set -u
RUN="$HOME/pascal-build/awdl-run1"; LOG="$RUN/log"; mkdir -p "$LOG"
WLAN=wlan0; ETH=enp3s0f1
OWL_REPO=https://github.com/seemoo-lab/owl; OWL_COMMIT=0ee3263f944ea721c355f826aa90d636d012a512
PHASE_SECONDS=${PHASE_SECONDS:-90}

say() { printf '[awdl-run1 %s] %s\n' "$(date +%H:%M:%S)" "$*"; }

# 0. Safety: this SSH session must not depend on the Wi-Fi link.
CLIENT=${SSH_CLIENT%% *}
if ! ip route get "$CLIENT" 2>/dev/null | grep -q "dev $ETH"; then
  say "SSH client $CLIENT is not routed over $ETH; refusing to touch $WLAN"; exit 2
fi
ip -br addr show "$ETH" | grep -q UP || { say "$ETH is not UP"; exit 2; }
say "SSH is on $ETH, ok"

# 1. Build dependencies (pacman keeps them; harmless).
asroot pacman -S --needed --noconfirm libev cmake >"$LOG/pacman.txt" 2>&1 || { say "pacman failed, see log/pacman.txt"; exit 3; }

# 2. OWL at the pinned commit (with its googletest/radiotap submodules).
if [ ! -x "$RUN/owl/build/daemon/owl" ]; then
  rm -rf "$RUN/owl"
  git clone -q --recurse-submodules "$OWL_REPO" "$RUN/owl" && git -C "$RUN/owl" checkout -q "$OWL_COMMIT" \
    && git -C "$RUN/owl" submodule update -q --init || { say "owl clone failed"; exit 3; }
  (cd "$RUN/owl" && mkdir -p build && cd build && cmake .. >"$LOG/cmake.txt" 2>&1 && make -j4 owl >"$LOG/make.txt" 2>&1) \
    || { say "owl build failed, see log/make.txt"; exit 3; }
fi
OWL="$RUN/owl/build/daemon/owl"; say "owl built: $OWL"

# 3. OpenDrop (optional; AirDrop over OWL's awdl0). Failure here only skips the OpenDrop steps.
OPENDROP=""
if [ ! -x "$RUN/venv/bin/opendrop" ]; then
  python3 -m venv "$RUN/venv" >"$LOG/opendrop-install.txt" 2>&1 && "$RUN/venv/bin/pip" install -q opendrop >>"$LOG/opendrop-install.txt" 2>&1 || true
fi
[ -x "$RUN/venv/bin/opendrop" ] && OPENDROP="$RUN/venv/bin/opendrop" && say "opendrop available" || say "opendrop not available (optional), see log/opendrop-install.txt"

# 4. Restore on any exit.
OWL_PID=""
restore() {
  say "restoring $WLAN"
  [ -n "$OWL_PID" ] && kill -INT "$OWL_PID" 2>/dev/null; sleep 2; pkill -INT -x owl 2>/dev/null; pkill -x opendrop 2>/dev/null
  asroot ip link set "$WLAN" down
  asroot iw dev "$WLAN" set type managed
  asroot ip link set "$WLAN" up
  asroot nmcli dev set "$WLAN" managed yes
  asroot nmcli dev connect "$WLAN" >/dev/null 2>&1 || true
  for i in $(seq 1 20); do nmcli -t -f DEVICE,STATE dev | grep -q "^$WLAN:connected" && break; sleep 1; done
  nmcli -t -f DEVICE,STATE dev | grep "^$WLAN:" ; iw dev "$WLAN" info | grep -E 'type|channel'
}
trap restore EXIT INT TERM

# 5. Before: what Linux knows about the card.
{ iw phy | grep -E -A3 'Supported interface modes|active monitor' ; iw dev "$WLAN" info; } >"$LOG/before.txt" 2>&1

# 6. Monitor mode by hand (OWL's own path asks for *active* monitor, which this card does not
#    advertise), then OWL with -N (skip its monitor-mode setup).
asroot nmcli dev set "$WLAN" managed no
asroot ip link set "$WLAN" down
asroot iw dev "$WLAN" set type monitor || { say "monitor mode refused"; exit 4; }
asroot ip link set "$WLAN" up
iw dev "$WLAN" info >>"$LOG/before.txt"

for CH in 44 6; do
  say "phase channel $CH: owl for $PHASE_SECONDS s (owner: AirDrop window open on the Mac)"
  asroot "$OWL" -N -i "$WLAN" -c "$CH" -vv >"$LOG/owl-ch$CH.raw" 2>&1 &
  OWL_PID=$!
  sleep 15
  ip -br addr show awdl0 >>"$LOG/owl-ch$CH.raw" 2>&1 || say "no awdl0 after 15 s"
  if [ -n "$OPENDROP" ] && [ "$CH" = 44 ]; then
    say "opendrop find (30 s)"; timeout 30 asroot "$OPENDROP" find >"$LOG/opendrop-find.raw" 2>&1 || true
    say "opendrop receive (45 s; owner: send a photo from the Mac's AirDrop window to the device named by this laptop)"
    (cd "$RUN" && timeout 45 asroot "$OPENDROP" receive >"$LOG/opendrop-receive.raw" 2>&1) || true
  else
    sleep $((PHASE_SECONDS - 15))
  fi
  ip n show dev awdl0 >"$LOG/neighbors-ch$CH.raw" 2>&1 || true
  kill -INT "$OWL_PID" 2>/dev/null; wait "$OWL_PID" 2>/dev/null; OWL_PID=""
  # OWL switches channels within its sequence; recorded for the summary.
done

# 7. Hash every MAC address in the logs (owner rule: no third-party MACs on disk/in reports).
python3 - "$LOG" <<'PY'
import hashlib, re, sys, glob, os
mac = re.compile(r'\b([0-9A-Fa-f]{1,2})(:[0-9A-Fa-f]{1,2}){5}\b')
for raw in glob.glob(os.path.join(sys.argv[1], '*.raw')):
    text = open(raw, errors='replace').read()
    text = mac.sub(lambda m: 'mac#' + hashlib.sha256(m.group(0).lower().encode()).hexdigest()[:8], text)
    open(raw[:-4] + '.txt', 'w').write(text); os.remove(raw)
PY

# 8. Summary.
say "SUMMARY"
for CH in 44 6; do
  F="$LOG/owl-ch$CH.txt"
  printf 'channel %s: peers added %s, removed %s, channel switches %s, awdl0 neighbours %s\n' "$CH" \
    "$(grep -c 'add peer' "$F")" "$(grep -c 'remove peer' "$F")" "$(grep -c 'switch channel' "$F")" "$(grep -c . "$LOG/neighbors-ch$CH.txt" 2>/dev/null || echo 0)"
  grep -E 'STATISTICS|TX action|RX action|add peer|master|election' "$F" | head -20
done
[ -f "$LOG/opendrop-find.txt" ] && { echo "opendrop find:"; grep -vE '^\s*$' "$LOG/opendrop-find.txt" | tail -8; }
[ -f "$LOG/opendrop-receive.txt" ] && { echo "opendrop receive:"; tail -8 "$LOG/opendrop-receive.txt"; ls -la "$RUN"/*.jpg "$RUN"/*.jpeg "$RUN"/*.png "$RUN"/*.heic 2>/dev/null; }
say "done; logs in $LOG"
