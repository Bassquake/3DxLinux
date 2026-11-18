#!/bin/bash
# SPDX-License-Identifier: MIT
# Build the .deb and .rpm packages from a `make dist` tree, so they hold the
# same static binaries as the .tar.gz. Double-clicking either one installs
# tdxd with the desktop's software app; the package's scripts then do what
# install.sh does (udev, uinput, systemd), with no commands to type.
#
#   packaging/mkpkg.sh DIST_DIR VERSION ARCH OUT_DIR [deb|rpm|all]
#
# ARCH is x86_64 or arm64. Needs dpkg-deb, and rpmbuild for the .rpm.
set -eu

DIST=$(cd "$1" && pwd); VERSION=$2; ARCH=$3; OUT=$(mkdir -p "$4" && cd "$4" && pwd); WHAT=${5:-all}
PV=${VERSION#v}; PV=${PV//-/+}         # 0.3.0, or 0.3.0+2+gabc1234 between releases
case $ARCH in
x86_64) DEB_ARCH=amd64; RPM_ARCH=x86_64; TRIPLET=x86_64-linux-gnu ;;
arm64)  DEB_ARCH=arm64; RPM_ARCH=aarch64; TRIPLET=aarch64-linux-gnu ;;
*) echo "unknown ARCH $ARCH" >&2; exit 1 ;;
esac
HOMEPAGE=https://github.com/Bassquake/SpacePilot-Pro-Linux
SUMMARY="3Dconnexion driver: SpacePilot PRO, SpaceMouse, LCD Manager and applets"
DESC="tdxd drives 3Dconnexion 3D mice (SpacePilot PRO, SpaceMouse Enterprise,
SpaceMouse Pro/Wireless, Giotto, ...) on Linux: motion and buttons for
spacenavd-compatible applications (Blender, FreeCAD, ...) and as an evdev
device, LCD and LED support, per-application profiles, a GTK port of the
3DxWare control panel (tdx-settings), and a port of Logitech's LCD Manager
with the Function Keys, RSS Feeds and Picture Viewer applets."

