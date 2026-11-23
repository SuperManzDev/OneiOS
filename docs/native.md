# The native path: XNU alone on the phone

The owner's decision of 2026-11-19, after session 7 (`docs/SM8750_PORT.md`): hosted iOS is stable, but it runs at 55-98
fps while cool and at 10 fps when iOS's thermal state is "too hot" (case 46 C), and the host's Linux kernel takes more
than half of the CPU it uses. Goal: XNU alone on the Galaxy S25 Ultra, started from a running Android with Linux then
shut down (no bootloader unlock). Months of work; the GPU (Adreno 830, no XNU driver anywhere) is the wall.

## Facts that shape it
- The SM8750's Oryon cores have no 16 KiB translation granule (ID_AA64MMFR0_EL1 TGran16 = 0, measured on a OnePlus 13
  by palazik/ios-dodge, `external/ios-dodge/NOTES.md`): 4 KiB pages. The standalone kernel carries UniXNU's native-4k
  patches (`out/kernel/current/series.txt`).
- From ios-dodge, **verified on the S25's live FDT on 2026-11-20** (`oneios-tool phone hw`; the numbers are in
  `docs/SM8750_PORT.md` section 0b): GICv3 distributor 0x16000000, redistributors 0x16080000 with a stride of
  0x40000, the timer at 19.2 MHz (virtual timer INTID 27), PSCI by SMC, no usable UART. The S25 adds: 9 GiB of RAM in
  one piece at 0x8_c000_0000, a **command-mode** panel (the bootloader's splash at 0xfc800000 is not scanned out by
  itself), Samsung's hypervisor-backed protections in its kernel (`CONFIG_UH`, `RKP`, `KDP`) and no kexec.
- Starting XNU alone needs Linux to hand the CPUs over (the bootloader is locked: no `fastboot boot`, which is how
  ios-dodge, ios-sweet and ios-lemonadep start theirs). One UI runs as a VM of Qualcomm's Gunyah at EL2
  (`gh_virt_wdt` is its watchdog), with Samsung's protections beside it: whether they let a kernel module load, turn
  off Linux's MMU and set XNU's own TTBR1/TCR/SCTLR is unknown [U], and the first try that fails resets the phone.
- **The same Gunyah runs protected VMs for Android** (AVF): `ro.boot.hypervisor.protected_vm.supported=true`,
  `/dev/gunyah`, AVF's crosvm in the apex. XNU as such a guest runs at EL1 on the real cores with its own page tables,
  and One UI stays up under it (see "A VM under Gunyah" below).
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

