#!/usr/bin/env python3
"""Fail the build on unsupported weak/runtime imports; never patch the Mach-O."""
import argparse
from pathlib import Path
import subprocess
import sys


def rejected_symbols(listing):
    return [line.strip() for line in listing.splitlines()
            if '(undefined)' in line.lower() and
            ('weak' in line.lower() or 'chkstk' in line or 'stack_chk' in line)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path)
    parser.add_argument('--listing', action='store_true', help='Read an existing nm -m text report')
    args = parser.parse_args()
    try:
        if args.listing:
            listing = args.path.read_text()
        else:
            result = subprocess.run(['xcrun', 'nm', '-m', str(args.path)],
                                    check=True, text=True, capture_output=True)
            listing = result.stdout
        if not listing.strip():
            raise ValueError('empty symbol table')
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Cannot verify kext symbols: {error}', file=sys.stderr)
        return 2
    errors = rejected_symbols(listing)
    if errors:
        print('Kext symbol validation FAILED:', file=sys.stderr)
        print('\n'.join(errors), file=sys.stderr)
        print('Do not load this binary. Rebuild with the kernel flags and inspect nm output.', file=sys.stderr)
        return 1
    print('Kext symbols OK: no weak undefined imports or undefined stack runtime probes.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
