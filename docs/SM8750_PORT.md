# SM8750 port — Galaxy S25 Ultra (pa3q)

Status (2026-11-19): **planning; nothing has run on an S25 Ultra.** This branch (`s25-ultra-sm8750`) starts from the
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
  Its input parser was checked on the PC against an S26-shaped `getevent -pl` sample. Not run on a phone.
