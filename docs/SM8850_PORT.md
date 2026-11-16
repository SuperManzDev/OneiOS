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

**On-device acceptance: PASS (2026-11-16).** The user downloaded the **new ARM64 runtime artifact** from GitHub Actions run `43799119719`, verified `oneios-runtime-0.0.0-ci-arm64.tar.zst: OK` with SHA256, and pushed only the newly built `displayctl` to `/data/local/tmp/oneios-m3q-diag/bin/displayctl`. The test ran **without the temporary `socket-helper.sh` wrapper** and completed:
- Panel: 1440x3120, 120 Hz active/native, 1:1 display; SurfaceFlinger layer and 8-bit sRGB/BGRA_8888 buffers created normally.
- `oneios-display: first frame posted (...)`, `the socket closed after 369 frames`, `layer removed`; `displayctl: 377 frames in 5.01 s`, `the helper removed its layer and exited 0`, `displayctl: ok`.
- User visually confirms: **"hết flicker rồi"** (white square no longer flickers), smooth motion, real on-panel display works. The layer and One UI are restored normally as reported by the helper.
- This result validates the **diagnostic's one-union-flush-per-step flicker mitigation on real SM8850 hardware**. Helper posts and diagnostic steps count different things; `377/5.01` steps per second is not a verified sustained 120 application FPS merely because the panel advertises 120 Hz.
- `Connection reset by peer` is logged when the diagnostic client closes its socket on completion; no failure status followed, and teardown succeeded.

### On-device RGBA_FP16 / scRGB-linear acceptance (2026-11-16)

The user ran the already-staged no-takeover command `/data/local/tmp/oneios-m3q-diag/bin/displayctl test --seconds 5 --format rgba16f` on the Galaxy S26 Ultra. Full on-phone diagnostic result:

- Native internal panel **1440x3120, 120 Hz**, 1:1 layer geometry.
- Sender: `pattern in R,G,B,A half float, linear extended sRGB`; frame stride **11520 bytes** (1440 x 8), protocol **format 2**.
- Helper selected Android **buffer format 22 RGBA_FP16**, actual buffer stride **1472 pixels**, and **`scRGB-linear` dataspace `0x18410000`**. This is the actual posted buffer, not merely the desired format. **No BGRA_8888 fallback** was reported.
- Helper log: `layer up`, `first frame posted (1440x3120 buffer, stride 1472, format 22 RGBA_FP16, dataspace 0x18410000 scrgb-linear; ...)`, `the socket closed after 459 frames`, `layer removed`, `displayctl: ok`.
- Diagnostic generated **496 animation steps in 5.00 seconds** (~99.2 steps/s); helper posted 459 buffers (~91.8 posts/s). These are different counts. The panel's refresh capability is 120 Hz, **not** proof of sustained 120 application FPS.
- `Connection reset by peer` preceded normal layer teardown when the diagnostic ended, consistent with previous successful trials.

**Result: PASS for host-to-Android native RGBA_FP16 allocation, frame posting, dataspace tagging and clean overlay teardown on SM8850.** Still **unverified by eye**: actual wide-gamut P3 separation in the patches, full-screen visual stability/flicker in the FP16 mode, extra HDR brightness/headroom, and precise color accuracy. The panel announced SRGB among available modes `NATIVE,SRGB,DISPLAY_P3`; the current active color mode does not alone establish visible P3 reproduction. No destructive `--takeover` was used.

**Next stage:** Passive read-only checks of SM8850 battery and thermal sensors using `batteryctl` and `thermalctl` from the same verified runtime; record which readings are valid and which are missing. Defer haptic playback, audio routing, Bluetooth controller takeover, camera acquisition, brightness writes and especially XNU hot-switch until independently validated, individually reversible tests and recovery checks. This successful FP16 host overlay does **not** establish guest Metal rendering, S Pen routed into iOS, XNU boot or safe recovery.

## On-device battery and thermal bridge inventory (2026-11-16)

The owner staged checksum-verified ARM64 `batteryctl` (448 KiB) and `thermalctl` (485 KiB) from the existing CI runtime under `/data/local/tmp/oneios-m3q-diag/bin/` and ran each **once**, read-only, with One UI running and no XNU session.

### Battery Samsung backend — PASS

