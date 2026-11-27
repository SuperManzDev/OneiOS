# OneiOS on the Galaxy S25 Ultra — Master Handoff, Build Record, Roadmap, and Repository Map

**Last updated:** 2026-11-27

**Target:** Samsung Galaxy S25 Ultra, `SM-S938U`, board/device `pa3q`, Snapdragon 8 Elite / SM8750, Adreno 830

**Guest:** iOS 27.0.1, build `24A446`, iPhone18,2/V54 identity

**Active checkout:** `/mnt/oneios-build/workspace/src/OneiOS-native-integration`

**Active branch:** `port/native-hardware-integration`

**Purpose of this file:** one self-contained record of what was attempted, what worked physically, what failed, why it failed, the repositories and code used, current artifacts, exact recovery rules, and the work still open.

> This supersedes the operational status sections of `~/oneios-s25u-knowledge-transfer.md` but does not delete that document. The older file remains the detailed record for Takes 13–17 and early display/touch discoveries. Its recorded SHA-256 was `85e6a0492b43417615e3fa518f443ed6d605a796b8cd499576c7cfee78cda0ef`.

---

## 1. Executive summary

OneiOS now boots Apple’s native XNU kernel at EL1 inside a **Gunyah shared-RAM VM** launched by Droid-VM’s crosvm on the S25 Ultra. The native path is not XNU running as a Linux process; it is a real VM with native XNU, prelinked Apple kexts, virtio devices, and a phone-hosted Metal/Turnip graphics stack.

### Physically proven on the phone

- Full boot to SpringBoard and ordinary app use.
- 1440×3120 framebuffer, @3x identity, correct 1:1 panel fit.
- Physical panel held at 120 Hz for an entire session when the **crosvm GPU scanout** path is used.
- Touch and side-key forwarding.
- Real Wi-Fi through `wifictl` and the phone’s radio.
- Speaker audio through crosvm virtio-snd/AAudio.
- AIDL Bluetooth HCI ownership, controller initialization, scanning traffic, and Android recovery.
- Native brightness control with bounded panel range and restoration of One UI’s prior level.
- Battery state bridge.
- Takeover and reliable return to One UI.
- Metal on Adreno through NativeMetal → metal2vulkan → Turnip → drm2kgsl/KGSL.

### Working but not yet daily-driver quality

- GPU rendering works, but memory pressure, occasional fence latency, and thermal load remain.
- Haptic messages and discrete impact classification work in the guest, but Samsung’s physically effective SemHaptic/framework path is not yet bridged after `system_server` stops.
- Audio transport works; a major low-volume cause was Android host media volume being left at 1/15. Host volume save/full-scale/restore is implemented but still needs a clean full-size phone confirmation.
- A safe KernelSU module and Android control app exist. They are inert at boot and can prepare memory manually. Phone-local VM launch is implemented and staged locally but needs one end-to-end owner run with the latest packages.
- A decrypted-only IPA installer and signer are host-built, trusted into a new native image, and packaged in its boot add-on. That image is staged but has not booted on the phone. Dynamic trust-inbox folding, phone-side Image regeneration/restart handling, and LaunchServices finalization are **not implemented end to end**, so newly imported apps cannot yet become runnable.

### Not complete

- Camera preview.
- GPS.
- Mobile data/SIM/calls.
- Screen recording/writeback.
- S Pen as Apple Pencil on the physical phone.
- Auto-brightness from the front sensor hub.
- Apple Account/iCloud on iOS 27.
- 16 KiB XNU pages; the raw EL1 feature result later showed this guest/SoC path is 4 KiB-only for the relevant configuration.

---

## 2. Non-negotiable safety and privacy rules

1. **Never write Knox or EFS.** No script or module may modify raw EFS partitions, Knox state, fuses, or bootloader security state. One reviewed Bluetooth path reads `/mnt/vendor/efs/bluetooth/bt_addr` read-only because the HAL/controller address must match; that value must never be printed or committed.
2. **Temporary KernelSU root is expected to disappear after reboot.** After a reboot the owner must restore temporary root and grant the ADB shell or the control app an unrestricted KernelSU profile.
3. **Never reboot without owner approval.** Reboots used during development were explicitly approved. Accepting the phone-side Launch warning also authorizes the takeover script’s last-resort reboot only if it cannot restore Android/One UI normally; the warning must disclose that possibility.
4. **Always arm the way back before stopping One UI.** The takeover script starts its failsafe and traps before stopping zygote/system_server.
5. **Volume Down held for 10 seconds stops the VM only when the input forwarder and `pmic_resin` monitor were both armed.** Otherwise use the PC stop command or timers. The normal VM limit is 900 seconds; a separate framework failsafe starts One UI at 1,200 seconds, and may wait up to 90 seconds for system_server before the disclosed last-resort reboot.
6. **No forced module removal.** Module release is refused while a VM or Gunyah shared parcel is active.
7. **No automatic RAM reservation or VM launch at boot.** KernelSU module boot hooks are deliberately absent.
8. **Do not print or commit Apple credentials, OTPs, anisette proofs, account tokens, private machine identities, or controller identity data.** Existing private Apple Account configuration was preserved without printing its contents.
9. **IPA installer boundary:** install only apps the owner is authorized to run. The tool refuses FairPlay-encrypted slices and never removes DRM.
10. **Thermal stop:** a diagnostic run was stopped when CPU cluster sensors reached roughly 103–105 °C, GPU about 85 °C, board about 58 °C, and battery about 44.8 °C. Do not repeat undersized-memory stress runs.

---

## 3. Hardware and host environment

### Phone

| Item | Value |
|---|---|
| Model | Samsung Galaxy S25 Ultra, `SM-S938U` |
| Product/device | `pa3qsqw` / `pa3q` |
| ADB identity | intentionally omitted from the committed handoff |
| SoC | Qualcomm SM8750, Snapdragon 8 Elite |
| GPU | Adreno 830, KGSL host path |
| Panel | 1440×3120, physical mode 0 at 120 Hz |
| Android | Android 17-era build in these sessions; exact fingerprint is target-gated by the KernelSU package |
| Kernel | `6.6.127-android15-8-p87b1ce1-abogkiS938USQUDDZJB-4k` |
| Root | temporary KernelSU, `u:r:ksu:s0` when fully granted |
| Hypervisor | Qualcomm Gunyah, `/dev/gunyah` |

### Build machine

- Linux x86_64.
- Workspace root: `/mnt/oneios-build/workspace/src`.
- Active worktree: `/mnt/oneios-build/workspace/src/OneiOS-native-integration`.
- Old/main checkout with historical user work: `/mnt/oneios-build/workspace/src/OneiOS`.
- Approved temp location: `/tmp/tungcode`.
- Android NDK used: `~/Android/Sdk/ndk/28.2.13676358`.
- iOS SDK used by cross-builds: iPhoneOS 27.1 SDK under the Swift SDK artifact bundle.
- Guest stubs: `out/ios27-tbd`, generated from the 24A446 dyld cache.

