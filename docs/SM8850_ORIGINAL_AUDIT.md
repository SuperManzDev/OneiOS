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
