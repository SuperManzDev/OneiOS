# SM8750 port — Galaxy S25 Ultra (pa3q)

Status (2026-11-19): **passive audit and read-only bridge tools PASS on the owner's S25 Ultra; no session, no active test yet.** This branch (`s25-ultra-sm8750`) starts from the
S26 Ultra work (YMGPwcca/OneiOS PR #1, SM8850 `m3q`), not from upstream `main` (S24 Ultra, SM8650 `e3q`), because
the S25 Ultra is closer to the S26 than to the S24 (section 2).

Marks as in `docs/hardware.md`: **[F]** read in a source or measured on the phone, **[I]** inference, **[U]** unknown
until the audit runs.

## 0. Gate: root

OneiOS needs root (KernelSU) with One UI running: xnuhost, KGSL, `/dev/udmabuf`, the bridges and the OneOS switch all
run as root. **The port cannot start without a rooted S25 Ultra.**

- One UI 8 removed the "OEM unlocking" toggle on the S25 series; reports say devices already unlocked may be
  re-locked by the update [F: press reports, July 2025]. The S24 and S26 targets of this project are rooted, so a
  route exists on some firmware [U: which one, and whether it exists for pa3q].
- US models (SM-S938U/U1) have never had an unlockable bootloader [F: press reports].
- First question for the owner: model number (`SM-S938B` or other), firmware (`ro.build.PDA`), and whether the
  bootloader is already unlocked and KernelSU installed. No other step of this plan is useful before that.

## 0b. The owner's phone (measured 2026-11-19, `audit-preflight-pa3q-ondevice.sh` + runtime 0.1.8 tools) [F]

- SM-S938B (`pa3qxeea`), SM8750 board `sun`, firmware `S938BXXSDDZJ1`, Android 17 (SDK 37), One UI 9.0 (`90000`),
  kernel `6.6.127-android15-8`, 4 KiB pages, `/proc/config.gz` present.
- Root: **temporary**, KernelSU LKM in "jailbreak mode" loaded through an exploit (dfroot) on a **locked** bootloader
  (`flash.locked=1`, `vbmeta.device_state=locked`, `verifiedbootstate=green`, `warranty_bit=0`). Domain `u:r:ksu:s0`,
  SELinux Enforcing. KernelSU's busybox (all nine applets) and resetprop present. Root is gone after any reboot: the
  return's failsafe reboot lands in stock One UI without root (re-run the exploit before a session). A kernel panic
  under load may come from the exploit's state, not OneiOS: read pstore before blaming our code.
- Audit: 198 lines, 81 PASS, 0 FAIL, 1 WARN (`hwservicemanager` stopped: Bluetooth is AIDL `IBluetoothHci/default`,
  registered; no HIDL), 22 BLOCKED (stage image not present, S24-only gate, session-only checks).
- OneOS app 0.5.14 installed; a stage at `/data/local/tmp/oneios` with runtime **0.1.8** (`ea06d46`), all checksums OK;
  no image set.
- Treble HALs registered: health, thermal, vibrator, lights, sensors, wifi supplicant, bluetooth (AIDL), audio core,
  camera provider `internal/0`.
- **CPU** (`bridgectl cpu`): cpu0-5 capacity 765, 384000..3532800 kHz; cpu6-7 capacity 1024, 1017600..4473600 kHz;
  MIDR `0x514f0014` / `0x513f0014` (Qualcomm Oryon, part 0x001); PAC (`paca pacg`), BTI. Plan 6 E + 2 P vCPUs, thread
  placement and `uclamp.min` done: the S26's shape, no change needed. Cache sysfs has `level`/`type` but no `size`,
  so caches print 0; nothing passes them to the guest (no use in xnuhost or the loader), so no fix needed.
- **GPU** (`gpuctl nodes`, `probe`): `Adreno830v2`, KGSL, udmabuf (64 MiB, 1024 runs), system heap open; drm2kgsl:
  chip `0x44050000`, gpu id 6850, `0x4000000000 + 192 GiB`, GMEM 12288 KiB, 160..832 MHz (`msm-adreno-tz`). The
  guest's Turnip (Droid-VM/mesa `cecc0e9`, `freedreno_devices.py`) has `Adreno (TM) 830`, a8xx gen1, by the wildcard
  `0xffff44050000` (and `0x44050001` KGSL): the chip is known to the guest driver [F]; never rendered by it [U].
- **Input** (evdev): touch `sec_touchscreen` event8 (direct, MT); `sec_touchpad` event9 (MT, not direct);
  `sec_touchproximity` event10 (direct, no MT: not a touch candidate in `input.c`); S Pen `sec_e-pen` event12 (pen,
  pressure, tilt); keys Volume Up `gpio-keys` event0, Power `pmic_pwrkey` event3, Volume Down `pmic_resin` event4;
  haptics `sec_vibrator_inputff` event6 (FF periodic, sine, custom); ALS `als_rear` event7.
- **Battery** (`batteryctl`): samsung backend works: 5000 mAh full/design, 753 cycles, max77775 fuel gauge.
- **Thermal** (`thermalctl`): nominal, step 0, no pseudo-zone at 105 C (no `cpu-hw-trip-*` on this phone); dead
  modem/mmW zones read -273000 and are not CPU/GPU zones.
- **Backlight**: `panel` max 0; `panel0-backlight` max 12750 (now 1571). Same shape as the S26: the bridge's
  default ceiling would be 12750; a slider sweep (read only) is needed before any write (S26 method).
- Memory: 11.4 GB, 16 GB zram swap, 3.2 GB available with One UI up.
- **Live FDT and /proc, 2026-11-20** (`oneios-tool phone hw`, read only; the files stay in the git-ignored
  `logs/pa3q-hw/`: they carry the handset's serial number). GICv3: GICD `0x16000000` (64 KiB), one GICR region
  `0x16080000` (2 MiB, stride `0x40000`), ITS `0x16040000`. PSCI 1.0 by SMC. CPUs: MPIDR `0x000`..`0x500` (6 cores,
  cluster 0, MIDR `0x514f0014`) and `0x10000`, `0x10100` (cluster 1, `0x513f0014`), REVIDR 0. Arch timer 19.2 MHz, PPIs
  13/14/11/10 (sec phys, phys, virt = INTID 27, hyp). RAM: 22 ranges; the largest is **9 GiB in one piece at
  `0x8_c000_0000`..`0xb_0000_0000`** (System RAM in iomem, no reserved hole in it). Splash: `cont_splash_region`
  `0xfc800000` + 43 MiB. Panel: `nt37801` AMOLED, **command mode** DSI with DSC, `card0-DSI-1` 1440x3120 at
  120/80/60/48/30/24/10 Hz (no scan-out without the DPU's kick or autorefresh). UFS: `ufshc@1d84000`, SPI 265.
  Partitions to back up before a native test: `modemst1` sda1, `modemst2` sda2 (4 MiB each), `fsc` sda3, `persist`
  sda5 (32 MiB), `efs` sda6, `sec_efs` sda7 (20 MiB each), `param` sda8, `steady` sda20, `fsg` sdd1 (4 MiB),
  `uefivarstore` sdd13. Kernel config: `CONFIG_UH=y`, `CONFIG_RKP=y`, `CONFIG_KDP=y` (Samsung's hypervisor-backed
  protections), `CONFIG_MODULE_SIG_PROTECT=y`, no kexec; VA 39 bits, PA 48 bits.
- **Gunyah and AVF** (same read): `/dev/gunyah` `crw-rw-rw-` (`vendor_gunyah_dev`), no `/dev/kvm`;
  `ro.boot.hypervisor.protected_vm.supported=true` and no `vm.supported` (protected VMs only, as on the other 8 Elite
  phones); `feature:android.software.virtualization_framework`; AVF's `crosvm` 0.1.0 with `--hypervisor`,
  `--protected-vm-without-firmware`, `--swiotlb`, `--serial`, `--gpu`, `--input`, `--vhost-user`, virtio over PCI only
  (no virtio-mmio option); no `pvmfw.img` in the apex. Qualcomm's own `qcrosvm` runs the `trustedvm` since boot.
  Modules: `kernelsu`, Qualcomm's `gh_*` and `gunyah_loader`, `gh_virt_wdt` (the watchdog of the VM One UI runs in).
  `vm info` from root's shell fails (`virtmgr`: "Failed to remove memlock rlimit: Permission denied").

## 1. Workflow (the S26 method)

1. **Passive audit**: `scripts/phone/audit-preflight-pa3q.sh` over adb. Read only: no staging, no writes, no
   vibration, no switch. Output stays on the PC (`~/oneios-pa3q-ci/audits/<stamp>/checks.tsv`); never publish it
   raw (MAC addresses, serials).
2. **Per-bridge read-only tools** from a checksum-verified CI runtime in `/data/local/tmp/oneios-pa3q-diag/bin/`:
   `bridgectl cpu`, `gpuctl nodes|routes|probe`, `brightctl inspect` (never `info`/`levels` before a ceiling is
   known), `batteryctl`, `thermalctl`, `wifictl status`, `bridgectl haptic-inspect`.
3. **One active test at a time**, each reversible, each written up here with its log lines: `displayctl test`
   (overlay, no takeover), `gpuctl submit`, `wifictl scan`, haptics, audio, camera.
4. **Fix, do not only probe** (`CLAUDE.md`): every finding that is wrong on pa3q is fixed in the bridge with a
   fake-sysfs or fake-HAL regression test, as the S26 did for `cpu-hw-trip-*` and the backlight ceiling.
5. Only after 1-4: the OneOS fork's device gate for pa3q, then the first switch with the Volume Up + Volume Down
   return tested.

## 2. What differs from the S24 and the S26

| Area | S24 Ultra (SM8650, baseline) | S26 Ultra (SM8850, PR #1) | S25 Ultra (SM8750) | Port work |
|---|---|---|---|---|
| CPU | 2 A520 + 5 A720 + 1 X4 [F] | cpu0-5 cap 741 / cpu6-7 cap 1024, all Oryon-class [F] | 6 Oryon "M" + 2 Oryon "L", no little cores [I] | same shape as S26: reuse its topology fixture; add a pa3q fixture from `bridgectl cpu` |
| GPU | Adreno 750, KGSL [F] | chip 0x44050a00, 192 GiB VBO, GMEM 18 MiB [F] | Adreno 830, KGSL [I] | `gpuctl probe`; drm2kgsl's chip table and VBO size; then guest Metal |
| Kernel | 6.1 android14 [F] | 6.12 [F] | 6.6 android15 at launch [I] | KGSL/udmabuf UAPI, `shmem_enabled`, page size (4 KiB expected [U]) |
| Panel | 1440x3120 [F] | 1440x3120 120 Hz, `panel0-backlight` max 12750, slider plateau 2300 [F] | 1440x3120 120 Hz [I] | backlight names and a measured ceiling (S26 method: slider sweep, read only) |
| Touch | `sec_touchscreen` [F] | `sec_touchscreen` event8 + `sec_touchpad` (not direct) [F] | `sec_touchscreen` [I] | audit's input section; S26's preference for `sec_touchscreen` already covers stray direct devices |
| S Pen | Bluetooth S Pen, `sec_e-pen` [F] | `sec_e-pen` event12 [F] | no Bluetooth in the pen; digitizer still `sec_e-pen` [I] | only the digitizer is used (`pen.c`), so likely none |
| Keys | event0/2/4 [F] | gpio-keys / pmic_resin / pmic_pwrkey [F] | [U] | the return chord depends on it: audit's input section first |
| Haptics | CS40L26A, `sec_vibrator_inputff` [F] | same name, upload failed with EINVAL [F] | [U] | `haptic-inspect`, then the S26's diagnostics for upload vs play |
| Thermal | xo-therm etc. [F] | `cpu-hw-trip-*` pseudo-zones, fixed [F] | [U] | zone names from the audit; a fixture |
| Battery | `power_supply/battery` [F] | works unchanged [F] | 5000 mAh [I] | `batteryctl` once |
| Wi-Fi | FastConnect 7800, supplicant AIDL [F] | supplicant AIDL works [F] | FastConnect 7900 [I] | `wifictl status`, then `scan` |

## 3. Files a pa3q port is expected to touch

From PR #1's diff (`git diff --stat 9f4459b pr1`): `runtime/bridges/tests/test_bridges.c` (topology fixture),
`runtime/bridges/thermal/thermal.c` (zone classification), `runtime/bridges/brightness/` (ceiling),
`runtime/bridges/haptics/haptic.c`, `runtime/xnuhost/src/input.c` and `config.c`, `runtime/xnuhost/drm2kgsl/`
(GPU), `tools/oneios-tool/tests/phone/hw-backend.sh` (a pa3q stand-in phone), the `app` submodule (OneOS's device
gate), and this file. The kernel (`kernel/`), the image set and iOS's userland are the same for every phone [I]: the
guest is still iPhone18,2 iOS 27.0.1.

## 4. Log

- 2026-11-19: branch created from PR #1 head `0e7329d`; `scripts/phone/audit-preflight-pa3q.sh` added (the S26
  audit plus input, KGSL, thermal, power_supply, backlight, HAL, boot-state and memory inventories, all read only).
  Its input parser was checked on the PC against an S26-shaped `getevent -pl` sample.
- 2026-11-19: ran on the owner's phone over wireless adb (section 0b): audit 0 FAIL; `bridgectl cpu`, `gpuctl nodes`,
  `gpuctl probe`, `batteryctl`, `thermalctl` all `ok`. Next: the S26 runtime's `displayctl` fix (the 0.1.8
  `displayctl test` waits for `session/ready` and posts 0 frames) and `brightctl inspect`, built by CI from this branch;
  the OneOS fork's device gate for pa3q; the image set (IPSW fetch and kernel build need Linux or macOS).
- 2026-11-19: runtime built from this branch in a Linux x86_64 container (NDK 28.2.13676358, build-tools 35.0.0,
  `external/virglrenderer` at `7e17c8e`; the fork's release workflow needs the S26 owner's self-hosted runner
  `oneios-cachyos`, so it cannot run in this fork). Staged to `/data/local/tmp/oneios-pa3q-diag` (17 files, SHA-256
  checked on the phone); the 0.1.8 stage untouched. Read-only results: `brightctl inspect` (`panel0-backlight` max
  12750 selected, no writes), `bridgectl haptic-inspect` (event6 periodic/sine/custom/gain, 4 effects),
  `gpuctl routes` (all four routes PASS; 256 GiB VBO starves addresses, 192 GiB works, as on the S26),
  `wifictl status` (supplicant AIDL, 5 GHz, ok).
- 2026-11-19: **two GPU fixes found on this phone** (`docs/gpu.md` section 1): the fence table (`gpuctl submit`
  crashed with SIGSEGV in runtime 0.1.8 too: the Adreno 830's fence-fd path) and the udmabuf ENOMEM fallback
  (`gpuctl large` failed: order-5 page-array allocation on the 6.6 kernel). Both PASS on the phone after the fix.
  Host tests: 14057 checks, 1 failed (`test_xnuhost.c:2329`, mlock past RLIMIT_MEMLOCK: the container runs as root
  with CAP_IPC_LOCK; not related).
- 2026-11-19: `displayctl test` on the panel (no takeover): layer up, first frame, layer removed, One UI back. The
  owner's panel was at HD+ (720x1560: One UI's "Screen resolution"); set to WQHD+ the helper reports 1440x3120 at
  1:1. The helper posted about 56 frames a second (279 in 5 s; the S26 posted 115): the panel ran at about 60 Hz.
  The square **flickered** at both resolutions: the test wrote the shared frame while the helper copied it (the
  known "no frame handshake" limit). Fixed with the frame handshake (`XHDW`/`XHDA`, `runtime/display/README.md`):
  483 steps in 8 s all acked, the owner saw no flicker. Display host tests 133 checks, 0 failed (with an ack-less
  stand-in helper too). Before a session: the panel at WQHD+; xnuhost to use the handshake for iOS's swaps.
- 2026-11-19: the device gate. `oneios_switch.sh` refused every phone but the S24 Ultra (`SM-S928*:e3q:SM8650`, a
  fail-closed policy line: the switch itself is device-generic, the volume keys are found by capability, the clocks,
  backlight, watchdog and USB by named sysfs paths). At the owner's decision `YumikoHayamato/OneOS`
  `oneios-s25-ultra` `d928cdc` adds `SM-S938*:pa3q:SM8750`; `app/` points at it. `oneios-tool` (built static,
  musl) runs from the owner's PC over wireless adb: `phone status` said `ready 0 unsupported device: SM-S938B (pa3q,
  SM8750)` before the new script was staged.
- 2026-11-19: on the phone the installed app is OneOS 0.5.14 (versionCode 143), older than the fork (its
  `oneios_root.sh` 32450 bytes `6c635500...`, the fork's 37161 `7c118c76...`; the app runs only the root script whose
  hash its build pins). So `app stage-scripts` was not used (it would stage the fork's newer trio beside the old
  app). Instead the staged `oneios_switch.sh` (taken then for 0.5.14's; it was a hand-made variant of it, see the
  entry of session 2 below) got the same one gate line (`SM-S938*:pa3q:SM8750`; the
  original kept at `/data/local/tmp/oneios_switch.sh.orig-0.5.14` and in the owner's `HakiOS/stage-backup/`). Then
  `oneios-tool phone status`: `ready 0 no iOS image set is staged`: the gate passes; the image set is what is left.
  The app re-sends its own scripts when it stages (the `script` verb): re-apply the line, or install the fork's APK
  (needs a GitHub token with `read:packages` for tribalfs' packages, and an uninstall of the 0.5.14 signature).

- 2026-11-19: **the owner's PC (Garuda, KDE X11), section 0 of `docs/building.md`.** `/` has about 280 MB free and
  is not to be used: the checkout is on `/mnt/tkg-build` (an ext4 image, loop-mounted from the NTFS games disk,
  ntfs-3g/FUSE) and the bulky parts (`inputs/`, NDK r28c, build-tools 35.0.0, JDK 17, pacman downloads) on
  `/mnt/giochi/Linux/oneios`. rustup 1.98.0 with `rust-src`, the pinned `ipsw` CLI. The missing Arch packages (tmux,
  cmake, ninja, qemu-user, dash, meson, python-mako, android-tools, libdispatch, plus jsoncpp, protobuf, abseil-cpp)
  are extracted, not installed, into a private sysroot `/mnt/tkg-build/oneios/sysroot` (`. sysroot/env.sh`;
  wrappers in `~/.local/bin`); the local pacman sync db was stale, so the packages come from a freshly fetched
  core/extra index. apfs-fuse `66b86bd` built from source into the same sysroot (needs `-include cstdint` with
  current GCC). mksh not installed (optional). xtool and the Xcode `.xip`: at section 9.
  **A desktop freeze**: `cargo build --release` of `oneios-tool` with the target on the loop image over ntfs-3g
  stalled I/O until only the mouse moved (no OOM, no panic). Heavy builds on this PC: `CARGO_TARGET_DIR` (and other
  build trees) on `/tmp` (tmpfs, 20 GB), `nice -n 19 ionice -c3`, few jobs; the result copied back. Built that way:
  3 min 23 s, `-j4`.
- 2026-11-19: **image inputs, kernel and `kernel.kc` on the owner's PC.** `fetch.sh` of `ios-27.0.1-iPhone18,2`:
  13 members, CRC-32 ok, both AEA images decrypted (OS 10,359,930,880 bytes, SystemOS 6,157,238,272), 16 GB in
  `inputs/24A446/` (on the NTFS disk); about 12 MB/s from Apple's CDN. `kernel-inputs.sh`: kernelcache.macho
  78,135,296 bytes, sha256 `349c4692...0c57e3`, the pinned one. Kernel (`HOSTED_OUT=me XNU_FILESET_REFERENCE=1`,
  `JOBS=4`, stock clang **22.1.8**, build trees in tmpfs): `xnu.macho` 24,820,104 bytes, sha256 `a1e5ca1f8a57536d...`,
  in 348 s; `_oneios_drivers_start` calls `oneios_hid_input_start`. `kernel.kc`: 96,043,008 bytes, sha256
  `fd1ff268a23c7631...`, audit 2711 paired, 93 on the stub, 9 data unpaired, 0 failures.
  Four fixes were needed on a fresh Linux checkout (none showed on the PC of 2026-11-12, whose `external/` was full,
  had no `/usr/bin/plutil` and clang 23):
  `setup-toolchain.sh` takes `LIBDISPATCH_PREFIX` (a libdispatch outside the system's paths);
  `unixnu-layout.sh` fetches Droid-VM-UniXNU itself when it is missing (build-hosted runs it before `stage_sources`);
  `build-hosted.sh` always binds UniXNU's `plutil` shim (GNUstep's `/usr/bin/plutil`, Arch `gnustep-base`, fails on
  xnu's Kernel.framework Info.plist); `clang-wrapper` defines `__PTRAUTH_INTRINSICS__` for arm64e when the compiler
  does not (stock clang 22 predefines only `__PTRAUTH__`; xnu's EXTERNAL_HEADERS/ptrauth.h then used its no-PAC
  stubs: "statement expression not allowed at file scope" in sys/ucred.h). Not yet checked: that clang 22's PAC code
  matches clang 23's at run time (qemu boot, section 5).
  Also on this PC: the owner's `panino-focus` service capped the tmux job's scope at 5% CPU (fixed by the owner).
- 2026-11-19: runtime from the branch: `runtime/build.sh` (external/virglrenderer at `7e17c8e`; xnuhost has
  drm2kgsl), elf-check 6 dynamic and 9 static ELF files, 0 failures; `runtime/package.sh 0.1.9-dev`:
  `oneios-runtime-0.1.9-dev-arm64.tar.zst` 838,706 bytes, sha256 `cc521463...4fc9c9` (runtime only: the hosted
  kernel is not releasable). First image set (no helpers, no Metal plug-in): 21 files, 17,862,581,529 bytes, archive
  11,139,881,053 bytes, sha256 `066eb02c...0336d8`, verified. **The Metal plug-in**: the owner has no Apple ID for an
  Xcode `.xip`, and the plug-in needs the iPhoneOS SDK's headers (Metal, Foundation, IOSurface). It is built instead by
  `.github/workflows/metal-plugin.yml` (manual) on GitHub's `xcode-27` runner (Xcode 27.2): the SystemOS member and the
  iOS 27 stubs are made on the runner and deleted there; the only output, the bundle, gpuaddon and build record, is
  encrypted with the repository secret `METAL_ARTIFACT_KEY` (the key file stays on the owner's PC) and kept one day.
- 2026-11-19: **the Metal plug-in and the guest helpers, from GitHub's macOS runner** (`metal-plugin.yml`, run
  44462716584, third try: before it, `oneios-tool` did not build on macOS since `fab228e` (`sched_getscheduler`,
  fixed in `af5df8a`), and Mesa's meson wanted Python's `packaging` (`b6da3cc`)). Xcode 27.2, SDK iPhoneOS27.2, stubs
  from 24A446's SystemOS (4723 stubs, cache UUID `77BC8C63...`): `NativeAdrenoMetal` 18,894,656 bytes, CDHash
  `74d4b44d...`; `libvulkan_freedreno.dylib` 16,871,920 bytes, CDHash `eb4b82da...`; macho-check passed. Helpers
  (run 44469915441, `[helpers-only]`): oneios-init `66c1359c...`, bootstat, seed, mg, thermal. Both artifacts were
  deleted from GitHub after the download. The SystemOS step takes about 25 minutes on the runner, the plug-in about
  25. **Image set with both** (no dropbear, no camera plug-in): 22 files, 17,898,683,607 bytes, archive sha256
  `e535905f...26182fe`; the hook placed the plug-in in `/private/var/oneios/addons/Extensions` and oneios-init's
  services (bootstat, identity, thermal); both CDHashes are in `overlay/userland/owner-trust.json`.
  The local ld64 with libtapi (`ios-toolchain.sh --ld`) stopped at its libblocksruntime check (the libdispatch of
  the private sysroot is not in `ldconfig`): not needed while the runner builds the iOS code.
- 2026-11-19: **staged on the owner's phone** (USB, root by dfroot). 23 GB of the owner's videos moved to the PC
  first (sha256 checked on both sides before each delete): /data had 15 GB free, 36 GB after. `phone stage runtime`
  0.1.9-dev: ok. `phone stage image` could only *replace* files of a staged set (`not staged: image/app.img`): the
  first import was the OneOS app's. It now does that import itself when no set is staged (`image/` and
  `image.conf` absent), through `oneios_root.sh begin/receive/finish image` with manifest.json's sizes and SHA-256,
  removing each pushed copy once received (so /data needs room for the largest file, not the set twice). On the
  phone: 23 files received, `finish image 27.0.1`, 17,898,693,361 bytes; `image-check`: manifest.json and its 22
  files agree; `phone sums`: kernel.kc `fd1ff268a23c7631...` (the PC's); `phone status`: runtime 0.1.9-dev, image
  27.0.1 24A446, **`ready 1`**; the pa3q gate line still in the staged `oneios_switch.sh`. 20 GB free on /data.
  Not tested by tests/phone (its import checks need the fork checkout `app/`). On this PC two old tests fail
  for reasons outside this change: sparse.rs's holes test, and tests/phone's `touch status` (it expects
  English "No such file"; the PC's locale is Italian).
- 2026-11-19: **PC boot of the staged pair** (section 5; qemu-aarch64 11.1.3 `-cpu cortex-a710`, runtime 0.1.9-dev's
  xnuhost, the final image set and `kernel.kc` `fd1ff268...`, `--ram 4G --cpus 8 --display shm`, boot arguments of
  section 5 plus `oneios-gpu-metal=1 oneios-adreno-selftest=1`). `PAC yes`; at +8 s `droidvm-gpu: Metal accelerator
  stage 5 status 0` and the selftest's IOKit match (`AGXAccelerator`, `MetalPluginName NativeAdrenoMetal`) and user
  client open (status 0; no 3D device under qemu); the addon folder mounted over /System/Library/Extensions (stage 5
  status 0); backboardd's code signature and library validation of `NativeAdrenoMetal` and
  `libvulkan_freedreno.dylib`: error 0 (the runner-built plug-in is trusted); `stage=backboardd-up`; **first frame
  of the guest's own at +341 s** (the Apple logo, 1440x3120). Ran 19 minutes with SpringBoard, backboardd and the
  daemons up: 0 panics, 0 `sheng-exc`. So the clang 22 kernel boots with PAC under qemu; the phone's own parts
  (KGSL, panel, HALs, timing) are not shown by it.
- 2026-11-19: **session 1 on the phone** (USB, KernelSU root, battery 85% charging, 26 C; the staged hand-made switch
  script with the pa3q gate line (session 2 below), runtime 0.1.9-dev, image set 27.0.1 24A446; `oneios-tool phone switch` at 10:24:46). Way back armed on
  `/dev/input/event0` + `event4`; `/dev/watchdog` busy (softdog not written); zygote stopped, system_server gone,
  surfaceflinger kept; xnuhost at 10:25:08 (takeover, 4 GiB locked). Darwin kernel at +2.7 s, `BSD root: disk7s1` at
  +5 s, launchd at +20 s, backboardd opened the framebuffer at +31 s, **first frame with content at +33 s** (qemu:
  +341 s). Touch `sec_touchscreen` event8, backlight and touch-IC wake: samsung backends. Thermal bridge: 29 zones,
  nominal throughout. One `drm2kgsl: virgl_fence_set_fd: failed err=-16` (EBUSY) at +32 s. 0 panics.
  **Ended early by the coordinator at +75 s** with `phone return`: the CPU zones read 93-104 C (`cpu-1-1-1` 104.2 C,
  case 28 C) because that script's `clocks_tops` writes each policy's `scaling_min_freq` to `cpuinfo_max_freq` (3.53 / 4.47 GHz
  pinned, GPU 1200 MHz; the fork's writes `cpuinfo_min_freq`). Return **in place**, Enforcing, 42 s;
  One UI's clocks written back; 76 C a minute later. Logs: `logs/pa3q-s1/`. The owner: the phone was cool to the hand
  (the cpu-* zones are junction temperatures; the case read 28 C), so session 2 ran until the owner returned.
- 2026-11-19: **session 2 on the phone** (10:29:44 to the owner's Volume Up + Volume Down at 10:38:55; same stage as
  session 1). First frame with content at +24 s, `stage=backboardd-up` +18 s, SpringBoard busy at +74 s (122 % in
  `oneios-cpu`), then the home screen, widgets, Spotlight, Settings and Photos in use; touch works. The owner: it works,
  not smooth: stutters at times, smooth at others; iOS's "too hot" screen for a few seconds at a time, then usable
  again; the volume keys "maybe" did nothing in iOS (not yet read in the log). **The return by the keys worked**
  (`watcher: return asked (keys: Volume Up + Volume Down down for 3 s (polled))`), in place, Enforcing, 40 s. USB:
  `cable 0` at 10:31:32 (the phone off the cable; adb came back after the return). 0 panics.
  What the logs show (`logs/pa3q-s2/`):
  - **The staged session scripts were not the fork's.** The `oneios_switch.sh` staged on 2026-11-19 02:42 (and its
    `.orig-0.5.14`) is a hand-made variant from the owner's earlier iOS 16.5 trials (its boot screen says "Starting
    iOS 16.5"; with `stage_ios16.sh` and an `oneios_return.sh` of 2026-11-14): `scaling_governor=performance` and
    every policy's floor at its top, `vm.dirty_ratio=50`, `dirty_background_ratio=20` (not restored by the return:
    still 50/20 in One UI after it), `renice -20` of xnuhost, no cpuset step, the boot screen's hand-off without
    `first frame posted` (the boot screen ran under iOS's layer to the return), and a fixed command line: `--ram
    4096M --width 1080 --height 2340 --scale 3 --hz 120 --display shm --gpu 3d --set wifi=sim`, a keybagd text fixup
    that 24A446 does not match (not applied), and boot arguments with `ool_port_array_enforced=0`,
    `oneios-legacy-bootstrap-replies=1` and `oneios-ubt=SpringBoard,backboardd` (none of them in this repository).
    Copies: `logs/pa3q-scripts-before-s3/`.
  - **Heat and the "too hot" screen**: the SoC rule took one reading of the hottest junction sensor (`cpu-1-1-1`,
    swinging 68-105 C between polls with the floors pinned), and its S24 thresholds (95/105 C) sit at this kernel's
    first passive trip (105 C): 20 level changes in 8 minutes, 5 into TRAPPING (iOS's "too hot" screen), while the case
    read 37-46 C and the battery 25-40 C. The thermal bridge's fix: `runtime/bridges/thermal/README.md`, "The SoC rule
    on the phone's own trips".
  - **Frames**: up to 1202 flushes in 10 s (120 fps) while something moves, GPU time 0.7-5.4 ms a frame (`gpu: N s
    level`), submit-to-retired mean 2.1-2.6 ms, max 11-23 ms; the scanout copy mean 0.7-1.8 ms, max up to 20.6 ms;
    blob creations up to 13-20 ms on the fast lane's side of the device mutex. 1-15 frames over budget per 10 s.
  - **Memory**: the guest's 4096 MiB (xnuhost's own fit said 7063 MiB) ran with 27-43 MiB free and a compressor of up
    to 606 MiB (1 jetsam kill); host page tables up to 655 MiB, 16 % of the guest, where `ram=auto` counts 8 %.
  - First-boot work: `lsd`, `iconservicesagent`, widgets, `IntelligencePlatform` (80-99 % for a minute), Spotlight.
- 2026-11-19: **staged for session 3** (fixes of the above, host tests only, nothing run on the phone yet):
  - `oneios-tool app stage-scripts`: the fork's `oneios_switch.sh` `564270b5...`, `oneios_return.sh` `94c19e0e...`,
    `bin/oneios_root.sh` `7c118c76...` (YumikoHayamato/OneOS `d928cdc`), replacing the hand-made trio. The installed app
    is still 0.5.14: start sessions with `oneios-tool phone switch`, not with the app (its stage would send its own
    scripts again).
  - The image set's `overlay/userland/xnuhost.conf` (`profiles/pa3q/`, staged with `oneios-tool phone manifest
    profiles/pa3q --stage`; `20265acd...`): `width=1080 height=2340 hz=120 wifi=sim ram-pagetable-percent=16` beside
    roottweak's `scale=3`, what the hand-made command line kept that the fork's lacks, and the page tables as measured.
    `ram=auto` then gives (MemAvailable - 768 - 256) / 1.16: about 6.4 GiB for 8.65 GiB at the start.
  - Runtime 0.1.10-dev (`runtime/package.sh 0.1.10-dev`, `11e5f688...`, 777,384 bytes): the thermal bridge's SoC rule
    on the kernel's trips and on 15 s of heat; the fast lane's thread and the display helper's copy loop at
    uclamp.min 1024 (`docs/gpu.md`, `runtime/display/README.md`); xnuhost takes `wifi=` and `bluetooth=` from the image
    set's `xnuhost.conf` (`docs/booter.md`). Host tests: thermal 145, bridges 362, display 133 + 10, xnuhost 14,079
    checks with 1 failure that is this PC's (`test_ramfix`'s `memfd_held(fd) == 3 * 4096`: the PC's shmem THP is
    `within_size`, so the test memfd gets 2 MiB pages).
  - What session 3 should show: `thermal: the SoC rule: heavy from 115.0 C, trapping from 120.0 C`, `memory: the
    guest gets ... (ram=auto)`, `clocks: .../policy0/scaling_min_freq ... -> 384000` and `policy6/... -> 1017600` (the hardware's floors) in `switch.log`, `gpu: the
    queue's thread ... at uclamp.min 1024`, `oneios-display: the copy loop at uclamp.min 1024`, the boot screen ended
    at `first frame posted`, `wifi: sim`.
- 2026-11-19: **session 3 on the phone** (battery 89 %, 30 C; `logs/pa3q-s3-failed-remount/`, `logs/pa3q-s3/`).
  - 11:29:30, first try: the fork's inner shell stopped at `making /data/oneios_switch.sh read-only` (`mount: can't
    find /data/oneios_switch.sh in /proc/mounts`, status 70); the switch returned to One UI in place (`switch-failed`).
    KernelSU's busybox 1.36.1.1 (topjohnwu) finds no bound file or folder in `/proc/mounts` when a remount names the
    mount point alone; the hand-made script had hidden this with `|| true`. Tried in a mount namespace of its own on the
    phone: `remount,bind,ro SOURCE TARGET` works for a file and a folder (writes refused), `bind,ro` in one step leaves
    the file writable, toybox's mount refuses. Fixed in the fork (YumikoHayamato/OneOS `oneios-s25-ultra` `d975ec1`:
    source and target, and `nosuid,nodev` given again, which a bind remount clears); staged (`b82d8943...`).
  - 11:33:59, second try: the session ran from 11:34:23 to the coordinator's `phone return` at 11:41:51 (the owner:
    enough). Seen on the phone: `oneios inner: cpuset / (cpus 0-7)`; the floors at the hardware's
    (`policy0/scaling_min_freq 556800 -> 384000`, policy6 1017600), the caps at the top; `memory: fixed RAM: ...
    prefaulted 6400 of 6400 MiB, locked 6400 MiB` (`ram=auto` with `ram-pagetable-percent=16`); `thermal: the SoC rule:
    heavy from 115.0 C, trapping from 120.0 C, ... the kernel's trips: first passive 105.0 C, hot 125.0 C (the median of
    25 SoC zones)`; `wifi: sim` from the image set; `oneios-display: the copy loop at uclamp.min 1024 while it runs`;
    the first frame with content at +25 s. **But runtime 0.1.10-dev was built 2D-only** (`gpu: 2D only: this build has
    no drm2kgsl (external/virglrenderer was absent ...)`): `external` is a link into a tmpfs
    (`/tmp/claude-1000/oneios-ram/external`) that the PC's reboot at 10:07 had emptied, and the build said nothing. So
    iOS drew on the CPU: host CPU 166-686 % of 800, the case 40.5-45.1 C, 5 level changes in 7 minutes, all by the case
    (light, moderate, heavy; none by the SoC, whose sustained reading stayed under 104 C, and no TRAPPING), and 3 user
    exceptions (`sheng-exc` in `push_surface`; none in session 2). Not a measure of the frame rate or of the heat with
    the GPU. Return in place, Enforcing, 44 s, by the fork's `oneios_return.sh` (`shmem_enabled back: never`). After
    it the fork's preflight said `battery too warm (42 C)`: the next switch waits for the battery.
  - Fixed: `runtime/build.sh` refuses a stage without `external/virglrenderer` (`docs/building.md` section 4);
    virglrenderer fetched again at `7e17c8e`; runtime 0.1.11-dev (`a6ecf94e...`, 840,592 bytes, with drm2kgsl;
    `runtime/xnuhost/build/android` removed first) staged. Session 4 is the first with the fork's scripts and the GPU.
- 2026-11-19: **session 4 on the phone** (11:54:23 to the coordinator's `phone return` at 12:01:13 at the owner's word;
  battery 28.0 C and the case 28.9 C at the start, the phone cooled off the cable; `logs/pa3q-s4/`). The fork's scripts,
  runtime 0.1.11-dev with drm2kgsl (`gpu: the queue's thread ... at uclamp.min 1024 while it runs`), 6400 MiB locked,
  the SoC rule at 115/120 C, `wifi: sim`; first frame with content at +25 s; 0 `sheng-exc`, 0 panics. Frames: up to 851
  flushes in 10 s; the scanout copy mean 1.2 ms, **max 2.7-3.5 ms** (session 2: up to 20.6 ms); 511-587 render-server
  frames per 10 s, GPU time 2.6-4.2 ms, submit-to-retired mean 3.5-5.1 ms, max 11 ms, 2 over budget per 10 s. Heat: the
  levels by the case only (light at 39.7 C, moderate at 42.1 C within 1.5 minutes), host CPU 260-708 % of 800 (sys
  135-397 %) in the first minutes, the battery 28.5 -> 37.2 C in 2.5 minutes, a net 1.2-2.5 A out of it on USB. Return in
  place, 44 s. **The owner: stable, but slow to start and slow: "an iPhone 11" in latency and in the waits when an app
  opens.** What the trips cost (`perf:` lines, means; p99; max): a system call 45 us (p50 6 us; 524 us; 577 ms), a page
  fault 6-12 us (81-131 us; 932 ms), an interrupt 22-50 us (max 545 ms), a batch of maps by release 24-27 us, trap to
  vCPU 6.8 us and back to the task 8.2 us. And **the disks**: `virtio_blk.c` answers each request inside the vCPU's
  write of QueueNotify (`virtio.c` `notify`, under the device's lock), one queue per disk, so a vCPU waits for every
  `pread` and, at a chunk's first read, for the read and SHA-256 of its whole 1 MiB (`xh_verify_range`; ARMv8 SHA
  instructions), and the disk serves one request at a time; the GPU and input devices have worker threads. Not yet
  measured: how much of an app's start that is (no per-disk statistics line yet).
