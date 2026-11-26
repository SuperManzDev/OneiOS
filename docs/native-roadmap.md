# OneiOS roadmap — native path (Galaxy S25 Ultra)

Owner goal (2026-11-25): make the native Gunyah VM feel like a daily driver—close the iOS/Android power gap, remove stutter (especially Safari), then fill in missing hardware. This is the native path's single current to-do list. Update status in the same commit as the change.

Legend: ✅ shipped to this branch · 🔄 in progress · ⏳ todo · 📱 needs an owner-run phone session.

## 1. Current state (log-grounded)

- **Hosted path:** full boot and Metal on Adreno, up to 120 fps. Syscall trapping and the old thermal path made it unsuitable for the daily-driver target.
- **Native path:** XNU at EL1 in a Gunyah shared-RAM crosvm VM; SpringBoard, six/eight vCPUs, 4 GiB tested, GPU, audio, Wi-Fi, touch/keys/keyboard and takeover work.
- ✅ Take 21 proved 1440×3120 @3x and a physical 120 Hz panel for the whole session.
- ✅ Take 22 proved native brightness and AIDL haptics; its guest did not open Bluetooth HCI.
- ✅ Take 23 proved sound, AIDL Bluetooth, Wi-Fi, touch and brightness on the phone. Bluetooth transferred 5,691 guest messages with no malformed bridge frames.
- **Take 23 display finding:** the image was accidentally built with `--gpu`, not `--gpu-scanout`. `vm-boot` therefore selected the pool-copy fallback. The panel remained under One UI's 10–30 Hz policy and partial-row copies caused visible flicker. This was a presentation-path regression, not a new Metal failure: the guest logged no rejected GPU operations.
- 🔄 `kernel-take23-fixed.img` is rebuilt with `--gpu-scanout`; its disk recipe contains `scanout 1440 3120 120` and its boot arguments contain `oneios-virtio-scanout=1`. It needs one short phone confirmation.
- 🔄 Haptics remain open, with a concrete Take 23 build regression fixed. Take 23 carried 14 guest haptic messages as one long envelope—13 duration effects and two stops—with **zero discrete impacts**. Unlike Take 22, its image omitted `oneios-hid-buttons-builtin=1` and `oneios-button-haptics=1`; the kernel logged `ONEIOS BUTTON HAPTICS: off`. The fixed image restores both arguments (and Take 22's `oneios-data-wait=1800`). The next session must confirm named interactions: keyboard tap, selection tick and lock/unlock.

## 2. The two findings behind “50 fps and hot”

1. **The original stutter was the panel, not the GPU.** A panel capped at 60 Hz while iOS produced about 100 fps dropped frames unevenly. The crosvm scanout helper must own the Surface and hold physical mode 0 at 120 Hz. The pool-copy fallback is not an equivalent path.
2. **Most of the power gap was avoidable clock forcing.** With floors, perf-core idle fell to 0% and GPU clocks stayed high for the same workload. CPU/GPU floors remain off by default. The next structural power win is replacing virtio-gpu polling with an interrupt or adaptive spin-to-WFI behavior.

## 3. Priorities

### P0 — visible regressions and hardware acceptance

- 🔄📱 **Take 23 fixed scanout:** run `build_take23_fixed.sh`, then `run_take23.sh`. Acceptance: display log says it asked SurfaceFlinger for native mode 0, panel policy 120–120 Hz, crosvm display service present, and no pool-copy lines or visible flicker.
- 🔄📱 **Haptics:** the fixed image restores Take 22's built-in side-key and stand-in button-haptics arguments. Reproduce keyboard, selection, lock and notification patterns separately. Acceptance: boot logs `ONEIOS BUTTON HAPTICS: on`; short interactions produce `native impact ... source N ms, classified after 5 ms silence`; no 110 ms HEAVY_CLICK for a 5–10 ms tap; sustained patterns remain VIB/STOP.
- ✅ **Bluetooth AIDL:** the S25 HAL is `android.hardware.bluetooth.IBluetoothHci/default`. The bridge now closes a stale previous AIDL client before initialize and retries `ALREADY_INITIALIZED` once. Physical preflight passed open, HCI Reset, Read BD_ADDR and close; Android Bluetooth recovered afterward. Never print controller identity data.
- ✅ **Brightness:** bounded test wrote 119 with ceiling 2953 and restored One UI's saved level.
- ✅ **Native haptic capability:** TICK/CLICK/HEAVY_CLICK read-only inventory passed on vibrator HAL v2.

### P1 — performance and upstream strategy

- ⏳📱 **Kill virtio-gpu busy polling.** Give the PCI function a real interrupt, or use adaptive spin→WFI. Measure frame round-trip, idle residency and thermals on Gunyah.
- ✅ **Upstream strategy:** cherry-pick isolated wins; do not wholesale-merge the rewritten Rust runtime.
- ✅ **GPU memory fixes already in the active add-on:** command-pool cutoff, idle image/buffer cleanup, Turnip BO-cache trim, reduced keep-room and command-buffer-boundary submit splitting.
- ⏳ **Auto-brightness:** port the sensor-hub/front-ALS path. First determine whether root can reach the S25 sensor service over QRTR; `als_rear` evdev is not a real front-ALS source.
- ⏳ **GPU map failures:** operation 4 is `DROIDVM_VIRTGPU_MAP`; `0xe00002c8` is `kIOReturnVMError` from `createMappingInTask`, not a submit refusal. Keep tracking under memory pressure.

### P2 — polish and inexpensive features

- ⏳ **Display fit:** correct safe-area/inset identity for rounded corners, Dynamic Island, home indicator and keyboard globe key.
- ⏳ **Shader/pipeline cache persistence:** keep the Metal/Turnip cache on persistent Data and restore the full prime list.
- ⏳📱 **S Pen → Apple Pencil:** run the existing pen implementation on the phone.
- ⏳📱 **GPS:** adapt the iOS 27.2 GNSS feeder to 24A446 and inject one fixed coordinate.
- ⏳ **16 KiB XNU pages:** first read raw `ID_AA64MMFR0_EL1` at EL1 and prove Gunyah stage 2 accepts a 16 KiB stage-1 guest. Do not start the pmap conversion before that.
- ⏳📱 **RAM experiments:** step reserve sizing while preserving One UI recovery; avoid the known 16 MiB metal2vulkan block-size hang. Balloon/free-page reporting is later work.
- ⏳ **6–7.5 GiB guest RAM:** reserve up front only after proving the way back still has enough host memory. The current validated configuration is 4 GiB with 2,032 required 2 MiB pages.

### P3 — larger work

- ⏳ **Safari RAM:** investigate per-process 32 MiB shared-cache privatization, likely tied to 4 KiB nesting/shared-region behavior.
- ⏳📱 **Mobile data:** test uplink-only first; SIM/calls are a separate project.
- ⏳📱 **Screen recording:** writeback-surface path currently returns `kIOReturnBusy`; find that first before adding a hardware encoder.
- ⏳ **Camera:** near-term target is a virtio raw-frame bridge into AVFoundation; computational photography/ANE work is deferred.

## 4. Decisions that stay decided

- Native display acceptance uses crosvm GPU scanout, not the pool-copy fallback.
- Clock floors are off by default.
- Upstream work is cherry-picked, not merged wholesale.
- Camera is a virtio bridge first; ANE/ISP reverse engineering is later.
- No Knox or EFS writes. Apple credentials, proofs, tokens and machine identity data are never printed or committed.

## 5. Current branch checkpoints

| Area | State |
|---|---|
| Native brightness, battery, Wi-Fi, audio, touch | ✅ phone-proven |
| Bluetooth AIDL HCI | ✅ phone-proven in Take 23 |
| 120 Hz crosvm scanout | ✅ Take 22; 🔄 fixed Take 23 image needs confirmation |
| Haptics | 🔄 transport works; interaction classification needs focused phone test |
| 4 GiB / six vCPUs | ✅ boots; performance needs fixed-scanout rerun |
| GPU interrupt/adaptive idle | ⏳ |
| ALS/sensor hub | ⏳ |
| GPS, pen, mobile data, camera, recording | ⏳ |
