#!/bin/bash
# Build the pinned Procursus ldid in-process signer for the iOS 27 guest on Linux or macOS.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(git -C "$HERE" rev-parse --show-toplevel)
OUT=${1:-$REPO/out/userland/appinst}
SIGNER=$REPO/external/UniXNU/scripts/appsync/signer.py
[ -f "$SIGNER" ] || { echo "build-signer: no $SIGNER (kernel/fetch.sh)" >&2; exit 1; }
mkdir -p "$OUT"

if [ "$(uname -s)" = Darwin ]; then
  python3 "$SIGNER" --output "$OUT/libunixnu-ldid.dylib" > "$OUT/ldid-build.json"
else
  shim=$(mktemp -d "${TMPDIR:-/tmp}/oneios-xcrun.XXXXXX")
  trap 'rm -rf "$shim"' EXIT
  cat > "$shim/xcrun" <<'SHIM'
#!/bin/sh
set -e
ROOT=__ROOT__
IOS_DIR=$ROOT/scripts/ios
export IOS_DIR
. "$ROOT/scripts/ios/env.sh"
case "${1:-}" in
  --sdk)
    sdk=$2; shift 2
    case "${1:-}" in
      --show-sdk-path) echo "$IOS_SDK" ;;
      --find) case "$2" in clang) command -v clang;; ld) echo "$IOS_LD";; *) command -v "$2";; esac ;;
      *) exit 2 ;;
    esac ;;
  clang) shift; exec "$ROOT/scripts/ios/ios-clang" "$@" ;;
  clang++) shift; exec "$ROOT/scripts/ios/ios-clang++" "$@" ;;
  ar) shift; exec llvm-ar "$@" ;;
  ranlib) shift; exec llvm-ranlib "$@" ;;
  nm) shift; exec llvm-nm "$@" ;;
  *) echo "xcrun shim: unsupported $*" >&2; exit 2 ;;
esac
SHIM
  sed -i "s#__ROOT__#$REPO#g" "$shim/xcrun"
  chmod 755 "$shim/xcrun"
  PATH="$shim:$PATH" python3 "$SIGNER" --output "$OUT/libunixnu-ldid.dylib" > "$OUT/ldid-build.json"
fi

python3 "$REPO/scripts/ios/ios-sign.py" --identifier org.unixnu.ldid "$OUT/libunixnu-ldid.dylib" >/dev/null
python3 "$REPO/scripts/ios/macho-check.py" --tbd "$REPO/out/ios27-tbd" "$OUT/libunixnu-ldid.dylib"
python3 -I "$REPO/userland/tweak" cdhash "$OUT/libunixnu-ldid.dylib" > "$OUT/ldid-cdhash.txt"
cat "$OUT/ldid-cdhash.txt"
