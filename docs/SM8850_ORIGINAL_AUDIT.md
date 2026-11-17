# SM8850 audit against the original S24 Ultra OneiOS switch/return contract

Audit started 2026-11-17. **No new device claims without a local report**.
This is a comparison and passive test harness, not authorization to boot
an iOS guest on Samsung SM-S948B / m3q / SM8850.

## Ground truth and references

- Local feature OneiOS: `YMGPwcca/OneiOS` branch `feat/oneios-sm8850-m3q`, starting audit from `bec7efec`.
- Its OneOS fork: `YMGPwcca/OneOS` branch `feat/oneios-sm8850-m3q` at `b81a6032`.
- Original S24 Ultra source: `akshaynexus/OneiOS:main` at `7b810126`, and
  `akshaynexus/OneOS:oneios` at `bdf3db05`.
- The existing submodule pin in the feature OneiOS tree is `820253bf`;
  that is **not** the OneOS feature-branch head `b81a6032`.
- The device reports OneOS APK versionName 0.5.16, versionCode 145,
  KernelSU root, and no `/data/local/tmp/oneios` stage. It remains
  in One UI with system_server running.
- Prior SM8850 results (GPU submissions, CPU, native vibrator presets,
  S Pen input and passive display probes) are in `docs/SM8850_PORT.md`.
  A prior test is not repeated merely to generate a new PASS.

## One command, passive only

```sh
cd ~/projects/OneiOS
git pull --ff-only
scripts/phone/audit-s24-preflight-m3q.sh
# To read all findings, use the path printed by the command:
# cat ~/oneios-m3q-ci/audits/<TIMESTAMP>/checks.tsv
```

With multiple ADB devices, set `ANDROID_SERIAL`. With a custom local
destination, set `ONEIOS_AUDIT_OUT`. The script refuses any connected
device whose codename is not exactly `m3q`. It uses `adb shell su` to
**read** sysfs, system properties, service registrations, input capabilities,
filesystem metadata and Android power state, and writes reports **only to
CachyOS**. It never writes to the phone, opens a test session, calls the
return script, stages anything, or plays any haptic. Treat the collected
report as local diagnostic data, not a file for a public GitHub issue.

Interpretations:
- `PASS` means this *specific, read-only predicate* passed.
- `WARN` is a deviation or a limited observation; inspect manually.
- `FAIL` is a baseline requirement not met by this snapshot.
- `BLOCKED` means cannot verify passively or without a staged image.
- `INFO` provides context, and proves nothing beyond the printed value.

A green host CI and even zero `FAIL` do **not** mean an S26 session is
safe. The strict S24 model gate is expected to be `BLOCKED`.

## Original S24 switch preflight and SM8850 coverage

This matrix is derived from
`YMGPwcca/OneOS:switcher/assets/oneios_switch.sh` at `b81a6032`
(lines 325-420 and 980-1084) and the `oneios_root.sh` stage verifier.

| Original S24 condition | Passive audit | What remains |
| --- | --- | --- |
| UID root, SHA-256 utility, SELinux | Read UID/commands/SELinux | Compare actual policy AVCs only if needed |
| Exact model SM-S928*, e3q, SM8650 | Record exact SM-S948B/m3q/SM8850; old allowlist **BLOCKED** | Never blindly bypass |
| Stage root ownership, no symlinks; pinned switch/return | Stage inventory | No OneiOS stage exists yet |
| Runtime ABI 1, non-placeholder, ARM64 ELF; full SHA list | Files and signatures when staged | Verify real runtime then; no stage now |
| Image format, manifest, image hashes, changed-file IDs | Image inventory | Kernelcache, iOS image and manifest absent |
| KernelSU/AP/Magisk BusyBox and nine specific applets; resetprop | Query paths and each applet | Need an actually executed rollback for validation |
| Init mount namespace / nsenter | Compare namespace IDs | Runner may re-enter via nsenter |
| Fully booted One UI, zygote, system_server, surfaceflinger | Read props/PIDs | Restart behavior unproven |
| No leftover OneOS/OneiOS session, stage mounts, leaked cgroup | Read current state/mounts/cgroup | Check again immediately before any switch |
| Both Volume Up/Down input capabilities | `getevent -pl` inventory | **3-second chord and key reader are not tested** |
| Battery >=30% or charging, <42 C | Read capacity/status/temp | Dynamic conditions, re-check before testing |
| At least 2 GiB free on /data | `stat -f` | A real image set needs much more |
| Writable wakelock node | Check permissions only | Actual acquisition/release not tested |
| Experimental acknowledgement | Not auto-set | Owner consent during a controlled session |

