# OneiOS roadmap — native path (Galaxy S25 Ultra)

Owner goal (2026-11-25): make the native Gunyah VM feel like a daily driver—close the iOS/Android power gap, remove stutter (especially Safari), then fill in missing hardware. This is the native path's single current to-do list. Update status in the same commit as the change.

Legend: ✅ shipped to this branch · 🔄 in progress · ⏳ todo · 📱 needs an owner-run phone session.

The verified review of Yumiko Take 22 and the supplied 762-line stability report is in [`STABILITY-INVESTIGATION-REVIEW.md`](STABILITY-INVESTIGATION-REVIEW.md). Its main correction is that native renderer fixes must be built into the pinned crosvm/`libvirglrenderer.so` artifacts, not only added to the local xnuhost patch series.

## 1. Current state (log-grounded)

- **Hosted path:** full boot and Metal on Adreno, up to 120 fps. Syscall trapping and the old thermal path made it unsuitable for the daily-driver target.
- **Native path:** XNU at EL1 in a Gunyah shared-RAM crosvm VM; SpringBoard, six/eight vCPUs, 4 GiB tested, GPU, audio, Wi-Fi, touch/keys/keyboard and takeover work.
- ✅ Take 21 proved 1440×3120 @3x and a physical 120 Hz panel for the whole session.
- ✅ Take 22 proved native brightness and AIDL haptics; its guest did not open Bluetooth HCI.
- ✅ Take 23 proved sound, AIDL Bluetooth, Wi-Fi, touch and brightness on the phone. Bluetooth transferred 5,691 guest messages with no malformed bridge frames.
- **Take 23 display finding:** the image was accidentally built with `--gpu`, not `--gpu-scanout`. `vm-boot` therefore selected the pool-copy fallback. The panel remained under One UI's 10–30 Hz policy and partial-row copies caused visible flicker. This was a presentation-path regression, not a new Metal failure: the guest logged no rejected GPU operations.
- ✅ The corrected `--gpu-scanout` path is phone-proven with the 2880 MiB diagnostic: crosvm display service present, physical mode 0 held at 120 Hz for 100% of 114 five-second checks, no pool-copy flicker. The final 4096 MiB appinst-capable Image is host-built/staged but still needs a short thermal-monitored phone boot.
- 🔄 Haptics remain open, with a concrete Take 23 build regression fixed. Take 23 carried 14 guest haptic messages as one long envelope—13 duration effects and two stops—with **zero discrete impacts**. Unlike Take 22, its image omitted `oneios-hid-buttons-builtin=1` and `oneios-button-haptics=1`; the kernel logged `ONEIOS BUTTON HAPTICS: off`. The fixed image restores both arguments (and Take 22's `oneios-data-wait=1800`). The next session must confirm named interactions: keyboard tap, selection tick and lock/unlock.

## 2. The two findings behind “50 fps and hot”

1. **The original stutter was the panel, not the GPU.** A panel capped at 60 Hz while iOS produced about 100 fps dropped frames unevenly. The crosvm scanout helper must own the Surface and hold physical mode 0 at 120 Hz. The pool-copy fallback is not an equivalent path.
2. **Most of the power gap was avoidable clock forcing.** With floors, perf-core idle fell to 0% and GPU clocks stayed high for the same workload. CPU/GPU floors remain off by default. The next structural power win is replacing virtio-gpu polling with an interrupt or adaptive spin-to-WFI behavior.

## 3. Priorities

### P0 — visible regressions and hardware acceptance

- 🔄📱 **Take 23 fixed scanout:** run `build_take23_fixed.sh`, then `run_take23.sh`. Acceptance: display log says it asked SurfaceFlinger for native mode 0, panel policy 120–120 Hz, crosvm display service present, and no pool-copy lines or visible flicker.
- 🔄📱 **Haptics:** the fixed image restores Take 22's built-in side-key and stand-in button-haptics arguments. The guest now emits 15–20 ms discrete impacts and the bridge reports successful HAL effects, but none are felt during takeover; even a raw 300 ms evdev sine is silent. After restoring One UI, Android's own 500 ms maximum-amplitude VibratorManager command is felt, while direct AIDL `on(700)` produces only a short blip. Pre-takeover `vibrator_manager cancel` did not make direct HAL effects physical after the framework stopped. The S25 needs its Samsung SemHaptic/framework vendor path bridged; do not guess `FF_CUSTOM` firmware indices. Acceptance: physical keyboard, selection and lock impacts, not merely successful Binder replies.
- ✅ **Bluetooth AIDL:** the S25 HAL is `android.hardware.bluetooth.IBluetoothHci/default`. The bridge now closes a stale previous AIDL client before initialize and retries `ALREADY_INITIALIZED` once. Physical preflight passed open, HCI Reset, Read BD_ADDR and close; Android Bluetooth recovered afterward. Never print controller identity data.
- ✅ **Brightness:** bounded test wrote 119 with ceiling 2953 and restored One UI's saved level.
- 🔄📱 **Speaker level:** Take 23's guest reached volume 128/128 (0 dB digital gain) and virtio-snd dropped no periods, but Android's host media stream was only 1/15. Running the AAudio helper as uid 2000 was rejected (`VIRTIO_SND_R_PCM_PREPARE ... Failed to open stream`), so keep the proven root AAudio stream. `vm-boot` now saves Android's media level, sets it to 15 for the VM, and restores it after One UI is back.
- 🔄 **Independent thermal/recovery guard:** the temporary 2240 MiB diagnostic was stopped when cluster sensors reached about 103–105 °C, GPU about 85 °C, board 58 °C and battery 44.8 °C. Do not use that undersized guest again. Before another extended run, add a host process independent of crosvm/the guest bridge that reads sensors and the escape chord directly, applies configurable stop/cooldown hysteresis, terminates a hung VM and restores display/audio/brightness/One UI idempotently. The existing thermal bridge is telemetry and affinity policy, not this fail-independent guard.
- ✅ **Native haptic capability:** TICK/CLICK/HEAVY_CLICK read-only inventory passed on vibrator HAL v2.

### P1 — performance and upstream strategy

- ⏳📱 **Kill virtio-gpu busy polling.** First add bounded adaptive spin/backoff with the current polling behavior as rollback. Then trace crosvm's existing PCI INTx mapping and prove an interrupt counter before designing new Gunyah irqfd plumbing. Measure frame round-trip, idle residency and thermals; avoid host CPUs 6–7 in the interim diagnostic profile.
- ✅ **Upstream strategy:** cherry-pick isolated wins; do not wholesale-merge the rewritten Rust runtime or Yumiko Take 22.
- ⏳ **Native renderer correctness:** reconstruct the exact source for the pinned phone `libvirglrenderer.so`, then evaluate WAIT_FENCE commits `a5e12ac`/`f7da477` and both zero-resource-ID guards `25c4e7f9`/`78c4b86e`. Changes under local `runtime/xnuhost/drm2kgsl/patches/` alone do not affect native crosvm sessions.
- ⏳ **Warm shader cache:** adapt Take 22's persistent per-client cache from `14027c71` to the exact NativeMetal pin before trying render-server pipeline prewarm. Measure the same cold/warm scene and bound cache growth.
- ✅ **GPU memory fixes already in the active add-on:** command-pool cutoff, idle image/buffer cleanup, Turnip BO-cache trim, reduced keep-room and command-buffer-boundary submit splitting. Verify these hooks execute in the staged add-on before adding overlapping Take 22 lifetime policy.
- ⏳ **Auto-brightness:** port the sensor-hub/front-ALS path. First determine whether root can reach the S25 sensor service over QRTR; `als_rear` evdev is not a real front-ALS source.
- ⏳ **GPU map failures:** operation 4 is `DROIDVM_VIRTGPU_MAP`; `0xe00002c8` is `kIOReturnVMError` from `createMappingInTask`, not a submit refusal. Keep tracking under memory pressure.

### P2 — polish and inexpensive features

- ⏳ **Display fit:** correct safe-area/inset identity for rounded corners, Dynamic Island, home indicator and keyboard globe key.
- ⏳ **Shader/pipeline cache persistence:** keep the Metal/Turnip cache on persistent Data and restore the full prime list.
- ⏳📱 **S Pen → Apple Pencil:** run the existing pen implementation on the phone.
- ⏳📱 **GPS:** adapt the iOS 27.2 GNSS feeder to 24A446 and inject one fixed coordinate.
- ⛔ **16 KiB XNU pages:** raw EL1 `ID_AA64MMFR0_EL1` was read as `0x100010000f001021`; the relevant guest configuration exposes 4 KiB granules only. Do not begin a 16 KiB pmap conversion unless the hypervisor/CPU exposure changes.
- ⏳📱 **RAM experiments:** step reserve sizing while preserving One UI recovery; avoid the known 16 MiB metal2vulkan block-size hang. Balloon/free-page reporting is later work.
- 🔄📱 **On-demand KernelSU module, phone launch and Home shortcut:** the package remains inert after reboot. The PC stages the 4 GiB Image, crosvm, disks, helpers and recovery scripts once with `stage_take23_on_phone.sh` / `phone vm-boot --stage-only`; staging never starts the VM or stops One UI. The **OneiOS Memory** APK keeps Prepare and Launch separate. Its pinned **Launch OneiOS** shortcut opens a required warning that the screen may be black until iOS posts a frame and names the conditional 10-second Volume Down escape, 15-minute VM stop, 20-minute framework failsafe, and possible last-resort reboot. Launch is enabled only when the pool is ready and the launcher is staged; it starts the phone-resident payload, so the PC is not required afterward. Prepare remains bounded and screen-safe; Release refuses active VMs/parcels or foreign modules. It never accesses Knox/EFS. Needs one owner-run end-to-end confirmation after the current phone is reconnected.
- ⏳ **6–7.5 GiB guest RAM:** reserve up front only after proving the way back still has enough host memory. The 4 GiB image needs 2,032 available 2 MiB pages; 3040 MiB needs 1,504.

### P3 — larger work

- 🔄📱 **Decrypted IPA installer:** appinst and the pinned arm64e in-process signer build and are packaged/trusted in the host-built native Image; encrypted/FairPlay Mach-Os are refused (`cryptid=1`) and no DRM is stripped. The Image is staged but not phone-booted, and neither binary has executed in the guest. Dynamic trust-inbox folding, phone-side Image regeneration/restart handling and LaunchServices finalization are not implemented, so imported apps cannot yet run. First validation must use an owner-authorized open-source/decrypted IPA.
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