`batteryctl` reported `bridges: hw: battery: backend samsung (the default)`, `status "Discharging"` and `batteryctl: ok`. The sample was **90%**, `present=1`, `plugged=0`, `charging=0`, `full=0`, **30.1 C** (`temp=301` tenths), **4327 mV**, **-270 mA**, **5000 mAh** reported both full and design, **28 cycles**, and `minutes=1000` (an instantaneous approximate runtime estimate, not a measured discharge duration). The Samsung default sysfs backend appears functionally compatible; this is a single snapshot rather than longitudinal power validation.

### Thermal Samsung backend — false `trapping` from non-live trip zones

The original `thermalctl` listed **30 thermal zones of interest**, including **`cpu-hw-trip-0` = 105000** and **`cpu-hw-trip-1` = 105000** millidegrees. Other readings at the same moment were real CPU max **37.4 C** (`cpu-0-5-1`), GPU max **35.9 C**, DDR **35.5 C**, AP board **32.2 C**, battery **30.1 C**, `lrp`/case **32.1 C**. `xo-therm` was absent/unknown. The old thermal reader used broad `cpu-*` matching, accidentally included the two `cpu-hw-trip-*` zones and reported **`cpu 105.0 soc 105.0 (cpu-hw-trip-0)`**, **`level trapping (40)`**, despite the hardware being cool. `xnuhost step 0` was unaffected, but the virtual thermal notification to iOS would have been incorrect.

**Fix:** `runtime/bridges/thermal/thermal.c` now narrowly excludes `cpu-hw-trip-*` before treating `cpu-*` as a genuine live CPU sensor. Their value/name strongly suggests a hardware trip threshold pseudo-zone, though the precise Samsung driver implementation is not verified. The regression test adds both 105 C pseudo-zones and checks they do not cause a false critical pressure, while verifying a **genuine** CPU zone at 105.5 C still triggers `TRAPPING`. A second fixture mirrors the user's cool readings and expects nominal pressure and cap 0. No generic temperature ceiling was lowered, no thermal safety mechanisms were disabled, no S24 behavior changed.

