#!/usr/bin/env python3
"""Sanitize a macOS Wi-Fi log for the repository.

usage: sanitize.py IN OUT [--ssid NAME ...]

- every MAC address (BSSIDs, the card's address, the Private Wi-Fi Address)
  becomes 02:00:00:00:00:NN, numbered by first appearance across one run
  (all-zero and broadcast stay); the same address keeps its number in every
  file of one invocation series via the MAP file next to OUT (not committed);
- the SSIDs named with --ssid (never stored here) and every non-empty
  ssid="..." value become SSID-A, SSID-B, ...;
- the host name, the user name and private IPv4 addresses are replaced;
- a .panic report's JSON panicString is unescaped to plain text.
The output is checked: no input SSID, MAC or user name may remain.
"""
import json, re, sys
from pathlib import Path

def main():
    args = sys.argv[1:]
    if len(args) < 2: sys.exit(__doc__)
    src, dst = Path(args[0]), Path(args[1])
    ssids = [args[i + 1] for i, a in enumerate(args) if a == '--ssid']
    text = src.read_text(errors='replace')
    if src.suffix == '.panic':
        lines = text.splitlines()
        body = json.loads(lines[1]) if len(lines) > 1 else {}
        text = lines[0] + '\n\n' + (body.get('macOSPanicString') or body.get('panicString', ''))
    mapfile = dst.parent / '.sanitize-map.json'
    state = json.loads(mapfile.read_text()) if mapfile.exists() else {'mac': {}, 'ssid': {}}
    def mac(m):
        v = m.group(0).lower().replace('-', ':')
        if v in ('00:00:00:00:00:00', 'ff:ff:ff:ff:ff:ff'): return m.group(0)
        n = state['mac'].setdefault(v, len(state['mac']) + 1)
        return '02:00:00:00:00:%02x' % n
    def ssid_name(name):
        n = state['ssid'].setdefault(name, len(state['ssid']))
        return 'SSID-' + chr(ord('A') + n)
    for s in sorted(ssids, key=len, reverse=True):
        text = re.sub(re.escape(s), lambda m: ssid_name(s), text, flags=re.I)
    text = re.sub(r'ssid="([^"]+)"', lambda m: 'ssid="%s"' % (m.group(1) if m.group(1).startswith('SSID-') else ssid_name(m.group(1))), text)
    text = re.sub(r'\b([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b', mac, text)
    text = re.sub(r'\b[A-Za-z0-9]+s-MacBook-Pro\b', 'HOST', text)
    text = re.sub(r'/Users/[A-Za-z0-9._-]+', '/Users/USER', text)
    text = re.sub(r'\bravi\b', 'USER', text)
    text = re.sub(r'\b(192\.168|10\.\d+|172\.(1[6-9]|2\d|3[01]))\.\d+\.\d+\b', '192.0.2.x', text)
    for s in ssids:
        assert s.lower() not in text.lower(), 'SSID left in output'
    for m in re.finditer(r'\b([0-9a-f]{2}:){5}[0-9a-f]{2}\b', text.lower()):
        assert m.group(0).startswith('02:00:00:00:00:') or m.group(0) in ('00:00:00:00:00:00', 'ff:ff:ff:ff:ff:ff'), 'MAC left'
    assert not re.search(r'\bravi\b', text), 'user name left'
    dst.write_text(text)
    mapfile.write_text(json.dumps(state))

main()
