# gpuprobe (gpuprobe.c): does Android's GPU write into a crosvm VM's restricted DMA pool? (docs/native.md, the GPU)
#   make    build/android/gpuprobe (static, the NDK's): crosvm's pool found with runtime/display's xhd.c, kgsl's UAPI
#           header from external/virglrenderer (Droid-VM's drm2kgsl: scripts/setup/get-references.sh virglrenderer)
include $(dir $(lastword $(MAKEFILE_LIST)))../host.mk
ANDROID_CC ?= $(TOOLS)/aarch64-linux-android30-clang
KGSL_H ?= ../../external/virglrenderer/src/drm/drm2kgsl
A := build/android

all: $(A)/gpuprobe

$(A)/gpuprobe: gpuprobe.c ../display/src/xhd.c ../display/src/xhd.h
	@[ -f $(KGSL_H)/msm_kgsl.h ] || { echo "gpuprobe: no $(KGSL_H)/msm_kgsl.h (scripts/setup/get-references.sh virglrenderer)" >&2; exit 1; }
	@mkdir -p $(A)
	$(ANDROID_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -static -I../display/src -I$(KGSL_H) gpuprobe.c ../display/src/xhd.c -lm -o $@

clean:
	rm -rf build
.PHONY: all clean
