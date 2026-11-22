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
	for b in tdxd tdx-lcd tdxctl tdx-lcdmgr tdx-lcd-demo tdx-focus tdx-tray tdx-settings tdx-lcdmanager \
		 tdx-lcd-functionkeys tdx-lcd-rss tdx-lcd-pictures tdx-extract-3dxware-ui; do
		install -Dm755 "$DIST/bin/$b" "$R/usr/bin/$b"
	done
	install -Dm755 "$DIST/lib/tdx-settings-helper" "$R/usr/lib/tdxd/tdx-settings-helper"
	install -Dm755 "$DIST/lib/tdx-focus-kwin" "$R/usr/lib/tdxd/tdx-focus-kwin"
	install -Dm755 "$DIST/bin/tdx-freecad-addon" "$R/usr/bin/tdx-freecad-addon"
	install -Dm644 "$DIST/freecad/tdxd_freecad.py" "$R/usr/share/tdxd/freecad/tdxd_freecad.py"
	install -Dm644 "$DIST/freecad/TdxdSpaceMouse/InitGui.py" "$R/usr/share/tdxd/freecad/TdxdSpaceMouse/InitGui.py"
	install -Dm644 "$DIST/lib/python/tdxlcd.py" "$R/usr/lib/tdxd/python/tdxlcd.py"
	install -Dm644 "$DIST/lib/python/lcdapplet.py" "$R/usr/lib/tdxd/python/lcdapplet.py"
	install -Dm755 "$DIST/lib/libtdxlcd.so.1" "$R$L/libtdxlcd.so.1"
	ln -s libtdxlcd.so.1 "$R$L/libtdxlcd.so"
	install -Dm644 "$DIST/lib/libtdxlcd.a" "$R$L/libtdxlcd.a"
	install -Dm644 "$DIST/include/tdxlcd.h" "$R/usr/include/tdxlcd.h"
	install -d "$R/usr/lib/systemd/system" "$R/usr/lib/systemd/user"
	sed 's#/usr/local/#/usr/#g' "$DIST/tdxd.service" > "$R/usr/lib/systemd/system/tdxd.service"
	for u in tdx-lcdmgr tdx-focus tdx-tray; do
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
	# the settings are not package (config) files: the defaults live here and the
	# install script copies each to /etc only when it is missing (see SETTINGS)
	install -Dm644 "$DIST/tdxd.conf" "$R/usr/share/tdxd/defaults/tdxd.conf"
	install -Dm644 "$DIST/blender.conf.example" "$R/usr/share/tdxd/defaults/blender.conf.example"
	install -Dm644 "$DIST/freecad.conf" "$R/usr/share/tdxd/defaults/freecad.conf"
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
# The settings in /etc are not dpkg/rpm config files: as those, an uninstall that
# deleted them (see CLEANUP) left dpkg remembering them as "removed by the user",
# so a reinstall never brought them back (tdxd ran without its defaults), and an
# upgrade after the settings panel had changed them stopped at a keep-or-replace
# question. Instead the package carries the defaults in /usr/share/tdxd/defaults
# and puts each one in place only where it is missing - your settings stay.
SETTINGS='D=/usr/share/tdxd/defaults
if [ -d "$D" ]; then
	mkdir -p /etc/tdxd.d
	[ -e /etc/tdxd.conf ] || install -m 644 "$D/tdxd.conf" /etc/tdxd.conf
	[ -e /etc/tdxd.d/freecad.conf ] || install -m 644 "$D/freecad.conf" /etc/tdxd.d/freecad.conf
	install -m 644 "$D/blender.conf.example" /etc/tdxd.d/blender.conf.example  # an example: always current
fi'
POST_INSTALL='modprobe uinput 2>/dev/null || true
# the settings panel was "3D Mouse Settings (tdxd)" before 0.3.5: the copies of
# its menu entry that the 3DxWare-look importer made (they win over this
# package entry) get the new name; only that exact line of tdxd own entry
for e in /usr/local/share/applications/tdx-settings.desktop \
	$(getent passwd | cut -d: -f6 | sed "s|$|/.local/share/applications/tdx-settings.desktop|"); do
	[ -f "$e" ] && [ ! -L "$e" ] || continue
	sed -i "s/^Name=3D Mouse Settings (tdxd)\$/Name=3Dconnexion Settings/" "$e" 2>/dev/null || true
done
# a copy of tdxd'"'"'s own rule in /etc (the 0.3.1 workaround, or install.sh) replaces
# the packaged one of the same name, so later fixes never apply: remove copies of
# ours (recognised by the header line); rules of anyone else stay
for r in /etc/udev/rules.d/70-3dconnexion-tdxd.rules /etc/udev/rules.d/99-3dconnexion-tdxd.rules; do
	[ -f "$r" ] || continue
	head -n 1 "$r" | grep -q "^# tdxd: access to 3Dconnexion devices" || continue
	echo "tdxd: removing $r, an older copy of the packaged udev rule" >&2
	rm -f "$r"
