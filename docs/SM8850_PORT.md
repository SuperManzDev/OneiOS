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

The host shell was Bash on CachyOS. An initial `getevent -lp | Select-String ...` command failed because `Select-String` is PowerShell-only. A subsequent `adb shell su -c 'getevent -pl'` capture provided the following actual evdev capabilities.

- Android display reports an internal 1440 x 3120 panel at 600 dpi and portrait orientation 0, with 120 Hz modes for native (1440 x 3120), FHD+ (1080 x 2340) and HD+ (720 x 1560) resolutions.
- SurfaceFlinger's active panel mode reports 1440 x 3120, peakRefreshRate about 120 Hz and vsyncRate about 240 Hz; Android's 48 Hz `renderFrameRate` is a per-render-rate observation, **not** evidence that the panel's native refresh is 48 Hz.
- The display reports ARR support and intermediate render rates; do not conflate app render-frame pacing with the selected display mode.
- `/dev/dri/card0` and `/dev/dri/renderD128` exist (crw-rw-rw-); `/sys/class/backlight/panel` and `/sys/class/backlight/panel0-backlight` exist. `/sys/class/graphics` is absent on this host; this alone is not a fault.
- `panel0-backlight` resolves through the Qualcomm MDSS MDP backlight class. No brightness values or writable interface were exercised.
- `dumpsys display` exposes the internal cutout and rounded corners; no display bridge has been launched or validated on this phone.
- Touchscreen: `/dev/input/event8` `sec_touchscreen`, `INPUT_PROP_DIRECT`, 10 touch slots, MT X/Y 0..4095. This is eligible for the existing virtio-input bridge.
- Samsung `/dev/input/event9` `sec_touchpad` is a separate MT device, also X/Y 0..4095, without `INPUT_PROP_DIRECT`. It must not be mixed with the direct panel.
- A **connected HUAWEI Band 11-F88** on `/dev/input/event14` exposes `INPUT_PROP_DIRECT` and MT X/Y 0..1079 / 0..2399 (8 slots). Its virtual touchscreen must not be selected instead of Samsung's internal panel if event numbers reorder. The input scanner now first looks for an eligible `sec_touchscreen` and prefers it over other direct touch devices. Without a native Samsung panel, generic direct touch selection is unchanged.
- Physical keys are split across `gpio-keys` event0 (Volume Up), `pmic_resin` event5 (Volume Down), and `pmic_pwrkey` event4 (Power). A paired HUAWEI Band consumer-control device (event13) also advertises volume keys.
- S Pen: `/dev/input/event12` `sec_e-pen` advertises BTN_TOUCH and BTN_STYLUS, ABS_X 0..7330, ABS_Y 0..15881, pressure 0..4095, distance 0..255, and tilt X/Y -63..63. **BTN_TOOL_PEN is absent**. The original `xh_pen_rank` rejects it; the port now accepts the exact `sec_e-pen` signature with tip, side key, distance and both tilt axes, while rejecting other names, touchscreen and hardware keys. Without a real pen event trace, only tip-contact behavior is justified; hover/in-range without BTN_TOOL_PEN remains unproven and is not synthesized.
- Backlight: `/sys/class/backlight/panel` reports max 0, while `panel0-backlight` reports max 12750 and brightness/actual_brightness both 0 at capture. The Samsung backend's lookup skips zero maxima, so it would choose `panel0-backlight`. Do **not** interpret sysfs zero as proof that the display is off; Android reported display ON.
- **Important brightness side effect:** existing `brightctl info` calls `br_bright_open`, which can write a midrange value if the initial brightness reads 0 (and may enable the ALS). Even the `info` subcommand is not strictly read-only. Until a dedicated passive probe is added, use `cat` on the sysfs nodes and **do not run brightctl info/levels on this SM8850 test phone**.
- The first multi-line `adb shell su -c '...'` backlight attempt printed KernelSU's `Argument to option 'c' missing` and then output from the remaining shell loop. Treat the displayed sysfs values as a passive shell reading, not a successful root-only command; no writes were made.

**Next safe read-only command from the Linux host (when a fresh snapshot is needed):**

```sh
adb shell getevent -pl | grep -E 'add device|name:|ABS_MT_(POSITION_X|POSITION_Y|SLOT|TRACKING_ID)|ABS_(X|Y|PRESSURE|DISTANCE|TILT_X|TILT_Y)|BTN_(TOOL_PEN|TOUCH|STYLUS|STYLUS2)|KEY_(POWER|VOLUMEUP|VOLUMEDOWN)'
```

Keep any `displayctl` test separate from input inventory: it intentionally creates a transient SurfaceFlinger layer above One UI. No `--takeover`, mode change, hot-switch or kernel patch is justified by the passive display dump.

Input compatibility is now covered by `runtime/xnuhost/tests/test_xnuhost.c` and the `xnuhost-input` SM8850 CI job; the tests do not emulate the Samsung hardware itself. Capture a **bounded** non-destructive event trace of S Pen hover, tip contact and lift, including all raw keys/MSC/ABS codes, before making claims about hover support.

## Scope of first commit series

Only a regression fixture for SM8850 CPU topology and the companion OneOS submodule URL/branch were added. No changes to xnuhost, kernel, Metal or GPU driver behavior are justified solely by these measurements. OneOS still blocks hot-switch on SM8850 until independent recovery verification.

Companion: https://github.com/YMGPwcca/OneOS/tree/feat/oneios-sm8850-m3q