---

## 4. Architecture

### 4.1 Boot chain

```text
Android / KernelSU
  └─ Droid-VM crosvm
      └─ Gunyah pseudo-unprotected shared-RAM VM
          └─ xnuloader-crosvm Linux Image wrapper
              └─ Darwin boot_args + Apple device tree
                  └─ native XNU at EL1
                      └─ XnuPrelink MH_FILESET kernel collection
                          └─ iOS 27.0.1 userspace and SpringBoard
```

The loader Image is not a normal Android boot image. `xnuloader-crosvm` places the prelinked native collection, boot arguments, ADT, trust caches, optional boot add-on, and sidecars inside a Linux Image-shaped payload crosvm can load.

### 4.2 Native versus hosted kernel

This distinction caused a major early failure and must never be blurred:

- **Hosted kernel:** modified XNU intended to run as a Linux process under `xnuhost`.
- **Native kernel:** XNU running at EL1 in Gunyah/crosvm.

The native VM must use a kernel built by `kernel/build-native.sh`, then prelinked with `kernel/prelink/build.sh`. Using a hosted artifact in Gunyah caused the Take 15 alignment fault (`0x96000021`) almost immediately after handoff.

### 4.3 Shared RAM and host modules

The native VM uses Droid-VM’s pseudo-unprotected mode. Required support:

| Component | Role |
|---|---|
| `gh_hugepage_reserve` | Reserves 2 MiB pages before/for VM RAM |
| `gunyah_host_share` | Shares the VM RAM memfd with Gunyah and publishes `/dev/gunyah_share` |
| `udmabuf_gki.ko` | Replaces fragmented-host udmabuf allocations with kvmalloc-capable path; raises limits |
| `gunyah_halt_poll_gki.6.6` | Short adaptive vCPU halt polling, configured at 200,000 ns in proven sessions |

A 4096 MiB VM needs **2,032 available 2 MiB pages** under the current allowance. Late preparation often fails because Android has already fragmented memory. This motivated the on-demand KernelSU control package and the future early-reservation design.

### 4.4 Storage

The VM uses five virtio-blk devices:

1. OS volume, read-only, hash verified.
2. SystemOS volume, read-only, hash verified.
3. App cryptex image, read-only.
4. Data seed, read-only.
5. Persistent writable Data store at `/data/local/tmp/oneios-vmboot/data.img`.

The disk list is emitted beside the Image as `IMAGE.disks`. Phone launch verifies the kernel Image SHA-256, checks the read-only disk sizes, requires the helper/crosvm files, and relies on guest-side hash lists for OS/SystemOS/App integrity. It does not cryptographically validate every staged helper.

### 4.5 GPU path

```text
iOS Metal client
  → NativeAdrenoMetal plug-in
  → metal2vulkan translator
  → Turnip Vulkan driver
  → NativeAdreno guest kernel user client
  → virtio-gpu DRM native context
  → crosvm/virglrenderer
  → drm2kgsl
  → Android KGSL / Adreno 830
```

Important active fixes in the Metal add-on:

- spare command-pool cutoff,
- idle image/buffer cleanup,
- Turnip BO-cache trim,
- reduced pool keep-room,
- full-rate display links,
- command-buffer-boundary submit splitting,
- memory and command-pool telemetry.

### 4.6 Display paths

There are two paths, and only one is accepted for normal use:

1. **crosvm GPU scanout — correct path**
   - Image built with `--gpu-scanout`.
   - `.disks` contains `scanout 1440 3120 120`.
   - Boot args contain `oneios-virtio-scanout=1`.
   - Android helper asks SurfaceFlinger for physical mode 0 held at 120–120 Hz.
   - No CPU pool-copy fallback.

2. **pool-copy fallback — diagnostic only**
   - Selected accidentally when Take 23 was built with `--gpu` instead of `--gpu-scanout`.
   - Panel remained under One UI’s 10–30 Hz policy.
   - Partial-row copies produced the owner-visible flicker.

The original Take 23 flicker was therefore a packaging/presentation regression, not a new Metal command failure.

### 4.7 Input

- Samsung touchscreen is discovered by input device name and `INPUT_PROP_DIRECT`.
- Side-key devices are forwarded separately and never mistaken for touch.
- After Android system_server stops, Samsung TSP wake sequence is `2,0` then `2,1` through `/sys/class/sec/tsp/enabled`.
- Volume Down held for 10 seconds is the emergency VM stop when the touch socket and `pmic_resin` monitor both started; PC `vm-boot --stop` and the two timers remain the fallback.

### 4.8 Hardware bridge

A framed protocol runs over crosvm’s second ns16550 serial port. Channels currently cover:

- battery,
- haptics,
- Bluetooth HCI,
- brightness,
- thermal protocol support where a host source is served.

The phone-side server is `runtime/bridges/build/android/bridgectl`. Wi-Fi uses a separate tap bridge (`wifictl`).

---

## 5. Repository and dependency map

### 5.1 Main project checkouts

| Path/repository | Use |
|---|---|
| `OneiOS-native-integration` | Active native hardware integration worktree and current commits |
| `OneiOS` | Older/main checkout, Takes 13–21 history and preserved unrelated user work |
| OneOS/OneiOS Android app submodule | Root/session manager and historical app UI; not overwritten during this work |

Repository state when this handoff was finalized:

- branch `port/native-hardware-integration`; integration-code baseline immediately before this documentation commit: `a5611301`;
- no configured upstream for this work branch;
- `app/` submodule is uninitialized in this checkout, gitlink `81438a53202825faa110183f10faea7141307e8d`;
- source changes and this handoff are intentionally committed;
- phone/build logs remain untracked evidence and must be sanitized before any commit because raw logs can contain Wi-Fi/Bluetooth/controller or LAN identifiers.

### 5.2 Four pinned native kernel sources

From `kernel/pins.sh`:

| Repository | Pin | Use |
|---|---:|---|
| UniXNU | `1446f449b6ac98b8a386752f520e402866ddbe99` | reference source/pin; `kernel/pins.sh` explicitly says the active build does not build from this tree |
| Droid-VM-UniXNU | `62d0b11773ec17561fb409ed6e4c5adfa29ff45b` | actual native build base, board, patches, GPU/audio inputs |
| UniXNU-droidvm | `f9121cdc629d64cbd148980cdeef1f069c7f0068` | iPadOS/iOS guest packages and virtio drivers |
| xnu-linux-toolchain | `909990e4ec2c6cda30c20cdb23219946918c7fd6` | Linux XNU toolchain bootstrap |

### 5.3 Main reference/build repositories