# ---- stage the files once, as installed under /usr ----------------------
# (on a Linux filesystem: a Windows drive would report every file as 0777)
STAGE=$(mktemp -d /tmp/tdxd-pkg.XXXXXX)
trap 'rm -rf "$STAGE"' EXIT
stage() { # stage LIBDIR -> $STAGE/root
	local R=$STAGE/root L=$1
	rm -rf "$R"
	for b in tdxd tdx-lcd tdxctl tdx-lcdmgr tdx-lcd-demo tdx-focus tdx-settings tdx-lcdmanager \
		 tdx-lcd-functionkeys tdx-lcd-rss tdx-lcd-pictures tdx-extract-3dxware-ui; do
		install -Dm755 "$DIST/bin/$b" "$R/usr/bin/$b"
	done
	install -Dm755 "$DIST/lib/tdx-settings-helper" "$R/usr/lib/tdxd/tdx-settings-helper"
	install -Dm755 "$DIST/lib/tdx-focus-kwin" "$R/usr/lib/tdxd/tdx-focus-kwin"
	install -Dm644 "$DIST/lib/python/tdxlcd.py" "$R/usr/lib/tdxd/python/tdxlcd.py"
	install -Dm644 "$DIST/lib/python/lcdapplet.py" "$R/usr/lib/tdxd/python/lcdapplet.py"
	install -Dm755 "$DIST/lib/libtdxlcd.so.1" "$R$L/libtdxlcd.so.1"
	ln -s libtdxlcd.so.1 "$R$L/libtdxlcd.so"
	install -Dm644 "$DIST/lib/libtdxlcd.a" "$R$L/libtdxlcd.a"
	install -Dm644 "$DIST/include/tdxlcd.h" "$R/usr/include/tdxlcd.h"
	install -d "$R/usr/lib/systemd/system" "$R/usr/lib/systemd/user"
	sed 's#/usr/local/#/usr/#g' "$DIST/tdxd.service" > "$R/usr/lib/systemd/system/tdxd.service"
	for u in tdx-lcdmgr tdx-focus; do
		sed 's#/usr/local/bin#/usr/bin#' "$DIST/$u.service" > "$R/usr/lib/systemd/user/$u.service"
	done
	install -Dm644 "$DIST/70-3dconnexion-tdxd.rules" "$R/usr/lib/udev/rules.d/70-3dconnexion-tdxd.rules"
	install -d "$R/usr/lib/modules-load.d"
	echo uinput > "$R/usr/lib/modules-load.d/tdxd.conf"
	for d in "$DIST"/*.desktop; do
		install -Dm644 "$d" "$R/usr/share/applications/$(basename "$d")"
	done
	install -d "$R/usr/share/polkit-1/actions"
	sed 's#/usr/local/lib/tdxd#/usr/lib/tdxd#' "$DIST/org.tdxd.settings.policy" \
		> "$R/usr/share/polkit-1/actions/org.tdxd.settings.policy"
	install -Dm644 "$DIST/tdxd.conf" "$R/etc/tdxd.conf"
	install -Dm644 "$DIST/blender.conf.example" "$R/etc/tdxd.d/blender.conf.example"
	install -Dm644 "$DIST/freecad.conf" "$R/etc/tdxd.d/freecad.conf"
	for f in README.md PROTOCOL.md; do
		install -Dm644 "$DIST/$f" "$R/usr/share/doc/tdxd/$f"
	done
	printf 'tdxd - %s\n\nMIT License. Copyright (c) the tdxd authors.\n' "$HOMEPAGE" \
		> "$R/usr/share/doc/tdxd/copyright"
	find "$R" -type d -exec chmod 755 {} +
	find "$R/usr/lib/systemd" "$R/usr/share" "$R/usr/lib/modules-load.d" -type f -exec chmod 644 {} +
}

# ---- what the package does after install / before removal -------------------
# $1: "install" or "upgrade"
POST_INSTALL='modprobe uinput 2>/dev/null || true
if command -v udevadm >/dev/null 2>&1; then
	udevadm control --reload-rules 2>/dev/null || true
	udevadm trigger --subsystem-match=hidraw --subsystem-match=usb --subsystem-match=misc 2>/dev/null || true
fi
if [ -d /run/systemd/system ]; then
	systemctl daemon-reload || true
	systemctl enable tdxd.service >/dev/null 2>&1 || true
	# restart, not try-restart: also starts a tdxd that was stopped, so after an
	# install or upgrade the new version always runs (seen: an upgrade left it dead)
	systemctl restart tdxd.service || true
	# the LCD Manager runs in every user session; (re)start it now for those logged
	# in, unless that user has turned it off
	# and so does tdx-focus (which app has the focus: per-application profiles and
	# the name on the LCD; KDE Plasma, sway, X11 - it exits quietly elsewhere)
	systemctl --global enable tdx-lcdmgr.service tdx-focus.service >/dev/null 2>&1 || true
	for u in $(loginctl list-users --no-legend 2>/dev/null | awk "{print \$2}"); do
		systemctl --user -M "$u@" daemon-reload >/dev/null 2>&1 || true
		for s in tdx-lcdmgr tdx-focus; do
			if systemctl --user -M "$u@" is-enabled $s.service >/dev/null 2>&1; then
				systemctl --user -M "$u@" restart $s.service >/dev/null 2>&1 || true
			fi
		done
	done
fi
if [ -x /usr/local/bin/tdxd ]; then
	echo "tdxd: an older copy from install.sh is in /usr/local; remove it with: sudo ./install.sh --remove" >&2
fi'
PRE_REMOVE='if [ -d /run/systemd/system ]; then
	for u in $(loginctl list-users --no-legend 2>/dev/null | awk "{print \$2}"); do
		systemctl --user -M "$u@" stop tdx-lcdmgr.service tdx-focus.service >/dev/null 2>&1 || true
	done
	systemctl --global disable tdx-lcdmgr.service tdx-focus.service >/dev/null 2>&1 || true
	systemctl disable --now tdxd.service >/dev/null 2>&1 || true
fi'
POST_REMOVE='if [ -d /run/systemd/system ]; then systemctl daemon-reload || true; fi
if command -v udevadm >/dev/null 2>&1; then udevadm control --reload-rules 2>/dev/null || true; fi'
# Uninstalling (Discover's Uninstall = a remove, not a purge) leaves nothing of
# tdxd behind, settings included - the user's choice. Never on an upgrade. Only
# tdxd's own, exactly named files: the settings and profiles, and what the
# 3DxWare-look importer wrote system-wide (as root) and in each home.
CLEANUP='rm -f /etc/tdxd.conf /etc/tdxd.conf.rpmsave /etc/tdxd.conf.rpmnew /etc/tdxd.conf.dpkg-old \
	/etc/tdxd.conf.dpkg-dist /etc/tdxd.conf.dpkg-new /etc/tdxd.conf.tdx-settings.tmp
rm -rf /etc/tdxd.d /usr/local/share/tdxd
# Python bytecode caches written when the programs ran (not package files):
# without this /usr/lib/tdxd stays behind
rm -rf /usr/lib/tdxd/python/__pycache__
rmdir /usr/lib/tdxd/python /usr/lib/tdxd 2>/dev/null || true
rm -f /usr/local/share/icons/hicolor/*/apps/tdxd-*.png
for e in tdx-settings tdx-lcdmanager tdx-lcd-functionkeys tdx-lcd-rss tdx-lcd-pictures; do
	rm -f "/usr/local/share/applications/$e.desktop"
done
getent passwd | while IFS=: read -r _name _pw uid _gid _gecos home _shell; do
	case "$uid" in ""|*[!0-9]*) continue ;; esac
	[ "$uid" -eq 0 ] || { [ "$uid" -ge 1000 ] && [ "$uid" -lt 60000 ]; } || continue
	case "$home" in /?*) ;; *) continue ;; esac
	case "$home" in *..*) continue ;; esac
	[ -d "$home" ] || continue
	rm -rf "$home/.config/tdxd" "$home/.local/share/tdxd"
	rm -f "$home"/.local/share/icons/hicolor/*/apps/tdxd-*.png
	for e in tdx-settings tdx-lcdmanager tdx-lcd-functionkeys tdx-lcd-rss tdx-lcd-pictures; do
		rm -f "$home/.local/share/applications/$e.desktop"
	done
done'

# ---- .deb ---------------------------------------------------------------------
# Recommends are installed by default, and so are theirs: xdg-utils (RSS "open
# in browser") and x11-utils (xprop, for tdx-focus) pulled in ~60 packages -
# Perl libraries and a terminal emulator - on a minimal system, so they are
# only Suggests. Every desktop has them anyway.
build_deb() {
	stage "/usr/lib/$TRIPLET"
	local R=$STAGE/root D=$STAGE/root/DEBIAN
	install -d "$D"
	cat > "$D/control" <<EOF
Package: tdxd
Version: $PV-1
Architecture: $DEB_ARCH
Maintainer: tdxd project <$HOMEPAGE/issues>
Installed-Size: $(du -sk --exclude=DEBIAN "$R" | cut -f1)
Depends: python3, python3-gi, python3-gi-cairo, gir1.2-gtk-3.0, pkexec | policykit-1
Recommends: p7zip-full, librsvg2-common
Suggests: xdg-utils, x11-utils
Conflicts: spacenavd
Section: utils
Priority: optional
Homepage: $HOMEPAGE
Description: $SUMMARY
$(echo "$DESC" | sed 's/^/ /')
EOF
	printf '/etc/tdxd.conf\n/etc/tdxd.d/blender.conf.example\n/etc/tdxd.d/freecad.conf\n' > "$D/conffiles"
	{ echo '#!/bin/sh'; echo 'set -e'
	  echo '[ "$1" = configure ] || exit 0'
	  echo 'if [ -n "${2:-}" ]; then set -- upgrade; else set -- install; fi'
	  echo "$POST_INSTALL"; } > "$D/postinst"
	{ echo '#!/bin/sh'; echo 'set -e'; echo '[ "$1" = remove ] || exit 0'; echo "$PRE_REMOVE"; } > "$D/prerm"
	{ echo '#!/bin/sh'; echo 'set -e'; echo "$POST_REMOVE"
	  echo 'case "$1" in remove|purge) ;; *) exit 0 ;; esac'; echo "$CLEANUP"; } > "$D/postrm"
	chmod 755 "$D/postinst" "$D/prerm" "$D/postrm"
	local f=$OUT/tdxd_${PV}-1_$DEB_ARCH.deb
	dpkg-deb --root-owner-group -Zxz --build "$R" "$f" >/dev/null
	echo "wrote $f"
}

# ---- .rpm ---------------------------------------------------------------------
build_rpm() {
	stage /usr/lib64
	local top=$STAGE/rpmbuild spec=$STAGE/tdxd.spec
	install -d "$top/BUILD" "$top/RPMS" "$top/SPECS" "$top/SRPMS" "$top/SOURCES"
	{
	cat <<EOF
Name: tdxd
Version: $PV
Release: 1
Summary: $SUMMARY
License: MIT
URL: $HOMEPAGE
Requires: python3, python3-gobject, python3-cairo, gtk3, polkit
Recommends: p7zip, librsvg2
Suggests: xdg-utils, xprop
Conflicts: spacenavd
AutoReqProv: no
# prebuilt static binaries: nothing to strip or split out
%global debug_package %{nil}
%global __os_install_post %{nil}
%global _build_id_links none
%global _binary_payload w9.xzdio

%description
$DESC

%install
mkdir -p %{buildroot}
cp -a $STAGE/root/. %{buildroot}/

%post
if [ "\$1" -ge 2 ]; then set -- upgrade; else set -- install; fi
EOF
	echo "$POST_INSTALL" | sed 's/%/%%/g'
	echo; echo '%preun'; echo 'if [ "$1" -eq 0 ]; then'; echo "$PRE_REMOVE" | sed 's/%/%%/g'; echo 'fi'
	echo; echo '%postun'; echo "$POST_REMOVE" | sed 's/%/%%/g'
	echo 'if [ "$1" -eq 0 ]; then'; echo "$CLEANUP" | sed 's/%/%%/g'; echo 'fi'
	echo; echo '%files'
	echo '%config(noreplace) /etc/tdxd.conf'
	echo '%config(noreplace) /etc/tdxd.d/blender.conf.example'
	echo '%config(noreplace) /etc/tdxd.d/freecad.conf'
	echo '%dir /etc/tdxd.d'
	echo '%dir /usr/lib/tdxd'
	echo '%dir /usr/lib/tdxd/python'
	echo '%dir /usr/share/doc/tdxd'
	(cd "$STAGE/root" && find . \( -type f -o -type l \) ! -path './etc/*' | sed 's#^\.##' | sort)
	} > "$spec"
	rpmbuild -bb --quiet --target "$RPM_ARCH" --define "_topdir $top" \
		--define "_rpmdir $top/RPMS" "$spec" >/dev/null
	local f=$OUT/tdxd-$PV-1.$RPM_ARCH.rpm
	mv "$top/RPMS/$RPM_ARCH/tdxd-$PV-1.$RPM_ARCH.rpm" "$f"
	echo "wrote $f"
}

case $WHAT in
deb) build_deb ;;
rpm) build_rpm ;;
all) build_deb; build_rpm ;;
esac
