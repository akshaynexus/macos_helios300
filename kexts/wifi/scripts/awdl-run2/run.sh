#!/bin/bash
# awdl-run2 (run 1 + fixes: dedicated monitor vif mon0, CMake 4 policy, asroot timeout order,
# OpenDrop setuptools, all logs through the MAC hash): can the Helios 300's QCA6174 (ath10k, WLAN.RM.4.4.1) take part in AWDL at all?
#
# Runs SEEMOO's OWL (userspace AWDL, GPL-3.0, pinned commit) on the laptop's Linux with
# wlan0 in monitor mode, on channel 44 and then channel 6, with a Mac's AirDrop window
# open nearby. Optionally runs OpenDrop ("find" and "receive") over OWL's awdl0.
#
# Run through hb; uses `asroot` only. Self-contained: builds OWL under ~/pascal-build/awdl-run2.
# Takes the Wi-Fi link down for ~4 minutes; SSH must be on Ethernet (checked first).
# Restores NetworkManager/managed mode at the end (also on Ctrl-C / error).
# Logs: ~/pascal-build/awdl-run2/log/*.txt with every MAC address replaced by a hash.
set -u
RUN="$HOME/pascal-build/awdl-run2"; LOG="$RUN/log"; mkdir -p "$LOG"
WLAN=wlan0; MON=mon0; ETH=enp3s0f1
OWL_REPO=https://github.com/seemoo-lab/owl; OWL_COMMIT=0ee3263f944ea721c355f826aa90d636d012a512
PHASE_SECONDS=${PHASE_SECONDS:-90}

say() { printf '[awdl-run2 %s] %s\n' "$(date +%H:%M:%S)" "$*"; }

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
  (cd "$RUN/owl" && mkdir -p build && cd build && cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 .. >"$LOG/cmake.txt" 2>&1 && make -j4 owl >"$LOG/make.txt" 2>&1) \
    || { say "owl build failed, see log/make.txt"; exit 3; }
fi
OWL="$RUN/owl/build/daemon/owl"; say "owl built: $OWL"

# 3. OpenDrop (optional; AirDrop over OWL's awdl0). Failure here only skips the OpenDrop steps.
OPENDROP=""
if [ ! -x "$RUN/venv/bin/opendrop" ]; then
  python3 -m venv "$RUN/venv" >"$LOG/opendrop-install.txt" 2>&1 && "$RUN/venv/bin/pip" install -q 'setuptools<81' opendrop >>"$LOG/opendrop-install.txt" 2>&1 || true
fi
[ -x "$RUN/venv/bin/opendrop" ] && "$RUN/venv/bin/opendrop" --help >/dev/null 2>>"$LOG/opendrop-install.txt" && OPENDROP="$RUN/venv/bin/opendrop" && say "opendrop available" || say "opendrop not available (optional), see log/opendrop-install.txt"

# 4. Restore on any exit.
OWL_PID=""
restore() {
  say "restoring $WLAN"
  [ -n "$OWL_PID" ] && kill -INT "$OWL_PID" 2>/dev/null; sleep 2; pkill -INT -x owl 2>/dev/null; pkill -x opendrop 2>/dev/null
  asroot iw dev "$MON" del 2>/dev/null || true
  asroot ip link set "$WLAN" up
  asroot nmcli dev set "$WLAN" managed yes
  asroot nmcli dev connect "$WLAN" >/dev/null 2>&1 || true
  for i in $(seq 1 20); do nmcli -t -f DEVICE,STATE dev | grep -q "^$WLAN:connected" && break; sleep 1; done
  nmcli -t -f DEVICE,STATE dev | grep "^$WLAN:" ; iw dev "$WLAN" info | grep -E 'type|channel'
}
trap restore EXIT INT TERM

# 5. Before: what Linux knows about the card.
{ iw phy | sed -n '/Supported interface modes/,/Band 1/p'; iw phy | grep -i 'active monitor'; iw dev; } 2>&1 | grep -viE 'ssid' >"$LOG/before.raw"

# 6. A dedicated monitor vif (run 1: `iw dev wlan0 set type monitor` returned 0 but wlan0 stayed
#    managed, so OWL saw no radiotap DLT). wlan0 is released by NetworkManager and taken down;
#    mon0 is added on the same phy with a locally administered copy of wlan0's address (OWL
#    gives awdl0 that address). OWL runs with -N: its own path asks for *active* monitor mode,
#    which this card does not advertise.
PHY=$(iw dev "$WLAN" info | awk '/wiphy/ {print "phy"$2}')
HWADDR=$(cat /sys/class/net/$WLAN/address)
LADDR=$(printf '%02x%s' $(( 0x${HWADDR:0:2} | 0x02 )) "${HWADDR:2}")
asroot nmcli dev set "$WLAN" managed no
asroot ip link set "$WLAN" down
asroot iw dev "$MON" del 2>/dev/null || true
asroot iw phy "$PHY" interface add "$MON" type monitor || { say "cannot add $MON on $PHY"; exit 4; }
asroot ip link set "$MON" address "$LADDR" || say "mon0 address not set (continuing)"
asroot ip link set "$MON" up || { say "cannot bring $MON up"; exit 4; }
sleep 1
iw dev "$MON" info 2>&1 | grep -viE 'ssid' >>"$LOG/before.raw"
iw dev "$MON" info | grep -q 'type monitor' || { say "$MON is not in monitor mode"; exit 4; }
say "$MON up in monitor mode on $PHY"

for CH in 44 6; do
  say "phase channel $CH: owl for $PHASE_SECONDS s (owner: AirDrop window open on the Mac)"
  asroot "$OWL" -N -i "$MON" -c "$CH" -vv >"$LOG/owl-ch$CH.raw" 2>&1 &
  OWL_PID=$!
  sleep 15
  ip -br addr show awdl0 >>"$LOG/owl-ch$CH.raw" 2>&1 || { say "no awdl0 after 15 s"; tail -5 "$LOG/owl-ch$CH.raw"; }
  if [ -n "$OPENDROP" ] && [ "$CH" = 44 ]; then
    say "opendrop find (30 s)"; asroot timeout 30 "$OPENDROP" find >"$LOG/opendrop-find.raw" 2>&1 || true
    say "opendrop receive (45 s; owner: send a photo from the Mac's AirDrop window to the device named by this laptop)"
    (cd "$RUN" && asroot timeout 45 "$OPENDROP" receive >"$LOG/opendrop-receive.raw" 2>&1) || true
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
