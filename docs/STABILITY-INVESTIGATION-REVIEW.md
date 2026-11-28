# S25 Ultra stability investigation review

Status: source review complete; no phone run was performed for this review.

This document records the review of two inputs against the active native branch:

- `YumikoHayamato/OneiOS`, branch `take22`, commit `913fcf7010a5aadb00d7a63d471e58e57ef7e532`.
- The supplied 762-line report `OneiOS S25 Ultra Stability Investigation.md`, SHA-256 `cccd1c01983d9ccbc376e7d510616471253354346d668f81127fff679139b2c4`.

The comparison target was `port/native-hardware-integration` at `34a35acee6896cbe43ea651aeed3cfc4e4684764`. Raw phone logs and private identifiers were not used or published.

## Conclusions

1. Do not merge Take 22 wholesale. It contains useful cache, lifetime and diagnostic work, but also an obsolete app registrar, broad daemon pruning, an alternate huge-page module, a different Metal stack and unsafe build-specific screen-recording hooks.
2. Add an independent host thermal/recovery guard before another extended phone session. The current thermal bridge reports state and can alter affinity; it is not a fail-independent watchdog that can stop crosvm and restore One UI.
3. Fix or adapt virtio-gpu polling next. The current guest path polls roughly every 100 microseconds and is a credible idle/heat source. First trace the PCI INTx route already represented by crosvm; do not assume a new Gunyah irqfd architecture is required.
4. Apply renderer fixes to the active native artifacts. Native Gunyah sessions use the checksum-pinned crosvm and `libvirglrenderer.so`; changing only `runtime/xnuhost/drm2kgsl/patches/` does not change that path.
5. Port the persistent per-client Metal cache from Take 22 first. It is the clearest explanation for the reported cold-to-warm shader improvement and is lower risk than pipeline prewarming.
6. Do not adopt the report's proposed 16 MiB metal2vulkan default. This branch records a 16 MiB hang. Verify existing trim hooks and test 32 MiB before reconsidering 16 MiB.
7. Use a 3.0 GiB guarded diagnostic profile before retrying 4 GiB. Qualify newer CMA-backed huge-page custody separately and keep the KernelSU package inert at boot while doing so.
8. Treat screen recording and phone-local IPA trust folding as later, independent milestones. Neither is ready for a daily-driver acceptance session.

## Corrected architecture assumptions

### Active renderer path

The accepted native path is:

```text
NativeMetal -> metal2vulkan -> Turnip -> virtio-gpu ->
pinned crosvm/libvirglrenderer -> drm2kgsl -> KGSL -> Adreno 830
```

The local `runtime/xnuhost/drm2kgsl/patches/` series belongs to a different execution path. Renderer fixes must be applied to the source revision used to build the pinned native `libvirglrenderer.so`, rebuilt reproducibly, and deployed with a new checksum.

### Accepted display path

The accepted path is crosvm `--gpu-scanout`. The local Android display helper already waits for release fences in its AHardwareBuffer/pool-copy path, but that does not prove crosvm scanout exports and consumes fences correctly. Fence work for the final path belongs in the exact pinned crosvm/rutabaga/renderer source after auditing it.

### Current installer boundary

The guest half of appinst already refuses `cryptid=1`, signs decrypted input, records CDHashes, writes pending state and attempts LaunchServices finalization. The missing production links are guest-to-host export, bounded/atomic phone-side trust folding, Image regeneration, restart consumption, last-known-good rollback and runtime qualification.

## Supplied report: validated findings

