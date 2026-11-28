# Take 23.1 — guarded 3 GiB native baseline

Status: **host-built and host-tested; not staged, not phone-booted, and not phone-qualified**.

Take 23.1 begins with the safety work required by `STABILITY-INVESTIGATION-REVIEW.md`. It deliberately does not combine renderer, shader-cache, CMA, screen-recording or appinst changes with the first guarded baseline.

## Baseline

- 3072 MiB shared RAM; 1520 required 2 MiB pages.
- Six vCPUs.
- 1440×3120 at 3x and 120 Hz.
- crosvm `--gpu-scanout`; pool-copy remains diagnostic only.
- CPU/GPU clock floors off.
- Existing Take 23 native kernel and GPU add-on, so the first comparison changes host safety and RAM size rather than guest GPU policy.
- Non-appinst baseline. The appinst candidate remains a separate unqualified milestone.

Host artifact built on 2026-11-27:

```text
out/crosvm/kernel-take23.1-guarded-3072.img
Image SHA-256: 37174ad656ab2019572e25553bcb41daf70b9cad7a7c5ab261854443c132eb7a
ADT SHA-256:   d5642e63a4ec0c37c51ab40aed76ead3ae6a7565cdb36fc7b684c15ce850d9c4
disks SHA-256: beafccd4b4ee928667eb4bd285947bf35ad07b4ee74bf73b9d49db02eabe4541
```

The Image and build logs are local ignored artifacts and are not distributable project source.

## Independent guard, first implementation

`runtime/guard/oneios-guard` is a separate static Android process. The generated takeover launcher must start it and wait for its `armed` record before stopping One UI.

Implemented host-side behavior:

- discovers a non-direct physical Volume Down input device without grabbing it;
- requires SoC, GPU, board/case and battery thermal channels;
- takes sustained cool preflight samples before arming;
- applies configurable sustained stop thresholds;
- treats loss of any required thermal channel as a stop condition;
- recognizes a ten-second Volume Down hold independently of the touch forwarder;
- records crosvm PID and `/proc/PID/stat` start time and validates command/session identity before signalling;
- requests graceful crosvm stop, then TERM/KILL escalation;
- coordinates `cleanup.done` before asking the shared recovery script to start One UI;
- requests the existing named last-resort reboot rather than starting Android over takeover-owned hardware when cleanup cannot be verified;
- stores guard state/logs under a root-only directory;
- persists/restores host media volume through recovery state rather than only a shell-local variable.

The generated launcher now:

1. stages and arms the guard;
2. refuses takeover if it cannot arm;
3. cancels huge-page acquisition when a guard stop appears;
4. checks for a guard stop again immediately before crosvm;
5. publishes crosvm identity after fork;
6. restores host resources before publishing `cleanup.done`;
7. exposes the same idempotent `ui.sh --recover` path to the guard and normal trap;
8. makes the final staged session directory root-owned mode 0700.

## Current limits before phone qualification

The first implementation is intentionally **not yet approved for a phone launch**. Remaining host work includes:

- replace check-then-`kill(pid)` with pidfd signalling to remove the final PID-reuse race;
- add process-level tests with fake procfs, evdev, thermal sysfs, crosvm control and recovery scripts;
- make the session-level launcher lock survive and reject a second concurrent launcher;
- add bounded key-device reopen and validate the exact S25 PMIC input identity;
- exercise guard-trigger-during-acquire, trigger-before-VM-registration, stale recovery owner and failed-cleanup paths end to end;
- review the default stop thresholds against owner-approved first-session policy.

No phone command should be run merely because the host Image exists.

## Build

```bash
./build_take23_1.sh
```

The script runs guard tests, the oneios-phone tests, builds the static Android guard and builds the 3072 MiB Image. It does not contact the phone.

Current host results:

```text
guard policy: 31 checks, 0 failures
oneios-phone: 69 passed, 0 failed
Android guard: static arm64, Android API 30
runtime stage ELF check: 0 failures
```

## Future stage-only command

After the remaining guard work is reviewed, staging can use the parameterized script:

```bash
IMAGE=out/crosvm/kernel-take23.1-guarded-3072.img \
STAGE_OUT=phone-vmboot-take23.1-guarded-3072-staged \
PIN_FIRST=0 \
./stage_take23_on_phone.sh
```

That command is stage-only and must not start the VM. `PIN_FIRST=0` keeps this diagnostic profile off prime cores 6–7 while virtio-gpu polling remains.

Do not invoke `run_take23.sh`, `oneios-native launch`, the Android Launch shortcut or the staged `boot.sh` until the guard is approved for phone qualification.
