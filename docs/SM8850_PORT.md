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
- S Pen: `/dev/input/event12` `sec_e-pen` advertises BTN_TOUCH and BTN_STYLUS, ABS_X 0..7330, ABS_Y 0..15881, pressure 0..4095, distance 0..255, and tilt X/Y -63..63. **Correction:** the earlier claim that BTN_TOOL_PEN was absent was incorrect: `getevent` prints `BTN_DIGI` for evdev key code `0x140`, exactly the same numeric value as Linux `BTN_TOOL_PEN`. A full evdev capture confirms `BTN_DIGI DOWN/UP` at the start/end of hover. OneiOS's original strict detection and hover logic is compatible; an unnecessary tool-less Samsung detection exception was reverted.
- Backlight: `/sys/class/backlight/panel` reports max 0, while `panel0-backlight` reports max 12750. The Samsung backend's lookup skips zero maxima and chooses `panel0-backlight`. An earlier reading had brightness/actual_brightness 0 even when Android reported display ON; a later user-controlled slider sweep establishes that the node's **brightness** responds to One UI. `actual_brightness` still reads 0 at all tested levels (not a usable luminance sensor). `bl_power=0` in these observations while the screen is lit (normally Linux unblank).
- **Important brightness side effect:** existing `brightctl info` calls `br_bright_open`, which can write a midrange value if the initial brightness reads 0 (and may enable ALS and touch IC power changes). **Do not run `info`, `levels`, or `als` on SM8850 during discovery.** The SM8850 branch now adds **`brightctl inspect`**, a separate `O_RDONLY`-only path that reports named backlight sysfs values without opening the active bridge, touching ALS or changing the touch IC. The fixture test `runtime/bridges/brightness/test_inspect.sh` covers the exact S26 observation (`panel` max 0, `panel0-backlight` max 12750, both current values 0) and no file content changes. Probe must still be built and tested on the phone before the active brightness route can be evaluated.
- The first multi-line `adb shell su -c '...'` backlight attempt printed KernelSU's `Argument to option 'c' missing` and then output from the remaining shell loop. Treat the displayed sysfs values as a passive shell reading, not a successful root-only command; no writes were made.

**Next safe read-only command from the Linux host (when a fresh snapshot is needed):**

```sh
adb shell getevent -pl | grep -E 'add device|name:|ABS_MT_(POSITION_X|POSITION_Y|SLOT|TRACKING_ID)|ABS_(X|Y|PRESSURE|DISTANCE|TILT_X|TILT_Y)|BTN_(TOOL_PEN|TOUCH|STYLUS|STYLUS2)|KEY_(POWER|VOLUMEUP|VOLUMEDOWN)'
```

Keep any `displayctl` test separate from input inventory: it intentionally creates a transient SurfaceFlinger layer above One UI. No `--takeover`, mode change, hot-switch or kernel patch is justified by the passive display dump.

### One UI manual brightness sweep (read-only, 2026-11-16)

Adaptive brightness was intended to be disabled for repeatability; the user manually selected roughly three different positions on the One UI slider. The percentages label the approximate slider positions, **not calibrated luminance percentages**. No sysfs writes or root-only commands were used for this sweep.

| Slider position | `settings get system screen_brightness` | `panel0-backlight/brightness` | `actual_brightness` | `bl_power` |
| --- | ---: | ---: | ---: | ---: |
| Low (~20%) | 49 | 512 | 0 | 0 |
| Middle (~50%) | 115 | 1228 | 0 | 0 |
| High (~90%) | 222 | 2300 | 0 | 0 |

`settings get system screen_brightness_float` returned `null` at every position. `panel` remained max/brightness/actual=0 and `bl_power=0`; `panel0-backlight/max_brightness` stayed 12750.

**Follow-up at the exact manual slider maximum:** The user moved the One UI brightness slider fully to its end (without an explicit Extra brightness/HBM reading). `adb shell settings get system screen_brightness` returned **255** and `adb shell cat /sys/class/backlight/panel0-backlight/brightness` returned **2300**. The earlier near-high slider at Android value 222 already read 2300, so the **observed normal slider output plateaus** by that point. We have not observed/verified the purpose of the hardware's 12750 upper bound, nor measured HDR/HBM or extra-brightness output.

**SM8850 manual test profile:** Use `ONEIOS_BRIGHT_MAX=2300` as an **explicit opt-in ceiling candidate only in controlled fake-sysfs or future permitted active tests**, never silently change Samsung S24 or generic profiles. The new `test_sm8850_manual_brightness_cap` asserts that `br_bright_open` with `o.ceiling=2300` picks the nonzero-max `panel0-backlight` and maps a full slider to <=2300 in fake sysfs. This is not a physical write validation or a promise that any value up to 2300 is harmless under all panel modes.

