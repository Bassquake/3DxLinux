# tdxd - Linux driver for 3Dconnexion devices (rebuilt from 3DxWare 10.9.14 RE).
#
# Native build:          make
# Cross build (arm64):   make ARCH=arm64          (uses aarch64-linux-gnu-gcc)
# Any other target:      make CROSS_COMPILE=riscv64-linux-gnu-
# Static binaries:       make STATIC=1
# Tests (native only):   make test
#
# Only the Linux UAPI headers are needed (hidraw, uinput, usbfs): no libusb,
# no libudev, no X11, so the code builds unchanged for x86_64, arm64, armhf,
# riscv64, ppc64le, ...

ARCH ?= $(shell uname -m)
ifeq ($(ARCH),arm64)
  CROSS_COMPILE ?= aarch64-linux-gnu-
endif
ifeq ($(ARCH),armhf)
  CROSS_COMPILE ?= arm-linux-gnueabihf-
endif
ifeq ($(ARCH),riscv64)
  CROSS_COMPILE ?= riscv64-linux-gnu-
endif

CC      ?= cc
ifneq ($(CROSS_COMPILE),)
  CC := $(CROSS_COMPILE)gcc
endif
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_DEFAULT_SOURCE -Isrc
LDFLAGS ?=
ifeq ($(STATIC),1)
  LDFLAGS += -static
endif

BUILD   := build/$(ARCH)
PREFIX  ?= /usr/local
DESTDIR ?=

CORE := devices_gen report config hidraw lcd lcd_ent deflate gfx output process
CORE_OBJ := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE)))

all: $(BUILD)/tdxd $(BUILD)/tdx-lcd

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

$(BUILD)/tdxd: $(BUILD)/tdxd.o $(CORE_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/tdx-lcd: $(BUILD)/tdx-lcd.o $(CORE_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/test_core: tests/test_core.c $(CORE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

test: $(BUILD)/test_core
	rm -rf $(BUILD)/vectors && mkdir -p $(BUILD)/vectors
	$(BUILD)/test_core $(BUILD)/vectors
	@if command -v python3 >/dev/null; then python3 tests/check_deflate.py $(BUILD)/vectors; 	 else echo "python3 not found: skipping independent deflate check"; fi

# Regenerate the device table from an extracted 3DxWare Base.xml
devices:
	@test -n "$(BASE_XML)" || (echo "usage: make devices BASE_XML=path/to/Base.xml"; exit 1)
	python3 tools/extract_devices.py "$(BASE_XML)" > src/devices_gen.c

install: all
	install -Dm755 $(BUILD)/tdxd     $(DESTDIR)$(PREFIX)/bin/tdxd
	install -Dm755 $(BUILD)/tdx-lcd  $(DESTDIR)$(PREFIX)/bin/tdx-lcd
	install -Dm644 packaging/99-3dconnexion-tdxd.rules $(DESTDIR)/etc/udev/rules.d/99-3dconnexion-tdxd.rules
	install -Dm644 packaging/tdxd.service $(DESTDIR)/etc/systemd/system/tdxd.service
	test -e $(DESTDIR)/etc/tdxd.conf || install -Dm644 packaging/tdxd.conf $(DESTDIR)/etc/tdxd.conf

clean:
	rm -rf build

.PHONY: all test install clean devices