| Report claim | Verdict | Branch-specific result |
|---|---|---|
| Independent thermal guard and unconditional escape are P0 | Verified gap | Implement before an extended run. Thresholds must remain configurable until phone A/B evidence establishes policy. |
| Virtio-gpu polling is a structural heat source | Verified | Add adaptive spin/backoff first, then prove PCI interrupt delivery. Preserve the current poll mode as rollback. |
| Avoid prime cores while polling remains | Reasonable mitigation | Use a diagnostic affinity that avoids CPUs 6–7; this is not a root-cause fix. |
| Port virglrenderer WAIT_FENCE fix | Strong candidate | PR commits `a5e12ac` and `f7da477f32abd952a8b6d35b8b3534cd0d513176` address `timeout=0` becoming an indefinite KGSL wait. Confirm against the active renderer base. |
| Port zero-resource-ID cleanup guards | Strong candidate | Port both upstream fixes, represented by commits `25c4e7f9` and `78c4b86e`, to the active renderer. One guard alone does not cover refused-blob cleanup. |
| Apply A830 fixes as one bundle | Partly valid | Bindless invalidation is partly present already. Audit exact guest Mesa patches; treat command-stream VBO alias lifetime as an isolated experiment. Do not port the separate IB cache blindly. |
| Local xnuhost drm2kgsl patches fix native mode | Incorrect target | The native mode uses the pinned external renderer binary. Patching only local xnuhost code has no effect on it. |
| Add map/fence instrumentation | Verified need | Correlate guest map, PCI BAR allocation, udmabuf creation/import, drm2kgsl/KGSL import and fence retirement with one resource ID. |
| Reduce pool blocks directly to 16 MiB | Rejected for now | This branch has a known 16 MiB hang and already carries several trim controls. Verify activation, classify the hang and test 32 MiB first. |
| Add explicit scanout fences in local display helper | Misplaced | Audit and modify crosvm's Android scanout backend, not the pool-copy helper, for the accepted path. |
| Use CMA-backed huge-page custody | Valid mechanism, unqualified here | Upgrade and test the module manually with `VERIFY_CMA`, release checks and reboot/disable tests. Do not combine module upgrade and automatic boot reservation. |
| Default to 3.0–3.5 GiB daily RAM | Good near-term policy | Start at 3.0 GiB for guarded diagnostics; move to 3.5 GiB only after recovery headroom is measured. |
| Screen-recording recommendations are sufficient | Not validated | The report did not resolve the actual first failure: writeback-surface creation returns `kIOReturnBusy`. Localize that IOKit call before adding an encoder. |
| IPA trust-fold plan is nearly complete | Partly true | Guest staging is substantial, but host transport, atomic fold, restart/rollback and proven registration remain missing. |

## Take 22 port matrix

### Port now, as isolated changes

| Candidate | Source | Why | Conditions |
|---|---|---|---|
| Persistent per-client Metal cache | `14027c71`, `userland/metal/patches/NativeMetal/0017-client-cache-kept-across-boots.patch` | Preserves shader/pipeline artifacts in a sandbox-writable persistent location and directly targets cold-versus-warm boot latency. | Adapt to the exact NativeMetal pin; shared cache remains first choice; reject unsafe names and `/var/empty`; log selected path; add opt-out and tests. |
| Read-only thermal/power telemetry | `608006c9` | Adds cooling state, battery/USB current and voltage, online CPUs, LMH limits, hardware frequency and GPU cap/thermal state without changing policy. | Port only reads; tolerate missing vendor nodes; do not restore cap lifting. |
| Standalone jetsam derivation utility | `e7de2a94`, `userland/jetsam_derive.py` | Provides reproducible candidate memory limits and tests. | Do not activate generated lists by default; compare against current `oneios-mem` evidence first. |

### Investigate and adapt

