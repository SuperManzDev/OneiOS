#!/bin/bash
# scripts/hugepage/build-share.sh [WORK]: Droid-VM's gunyah_host_share (GPL-2.0-or-later,
# https://github.com/Droid-VM/gunyah_host_mod, gunyah_host_share/GKI6.6/gunyah_share_mod.c) for the phone's KMI,
# android15-6.6. It adds /dev/gunyah_share, through which Droid-VM's crosvm SHAREs a running VM's RAM with it as a
# Gunyah memparcel: the pseudo-unprotected VM, whose RAM Android can read (docs/native.md "The shared-RAM VM").
# Built as its authors build it (their docker_exec.sh), in the DDK image scripts/hugepage/build.sh already fetched and
# checked by digest (run that first), under bubblewrap: no network, the source read-only, nothing written outside WORK.
# The module's source stays outside this repository (GPL: docs/references.md).
#
# Output: WORK/out/gunyah_host_share-android15-6.6.ko, WORK/out/gunyah_host_share.txt (the pins and the .ko's modinfo).
set -euo pipefail
WORK=${1:-/mnt/tkg-build/oneios/hugepage}
TAG=android15-6.6
REPO=https://github.com/Droid-VM/gunyah_host_mod.git
COMMIT=44e8ae5a088a3a35290a5cacb205640cff187b99
ROOTFS=$WORK/ddk-$TAG/rootfs
SRC=$WORK/gunyah_host_mod
OUT=$WORK/out
KO=$OUT/gunyah_host_share-$TAG.ko
sha() { sha256sum < "$1" | cut -d' ' -f1; }

[ -e "$ROOTFS/.oneios-done" ] || { echo "build-share: no DDK image in $ROOTFS (scripts/hugepage/build.sh first)" >&2; exit 1; }
ls -d "$ROOTFS"/opt/ddk/clang/clang-* "$ROOTFS"/opt/ddk/kdir/* >/dev/null || { echo "build-share: no /opt/ddk in the image" >&2; exit 1; }

# 1. The module's source at the pinned commit (the commit docs/references.md reads).
if [ ! -d "$SRC/.git" ]; then
  git clone -q "$REPO" "$SRC"
fi
if [ "$(git -C "$SRC" rev-parse HEAD)" != "$COMMIT" ]; then
  git -C "$SRC" fetch -q --depth 1 origin "$COMMIT" 2>/dev/null || git -C "$SRC" fetch -q origin
  git -C "$SRC" checkout -q "$COMMIT"
fi
[ -z "$(git -C "$SRC" status --porcelain)" ] || { echo "build-share: $SRC has local changes" >&2; exit 1; }

# 2. The .ko, in the image. docker_exec.sh writes /out_<module>/<kmi>.ko; its mount point has to exist in the
# read-only root.
mkdir -p "$ROOTFS/out_gunyah_host_share" "$OUT/share"
rm -f "$OUT/share/$TAG.ko" "$KO"
bwrap --unshare-all --die-with-parent --clearenv \
  --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin --setenv HOME /tmp \
  --ro-bind "$ROOTFS" / --proc /proc --dev /dev --tmpfs /tmp \
  --ro-bind "$SRC/gunyah_host_share/GKI6.6" /src --bind "$OUT/share" /out_gunyah_host_share \
  bash /src/docker_exec.sh gunyah_share_mod.c:gunyah_host_share
[ -f "$OUT/share/$TAG.ko" ] || { echo "build-share: no $TAG.ko" >&2; exit 1; }
mv "$OUT/share/$TAG.ko" "$KO"
rmdir "$OUT/share" 2>/dev/null || true

{
  echo "image ylarod/ddk-min:$TAG (scripts/hugepage/build.sh)"
  echo "source $REPO $COMMIT gunyah_host_share/GKI6.6/gunyah_share_mod.c"
  echo "ko sha256 $(sha "$KO")"
  modinfo "$KO"
} > "$OUT/gunyah_host_share.txt"
cat "$OUT/gunyah_host_share.txt"
