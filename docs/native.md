# The native path: XNU alone on the phone

The owner's decision of 2026-11-19, after session 7 (`docs/SM8750_PORT.md`): hosted iOS is stable, but it runs at 55-98
fps while cool and at 10 fps when iOS's thermal state is "too hot" (case 46 C), and the host's Linux kernel takes more
than half of the CPU it uses. Goal: XNU alone on the Galaxy S25 Ultra, started from a running Android with Linux then
shut down (no bootloader unlock). Months of work; the GPU (Adreno 830, no XNU driver anywhere) is the wall.

## Facts that shape it
- The SM8750's Oryon cores have no 16 KiB translation granule (ID_AA64MMFR0_EL1 TGran16 = 0, measured on a OnePlus 13
  by palazik/ios-dodge, `external/ios-dodge/NOTES.md`): 4 KiB pages. The standalone kernel carries UniXNU's native-4k
  patches (`out/kernel/current/series.txt`).
- From ios-dodge (to verify on the S25's live FDT): GICv3 distributor 0x16000000, redistributors 0x16080000 with a
  stride of 0x40000, the timer at 19.2 MHz, PSCI by SMC, a framebuffer console (no usable UART).
- Safety (ios-dodge `RECOVERY.md`: ios-topaz lost its GPT to an iOS UFS driver that wrote LUN0): before any test on
  the phone, back up LUN0's GPT and the handset's unique partitions (Samsung's EFS and the modem's); iOS runs from
  RAM; XNU writes nothing to UFS until its driver is trusted.

## The bench (PC, QEMU)
- QEMU: Arch's `qemu-system-aarch64` 11.1.1 and its 13 dependencies, extracted into `/mnt/tkg-build/oneios/sysroot`
  (each package's SHA-256 checked against pacman's sync database; the files from the Arch archive), run with
  `LD_LIBRARY_PATH=$SYSROOT/usr/lib`. Nothing installed to `/`.
- Kernel: `scripts/bg.sh kernel-vm env LIBDISPATCH_PREFIX=/mnt/tkg-build/oneios/sysroot/usr kernel/build-linux.sh`
  (`out/kernel/current/xnu.macho`, 140 s; `build-linux.sh` now binds UniXNU's `plutil` as `build-hosted.sh` does:
  GNUstep's `/usr/bin/plutil` stopped installhdrs).
- Loader: `runtime/loader/tools/xnuloader-qemu.c` places a kernel as a firmware loader does (`xl_boot_place` with
  virt_base = the kernel's base) on virt (GICv3 0x08000000/0x080a0000, PL011 0x09000000, PSCI by HVC), with a stub 2 MiB
  into RAM that sets x0 and branches; `scripts/qemu/boot-vm.sh` runs it under a timeout in tmux and keeps the console
  and every exception (`out/qemu/vm/`).
- First run (2026-11-19, the plain `xnu.macho`): the MMU comes on and the kernel runs at its virtual addresses, then
  stops in `static_if_init` (FAR 0x2000000019e230, a pointer with PAC's error bits). An MH_EXECUTE kernel with
  `LC_DYLD_CHAINED_FIXUPS` is neither rebased nor signed by `arm_slide_rebase_and_sign_image` (only `__thread_starts`
  are), so it boots as a kernel collection, as on an iPhone.
- Its collection: `scripts/bg.sh prelink-vm env KERNEL=$PWD/out/kernel/current/xnu.macho OUT=$PWD/out/kernel/vm-24A446
  kernel/prelink/build.sh` (never over the phone's `out/kernel/24A446`): 93 MB with Apple's iPhone18,2 kexts. The emit
  step binds a leaf pair whose native name the kernel lacks to that kernel's own function with the same instructions
  (`kernel/prelink/README.md`).
- The whole loop in one job (about 8 minutes on this PC, most of it the kernel build):
  `scripts/bg.sh native-bench bash -c 'LIBDISPATCH_PREFIX=/mnt/tkg-build/oneios/sysroot/usr kernel/build-linux.sh &&
  KERNEL=$PWD/out/kernel/current/xnu.macho OUT=$PWD/out/kernel/vm-24A446 kernel/prelink/build.sh &&
  KERNEL=$PWD/out/kernel/vm-24A446/kernel.kc scripts/qemu/boot-vm.sh 60'`.
- The console: the board's `oneios-board-pl011-console.patch` compiles XNU's PL011 driver outside the hosted build
  (`PL011_UART`; the BCM2837 config had no serial driver at all), and `xnuloader-qemu` gives `/arm-io/uart0` an
  `AAPL,phandle` that `/defaults serial-device` names (`pe_serial.c` `get_serial_device_phandle`).
- Second blocker, fixed: a data abort in `_Block_copy` from `IOService::setNotification` (a kext's
  `addMatchingNotification` with a block). Apple's clang signs block descriptor pointers and stock clang does not; the
  hosted kernel stripped the signature in hosted 0007, the standalone kernel did not. The hunk is now the board's
  `oneios-kext-block-descriptors.patch` (both kernels; the hosted source is the same as before).
- **Where it stands (2026-11-19, QEMU, 60 s):** Darwin 25.5.0 with Apple's kexts runs through BSD and IOKit start:
  AMFI, Sandbox, the MAC framework, CoreAnalyticsHub, our board drivers (off: no bridge on virt), the network stack,
  APFS (`com.apple.filesystems.apfs` 3288.2.1), and stops at `Waiting on <dict ...IOMedia ... Apple_HFS>`: the root
  device. The kernel side of the bench is done; the next stage is a disk.

## Next
1. A root disk on the bench: a virtio-mmio block driver in the standalone kernel (Droid-VM's NativeVirtioBlock, not in
   `board.toml` yet) and QEMU virt's virtio-mmio windows in `xnuloader-qemu`'s tree; then the OS image to launchd.
2. The S25's live FDT, read only, for the SM8750 machine tree; how the phone gets its root (a RAM disk first: no UFS
   writes).
3. The Adreno 830's kernel side for XNU with Turnip (references: UniXNU's Adreno740, UniXNU-houji's A750 port).