- 2026-11-19: **other projects** (owner's question: who runs iOS 27 smoothly?). None found on an Android phone. HakiOS
  (`oosexgang/hakios-xnu-kernel`, MIT user space): XNU natively from its own UEFI (Mu-Silicium), loaded from RAM by
  fastboot (an unlocked bootloader; not possible on this phone: Samsung has no fastboot and this one's bootloader is
  locked, `verifiedbootstate green`), iOS 16.5's user space on the POCO X5 Pro (SM7325, Adreno 642L): page swipes
  100-104 fps at 1125x2436 and 107-118 at 750x1624 with its own Metal driver (`hakios-metal`: Metal to the Adreno's
  command packets over msm-DRM-style requests to its kernel driver, AIR to NIR to Mesa's ir3; A6xx only), against
  50-57 fps for UniXNU's Metal on Vulkan (Turnip, the kind of path NativeAdrenoMetal is); 120 not reached. Scion-Project
  (a HakiOS fork): iOS 27 on the Galaxy S9, its README's own states "built, not yet run" and "iOS 27 can hang". UniXNU
  ran iOS 26 natively on a Surface Duo (UEFI). vphone-cli: iOS 27 as a VM on Apple Silicon Macs only.
- 2026-11-19: **the boot's 11 s and the disks** (session 4's log): from launchd's first line to `dyld cache mapped
  system-wide` 11.3 s with nothing else logged; that is dyld registering the shared cache's files, which the guest reads
  and hashes whole (kernel patch 0014, all vCPUs taking megabytes in rising order: 5.8 GiB), while its disk driver
  (`OneiOSHostedDisk.cpp`) sends one request at a time per disk and xnuhost answers inside the notify, checking each
  chunk's 1 MiB and SHA-256 at its first read. The owner's go for an asynchronous disk; the full one needs the guest's
  driver changed too (interrupt completion, several requests, no bounce copy: a kernel build, whose tree and toolchain
  are gone with the tmpfs). First, host only (runtime 0.1.12-dev): **the read-ahead** (`imageset.c` `xh_verify_ahead`,
  `disk-ahead=8`: three threads check the next 8 chunks after a sequential read, the guest's read of them is a page-cache
  copy; a read waits for a chunk being checked instead of checking it twice; a failed chunk stays unchecked) and **a
  statistics line per disk** (`disk ROLE: 10 s: reads ..., in requests T ms ...; chunks checked: on the reads, ahead,
  waits`). Host tests: `test_imageset` 51 checks (20 runs, and ThreadSanitizer 3 runs: clean), `test_xnuhost` 14,079 with
  the PC's one THP failure, display mode, gpu policy 166, drm2kgsl 17, gpu leaks 956: 0 failed. Not yet run on the phone:
  session 5 says what the boot's gap and an app's start cost in each disk, and whether the guest's driver is next.
- 2026-11-19: **session 5 on the phone** (12:29:26 to the coordinator's `phone return` at 12:37:08; battery 30.6 C, the
  case 32.2 C at the start; runtime 0.1.12-dev, `ram=auto` 6144 MiB; `logs/pa3q-s5/`, with `host-memory-at-end.txt`).
  - The boot: launchd to `stage=bootstat-up` 12.1 s (session 4: 11.5 s), so the read-ahead did not shorten it. The disk
    lines say why: `disk systemos: 10 s: reads 7750 (3771 MiB) ... in requests 3273 ms (mean 422 us) ...; chunks checked:
    2255 on the reads (2372 ms), 1570 ahead (4339 ms on the workers), 86 waits`, then `reads 77712 (1928 MiB) ... in
    requests 2418 ms`: the disk was busy a third of the time, and the guest used 338 % of 800 (`kernel_task` 132 %, the
    parallel code hash). What holds the hashers back is the guest's driver: it keeps its lock across the host's answer
    *and* the copy out of its 1 MiB bounce buffer, so the hashing threads queue behind it. The fix is in
    `OneiOSHostedDisk.cpp` (several requests in flight, the client's pages in the descriptors, completion by interrupt),
    with a kernel build.
  - After the boot, while the owner opened apps, **the host swapped at 82-122 MB/s in and 89-129 MB/s out** (host CPU
    552-722 %, sys 347-410 %), and the case went 32 -> 46 C in two minutes: `thermal: trapping (was heavy; by case): case
    46.0 C`, iOS's "too hot" screen, by the case this time. The host's memory at the end: MemAvailable 950 MiB, MemFree
    460 MiB, Mlocked 6.3 GiB, AnonPages 2.5 MiB (the rest of the host's anonymous memory in swap: 820 MiB),
    PageTables 610 MiB, SUnreclaim 1.2 GiB: `vm_area_struct` 2,811,940 (549 MiB), `vma_lock` 176 MiB, `maple_node` 85 MiB
    (the iOS tasks' mappings: 2.8 million; xnuhost's own process 71,357), vmalloc 346 MiB; swappiness 130. In swap:
    the camera provider HAL 186 MiB, the display helper (`app_process OneiOSDisplay`) 44 MiB, xnuhost 30 MiB, adbd,
    audioserver, surfaceflinger 20-25 MiB each. Session 4 sat at the same edge (MemAvailable 1 GiB) without the storm.
  - Fixes for session 6: the fork's session runs with `vm.swappiness` 20 and the return writes One UI's back
    (YumikoHayamato/OneOS `81438a5`; staged: `oneios_switch.sh` `fab19cf9...`, `oneios_return.sh` `01b328c1...`), and
    `profiles/pa3q`'s `ram-pagetable-percent=28` (the page tables and the mappings' slab, 23 % of the guest measured):
    5632 MiB for MemAvailable 8.3-8.6 GiB, 512-768 MiB more for the host. Staged; not yet run.
- 2026-11-19: **session 6 on the phone** (12:46:00 to the coordinator's `phone return` at 12:54:16; battery and case
  33.5 C at the start; `logs/pa3q-s6/`). `swappiness: 130 -> 20` in `switch.log` and `20 -> 130 (One UI's)` in
  `return.log` (read back: 130); `ram=auto` 5888 MiB (MemAvailable 8611 MiB). **No swap storm**: host MemAvailable
  2488 -> 1361 MiB in the first 90 s (session 5: 1908 -> 1067), swap 0-0.25 MB/s (session 5: up to 122 MB/s each way).
  Still hot in the first minutes: the case 33.5 -> 44.0 C in 75 s (heavy, by the case), host CPU 419-698 % (sys
  231-389 %): for about 100 s after the first frame iOS keeps all eight vCPUs busy (`oneios-cpu` all 764-799 %:
  iconservicesagent 112-122 %, SpringBoard 39-89 %, backboardd 22-82 %, GeneralMapsWidget 108 % with geod 90 %,
  Spotlight, chronod, PerfPowerServices, biomed), then 157 %. The owner: a slight improvement, still too much lag.
  Touch to the guest's next frame (`input: touch: ... the guest's next frame X ms after the touch-down`, 22 touches):
  min 27, median 53, p90 95, max 768 ms; the touch reaches iOS's event driver 0.2-0.5 ms after the scan (max 3 ms),
  so the time is iOS's own work under the hosted costs. The 768 ms one: PosterBoard bringing up Metal (the plug-in's
  and freedreno's library validation, the AGX user client, two GPU contexts) for about 800 ms; meanwhile 235 GPU blobs
  in 10 s at a mean of 981 us each, and the render server's submit calls up to 3.8 ms (they share the device's mutex
  with the slow lane's blob creations).
- 2026-11-19: **two quick fixes for the lag of the first minutes, after session 6** (host tests only; not yet run on
  the phone).
  - **The GPU's two lanes, fixed** (`runtime/xnuhost/src/virtio_gpu.c`, `docs/gpu.md` "The render server's lane
    first"): the queue's thread took the device's mutex just to choose a command's lane, and the slow lane, with a queue
    of an app's blobs, locked that plain mutex again before the woken queue thread ran, so the render server's next
    command could wait for every queued blob, not one (session 6: 311-2458 blobs per 10 s, up to 13-27 ms each). Now
    the lane is chosen under its own lock and the slow lane gives way while the render server's lane waits: at most the
    blob in progress. The host test shows it (1 of 3 queued blobs before the submission; 3 of 3 without the giving way).
    On the phone, the new line: `gpu: 10 s: the render server's lane: N commands, K waited over 1 ms for an app's
    command (longest X us); the app lane gave way Y times`, with the frames over budget of the `gpu:` level lines and
    the owner's feel when apps open. Not changed: the cost of a blob, the time an app takes to open.
  - **Background jobs off in the store already on the phone** (`profiles/pa3q/overlay/userland/xnuhost.conf`,
    xnuhost's new `boot-args-add`, `docs/userland.md` section 4): `oneios-disable-jobs=` axassetsd, searchd,
    spotlight.IndexAgent, PerfPowerServices through hosted 0053 (one `oneios-jobs:` line each at boot). Proof: the
    `oneios-cpu` lines of the first 100 s without searchd and PerfPowerServices. The Maps widget's chain (the largest
    optional one in session 6) needs the owner to remove the widget. The runtime has to be staged before this profile:
    an older xnuhost refuses `boot-args-add` and does not start.
- 2026-11-19: **session 7 on the phone** (runtime 0.1.13-dev with the two fixes and the profile; xnuhost 640.8 s to its
  `exit 0` at 1205.0 s; `logs/pa3q-s7/`, local). The jobs: `oneios-jobs: com.apple.searchd: disabled in disabled.plist
  (it was not)`, the same for spotlight.IndexAgent (axassetsd was already). The GPU's lanes: 51 reports, the render
  server's longest wait for an app's command 1 us in most and 3.6-8.9 ms in the worst five (session 6: blobs of 13-27
  ms each queued ahead of it), the app lane gave way 84 times. Thermal: `trapping (was heavy; by case): case 46.0 C,
  SoC 104.6 C` 107 s after the start, and with it iOS's "too hot" state (about 10 fps). The owner, without the Maps
  widget: more responsive, no heavy lag, 30-40 fps steady, 20 while an app opens, and slower and slower as apps are
  opened and closed again until motion stops. What is left of the hosted lag is the trap path (more than half of the
  CPU the host spends is Linux's), the thermal trap and backboardd's GPU buffers (about 9300 small mappable ones of its
  own task, about 1.4 GiB, growing with each app open): the owner chose the native path (`docs/native.md`).
- 2026-11-19 and 20: **the native path on the PC** (`docs/native.md`): XNU alone at EL1 under QEMU's virt board. The
  plain kernel boots to the root-device wait (19th). The native kernel (`kernel/build-native.sh`: the hosted kernel's OS
  layer without its host, hosted 0076; its disk driver checks every chunk it reads against the image set's hash lists;
  `xnuloader-qemu --image`) mounts the image set's root, formats and fills the Data store from the seed and runs
  **launchd** and its boot tasks (20th). On the way: the prelink no longer applies the hosted rewrites to a native
  kernel's kexts, sheng's IPI recovery and the one-kind-of-CPU topology as under xnuhost, QEMU's CPU a Cortex-A710.
- 2026-11-20: **the native kernel's display and touch on the PC** (`docs/native.md`, "The display on the bench"): the
  hosted kernel's display, GPU transport (its control queue now as long as the device takes) and touch archives on
  QEMU's virtio-gpu, tablet and keyboard. backboardd draws the Apple logo, then SpringBoard **iOS 27's lock screen**,
  with the CPU (no 3D); a swipe through VNC reaches backboardd and the lock screen shows "Swipe up to open". Under QEMU
  an hour is minutes of a phone: the unlock did not finish in the run. Found with iOS's own log read from the store:
  rapportd spinning without a network (1.5 million log lines in 14 minutes), turned off on the bench.
- 2026-11-20 to 23: **a VM under the S25's Gunyah** (`docs/native.md`, "A VM under Gunyah"; read only first: `phone
  hw`, section 0b). AVF's crosvm runs our payload as a protected VM at the core's own speed (`phone vm-probe`, runs 1
  and 2); in 4 KiB pages its RAM ends near 0.5 GB (Gunyah's 18 MiB heap; runs 3 to 8). The owner's idea, Droid-VM's
  gh_hugepage_reserve, loaded after the temporary root and its pool grown with One UI's framework stopped about 37 s
  (`phone hugepool`, no restart of the phone; one panic first, ours: the insmod line lacked `pageblock_order_val`),
  gives **a 3 GiB VM, every page written and read back** (run 10). Step 1 of the road to an iOS guest there: **the
  native kernel boots in that VM** (`xnuloader-crosvm`, `phone vm-boot`, kernel 0077's ns16550a console; boot 1, 2
  GiB, 1 vCPU) to the root device's wait, and `crosvm stop` ends it with One UI on. Step 2, the disks (virtio over
  PCI, every buffer in the restricted DMA pool; boot 2): launchd, the 60 boot tasks, SpringBoard in about 30 s on one
  vCPU. Step 3, the screen (the boot video and a frame count in the pool, read by `displayctl pool` out of crosvm's
  own mapping of it; boot 3): **iOS's lock screen on the S25's panel**, SpringBoard up for the whole run. Step 4, the
  touch (crosvm's multi-touch device fed by `displayctl touch`, which takes the touchscreen; the kernel's virtio-input on
  PCI with its queues in the pool; boot 4b): a test swipe opened **iOS's Home screen**, then the owner's finger (Settings, Control
  Center; boot 5). Step 5, four vCPUs (ArmCpu's PSCI CPU_ON by HVC; boot 6): the Home screen and the apps in about two
  minutes. Step 6 (boots 7 and 8): the screen's path at about 5% of a core (write-only buffers in the helper, 1080x2340)
  and the real date (crosvm's PL030, patch 0078). Step 7 (boots 8 and 9): Android's GPU writes into the VM's shared pool
  (`runtime/gpuprobe`: kgsl's MAP_USER_MEM on crosvm's mapping of it), and a pool of 128 MiB works. Next: the GPU's
  device for the VM (drm2kgsl) with its buffers in the pool.