| Candidate | Source | Main risk or dependency |
|---|---|---|
| Turnip idle BO trim | `68f70720` Mesa patches | Must match the exact Mesa pin and have a safe idle caller. |
| NativeMetal/runtime idle trim | `68f70720` | Confirm current base APIs and ensure no in-flight object is reclaimed. |
| IOSurface command-use lifetime | `68f70720` | Requires completion-path stress tests across app surfaces, video and process death. |
| Drawable release after GPU read | `14027c71` patch 0016 | Depends on correct surface-use tracking; incorrect release can create reuse corruption. |
| Render-server pipeline retention/prewarm | `14027c71`, corrected by `6b8d8b4d` | Consumes memory, reconstructs descriptors and changes startup workload. Keep bounded and opt-in after persistent cache is proven. |
| Bounded pipeline wait | `2c31a1cf` | Can trade a hitch for a missing partial frame. Diagnostic only. |
| Zram compaction during takeover | `608006c9` | Can add CPU/latency; run only after recovery is armed and One UI is down. |
| Selected GPU stage timing | `0432e7df` | Port only counters needed to answer a current latency question; avoid restoring the large sampler. |
| Granule probe | `0432e7df` | Diagnostic-only and must be renumbered after current patches 0084/0085. |

### Already present or superseded

| Take 22 feature | Current state |
|---|---|
| Core huge-page reservation and pool checks | Present, with safer manual transactional KernelSU preparation. |
| Render queue priority | Present in the selected stack; do not duplicate it. |
| Kernel GPU blob/per-client/IOSurface accounting | Present. |
| Recovery trap, bounded runtime and One UI failsafe | Present and expanded with stage-only/on-phone launch and explicit warnings. |
| Fixed built-in application registrar | Superseded by decrypted-input appinst and user containers. |
| App add-on services namespace | Superseded by the broader reviewed `oneios/` add-on namespace. |

### Reject or defer

| Item | Reason |
|---|---|
| Whole Take 22 or whole `555b13d` Metal stack | Changes too many layers at once and conflicts with the branch's selected build path. |
| `ScreenRecord.inc` and follow-up fix | Rewrites executable shared-cache pages in backboardd using build-specific addresses; crash/code-signing and thermal risk. |
| Fixed app registrar | Obsolete system-app copy model; conflicts with appinst containers and trust flow. |
| Broad `prune1`/`prune2` launchd pruning | Can break networking/UI dependencies and create restart loops. |
| Alternate samfor12 huge-page module | Different page ownership/death behavior under the same apparent ABI; deliberately removed from this branch. |
| GPU cap lifting as normal policy | Defeats vendor limits and can increase heat without proven frame benefit. |
| Display-layer trimming | Alters compositor semantics without a reproduced current need. |
| Take 22 phone/build scripts | Assume old artifacts, patch numbering, app registrar and Image layout. |

## Required instrumentation before diagnosing `0xe00002c8`

Use one correlation ID from guest resource creation through host teardown. Aggregate successes and log failures/rate-limited slow paths.

1. Guest `DROIDVM_VIRTGPU_MAP`: resource ID, requested length/alignment, task, map result, current mapped bytes and map-entry count if available.
2. Crosvm/rutabaga: resource ID, blob size, BAR allocation offset/size and allocation failure reason.
3. udmabuf: resource ID, requested size, list count, ioctl errno and lifetime.
4. drm2kgsl/KGSL: resource ID, import handle, GPU VA size, bind result, timestamp/context and teardown reason.
5. Fence path: distinguish a zero-timeout status poll miss from a bounded wait timeout; record submit-to-retire mean/max and outstanding count.

This is needed to separate guest VA fragmentation, PCI BAR exhaustion, host udmabuf failure, KGSL import failure and delayed retirement. The existing `kIOReturnVMError` alone does not identify the layer.

## Implementation and test order

### Phase 0 — host-only safety

1. Implement `oneios-guard` as an independent root process, separate from crosvm and the guest bridge.
2. Read the hardware escape directly from evdev so it remains available without `system_server`.
3. Monitor battery, board, GPU and CPU-cluster sensors with configurable warning/stop/cooldown thresholds and hysteresis.
4. On stop: terminate the VM gracefully, force-stop only after a deadline, remove the scanout layer, restore panel/audio/brightness state, start One UI and record a sanitized reason.
5. Host-test sensor disappearance, stale PID, hung crosvm, repeated key events, partial restoration and idempotent cleanup.

### Phase 1 — observability and idle power

