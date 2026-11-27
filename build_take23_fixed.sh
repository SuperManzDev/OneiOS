#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

LOADER=runtime/loader/build/host/xnuloader-crosvm
KERNEL=${KERNEL:-out/kernel/native-take23-24A446/kernel.kc}
IMAGE_SET=${IMAGE_SET:-/mnt/oneios-build/workspace/src/OneiOS/out/image/24A446}
GPU_ADDON=${GPU_ADDON:-$IMAGE_SET/kernel.kc.gpuaddon}
RAM_MIB=${RAM_MIB:-4096}
OUT=${OUT:-out/crosvm/kernel-take23-fixed.img}

make -C runtime/loader
"$LOADER" "$KERNEL" "$OUT" \
  --ram-mib "$RAM_MIB" \
  --cpus 6 \
  --args-add 'oneios-data-wait=1800 oneios-hid-buttons-builtin=1 oneios-button-haptics=1 oneios_native_impacts=1' \
  --image "$IMAGE_SET" \
  --gpu-addon "$GPU_ADDON" \
  --display 1440x3120 \
  --scale 3 \
  --hz 120 \
  --shared-ram \
  --gpu-scanout \
  --audio \
  --wifi \
  --bridge

grep -qx 'scanout 1440 3120 120' "$OUT.disks"
label=$(dd if="$OUT" bs=1 skip=256 count=128 status=none | tr -d '\0')
case "$label" in
  *"ram-mib=$RAM_MIB cpus=6 "*'shared-ram=1 gpu=1'*) ;;
  *) echo "unexpected Image label: $label" >&2; exit 1 ;;
esac
sha256sum "$OUT" "$OUT.adt" "$OUT.disks"
