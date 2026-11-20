#!/bin/bash
# Build the native OneiOS XNU: XNU alone at EL1 (docs/native.md), first for QEMU's virt board (scripts/qemu/boot-vm.sh).
#   scripts/bg.sh kernel-native env LIBDISPATCH_PREFIX=/mnt/tkg-build/oneios/sysroot/usr kernel/build-native.sh
#   NATIVE_OUT=NAME ...  another work tree and output (out/kernel-native-NAME, out/kernel/current-native-NAME)
# It is kernel/build-hosted.sh with ONEIOS_MACHINE=native: the board's series, the whole hosted series (its machine layer
# stays off: every hosted change is under #if XNU_HOSTED) and 0076, which opens the hosted kernel's OS layer to this
# kernel (CONFIG_XNU_ONEIOS_NATIVE=y of kernel/device/configs/oneios_native_defconfig): the image set's disks, root,
# Data, seed and launch policy. The archives: storage (OneiOSHostedDisk on a mapped virtio-mmio window, every chunk of a
# read-only image checked against its hash list), board, aes, keystore.
# Output: out/kernel/current-native/xnu.macho (sealed). Its collection: kernel/prelink/build.sh with
# KERNEL=out/kernel/current-native/xnu.macho OUT=out/kernel/native-24A446 (never the phone's out/kernel/24A446).
exec env ONEIOS_MACHINE=native "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build-hosted.sh" "$@"
