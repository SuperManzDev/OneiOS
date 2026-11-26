#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

TOOL=tools/oneios-tool/target/release/oneios-tool
IMAGE=out/crosvm/kernel-take23-fixed-2880.img
DROIDVM=/mnt/oneios-build/dl/droidvm-prebuilt/f7c6ce8

# Stop only a prior OneiOS vm-boot session. qcrosvm is Android's unrelated trusted VM.
"$TOOL" phone vm-boot --stop || true

exec "$TOOL" phone vm-boot \
  --image "$IMAGE" \
  --droidvm "$DROIDVM" \
  --bridge runtime/display/build/android/displayctl \
  --bridgectl runtime/bridges/build/android/bridgectl \
  --wifictl runtime/bridges/build/android/wifictl \
  --takeover \
  --pin \
  --pin-first 2 \
  --thp \
  --thermal-log \
  --audio-uid 2000 \
  --native-haptics \
  --brightness \
  --brightness-max 2953 \
  --bluetooth \
  --seconds 900 \
  --out phone-vmboot-take23-fixed-2880
