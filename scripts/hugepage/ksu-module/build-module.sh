#!/bin/bash
# Build an inert, on-demand KernelSU module for the reviewed phone; a connected phone revalidates the checked-in target profile.
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
HERE=$ROOT/scripts/hugepage/ksu-module
OUT=${OUT:-$ROOT/out/ksu-module}
HUGE=${HUGE:-/mnt/oneios-build/hugepage/out/android15-6.6.ko}
SHARE=${SHARE:-/mnt/oneios-build/hugepage/out/gunyah_host_share-android15-6.6.ko}
UDMABUF=${UDMABUF:-/mnt/oneios-build/hugepage/out/udmabuf-android15-6.6.ko}
HALTPOLL=${HALTPOLL:-/mnt/oneios-build/dl/halt-poll/gunyah-halt-poll-gki-6.6.ko}
ADB=${ADB:-adb}
mkdir -p "$OUT"
for f in "$HUGE" "$SHARE" "$UDMABUF" "$HALTPOLL"; do [ -f "$f" ] || { echo "build-module: no $f" >&2; exit 1; }; done
command -v zip >/dev/null || { echo "build-module: zip is required" >&2; exit 1; }
PROFILE=${PROFILE:-$HERE/target-pa3q.conf}
[ -r "$PROFILE" ] || { echo "build-module: no reviewed target profile $PROFILE" >&2; exit 1; }
. "$PROFILE"
model=$TARGET_MODEL
device=$TARGET_DEVICE
kernel=$TARGET_UNAME
fingerprint=$TARGET_FINGERPRINT
phone() { "$ADB" shell "$1" | tr -d '\r'; }
if "$ADB" get-state >/dev/null 2>&1; then
    live_model=$(phone 'getprop ro.product.model')
    live_device=$(phone 'getprop ro.product.device')
    live_kernel=$(phone 'uname -r')
    live_fingerprint=$(phone 'getprop ro.build.fingerprint')
    [ "$live_model/$live_device/$live_kernel/$live_fingerprint" = "$model/$device/$kernel/$fingerprint" ] || {
        echo "build-module: connected phone differs from reviewed profile: $live_model/$live_device $live_kernel" >&2; exit 1;
    }
    echo "build-module: connected phone matches $PROFILE"
else
    echo "build-module: phone offline; packaging the fail-closed reviewed profile $PROFILE"
fi
case "$model/$device" in SM-S938U/pa3q) ;; *) echo "build-module: refused unreviewed target $model/$device" >&2; exit 1 ;; esac

stage=$(mktemp -d "${TMPDIR:-/tmp}/oneios-ksu.XXXXXX")
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/system/bin" "$stage/bin"
cp "$HERE/module.prop" "$HERE/customize.sh" "$HERE/action.sh" "$HERE/README.md" "$stage/"
cp "$HERE/oneios-native" "$stage/system/bin/oneios-native"
cp "$HUGE" "$stage/bin/gh_hugepage_reserve.ko"
cp "$SHARE" "$stage/bin/gunyah_host_share.ko"
cp "$UDMABUF" "$stage/bin/udmabuf_gki.ko"
cp "$HALTPOLL" "$stage/bin/gunyah_halt_poll.ko"
chmod 0755 "$stage/customize.sh" "$stage/action.sh" "$stage/system/bin/oneios-native"
chmod 0644 "$stage/bin/"*.ko "$stage/module.prop" "$stage/README.md"

version_code=$(git -C "$ROOT" rev-list --count HEAD)
sed -i "s/^versionCode=.*/versionCode=$version_code/" "$stage/module.prop"
quote() { printf '%s' "$1" | sed "s/'/'\\\\''/g"; }
{
    printf "TARGET_MODEL='%s'\n" "$(quote "$model")"
    printf "TARGET_DEVICE='%s'\n" "$(quote "$device")"
    printf "TARGET_UNAME='%s'\n" "$(quote "$kernel")"
    printf "TARGET_FINGERPRINT='%s'\n" "$(quote "$fingerprint")"
    printf "PAGEBLOCK_ORDER='%s'\nMIGRATE_CMA='%s'\nDISABLE_KAPI='%s'\n" "$PAGEBLOCK_ORDER" "$MIGRATE_CMA" "$DISABLE_KAPI"
    printf "VMID_OFF='%s'\nRM_OFF='%s'\nVM_STATUS_OFF='%s'\n" "$VMID_OFF" "$RM_OFF" "$VM_STATUS_OFF"
    printf "LAUNCH_RAM_MIB='%s'\nLAUNCH_NEED_PAGES='%s'\n" "$LAUNCH_RAM_MIB" "$LAUNCH_NEED_PAGES"
    printf "SHA_HUGEPAGE='%s'\n" "$(sha256sum "$stage/bin/gh_hugepage_reserve.ko" | cut -d' ' -f1)"
    printf "SHA_HOSTSHARE='%s'\n" "$(sha256sum "$stage/bin/gunyah_host_share.ko" | cut -d' ' -f1)"
    printf "SHA_UDMABUF='%s'\n" "$(sha256sum "$stage/bin/udmabuf_gki.ko" | cut -d' ' -f1)"
    printf "SHA_HALTPOLL='%s'\n" "$(sha256sum "$stage/bin/gunyah_halt_poll.ko" | cut -d' ' -f1)"
} > "$stage/target.conf"
chmod 0600 "$stage/target.conf"

cat > "$stage/NOTICE.txt" <<'NOTICE'
OneiOS on-demand module wrapper: MIT.

gh_hugepage_reserve: GPL, Droid-VM/gh-hugepage-reserve commit
00c8a0322aa6fb25593e00352af2764be5317d7e.

gunyah_host_share and udmabuf: GPL-2.0-or-later / GPL-2.0,
Droid-VM/gunyah_host_mod commit 44e8ae5a088a3a35290a5cacb205640cff187b99.

gunyah_halt_poll: Droid-VM prebuilt f7c6ce8 used by this S25 native path.
The source/build pins and reproducible build commands are in scripts/hugepage/build*.sh.
NOTICE
{
    echo "built from OneiOS $(git -C "$ROOT" rev-parse HEAD)"
    echo "target $model $device"
    echo "kernel $kernel"
    echo "fingerprint $fingerprint"
    (cd "$stage" && sha256sum bin/*.ko)
} > "$stage/build-manifest.txt"

zipfile=$OUT/oneios-native-on-demand-$device-$version_code.zip
rm -f "$zipfile"
(cd "$stage" && zip -q -r "$zipfile" .)
sha256sum "$zipfile" > "$zipfile.sha256"
echo "build-module: $zipfile"
cat "$zipfile.sha256"
unzip -l "$zipfile"
