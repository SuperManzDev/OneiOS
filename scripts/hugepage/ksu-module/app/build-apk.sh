#!/bin/bash
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
APP=$ROOT/scripts/hugepage/ksu-module/app
OUT=${OUT:-$ROOT/out/ksu-module/app}
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
BUILD_TOOLS=${BUILD_TOOLS:-$(find "$SDK/build-tools" -maxdepth 1 -mindepth 1 -type d | sort -V | tail -n 1)}
PLATFORM=${PLATFORM:-$(find "$SDK/platforms" -maxdepth 1 -mindepth 1 -type d | sort -V | tail -n 1)}
AAPT2=$BUILD_TOOLS/aapt2
D8=$BUILD_TOOLS/d8
ZIPALIGN=$BUILD_TOOLS/zipalign
APKSIGNER=$BUILD_TOOLS/apksigner
ANDROID_JAR=$PLATFORM/android.jar
for f in "$AAPT2" "$D8" "$ZIPALIGN" "$APKSIGNER" "$ANDROID_JAR"; do [ -e "$f" ] || { echo "build-apk: no $f" >&2; exit 1; }; done
command -v javac >/dev/null
command -v keytool >/dev/null
command -v jar >/dev/null
command -v zip >/dev/null
mkdir -p "$OUT"
work=$(mktemp -d "${TMPDIR:-/tmp}/oneios-native-app.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/gen" "$work/classes" "$work/dex"
"$AAPT2" compile --dir "$APP/res" -o "$work/resources.zip"
"$AAPT2" link -o "$work/unsigned.apk" -I "$ANDROID_JAR" --manifest "$APP/AndroidManifest.xml" \
    --java "$work/gen" --min-sdk-version 30 --target-sdk-version 37 --version-code 1 --version-name 0.1.0 \
    "$work/resources.zip"
mapfile -t sources < <(find "$APP/src" "$work/gen" -name '*.java' -type f | sort)
javac -encoding UTF-8 -Xlint:-options --release 8 -classpath "$ANDROID_JAR" -d "$work/classes" "${sources[@]}"
(cd "$work/classes" && jar cf "$work/classes.jar" .)
"$D8" --lib "$ANDROID_JAR" --min-api 30 --output "$work/dex" "$work/classes.jar"
zip -q -j "$work/unsigned.apk" "$work/dex/classes.dex"
"$ZIPALIGN" -f 4 "$work/unsigned.apk" "$work/aligned.apk"
key=$OUT/debug.keystore
if [ ! -f "$key" ]; then
    keytool -genkeypair -noprompt -keystore "$key" -storepass android -keypass android -alias androiddebugkey \
        -dname 'CN=OneiOS Native Control,O=OneiOS,C=US' -keyalg RSA -keysize 2048 -validity 10000 >/dev/null 2>&1
fi
apk=$OUT/oneios-native-control.apk
"$APKSIGNER" sign --ks "$key" --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey --out "$apk" "$work/aligned.apk"
"$APKSIGNER" verify --verbose "$apk"
sha256sum "$apk" > "$apk.sha256"
cat "$apk.sha256"
"$AAPT2" dump badging "$apk" | head -n 8
