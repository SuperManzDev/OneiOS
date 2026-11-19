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

