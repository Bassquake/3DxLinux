#!/bin/bash
# SPDX-License-Identifier: MIT
# Build the .deb and .rpm packages from a `make dist` tree, so they hold the
# same static binaries as the .tar.gz. Double-clicking either one installs
# 3dxlinuxd with the desktop's software app; the package's scripts then do what
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
HOMEPAGE=https://github.com/Bassquake/3DxLinux
SUMMARY="3Dconnexion driver: SpacePilot PRO, SpaceMouse, LCD Manager and applets"
DESC="3DxLinux drives 3Dconnexion 3D mice (SpacePilot PRO, SpaceMouse Enterprise,
SpaceMouse Pro/Wireless, Giotto, ...) and the CadMouse with its Universal
Receiver on Linux: motion and buttons for spacenavd-compatible applications
(Blender, FreeCAD, ...) and as an evdev device, LCD and LED support,
per-application profiles, 3Dconnexion Settings (a port of the 3DxWare control
panel, with its radial menus, Virtual NumPad and On-Screen Display), and a port
of Logitech's LCD Manager with the Function Keys, RSS Feeds and Picture Viewer
applets."

# ---- stage the files once, as installed under /usr ----------------------
# (on a Linux filesystem: a Windows drive would report every file as 0777)
STAGE=$(mktemp -d /tmp/3dxlinux-pkg.XXXXXX)
trap 'rm -rf "$STAGE"' EXIT
stage() { # stage LIBDIR -> $STAGE/root
	local R=$STAGE/root L=$1
	rm -rf "$R"
	for b in 3dxlinuxd 3dxlinux-lcd 3dxlinuxctl 3dxlinux-lcdmgr 3dxlinux-lcd-demo 3dxlinux-focus 3dxlinux-tray 3dxlinux-radial 3dxlinux-numpad 3dxlinux-osd 3dxlinux-settings 3dxlinux-lcdmanager \
		 3dxlinux-lcd-functionkeys 3dxlinux-lcd-rss 3dxlinux-lcd-pictures 3dxlinux-extract-3dxware-ui; do
		install -Dm755 "$DIST/bin/$b" "$R/usr/bin/$b"
	done
	install -Dm755 "$DIST/lib/3dxlinux-settings-helper" "$R/usr/lib/3dxlinux/3dxlinux-settings-helper"
	install -Dm755 "$DIST/lib/3dxlinux-focus-kwin" "$R/usr/lib/3dxlinux/3dxlinux-focus-kwin"
	install -Dm755 "$DIST/bin/3dxlinux-freecad-addon" "$R/usr/bin/3dxlinux-freecad-addon"
	install -Dm644 "$DIST/freecad/freecad_3dxlinux.py" "$R/usr/share/3dxlinux/freecad/freecad_3dxlinux.py"
	install -Dm644 "$DIST/freecad/SpaceMouse3DxLinux/InitGui.py" "$R/usr/share/3dxlinux/freecad/SpaceMouse3DxLinux/InitGui.py"
	install -Dm644 "$DIST/lib/python/lcd_3dxlinux.py" "$R/usr/lib/3dxlinux/python/lcd_3dxlinux.py"
	install -Dm644 "$DIST/lib/python/lcdapplet.py" "$R/usr/lib/3dxlinux/python/lcdapplet.py"
	install -Dm755 "$DIST/lib/lib3dxlinuxlcd.so.1" "$R$L/lib3dxlinuxlcd.so.1"
	ln -s lib3dxlinuxlcd.so.1 "$R$L/lib3dxlinuxlcd.so"
	install -Dm644 "$DIST/lib/lib3dxlinuxlcd.a" "$R$L/lib3dxlinuxlcd.a"
	install -Dm644 "$DIST/include/3dxlinuxlcd.h" "$R/usr/include/3dxlinuxlcd.h"
	install -d "$R/usr/lib/systemd/system" "$R/usr/lib/systemd/user"
	sed 's#/usr/local/#/usr/#g' "$DIST/3dxlinuxd.service" > "$R/usr/lib/systemd/system/3dxlinuxd.service"
	for u in 3dxlinux-lcdmgr 3dxlinux-focus 3dxlinux-tray; do
		sed 's#/usr/local/bin#/usr/bin#' "$DIST/$u.service" > "$R/usr/lib/systemd/user/$u.service"
	done
	install -Dm644 "$DIST/70-3dconnexion-3dxlinux.rules" "$R/usr/lib/udev/rules.d/70-3dconnexion-3dxlinux.rules"
	install -d "$R/usr/lib/modules-load.d"
	echo uinput > "$R/usr/lib/modules-load.d/3dxlinux.conf"
	for d in "$DIST"/*.desktop; do
		install -Dm644 "$d" "$R/usr/share/applications/$(basename "$d")"
	done
	install -d "$R/usr/share/polkit-1/actions"
	sed 's#/usr/local/lib/3dxlinux#/usr/lib/3dxlinux#' "$DIST/io.github.bassquake.3dxlinux.settings.policy" \
		> "$R/usr/share/polkit-1/actions/io.github.bassquake.3dxlinux.settings.policy"
	# the settings are not package (config) files: the defaults live here and the
	# install script copies each to /etc only when it is missing (see SETTINGS)
	install -Dm644 "$DIST/3dxlinux.conf" "$R/usr/share/3dxlinux/defaults/3dxlinux.conf"
	install -Dm644 "$DIST/blender.conf.example" "$R/usr/share/3dxlinux/defaults/blender.conf.example"
	install -Dm644 "$DIST/freecad.conf" "$R/usr/share/3dxlinux/defaults/freecad.conf"
	for f in README.md PROTOCOL.md; do
		install -Dm644 "$DIST/$f" "$R/usr/share/doc/3dxlinux/$f"
	done
	# AppStream: the name (3DxLinux), developer and licence software centres show
	sed -e "s/@VERSION@/$PV/" -e "s/@DATE@/$(date -u +%Y-%m-%d)/" "$DIST/io.github.bassquake._3DxLinux.metainfo.xml" \
		| install -Dm644 /dev/stdin "$R/usr/share/metainfo/io.github.bassquake._3DxLinux.metainfo.xml"
	printf '3DxLinux - %s\n\nMIT License. Copyright (c) the 3DxLinux authors.\n' "$HOMEPAGE" \
		> "$R/usr/share/doc/3dxlinux/copyright"
	find "$R" -type d -exec chmod 755 {} +
	find "$R/usr/lib/systemd" "$R/usr/share" "$R/usr/lib/modules-load.d" -type f -exec chmod 644 {} +
}

# ---- what the package does after install / before removal -------------------
# $1: "install" or "upgrade"
# The settings in /etc are not dpkg/rpm config files: as those, an uninstall that
# deleted them (see CLEANUP) left dpkg remembering them as "removed by the user",
# so a reinstall never brought them back (3dxlinuxd ran without its defaults), and an
# upgrade after the settings panel had changed them stopped at a keep-or-replace
# question. Instead the package carries the defaults in /usr/share/3dxlinux/defaults
# and puts each one in place only where it is missing - your settings stay.
SETTINGS='D=/usr/share/3dxlinux/defaults
if [ -d "$D" ]; then
	mkdir -p /etc/3dxlinux.d
	[ -e /etc/3dxlinux.conf ] || install -m 644 "$D/3dxlinux.conf" /etc/3dxlinux.conf
	[ -e /etc/3dxlinux.d/freecad.conf ] || install -m 644 "$D/freecad.conf" /etc/3dxlinux.d/freecad.conf
	install -m 644 "$D/blender.conf.example" /etc/3dxlinux.d/blender.conf.example  # an example: always current
fi'
POST_INSTALL='modprobe uinput 2>/dev/null || true
# the LCD Manager window is opened from 3Dconnexion Settings since 1.0, not
# the menu: hide the importer copies of its entry too
for e in /usr/local/share/applications/3dxlinux-lcdmanager.desktop \
	$(getent passwd | cut -d: -f6 | sed "s|$|/.local/share/applications/3dxlinux-lcdmanager.desktop|"); do
	[ -f "$e" ] && [ ! -L "$e" ] || continue
	grep -q "^NoDisplay=" "$e" || echo NoDisplay=true >> "$e" 2>/dev/null || true
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
	systemctl enable 3dxlinuxd.service >/dev/null 2>&1 || true
	# restart, not try-restart: also starts a 3dxlinuxd that was stopped, so after an
	# install or upgrade the new version always runs (seen: an upgrade left it dead)
	systemctl restart 3dxlinuxd.service || true
	# the LCD Manager runs in every user session; (re)start it now for those logged
	# in, unless that user has turned it off
	# and so does 3dxlinux-focus (which app has the focus: per-application profiles and
	# the name on the LCD; KDE Plasma, sway, X11 - it exits quietly elsewhere), and 3dxlinux-tray
	# (the 3Dconnexion icon in the system tray)
	systemctl --global enable 3dxlinux-lcdmgr.service 3dxlinux-focus.service 3dxlinux-tray.service >/dev/null 2>&1 || true
	for u in $(loginctl list-users --no-legend 2>/dev/null | awk "{print \$2}"); do
		systemctl --user -M "$u@" daemon-reload >/dev/null 2>&1 || true
		for s in 3dxlinux-lcdmgr 3dxlinux-focus 3dxlinux-tray; do
			if systemctl --user -M "$u@" is-enabled $s.service >/dev/null 2>&1; then
				systemctl --user -M "$u@" restart $s.service >/dev/null 2>&1 || true
			fi
		done
	done
fi
if [ -x /usr/local/bin/3dxlinuxd ]; then
	echo "3dxlinuxd: an older copy from install.sh is in /usr/local; remove it with: sudo ./install.sh --remove" >&2
fi'
PRE_REMOVE='if [ -d /run/systemd/system ]; then
	for u in $(loginctl list-users --no-legend 2>/dev/null | awk "{print \$2}"); do
		systemctl --user -M "$u@" stop 3dxlinux-lcdmgr.service 3dxlinux-focus.service 3dxlinux-tray.service >/dev/null 2>&1 || true
	done
	systemctl --global disable 3dxlinux-lcdmgr.service 3dxlinux-focus.service 3dxlinux-tray.service >/dev/null 2>&1 || true
	systemctl disable --now 3dxlinuxd.service >/dev/null 2>&1 || true
fi'
POST_REMOVE='if [ -d /run/systemd/system ]; then systemctl daemon-reload || true; fi
if command -v udevadm >/dev/null 2>&1; then udevadm control --reload-rules 2>/dev/null || true; fi'
# Uninstalling (Discover's Uninstall = a remove, not a purge) leaves nothing of
# 3dxlinuxd behind, settings included - the user's choice. Never on an upgrade. Only
# 3dxlinuxd's own, exactly named files: the settings and profiles, and what the
# 3DxWare-look importer wrote system-wide (as root) and in each home.
CLEANUP='rm -f /etc/3dxlinux.conf /etc/3dxlinux.conf.rpmsave /etc/3dxlinux.conf.rpmnew \
	/etc/3dxlinux.conf.dpkg-old /etc/3dxlinux.conf.dpkg-dist /etc/3dxlinux.conf.dpkg-new \
	/etc/3dxlinux.conf.3dxlinux-settings.tmp
rm -rf /etc/3dxlinux.d /usr/local/share/3dxlinux
# Python bytecode caches written when the programs ran (not package files):
# without this /usr/lib/3dxlinux stays behind
rm -rf /usr/lib/3dxlinux/python/__pycache__
rmdir /usr/lib/3dxlinux/python /usr/lib/3dxlinux 2>/dev/null || true
rm -f /usr/local/share/icons/hicolor/*/apps/3dxlinux-*.png
for e in 3dxlinux-settings 3dxlinux-lcdmanager 3dxlinux-lcd-functionkeys 3dxlinux-lcd-rss 3dxlinux-lcd-pictures; do
	rm -f "/usr/local/share/applications/$e.desktop"
done
getent passwd | while IFS=: read -r _name _pw uid _gid _gecos home _shell; do
	case "$uid" in ""|*[!0-9]*) continue ;; esac
	[ "$uid" -eq 0 ] || { [ "$uid" -ge 1000 ] && [ "$uid" -lt 60000 ]; } || continue
	case "$home" in /?*) ;; *) continue ;; esac
	case "$home" in *..*) continue ;; esac
	[ -d "$home" ] || continue
	rm -rf "$home/.config/3dxlinux" "$home/.local/share/3dxlinux"
	# "Start with the Desktop" turned off: the masks of 3dxlinuxd services in each home
	for u in 3dxlinux-tray 3dxlinux-lcdmgr 3dxlinux-focus; do
		m="$home/.config/systemd/user/$u.service"
		[ -L "$m" ] && [ "$(readlink "$m")" = /dev/null ] && rm -f "$m"
	done
	# the FreeCAD add-on loader (3dxlinux-freecad-addon), in each FreeCAD version'"'"'s folder
	fp="$home/.var/app/org.freecad.FreeCAD/data/FreeCAD"
	rm -rf "$home/.local/share/FreeCAD/Mod/SpaceMouse3DxLinux" "$fp/Mod/SpaceMouse3DxLinux"
	for m in "$home"/.local/share/FreeCAD/v*/Mod/SpaceMouse3DxLinux "$fp"/v*/Mod/SpaceMouse3DxLinux; do
		[ -d "$m" ] && rm -rf "$m"
	done
	# and the Flatpak FreeCAD permission for 3dxlinuxd socket it added
	o="$home/.local/share/flatpak/overrides/org.freecad.FreeCAD"
	[ -f "$o" ] && sed -i "s|/run/3dxlinux.sock;||; /^filesystems=\$/d" "$o"
	rm -f "$home"/.local/share/icons/hicolor/*/apps/3dxlinux-*.png
	for e in 3dxlinux-settings 3dxlinux-lcdmanager 3dxlinux-lcd-functionkeys 3dxlinux-lcd-rss 3dxlinux-lcd-pictures; do
		rm -f "$home/.local/share/applications/$e.desktop"
	done
done'

# ---- .deb ---------------------------------------------------------------------
# Recommends are installed by default, and so are theirs: xdg-utils (RSS "open
# in browser") and x11-utils (xprop, for 3dxlinux-focus) pulled in ~60 packages -
# Perl libraries and a terminal emulator - on a minimal system, so they are
# only Suggests. Every desktop has them anyway.
build_deb() {
	stage "/usr/lib/$TRIPLET"
	local R=$STAGE/root D=$STAGE/root/DEBIAN
	install -d "$D"
	cat > "$D/control" <<EOF
Package: 3dxlinux
Version: $PV-1
Architecture: $DEB_ARCH
Maintainer: 3DxLinux project <$HOMEPAGE/issues>
Installed-Size: $(du -sk --exclude=DEBIAN "$R" | cut -f1)
Depends: python3, python3-gi, python3-gi-cairo, gir1.2-gtk-3.0, pkexec | policykit-1
Recommends: p7zip-full, librsvg2-common, gir1.2-gtklayershell-0.1
Suggests: xdg-utils, x11-utils
Conflicts: spacenavd, tdxd
Replaces: tdxd
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
	# Discover shows a package file by its name up to the first dot: "3DxLinux"
	local f=$OUT/3DxLinux.${PV}-1.$DEB_ARCH.deb
	dpkg-deb --root-owner-group -Zxz --build "$R" "$f" >/dev/null
	echo "wrote $f"
}

# ---- .rpm ---------------------------------------------------------------------
build_rpm() {
	stage /usr/lib64
	local top=$STAGE/rpmbuild spec=$STAGE/3dxlinux.spec
	install -d "$top/BUILD" "$top/RPMS" "$top/SPECS" "$top/SRPMS" "$top/SOURCES"
	{
	cat <<EOF
Name: 3dxlinux
Version: $PV
Release: 1
Summary: $SUMMARY
License: MIT
URL: $HOMEPAGE
Requires: python3, python3-gobject, python3-cairo, gtk3, polkit
Recommends: p7zip, librsvg2, gtk-layer-shell
Suggests: xdg-utils, xprop
Conflicts: spacenavd
Obsoletes: tdxd
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
	echo 'for f in /etc/3dxlinux.conf /etc/3dxlinux.d/freecad.conf; do'
	echo '	[ -e "$f" ] || [ ! -e "$f.rpmsave" ] || mv "$f.rpmsave" "$f"'
	echo 'done'
	echo "$SETTINGS" | sed 's/%/%%/g'
	echo 'if [ -d /run/systemd/system ]; then systemctl try-restart 3dxlinuxd.service || true; fi'
	echo; echo '%files'
	echo '%dir /usr/share/3dxlinux'
	echo '%dir /usr/share/3dxlinux/defaults'
	echo '%dir /usr/lib/3dxlinux'
	echo '%dir /usr/lib/3dxlinux/python'
	echo '%dir /usr/share/doc/3dxlinux'
	(cd "$STAGE/root" && find . \( -type f -o -type l \) ! -path './etc/*' | sed 's#^\.##' | sort)
	} > "$spec"
	rpmbuild -bb --quiet --target "$RPM_ARCH" --define "_topdir $top" \
		--define "_rpmdir $top/RPMS" "$spec" >/dev/null
	local f=$OUT/3DxLinux.${PV}-1.$RPM_ARCH.rpm
	mv "$top/RPMS/$RPM_ARCH/3dxlinux-$PV-1.$RPM_ARCH.rpm" "$f"
	echo "wrote $f"
}

case $WHAT in
deb) build_deb ;;
rpm) build_rpm ;;
all) build_deb; build_rpm ;;
esac