**Key safety finding:** The active brightness bridge currently sets its ceiling to `br_bright_ceiling(12750) = 12750` by default; it only halves special max values 510/51000. The measured `panel0-backlight/brightness` at a near-high manual slider setting was **2300**, far lower than 12750. The upper region could be reserved for high-brightness/HDR handling or another extended hardware mode; there is not enough evidence yet to infer its purpose or safe maximum. **Do not run active `brightctl info/levels` or an SM8850 iOS brightness session using the unbounded default ceiling.** Need a passive reading at the slider's normal maximum (without extra-brightness/HBM toggles) and a validated ceiling policy before enabling active writes.

## Physical display helper diagnostic and correction (2026-11-16)

The verified SM8850 CI runtime `oneios-runtime-0.0.0-ci-arm64.tar.zst` was extracted and the display/brightness diagnostic binaries staged under `/data/local/tmp/oneios-m3q-diag` on the phone. This is **not** an iOS session.

- `brightctl inspect`: **PASS**, no writes. `panel` max/brightness/actual=0; `panel0-backlight` max=12750, brightness=645, actual=0, bl_power=0, chosen. Ends `inspect ok (no state changes)`.
- First `displayctl test --seconds 5`: the helper connects to Android's SurfaceFlinger and detects a 1440x3120 internal panel, native 120 Hz, SRGB mode, HDR types 2/3/4, reported panel max luminance 1275 nits (panel capability report, **not** measured actual brightness), and posts a SurfaceControl layer. Buffer type 5 (BGRA_8888), sRGB dataspace 0x08810000, 1:1 native size. On exit the layer was removed, process status 0.
- **Not yet a visual display PASS:** `displayctl` sent **482** test updates over 5.01s but `oneios-display` logged `the first frame waits for /data/local/tmp/displayctl.*/ready`, then `the socket closed after 0 frames`. The command's old `displayctl: ok` only certified transport and cleanup, not actual on-screen presentation.
- **Root cause in diagnostic harness:** `displayctl` launched `oneios-display --session DIR`, which waits for the real XNU `session/ready` file; a standalone color-bar diagnostic has no XNU process and never creates that file. This is distinct from and does not indicate an Android GPU/display driver failure.
- **Code fix:** `displayctl test` now launches `oneios-display --socket DIR/display.sock` so the helper uses its existing no-ready-hold diagnostic path. The production XNU `--session` readiness behavior is unchanged. The test now allowlists forwarded options, rejecting `--takeover`, `--session`, `--socket`, `--lib`, `--wait` and other potential endpoint/takeover overrides. Host test asserts `--socket` plus path, processes the test frame and checks that the temporary directory is removed.
- **Second on-phone diagnostic (2026-11-16, legacy runtime + temporary socket wrapper):** The user used a small `socket-helper.sh` wrapper to translate the old `displayctl`'s `--session DIR` argument into the display helper's existing `--socket DIR/display.sock` mode. The verified old Android binaries otherwise remained unchanged and were *not* replaced with the newly compiled `displayctl` yet. The helper successfully detected the native **1440x3120 at 120 Hz** panel, SRGB color mode, BGRA_8888 buffer format 5, dataspace 0x08810000, and showed a 1:1 geometry. Log sequence: `layer up`, **`first frame posted`**, `the socket closed after 578 frames`, `layer removed`; `displayctl` logged **387 pattern update steps over 5.01 seconds**, `the helper removed its layer and exited 0`, and `displayctl: ok`. These counters measure different events; their difference is not an error. The helper reports `reading xnuhost's socket failed: Connection reset by peer` when the diagnostic ends and its sender closes, then cleans up normally.
- **Acceptance split:** Actual host graphics buffer posting and clean SurfaceFlinger layer teardown are **PASS from logs**. Visual color bars / moving white square **have not been confirmed by the user**, so visible panel output cannot yet be marked PASS. The current fix's host regression CI passed, but the newly packaged `displayctl` binary still needs separate validation; the wrapper demonstrates equivalent no-ready-hold behavior on hardware. An XNU guest, guest Metal rendering, touch-to-iOS routing, and recovery remain **untested**.
- **Next required observation:** Ask the user whether colored bars and a moving white square were visible for the five seconds, with One UI restoring immediately afterward. If yes, mark diagnostic overlay as visually verified; if not, collect SurfaceFlinger/layer state rather than advancing to hot-switch.

## Physical visual acceptance and moving-square flicker (2026-11-16)

The user **visually confirmed** that the color-bar pattern and a moving white square really appeared on the Galaxy S26 Ultra's built-in display during the five-second no-takeover test, and that the motion looked smooth at the panel's 120 Hz setting. The user also observed **noticeable flicker on the moving white square**. This verifies the diagnostic overlay's ability to present visible content and restore One UI; it does **not** prove a measured stable 120 FPS (the log reported 387 pattern steps in 5.01 seconds and 578 helper buffer posts, which are different counters).