1. Add the five-layer GPU correlation counters.
2. Add adaptive GPU polling with bounded spin, increasing sleep/backoff and a fast return to low latency when work arrives.
3. Trace the existing PCI INTx route and add the missing guest interrupt identity only after the host counter is observable.
4. Add read-only Take 22 thermal/power fields.
5. Use a 3.0 GiB diagnostic Image and avoid host CPUs 6–7 while polling remains.

### Phase 2 — renderer correctness

1. Reconstruct the exact source provenance of the pinned native `libvirglrenderer.so`.
2. Rebuild it reproducibly with WAIT_FENCE and both zero-resource-ID guards.
3. Keep the old binary and checksum as a rollback artifact.
4. Audit exact Mesa coverage for safe A830 fixes.
5. Test command-stream VBO alias lifetime separately; do not combine it with the first renderer build.

### Phase 3 — warm-boot performance and memory

1. Port persistent per-client cache with an opt-out.
2. Record cold and warm compile/pipeline timings for the same app and scene.
3. Verify cache ownership, bounded growth and reuse after guest restart.
4. Add idle trim components individually while recording blobs, BO cache, guest wired bytes and process RSS.
5. Keep 64 MiB blocks initially; test 32 MiB only after the known 16 MiB hang has a classified cause.

### Phase 4 — memory reservation

1. Continue manual, inert-at-boot preparation.
2. Qualify the newer CMA reservoir in isolation with target/KMI checks, `VERIFY_CMA`, release, module-disable and reboot tests.
3. Compare 3.0, 3.5 and 4.0 GiB only after guarded recovery succeeds repeatedly.
4. Do not introduce automatic boot reservation in the same change as the module upgrade.

### Phase 5 — later features

1. Localize ReplayKit's `kIOReturnBusy` writeback-surface failure before designing encoding.
2. Define a bounded trust mailbox with version, transaction ID, strict 40-hex CDHashes, size/count limits, replay protection and atomic writes.
3. Add `Image.new`/`Image.last-good`, fsync/rename, boot heartbeat and automatic rollback.
4. Define an entitlement allowlist before arbitrary IPA imports.
5. Validate first with a small owner-authorized open-source decrypted app; never remove FairPlay or accept `cryptid=1`.

## Three guarded phone sessions

No session should begin until Phase 0 host tests pass.

### Session A — safest baseline

- 3.0 GiB, six vCPUs on non-prime host cores where feasible, floors off, accepted crosvm scanout, no new cache/lifetime policy.
- Validate guard stop, unconditional chord, 120 Hz ownership/restoration, temperature trend and baseline GPU/fence counters.
- Stop on guard threshold, panel loss, repeated real fence waits, recovery-state loss or Android restoration failure.

### Session B — renderer and polling

- Same workload and RAM as Session A.
- Enable adaptive polling and rebuilt renderer with WAIT_FENCE plus both resource-ID guards.
- Compare idle residency, temperature slope, submit-to-retire latency, frame-time tails and refusal counters.
- Roll back as one artifact if there is corruption, a new hang or restoration regression.

### Session C — persistent cache

- Same workload and scene on two clean guest boots.
- Measure cold compile, first-run interaction and warm compile for the same Notes/Safari actions, including menus that remained slow.
- Accept only if warm latency improves without unbounded cache growth, increased map failures or visual corruption.

## Safety boundaries

- Never write Knox/EFS. The Bluetooth address file, if accessed, remains read-only and its contents are never logged.
- Keep phone logs private and untracked unless manually sanitized.
- Do not commit Apple binaries, built Images, IPAs, credentials, OTPs, anisette material, signing identity, keystores or trust secrets.
- Keep clock floors off by default.
- Keep `--gpu-scanout` as the final display path; pool-copy remains diagnostic only.
- Keep the KernelSU module inert at boot until a separately reviewed policy explicitly changes that decision.
- Do not describe unbooted appinst artifacts, screen recording or trust folding as phone-proven.
