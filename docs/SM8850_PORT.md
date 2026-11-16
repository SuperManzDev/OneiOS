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

## Scope of first commit series

Only a regression fixture for SM8850 CPU topology and the companion OneOS submodule URL/branch were added. No changes to xnuhost, kernel, Metal or GPU driver behavior are justified solely by these measurements. OneOS still blocks hot-switch on SM8850 until independent recovery verification.

Companion: https://github.com/YMGPwcca/OneOS/tree/feat/oneios-sm8850-m3q