| Repository | Pin | Contribution |
|---|---:|---|
| Droid-VM/virglrenderer | `7e17c8e814e963912fc1e4e737137a31b5efa027` | drm2kgsl host renderer |
| Droid-VM/crosvm | `f82707d2cbd8d1666cd11428157ddb45c4d61282` | source reference; not the exact phone binary |
| DroidVM-Prebuilt-Root crosvm package | `f7c6ce856d5df95bc9cc04599b390ad2435bdc5a` (reported source `0f2db62ae`) | exact phone crosvm package fetched with `scripts/droidvm/fetch-crosvm.sh /mnt/oneios-build/dl/droidvm-prebuilt hackintosh`; file hashes are pinned by that script |
| Droid-VM/NativeAdreno | `627836d507ef08037fff2f9b12e9274ba7b2a4f3` | guest GPU user client and ABI |
| UniXNU/AdrenoPM4 | `282606b528b2595bc4274ea7490b62b323f35a5f` | PM4 encoder support |
| UniXNU/ArmSmmu4K | `83969cd874503e65997ff7639f7dcf3d0a92cd17` | SMMU types used by GPU sources |
| Droid-VM/NativeMetal | `b0d6a07a3da3e23486593aff235904177e94a3ff` | Metal frontend and Turnip glue |
| steelbrain/metal2vulkan | `624bab3918de5b978e1101517abca2b4fa426565` | Metal-to-Vulkan translator |
| Droid-VM/mesa | `cecc0e96d7193c72356a31abb0cc589e4fc0415e` | Turnip source |
| Droid-VM/NativeMobileDisplay | `ed08bd437b93772d4c41d74633af12446918f18a` | IOMobileFramebuffer backend |
| UniXNU/ShengHidInput | `24f1f3fbed6feff42e1ca02e2ce386a5c3c36da8` | touch/buttons/HID |
| Apple IOHIDFamily | `e31e90a24c27ad6cbe981650d12a1a4124065b03` | public ABI reference |
| Droid-VM/ownervisor | `65fc2c7037f494a238f4263f528a68ff21e87406` | guest owner tools/reference |
| Droid-VM/applebox | `6e3b47149e0e460636e7a95bba9d48e12170a925` | guest utilities reference |
| Droid-VM/droidvm-meta-repo | `a18ec72392721e34fbe98216a8be1ee3acc41ddc` | SM8650/iPadOS 27 research record |
| Droid-VM/macOS-DroidVM | `77f761485bfc2c2e379ee49eb5c6a25c19b98739` | compatibility and GPU research |
| Droid-VM/gunyah_host_mod | `44e8ae5a088a3a35290a5cacb205640cff187b99` | host-share and udmabuf modules |
| UniXNU/XnuPrelink | `9b950f2e104da77f494e8f2735c6f044f0e4db4d` | native fileset matcher/emitter |

### 5.4 Huge-page and signing source pins

| Project | Pin | Use |
|---|---:|---|
| Droid-VM/gh-hugepage-reserve | `00c8a0322aa6fb25593e00352af2764be5317d7e` | 2 MiB pool module |
| ProcursusTeam/ldid | `af86971ae72ec3ed3d0a699107c4e882324c941b` | in-process IPA signer |
| libimobiledevice/libplist | `cf5897a71ea412ea2aeb1e2f6b5ea74d4fabfd8c` | static ldid dependency |
| OpenSSL | `67b5686b4419b4cb8caa502711c41815f5279751` | static ldid crypto dependency |

### 5.5 Other repositories and ports studied

- `jesvi-eulah/Jesvi-iOS`, branch `feat/s20-apple-account-gps`, commit `e0388e9b76549857b2fd46ea5a849eafe673aca3`: Apple Account/GPS research. A local OneiOS-specific anisette adaptation is preserved in the older checkout and must not be overwritten or printed.
- Musicalista/OneiOS-A56, referenced change `26a2276`: stable AIDL Bluetooth direction.
- coreycusick/ios-guacamole, referenced commit `a3c8ef9`: GNSS feed idea.
- qiumochen/UniXNU-zizhan, referenced display-link work `42629e5`.
- ios-garnet, referenced snapshot-format work `f6276db`.
- YumikoHayamato/OneiOS, referenced write-only display buffers `cdb297bb`.
- iOSPlus10 patches: Bluetooth, Wi-Fi, camera, audio, and device-interface references.
- AltStore source: `altstoreio/AltStore`, inspected local commit `56854e6`; requires Xcode/Swift project graph.
- Feather source: `claration/feather`, inspected local commit `1535466`; GPL-3.0, used as installer UX/compatibility reference only.
- Additional read-only ports are pinned in `scripts/setup/get-references.sh` (`UniXNU-shennong`, `UniXNU-houji`, HakiOS SM8650, `vphone-linux`, Portsmith-iOS, and others).

---

## 6. Chronology of phone takes and root-cause work

### Takes 13 and earlier

- Established native Gunyah/crosvm path.
- Identified clock-floor heat problem and physical refresh mismatch.
- Decided GPU/CPU floors must be off by default.
- Detailed evidence remains in the older knowledge-transfer file.

### Take 14

- Reference native build and baseline for later comparisons.
- Screen-recording investigation later showed ReplayKit countdown succeeds but writeback surface creation returns `kIOReturnBusy`; replayd receives no frames.

### Take 15

- Immediate alignment fault from using the wrong kernel type.
- Root cause: hosted XNU artifact was placed inside Gunyah.
- Permanent rule: native image must use the native prelinked kernel collection.

### Take 16

- Successful native boot and performance verification.
- Demonstrated that the VM and GPU path were fundamentally viable.

### Take 17

- SpringBoard unlocked.
- Display zoom/crop and touch failures isolated.
- Led to full 1440×3120 packaging, @3x QuartzCore fixups, and Samsung TSP wake work.

### Takes 18–20

- Iterated display fit, TSP, shared RAM, recovery, and native device integration.
- See the older handoff and `phone-vmboot-take18/` through `take20/` in the older checkout.

### Take 21

- First strong acceptance session for the current display path.
- 1440×3120, @3x, touch working.
- Physical display stayed 120 Hz for 100% of the session.

### Take 22

- Added/validated native brightness, AIDL haptics, and Bluetooth channel advertisement.
- Display stayed at 120 Hz for 100% of 187 five-second samples.
- Bridge summary: 956 guest messages; 38 haptic, 916 brightness, 1 battery, 0 Bluetooth.
- Bluetooth Settings UI was cosmetic because guest bluetoothd never opened HCI.
- Haptics physically worked, but many short full-scale bursts were mapped to Samsung’s long 110 ms HEAVY_CLICK and felt delayed/wrong.
- Black-screen incident was not missing iOS frames: SurfaceFlinger screenshot showed iOS rendering while the Android panel was OFF/DOZE. Forcing panel power normal fixed it.

### Take 23 initial

What worked physically:

- Bluetooth, including real HCI traffic.
- Sound.
- Wi-Fi.
- Touch.
- Brightness.

