# OneiOS Native VM — on-demand KernelSU module

This module packages the reviewed Galaxy S25 Ultra Gunyah support modules and exposes one manual command. It is deliberately inert at boot.

## Screen-safety contract

The package contains **no** `post-fs-data.sh`, `service.sh`, or `boot-completed.sh`. Installing or enabling it does not reserve RAM, start a VM, stop Android, touch SurfaceFlinger, change the panel/backlight, or reboot the phone. KernelSU Manager's action button runs `status` only.

The only state-changing commands are explicit root commands:

```sh
oneios-native prepare 4096  # load the modules and try to reserve the 2 MiB pages
oneios-native release       # unload them only when no VM/parcel is active
```

`app/` builds the small **OneiOS Memory** control APK. Its launcher screen shows module status and the same Prepare/Release actions. The pinned **Prepare VM memory** home shortcut opens the app's confirmation dialog; it never reserves memory directly. KernelSU root must be granted to `dev.oneios.nativeprep` by the owner.

`prepare` leaves One UI and the screen running. It uses one bounded mode-1 acquire and fails closed if the pool remains short. A new preparation is transactional: any module or reserved pool it loaded is removed on failure; if cleanup itself cannot complete, status is marked partial and the app requires Release before another attempt. It never starts crosvm; VM launch stays a separate PC/user action. It does not lower the module's host-memory reserve.

The package is bound to the model, device, exact kernel release, Android build fingerprint, module hashes, and reviewed private `struct gunyah_vm` offsets recorded in `target.conf`. Any mismatch is refused before `insmod`.

## Why manual preparation may still be short

A command issued late in Android's boot can find RAM already fragmented. The safe module intentionally does not blank the screen or stop One UI to compact memory. Run `prepare` soon after boot, or use a future separately reviewed early-reservation mode. Early automatic reservation is not part of this package.

## Build

With the reviewed `.ko` outputs already built and the target phone connected read-only over ADB:

```sh
scripts/hugepage/ksu-module/build-module.sh
scripts/hugepage/ksu-module/test.sh
scripts/hugepage/ksu-module/app/test.sh
```

The ZIP is written under `out/ksu-module/`. Installation and any reboot require separate owner approval.

## Boundaries

- No Knox or EFS access.
- No automatic boot action.
- No forced module removal.
- No RAM-reserve lowering.
- No display, touch, vibrator, Bluetooth, Wi-Fi, or audio changes.
- Module sources and pins remain the ones documented by `scripts/hugepage/build*.sh`; the ZIP includes their notices and hashes.
