#!/bin/bash
# runtime/vmprobe/build.sh [OUT]: the vmprobe payloads (probe.S) with the PC's clang and ld.lld, into OUT (default
# out/vmprobe): vmprobe.bin for crosvm on the phone (8250 at 0x3f8) and vmprobe-qemu.bin for the PC's check under
# QEMU virt (PL011 at 0x09000000). `oneios-tool phone vm-probe` runs the first; docs/native.md "A VM under Gunyah".
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${1:-$repo/out/vmprobe}
mkdir -p "$OUT"
F=(--target=aarch64-none-elf -march=armv8-a -nostdlib -ffreestanding)
for v in "vmprobe 0x3f8 2" "vmprobe-qemu 0x09000000 1"; do
  set -- $v
  clang "${F[@]}" -DUART_BASE="$2" -DRAM_GIB_BLOCK="$3" -c "$here/probe.S" -o "$OUT/$1.o"
  ld.lld -T "$here/probe.ld" -nostdlib -static "$OUT/$1.o" -o "$OUT/$1.elf"
  llvm-objcopy -O binary "$OUT/$1.elf" "$OUT/$1.bin"
  rm -f "$OUT/$1.o"
  printf '%s  %s bytes\n' "$OUT/$1.bin" "$(stat -c %s "$OUT/$1.bin")"
done
