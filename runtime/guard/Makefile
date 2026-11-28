# Independent native-session thermal/key/recovery guard.
include $(dir $(lastword $(MAKEFILE_LIST)))../host.mk
ANDROID_CC ?= $(TOOLS)/aarch64-linux-android30-clang
WARN := -Wall -Wextra -Werror
INC := -I. -I../bridges/thermal -I../bridges/include
THERMAL_SRC := ../bridges/thermal/thermal.c ../bridges/cpu/topology.c ../bridges/hw/hw_backend.c \
  ../bridges/hw/hw_log.c ../bridges/hal/hal.c ../bridges/wifi/aidl_parcel.c ../bridges/wifi/aidl_binder.c
SRC := oneios_guard.c policy.c $(THERMAL_SRC)
HDR := guard.h ../bridges/thermal/thermal.h

all: build/android/oneios-guard

build/android/oneios-guard: $(SRC) $(HDR)
	@mkdir -p $(dir $@)
	$(ANDROID_CC) -std=gnu11 -O2 -g $(WARN) $(INC) -static $(SRC) -o $@

build/host/test_guard: test_guard.c policy.c guard.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O1 -g $(WARN) $(INC) -fsanitize=address,undefined test_guard.c policy.c -o $@

build/host/oneios-guard: $(SRC) $(HDR)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O1 -g $(WARN) $(INC) -fsanitize=address,undefined $(SRC) -lpthread -o $@

test: build/host/test_guard build/host/oneios-guard
	build/host/test_guard
	build/host/oneios-guard 2>&1 | grep -q '^usage:'

clean:
	rm -rf build

.PHONY: all test clean
