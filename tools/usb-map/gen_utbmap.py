#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""gen_utbmap.py: build a USBToolBox map kext (UTBMap.kext) from a port table.

  gen_utbmap.py [--ports helios300-ports.json] OUTDIR

The kext is code-less: Info.plist only, one personality per controller that merges a "ports" dictionary into
the xHCI device for USBToolBox.kext to apply. Exit 0 written, 1 refused. The table format and the Helios's
evidence are in docs/USB-MAP.md.
"""
import argparse
import json
import os
import plistlib
import shutil
import struct
import sys

# Apple's limit: a controller with more ports than this loses the ones past it.
MAX_PORTS = 15
# USB connector types macOS takes from UsbConnector: 0 USB 2 A, 3 USB 3 A, 9/10 USB-C, 255 internal.
CONNECTORS = (0, 3, 9, 10, 255)
BUNDLE_ID = "io.github.akshaynexus.helios300.UTBMap"


class TableError(Exception):
    pass


def build_plist(table):
    ports = table.get("ports") or []
    pci_id = table.get("controller", {}).get("pci_id", "")
    if not pci_id.startswith("0x") or len(pci_id) != 10:
        raise TableError("controller.pci_id must be like 0xa12f8086, got %r" % pci_id)
    if not ports:
        raise TableError("no ports in the table")
    if len(ports) > MAX_PORTS:
        raise TableError("%d ports: macOS allows %d per controller" % (len(ports), MAX_PORTS))
    entries, numbers = {}, set()
    for p in ports:
        name, port, connector = p.get("name"), p.get("port"), p.get("connector")
        if not isinstance(name, str) or not name or name in entries:
            raise TableError("bad or repeated port name %r" % (name,))
        if not isinstance(port, int) or not 1 <= port <= 255 or port in numbers:
            raise TableError("%s: bad or repeated port number %r" % (name, port))
        if connector not in CONNECTORS:
            raise TableError("%s: connector %r is not one of %r" % (name, connector, CONNECTORS))
        numbers.add(port)
        entries[name] = {
            "port": struct.pack("<I", port),
            "UsbConnector": connector,
            "#comment": p.get("what", ""),
        }
    personality = {
        "CFBundleIdentifier": "com.dhinakg.USBToolBox.kext",
        "IOClass": "USBToolBox",
        "IOMatchCategory": "USBToolBox",
        "IOPCIPrimaryMatch": pci_id,
        "IOProviderClass": "IOPCIDevice",
        "IOProviderMergeProperties": {"ports": entries, "port-count": struct.pack("<I", max(numbers))},
    }
    return {
        "CFBundleDevelopmentRegion": "English",
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "UTBMap",
        "CFBundlePackageType": "KEXT",
        "CFBundleShortVersionString": "1.0",
        "CFBundleSignature": "????",
        "CFBundleVersion": "1.0",
        "IOKitPersonalities": {"Controller-0": personality},
        "OSBundleLibraries": {"com.dhinakg.USBToolBox.kext": "1.0.0"},
        "OSBundleRequired": "Root",
    }


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--ports", default=os.path.join(here, "helios300-ports.json"))
    ap.add_argument("out")
    args = ap.parse_args(argv)
    try:
        with open(args.ports) as f:
            info = build_plist(json.load(f))
    except (OSError, ValueError, TableError) as e:
        print("gen_utbmap: %s" % e, file=sys.stderr)
        return 1
    bundle = os.path.join(args.out, "UTBMap.kext")
    shutil.rmtree(bundle, ignore_errors=True)
    os.makedirs(os.path.join(bundle, "Contents"))
    with open(os.path.join(bundle, "Contents", "Info.plist"), "wb") as f:
        plistlib.dump(info, f)
    print("gen_utbmap: %s, %d ports" % (bundle, len(info["IOKitPersonalities"]["Controller-0"]["IOProviderMergeProperties"]["ports"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
