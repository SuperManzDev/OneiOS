#!/bin/bash
# Boot the standalone kernel (kernel/build-linux.sh: out/kernel/current/xnu.macho) on qemu-system-aarch64's virt board,
# placed as a firmware loader places it on a phone (runtime/loader/tools/xnuloader-qemu.c): the bench of the native
# path (docs/native.md) before anything runs on the phone. Always in tmux, under a timeout:
#   scripts/bg.sh qemu-vm scripts/qemu/boot-vm.sh [SECONDS]                 (default 30)
#   KERNEL=FILE CPUS=N BOOT_ARGS="..." OUT=DIR ...                           another kernel, CPU count, boot arguments
# Out: out/qemu/vm/serial.log (the kernel's console on the PL011), qemu.log (every exception QEMU saw, with ESR, FAR and
# ELR: the first 64 MB), and xnuloader-qemu's layout files. QEMU comes from the project's sysroot
# (/mnt/tkg-build/oneios/sysroot, QEMU_SYSROOT=; Arch's qemu-system-aarch64 extracted there, never installed to /).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SECS=${1:-30}
SYSROOT=${QEMU_SYSROOT:-/mnt/tkg-build/oneios/sysroot}
QEMU=${QEMU:-$SYSROOT/usr/bin/qemu-system-aarch64}
KERNEL=${KERNEL:-$ROOT/out/kernel/current/xnu.macho}
OUT=${OUT:-$ROOT/out/qemu/vm}
[ -x "$QEMU" ] || { echo "boot-vm: no $QEMU (docs/native.md: QEMU in the sysroot)" >&2; exit 1; }
[ -f "$KERNEL" ] || { echo "boot-vm: no $KERNEL (scripts/bg.sh kernel-vm kernel/build-linux.sh)" >&2; exit 1; }
make -s -C "$ROOT/runtime/loader" build/host/xnuloader-qemu
rm -rf "$OUT"
mkdir -p "$OUT"
opts=()
[ -z "${CPUS:-}" ] || opts+=(--cpus "$CPUS")
[ -z "${BOOT_ARGS:-}" ] || opts+=(--args "$BOOT_ARGS")
"$ROOT/runtime/loader/build/host/xnuloader-qemu" "$KERNEL" "$OUT" "${opts[@]}" | tee "$OUT/layout.txt"
mapfile -t args < "$OUT/qemu.args"
echo "boot-vm: $SECS s of QEMU $(LD_LIBRARY_PATH=$SYSROOT/usr/lib "$QEMU" --version | head -1)"
set +e
# QEMU's own log (-d) goes to stderr, here into the pipe; when head has its 64 MB QEMU gets SIGPIPE and stops.
LD_LIBRARY_PATH=$SYSROOT/usr/lib timeout "$SECS" "$QEMU" "${args[@]}" -display none -monitor none \
  -serial "file:$OUT/serial.log" -d int,guest_errors,unimp 2>&1 >/dev/null | head -c 64000000 > "$OUT/qemu.log"
code=${PIPESTATUS[0]}
set -e
echo "boot-vm: QEMU exit $code (124 is the timeout); console $(wc -c < "$OUT/serial.log") bytes, QEMU log $(wc -l < "$OUT/qemu.log") lines"
echo "--- the console's last lines ---"
tail -n 25 "$OUT/serial.log" || true
echo "--- the first exceptions ---"
grep -m 12 -E 'Taking exception|ESR|FAR|ELR' "$OUT/qemu.log" || true
