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
- 2026-11-19, QEMU, 60 s: Darwin 25.5.0 with Apple's kexts runs through BSD and IOKit start: AMFI, Sandbox, the MAC
  framework, CoreAnalyticsHub, our board drivers (off: no bridge on virt), the network stack, APFS
  (`com.apple.filesystems.apfs` 3288.2.1), and stops at `Waiting on <dict ...IOMedia ... Apple_HFS>`: the root device.

## The native kernel and the image set's disks (2026-11-20)
The plain kernel of `build-linux.sh` has no disks and none of what the hosted kernel does between the root mount and
launchd. The **native kernel** has both, from the hosted kernel's own code:
- `kernel/build-native.sh` (= `build-hosted.sh` with `ONEIOS_MACHINE=native`): the board's series and the whole hosted
  series, with `CONFIG_XNU_ONEIOS_NATIVE=y` and without `CONFIG_XNU_HOSTED` (`kernel/device/configs/oneios_native_defconfig`),
  so the hosted machine layer stays out; hosted `0076-native-os-layer.patch` opens to it what is about iOS on the image
  set and not about the host (the root, Data, seed and launch policy of 0004 and the later blocks of `kern_exec.c`, the
  boot manifest's `/chosen`, the parallel code hash, iOS 27's shared region, the AES and key store starts, one kind of
  CPU, the debugger's acknowledge timeout; `kernel/patches/oneios-hosted/README.md` "0076"). The archives: `storage`,
  `board`, `aes`, `keystore`. `out/kernel/current-native/xnu.macho`.
- The disks: `OneiOSHostedDisk` (the hosted class, the same nodes) maps its virtio-mmio window, and since no host checks
  the reads, checks every chunk of os, systemos and app against the image set's hash list when it is first read
  ("oneios,hash-list" in the node; verified only when the list's SHA-256 is the node's "oneios,verified").
- The collection: `kernel/prelink/build.sh` sees a native kernel (no `hosted_halt`) and leaves Apple's kexts as they are
  (`out/kernel/native-24A446/kernel.kc`). Its step 3, the hosted rewrites, broke a native kernel twice: smr_hash's `orr`
  turned into an `and` made APFS read `0xff00fff007ddd988` (`__smrh_empty` with bits 48..55 cleared) at the Data volume's
  first mount, and `mrs MPIDR_EL1` became `ldr [x18]`.
- The loader: `xnuloader-qemu ... --image DIR --data FILE` reads the image set as xnuhost does (its `imageset.c`: manifest,
  hash lists, trust caches): the five disks on transports 0-4 (os, systemos, app, the seed read-only; the Data store
  writable), each node with xnuhost's identity, `/chosen` root-matching on the OS volume, the four trust cache modules.
  Modern virtio (`-global virtio-mmio.force-legacy=false`), and a Cortex-A710 (`pauth-impdef=on,sve=off,cntfrq=1 GHz`):
  QEMU's `max` has FEAT_LVA, which the DEVELOPMENT kernel asserts no CPU has (`vm_sanitize.c:643`, at launchd's first
  jobs).
- The loop: `kernel/build-native.sh` (about 25 min from an empty tree, 2 min after a one-file change), then
  `KERNEL=$PWD/out/kernel/current-native/xnu.macho kernel/prelink/build.sh` (1 min), then
  `scripts/bg.sh qemu-native env KERNEL=$PWD/out/kernel/native-24A446/kernel.kc IMAGE=<image set> DATA=<store>
  CPUS=4 RAM_MIB=2816 ARGS_ADD=oneios-data-wait=1800 OUT=$PWD/out/qemu/native scripts/qemu/boot-vm.sh 3600`. The
  store (16 GiB, sparse, made by `boot-vm.sh`) is kept in RAM (`/tmp`); the image set stays where it is, read-only.

Runs of 2026-11-20 (consoles in `logs/native-qemu/`, local):
1. The disks start, all three images verified; the root (`Rave24A446.V54OS`) mounts and is accepted as a verified one;
   the SystemCryptex over `/`; AES and Image4; `newfs_apfs` for the Data store, then APFS's data abort on the cleared
   pointer above. Fixed in the prelink.
2. cpu 1 stops at its start (`cpu_common.c:957`, `cpu_signal` 0x8 instead of `SIGPdisabled`): sheng's IPI recovery
   cleared the flag from another vCPU, as it did under xnuhost (hosted 0006 turned it off; 0076 turns it off here too).
   The others went on at 0.2 MB/s of disk.
3. No stop until launchd: `newfs_apfs` (the first user process; its dyld hashed the shared cache, 5.8 GiB, at about
   12 MB/s on 4 vCPUs) formatted the store, `seed verified: 6039 files (1252972721 bytes), 14818 folders`, the launch
   policy ran (cryptex views, GPU addon, staged apps, jobs, features), **`load_init_program: /sbin/launchd`**, launchd
   started `oneios-init` and `oneios-bootstat` (`ONEIOS-GUEST stage=bootstat-up`), then `sched_clutch.c:6589` (one
   kind of CPU; hosted 0016, opened by 0076).
4. The store's second start (mounted as it is, no seed): launchd in about a minute; `hello, launchd`, `Darwin
   Bootstrapper Version 7.0.0`, and its boot tasks run (exclaves-boot, fsck, mount-phase-1, data-protection,
   detect-installed-roots, boot mode `main`, mount-phase-2, init-with-data-volume, MSUEarlyBootTask, MobileAssetEarlyBootTask,
   fips, ...), then `vm_sanitize.c:643` (the CPU model, above).
5. **The Cortex-A710 (about 15 minutes, stopped by hand): no panic.** launchd's 60 boot tasks (keybag, usermanagerd,
   dirs_cleaner, ...), then its daemons: 51 programs in all, among them runningboardd, configd, logd, amfid, keybagd,
   powerd, thermalmonitord, locationd, CommCenter, bluetoothd, sharingd, containermanager, symptomsd, driverkitd and
   **backboardd** (`ONEIOS-GUEST stage=backboardd-up t=176`: 176 s after `oneios-bootstat`); no crash on the console.
   Not seen: SpringBoard (backboardd has no display here). The clock is at 1970 (no RTC on the machine tree).
   logd_helper used 88 % of a vCPU for 100 s (`caught burning CPU`).

## Next
1. A display on the bench, for backboardd and SpringBoard (QEMU's ramfb or virtio-gpu 2D through a scanout like
   UniXNU's GopFramebuffer), and the RTC (virt's PL031) for the clock.
2. The S25's live FDT, read only, for the SM8750 machine tree; how the phone gets its root (a RAM disk first: no UFS
   writes); the phone's ID registers (FEAT_LVA: the assertion above).
3. The Adreno 830's kernel side for XNU with Turnip (references: UniXNU's Adreno740, UniXNU-houji's A750 port).