The original script explicitly stops One UI's zygote/system_server after
arming return workers; it is not a harmless diagnostic. Never run
`oneios_switch.sh --check` directly as a substitute for this audit when the
stage is absent, and never call it in switch mode on an unvalidated phone.

## Original S24 in-place return: Gate A/B/C

Source: `YMGPwcca/OneOS:switcher/assets/oneios_return.sh` at
`b81a6032` (specifically its Gate A/B/C and failsafe).

| Return mechanism | Read-only compatibility evidence | Still unverified |
| --- | --- | --- |
| Session teardown, cgroup kill, mount cleanup | Current mount/cgroup inventory | Actual teardown, D-state task evacuation |
| Gate A: same boot, no stage mounts, unchanged SELinux | boot ID only within current session; SELinux reading | Before/after equality under a session |
| Gate A: vold/keystore2/servicemanager stable | Read service states and PIDs | Uninterrupted PIDs under takeover |
| Gate A: no new mount on One UI partitions | Current mounts snapshot | Delta across a live session |
| Gate B: composer discovery and service registration | Query vendor composer and AIDL/HIDL | **Do not restart composer during audit** |
| Gate B: restarted surfaceflinger, SurfaceFlinger binder | Read process and binder | Restart without black screen |
| Gate B: audio policy, media, camera, wificond, netd | Read init service states and binder presence | Restart behavior |
| Gate B: brightness and CPU/GPU clock restoration | Read original values and nodes | Actual write/restore of each |
| Gate B: boot-loop counters restored via resetprop -n | Verify resetprop exists | Actual counter restoration |
| Gate B: USB gadget/adbd restored to baseline | Read USB props/UDC current state | USB continuity across takeover/return |
| Gate C: zygote start, system_server and user 0 unlock | Current running states; may inspect on demand | 60s+180s+20s recovery timing |
| failsafe reboot and Volume chord | Present command paths, key capability | Actual fallback is **not** triggered |
| Upstream shmem advise/restore | Read `shmem_enabled` options and current mode | Future write, round-trip restore and recovery gate |

For SM8850, the camera, display composer, watchdog, audio and thermal
implementations may differ from S24. A passive report can catch missing
services and unsupported device paths, **not certify a full restore**.
The S24 return script is designed to reboot on failed Gate A/B/C; never
assume it will return in place on an untested One UI 9.0 build.

## The original OneOS app's 14 hardware checks

Source: `OneiosChecks.kt` and `oneios_root.sh check_cmd()` on the S24
fork. The audit explicitly inventories all **14** and checks which tool
binaries are staged. It does **not** automatically invoke any active test.