What regressed:

- Severe visual flicker.
- Haptics appeared missing or inconsistent.
- UI/GPU still showed latency under load.

Root causes:

1. **Flicker:** image was built with `--gpu`, not `--gpu-scanout`; vm-boot selected pool-copy fallback and the panel stayed under low-rate One UI policy.
2. **Haptics:** Take 23 packaging had omitted Take 22’s `oneios-hid-buttons-builtin=1` and `oneios-button-haptics=1`; those arguments were restored. Later testing proved a deeper Samsung takeover issue remained.
3. **Audio level:** guest reached 128/128 and 0 dB, but Android host media volume was 1/15. UID 2000 AAudio was attempted and failed stream preparation, so the proven root AAudio path is retained while host volume is saved, raised, and restored.

The session was stopped cleanly and One UI returned.

### Corrected Take 23 scanout attempt

- Rebuilt with `--gpu-scanout`, `scanout 1440 3120 120`, and restored haptic boot arguments.
- A 4096 MiB launch was refused safely before crosvm because only 1,764/2,032 pool pages were available.
- One UI returned automatically; no VM was launched.

### Refill and diagnostic RAM attempts

- Late huge-page refill after Android had fragmented RAM repeatedly failed to reach 2,032 pages.
- 3040 MiB and 2880 MiB diagnostic images were generated.
- A 2880 MiB session ran with correct scanout.
- A 2240 MiB audio/haptics diagnostic was intentionally stopped because of extreme heat and memory pressure. This size must not be used again.

### Haptic isolation results

- Guest now produces 15–20 ms discrete impacts and bridge Binder calls report success.
- During takeover, AIDL effects were physically silent.
- A raw 300 ms Samsung evdev sine was also silent during takeover.
- After One UI returned, Android VibratorManager at maximum amplitude was clearly felt.
- Direct AIDL under One UI produced only a short blip for a nominal 700 ms request.
- Conclusion: motor and guest classifier work; Samsung’s effective SemHaptic/framework-managed path is missing once system_server is stopped. Do not guess FF_CUSTOM firmware indices.

### Bluetooth acceptance

- S25 exposes `android.hardware.bluetooth.IBluetoothHci/default` (AIDL), not the old HIDL-only route.
- Bridge now closes a stale previous AIDL client before initialize and retries `ALREADY_INITIALIZED` once.
- Physical preflight passed open → HCI Reset → Read BD_ADDR → close, with Android Bluetooth recovery.
- Take 23 carried thousands of Bluetooth messages without malformed bridge frames.

### Brightness acceptance

- Physical node is `panel0-backlight`, max 12,750; safe manual ceiling is 2,953.
- Bounded test wrote 119 and restored One UI’s prior value.
- Session bridge saves/restores panel level and TSP/ALS state.

---

## 7. Current feature matrix

| Feature | State | Evidence / remaining issue |
|---|---|---|
| Native XNU boot | ✅ | boots SpringBoard in Gunyah/crosvm |
| 1440×3120 @3x | ✅ | Take 21/22 and fixed scanout builds |
| 120 Hz physical panel | ✅ with scanout | Take 22 full-session proof; never use pool-copy fallback for acceptance |
| Touch | ✅ | TSP wake + input forwarder, no dropped HID reports in accepted runs |
| Side keys | ✅ | forwarded; button-haptics stand-in publishes expected service |
| Wi-Fi | ✅ | real wlan0/tap path, traffic and recovery |
| Bluetooth | ✅ | AIDL HCI ownership and guest traffic proven |
| Audio transport | ✅ | virtio-snd/AAudio, no guest period drops in accepted path |
| Audio loudness | 🔄 | host volume 1/15 identified; save/full-scale/restore implemented, clean rerun pending |
| Brightness | ✅ | bounded native control and restoration |
| Battery | ✅ | host values visible in guest |
| Haptics transport | ✅ | guest PCM and impacts reach bridge |
| Haptics physical fidelity | ⏳ | Samsung SemHaptic path required during takeover |
| Metal/GPU | ✅ experimental | works; memory/fence/thermal optimization remains |
| Camera | ⏳ | black preview; virtio/raw-frame bridge planned |
| GPS | ⏳ | GNSS feeder port planned |
| S Pen | ⏳📱 | implementation exists, physical acceptance not run |
| Mobile data | ⏳ | first test should be uplink-only; SIM/calls separate project |
| Screen recording | ⏳ | writeback-surface `kIOReturnBusy` |
| Apple Account/iCloud | ⏳ | private helper and iOS 27 adaptation not complete |
| IPA installation | 🔄 | installer/signer/add-on/image built; phone trust-inbox fold and LaunchServices test pending |

---

## 8. Major root causes and lessons

1. **Wrong kernel type causes immediate native crash.** Hosted and native artifacts are not interchangeable.
2. **Physical refresh can dominate perceived smoothness.** iOS drawing near 100 fps on a 60/30 Hz panel still feels bad; clocks were not the main issue.
3. **Clock floors were harmful.** They drove perf-core idle to zero and increased SoC temperatures without fixing the panel mismatch.
4. **Pool-copy is not equivalent to scanout.** It caused visible flicker and low-rate panel policy.
5. **A valid rendered frame can coexist with a black physical display.** Android panel power state must be forced normal during takeover.
6. **Root UID does not imply correct Android audio behavior.** Host media volume remains relevant; alternate UID 2000 was rejected by AAudio on this phone.
7. **Binder success does not prove physical haptics.** Samsung framework state and vendor SemHaptic behavior matter.
8. **Late huge-page allocation is unreliable.** Stopping One UI after fragmentation may still not recover enough 2 MiB pages.
9. **GPU op 4 is mapping, not submission.** `0xe00002c8` is `kIOReturnVMError` from `createMappingInTask` in `DROIDVM_VIRTGPU_MAP`.
10. **Kernel compiler changes emit weak local `.ptrauth` aliases.** HID and display generators now ignore those linker-local aliases while retaining the real global method bindings.
11. **SPTM/TXM means no casual runtime trust-cache bypass.** New ad-hoc app CDHashes must be present in a boot trust cache.

---

## 9. Current branch commits from this work

Newest first:

