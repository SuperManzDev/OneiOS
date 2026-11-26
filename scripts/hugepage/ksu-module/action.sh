#!/system/bin/sh
# KernelSU Manager's action button is deliberately read-only. Preparing RAM is never a one-tap side effect.
exec /data/adb/modules/oneios_native_on_demand/system/bin/oneios-native status