| Check ID | Original test | SM8850 audit policy |
| --- | --- | --- |
| cpu | `bridgectl cpu` | Already measured in `SM8850_PORT.md`, not repeated |
| gpu | `gpuctl` nodes/routes/probe/submit | Prior device GPU PASS, not repeated |
| display | `displayctl test` overlay | Active screen test; separate consent |
| haptic | `bridgectl haptic 200 1000` | Active buzz, old evdev may fail; native HAL independently tested |
| haptic-pattern | three taps then ramp | Active, no unsolicited playback |
| audio | 440 Hz tone via audio helper | Active output; consent and volume control |
| bt | Bluetooth controller test | Requires Bluetooth off; **do not steal live controller** |
| brightness | `brightctl info` | Passive candidate, requires tool |
| levels | change panel brightness then restore | Active sysfs writes, separate controlled test |
| als | read 10s ambient sensor | May enable sensor node; separate permission |
| camera-list | NDK camera enumeration | Passive candidate, requires tool |
| camera | NDK wide camera capture | Active camera acquisition, picture stored on phone |
| camera-hal-list | experimental direct HAL enumerate | Isolated experiment; not part of Run all |
| camera-hal | experimental direct HAL capture | High-risk active vendor HAL use; defer |

The documented native HEAVY_CLICK and `haptic-native-inspect` were
already tested. Neither substitutes for a guest-originated impact.

## Upstream drift as of 2026-11-17

Verified from actual GitHub compare history:

| Upstream | Our base | New head | Missing commits |
| --- | --- | --- | ---: |
| `akshaynexus/OneiOS:main` | `9f4459b4` | `7b810126` | 25 |
| `akshaynexus/OneOS:oneios` | `c27dc02c` | `bdf3db05` | 14 |

**Candidate syncs, in safety order:**

1. **High:** OneOS `2f7516fa` and `bdf3db05`: save/restore
   `shmem_enabled` (including exact bare mode), test restoration. Adapt
   only after passive SM8850 shmem support is confirmed. Keep strict
   SM8850 switch gate until recovery is actually verified.
2. **High for later guest boot:** OneiOS `432ffd9f` / `7b810126`:
   hosted `0080-shared-region-protect.patch`, shared region / pmap
   corrections. Requires kernel/patch-series validation and prelink,
   not only a runtime package build.
3. **Medium:** OneiOS `da39094f`: include Android `oneios-tool` in
   the runtime package; review our standalone CI and package tests so
   it remains local-first.
4. **Medium:** OneiOS `fc46ae77` (uclamp ceiling removal) and
   `hostcall.h`/task changes; relevant to SM8850 6E/2P topology.
5. **Optional for image setup:** OneOS `bb1ef384`, OneiOS `1ef92a8a`:
   `oneios-tool serve`, resumed/network-aware firmware download and
   import; exercise the updated protocol under test before syncing.
6. **Later:** OneOS `7447a9df` dynamic versionCode; compare with
   installed 0.5.16 / versionCode 145 and preserve signature/data.
7. **Do NOT cherry-pick unmodified:** OneOS `8b27d358`/
   `e5004eb0`: changing S24-only refusal to a warning without
   validated S26 recovery. Also do not replace our m3q `.gitmodules`
   branch with upstream's `app/` pin or restore GitHub Actions builds.

The source trees have diverged; these are **not** safe blind fast-forwards.
First collect the read-only report, then make a separate sync branch or
reviewed selective patches; run `scripts/ci-local.sh all` on CachyOS
for every code batch. No automatic phone stage/switch in CI.

## Stop conditions

- Any missing required root/busybox/resetprop/zygote/battery/wakelock condition.
- Incorrect or unknown composer / critical One UI services.
- No verified image/kernelcache specifically prepared for the device.
- No proven S26-specific return path, including watchdog, USB and keys.
- No local reproduction of upstream changes with SM8850 test coverage.

**A PASS inventory is not a PASS boot.** The test must stop before port or
switch operations whenever any condition remains unverified.

## Actual first-pass S26 Ultra results (owner's passive report)

The owner ran `scripts/phone/audit-s24-preflight-m3q.sh` on the rooted
Samsung SM-S948B (m3q/SM8850, Android 17 SDK 37, Linux 6.12.69) while
One UI was running. The returned breakdown:

| Verdict | Count | Meaning |
| --- | ---: | --- |
| PASS | **71** | Specific passive prerequisites observed |
| WARN | **1** | `init.svc.hwservicemanager=stopped` |
| FAIL | **0** | No failed *measured* passive predicates |
| BLOCKED | **24** | Untested active/rollback tasks or absent OneiOS stage |
| INFO | **36** | Context only |

