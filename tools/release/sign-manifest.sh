#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# sign-manifest.sh <manifest.json> | --public    the release's Ed25519 signature (docs/MANIFEST.md "Trust chain").
#
# Writes <manifest.json>.sig: the 64 raw bytes of an Ed25519 signature over the exact bytes of manifest.json. Helios 300
# Setup.app refuses an update unless this verifies against the public key compiled into it.
#
# The private key is $HELIOS_SIGNING_KEY or ~/.config/helios300-release/manifest-ed25519, a PEM file that is never
# committed. When it does not exist it is created (mode 0600) and this says so. The public half is
# tools/release/manifest-ed25519.pub (hex) and ManifestVerifier.releaseKeyHex in the app: a key that is not that one
# signs a release the app will refuse, which is fine for a local or 1401 source build and never for a published one.
# --public prints the current key's public half as hex, for committing after a deliberate key change.
# Exit: 0 signed (or printed), 1 failure.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
KEY="${HELIOS_SIGNING_KEY:-$HOME/.config/helios300-release/manifest-ed25519}"
COMMITTED="$HERE/manifest-ed25519.pub"
die() { echo "sign-manifest: FAIL — $*" >&2; exit 1; }
command -v openssl >/dev/null || die "openssl not found"

if [ ! -f "$KEY" ]; then
	( umask 077; mkdir -p "$(dirname "$KEY")"; openssl genpkey -algorithm ed25519 -out "$KEY" ) || die "could not create $KEY"
	chmod 600 "$KEY"
	cat >&2 <<NOTICE
sign-manifest: NOTICE — created a NEW release signing key at $KEY (mode 0600).
  Back it up somewhere safe and offline. It is never committed. Anyone holding it can sign an update the app accepts.
  If this is not the key in tools/release/manifest-ed25519.pub, the app will refuse what it signs.
NOTICE
fi
[ "$(stat -c %a "$KEY" 2>/dev/null || stat -f %Lp "$KEY")" = 600 ] || die "$KEY must be mode 0600"

public_hex="$(openssl pkey -in "$KEY" -pubout -outform DER | tail -c 32 | od -An -v -tx1 | tr -d ' \n')"
[ "${#public_hex}" = 64 ] || die "$KEY is not an Ed25519 key"
if [ "${1:-}" = --public ]; then echo "$public_hex"; exit 0; fi

MANIFEST="${1:?usage: sign-manifest.sh <manifest.json> | --public}"
[ -f "$MANIFEST" ] || die "no $MANIFEST"
SIG="$MANIFEST.sig"
openssl pkeyutl -sign -rawin -inkey "$KEY" -in "$MANIFEST" -out "$SIG.part" || die "openssl could not sign $MANIFEST"
[ "$(wc -c <"$SIG.part")" = 64 ] || die "the signature is not 64 bytes"
mv "$SIG.part" "$SIG"
# Check it the way the app will: against the committed public key, not the private key that just made it.
want="$(tr -d ' \n' <"$COMMITTED")"
if [ "$public_hex" != "$want" ]; then
	echo "sign-manifest: NOTE — signed with a key that is not the release key (tools/release/manifest-ed25519.pub); the app will refuse this release. Do not publish it."
	exit 0
fi
der="$(mktemp)"; trap 'rm -f "$der"' EXIT
python3 -I -c 'import binascii,sys; sys.stdout.buffer.write(binascii.unhexlify("302a300506032b6570032100"+sys.argv[1]))' "$want" >"$der"
openssl pkeyutl -verify -rawin -pubin -keyform DER -inkey "$der" -in "$MANIFEST" -sigfile "$SIG" >/dev/null || die "the signature does not verify against the committed key"
echo "sign-manifest: $SIG verifies against the committed release key"
