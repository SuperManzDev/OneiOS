#!/usr/bin/env bash
# Take 23.1 host build: independent guard + conservative 3 GiB native baseline.
# This script never contacts the phone and never launches a VM.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

RAM_MIB=${RAM_MIB:-3072}
OUT=${OUT:-out/crosvm/kernel-take23.1-guarded-${RAM_MIB}.img}
KERNEL=${KERNEL:-out/kernel/native-take23-24A446/kernel.kc}
IMAGE_SET=${IMAGE_SET:-/mnt/oneios-build/workspace/src/OneiOS/out/image/24A446}
GPU_ADDON=${GPU_ADDON:-$IMAGE_SET/kernel.kc.gpuaddon}

[ "$RAM_MIB" = 3072 ] || {
  echo "Take 23.1 baseline is 3072 MiB; set TAKE23_1_EXPERIMENTAL_RAM=1 to build another size" >&2
  [ "${TAKE23_1_EXPERIMENTAL_RAM:-0}" = 1 ] || exit 2
}

make -C runtime/guard test
make -C runtime/guard all
cargo test --manifest-path tools/oneios-tool/Cargo.toml -p oneios-phone

RAM_MIB="$RAM_MIB" KERNEL="$KERNEL" IMAGE_SET="$IMAGE_SET" GPU_ADDON="$GPU_ADDON" OUT="$OUT" \
  ./build_take23_fixed.sh

need=$(( (RAM_MIB - 32 + 1) / 2 ))
[ "$need" = 1520 ] || [ "${TAKE23_1_EXPERIMENTAL_RAM:-0}" = 1 ]
printf 'Take 23.1 host build complete\n  Image: %s\n  RAM: %s MiB\n  huge-page requirement: %s pages\n  phone action: none\n' \
  "$OUT" "$RAM_MIB" "$need"