The return prerequisites observable on an idle One UI passed:
KernelSU root and its BusyBox applets/resetprop, SELinux Enforcing,
init namespace, zygote/system_server/surfaceflinger, critical services,
Binder `SurfaceFlinger` / audio / vold / installd, composer3 AIDL and
the Samsung vendor composer, volume-key capability, battery at 96%,
28.9 C, free storage over 2 GiB, wakelock and cgroup permissions.

**Findings requiring further inspection:**

- `hwservicemanager` is **stopped**. AIDL composer and the required Binder
  services are registered, so this alone does *not* establish an error.
  Inspect HIDL clients (if any) and this vendor build's service topology;
  do not start it manually.
- `/sys/class/backlight/panel` reports 0/0 while
  `panel0-backlight` reports 0/12750. These are passive sysfs values
  only; determine which path is the actual controller and how One UI
  represents current brightness before any sysfs writes.
- `sys.usb.config=adb`, controller `a600000.dwc3` reports
  `not attached`. These readings are consistent with a wireless-only
  ADB connection. A real USB cable and a later controlled session are
  needed for continuity testing.
- `shmem_enabled` lists `advise` and currently selects `never`.
  This meets the *capability* prerequisite for upstream's shmem patch,
  but does **not** prove a mode change/restore round-trip.
- Hardware inventory shows standalone diagnostic `bridgectl`,
  `displayctl`, `brightctl` but no staged `gpuctl`, `camctl`
  or `oneios-audio`. No OneiOS stage exists yet, so absence of staged
  binaries is expected and does not mean they are missing from the
  built archive. A host-side archive inventory is needed.
- The original `SM-S928*/e3q/SM8650` switch guard is **BLOCKED**,
  correctly, on `SM-S948B/m3q/SM8850`; no code should bypass it.
  OneOS APK 0.5.16 (versionCode 145) being installed is not proof of
  staging or switch support.

**Important upstream code-review finding:** at OneOS upstream
`bdf3db05`, `oneios_switch.sh` changes `shmem_enabled` to `advise`
while One UI is still up, but `oneios_return.sh` has an early-return
branch for a session that never stopped One UI *before* its shmem restore
block in Gate A. Consequently, a canceled/failed session before zygote
stop may leave the mode changed. This is a source-level potential defect,
not a confirmed S26 incident. Do not copy that implementation unchanged.
The eventual patch needs a fake-phone regression proving both the
early-return and full Gate A paths restore the **bare** original mode
(e.g., `never`), plus idempotent/no-change cases, before physical testing.

For the second passive pass, run:

```sh
cd ~/projects/OneiOS
git pull --ff-only
ONEIOS_AUDIT_OUT="$HOME/oneios-m3q-ci/audits/followup-s24-vs-m3q" \
  ./scripts/phone/audit-s24-followup-m3q.sh
cat "$HOME/oneios-m3q-ci/audits/followup-s24-vs-m3q/followup.tsv"
cat "$HOME/oneios-m3q-ci/audits/followup-s24-vs-m3q/host-artifact.txt"
```

This reads HIDL/AIDL, display backlight attributes and source paths,
USB gadget and UDC state, shmem options, keys and watchdog presence,
and the **local** package archive's binary names. It never writes to
the device or starts a session. It cannot validate return Gates A/B/C.
A PASS in this follow-up still does not authorize switch.

## Second passive S26 audit: physical key origin, display power and USB (2026-11-17)

Owner-supplied `audit-s24-followup-m3q.sh` result: **PASS=3, WARN=1,
FAIL=0, BLOCKED=5, INFO=49**. The standalone CI archive for commit
`78e4e025` passed SHA-256 and contains `bin/bridgectl`, `brightctl`,
`camctl`, `displayctl`, `gpuctl`, and `oneios-audio`. That archive
inventory does not test those tools on the live device and does not
imply a staged runtime exists.

