#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reduce a usbmon pcap (LINKTYPE_USB_LINUX_MMAPPED) to a firmware-download trace.

  usbmon_reduce.py capture.pcap [--dev DEVNUM] > trace.txt

Output has the line format tools/linux/qca_rome_usb.c prints, so a capture
of Linux btusb and a run of our core can be diffed line by line:

  S ctrl c0 09 0000 0000 0014                    vendor IN submitted
  C ctrl c0 09 0000 0000 0014 len=20 data=<hex>  ... completed, data inline
  S ctrl 40 01 0000 0000 001c len=28 sha256=<h>  vendor OUT, payload hashed
  C ctrl 40 01 0000 0000 001c status=0 len=28
  S bulk 02 len=4096 sha256=<h>                  bulk OUT EP 0x02, hashed
  C bulk 02 status=0 len=4096

Only vendor control transfers and bulk OUT EP 0x02 become trace lines.
Payloads that could carry firmware or user data are reduced to SHA-256 (the
Qualcomm firmware is not redistributed through captures); vendor IN data
(version/status, 1-20 bytes) is kept. Everything else becomes a '#' comment:
standard requests (with setup fields), HCI command opcodes (class requests,
parameters dropped), and counts of interrupt/bulk IN traffic (HCI events and
ACL data are never printed: they contain the BD_ADDR).
"""
import argparse
import hashlib
import struct
import sys


def records(path):
    with open(path, "rb") as f:
        gh = f.read(24)
        magic, _, _, _, _, _, link = struct.unpack("<IHHiIII", gh)
        if magic != 0xA1B2C3D4 or link != 220:
            sys.exit(f"{path}: not a little-endian pcap with linktype 220")
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            sec, usec, incl, orig = struct.unpack("<IIII", rh)
            body = f.read(incl)
            hdr = body[:64]
            (uid, typ, xfer, ep, dev, bus, fsetup, fdata, tsec, tusec, status,
             len_urb, len_cap) = struct.unpack("<QBBBBHbbqiiII", hdr[:40])
            setup = hdr[40:48]
            yield dict(id=uid, type=chr(typ), xfer=xfer, ep=ep, dev=dev,
                       setup_present=(fsetup == 0), setup=setup, status=status,
                       len_urb=len_urb, data=body[64:64 + len_cap],
                       ts=tsec + tusec / 1e6)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--dev", type=int, default=None)
    a = ap.parse_args()
    pending = {}
    counts = {}
    t0 = None
    out = []
    for r in records(a.pcap):
        if a.dev is not None and r["dev"] != a.dev:
            continue
        if t0 is None:
            t0 = r["ts"]
        epaddr = r["ep"]  # bit 7 = IN
        if r["xfer"] == 2:  # control
            if r["type"] == "S":
                rt, req, wv, wi, wl = struct.unpack("<BBHHH", r["setup"])
                pending[r["id"]] = (rt, req, wv, wi, wl)
                what = f"ctrl {rt:02x} {req:02x} {wv:04x} {wi:04x} {wl:04x}"
                kind = rt & 0x60
                if kind == 0x40:
                    if rt & 0x80:
                        out.append(f"S {what}")
                    else:
                        d = r["data"]
                        if len(d) != wl:
                            out.append(f"# WARNING payload captured {len(d)} of {wl}")
                        out.append(f"S {what} len={wl} sha256={hashlib.sha256(d).hexdigest()}")
                elif kind == 0x20 and rt == 0x20 and len(r["data"]) >= 2:
                    op = r["data"][0] | (r["data"][1] << 8)
                    out.append(f"# t={r['ts'] - t0:.3f} hci-cmd opcode=0x{op:04x} plen={max(0, len(r['data']) - 3)}")
                else:
                    out.append(f"# t={r['ts'] - t0:.3f} std {what}")
            else:
                s = pending.pop(r["id"], None)
                if not s:
                    continue
                rt, req, wv, wi, wl = s
                if rt & 0x60 != 0x40:
                    if r["status"] != 0:
                        out.append(f"# std/class ctrl {rt:02x} {req:02x} status={r['status']}")
                    continue
                what = f"ctrl {rt:02x} {req:02x} {wv:04x} {wi:04x} {wl:04x}"
                if r["status"] != 0:
                    out.append(f"C {what} status={r['status']}")
                elif rt & 0x80:
                    d = r["data"]
                    out.append(f"C {what} len={r['len_urb']} data={d[:64].hex()}")
                else:
                    out.append(f"C {what} status=0 len={r['len_urb']}")
        elif r["xfer"] == 3 and epaddr == 0x02:  # bulk OUT EP2
            if r["type"] == "S":
                d = r["data"]
                if len(d) != r["len_urb"]:
                    out.append(f"# WARNING payload captured {len(d)} of {r['len_urb']}")
                out.append(f"S bulk 02 len={r['len_urb']} sha256={hashlib.sha256(d).hexdigest()}")
            else:
                if r["status"] != 0:
                    out.append(f"C bulk 02 status={r['status']}")
                else:
                    out.append(f"C bulk 02 status=0 len={r['len_urb']}")
        else:
            key = f"xfer{r['xfer']} ep{epaddr:02x} {r['type']}"
            counts[key] = counts.get(key, 0) + 1
    for line in out:
        print(line)
    for k in sorted(counts):
        print(f"# other: {k} x{counts[k]}")


if __name__ == "__main__":
    main()