The diagnostic's original animation loop called `xhd_send_flush` **twice** per logical movement: once for the old square and once for the new position. With the shared memfd and the helper's partial-damage `ANativeWindow_lock/unlockAndPost` path, these independent packets could cause an intermediate buffer containing only one updated rectangle (especially at the moving edge). This is a plausible, source-backed explanation for the flicker, **not yet confirmed as the only cause**; the shared framebuffer still lacks a producer/consumer write barrier and can race during copying.

**Flicker mitigation implemented in the diagnostic only:** Compose the restored background and new square first, form the union of old and new rectangles using `xhd_rect_union`, then send **one** `XHDF` damage message for each movement. CI's stand-in display helper counts actual received packets and asserts the initial full-frame message plus exactly one partial update per completed movement for both 8-bit BGRX and RGBA16F. No change to real guest XNU frame transport, graphics drivers, brightness controls, root/model gate or return flow.

**Pending on-device acceptance:** Build and run a new CI `displayctl` binary containing this change without the temporary `socket-helper.sh` wrapper; confirm the white square no longer flickers, animations remain smooth, first frame is posted, and the helper exits cleanly. If flicker persists, investigate shared memfd producer/consumer synchronization and SurfaceFlinger queue timing before attempting guest rendering or color-space testing.

## Captured S Pen evdev trace (2026-11-16)

The user provided 11,126 `getevent -lt /dev/input/event12` log lines across 17.82 s, containing 3,507 SYN_REPORT frames, 2 complete hover/contact/out-of-range cycles and a third, still-in-hover cycle truncated by the recording timeout. No `BTN_STYLUS` press/release appeared in this capture (the side button capability exists but remains untested dynamically).

| Event | evdev time (s) | Meaning |
| --- | ---: | --- |
| BTN_DIGI DOWN (key 0x140) | 868.741952 | First hover begins; positions/distance/tilt arrive; distance 77 |
| BTN_TOUCH DOWN | 870.919734 | Tip contacts screen; pressure subsequently rises up to 156 |
| BTN_TOUCH UP | 873.328402 | Tip lifts; BTN_DIGI stays down and hover continues |
| BTN_DIGI UP | 874.744644 | First hover ends; no contact remains |
| BTN_DIGI DOWN | 881.791380 | Second hover begins |
| BTN_TOUCH DOWN | 882.681687 | Second contact; max observed pressure 272 |
| BTN_TOUCH UP | 883.357822 | Second tip lifts; hover continues |
| BTN_DIGI UP | 884.031229 | Second hover ends |
| BTN_DIGI DOWN | 886.161176 | Third hover begins; timeout ends mid-hover |

The approximately 2 ms spacing of many frames demonstrates the input report cadence *in this capture*, not a guaranteed constant 500 Hz device polling rate. The observed pen coordinate range during the trace was X 0..6728, Y 0..12545; kernel-advertised full ranges remain X 0..7330 and Y 0..15881. The tool uses the existing evdev BTN_TOOL_PEN_ constant `0x140`; in-range is properly derived from this key, independently of tip contact. Regression tests now cover the strict code-0x140 lookup and captured hover-contact-lift-leave sampling using the measured axis bounds.

Input compatibility is covered by `runtime/xnuhost/tests/test_xnuhost.c` and the `xnuhost-input` SM8850 CI job. These are host tests of simulated events, **not** real guest iOS pen rendering, pressure, handwriting or side-button usability.

## Scope and boundaries of the SM8850 development branch

- Added CPU topology regression fixture from measured SM8850 sysfs and companion OneOS submodule fork/branch wiring.
- Preserved the original strict S Pen detection: the `BTN_DIGI` label in Android `getevent` and `BTN_TOOL_PEN` in OneiOS both mean Linux key `0x140`. Added regression tests grounded in the live hover/contact/lift trace; reverted the unnecessary tool-less lookup exception.
- Added a change to xnuhost's touchscreen selection to prefer a genuine direct `sec_touchscreen` over direct virtual touch devices from connected wearables, with dedicated tests and a SM8850 CI job.
- Display and backlight findings are **documented**, with a new passive `brightctl inspect` tool and host fixture tests. The on-phone display driver, brightness output and touch-power writes have **not** been tested. `displayctl test` now explicitly refuses the session-only `--takeover` flag, so visual tests can overlay a temporary pattern without changing display modes or invoking hot-switch.
- No modifications to XNU, kernel patches, GPU driver behavior or the Android kernel. OneOS still blocks hot-switch on SM8850 until independent recovery verification. A green host CI suite is not proof of real-phone pen, touch, graphics, iOS boot, or safe recovery.

Companion: https://github.com/YMGPwcca/OneOS/tree/feat/oneios-sm8850-m3q