- **Physical rescue keys, now identified:** `gpio-keys` at
  `/dev/input/event0` advertises Volume Up, and `pmic_resin` at
  `/dev/input/event5` advertises Volume Down. A connected
  `HUAWEI Band 11-F88 Consumer Control` at `/dev/input/event13`
  advertises both keys but is an **external** input device.
  The S24 `oneios_switch.sh` intentionally combines volume events
  from multiple readers using shared `control/keys/up` and `down`
  and also polls all candidate devices. Its fake-phone JUnit tests
  cover separate device readers and split-device polling: separation
  alone is **not** a regression. The existing candidate discovery,
  however, includes the external wearable; do not count the wearable
  as proof of physical recovery capability. Before enabling an m3q
  session, restrict recovery-key selection to proven physical hardware
  sources and add fake tests for a transient/disconnecting external
  input and cross-device chord. Test a *physical* up/down press while
  Android stays live; do not engage the recovery script yet.
- **HIDL:** `init.svc.hwservicemanager=stopped` remains the only
  warning. The vendor QTI composer service and Composer3 AIDL binder
  are **running/registered**, as are SurfaceFlinger, audio-policy,
  camera, networking, keystore2 and servicemanager.
  `lshal` returned no matching legacy HIDL interfaces. A stopped
  hwservicemanager alone is not proof of broken modern AIDL services,
  but any legacy bridge dependency must be inspected before runtime.
- **Brightness:** `panel` (a virtual LCD node) reads `0/0`; the
  actual qcom MDSS backlight sysfs candidate is
  `panel0-backlight` at
  `/sys/devices/platform/soc/9800000.qcom,mdss_mdp/backlight/panel0-backlight`,
  with `max_brightness=12750`. Both reported zero and the latter
  `bl_power=4` *while* `dumpsys display` reported
  `mScreenState=OFF`; that is **not** evidence of brightness failure.
  Re-read while the display is visibly awake to identify the live
  backlight path. Do not write brightness or take display ownership.
- **USB:** `sys.usb.config=adb`, `sys.usb.state=adb`,
  `init.svc.adbd=running`, controller `a600000.dwc3`, and
  `/config/usb_gadget/g1/UDC=a600000.dwc3`; nevertheless
  `/sys/class/udc/a600000.dwc3/state=not attached`.
  This is compatible with the known wireless-only ADB session, not
  proof of USB transport continuity. Attach a data-capable USB cable
  to CachyOS and recheck physical enumeration in a later read-only
  audit. Do not alter `sys.usb.*` yet.
- **Shared memory:** `shmem_enabled` is
  `always within_size advise [never] deny force`, writable as root.
  The host supports the upstream `advise` mode and currently chooses
  `never`. The mode round-trip is **not** tested; the upstream
  early-return restoration edge case remains open.
- **Watchdogs:** `/dev/watchdog` and `/dev/watchdog0` exist.
  The S24 switch closes/feeds Samsung's software watchdog after
  framework stop; presence of character devices alone proves
  nothing about identical semantics on S26. Do not open/write them
  as a read-only test.
- **Cgroups:** root `cgroup.controllers` reports `memory`,
  while root `cgroup.kill` does not exist. The S24 return has a
  per-process SIGKILL fallback where `oneios_session/cgroup.kill`
  is unavailable. This is a **recovery-path compatibility question**,
  not a reason to simulate a kill against the live system.
- **Local-CI timing:** the posted status still reported
  `78e4e025` PASS while the source was already checked out at
  `379f64a5`. This is a prior run, **not proof that 379f64a5 passed**.
  Wait for the standalone watcher to process that commit and inspect
  its own status before treating its new scripts as CI-verified.

### Remaining checkpoints before *any* m3q recovery port