| Commit | Purpose |
|---|---|
| `a5611301` | disclose recovery timers and make appinst add-on clean-build default use the appinst kernel tree |
| `1a074b7a` | document Feather and AltStore trust models |
| `d21c95ca` | place trusted appinst payload in native boot add-on |
| `cf86cc0c` | decrypted IPA and injected-component preflight |
| `3c816198` | package reviewed KernelSU target while phone is offline |
| `be00ffae` | build pinned arm64e signer and appinst add-on |
| `85fb3e5a` | stage once and launch OneiOS from phone |
| `828e1ee1` | keep root actions observed across rotation |
| `5a7b354c` | refresh pinned shortcut metadata |
| `a667ab2e` | transactional failures and truthful status |
| `e207c3b4` | harden app states/accessibility |
| `80070d67` | add screen-safe control app and shortcut |
| `3531b7b8` | inert on-demand KernelSU module |
| `87e848d9` | preserve host audio volume; release Android vibration state |
| `f2658b24` | attempted UID-2000 AAudio route; later reverted in launcher because stream open failed |
| `a21bab3f` | parameterized RAM diagnostic images |
| `c94e5f82` | restore Take 22 button-haptic arguments |
| `3c1b5d70` | restore GPU scanout for flicker-free Take 23 |
| `63272210` | AIDL Bluetooth ownership before initialize |
| `cc822bc1` | Take 23 recipe |
| `7eaf766c` | ignore weak display ptrauth aliases |
| `90d6ec2c` | ignore weak HID ptrauth aliases |
| `1a38396e` | Bluetooth H4/POSIX transport and haptic classifier changes |
| `57b1bb75` | force panel on at takeover start |
| `0e5381da` | Take 22 recipe |
| `fed9fec9` | safe hardware opt-in gates |
| `ff1b1eec` | native VM serial Bluetooth opt-in |
| `0591bb1d` | AIDL Bluetooth HCI client integration |
| `77a78860` | native serial brightness bridge |
| `acdaa0e9` | native AIDL haptic preset opt-in |
| `37a4c6c8` | combined native bridge host tests |

Historical native base commits include `1c1f21c5`, `5573557b`, `273ed5e5`, `c04882e4`, `68fd3f4d`, `cdb297bb`, and the native bridge merges described in the older handoff.

---

## 10. Build and artifact record

### 10.1 Current appinst-capable native kernel

| Artifact | SHA-256 |
|---|---|
| `out/kernel/current-native-take23-appinst/xnu.macho` | `14a73c07bd06c1a25e47fc139e10ab2597e09ed76d084d51592941ab1eafd26f` |
| `out/kernel/native-take23-appinst-24A446/kernel.kc` | `ca083f979f50a1430864dc0449288ebe502ed5f5f5f5314331f0d3776570a7c6` |

Prelink audit: 2,711 paired targets, 93 intentional function stubs, 9 unpaired data targets, **0 failures**. The prelink record still says **runtime qualification: pending**: this appinst kernel/Image is host-built and staged, not phone-booted.

### 10.2 Current appinst-capable VM image

| Artifact | SHA-256 |
|---|---|
| `out/crosvm/kernel-take23-fixed-appinst.img` | `14527bf58b9fb525d2ce63a81d5539a34e3db5c88049c1e014c0bf850a794539` |
| ADT sidecar | `14b01ec6d1f7b537074a8422ff2f3da8f41f8f2284275c076b84f35ddc7ee424` |
| disk recipe | `00605030e7ca8c54c77f3558c53dda63f0b9072d3cdd0ec0a24c7403c4f395ee` |

Configuration:

- 4096 MiB RAM.
- six vCPUs pinned to cores 2–7.
- 1440×3120 @3x.
- 120 Hz.
- shared RAM.
- crosvm GPU scanout.
- GPU, audio, Wi-Fi, touch, serial bridge.
- native haptic opt-in and button-haptics arguments.
- appinst boot add-on and appinst/signer owner trust.

### 10.3 KernelSU and Android control artifacts

At the last local package build:

| Artifact | SHA-256 |
|---|---|
| `out/ksu-module/oneios-native-on-demand-pa3q-538.zip` | `f74338670545f56a6152237d4257186d59ae285c2aea0bd7ad508740c4a293f4` |
| `out/ksu-module/app/oneios-native-control.apk` | `5ef0b3c025d4d226d7336eb1703a17cbbdee094d2ef65f55535425fc501ba525` |

Do not select a package by filename/version or mtime alone. Verify its adjacent SHA-256, `build-manifest.txt`, exact target/kernel/fingerprint, source commit, and packaged module hashes. The APK update identity is the certificate SHA-256 `0c33f5811b7648f077885ea4c95073198c460530955543851cf418997082ec28`. Its private debug keystore lives only under ignored `out/ksu-module/app/debug.keystore`; never commit it, but preserve it securely or future APKs cannot update the installed app in place.

### 10.4 App installer artifacts

| Artifact | SHA-256 / CDHash |
|---|---|
| `out/userland/appinst-final-v2/appinst` | SHA-256 `7337b928d526aa57072e8789ba9213b2779836701275db0dbe24d2500435a4f5`; CDHash `d45d6c6f185c060afb5299ddbf291ba638cb40b6` |
| `libunixnu-ldid.dylib` | SHA-256 `9d39e47e1abc1f9e078fbf45b4a4bedec3bf51c958e188eefe6053d169a77d85`; CDHash `ef35e92dacb4631440e05fec1bd07d68d4f691c8` |
| `appinst.gpuaddon` | `e80396adee32f1708fd2fabb877d34e43f8f837f237d7a6c9a5e84948d10904a` |

Both Mach-Os are arm64e, min iOS 27.0, ad-hoc signed, and use 4096-byte signature pages.

---

## 11. Exact build workflow

### Native kernel

```bash
cd /mnt/oneios-build/workspace/src/OneiOS-native-integration
NATIVE_OUT=take23-appinst kernel/build-native.sh
```

Expected native Mach-O:

```text
out/kernel/current-native-take23-appinst/xnu.macho
```

### Prelink

```bash
KERNEL=out/kernel/current-native-take23-appinst/xnu.macho \
OUT=out/kernel/native-take23-appinst-24A446 \
kernel/prelink/build.sh
```

Require `audit ... 0 failures` before image generation.

### Appinst and signer

```bash
userland/appinst/build.sh out/userland/appinst-final-v2
```

This builds the pinned in-process ldid signer and audits both binaries.

### Boot add-on and appinst owner trust image-set view

```bash
userland/appinst/build-addon.sh \
  out/userland/appinst-final-v2 \
  /mnt/oneios-build/workspace/src/OneiOS/out/image/24A446/kernel.kc.gpuaddon \
  out/userland/appinst-final-v2/appinst.gpuaddon

userland/appinst/prepare-image-set.py \
  /mnt/oneios-build/workspace/src/OneiOS/out/image/24A446 \
  out/userland/appinst-final-v2 \
  out/image/appinst-24A446-v2
```

### VM Image

```bash
KERNEL=out/kernel/native-take23-appinst-24A446/kernel.kc \
IMAGE_SET=out/image/appinst-24A446-v2 \
GPU_ADDON=out/userland/appinst-final-v2/appinst.gpuaddon \
OUT=out/crosvm/kernel-take23-fixed-appinst.img \
./build_take23_fixed.sh
```

Verify `.disks` contains:

```text
scanout 1440 3120 120
```

### Stage without launching

```bash
./stage_take23_on_phone.sh
```