## The display on the bench (2026-11-20)
The hosted kernel's display, as it is: UniXNU's NativeMobileDisplay under the collection's `IOMobileFramebufferLegacy`
and Droid-VM's virtio-gpu scanout (`kernel/patches/oneios-hosted/display/README.md`), with Droid-VM's virtio-gpu transport
(the `gpu` archive). Without a 3D device the GPU archive says "display only" and publishes no Metal accelerator, so iOS
draws with the CPU (as on the PC under xnuhost, `docs/gpu.md`).
- The native kernel links `gpu` and `display` (`CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y`); 0076 turns the display on as the
  hosted kernel has it (`bsd_init.c`'s two switches) and gives the two hosted hooks the GPU archive declares weak a
  native default.
- `xnuloader-qemu --display 828x1792 --scale 2` (boot-vm.sh `SCREEN=828x1792 SCALE=2`): QEMU's `virtio-gpu-device` on
  transport 5 (2D; `qemu-hw-display-virtio-gpu`'s module, from the sysroot with `QEMU_MODULE_DIR`), the boot video right
  after the kernel's window (6 MiB at 0xf3004000), the tree's mode and scale, `offline-legacy-unit-scale=2 sheng-vsync=60`
  in the boot arguments. 828x1792 at @2x is an iPhone 11's 414x896 points: a third of the phone panel's pixels for the
  CPU to draw.
- The transport wanted a control queue of 512 descriptors; QEMU's 2D device has 64. Now it takes what the device offers
  (`staged/gpu.rs` `TRANSPORT`, 32 slots of which 12 large on QEMU; 256 as before with xnuhost and crosvm):
  `virtio-gpu: the device's control queue is 64 descriptors: 32 command slots, 12 of them large`, then
  `oneios-display: panel 828x1792 at 0xf3004000 ...; virtio scanout state 2 (2 = presenting)`.
- Watching: `VNC=1` serves the display on 127.0.0.1:5901 (`krdc vnc://127.0.0.1:5901`, this PC only);
  `scripts/qemu/shot.sh out/qemu/native` writes a PNG of it through QEMU's monitor.
- Run 6 (4 vCPUs, the store's third start): thermalmonitord, then backboardd (`ONEIOS-GUEST stage=backboardd-up
  t=208`) open the framebuffer; 15 minutes after launchd, `oneios-display: swap 1 accepted`, `the compositor swaps
  format 'BGRA' ... 828x1792 ... taken`, `frame 1 scanned out through virtio-gpu`: **the Apple logo on the screen**, drawn
  by backboardd with the CPU (`logs/native-qemu/2026-11-20-run6-apple-logo.png`). SpringBoard runs (pid 34) and had
  not drawn after 12 more minutes; chronod aborted once (EXC_CRASH). Ended at 40 minutes by a mistake of the bench's
  (killing the `timeout` that holds QEMU kills QEMU too: give a long timeout from the start).

- Run 7 (6 vCPUs, a 3 h limit, stopped by hand after 1 h 50 min): launchd 13 minutes after the start, backboardd at
  t=271 s, the logo 18 minutes after launchd, and nothing else for an hour while all six vCPUs were 65-80 % busy. iOS's
  own log, read from a copy of the store (`oneios-tool ioslog health` / `show --proc`, no mount): no hang, no watchdog,
  no jetsam, one crash (chronod); SpringBoard took the display (`FBSDisplayConfiguration ... Main; mode: "414x896@2x
  60Hz sRGB SDR"`) and bootstrapped PosterBoard, then went quiet; and **rapportd logged 1.54 million lines in 14 minutes**
  (`DiscoveryCache: After pruning 0 records, 0 records remain cached`, between Bonjour browses that mDNSResponder never
  answers: `kDNSServiceErr_Timeout`). Without a network interface rapportd spins and logd_helper with it. The bench now
  turns off rapportd and analyticsd with the phone's four (`oneios-disable-jobs=`, hosted 0053); the lasting answer is
  a network on the bench.
- Touch and keys: `xnuloader-qemu --input tablet,keyboard` (boot-vm.sh `INPUT=tablet,keyboard`) puts QEMU's
  virtio-tablet-device and virtio-keyboard-device on the transports after the GPU as `/arm-io/input-N`; Droid-VM's
  virtio-input transport takes an absolute pointer as one finger while its button is down (the VNC client's mouse), and
  the native kernel links the hosted `hid` archive. Run 8: `droidvm input /arm-io/input-0: QEMU Virtio Tablet, pointer`,
  `input-1: QEMU Virtio Keyboard, keyboard`, `touch screen: Digitizer/Touch Screen, Built-In, DisplayIntegrated published
  on the event service`, both event drivers attached.
- The CPU report runs without a GPU now: `oneios-cpu: 10 s: kernel_task[0] 298%, launchd[1] 6%, ...` (the kernel's
  threads hashing the shared cache for launchd's dyld).
- **Run 8 (rapportd and analyticsd off, touch and keys): the lock screen.** launchd 11 minutes after the start, the
  logo, and 18 minutes after launchd **iOS 27's lock screen** (the date and clock, the battery, the home indicator; the
  date is 31 December 1969 in Pacific time: no RTC), drawn by SpringBoard with the CPU
  (`logs/native-qemu/2026-11-20-run8-lock-screen.png`). `oneios-cpu` then: iconservicesagent, chronod, backboardd and
  SpringBoard. A swipe sent as VNC pointer events reached backboardd (the touch screen's event service has one user
  client, backboardd) and the lock screen answered with **"Swipe up to open"**; the unlock itself did not happen within
  the run: the clock went away and the screen stayed on the date, with backboardd and BackgroundShortcutRunner taking
  most of the six vCPUs and spindump running (an hour under QEMU is a few minutes of a phone). The run was stopped at
  2 h 25 min; its unified log was not on the store yet (logd had not written it out).
- Touching from a script: boot-vm.sh gives QEMU a QMP socket too (`qmp.sock`), and `scripts/qemu/touch.sh OUT tap X Y`
  / `swipe X1 Y1 X2 Y2 [MS]` sends the tablet's absolute events through it (QEMU's monitor has only relative ones).

## A VM under Gunyah (2026-11-20)
What the S25 says (read only, `docs/SM8750_PORT.md` section 0b): protected VMs only, `/dev/gunyah`, AVF's crosvm 0.1.0
with `--hypervisor`, `--protected-vm-without-firmware` and `--swiotlb`, its virtio devices on PCI. What others ran on
Gunyah [F, their reports]:
- OnePlus 13 (8 Elite, kernel 6.6.126, unlocked): Debian and Windows guests under Droid-VM's crosvm build,
  `--hypervisor gunyah --protected-vm-without-firmware --disable-sandbox`, virtio over PCI, a guest kernel with
  `CONFIG_DMA_RESTRICTED_POOL` (the host reads no guest RAM outside the swiotlb pool), 2 MiB huge pages reserved by a
  module against Android's fragmentation. AVF's own crosvm with a Debian kernel put that phone into Qualcomm's crash
  dump mode (a forced restart). https://blog.lyc8503.net/en/post/android-virtualization-on-snapdragon/
- Lenovo Legion Y700 gen 4 (SM8750, Android 15, unlocked): Debian under crosvm with `--protected-vm-without-firmware
  --swiotlb 64` after `ulimit -l unlimited`. https://github.com/polygraphene/gunyah-on-sd-guide
- Droid-VM's UniXNU guest (`external/UniXNU-droidvm/README.md`, "State"): XNU to the lock and Home screen under crosvm
  on Gunyah on SM8650 and SM8850, 4 KiB pages, devices on virtio-mmio (Droid-VM's crosvm branch `wip/virtio-mmio`),
  `--protected-vm-pseudo-unprotected`, which needs Droid-VM's host modules (`docs/plan.md` section 8). Phone resets
  seen there: a vCPU stuck in the hypervisor with a device the guest did not know; a guest stuck in WFE killed from
  the host (`tools/rig/pacprobe/README.md`).

None of them is a Samsung: whether Samsung's Gunyah takes a VM made from root's shell is unknown [U]; the bootloader
unlock of those phones is not what Gunyah asks for (root and `/dev/gunyah` are).

What it would change: the bench's native kernel is already a guest of that shape (GICv3, PSCI, Droid-VM's virtio-mmio
layout). Under crosvm it needs PSCI by HVC, the 19.2 MHz timer, every virtio buffer in the swiotlb pool (a protected
VM's host sees nothing else), and either a virtio-pci transport (AVF's crosvm) or Droid-VM's crosvm (BSD-3, built for
Android on the PC). The disk is the image set's files on /data served as virtio-blk, as on the bench: no UFS driver,
no RAM disk, nothing written to UFS by XNU. One UI stays up under the VM, and the way back is `crosvm stop`, no
reboot. The risk is the others': a guest that hangs in the hypervisor can reset the phone (One UI comes back, without
the temporary root).

The first test: a payload of a few instructions (Droid-VM's pacprobe is the model) under AVF's crosvm, 1 vCPU, 128
MiB, no device but the serial port: it prints a line on the 8250 at 0x3f8, reads the ID registers (TGran16, VARange,
PAC), never waits in WFE and powers off by PSCI by itself; a run still up after 20 s is stopped through
`crosvm stop`. It answers whether Samsung's Gunyah runs our VM at all.

**Run 1, 2026-11-20: it does.** `oneios-tool phone vm-probe` (`runtime/vmprobe/probe.S`, AVF's crosvm with
`--hypervisor gunyah --protected-vm-without-firmware`, 1 vCPU, 128 MiB; `logs/pa3q-vmprobe/run1/`): crosvm made the
VM ("creating hypervisor: Gunyah"), the payload ran and printed on the 8250; the 2^28-turn loop with the MMU off did
not end in 20 s, `crosvm stop` ended the VM (crosvm exit 0) and One UI ran on. What the vCPU says:
- EL1 (`CurrentEL` 4); `MIDR_EL1` `0x000f0480` (Gunyah's, not Oryon's), `MPIDR_EL1` `0x80000000`; PSCI 1.1 over HVC;
  `CNTFRQ_EL0` 19.2 MHz; the device tree at `0x87600000` (RAM from `0x80000000`), its magic right.
- `ID_AA64MMFR0_EL1` `0x100010000f001021`: 36-bit IPA, 16-bit ASIDs, **4 KiB granule only** (no 16 KiB, no 64 KiB).
  `ID_AA64MMFR2_EL1` `0x1220001112101011`: **VARange 0, no FEAT_LVA** (the bench's `vm_sanitize.c:643` assertion came
  from QEMU's `max` CPU, not from this core). `ID_AA64MMFR1_EL1` `0x1000111110312122`: PAN 3, VHE, HAFDBS 2.
- `ID_AA64ISAR1_EL1` `0x0011111101211052`: **pointer authentication, APA 5 (QARMA5 with FPACCOMBINE) and GPA 1**, LRCPC
  2, BF16, I8MM; `ID_AA64PFR1_EL1` `0x21`: BTI, SSBS 2; `ID_AA64PFR0_EL1` `0x1101000001111111`: FP16, GIC system
  registers, no SVE. `CTR_EL0` `0xb444c004`: 64-byte lines, IDC and DIC (no cache maintenance between data and code).
- `SCTLR_EL1` at entry `0x30d00980` (MMU, caches off).
The probe since run 1 times 2^20 turns with the MMU off, then turns the MMU on (an identity map, write-back RAM) and
times 2^28 turns, before it powers off: the vCPU's own speed, and whether PSCI's SYSTEM_OFF ends a VM on Gunyah.

**Run 2, 2026-11-23: the whole probe, and the VM ends by itself** (`logs/pa3q-vmprobe/run2/`): crosvm's run took about
a second, PSCI SYSTEM_OFF ended it (crosvm exit 0, no stop needed), One UI ran on. `SCTLR_EL1.I` is set when the
probe writes it (`0x30d01980`), yet with the MMU off a turn of the loop costs 105 ns (2^20 turns in 2,110,484 ticks of
19.2 MHz): every fetch goes to memory, which is why run 1's 2^28 turns could not end in 20 s. With the MMU on
(`0x30d01985`: M, C, I) 2^28 turns take 1,800,312 ticks, 93.8 ms: 0.35 ns a turn, 2.86 billion turns a second, the core's
own speed. Code that stays in the VM loses nothing to Gunyah.

Since run 2 the probe also reads `/memory` from the device tree, maps all of it, writes every 4 KiB page above itself
(but the tree's blocks) with its own address and reads it back (`MEM_PAGES`, `MEM_BAD_PAGES`, the ticks of each pass,
`MEM_AT` every 256 MiB): how much RAM a protected VM can have while One UI runs, and what lending it costs. `vm-probe`
keeps crosvm's tree (`--dump-device-tree-blob`, `fdt.dtb` in DIR: the machine an XNU guest gets) and refuses a VM
that would leave Android less than 1 GiB of its MemAvailable (the VM's RAM is pinned).

**Runs 3 and 4, 2026-11-23: 128 MiB yes, 1 GiB no** (`logs/pa3q-vmprobe/run3-128m/`, `run4-1g/`). Run 3: `/memory`
`0x80000000` + 128 MiB, the image at `0x80000000`; 31,744 pages written and read back, none bad, the whole run about
2 s; crosvm's tree kept (`fdt.dtb`, 4,251 bytes). The first write of a page costs 16.7 us and a read after it 9 ns:
Gunyah's demand paging gives the guest each page when it first touches it (`drivers/virt/gunyah/vm_mgr_mem.c`
`gunyah_gup_demand_page`: the page pinned, Qualcomm's platform hook assigns it by an SCM call, then donated and
mapped, one folio at a time). Run 4: the guest wrote past `0x90000000` (256 MiB), then crosvm logged "unknown gh exit
reason: 3" and "vcpu crashed": 3 is `GUNYAH_VCPU_EXIT_PAGE_FAULT`, the kernel could not give a page and hands the fault
to the VMM (`page_fault.attempt` is its error), which AVF's crosvm does not handle. crosvm ended by itself, One UI ran
on. The kernel's own line ("Failed to provide folio for guest addr: ...") had left the phone's log, which keeps
minutes (Wi-Fi's lines): `vm-probe` now keeps the kernel's lines of each run (`dmesg.txt`), and `phone dmesg` reads it.

Why 256 MiB [I]: that is 65,536 pages of 4 KiB, and they are 4 KiB because of the phone's transparent huge pages
(`phone hw` `thp.txt`): `enabled` always, `defrag` never, **`shmem_enabled` never**. crosvm's guest RAM is a memfd
(shmem), so `--hugepages` (MADV_HUGEPAGE) gets no 2 MiB folio; with them the same RAM takes 512 times fewer demand
paging calls (and SCM calls). buddyinfo had 11 free blocks of 2 MiB and 76 of 4 MiB: about 320 MiB without
compaction. `vm-probe --thp` sets `shmem_enabled` advise and `defrag` madvise for the run only and writes the old
values back after it (read first and checked against the words the kernel offers, a trap for an early end; a
restart of the phone also brings the defaults back).

**Runs 5 to 7, 2026-11-23: the limit is Gunyah's own memory** (`logs/pa3q-vmprobe/run5-1g-confirm/`, `run6-1g-thp/`,
`run7-1g-thp-compact/`). Run 5 (1 GiB, as run 4) kept the kernel's lines: "gunyah_vm_mgr: Failed to map guest address
0x9d557000: 10" and "Failed to provide folio ...: -12". 10 is GUNYAH_ERROR_NOMEM from the `addrspace_map` hypercall:
the hypervisor has no memory left for the VM's stage 2 and its records, after about 470 MiB of 4 KiB pages. Gunyah's
whole region is 18 MiB (`gunyah_hyp_region@80000000`, `0x1200000`) for all its VMs (One UI's, the trusted UI's,
Samsung's uH, ours). Run 6 (`--thp`, the settings set and written back as they should): to `0xa1110000`; buddyinfo
had no free 2 MiB block before it, so the folios were 4 KiB again. Run 7 (`--thp` and one `compact_memory` first):
127 free 2 MiB blocks before the VM, to `0xa64a8000` (610 MiB). Each of runs 4 to 7 filled the hypervisor's memory
for a moment (crosvm ends at once and gives it back); One UI ran on each time, but it is not a thing to do often.

Why so few 2 MiB blocks (`phone hw`, `meminfo.txt`, `thp.txt` with pagetypeinfo): MemFree 0.99 GB of 11.4 GB; the file
cache holds 3.6 GB (2.6 GB of it inactive), apps 2.5 GB, the kernel's slabs, stacks, page tables and vmalloc 1.7 GB.
Of the 5,736 pageblocks of 2 MiB, 3,722 are Movable (7.3 GB), 1,581 Unmovable (3.1 GB), 312 CMA, 121 Reclaimable.
Compaction moves pages but frees none: a VM of iOS's size (3 to 4 GiB) in 2 MiB folios needs that much memory freed
first (the file cache dropped, then compacted) and taken at once (a guest that touches all its RAM at boot), at every
start, since a temporary root cannot reserve huge pages at boot (Droid-VM's `gh_hugepage_reserve` does, from a
module loaded at boot; lyc8503 hit the same wall at 1 GB without it).

**The file cache dropped, 2026-11-23: no** (`oneios-tool phone hugeblocks --free`: a sync, `drop_caches` 3, one
`compact_memory`; no VM). First try: MemFree 505 MiB to 2,902 MiB in 2.4 s, Cached 3,872 to 1,603 MiB, free 2 MiB
blocks 0 to 124 (248 MiB). Second try: MemFree 1,461 to 2,851 MiB, 2 MiB blocks 0 to 26 (52 MiB); by order (4 KiB <<
order) `64331 53559 29200 13048 7123 2764 948 289 93 22 2`: the freed memory is in pieces of 4 KiB to 1 MiB, and
compaction does not join them (most Movable pageblocks hold pages it cannot move). So at run time this phone cannot
give a VM gigabytes of 2 MiB blocks, and in 4 KiB pages Gunyah's 18 MiB stops it near 500 MiB.

**Run 8, 2026-11-23: a bigger VM does not get further** (`logs/pa3q-vmprobe/run8-2g/`, 2 GiB, as run 5): "Failed to
map guest address 0x9632b000: 10", at about 350 MiB, before run 5's 470 MiB of a 1 GiB VM. The limit is the
hypervisor's, not the VM's.

**Verdict for this phone: a VM under Gunyah is not a way to run iOS** [F]. Its vCPUs run at the core's own speed, but
its RAM ends near 0.5 GB: Gunyah lends it in 4 KiB pages from a hypervisor heap of 18 MiB for all its VMs, the 2 MiB
blocks that would take 512 times less of it are not there at run time (52 to 248 MiB of them, even with the file
cache dropped and the memory compacted), and with a temporary root nothing can reserve them at boot (Droid-VM's way:
an unlocked bootloader or a lasting root). iOS needs 3 to 4 GB. The probe, `vm-probe`, `hugeblocks` and `dmesg` stay
for another phone or another firmware.

### gh_hugepage_reserve with One UI stopped (2026-11-23)
The owner's idea: the module of Droid-VM made for this wall (https://github.com/Droid-VM/gh-hugepage-reserve,
GPL-2.0), and One UI's framework stopped and started again around its acquire, as the iOS switch and return do, so
that the RAM apps and system_server held is free without a restart of the phone (and of its temporary root). Its
README measured the same wall ("84% of all 2MB windows hold an unmovable hard straggler" after uptime); its tests
ran in QEMU, the on-device ones are its open work. Built with `scripts/hugepage/build.sh` (the authors' DDK image
for android15-6.6 by digest, under bubblewrap); `oneios-tool phone hugepool` loads it and runs the cycle.
- Its ABI preflight on the phone's BTF (with the phone's kernel version: run with the PC's 7.0.10 it picks the
  wrong rows) passes every function but `kallsyms_lookup_name`, a false alarm: its argument is plain `char`
  (unsigned since Linux 6.2's `-funsigned-char`, encoding 0 in BTF, the same as `strlen`'s), which the preflight
  prints as `uchar`. It stays in `disable_kapi` anyway (it only turns off a CMA lever this use leaves off).
- Load 1 (One UI up, `pool_want=64`): insmod in 0.11 s, every hook attached, "RAM: 22 ranges, 11450 MB
  present", `pool_size_max` 2678; the pool stayed at 0 (no free 2 MiB block beside One UI). One UI ran on.
- **Crash 1**: `acquire 1` beside One UI panicked the kernel in the module's worker ("Unable to handle kernel
  NULL pointer dereference", `gh_pool_ext_to_avail+0x7c4`, `gh_adjust_fn`; `/proc/last_kmsg`, kept in
  `logs/pa3q-hugepool/crash1-pstore.txt`); Samsung's upload path restarted the phone in about 20 s, One UI came back,
  nothing was lost. The cause is ours: our insmod line had no `pageblock_order_val` (the module's own load.sh always
  passes the one of `/proc/pagetypeinfo`), so the module took -1 as its block order and its block size S as
  `1u << (-1 - 9)` (4 Mi slots on arm64): `gh_try_complete_block` walked siblings from slot 0 into a GB with no RAM
  (slot 1024, 4 GiB) and read a NULL slot. `hugepool load` now passes `pageblock_order_val` (refusing a phone
  whose order is not 9), `migrate_cma_val=-1` and `pool_want_with_cma=0` (the CMA side off). The module's own
  fragility (no check of that value, no NULL check there) is the authors' to hear about.

## Next
1. The owner's choice, with the facts above. The Gunyah VM was tried (2026-11-20 to 23) and does not fit iOS on this
   phone ("A VM under Gunyah", verdict). Left: the hosted path that runs today, or XNU alone, whose unknowns are the
   hand-off under Samsung's protections, the command-mode panel with no display driver, and UFS behind file-based
   encryption (a RAM disk cannot hold the 16 GB image set).
2. On the bench: the unlock and the Home screen (more time, or less work: the display's size, the jobs that a bench has
   no use for), a network (QEMU's virtio-net: mDNSResponder, rapportd and the others have nothing to talk to), the RTC
   (virt's PL031) for the clock, and the CPU report's idle time (`hosted_processors_idle_mach` from XNU's own processor
   idle counters, so that `all` is bounded by the CPUs). The bench's speed is QEMU's (TCG on x86): it proves the paths,
   not the frame rate.
3. The phone's ID registers (FEAT_LVA: the assertion above), from the first test's payload.
4. The Adreno 830's kernel side for XNU with Turnip (references: UniXNU's Adreno740, UniXNU-houji's A750 port).