1. With One UI screen visibly on, read the physical MDSS
   backlight and confirm the framework reports ON. No sysfs writes.
2. Verify one **ordinary press each** of physical Volume Up and
   Volume Down with a bounded `getevent` capture. An external wearable
   cannot be an emergency-return dependency.
3. Connect a real USB **data** cable and confirm a USB ADB endpoint,
   USB controller attachment and serial route distinct from Wi-Fi ADB.
4. Reproduce recovery watchdog/USB/key-state logic in fake-phone tests
   specific to the S26 input graph; resolve shmem early-return.
5. Build and test the relevant OneOS fork locally before updating
   the OneiOS `app/` submodule pin; do not install/debug APK or
   touch OneOS 0.5.16 until the tests pass.
6. Later, the 14 original hardware checks still include active
   display, haptic, audio, brightness, camera and Bluetooth tests.
   Run each as a separately bounded diagnostic only with suitable
   prerequisites; do not label the existing passive inventories
   as full hardware PASS. The full return Gates A/B/C cannot be
   certified without a controlled recovery exercise.

**No switch permission follows from this audit.** Keep the
`SM-S928*/e3q/SM8650` root guard and app-level m3q block until
recovery can be demonstrated independently.


## Third passive audit — screen ON, genuine volume keys, no USB cable (2026-11-17)

The owner kept One UI running and used read-only ADB commands:

- Screen **ON**: Android DisplayManager reported
  `mScreenState=ON`, `mScreenBrightness=0.33885762`.
  `/sys/class/backlight/panel0-backlight` (QCOM MDSS) changed
  from the previous screen-OFF reading of 0 to **brightness=865**
  out of max 12750, `bl_power=0`, while its
  `actual_brightness=0`. This identifies the *likely* active
  backlight control node; `actual_brightness` is not reliable
  for reporting screen power/brightness on this kernel without
  additional driver-level validation. The virtual
  `/sys/class/backlight/panel` remains `0/0`.
  Never map the framework's normalized brightness to 865/12750
  as a linear fraction, and do not write either sysfs node yet.
- **Physical Volume Up**: `/dev/input/event0` emitted one
  `KEY_VOLUMEUP DOWN` followed by `UP`.
  **Physical Volume Down**: `/dev/input/event5` emitted one
  `KEY_VOLUMEDOWN DOWN` followed by `UP`. These *individually*
  validate the genuine key event sources on m3q; they do not
  exercise the 3-second simultaneous chord or OneOS readers.
  `/dev/input/event13` is an external Huawei Band, not a
  permissible sole rescue-key source.
- A data-capable **USB cable is not available** at this
  stage. Continue read-only checks over Wi-Fi ADB while One UI
  is up; do not label USB recovery as PASS. Wireless ADB is
  not a reliable recovery path when the Android framework
  or network stack is stopped. Do not switch, attempt in-place
  return, stop zygote, or open watchdog devices until
  independent return conditions are met.

### Correct command for the next brightness probe

The SM8850 brightness README specifically warns that
**`brightctl info` is not passive**: it opens the bridge and
can write panel brightness, touch IC state or ALS enable.
Only `brightctl inspect` uses read-only sysfs access.
The owner already has a standalone diagnostic `brightctl`
at `/data/local/tmp/oneios-m3q-diag/bin/brightctl`;
confirm `inspect` support before use, or deploy the
verified current runtime binary by an explicit diagnostic
procedure. Do **not** call `info`, `levels` or `als`
during passive validation.

```sh
adb shell su -c '/data/local/tmp/oneios-m3q-diag/bin/brightctl inspect'
```

The historical OneOS hardware checklist's `brightness`
entry invokes `brightctl info`, so the app's "Run all"
is **not** safe to run indiscriminately on the unported
S26 Ultra. Tests of actual screen-writing, ALS and
camera acquisition remain separate opt-in exercises.
Keep the standalone app's 0.5.16 installation and
the original SM8850 switch guards unchanged.
