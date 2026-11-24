#!/bin/bash
# scripts/hugepage/build-udmabuf.sh [WORK]: Droid-VM's udmabuf module (GPL-2.0-or-later, https://github.com/Droid-VM/gunyah_host_mod,
# udmabuf/udmabuf.c) for the phone's KMI, android15-6.6. GKI 6.6's own udmabuf keeps a buffer's page array in one kmalloc_array,
# which a fragmented phone cannot give for a large buffer (64 MiB: an order-5 allocation): crosvm's virtio-gpu then fails the
# guest's large GPU blobs ("GUEST-ALLOC: create_udmabuf failed ... Out of memory", vm-boot gpu6, docs/native.md "The GPU in the
# shared-RAM VM"). The module redirects udmabuf_create to a copy that uses kvmalloc (its "override" mode, chosen by itself at
# insmod; "paramonly" when it cannot hook and only raises the limits). Built in the DDK image scripts/hugepage/build.sh fetched
# (run that first), under bubblewrap: no network, the source read-only, nothing written outside WORK. The module's source stays
# outside this repository (GPL: docs/references.md).
#
# Output: WORK/out/udmabuf-android15-6.6.ko, WORK/out/udmabuf.txt (the pins and the .ko's modinfo).
set -euo pipefail
WORK=${1:-/mnt/tkg-build/oneios/hugepage}
TAG=android15-6.6
COMMIT=44e8ae5a088a3a35290a5cacb205640cff187b99
ROOTFS=$WORK/ddk-$TAG/rootfs
SRC=$WORK/gunyah_host_mod
OUT=$WORK/out
KO=$OUT/udmabuf-$TAG.ko
sha() { sha256sum < "$1" | cut -d' ' -f1; }

[ -e "$ROOTFS/.oneios-done" ] || { echo "build-udmabuf: no DDK image in $ROOTFS (scripts/hugepage/build.sh first)" >&2; exit 1; }
[ -d "$SRC/.git" ] && [ "$(git -C "$SRC" rev-parse HEAD)" = "$COMMIT" ] || { echo "build-udmabuf: $SRC is not at $COMMIT (scripts/hugepage/build-share.sh fetches it)" >&2; exit 1; }
[ -z "$(git -C "$SRC" status --porcelain)" ] || { echo "build-udmabuf: $SRC has local changes" >&2; exit 1; }

# The module as gunyah_host_mod's build.sh builds it (build_mod: the source and its headers into a folder of its own, one
# obj-m line, the kernel tree's make with clang), its name udmabuf_gki_6.6 as theirs (beside the built-in udmabuf).
mkdir -p "$ROOTFS/out_udmabuf" "$OUT/ub"
rm -f "$OUT/ub/udmabuf-gki-6.6.ko" "$KO"
bwrap --unshare-all --die-with-parent --clearenv \
  --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin --setenv HOME /tmp \
  --ro-bind "$ROOTFS" / --proc /proc --dev /dev --tmpfs /tmp \
  --ro-bind "$SRC/udmabuf" /src --bind "$OUT/ub" /out_udmabuf \
  bash -c 'set -e; K=/opt/ddk/kdir/$(ls /opt/ddk/kdir); export PATH=$(ls -d /opt/ddk/clang/clang-*)/bin:$PATH; B=/tmp/build; mkdir -p $B;
           cp /src/udmabuf.c $B/udmabuf-gki-6.6.c; cp /src/*.h $B/ 2>/dev/null || true; printf "obj-m += udmabuf-gki-6.6.o\n" > $B/Makefile;
           make -C $K -j"$(nproc)" M=$B ARCH=arm64 LLVM=1 LLVM_IAS=1 modules; llvm-strip -d $B/udmabuf-gki-6.6.ko; cp $B/udmabuf-gki-6.6.ko /out_udmabuf/'
[ -f "$OUT/ub/udmabuf-gki-6.6.ko" ] || { echo "build-udmabuf: no .ko" >&2; exit 1; }
mv "$OUT/ub/udmabuf-gki-6.6.ko" "$KO"
rmdir "$OUT/ub" 2>/dev/null || true
{
  echo "image ylarod/ddk-min:$TAG (scripts/hugepage/build.sh)"
  echo "source https://github.com/Droid-VM/gunyah_host_mod.git $COMMIT udmabuf/udmabuf.c"
  echo "ko sha256 $(sha "$KO")"
  modinfo "$KO"
} > "$OUT/udmabuf.txt"
cat "$OUT/udmabuf.txt"