done
if command -v udevadm >/dev/null 2>&1; then
	udevadm control --reload-rules 2>/dev/null || true
	udevadm trigger --subsystem-match=hidraw --subsystem-match=usb --subsystem-match=misc \
		--subsystem-match=input 2>/dev/null || true
	udevadm settle 2>/dev/null || true
	# libinput reads LIBINPUT_IGNORE_DEVICE only when a device is added: re-add the
	# 3D mouse event nodes the rule now hides, as a replug would
	for ev in /sys/class/input/event*; do
		udevadm info -q property -p "$ev" 2>/dev/null | grep -q "^LIBINPUT_IGNORE_DEVICE=1" || continue
		case "$(cat "$ev/device/name" 2>/dev/null)" in *3Dconnexion*|*SpaceMouse*|*SpacePilot*|*SpaceNavigator*) ;; *) continue ;; esac
		udevadm trigger --action=remove "$ev" 2>/dev/null || true
		udevadm trigger --action=add "$ev" 2>/dev/null || true
	done
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
	# the name on the LCD; KDE Plasma, sway, X11 - it exits quietly elsewhere), and tdx-tray
	# (the 3Dconnexion icon in the system tray)
	systemctl --global enable tdx-lcdmgr.service tdx-focus.service tdx-tray.service >/dev/null 2>&1 || true
	for u in $(loginctl list-users --no-legend 2>/dev/null | awk "{print \$2}"); do
		systemctl --user -M "$u@" daemon-reload >/dev/null 2>&1 || true
		for s in tdx-lcdmgr tdx-focus tdx-tray; do
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
		systemctl --user -M "$u@" stop tdx-lcdmgr.service tdx-focus.service tdx-tray.service >/dev/null 2>&1 || true
	done
	systemctl --global disable tdx-lcdmgr.service tdx-focus.service tdx-tray.service >/dev/null 2>&1 || true
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
	# "Start with the Desktop" turned off: the masks of tdxd services in each home
	for u in tdx-tray tdx-lcdmgr tdx-focus; do
		m="$home/.config/systemd/user/$u.service"
		[ -L "$m" ] && [ "$(readlink "$m")" = /dev/null ] && rm -f "$m"
	done
	# the FreeCAD add-on loader (tdx-freecad-addon), in each FreeCAD version'"'"'s folder
	fp="$home/.var/app/org.freecad.FreeCAD/data/FreeCAD"
	rm -rf "$home/.local/share/FreeCAD/Mod/TdxdSpaceMouse" "$fp/Mod/TdxdSpaceMouse"
	for m in "$home"/.local/share/FreeCAD/v*/Mod/TdxdSpaceMouse "$fp"/v*/Mod/TdxdSpaceMouse; do
		[ -d "$m" ] && rm -rf "$m"
	done
	# and the Flatpak FreeCAD permission for tdxd socket it added
	o="$home/.local/share/flatpak/overrides/org.freecad.FreeCAD"
	[ -f "$o" ] && sed -i "s|/run/tdxd.sock;||; /^filesystems=\$/d" "$o"
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
	# no conffiles: the settings are copied in by SETTINGS (see there)
	{ echo '#!/bin/sh'; echo 'set -e'
	  echo '[ "$1" = configure ] || exit 0'
	  echo 'if [ -n "${2:-}" ]; then set -- upgrade; else set -- install; fi'
	  echo "$SETTINGS"
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
	echo "$SETTINGS" | sed 's/%/%%/g'
	echo "$POST_INSTALL" | sed 's/%/%%/g'
	echo; echo '%preun'; echo 'if [ "$1" -eq 0 ]; then'; echo "$PRE_REMOVE" | sed 's/%/%%/g'; echo 'fi'
	echo; echo '%postun'; echo "$POST_REMOVE" | sed 's/%/%%/g'
	echo 'if [ "$1" -eq 0 ]; then'; echo "$CLEANUP" | sed 's/%/%%/g'; echo 'fi'
	# after the whole upgrade: an older package listed the settings as %config
	# files, and removing it after the new one's %post may have deleted them
	# (unchanged) or moved them to .rpmsave (changed): bring them back
	echo; echo '%posttrans'
	echo 'for f in /etc/tdxd.conf /etc/tdxd.d/freecad.conf; do'
	echo '	[ -e "$f" ] || [ ! -e "$f.rpmsave" ] || mv "$f.rpmsave" "$f"'
	echo 'done'
	echo "$SETTINGS" | sed 's/%/%%/g'
	echo 'if [ -d /run/systemd/system ]; then systemctl try-restart tdxd.service || true; fi'
	echo; echo '%files'
	echo '%dir /usr/share/tdxd'
	echo '%dir /usr/share/tdxd/defaults'
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
