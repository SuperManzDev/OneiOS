#!/bin/bash
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
HERE=$ROOT/scripts/hugepage/ksu-module
fail=0
ok() { echo "ok $1: $2"; }
bad() { echo "FAIL $1: $2"; fail=1; }

n=1
for f in oneios-native customize.sh action.sh build-module.sh; do
    if bash -n "$HERE/$f" && dash -n "$HERE/$f"; then ok "$n" "$f parses as POSIX shell"; else bad "$n" "$f syntax"; fi
    n=$((n + 1))
done

if find "$HERE" -maxdepth 1 -type f \( -name 'post-fs-data.sh' -o -name 'service.sh' -o -name 'boot-completed.sh' \) | grep -q .; then
    bad "$n" "an automatic boot script exists"
else
    ok "$n" "no automatic boot script"
fi
n=$((n + 1))

if grep -E 'setprop[[:space:]]+ctl\.(stop|restart)|setprop[[:space:]]+sys\.powerctl|/system/bin/reboot|[;&][[:space:]]*reboot|>[[:space:]]*/sys/class/backlight|input[[:space:]]+keyevent|setenforce' \
    "$HERE/oneios-native" "$HERE/action.sh" "$HERE/customize.sh" >/dev/null; then
    bad "$n" "a runtime script can stop Android, reboot, or touch the screen"
else
    ok "$n" "runtime scripts have no framework/display/reboot operation"
fi
n=$((n + 1))

if grep -q 'oneios-native status' "$HERE/action.sh" && ! grep -Eq 'prepare|release|insmod|rmmod' "$HERE/action.sh"; then
    ok "$n" "KernelSU action is read-only status"
else
    bad "$n" "KernelSU action is not status-only"
fi
n=$((n + 1))

if grep -q 'echo "automatic boot action: none"' "$HERE/oneios-native" && grep -q '^    prepare|release)' "$HERE/oneios-native" && \
   grep -q 'Android and the screen stay running' "$HERE/oneios-native"; then
    ok "$n" "memory reservation requires the explicit prepare command"
else
    bad "$n" "prepare is not explicitly gated"
fi
n=$((n + 1))

if grep -q 'TARGET_UNAME' "$HERE/oneios-native" && grep -q 'TARGET_FINGERPRINT' "$HERE/oneios-native" && \
   grep -q 'module hash differs' "$HERE/oneios-native" && grep -q 'active_vms' "$HERE/oneios-native" && \
   grep -q 'refusing to unload modules another tool installed' "$HERE/oneios-native"; then
    ok "$n" "target, hashes, ownership, and active-VM release are fail-closed"
else
    bad "$n" "safety gates are incomplete"
fi
n=$((n + 1))

if grep -q 'Knox or EFS' "$HERE/README.md" && ! grep -E '/efs|/dev/block|knox' "$HERE/oneios-native" "$HERE/action.sh" "$HERE/customize.sh" >/dev/null; then
    ok "$n" "Knox/EFS boundary is documented and scripts do not access them"
else
    bad "$n" "Knox/EFS boundary"
fi
n=$((n + 1))

if [ "${BUILD:-0}" = 1 ]; then
    "$HERE/build-module.sh" >/tmp/oneios-ksu-build-test.log
    zipfile=$(sed -n 's/^build-module: //p' /tmp/oneios-ksu-build-test.log | head -n 1)
    if [ -f "$zipfile" ] && unzip -Z1 "$zipfile" | grep -qx 'module.prop' && \
       unzip -Z1 "$zipfile" | grep -qx 'system/bin/oneios-native' && \
       ! unzip -Z1 "$zipfile" | grep -Eq '(^|/)(post-fs-data|service|boot-completed)\.sh$'; then
        ok "$n" "built ZIP is complete and inert"
    else
        bad "$n" "built ZIP contents"
    fi
    n=$((n + 1))
fi

echo "$((n - 1)) checks, $fail failed"
exit "$fail"
