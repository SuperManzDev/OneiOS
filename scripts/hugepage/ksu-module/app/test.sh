#!/bin/bash
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
APP=$ROOT/scripts/hugepage/ksu-module/app
APK=$ROOT/out/ksu-module/app/oneios-native-control.apk
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
TOOLS=$(find "$SDK/build-tools" -maxdepth 1 -mindepth 1 -type d | sort -V | tail -n 1)
fail=0
n=1
ok() { echo "ok $1: $2"; }
bad() { echo "FAIL $1: $2"; fail=1; }

if bash -n "$APP/build-apk.sh"; then ok "$n" "build script syntax"; else bad "$n" "build script syntax"; fi
n=$((n + 1))
"$APP/build-apk.sh" >/tmp/oneios-native-app-build.log
if [ -f "$APK" ] && "$TOOLS/apksigner" verify "$APK"; then ok "$n" "APK builds and verifies"; else bad "$n" "APK signature"; fi
n=$((n + 1))
if "$TOOLS/aapt2" dump badging "$APK" | grep -q "package: name='dev.oneios.nativeprep'" && \
   "$TOOLS/aapt2" dump badging "$APK" | grep -q "launchable-activity: name='dev.oneios.nativeprep.MainActivity'"; then
    ok "$n" "package and launcher activity"
else
    bad "$n" "manifest identity"
fi
n=$((n + 1))
if unzip -Z1 "$APK" | grep -qx classes.dex && unzip -Z1 "$APK" | grep -q 'res/'; then ok "$n" "DEX and resources packaged"; else bad "$n" "APK contents"; fi
n=$((n + 1))
source=$APP/src/dev/oneios/nativeprep/MainActivity.java
if grep -q 'setPositiveButton("Prepare"' "$source" && grep -q 'ACTION_PREPARE.equals.*getIntent' "$source" && \
   grep -q 'postDelayed(this::confirmPrepare' "$source" && ! grep -q 'postDelayed.*runCommand' "$source"; then
    ok "$n" "home shortcut opens confirmation, never automatic preparation"
else
    bad "$n" "prepare confirmation gate"
fi
n=$((n + 1))
if grep -q 'isRequestPinShortcutSupported' "$source" && grep -q 'requestPinShortcut' "$source" && grep -q 'prepare_vm_memory' "$source" && \
   grep -q 'R.drawable.ic_shortcut_prepare' "$source" && [ -f "$APP/res/drawable/ic_shortcut_prepare.xml" ]; then
    ok "$n" "pinned home-screen shortcut"
else
    bad "$n" "shortcut API"
fi
n=$((n + 1))
if grep -q 'It does not stop One UI, turn off the display, or start the VM' "$source" && \
   ! grep -R -E '/efs|/dev/block|setprop[[:space:]]+ctl\.(stop|restart)|sys\.powerctl|/system/bin/reboot|[;&][[:space:]]*reboot' "$APP/src" "$APP/res" >/dev/null; then
    ok "$n" "screen safety and Knox/EFS boundary"
else
    bad "$n" "unsafe app operation"
fi
n=$((n + 1))
if grep -q 'setMinHeight(dp(52))' "$source" && grep -q 'WindowInsets.Type.systemBars' "$source" && \
   grep -q 'ACCESSIBILITY_LIVE_REGION_POLITE' "$source" && grep -q 'IMPORTANT_FOR_ACCESSIBILITY_NO' "$source" && \
   grep -q 'app_text_secondary' "$APP/res/values-night/colors.xml" && [ -f "$APP/res/values-night/styles.xml" ] && \
   ! grep -q 'screenOrientation' "$APP/AndroidManifest.xml"; then
    ok "$n" "growing 48dp+ controls, insets, accessibility semantics, rotation and dark theme"
else
    bad "$n" "native UI floor"
fi
n=$((n + 1))

if grep -q 'waitFor(150, TimeUnit.SECONDS)' "$source" && grep -q 'timeout -s TERM -k 5 140' "$source" && \
   grep -q 'Current status after timeout' "$source" && grep -q 'Root access required' "$source" && \
   grep -q 'permission denied' "$source" && grep -q 'Partially prepared — release required' "$source" && \
   grep -q 'Not prepared' "$source" && grep -q 'Ready for VM' "$source"; then
    ok "$n" "bounded root process and explicit readiness/error states"
else
    bad "$n" "root timeout or state model"
fi
n=$((n + 1))

echo "$((n - 1)) checks, $fail failed"
exit "$fail"
