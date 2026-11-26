#!/bin/bash
# Add the built appinst tools and launchd fragments to the native boot add-on.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(git -C "$HERE" rev-parse --show-toplevel)
BUILD=${1:-$REPO/out/userland/appinst}
BASE=${2:-$REPO/out/image/24A446/kernel.kc.gpuaddon}
OUT=${3:-$REPO/out/userland/appinst/appinst.gpuaddon}
ADD=${ADDON_ADD_DIR:-$REPO/out/kernel-native-take23/unixnu/oneios/ios-27.0.1-24A446/oneios_native/tree/scripts/addon-add-dir.py}
for f in "$BUILD/appinst" "$BUILD/libunixnu-ldid.dylib" "$HERE/services.d/appinst-finalize.conf" "$HERE/services.d/appinst-serve.conf" "$BASE" "$ADD"; do
  [ -f "$f" ] || { echo "build-addon: no $f" >&2; exit 1; }
done
stage=$(mktemp -d "${TMPDIR:-/tmp}/oneios-appinst-addon.XXXXXX")
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/bin" "$stage/services.d"
cp "$BUILD/appinst" "$stage/bin/appinst"
cp "$BUILD/libunixnu-ldid.dylib" "$stage/bin/libunixnu-ldid.dylib"
cp "$HERE/services.d/"*.conf "$stage/services.d/"
chmod 755 "$stage/bin/appinst"
chmod 644 "$stage/bin/libunixnu-ldid.dylib" "$stage/services.d/"*.conf
mkdir -p "$(dirname "$OUT")"
python3 "$ADD" "$BASE" "$OUT" appinst "$stage"
sha256sum "$OUT" > "$OUT.sha256"
cat "$OUT.sha256"
