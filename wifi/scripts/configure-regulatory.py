#!/usr/bin/env python3
"""Configure country, installation environment and the existing 10 dBm ceiling."""
import argparse
import os
from pathlib import Path
import plistlib
import re
import tempfile


def configure(path, country='00', indoor=False, power=10, station_dfs=True, beacon_timeout=30000):
    data = (Path(__file__).resolve().parents[1] / 'src/kext/Ath10kRegulatoryData.hpp').read_text()
    countries = data.split('kRegulatoryCountries[]={')[1].split('};')[0]
    codes = set(re.findall(r'"([A-Z0-9]{2})"', countries))
    if country not in codes:
        raise ValueError('País desconhecido; use código ISO de duas letras ou 00.')
    if type(power) is not int or not 1 <= power <= 10 or type(indoor) is not bool:
        raise ValueError('Potência deve ser de 1 a 10 dBm; indoor deve ser booleano.')
    if type(station_dfs) is not bool or type(beacon_timeout) is not int or not 10000 <= beacon_timeout <= 120000:
        raise ValueError('DFS deve ser booleano; timeout do AP deve ser de 10000 a 120000 ms.')
    path = Path(path)
    obj = plistlib.loads(path.read_bytes())
    if obj.get('CFBundleIdentifier') != 'br.com.jonasw8.driver.ath10kmac':
        raise ValueError('Info.plist não pertence ao Ath10kMac.')
    for name in ('QCA6174', 'QCA9377'):
        p = obj['IOKitPersonalities'][name]
        p['Ath10kCountryCode'] = country
        p['Ath10kIndoor'] = indoor
        p['Ath10kTxPowerDbm'] = power
        p['Ath10kStationDFS'] = station_dfs
        p['Ath10kBeaconTimeoutMs'] = beacon_timeout
    fd, temporary = tempfile.mkstemp(prefix='ath10k-regulatory-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as out:
            out.write(plistlib.dumps(obj, sort_keys=False));out.flush();os.fsync(out.fileno())
        os.chmod(temporary, path.stat().st_mode & 0o777)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--country', default='00')
    parser.add_argument('--indoor', action='store_true')
    parser.add_argument('--power-dbm', type=int, default=10)
    parser.add_argument('--no-station-dfs', action='store_true')
    parser.add_argument('--beacon-timeout-ms', type=int, default=30000)
    parser.add_argument('--plist', type=Path, default=Path(__file__).resolve().parents[1] / 'Ath10kMac.kext/Contents/Info.plist')
    args = parser.parse_args()
    try:
        configure(args.plist, args.country, args.indoor, args.power_dbm, not args.no_station_dfs, args.beacon_timeout_ms)
    except (ValueError, KeyError, OSError, plistlib.InvalidFileException) as error:
        parser.error(str(error))
    print(f'País {args.country}, indoor={args.indoor}, teto {args.power_dbm} dBm. Compile e reinstale para aplicar.')


if __name__ == '__main__':
    main()
