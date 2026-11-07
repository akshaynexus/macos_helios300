#!/usr/bin/env python3
"""Configure an explicit OPEN test AP before building; never edits a loaded kext."""
import argparse
import os
from pathlib import Path
import plistlib
import re
import tempfile


def configure(path, ssid=None, bssid=None, power=10, disable=False):
    if not disable:
        encoded = ssid.encode('utf-8') if ssid is not None else b''
        if not 1 <= len(encoded) <= 32:
            raise ValueError('SSID deve ocupar de 1 a 32 bytes UTF-8')
        if not bssid or not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', bssid):
            raise ValueError('BSSID deve ter o formato 02:11:22:33:44:55')
        address = bytes.fromhex(bssid.replace(':', ''))
        if not any(address) or address[0] & 1:
            raise ValueError('BSSID deve ser unicast e diferente de zero')
        if not isinstance(power, int) or not 1 <= power <= 10:
            raise ValueError('Potência de teste deve estar entre 1 e 10 dBm')
    path = Path(path)
    obj = plistlib.loads(path.read_bytes())
    if obj.get('CFBundleIdentifier') != 'br.com.jonasw8.driver.ath10kmac':
        raise ValueError('Info.plist não pertence ao Ath10kMac')
    personalities = obj['IOKitPersonalities']
    for name in ('QCA6174', 'QCA9377'):
        p = personalities[name]
        p['Ath10kOpenConnect'] = not disable
        for key in ('Ath10kConnectSSID', 'Ath10kConnectBSSID', 'Ath10kConnectPowerHalfDbm'):
            p.pop(key, None)
        if not disable:
            p['Ath10kConnectSSID'] = encoded
            p['Ath10kConnectBSSID'] = address
            p['Ath10kConnectPowerHalfDbm'] = power * 2
    # Atomic update, preserving permissions. There is no network operation here.
    fd, temporary = tempfile.mkstemp(prefix='ath10k-config-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as out:
            out.write(plistlib.dumps(obj, sort_keys=False))
            out.flush()
            os.fsync(out.fileno())
        os.chmod(temporary, path.stat().st_mode & 0o777)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ssid')
    parser.add_argument('--bssid')
    parser.add_argument('--power-dbm', type=int, default=10)
    parser.add_argument('--disable', action='store_true')
    parser.add_argument('--plist', type=Path, default=Path(__file__).resolve().parents[1] / 'Ath10kMac.kext/Contents/Info.plist')
    args = parser.parse_args()
    if args.disable and (args.ssid is not None or args.bssid is not None):
        parser.error('--disable não aceita SSID/BSSID')
    try:
        configure(args.plist, args.ssid, args.bssid, args.power_dbm, args.disable)
    except (ValueError, KeyError, OSError, plistlib.InvalidFileException) as error:
        parser.error(str(error))
    print('Conexão desativada.' if args.disable else 'AP aberto configurado. Compile a kext novamente; a configuração entra em vigor após instalar e reiniciar.')


if __name__ == '__main__':
    main()
