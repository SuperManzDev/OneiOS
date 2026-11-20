#!/bin/bash
# Boot a native kernel on qemu-system-aarch64's virt board, placed as a firmware loader places it on a phone
# (runtime/loader/tools/xnuloader-qemu.c): the bench of the native path (docs/native.md) before anything runs on the
# phone. Always in tmux, under a timeout:
#   scripts/bg.sh qemu-vm scripts/qemu/boot-vm.sh [SECONDS]                 (default 30)
#   KERNEL=FILE CPUS=N RAM_MIB=N BOOT_ARGS="..." ARGS_ADD="..." OUT=DIR ... another kernel, CPUs, the kernel's window,
#                                                                            all the boot arguments, a few more of them
#   IMAGE=DIR [DATA=FILE] ...   with an image set's disks (os, systemos, app, the Data seed, the Data store): the native
#                               kernel of kernel/build-native.sh, as a collection (kernel/prelink/build.sh). DATA is the
#                               writable store, kept between runs (first start: formatted and filled from the seed by
#                               the kernel); made sparse at the manifest's size when missing. Default
#                               ${TMPDIR:-/tmp}/oneios-qemu-data.img, in RAM on this PC: the image set itself stays on
#                               its disk, read-only, and every chunk the kernel reads is checked against its hash list.
#   QEMU_DEBUG=LIST ...         QEMU's -d log (default int,guest_errors,unimp: every exception, for a kernel that stops
#                               early; with IMAGE guest_errors,unimp, because a long boot's interrupts would fill the
#                               log's 64 MB and stop QEMU)
# Out: OUT (out/qemu/vm) serial.log (the kernel's console on the PL011), qemu.log (QEMU's -d log, the first 64 MB), and
# xnuloader-qemu's layout files. A guest restart ends the run (-no-reboot). QEMU comes from the project's sysroot
# (/mnt/tkg-build/oneios/sysroot, QEMU_SYSROOT=; Arch's qemu-system-aarch64 extracted there, never installed to /).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SECS=${1:-30}
SYSROOT=${QEMU_SYSROOT:-/mnt/tkg-build/oneios/sysroot}
QEMU=${QEMU:-$SYSROOT/usr/bin/qemu-system-aarch64}
KERNEL=${KERNEL:-$ROOT/out/kernel/current/xnu.macho}
OUT=${OUT:-$ROOT/out/qemu/vm}
IMAGE=${IMAGE:-}
[ -x "$QEMU" ] || { echo "boot-vm: no $QEMU (docs/native.md: QEMU in the sysroot)" >&2; exit 1; }
[ -f "$KERNEL" ] || { echo "boot-vm: no $KERNEL (scripts/bg.sh kernel-vm kernel/build-linux.sh)" >&2; exit 1; }
make -s -C "$ROOT/runtime/loader" build/host/xnuloader-qemu
rm -rf "$OUT"
mkdir -p "$OUT"
opts=()
[ -z "${CPUS:-}" ] || opts+=(--cpus "$CPUS")
[ -z "${RAM_MIB:-}" ] || opts+=(--ram-mib "$RAM_MIB")
[ -z "${BOOT_ARGS:-}" ] || opts+=(--args "$BOOT_ARGS")
[ -z "${ARGS_ADD:-}" ] || opts+=(--args-add "$ARGS_ADD")
debug=${QEMU_DEBUG:-int,guest_errors,unimp}
if [ -n "$IMAGE" ]; then
  [ -f "$IMAGE/manifest.json" ] || { echo "boot-vm: $IMAGE has no manifest.json" >&2; exit 1; }
  DATA=${DATA:-${TMPDIR:-/tmp}/oneios-qemu-data.img}
  if [ ! -f "$DATA" ]; then
    gib=$(grep -o '"store_gib": *[0-9]*' "$IMAGE/manifest.json" | grep -o '[0-9]*$' || true)
    truncate -s "${gib:-16}G" "$DATA"
    echo "boot-vm: a new Data store $DATA (${gib:-16} GiB, sparse): the kernel formats it and unpacks the seed"
  fi
  opts+=(--image "$IMAGE" --data "$DATA")
  debug=${QEMU_DEBUG:-guest_errors,unimp}
fi
"$ROOT/runtime/loader/build/host/xnuloader-qemu" "$KERNEL" "$OUT" "${opts[@]}" | tee "$OUT/layout.txt"
mapfile -t args < "$OUT/qemu.args"
echo "boot-vm: $SECS s of QEMU $(LD_LIBRARY_PATH=$SYSROOT/usr/lib "$QEMU" --version | head -1)"
set +e
# QEMU's own log (-d) goes to stderr, here into the pipe; when head has its 64 MB QEMU gets SIGPIPE and stops.
LD_LIBRARY_PATH=$SYSROOT/usr/lib timeout "$SECS" "$QEMU" "${args[@]}" -display none -monitor none -no-reboot \
  -serial "file:$OUT/serial.log" -d "$debug" 2>&1 >/dev/null | head -c 64000000 > "$OUT/qemu.log"
code=${PIPESTATUS[0]}
set -e
echo "boot-vm: QEMU exit $code (124 is the timeout); console $(wc -c < "$OUT/serial.log") bytes, QEMU log $(wc -l < "$OUT/qemu.log") lines"
echo "--- the console's last lines ---"
tail -n 25 "$OUT/serial.log" || true
echo "--- the first exceptions ---"
grep -m 12 -E 'Taking exception|ESR|FAR|ELR' "$OUT/qemu.log" || true