**Post-fix on-device acceptance: PASS (2026-11-16).** The owner downloaded runtime artifact from [Actions run 43802356785](https://github.com/YMGPwcca/OneiOS/actions/runs/43802356785), verified `oneios-runtime-0.0.0-ci-arm64.tar.zst: OK` with SHA256, deployed only the updated `thermalctl`, and reran the read-only check with One UI still active:

- `backend: samsung`; **`zones: 28 of interest`**; neither `cpu-hw-trip-0` nor `cpu-hw-trip-1` appears among the accepted live zones.
- `reading (C): cpu 40.5 gpu 37.4 ddr 37.4 soc 40.5 (cpu-0-5-1) xo - ap 33.3 battery 29.6 lrp 32.4; case = 32.4 (lrp)`.
- **`level: nominal (0); case rule 0, SoC rule 0, battery rule 0; xnuhost step 0`**.
- The list printed `cpu-0-5-1` at 40.1 C, while the later aggregated read was 40.5 C. The two readings occur at different times and can naturally change, especially with CPU activity; no inconsistency is established.

**Conclusion:** The narrowly scoped pseudo-zone exclusion works in the actual S26 Ultra thermal sysfs, eliminates the false SoC 105 C / trapping report, and retains actual CPU/GPU/DDR and case/battery temperatures. The existing host regression also preserves real high-temperature alerts; the phone test does not itself establish every high-heat response. This is **thermal sensor classification and nominal pressure PASS**, not XNU boot/guest thermal-policy validation. No sysfs writes, hot-switch or physical thermal load test was performed.

## On-device Wi-Fi status / Supplicant AIDL (2026-11-16)

The owner deployed the existing, checksum-verified `wifictl` ARM64 utility from the CI runtime to `/data/local/tmp/oneios-m3q-diag/bin/wifictl` and ran **`wifictl status` only**, with One UI connected to Wi-Fi. No SSID, passphrase, join/leave, interface ownership change, or network reset was performed.

- **Status probe PASS:** `wlan0` exists; `android.hardware.wifi.supplicant.ISupplicant/default` is available; station interface `wlan0` returned successfully. The interface and supplicant both reported the same MAC address (omitted from this public document).
- Supplicant reported **one configured network** (network ID `0`) and a valid `signal poll`: **-49 dBm**, reported transmit PHY rate **468 Mb/s**, receive PHY rate **526 Mb/s**, frequency **5805 MHz** (5 GHz, channel 161). Utility exited with **`ok`**.
- These are currently reported station/link statistics, **not** a measured IP throughput or proof of guest networking. The AIDL service lookup and read-only status query work on SM8850 under One UI.
- Source review: `wifictl status` uses `br_supplicant_open(ifname, 0, ...)`, `br_supplicant_status`, and close; it does not call `br_wlan_prepare`, remove configured networks or change IP addresses. `wifictl scan [SECONDS [IFACE]]` uses nl80211 **`NL80211_CMD_TRIGGER_SCAN`** and **`GET_SCAN`**; it triggers a real radio scan (possibly causing brief connectivity/latency variation), but does not call the disruptive session `join` path. A single supervised `scan 6 wlan0` is the next candidate check, with returned SSIDs/BSSIDs redacted before sharing.
- **Physical scan acceptance: PASS (2026-11-16).** From the already-staged checksum-verified `wifictl` binary, the owner ran `wifictl scan 6 wlan0` via root while One UI was active. `nl80211` scan/dump returned **46 BSS/network entries** and `ok`, without any error or resource-busy message. Results spanned **2.4 GHz** (2412–2462 MHz) and **5 GHz** (5180–5805 MHz), reporting WPA2 and mixed WPA3/WPA2 security with several hidden-SSID entries and signal readings around -50 to -89 dBm. All SSIDs and BSSIDs were **redacted from the shared log**, and none are stored in this public port report. This confirms the non-join network discovery path on SM8850; it does **not** prove a 6 GHz scan, actual Internet throughput, guest iOS association, DHCP, IP transfer, or long-term reconnection stability. The scan triggers real nl80211 radio work and could transiently affect latency, but this command did not invoke the session's network takeover path.
- **Physical status-after-scan acceptance: PASS (2026-11-16).** A repeat `wifictl status` after the 46-BSS nl80211 scan still returned `ok`, same configured network ID `0`, `android.hardware.wifi.supplicant.ISupplicant/default`, connected `wlan0` station, **-55 dBm**, TX/RX PHY rates **234/433 Mb/s**, **5805 MHz** (5 GHz, channel 161). The MAC matched the prior read (omitted from this public document). Signal and link rates are dynamic; there is no evidence from the two snapshots of a dropped association. One UI's network configuration was not changed.
- **Pending Wi-Fi:** separately plan reversible guest `en0` association, DHCP, IP frames, disconnect and restoration. **Do not run `wifictl join` or `restore`** while still in passive checks; those can change One UI Wi-Fi state. No XNU boot/hot-switch.

## On-device haptic capability inventory (2026-11-16)

The owner ran `adb shell su -c 'getevent -pl'` and filtered **only for haptic/vibrator input devices**, without opening the active OneiOS haptic backend or uploading any effects.

- Samsung `sec_vibrator_inputff` device was found at **`/dev/input/event7`** on this S26 Ultra, exposing `FF_PERIODIC`, `FF_SINE`, `FF_CUSTOM`, `FF_GAIN` and no advertised `FF_RUMBLE`. This **matches the FF_PERIODIC + FF_SINE capabilities required by the Samsung OneiOS haptic backend**, so the capability/device-inventory check is **PASS**.
- The older S24 example documents `/dev/input/event8`. The backend does **not** hardcode event8: `br_haptic_find` enumerates `/dev/input/event*`, checks `EVIOCGBIT(EV_FF)` for periodic/sine and prefers a device named `sec_vibrator_inputff`. So event7 requires **no source change** or Samsung-specific path override.
- **Active vibration is still untested.** `br_haptic_open_backend` sends `FF_GAIN=10000` on opening the Samsung evdev backend; `br_haptic_vib` then uses `EVIOCSFF` followed by an `EV_FF` play event. Normal close queues a stop and closes its descriptor, but the interaction with One UI's own haptic gain/state has not been verified on SM8850.
- **First actual vibration test FAIL (2026-11-16).** The user deployed the checksum-verified CI `bridgectl` from the same runtime and ran `ONEIOS_HW_BACKEND_HAPTICS=samsung bridgectl haptic 80 150` while One UI stayed up. The correct `/dev/input/event7` Samsung driver opened and reported sine effects, period 4..10 ms and level 40..255, but the worker logged **`request (80 ms, intensity 150) failed: Invalid argument`** and **`0 effects played, 1 failed`**. The CLI nevertheless printed **`ok`** because the earlier command ignored worker errors. **Haptic capability PASS; actual haptic playback FAIL.** Do not assume an effect was audible/felt.
- **Mapped request:** `br_haptic_map(150,0)` produces a **6 ms** period and level **72**; the Samsung backend builds an `FF_PERIODIC`/`FF_SINE` effect with 80 ms length. The old error line does not distinguish failed `EVIOCSFF` upload from failed `EV_FF` play, and a failed `FF_GAIN=10000` write is also silently ignored by the original code. The S24 CS40L26A-specific assumptions must **not** be presumed identical on S26 merely because the capability flags match. Driver support for an advertised waveform is not proof that every parameter/format is accepted. Independent Linux implementations exhibit `EINVAL` for device-specific invalid effects; the correct S26 driver source has not yet been verified.
- **Diagnostic-only source fix:** `haptic.c` now reports **`effect upload` versus `effect play`** failures with duration, intensity, period, mapped magnitude and effect id; it also logs gain-setting failure. `bridgectl` now waits for the asynchronous worker and returns nonzero when no effect played or any request failed. New **`bridgectl haptic-inspect`** opens input devices `O_RDONLY` and uses read-only `EVIOCGBIT`, `EVIOCGNAME`, `EVIOCGEFFECTS` ioctls to print the feature bits and maximum effect slots, **without issuing gain, upload, playback, or input grab**. Fake-motor tests cover `-EINVAL` at upload and at play, including the observed `80 ms / 150` mapping. The active waveform/driver behavior is unchanged.
- **Passive diagnostic acceptance: PASS (2026-11-16).** The owner downloaded CI run `43821960082`'s Android ARM64 runtime and verified `oneios-runtime-0.0.0-ci-arm64.tar.zst: OK` with SHA256. After deploying only its updated `bridgectl`, `bridgectl haptic-inspect` returned `/dev/input/event7 "sec_vibrator_inputff" FF_PERIODIC=1 FF_SINE=1 FF_CUSTOM=1 FF_GAIN=1 max_effects=4`, then `haptic-inspect: ok (O_RDONLY; no FF_GAIN, EVIOCSFF, or playback)` and `ok`. Device identity/capability inspection works without changing actuator state. `EVIOCGEFFECTS=4` reports **maximum supported effect slots**, not how many slots are currently free; it does **not** prove the original `EINVAL` resulted from capacity exhaustion or that One UI is not holding any effect slots.
- **Kernel observation:** filtered `dmesg` contained repeated `cs40l26 53-0040` suspend/resume and IRQ enable/disable entries but **no explicit FF upload/play rejection** in the captured tail. This confirms that the active actuator binds to a driver with CS40L26 identity at I2C `53-0040`, but those power-state logs do not explain the earlier `EINVAL`. The `readlink -f /sys/class/input/event7/device` command printed no path; this should be investigated separately rather than interpreted as a confirmed absent device.
- **Reference-source caveat:** Linux input FF documentation defines `EVIOCSFF` as an effect upload, with a separate playback event. Public *non-Samsung* CS40L26 driver code supports `FF_PERIODIC`/`FF_SINE`, but the exact Android 17 Samsung S26 Ultra driver contract has not yet been verified. Source references: `https://android.googlesource.com/kernel/common/+/refs/heads/android-mainline/Documentation/input/ff.rst` and `https://android.googlesource.com/kernel/google-modules/amplifiers/+/aba0cca145df5e55d1c5c4294a90bc382b0d2ec1/cs40l26/cs40l26.c`.
- **Next controlled diagnostic:** Host CI for instrumented haptics succeeded, and a checksum-verified new `bridgectl` is present on the phone. An **optional single identical** `ONEIOS_HW_BACKEND_HAPTICS=samsung bridgectl haptic 80 150` test, while One UI is running and the owner can observe/recover, will now distinguish **`effect upload failed`** vs **`effect play failed`** and return a nonzero shell status on error. Do not raise amplitude, run `haptic-pattern`, change waves to `FF_CUSTOM`, or try Bluetooth/hot-switch. This is an active effect attempt, not a read-only check. The correct fix remains undecided until the failing stage is recorded.



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