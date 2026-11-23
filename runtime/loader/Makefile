# The OneiOS loader library (libxnuloader.a), its tests, xnuloader-inspect, xnuloader-qemu and xnuloader-crosvm.
#   make            host build into build/host
#   make test       host unit tests (generated fixtures, no Apple files)
#   make android    cross-build for aarch64-linux-android into build/android
# xnuloader-qemu places a standalone kernel for qemu-system-aarch64's virt board (scripts/qemu/boot-vm.sh, docs/native.md),
# with an image set's disks: it compiles xnuhost's imageset.c (the manifest, hash lists and trust caches as xnuhost reads them).
# xnuloader-crosvm makes the native kernel a Linux Image for a VM of Android's crosvm under the phone's Gunyah (docs/native.md).
include $(dir $(lastword $(MAKEFILE_LIST)))../host.mk
ANDROID_CC ?= $(TOOLS)/aarch64-linux-android30-clang
ANDROID_AR ?= $(TOOLS)/llvm-ar
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Iinclude
SRC := src/macho.c src/adt.c src/boot.c src/machine.c
HDR := $(wildcard include/xnuloader/*.h) tests/fixture.h

all: build/host/libxnuloader.a build/host/xnuloader-inspect build/host/xnuloader-qemu build/host/xnuloader-crosvm

build/host/%.o: src/%.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@
build/host/libxnuloader.a: $(SRC:src/%.c=build/host/%.o)
	$(AR) rcs $@ $^
build/host/xnuloader-inspect: tools/xnuloader-inspect.c build/host/libxnuloader.a
	$(CC) $(CFLAGS) $^ -o $@
build/host/xnuloader-qemu: tools/xnuloader-qemu.c ../xnuhost/src/imageset.c ../xnuhost/src/imageset.h build/host/libxnuloader.a
	$(CC) $(CFLAGS) -I../xnuhost/src tools/xnuloader-qemu.c ../xnuhost/src/imageset.c build/host/libxnuloader.a -lpthread -o $@
build/host/xnuloader-crosvm: tools/xnuloader-crosvm.c build/host/libxnuloader.a
	$(CC) $(CFLAGS) $^ -o $@
build/host/test_loader: tests/test_loader.c build/host/libxnuloader.a $(HDR)
	$(CC) $(CFLAGS) -fsanitize=address,undefined tests/test_loader.c $(SRC) -o $@

test: build/host/test_loader build/host/xnuloader-inspect
	build/host/test_loader
	build/host/xnuloader-inspect board 4 > build/host/board.adt
	build/host/xnuloader-inspect adt build/host/board.adt > /dev/null

build/android/%.o: src/%.c $(HDR)
	@mkdir -p $(@D)
	$(ANDROID_CC) $(CFLAGS) -fPIC -c $< -o $@
build/android/libxnuloader.a: $(SRC:src/%.c=build/android/%.o)
	$(ANDROID_AR) rcs $@ $^
android: build/android/libxnuloader.a

clean:
	rm -rf build
.PHONY: all test android clean
