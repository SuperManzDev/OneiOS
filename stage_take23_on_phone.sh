#!/usr/bin/env bash
# Push the reviewed 4 GiB Take 23 VM and its helpers to the phone without starting it or stopping One UI.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

exec tools/oneios-tool/target/release/oneios-tool phone vm-boot \
  --stage-only \
  --image out/crosvm/kernel-take23-fixed-appinst.img \
  --droidvm /mnt/oneios-build/dl/droidvm-prebuilt/f7c6ce8 \
  --bridge runtime/display/build/android/displayctl \
  --bridgectl runtime/bridges/build/android/bridgectl \
  --wifictl runtime/bridges/build/android/wifictl \
  --takeover \
  --pin \
  --pin-first 2 \
  --thp \
  --thermal-log \
  --native-haptics \
  --brightness \
  --brightness-max 2953 \
  --bluetooth \
  --seconds 900 \
  --out phone-vmboot-take23-staged
