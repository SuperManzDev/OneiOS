#!/usr/bin/env bash
# Push a reviewed Take 23/23.1 VM and its helpers to the phone without starting it or stopping One UI.
# Take 23.1 callers set IMAGE, STAGE_OUT and PIN_FIRST=0; the historical defaults remain for reproducibility.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

IMAGE=${IMAGE:-out/crosvm/kernel-take23-fixed-appinst.img}
STAGE_OUT=${STAGE_OUT:-phone-vmboot-take23-staged}
PIN_FIRST=${PIN_FIRST:-2}
GUARD=${GUARD:-runtime/guard/build/android/oneios-guard}

exec tools/oneios-tool/target/release/oneios-tool phone vm-boot \
  --stage-only \
  --image "$IMAGE" \
  --droidvm /mnt/oneios-build/dl/droidvm-prebuilt/f7c6ce8 \
  --bridge runtime/display/build/android/displayctl \
  --bridgectl runtime/bridges/build/android/bridgectl \
  --wifictl runtime/bridges/build/android/wifictl \
  --guard "$GUARD" \
  --takeover \
  --pin \
  --pin-first "$PIN_FIRST" \
  --thp \
  --thermal-log \
  --native-haptics \
  --brightness \
  --brightness-max 2953 \
  --bluetooth \
  --seconds 900 \
  --out "$STAGE_OUT"
