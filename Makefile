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
# Version reported by `tdxd -V` / `tdx-lcd -V` and used in package names.
# Release builds: make release VERSION=0.1.0
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_DEFAULT_SOURCE -Isrc
CFLAGS  += -DTDX_VERSION='"$(VERSION)"'
LDFLAGS ?=
ifeq ($(STATIC),1)
  LDFLAGS += -static
endif

BUILD   := build/$(ARCH)
PREFIX  ?= /usr/local
DESTDIR ?=

CORE := devices_gen report config hidraw lcd lcd_ent lcd_giotto deflate gfx output process profile control
CORE_OBJ := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE)))

all: $(BUILD)/tdxd $(BUILD)/tdx-lcd $(BUILD)/tdxctl

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

$(BUILD)/tdxd: $(BUILD)/tdxd.o $(CORE_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/tdx-lcd: $(BUILD)/tdx-lcd.o $(CORE_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/tdxctl: $(BUILD)/tdxctl.o $(BUILD)/control.o
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
	install -Dm755 $(BUILD)/tdxctl   $(DESTDIR)$(PREFIX)/bin/tdxctl
	install -Dm755 packaging/tdx-focus $(DESTDIR)$(PREFIX)/bin/tdx-focus
	sed 's#/usr/local/bin#$(PREFIX)/bin#' packaging/tdx-focus.service | install -Dm644 /dev/stdin $(DESTDIR)/etc/systemd/user/tdx-focus.service
	install -Dm644 packaging/profiles/blender.conf.example $(DESTDIR)/etc/tdxd.d/blender.conf.example
	install -Dm644 packaging/99-3dconnexion-tdxd.rules $(DESTDIR)/etc/udev/rules.d/99-3dconnexion-tdxd.rules
	install -Dm644 packaging/tdxd.service $(DESTDIR)/etc/systemd/system/tdxd.service
	test -e $(DESTDIR)/etc/tdxd.conf || install -Dm644 packaging/tdxd.conf $(DESTDIR)/etc/tdxd.conf

# Prebuilt package for testing on another machine:
#   make dist                 -> build/dist/tdxd-<version>-linux-<arch>.tar.gz (static)
#   make dist ARCH=arm64
# Release assets for both architectures plus SHA256SUMS:
#   make release VERSION=0.1.0
DIST    := build/dist/tdxd-$(VERSION)-linux-$(ARCH)
STRIP   ?= $(if $(CROSS_COMPILE),$(CROSS_COMPILE)strip,strip)
STAGE   := $(or $(TMPDIR),/tmp)/tdxd-dist-$(ARCH)

dist:
	rm -rf $(BUILD)   # rebuild everything so the embedded version is current
	$(MAKE) STATIC=1 ARCH=$(ARCH) VERSION=$(VERSION) all
	rm -rf $(DIST) && mkdir -p $(DIST)/bin
	cp $(BUILD)/tdxd $(BUILD)/tdx-lcd $(BUILD)/tdxctl packaging/tdx-focus $(DIST)/bin/
	$(STRIP) $(DIST)/bin/tdxd $(DIST)/bin/tdx-lcd $(DIST)/bin/tdxctl
	cp packaging/99-3dconnexion-tdxd.rules packaging/tdxd.service packaging/tdxd.conf \
	   packaging/tdx-focus.service packaging/profiles/blender.conf.example \
	   packaging/install.sh README.md docs/PROTOCOL.md $(DIST)/
	chmod +x $(DIST)/install.sh
	echo "$(VERSION) $(ARCH)" > $(DIST)/VERSION
	# Archive from a staging copy on a Linux filesystem so file modes are real:
	# 0644 for data, 0755 for programs/scripts, root ownership. (Building on WSL's
	# /mnt drives reports 0777 for everything, and root's tar restores archived modes.)
	rm -rf $(STAGE) && mkdir -p $(STAGE) && cp -r $(DIST) $(STAGE)/
	find $(STAGE) -type d -exec chmod 755 {} + && find $(STAGE) -type f -exec chmod 644 {} +
	chmod 755 $(STAGE)/$(notdir $(DIST))/bin/* $(STAGE)/$(notdir $(DIST))/install.sh
	tar -C $(STAGE) --owner=0 --group=0 --numeric-owner -czf $(DIST).tar.gz $(notdir $(DIST))
	rm -rf $(STAGE)
	@echo "wrote $(DIST).tar.gz"

release:
	$(MAKE) dist ARCH=x86_64 VERSION=$(VERSION)
	$(MAKE) dist ARCH=arm64 VERSION=$(VERSION)
	cd build/dist && sha256sum tdxd-$(VERSION)-linux-x86_64.tar.gz \
	   tdxd-$(VERSION)-linux-arm64.tar.gz > tdxd-$(VERSION)-SHA256SUMS
	@cat build/dist/tdxd-$(VERSION)-SHA256SUMS

clean:
	rm -rf build

.PHONY: all test install clean devices dist release