This uses `phone vm-boot --stage-only`. It pushes the Image, helpers, recovery scripts, and crosvm package but does not stop One UI or start a VM.

### Prepare from the phone

The **OneiOS Memory** app or root shell runs:

```bash
oneios-native prepare 4096
```

This keeps One UI running and performs one bounded mode-1 acquire. It is transactional: a failed new preparation rolls back the modules/pool; incomplete cleanup is marked partial and must be released.

### Launch from the phone

After status says prepared and launcher staged:

```bash
oneios-native launch
```

The Home shortcut opens a confirmation first. Launch uses the staged recovery script, stops One UI, and starts the VM. The panel may be black until the first iOS frame. Volume Down works only if its key monitor armed; otherwise use PC stop or timers. The normal VM stop is 15 minutes, the independent framework failsafe is 20 minutes, and inability to restore system_server can trigger the explicitly disclosed last-resort reboot.

### Release without launching

```bash
oneios-native release
```

Release refuses active VMs, active parcels, foreign modules, or incomplete unloads.

---

## 12. KernelSU module and Android control app

### Module safety model

Location: `scripts/hugepage/ksu-module/`.

- No `post-fs-data.sh`.
- No `service.sh`.
- No `boot-completed.sh`.
- KernelSU Manager action is read-only status.
- Package is bound to model, board, exact kernel release, Android fingerprint, reviewed Gunyah offsets, and module SHA-256 hashes.
- No Knox/EFS paths.
- No forced unload.
- Prepare and Launch are separate explicit commands.
- Root work has inner and outer timeouts.
- New preparation failures roll back; incomplete state remains owned and visible as partial.

### Android app

Package: `dev.oneios.nativeprep`.

UI states include:

- Not prepared.
- Preparing.
- Prepared / ready to launch.
- Prepared but launcher not staged.
- Partially prepared — release required.
- Root access required.
- Unsupported build.
- Not enough contiguous memory.
- Timeout with a follow-up status read.

The app supports dark theme, large fonts, insets, rotation, live-region status announcements, and a distinct pinned shortcut icon. Active root work is preserved through declared orientation/screen-size configuration changes.

---

## 13. IPA installer work

### Intended scope

`appinst` installs **already-decrypted** IPAs the owner is authorized to run. It does not strip FairPlay, bypass DRM, or supply Apple signing credentials.

### Pipeline

1. Safe ZIP extraction; rejects absolute paths, `..`, and zip-slip.
2. Locate exactly one `Payload/*.app`.
3. Walk every Mach-O in the app bundle.
4. Thin fat binaries to arm64e, falling back to arm64 where needed.
5. Refuse any `LC_ENCRYPTION_INFO(_64)` with nonzero `cryptid`.
6. Sign in-process using pinned `libunixnu-ldid.dylib` (`-Hsha256 -M -S`).
7. Compute SHA-256 CodeDirectory CDHashes.
8. Write trust-inbox JSON.
9. Stage bundle and data container.
10. Request guest restart.
11. On next trusted boot, finalize LaunchServices registration.

### Feather and AltStore

- Feather provides a polished import/sign/install UX and multiple certificate-based install routes.
- AltStore provides Apple-ID/developer-certificate signing and AltServer installation/refresh.
- Neither unchanged route solves OneiOS’s SPTM/TXM trust requirement.
- OneiOS does not need seven-day certificate refresh when the boot trust cache carries the app’s ad-hoc CDHash.
- Their code was inspected as a reference; no Feather/AltStore code was copied into appinst.

### Subway Surf IPA preflight

File found locally:

```text
/mnt/oneios-build/workspace/Subway_Surf.ipa
```

SHA-256:

```text
235e36edd939e5acc0dcbbde9f19af27b32d8e7cf50f096e61d3131901d15f61
```

Results:

- Bundle ID `com.kiloo.subwaysurfers`.
- 42 Mach-O files inspected.
- Every detected slice had `cryptid=0`; it is decrypted input.
- Archive includes injected CydiaSubstrate and iGameGod-related paths. Treat it as untrusted modified software. It was not executed or installed during this work.

### AltStore input

- Source cloned and inspected at commit `56854e6`.
- The Windows IPA path was not visible to the Linux build machine.
- To inspect it, copy it to a path visible here, for example `/mnt/oneios-build/workspace/AltStore.ipa`, then run:

```bash
userland/appinst/host/ipa-check.py /mnt/oneios-build/workspace/AltStore.ipa
```

### Remaining IPA blocker

The initial appinst and signer are host-built, present in the add-on, and listed in the current Image’s owner trust data; that Image has been staged but not phone-booted, and the binaries have never executed in the guest. Dynamic installation is **not integrated**: `appinst_request_restart()` only writes a guest marker, no Android/host watcher consumes it, `fold-trust-inbox.py` remains a host reference, the phone does not rebuild/restage the Image with new app hashes, and LaunchServices finalization is unverified. A newly imported app therefore cannot yet become runnable.

---

## 14. Validation suites

Latest relevant host results:

| Suite | Result |
|---|---:|
| loader | 198 checks, 0 failed |
| display | 173 checks, 0 failed |
| display modes | 21 checks, 0 failed |
| bridge stream | 72 checks, 0 failed |
| bridges | 362 checks, 0 failed |
| Wi-Fi | 162 checks, 0 failed |
| brightness | 113 checks, 0 failed |
| battery | 83 checks, 0 failed |
| thermal | 145 checks, 0 failed |
| audio helper | 20,434 checks, 0 failed |
| actuator classifier | 19 checks, 0 failed |
| oneios-phone | 68 tests passed |
| oneios-kernel | 7 tests passed |
| KernelSU module | 12 checks, 0 failed |
| Android control APK | 9 checks, 0 failed |
| IPA checker | 6 checks, 0 failed |
| current prelink | 0 audit failures; `take23-appinst-prelink.log`, host-built appinst kernel, runtime qualification pending |

Provenance note: bridge counts are from `take23-bridges-tests-bt-retry.log`; appinst prelink is from `take23-appinst-prelink.log`; loader/display/phone/kernel suite counts were rerun from their named project commands during Take 23 work. Results span several commits, so each row is host-test evidence for that subsystem, not a single monolithic release certification.

---

## 15. Last observed phone/staging state on 2026-11-26

The newest appinst-capable Image was successfully staged once while ADB was connected:

```text
/data/local/tmp/oneios-vmboot/kernel.img
SHA-256 14527bf58b9fb525d2ce63a81d5539a34e3db5c88049c1e014c0bf850a794539
```

`stage-only` explicitly reported that no VM started and One UI was not stopped.

Immediately afterward, a manual `prepare 4096` attempt encountered an ADB disconnect and left the command lock directory while no support modules or ownership marker were present. Before the next prepare:

1. reconnect ADB and restore unrestricted temporary KernelSU root;
2. exclude every running `oneios-native prepare`, `launch`, or `release` process, staged `boot.sh`, OneiOS crosvm process, active crosvm control socket, active VM count, and outstanding Gunyah parcel;
3. confirm no OneiOS support module is loaded and ownership says no marker;
4. remove only the expected-empty lock with `rmdir /data/local/tmp/oneios-native-on-demand.lock`—never recursive deletion;
5. rerun status, then prepare from the app.

At the time this document was written, ADB had disappeared again, so the latest launch-capable KernelSU ZIP/APK and the appinst-enabled staged Image have not received a final full phone launch.

---

## 16. Roadmap

### P0 — next owner session

1. Reconnect phone; restore full KernelSU shell permission.
2. Update/install the latest KernelSU ZIP and Android control APK if the phone still has an older version.
3. Clear the verified stale command lock only if no process/modules/ownership marker exists.
4. Open OneiOS Memory; verify status says launcher staged and no automatic action.
5. Press Prepare 4 GiB. If it fails, confirm rollback/partial reporting; do not force launch.
6. If ready, use Launch OneiOS and accept the black-screen/last-resort-reboot warning. Keep the phone cool, ADB attached, and the first confirmation deliberately short; do not leave an unmonitored 15-minute run until an automatic thermal stop is implemented.
7. Predefine abort limits below the prior dangerous diagnostic and stop immediately on rapid CPU/GPU rise. Confirm:
   - physical panel 120 Hz and no flicker,
   - speaker loudness with host volume handling,
   - Bluetooth/Wi-Fi/touch/brightness still work,
   - thermals remain below the prior dangerous diagnostic,
   - One UI returns by Volume Down or normal stop.
8. Verify boot log writes and starts appinst/appinst-serve from `/private/var/oneios`.
9. Use a small open-source/decrypted IPA for first installer validation, not the modified Subway archive.

### P1 — performance and power

- Replace virtio-gpu busy polling with a real interrupt or adaptive spin→WFI.
- Continue GPU mapping/fence analysis.
- Add automatic thermal stop to phone-local launch.
- Prove 4 GiB preparation soon after boot; later consider separately approved early reservation.
- Keep clock floors off by default.

### P2 — hardware polish

- Samsung SemHaptic/framework bridge.
- Front ALS/sensor-hub auto-brightness.
- Safe-area and keyboard inset tuning.
- Full shader/pipeline cache persistence and full primelist.
- S Pen phone acceptance.
- GPS fixed-coordinate proof.

### P3 — major features

- Complete appinst trust-inbox fold and LaunchServices registration.
- Camera raw-frame bridge.
- ReplayKit writeback fix.
- Mobile-data uplink test, later SIM/calls.
- Safari shared-cache/private-memory investigation.
- Apple Account/iCloud adaptation without exposing secrets.

---

## 17. Key files

### Native integration

- `kernel/build-native.sh`
- `kernel/prelink/build.sh`
- `kernel/patches/oneios-hosted/series`
- `kernel/patches/oneios-hosted/0084-native-bluetoothd-hci-transport.patch`
- `kernel/patches/oneios-hosted/0085-native-addon-oneios-prefix.patch`
- `runtime/loader/tools/xnuloader-crosvm.c`
- `tools/oneios-tool/crates/oneios-phone/src/vmboot.rs`
- `runtime/display/java/OneiOSDisplay.java`
- `runtime/bridges/devices/bridge_stream.c`
- `runtime/bridges/bt/hci_hidl.c`
- `drivers/haptics/oneios_actuator.h`
- `drivers/haptics/oneios_haptic.c`

### Phone-local launch

- `build_take23_fixed.sh`
- `stage_take23_on_phone.sh`
- `run_take23.sh`
- `scripts/hugepage/ksu-module/oneios-native`
- `scripts/hugepage/ksu-module/app/`
- `docs/native-roadmap.md`

### IPA installer

- `userland/appinst/appinst.m`
- `userland/appinst/macho.c`
- `userland/appinst/unzip.c`
- `userland/appinst/serve.m`
- `userland/appinst/register.m`
- `userland/appinst/build.sh`
- `userland/appinst/build-signer.sh`
- `userland/appinst/build-addon.sh`
- `userland/appinst/prepare-image-set.py`
- `userland/appinst/host/ipa-check.py`
- `userland/appinst/host/fold-trust-inbox.py`
- `docs/appinst.md`

### Logs

- `phone-vmboot-take22/`
- `phone-vmboot-take23/`
- `phone-vmboot-take23-fixed-2880/`
- `phone-vmboot-take23-audio-haptic-2240/`
- `take23-host.log`
- `take23-fixed-2880-host.log`
- `take23-appinst-kernel-build.log`
- `take23-appinst-prelink.log`
- `take23-fixed-appinst-image-build.log`

---

## 18. Recovery checklist

If the screen is black during a VM session:

1. Wait briefly for the first iOS frame if the VM just started, while watching thermals/logs.
2. If the input/key monitor was armed, hold Volume Down for 10 seconds. If it was not armed, skip directly to the PC stop command or timers.
3. From a connected computer:

```bash
tools/oneios-tool/target/release/oneios-tool phone vm-boot --stop
```

4. Read `/data/local/tmp/oneios-vmboot/run.txt` and `ui.log`.
5. Verify zygote and system_server are running.
6. Do not manually start random Android services unless the recovery script failed and the logs identify why.
7. Timer semantics: crosvm normally stops at 900 seconds; the independent takeover failsafe starts One UI at 1,200 seconds; recovery then waits up to 90 seconds for system_server. If recovery still fails, the launch-approved last resort may reboot the phone.

If memory preparation fails:

- no VM should start;
- a new preparation should roll back every module it loaded;
- if rollback cannot finish, ownership must read `partial` and Release must be used;
- never delete ownership state while modules remain loaded;
- never force-rmmod a module with an active VM/parcel.

---

## 19. Final current assessment

The project has crossed the “does native iOS boot?” milestone and is now in systems-integration territory. The most valuable unique result is the native Gunyah path with real Metal, 120 Hz scanout, touch, Wi-Fi, Bluetooth, audio, brightness, and takeover recovery. The immediate quality blockers are thermal/power behavior, Samsung haptics, GPU latency/memory, and reliable early huge-page availability. The phone-local launcher and control app substantially reduce PC dependence. The decrypted-only IPA components and initial trust are host-built into an unqualified image, but dynamic trust folding, phone-side Image regeneration/restart handling, and LaunchServices registration are still unimplemented; no claim of end-to-end IPA installation is justified yet.

---

## Appendix A — Attempt ledger: what was tried and what was learned

This table intentionally includes unsuccessful or superseded work so future sessions do not repeat it.

