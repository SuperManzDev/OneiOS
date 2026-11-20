#!/bin/bash
# A picture of the display of a running scripts/qemu/boot-vm.sh (its SCREEN=): QEMU's monitor writes the console as PNG.
#   scripts/qemu/shot.sh [OUT [FILE]]    OUT: boot-vm.sh's folder (default out/qemu/vm); FILE: default OUT/shot-<time>.png
# Needs socat. Prints the file's path; nothing when QEMU has no display or is not running.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${1:-$ROOT/out/qemu/vm}
FILE=${2:-$OUT/shot-$(date +%H%M%S).png}
[ -S "$OUT/monitor.sock" ] || { echo "shot: no $OUT/monitor.sock (is boot-vm.sh running?)" >&2; exit 1; }
case "$FILE" in /*) ;; *) FILE=$PWD/$FILE ;; esac
printf 'screendump %s -f png\n' "$FILE" | socat -t 2 - "UNIX-CONNECT:$OUT/monitor.sock" > /dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$FILE" ] && break; sleep 0.2; done
[ -s "$FILE" ] || { echo "shot: QEMU wrote no picture" >&2; exit 1; }
echo "$FILE"
