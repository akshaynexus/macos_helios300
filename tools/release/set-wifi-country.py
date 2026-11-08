#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""set-wifi-country.py — bake the Wi-Fi regulatory country into the BUILT Ath10kMac.kext (release time).

    python3 -I tools/release/set-wifi-country.py --plist <stage>/Ath10kMac.kext/Contents/Info.plist \
        --regdb <self>/kexts/wifi/src/kext/Ath10kRegulatoryData.hpp --country IN

Ath10kPCIDevice::start reads Ath10kCountryCode from its IOKit personality; the boot-arg ath10k_cc=XX still wins
over it. A boot-arg is the wrong carrier for a release default: 1401 leaves boot-args out of NVRAM → Delete on
purpose (policy 2c), so a boot-arg written into a new config.plist never reaches a Mac that already has
boot-args in NVRAM. The personality key travels inside the kext that OpenCore injects, so the country is what
the release says on every boot. Only Ath10kCountryCode is written (kexts/wifi/scripts/configure-regulatory.py,
the developer tool, also resets indoor/TX power/DFS/beacon timeout; the release keeps the kext's defaults for
those). The code must be one of the wireless-regdb countries compiled into the kext; "00" is the world domain.
"""
import argparse
import os
import plistlib
import re
import sys
import tempfile

BUNDLE_ID = "br.com.jonasw8.driver.ath10kmac"


def countries(regdb):
    with open(regdb, encoding="utf-8") as fh:
        data = fh.read()
    try:
        table = data.split("kRegulatoryCountries[]={")[1].split("};")[0]
    except IndexError:
        raise SystemExit(f"set-wifi-country: no kRegulatoryCountries table in {regdb}") from None
    return set(re.findall(r'"([A-Z0-9]{2})"', table))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plist", required=True)
    ap.add_argument("--regdb", required=True)
    ap.add_argument("--country", required=True)
    a = ap.parse_args(argv)
    cc = a.country.upper()
    if cc not in countries(a.regdb):
        print(f"set-wifi-country: FAIL {a.country!r} is not a wireless-regdb country in this Ath10kMac (use ISO 3166 "
              "alpha-2, or 00 for world rules)")
        return 1
    with open(a.plist, "rb") as fh:
        d = plistlib.load(fh)
    if d.get("CFBundleIdentifier") != BUNDLE_ID:
        print(f"set-wifi-country: FAIL {a.plist} is not Ath10kMac's Info.plist")
        return 1
    pers = d.get("IOKitPersonalities") or {}
    if not pers:
        print("set-wifi-country: FAIL Ath10kMac has no IOKitPersonalities")
        return 1
    for p in pers.values():
        p["Ath10kCountryCode"] = cc
    fd, tmp = tempfile.mkstemp(prefix=".wifi-country-", dir=os.path.dirname(os.path.abspath(a.plist)))
    try:
        with os.fdopen(fd, "wb") as out:
            plistlib.dump(d, out, sort_keys=False)
        os.chmod(tmp, os.stat(a.plist).st_mode & 0o777)
        os.replace(tmp, a.plist)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    print(f"set-wifi-country: Ath10kCountryCode={cc} in {len(pers)} personalit{'y' if len(pers) == 1 else 'ies'} "
          f"({', '.join(sorted(pers))}); boot-arg ath10k_cc=XX still overrides it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
