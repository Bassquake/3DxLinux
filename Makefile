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

CORE := devices_gen report config hidraw lcd lcd_ent lcd_giotto deflate gfx font_aa output process profile control receiver \
        lcdmgr_db lcdmgr_core
CORE_OBJ := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE)))

all: $(BUILD)/tdxd $(BUILD)/tdx-lcd $(BUILD)/tdxctl $(BUILD)/tdx-lcdmgr $(BUILD)/libtdxlcd.a \
     $(BUILD)/libtdxlcd.so $(BUILD)/tdx-lcd-demo

# LCD Manager (Linux LCDMon) and the lglcd-compatible applet library
$(BUILD)/tdx-lcdmgr: $(BUILD)/tdx-lcdmgr.o $(CORE_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/libtdxlcd.a: $(BUILD)/tdxlcd.o
	$(CROSS_COMPILE)ar rcs $@ $^

$(BUILD)/tdxlcd.pic.o: src/tdxlcd.c src/tdxlcd.h src/lcdmgr_proto.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -pthread -c $< -o $@

$(BUILD)/libtdxlcd.so: $(BUILD)/tdxlcd.pic.o
	$(CC) -shared -pthread -Wl,-soname,libtdxlcd.so.1 $^ -o $@

$(BUILD)/tdx-lcd-demo: $(BUILD)/tdx-lcd-demo.o $(BUILD)/libtdxlcd.a $(BUILD)/gfx.o $(BUILD)/font_aa.o
	$(CC) $(LDFLAGS) -pthread $^ -o $@

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

# Settings panel (needs python3-gi, gir1.2-gtk-3.0, python3-gi-cairo and a display)
test-gui: $(BUILD)/tdxd
	python3 tests/test_settings.py $(BUILD)/tdxd

# LCD Manager end to end with a virtual LCD (no device needed)
test-lcdmgr: $(BUILD)/tdx-lcdmgr $(BUILD)/tdx-lcd-demo
	python3 tests/test_lcdmgr.py $(BUILD)

# The LCD applet ports (Function Keys, RSS Feeds, Picture Viewer)
test-lcdapplets: $(BUILD)/tdx-lcdmgr
	python3 tests/test_lcdapplets.py $(BUILD)

# FreeCAD key sets: the importer and the FreeCAD add-on (stand-ins, no FreeCAD needed)
test-freecad:
	python3 tests/test_freecad.py

# 3DxWare's application configurations -> tdxd profiles (the importer)
test-apps:
	python3 tests/test_app_profiles.py

# the system tray icon, on a private session bus
test-tray:
	dbus-run-session -- python3 tests/test_tray.py

# Regenerate src/font_aa.c (needs python3-gi with Pango and the DejaVu fonts)
font:
	python3 tools/gen_font.py > src/font_aa.c

# Regenerate the device table from an extracted 3DxWare Base.xml
devices:
	@test -n "$(BASE_XML)" || (echo "usage: make devices BASE_XML=path/to/Base.xml"; exit 1)
	python3 tools/extract_devices.py "$(BASE_XML)" > src/devices_gen.c

install: all
	install -Dm755 $(BUILD)/tdxd     $(DESTDIR)$(PREFIX)/bin/tdxd
	install -Dm755 $(BUILD)/tdx-lcd  $(DESTDIR)$(PREFIX)/bin/tdx-lcd
	install -Dm755 $(BUILD)/tdxctl   $(DESTDIR)$(PREFIX)/bin/tdxctl
	install -Dm755 packaging/tdx-focus $(DESTDIR)$(PREFIX)/bin/tdx-focus
	install -Dm755 packaging/tdx-freecad-addon $(DESTDIR)$(PREFIX)/bin/tdx-freecad-addon
	install -Dm644 packaging/freecad/tdxd_freecad.py $(DESTDIR)$(PREFIX)/share/tdxd/freecad/tdxd_freecad.py
	install -Dm644 packaging/freecad/TdxdSpaceMouse/InitGui.py $(DESTDIR)$(PREFIX)/share/tdxd/freecad/TdxdSpaceMouse/InitGui.py
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/tdx-settings | install -Dm755 /dev/stdin $(DESTDIR)$(PREFIX)/bin/tdx-settings
	install -Dm755 gui/tdx-settings-helper $(DESTDIR)$(PREFIX)/lib/tdxd/tdx-settings-helper
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/tdx-tray | install -Dm755 /dev/stdin $(DESTDIR)$(PREFIX)/bin/tdx-tray
	install -Dm755 packaging/tdx-focus-kwin $(DESTDIR)$(PREFIX)/lib/tdxd/tdx-focus-kwin
	install -Dm755 tools/extract_3dxware_ui.py $(DESTDIR)$(PREFIX)/bin/tdx-extract-3dxware-ui
	install -Dm755 $(BUILD)/tdx-lcdmgr $(DESTDIR)$(PREFIX)/bin/tdx-lcdmgr
	install -Dm755 $(BUILD)/tdx-lcd-demo $(DESTDIR)$(PREFIX)/bin/tdx-lcd-demo
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/tdx-lcdmanager | install -Dm755 /dev/stdin $(DESTDIR)$(PREFIX)/bin/tdx-lcdmanager
	install -Dm644 gui/tdxlcd.py $(DESTDIR)$(PREFIX)/lib/tdxd/python/tdxlcd.py
	install -Dm644 gui/lcdapplet.py $(DESTDIR)$(PREFIX)/lib/tdxd/python/lcdapplet.py
	for a in tdx-lcd-functionkeys tdx-lcd-rss tdx-lcd-pictures; do \
	  install -Dm755 gui/$$a $(DESTDIR)$(PREFIX)/bin/$$a; done
	install -Dm644 $(BUILD)/libtdxlcd.a $(DESTDIR)$(PREFIX)/lib/libtdxlcd.a
	install -Dm755 $(BUILD)/libtdxlcd.so $(DESTDIR)$(PREFIX)/lib/libtdxlcd.so.1
	ln -sf libtdxlcd.so.1 $(DESTDIR)$(PREFIX)/lib/libtdxlcd.so
	install -Dm644 src/tdxlcd.h $(DESTDIR)$(PREFIX)/include/tdxlcd.h
	sed 's#/usr/local/bin#$(PREFIX)/bin#' packaging/tdx-lcdmgr.service | install -Dm644 /dev/stdin $(DESTDIR)/etc/systemd/user/tdx-lcdmgr.service
	for d in tdx-settings tdx-lcdmanager tdx-lcd-functionkeys tdx-lcd-rss tdx-lcd-pictures; do \
	  install -Dm644 gui/$$d.desktop $(DESTDIR)$(PREFIX)/share/applications/$$d.desktop; done
	sed 's#/usr/local/lib/tdxd#$(PREFIX)/lib/tdxd#' gui/org.tdxd.settings.policy | install -Dm644 /dev/stdin $(DESTDIR)/usr/share/polkit-1/actions/org.tdxd.settings.policy
	sed 's#/usr/local/bin#$(PREFIX)/bin#' packaging/tdx-focus.service | install -Dm644 /dev/stdin $(DESTDIR)/etc/systemd/user/tdx-focus.service
	sed 's#/usr/local/bin#$(PREFIX)/bin#' packaging/tdx-tray.service | install -Dm644 /dev/stdin $(DESTDIR)/etc/systemd/user/tdx-tray.service
	install -Dm644 packaging/profiles/blender.conf.example $(DESTDIR)/etc/tdxd.d/blender.conf.example
	test -e $(DESTDIR)/etc/tdxd.d/freecad.conf || install -Dm644 packaging/profiles/freecad.conf $(DESTDIR)/etc/tdxd.d/freecad.conf
	sed -e 's/@VERSION@/$(VERSION)/' -e "s/@DATE@/$$(date -u +%Y-%m-%d)/" packaging/io.github.bassquake.tdxd.metainfo.xml | install -Dm644 /dev/stdin $(DESTDIR)$(PREFIX)/share/metainfo/io.github.bassquake.tdxd.metainfo.xml
	install -Dm644 packaging/70-3dconnexion-tdxd.rules $(DESTDIR)/etc/udev/rules.d/70-3dconnexion-tdxd.rules
	rm -f $(DESTDIR)/etc/udev/rules.d/99-3dconnexion-tdxd.rules  # old name, too late for uaccess
	install -Dm644 packaging/tdxd.service $(DESTDIR)/etc/systemd/system/tdxd.service
	test -e $(DESTDIR)/etc/tdxd.conf || install -Dm644 packaging/tdxd.conf $(DESTDIR)/etc/tdxd.conf

# Prebuilt package for testing on another machine:
#   make dist                 -> build/dist/3DxLinux-<version>-linux-<arch>.tar.gz (static)
#   make dist ARCH=arm64
# Release assets for both architectures plus SHA256SUMS:
#   make release VERSION=0.1.0
DIST    := build/dist/3DxLinux-$(VERSION)-linux-$(ARCH)
STRIP   ?= $(if $(CROSS_COMPILE),$(CROSS_COMPILE)strip,strip)
STAGE   := $(or $(TMPDIR),/tmp)/tdxd-dist-$(ARCH)

dist:
	rm -rf $(BUILD)   # rebuild everything so the embedded version is current
	$(MAKE) STATIC=1 ARCH=$(ARCH) VERSION=$(VERSION) all
	rm -rf $(DIST) && mkdir -p $(DIST)/bin $(DIST)/lib
	cp $(BUILD)/tdxd $(BUILD)/tdx-lcd $(BUILD)/tdxctl packaging/tdx-focus packaging/tdx-freecad-addon $(DIST)/bin/
	mkdir -p $(DIST)/freecad/TdxdSpaceMouse
	cp packaging/freecad/tdxd_freecad.py $(DIST)/freecad/
	cp packaging/freecad/TdxdSpaceMouse/InitGui.py $(DIST)/freecad/TdxdSpaceMouse/
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/tdx-settings > $(DIST)/bin/tdx-settings
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/tdx-tray > $(DIST)/bin/tdx-tray
	cp gui/tdx-settings-helper packaging/tdx-focus-kwin $(DIST)/lib/
	cp tools/extract_3dxware_ui.py $(DIST)/bin/tdx-extract-3dxware-ui
	cp $(BUILD)/tdx-lcdmgr $(BUILD)/tdx-lcd-demo $(DIST)/bin/
	$(STRIP) $(DIST)/bin/tdx-lcdmgr $(DIST)/bin/tdx-lcd-demo
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/tdx-lcdmanager > $(DIST)/bin/tdx-lcdmanager
	mkdir -p $(DIST)/lib/python $(DIST)/include
	cp gui/tdxlcd.py gui/lcdapplet.py $(DIST)/lib/python/
	cp gui/tdx-lcd-functionkeys gui/tdx-lcd-rss gui/tdx-lcd-pictures $(DIST)/bin/
	cp $(BUILD)/libtdxlcd.a $(DIST)/lib/
	cp $(BUILD)/libtdxlcd.so $(DIST)/lib/libtdxlcd.so.1
	cp src/tdxlcd.h $(DIST)/include/
	$(STRIP) $(DIST)/bin/tdxd $(DIST)/bin/tdx-lcd $(DIST)/bin/tdxctl
	cp packaging/70-3dconnexion-tdxd.rules packaging/tdxd.service packaging/tdxd.conf \
	   packaging/tdx-focus.service packaging/tdx-tray.service packaging/profiles/blender.conf.example packaging/profiles/freecad.conf \
	   packaging/install.sh packaging/tdx-lcdmgr.service gui/*.desktop gui/org.tdxd.settings.policy \
	   packaging/io.github.bassquake.tdxd.metainfo.xml README.md docs/PROTOCOL.md $(DIST)/
	chmod +x $(DIST)/install.sh
	echo "$(VERSION) $(ARCH)" > $(DIST)/VERSION
	# Archive from a staging copy on a Linux filesystem so file modes are real:
	# 0644 for data, 0755 for programs/scripts, root ownership. (Building on WSL's
	# /mnt drives reports 0777 for everything, and root's tar restores archived modes.)
	rm -rf $(STAGE) && mkdir -p $(STAGE) && cp -r $(DIST) $(STAGE)/
	find $(STAGE) -type d -exec chmod 755 {} + && find $(STAGE) -type f -exec chmod 644 {} +
	find $(STAGE)/$(notdir $(DIST))/lib -type d -exec chmod 755 {} +
	chmod 755 $(STAGE)/$(notdir $(DIST))/bin/* $(STAGE)/$(notdir $(DIST))/lib/tdx-settings-helper \
	   $(STAGE)/$(notdir $(DIST))/lib/tdx-focus-kwin \
	   $(STAGE)/$(notdir $(DIST))/lib/libtdxlcd.so.1 \
	   $(STAGE)/$(notdir $(DIST))/install.sh
	tar -C $(STAGE) --owner=0 --group=0 --numeric-owner -czf $(DIST).tar.gz $(notdir $(DIST))
	rm -rf $(STAGE)
	@echo "wrote $(DIST).tar.gz"

# .deb and .rpm from the dist tree (double-click installs; needs dpkg-deb, rpmbuild)
packages: dist
	bash packaging/mkpkg.sh $(DIST) $(VERSION) $(ARCH) build/dist

release:
	$(MAKE) packages ARCH=x86_64 VERSION=$(VERSION)
	$(MAKE) packages ARCH=arm64 VERSION=$(VERSION)
	cd build/dist && sha256sum 3DxLinux-$(VERSION)-linux-x86_64.tar.gz \
	   3DxLinux-$(VERSION)-linux-arm64.tar.gz \
	   3DxLinux.$(VERSION)-1.amd64.deb 3DxLinux.$(VERSION)-1.arm64.deb \
	   3DxLinux.$(VERSION)-1.x86_64.rpm 3DxLinux.$(VERSION)-1.aarch64.rpm > 3DxLinux-$(VERSION)-SHA256SUMS
	@cat build/dist/3DxLinux-$(VERSION)-SHA256SUMS

clean:
	rm -rf build

.PHONY: all test test-gui test-lcdmgr test-lcdapplets test-freecad test-apps test-tray font install clean devices dist packages release
