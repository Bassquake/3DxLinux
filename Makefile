# 3dxlinuxd - Linux driver for 3Dconnexion devices (rebuilt from 3DxWare 10.9.14 RE).
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
# Version reported by `3dxlinuxd -V` / `3dxlinux-lcd -V` and used in package names.
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

all: $(BUILD)/3dxlinuxd $(BUILD)/3dxlinux-lcd $(BUILD)/3dxlinuxctl $(BUILD)/3dxlinux-lcdmgr $(BUILD)/lib3dxlinuxlcd.a \
     $(BUILD)/lib3dxlinuxlcd.so $(BUILD)/3dxlinux-lcd-demo

# LCD Manager (Linux LCDMon) and the lglcd-compatible applet library
$(BUILD)/3dxlinux-lcdmgr: $(BUILD)/3dxlinux-lcdmgr.o $(CORE_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/lib3dxlinuxlcd.a: $(BUILD)/3dxlinuxlcd.o
	$(CROSS_COMPILE)ar rcs $@ $^

$(BUILD)/3dxlinuxlcd.pic.o: src/3dxlinuxlcd.c src/3dxlinuxlcd.h src/lcdmgr_proto.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -pthread -c $< -o $@

$(BUILD)/lib3dxlinuxlcd.so: $(BUILD)/3dxlinuxlcd.pic.o
	$(CC) -shared -pthread -Wl,-soname,lib3dxlinuxlcd.so.1 $^ -o $@

$(BUILD)/3dxlinux-lcd-demo: $(BUILD)/3dxlinux-lcd-demo.o $(BUILD)/lib3dxlinuxlcd.a $(BUILD)/gfx.o $(BUILD)/font_aa.o
	$(CC) $(LDFLAGS) -pthread $^ -o $@

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

$(BUILD)/3dxlinuxd: $(BUILD)/3dxlinuxd.o $(CORE_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/3dxlinux-lcd: $(BUILD)/3dxlinux-lcd.o $(CORE_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/3dxlinuxctl: $(BUILD)/3dxlinuxctl.o $(BUILD)/control.o
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/test_core: tests/test_core.c $(CORE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

test: $(BUILD)/test_core
	rm -rf $(BUILD)/vectors && mkdir -p $(BUILD)/vectors
	$(BUILD)/test_core $(BUILD)/vectors
	@if command -v python3 >/dev/null; then python3 tests/check_deflate.py $(BUILD)/vectors; 	 else echo "python3 not found: skipping independent deflate check"; fi

# Settings panel (needs python3-gi, gir1.2-gtk-3.0, python3-gi-cairo and a display)
test-gui: $(BUILD)/3dxlinuxd
	python3 tests/test_settings.py $(BUILD)/3dxlinuxd

# LCD Manager end to end with a virtual LCD (no device needed)
test-lcdmgr: $(BUILD)/3dxlinux-lcdmgr $(BUILD)/3dxlinux-lcd-demo
	python3 tests/test_lcdmgr.py $(BUILD)

# The LCD applet ports (Function Keys, RSS Feeds, Picture Viewer)
test-lcdapplets: $(BUILD)/3dxlinux-lcdmgr
	python3 tests/test_lcdapplets.py $(BUILD)

# FreeCAD key sets: the importer and the FreeCAD add-on (stand-ins, no FreeCAD needed)
test-freecad:
	python3 tests/test_freecad.py

# 3DxWare's application configurations -> 3dxlinuxd profiles (the importer)
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
	install -Dm755 $(BUILD)/3dxlinuxd     $(DESTDIR)$(PREFIX)/bin/3dxlinuxd
	install -Dm755 $(BUILD)/3dxlinux-lcd  $(DESTDIR)$(PREFIX)/bin/3dxlinux-lcd
	install -Dm755 $(BUILD)/3dxlinuxctl   $(DESTDIR)$(PREFIX)/bin/3dxlinuxctl
	install -Dm755 packaging/3dxlinux-focus $(DESTDIR)$(PREFIX)/bin/3dxlinux-focus
	install -Dm755 packaging/3dxlinux-freecad-addon $(DESTDIR)$(PREFIX)/bin/3dxlinux-freecad-addon
	install -Dm644 packaging/freecad/freecad_3dxlinux.py $(DESTDIR)$(PREFIX)/share/3dxlinux/freecad/freecad_3dxlinux.py
	install -Dm644 packaging/freecad/SpaceMouse3DxLinux/InitGui.py $(DESTDIR)$(PREFIX)/share/3dxlinux/freecad/SpaceMouse3DxLinux/InitGui.py
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/3dxlinux-settings | install -Dm755 /dev/stdin $(DESTDIR)$(PREFIX)/bin/3dxlinux-settings
	install -Dm755 gui/3dxlinux-settings-helper $(DESTDIR)$(PREFIX)/lib/3dxlinux/3dxlinux-settings-helper
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/3dxlinux-tray | install -Dm755 /dev/stdin $(DESTDIR)$(PREFIX)/bin/3dxlinux-tray
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/3dxlinux-radial | install -Dm755 /dev/stdin $(DESTDIR)$(PREFIX)/bin/3dxlinux-radial
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/3dxlinux-numpad | install -Dm755 /dev/stdin $(DESTDIR)$(PREFIX)/bin/3dxlinux-numpad
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/3dxlinux-osd | install -Dm755 /dev/stdin $(DESTDIR)$(PREFIX)/bin/3dxlinux-osd
	install -Dm755 packaging/3dxlinux-focus-kwin $(DESTDIR)$(PREFIX)/lib/3dxlinux/3dxlinux-focus-kwin
	install -Dm755 tools/extract_3dxware_ui.py $(DESTDIR)$(PREFIX)/bin/3dxlinux-extract-3dxware-ui
	install -Dm755 $(BUILD)/3dxlinux-lcdmgr $(DESTDIR)$(PREFIX)/bin/3dxlinux-lcdmgr
	install -Dm755 $(BUILD)/3dxlinux-lcd-demo $(DESTDIR)$(PREFIX)/bin/3dxlinux-lcd-demo
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/3dxlinux-lcdmanager | install -Dm755 /dev/stdin $(DESTDIR)$(PREFIX)/bin/3dxlinux-lcdmanager
	install -Dm644 gui/lcd_3dxlinux.py $(DESTDIR)$(PREFIX)/lib/3dxlinux/python/lcd_3dxlinux.py
	install -Dm644 gui/lcdapplet.py $(DESTDIR)$(PREFIX)/lib/3dxlinux/python/lcdapplet.py
	for a in 3dxlinux-lcd-functionkeys 3dxlinux-lcd-rss 3dxlinux-lcd-pictures; do \
	  install -Dm755 gui/$$a $(DESTDIR)$(PREFIX)/bin/$$a; done
	install -Dm644 $(BUILD)/lib3dxlinuxlcd.a $(DESTDIR)$(PREFIX)/lib/lib3dxlinuxlcd.a
	install -Dm755 $(BUILD)/lib3dxlinuxlcd.so $(DESTDIR)$(PREFIX)/lib/lib3dxlinuxlcd.so.1
	ln -sf lib3dxlinuxlcd.so.1 $(DESTDIR)$(PREFIX)/lib/lib3dxlinuxlcd.so
	install -Dm644 src/3dxlinuxlcd.h $(DESTDIR)$(PREFIX)/include/3dxlinuxlcd.h
	sed 's#/usr/local/bin#$(PREFIX)/bin#' packaging/3dxlinux-lcdmgr.service | install -Dm644 /dev/stdin $(DESTDIR)/etc/systemd/user/3dxlinux-lcdmgr.service
	for d in 3dxlinux-settings 3dxlinux-lcdmanager 3dxlinux-lcd-functionkeys 3dxlinux-lcd-rss 3dxlinux-lcd-pictures; do \
	  install -Dm644 gui/$$d.desktop $(DESTDIR)$(PREFIX)/share/applications/$$d.desktop; done
	sed 's#/usr/local/lib/3dxlinux#$(PREFIX)/lib/3dxlinux#' gui/io.github.bassquake.3dxlinux.settings.policy | install -Dm644 /dev/stdin $(DESTDIR)/usr/share/polkit-1/actions/io.github.bassquake.3dxlinux.settings.policy
	sed 's#/usr/local/bin#$(PREFIX)/bin#' packaging/3dxlinux-focus.service | install -Dm644 /dev/stdin $(DESTDIR)/etc/systemd/user/3dxlinux-focus.service
	sed 's#/usr/local/bin#$(PREFIX)/bin#' packaging/3dxlinux-tray.service | install -Dm644 /dev/stdin $(DESTDIR)/etc/systemd/user/3dxlinux-tray.service
	install -Dm644 packaging/profiles/blender.conf.example $(DESTDIR)/etc/3dxlinux.d/blender.conf.example
	test -e $(DESTDIR)/etc/3dxlinux.d/freecad.conf || install -Dm644 packaging/profiles/freecad.conf $(DESTDIR)/etc/3dxlinux.d/freecad.conf
	sed -e 's/@VERSION@/$(VERSION)/' -e "s/@DATE@/$$(date -u +%Y-%m-%d)/" packaging/io.github.bassquake._3DxLinux.metainfo.xml | install -Dm644 /dev/stdin $(DESTDIR)$(PREFIX)/share/metainfo/io.github.bassquake._3DxLinux.metainfo.xml
	install -Dm644 packaging/70-3dconnexion-3dxlinux.rules $(DESTDIR)/etc/udev/rules.d/70-3dconnexion-3dxlinux.rules
	install -Dm644 packaging/3dxlinuxd.service $(DESTDIR)/etc/systemd/system/3dxlinuxd.service
	test -e $(DESTDIR)/etc/3dxlinux.conf || install -Dm644 packaging/3dxlinux.conf $(DESTDIR)/etc/3dxlinux.conf

# Prebuilt package for testing on another machine:
#   make dist                 -> build/dist/3DxLinux-<version>-linux-<arch>.tar.gz (static)
#   make dist ARCH=arm64
# Release assets for both architectures plus SHA256SUMS:
#   make release VERSION=0.1.0
DIST    := build/dist/3DxLinux-$(VERSION)-linux-$(ARCH)
STRIP   ?= $(if $(CROSS_COMPILE),$(CROSS_COMPILE)strip,strip)
STAGE   := $(or $(TMPDIR),/tmp)/3dxlinux-dist-$(ARCH)

dist:
	rm -rf $(BUILD)   # rebuild everything so the embedded version is current
	$(MAKE) STATIC=1 ARCH=$(ARCH) VERSION=$(VERSION) all
	rm -rf $(DIST) && mkdir -p $(DIST)/bin $(DIST)/lib
	cp $(BUILD)/3dxlinuxd $(BUILD)/3dxlinux-lcd $(BUILD)/3dxlinuxctl packaging/3dxlinux-focus packaging/3dxlinux-freecad-addon $(DIST)/bin/
	mkdir -p $(DIST)/freecad/SpaceMouse3DxLinux
	cp packaging/freecad/freecad_3dxlinux.py $(DIST)/freecad/
	cp packaging/freecad/SpaceMouse3DxLinux/InitGui.py $(DIST)/freecad/SpaceMouse3DxLinux/
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/3dxlinux-settings > $(DIST)/bin/3dxlinux-settings
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/3dxlinux-tray > $(DIST)/bin/3dxlinux-tray
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/3dxlinux-radial > $(DIST)/bin/3dxlinux-radial
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/3dxlinux-numpad > $(DIST)/bin/3dxlinux-numpad
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/3dxlinux-osd > $(DIST)/bin/3dxlinux-osd
	cp gui/3dxlinux-settings-helper packaging/3dxlinux-focus-kwin $(DIST)/lib/
	cp tools/extract_3dxware_ui.py $(DIST)/bin/3dxlinux-extract-3dxware-ui
	cp $(BUILD)/3dxlinux-lcdmgr $(BUILD)/3dxlinux-lcd-demo $(DIST)/bin/
	$(STRIP) $(DIST)/bin/3dxlinux-lcdmgr $(DIST)/bin/3dxlinux-lcd-demo
	sed 's/^APP_VERSION = .*/APP_VERSION = "$(VERSION)"/' gui/3dxlinux-lcdmanager > $(DIST)/bin/3dxlinux-lcdmanager
	mkdir -p $(DIST)/lib/python $(DIST)/include
	cp gui/lcd_3dxlinux.py gui/lcdapplet.py $(DIST)/lib/python/
	cp gui/3dxlinux-lcd-functionkeys gui/3dxlinux-lcd-rss gui/3dxlinux-lcd-pictures $(DIST)/bin/
	cp $(BUILD)/lib3dxlinuxlcd.a $(DIST)/lib/
	cp $(BUILD)/lib3dxlinuxlcd.so $(DIST)/lib/lib3dxlinuxlcd.so.1
	cp src/3dxlinuxlcd.h $(DIST)/include/
	$(STRIP) $(DIST)/bin/3dxlinuxd $(DIST)/bin/3dxlinux-lcd $(DIST)/bin/3dxlinuxctl
	cp packaging/70-3dconnexion-3dxlinux.rules packaging/3dxlinuxd.service packaging/3dxlinux.conf \
	   packaging/3dxlinux-focus.service packaging/3dxlinux-tray.service packaging/profiles/blender.conf.example packaging/profiles/freecad.conf \
	   packaging/install.sh packaging/3dxlinux-lcdmgr.service gui/*.desktop gui/io.github.bassquake.3dxlinux.settings.policy \
	   packaging/io.github.bassquake._3DxLinux.metainfo.xml README.md docs/PROTOCOL.md $(DIST)/
	chmod +x $(DIST)/install.sh
	echo "$(VERSION) $(ARCH)" > $(DIST)/VERSION
	# Archive from a staging copy on a Linux filesystem so file modes are real:
	# 0644 for data, 0755 for programs/scripts, root ownership. (Building on WSL's
	# /mnt drives reports 0777 for everything, and root's tar restores archived modes.)
	rm -rf $(STAGE) && mkdir -p $(STAGE) && cp -r $(DIST) $(STAGE)/
	find $(STAGE) -type d -exec chmod 755 {} + && find $(STAGE) -type f -exec chmod 644 {} +
	find $(STAGE)/$(notdir $(DIST))/lib -type d -exec chmod 755 {} +
	chmod 755 $(STAGE)/$(notdir $(DIST))/bin/* $(STAGE)/$(notdir $(DIST))/lib/3dxlinux-settings-helper \
	   $(STAGE)/$(notdir $(DIST))/lib/3dxlinux-focus-kwin \
	   $(STAGE)/$(notdir $(DIST))/lib/lib3dxlinuxlcd.so.1 \
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
