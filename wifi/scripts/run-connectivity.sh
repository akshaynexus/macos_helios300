#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
exec open "$PWD/build/Ath10k Connectivity.app"