| Attempt | Result | Keep / reject |
|---|---|---|
| Hosted XNU process path | Full boot, Metal and high frame rates, but trap overhead and thermal throttling made it the wrong daily-driver direction | Keep as diagnostic/reference path; native is primary |
| Native Gunyah with hosted kernel artifact | Immediate alignment fault | Permanently reject; native kernel only |
| 1080×2340 / @2x packaging | Zoom/crop and wrong identity for S25 panel | Reject for final S25 profile |
| 1440×3120 / @3x packaging | Correct panel geometry | Keep |
| Android panel left under prior One UI policy | Guest could draw near 100 fps while physical panel ran 60/30 Hz | Reject; takeover must hold native 120 Hz mode |
| CPU/GPU clock floors | Reduced idle, raised heat, did not solve panel mismatch | Off by default; A/B flags only |
| Pool-copy display fallback | Visible flicker and low-rate policy in initial Take 23 | Diagnostic only; final acceptance requires crosvm GPU scanout |
| Host-side Power-button panel blanking | Caused/complicated black panel while valid iOS frames existed | Removed; iOS owns lock/wake, takeover forces panel normal |
| Brightness using unbounded `max_brightness` | Unsafe because 12,750 is not manual range | Reject; ceiling 2,953 on this phone |
| HIDL-only Bluetooth | No S25 service; phone is AIDL | Replaced with stable AIDL client |
| AIDL initialize without taking ownership | `ALREADY_INITIALIZED`/I/O failures while Android client state lingered | Close old AIDL owner first; retry once |
| Bluetooth Settings toggle before bluetoothd transport patch | Cosmetic; guest sent no HCI open | Fixed with transport-encoding 10 + `BT_HCI_TRANSPORT=H4:POSIX` |
| Native haptic heavy mapping by peak only | Short taps became Samsung 110 ms HEAVY_CLICK | Reworked classifier; still blocked by SemHaptic takeover state |
| Direct AIDL haptics after system_server stop | Binder calls succeeded but motor often silent | Not physical acceptance; needs Samsung SemHaptic bridge |
| Raw evdev sine during takeover | Accepted/logged but physically silent | Not a viable fallback on this S25 state |
| Android VibratorManager under One UI | Clearly felt | Proves actuator hardware works |
| AAudio backend as UID 2000 | `VIRTIO_SND_R_PCM_PREPARE` / failed to open stream | Reject on this phone |
| Root AAudio with Android media at 1/15 | Transport worked but speaker was very quiet | Save host volume, set 15/15 during VM, restore after One UI |
| 4096 MiB late pool refill | Frequently short of 2,032 pages after Android fragmentation | Prepare early; fail closed; no force launch |
| 3040/2880 MiB diagnostics | Useful to prove scanout with smaller pool | Diagnostic only, not final RAM target |
| 2240 MiB diagnostic | Extreme CPU/GPU heat and memory pressure | Permanently reject |
| `--gpu` instead of `--gpu-scanout` | Selected wrong presentation path | Packaging error; fixed build script checks `scanout` line |
| UID/root assumptions for module control | Restricted KernelSU profiles had UID 0 but no effective capabilities/kallsyms | Require unrestricted profile and explicit checks |
| Automatic KernelSU boot reservation | Owner explicitly did not want random black screen/automatic action | Not implemented; module remains inert |
| Phone-local launcher without warning | Too risky because One UI stops and first frame may be delayed | Launch now has explicit black-screen/timer/reboot warning |
| Runtime AMFI patch / legacy trust load for IPA | Wrong for SPTM/TXM; legacy loader unavailable, IMG4 path needs Apple keys | Reject |
| Certificate-based AltStore/Feather unchanged | Adds provisioning/certificate/refresh dependencies and still does not solve OneiOS TXM trust | Reference UX only |
| External `ldid` executable dependency | Extra guest process and missing artifact | Replaced by pinned in-process arm64e signer dylib |
| Modified Subway Surf IPA | Decrypted, but contains injected CydiaSubstrate/iGameGod components | Do not use as first trust/installer acceptance IPA |

---

## Appendix B — Status vocabulary

To avoid overstating results, use these labels consistently:

- **source-only:** code exists but has not built.
- **host-built:** binary/image built on Linux/macOS and passed format checks.
- **host-tested:** unit/integration tests passed off-phone.
- **phone-staged:** copied to the phone without starting it.
- **phone-booted:** the exact artifact reached XNU/iOS on the phone.
- **physically accepted:** the owner observed the hardware behavior.
- **runtime-qualified:** exact artifact completed the intended phone scenario and clean return.
- **historical evidence:** result came from an older artifact and may not prove current HEAD.

Current appinst image status is **host-built + host-tested + phone-staged**, not phone-booted or runtime-qualified.

---

## Appendix C — Documents and precedence

Read in this order:

1. `docs/ONEIOS-S25U-MASTER-HANDOFF.md` — authoritative current integration/build/recovery status.
2. `docs/native-roadmap.md` — short current priorities.
3. `docs/native.md` — native architecture and accumulated technical detail.
4. `docs/hardware.md` — hardware bridge research and measurements.
5. `docs/gpu.md` — GPU architecture, telemetry and memory work.
6. `docs/appinst.md` — installer design and explicit incomplete integration.
7. `~/oneios-s25u-knowledge-transfer.md` — historical Takes 13–17.
8. Top-level `README.md`, `docs/SM8750_PORT.md`, `docs/newtodo.md` — useful historical context, but some statements predate current Take 23 integration.

When documents conflict, the master handoff wins for current status, but source code and artifact logs win over all prose.

---

## Appendix D — Artifact and evidence hygiene

- Raw phone logs can contain Wi-Fi addresses, Bluetooth/controller identifiers, LAN information and app/process data. They remain untracked evidence and must be redacted before any commit or publication.
- The Android control APK keystore is sensitive ignored output. Preserve it privately for update continuity; never commit it.
- Apple IPSW contents, image sets, kernel collections containing Apple kexts, guest disks and private account helpers are not distributable project source artifacts.
- Publish only source, patches, reproducible build instructions, and legally redistributable runtime components.
- Every release-like artifact should have a machine-readable manifest containing source HEAD, dependency pins, target fingerprint, size, SHA-256, code-sign CDHashes where relevant, and phone qualification state.

---

## Appendix E — Immediate reconnect checklist

When ADB returns:

```bash
adb devices -l
adb shell su -c id
adb shell su -c 'head -n 1 /proc/kallsyms'
```

Then:

1. verify exact model/device/kernel/fingerprint;
2. inspect `oneios-native` processes, crosvm, sockets, module list, active VM/parcel counts and ownership marker;
3. remove a stale lock with `rmdir` only after all activity is excluded;
4. install/verify the newest hash-checked KernelSU ZIP and APK if needed;
5. rerun `stage_take23_on_phone.sh` if the staged Image hash differs;
6. run read-only status;
7. prepare 4 GiB while the phone is cool;
8. launch only with ADB attached and live thermal monitoring;
9. stop early for the first current-image run;
10. collect and redact logs before committing any evidence.
