# SM8850 host validation — Galaxy S26 Ultra

Target measured on 2026-11-16: Samsung SM-S948B (m3q), Qualcomm SM8850 (canoe), Android 17 / One UI 9.0, kernel 6.12.69, 4 KiB pages, KernelSU/SELinux Enforcing.

## CPU results

| Host CPUs | capacity | max frequency (kHz) | Current OneiOS relative class |
| --- | ---: | ---: | --- |
| 0–5 | 741 | 3628800 | E |
| 6–7 | 1024 | 4742400 | P |

bridgectl cpu from runtime v0.1.9 reported 8 CPUs and a 6E/2P plan, successful vcpu7 affinity and uclamp.min. The new fixture in runtime/bridges/tests/test_bridges.c captures the observed topology. These relative E/P labels are *not* verified physical efficiency/performance core labels. Do not force an S24 2E/6P configuration without XNU scheduler validation.

## GPU results

Verified SHA-256 of the public OneiOS v0.1.9 release before running diagnostics, as KernelSU root while One UI was running:

- gpuctl nodes: KGSL, udmabuf, DMA heap open. udmabuf limits 64 MiB per buffer, 1024 runs.
- gpuctl routes: memfd-udmabuf, system DMA heap, qcom system heap, and KGSL GPUOBJ_ALLOC all successfully imported, bound and CPU mapped. A 256 GiB VBO starves GPU addresses; 192 GiB works.
- gpuctl probe: chip 0x44050a00, GPU ID 6860, 192 GiB VBO, GMEM 18432 KiB.
- gpuctl submit -v: 16 KiB buffer, CP_NOP command submission, GPU fence retired after 0.72 ms, zero faults and async errors.
- gpuctl large -v: 96 MiB split across 2 udmabufs; 1100 separate pages split across 2 udmabufs; all command buffers and fences succeeded (5.30, 0.16, 0.38 ms), zero faults and async errors.
- KGSL reported that imported buffers retained IO coherence. GPU clocks reported 160–1300 MHz.

These results show basic host GPU execution, **not** guest Metal or graphics output, sustained FPS or reliability.

## Display and input inventory (2026-11-16, non-invasive)

The host shell was Bash on CachyOS. An initial `getevent -lp | Select-String ...` command failed because `Select-String` is PowerShell-only; **no input-device capabilities were collected** in that run. Do not infer touchscreen/S Pen compatibility from it.

- Android display reports an internal 1440 x 3120 panel at 600 dpi and portrait orientation 0, with 120 Hz modes for native (1440 x 3120), FHD+ (1080 x 2340) and HD+ (720 x 1560) resolutions.
- SurfaceFlinger's active panel mode reports 1440 x 3120, peakRefreshRate about 120 Hz and vsyncRate about 240 Hz; Android's 48 Hz `renderFrameRate` is a per-render-rate observation, **not** evidence that the panel's native refresh is 48 Hz.
- The display reports ARR support and intermediate render rates; do not conflate app render-frame pacing with the selected display mode.
- `/dev/dri/card0` and `/dev/dri/renderD128` exist (crw-rw-rw-); `/sys/class/backlight/panel` and `/sys/class/backlight/panel0-backlight` exist. `/sys/class/graphics` is absent on this host; this alone is not a fault.
- `panel0-backlight` resolves through the Qualcomm MDSS MDP backlight class. No brightness values or writable interface were exercised.
- `dumpsys display` exposes the internal cutout and rounded corners; no display bridge has been launched or validated on this phone.

**Next safe read-only command from the Linux host:**

```sh
adb shell getevent -pl | grep -E 'add device|name:|ABS_MT_(POSITION_X|POSITION_Y|SLOT|TRACKING_ID)|ABS_(X|Y|PRESSURE|DISTANCE|TILT_X|TILT_Y)|BTN_(TOOL_PEN|TOUCH|STYLUS|STYLUS2)|KEY_(POWER|VOLUMEUP|VOLUMEDOWN)'
```

Keep any `displayctl` test separate from input inventory: it intentionally creates a transient SurfaceFlinger layer above One UI. No `--takeover`, mode change, hot-switch or kernel patch is justified by the passive display dump.
## Scope of first commit series

Only a regression fixture for SM8850 CPU topology and the companion OneOS submodule URL/branch were added. No changes to xnuhost, kernel, Metal or GPU driver behavior are justified solely by these measurements. OneOS still blocks hot-switch on SM8850 until independent recovery verification.

Companion: https://github.com/YMGPwcca/OneOS/tree/feat/oneios-sm8850-m3q