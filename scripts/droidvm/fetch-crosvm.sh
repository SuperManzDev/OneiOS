#!/bin/sh
# scripts/droidvm/fetch-crosvm.sh [DIR]: Droid-VM's own build of crosvm for Android (BSD-3-Clause, with Droid-VM's
# GPL-2.0-or-later additions; source https://github.com/Droid-VM/crosvm, wip/hackintosh) and the three libraries of
# its package that the phone does not have, from their published prebuilts (https://github.com/Droid-VM/
# DroidVM-Prebuilt-Root, manual-build/arm64-v8a), at a pinned commit, each file checked against its SHA-256.
# This crosvm has the pseudo-unprotected VM (--protected-vm-pseudo-unprotected): a protected VM whose RAM is SHAREd
# with it after start, so Android can still read it (docs/native.md "The shared-RAM VM"); it needs
# gunyah_host_share (scripts/hugepage/build-share.sh). crosvm finds the libraries next to itself (RUNPATH $ORIGIN);
# the rest it links (libbase, libcutils, libminijail, libcap, ...) are the phone's own, in /system/lib64.
# Built by Droid-VM, not here: building it needs their AOSP tree (crosvm-minimal-manifest, tens of GB). The owner
# chose the prebuilt (2026-11-23). Nothing of it goes into this repository.
#
# Output: DIR/82c59c2/{crosvm,libvirglrenderer.so,libgfxstream_backend.so,libvncserver.so}
set -eu
DIR=${1:-/mnt/tkg-build/oneios/dl/droidvm-prebuilt}
COMMIT=82c59c2834cb7e2f0284b03849fc921057804b75
BASE=https://raw.githubusercontent.com/Droid-VM/DroidVM-Prebuilt-Root/$COMMIT/manual-build/arm64-v8a/usr
OUT=$DIR/$(printf %.7s "$COMMIT")
mkdir -p "$OUT"
# FILE PATH-UNDER-usr SHA-256
while read -r f at sum; do
  [ -n "$f" ] || continue
  if [ -f "$OUT/$f" ] && [ "$(sha256sum < "$OUT/$f" | cut -d' ' -f1)" = "$sum" ]; then
    continue
  fi
  echo "fetch-crosvm: $f"
  curl -fsSL --retry 3 -A git-client -o "$OUT/$f.part" "$BASE/$at"
  got=$(sha256sum < "$OUT/$f.part" | cut -d' ' -f1)
  [ "$got" = "$sum" ] || { echo "fetch-crosvm: $f: sha256 $got, not $sum" >&2; rm -f "$OUT/$f.part"; exit 1; }
  mv "$OUT/$f.part" "$OUT/$f"
done <<EOF
crosvm bin/crosvm cd3693ba876720f7ab7c08e5d6358101336f3bbcdb3d7d4f324ac29274bf6a7b
libvirglrenderer.so lib/libvirglrenderer.so 40b5edaa7c722b801b2b08d2f2d64fd55e56bebf62baec2dbb17e662a49f633d
libgfxstream_backend.so lib/libgfxstream_backend.so ed5395e443fb2b3f71f647e80e6f434e93b342b463ce841c9319903affedbf52
libvncserver.so lib/libvncserver.so eaf7d95c7f4fe0ca8a935192442f332e9313ba51563c5990a063759348eeeb00
EOF
echo "fetch-crosvm: $OUT"
ls -l "$OUT"
