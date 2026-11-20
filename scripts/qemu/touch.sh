#!/bin/bash
# A finger on the display of a running scripts/qemu/boot-vm.sh (INPUT=tablet: QEMU's virtio-tablet-device, which the
# kernel's virtio-input transport takes as one finger while its button is down), through QEMU's QMP socket.
#   scripts/qemu/touch.sh OUT tap X Y                    a tap at pixel X,Y of the display
#   scripts/qemu/touch.sh OUT swipe X1 Y1 X2 Y2 [MS]     a finger down at X1,Y1, moved to X2,Y2 in MS ms (default 400)
# OUT is boot-vm.sh's folder (out/qemu/native); the display's size comes from its layout.txt ("display: WxH").
# Needs socat. QMP's absolute axes run 0..32767 over the whole display.
set -euo pipefail
OUT=${1:?OUT}; what=${2:?tap or swipe}; shift 2
[ -S "$OUT/qmp.sock" ] || { echo "touch: no $OUT/qmp.sock (is boot-vm.sh running, with INPUT=tablet?)" >&2; exit 1; }
size=$(grep -o 'display: [0-9]*x[0-9]*' "$OUT/layout.txt" | grep -o '[0-9]*x[0-9]*' || true)
[ -n "$size" ] || { echo "touch: $OUT/layout.txt names no display" >&2; exit 1; }
W=${size%x*}; H=${size#*x}
abs() { echo $(( $1 * 32767 / ($2 - 1) )); }       # a pixel to QMP's 0..32767
event() {   # x y [down|up]: one move, and the button's change if given
  local x y e
  x=$(abs "$1" "$W"); y=$(abs "$2" "$H")
  e="{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$x}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$y}}"
  [ -z "${3:-}" ] || e="$e,{\"type\":\"btn\",\"data\":{\"down\":$([ "$3" = down ] && echo true || echo false),\"button\":\"left\"}}"
  printf '{"execute":"input-send-event","arguments":{"events":[%s]}}\n' "$e"
}
case "$what" in
  tap)
    [ $# -eq 2 ] || { echo "touch: tap X Y" >&2; exit 2; }
    { echo '{"execute":"qmp_capabilities"}'; sleep 0.1
      event "$1" "$2" down; sleep 0.12; event "$1" "$2" up; sleep 0.3; } | socat -t 1 - "UNIX-CONNECT:$OUT/qmp.sock" > /dev/null ;;
  swipe)
    [ $# -ge 4 ] || { echo "touch: swipe X1 Y1 X2 Y2 [MS]" >&2; exit 2; }
    x1=$1 y1=$2 x2=$3 y2=$4 ms=${5:-400} steps=12
    { echo '{"execute":"qmp_capabilities"}'; sleep 0.1
      event "$x1" "$y1" down
      for i in $(seq 1 $steps); do
        sleep "$(awk -v m="$ms" -v s="$steps" 'BEGIN { printf "%.3f", m / s / 1000 }')"
        event $(( x1 + (x2 - x1) * i / steps )) $(( y1 + (y2 - y1) * i / steps ))
      done
      sleep 0.05; event "$x2" "$y2" up; sleep 0.3; } | socat -t 1 - "UNIX-CONNECT:$OUT/qmp.sock" > /dev/null ;;
  *) echo "touch: tap or swipe" >&2; exit 2 ;;
esac
echo "touch: $what ${*}"
