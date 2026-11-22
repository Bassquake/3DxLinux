#!/bin/sh
# Install a prebuilt tdxd package (run from the unpacked directory, as root).
#   sudo ./install.sh            install / upgrade
#   sudo ./install.sh --remove   uninstall (keeps /etc/tdxd.conf)
set -eu
cd "$(dirname "$0")"
PREFIX=${PREFIX:-/usr/local}

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root (sudo ./install.sh)" >&2
	exit 1
fi

if [ "${1:-}" = "--remove" ]; then
	systemctl disable --now tdxd 2>/dev/null || true
	rm -rf "$PREFIX/share/tdxd/freecad"
	rm -f "$PREFIX/bin/tdxd" "$PREFIX/bin/tdx-lcd" "$PREFIX/bin/tdxctl" "$PREFIX/bin/tdx-focus" \
	      "$PREFIX/bin/tdx-tray" /etc/systemd/user/tdx-tray.service \
	      "$PREFIX/share/metainfo/io.github.bassquake.tdxd.metainfo.xml" \
	      "$PREFIX/bin/tdx-freecad-addon" \
	      "$PREFIX/bin/tdx-settings" "$PREFIX/lib/tdxd/tdx-settings-helper" \
	      "$PREFIX/lib/tdxd/tdx-focus-kwin" \
	      "$PREFIX/bin/tdx-extract-3dxware-ui" \
	      "$PREFIX/bin/tdx-lcdmgr" "$PREFIX/bin/tdx-lcdmanager" "$PREFIX/bin/tdx-lcd-demo" \
	      "$PREFIX/lib/tdxd/python/tdxlcd.py" "$PREFIX/lib/tdxd/python/lcdapplet.py" \
	      "$PREFIX/bin/tdx-lcd-functionkeys" "$PREFIX/bin/tdx-lcd-rss" "$PREFIX/bin/tdx-lcd-pictures" \
	      "$PREFIX/lib/libtdxlcd.a" "$PREFIX/lib/libtdxlcd.so" \
	      "$PREFIX/lib/libtdxlcd.so.1" "$PREFIX/include/tdxlcd.h" /etc/systemd/user/tdx-lcdmgr.service \
	      "$PREFIX/share/applications/tdx-settings.desktop" \
	      "$PREFIX/share/applications/tdx-lcdmanager.desktop" \
	      "$PREFIX/share/applications/tdx-lcd-functionkeys.desktop" \
	      "$PREFIX/share/applications/tdx-lcd-rss.desktop" \
	      "$PREFIX/share/applications/tdx-lcd-pictures.desktop" \
	      /usr/share/polkit-1/actions/org.tdxd.settings.policy \
	      /etc/udev/rules.d/70-3dconnexion-tdxd.rules /etc/udev/rules.d/99-3dconnexion-tdxd.rules \
	      /etc/systemd/system/tdxd.service \
	      /etc/systemd/user/tdx-focus.service /etc/tdxd.d/blender.conf.example
	rmdir "$PREFIX/lib/tdxd/python" "$PREFIX/lib/tdxd" 2>/dev/null || true
	udevadm control --reload 2>/dev/null || true
	systemctl daemon-reload 2>/dev/null || true
	echo "removed (kept /etc/tdxd.conf and any profiles in /etc/tdxd.d)"
	exit 0
fi

install -Dm755 bin/tdxd    "$PREFIX/bin/tdxd"
install -Dm755 bin/tdx-lcd "$PREFIX/bin/tdx-lcd"
install -Dm755 bin/tdxctl  "$PREFIX/bin/tdxctl"
install -Dm755 bin/tdx-focus "$PREFIX/bin/tdx-focus"
install -Dm755 bin/tdx-tray "$PREFIX/bin/tdx-tray"   # the 3Dconnexion icon in the system tray
# AppStream: the name (3DxLinux), developer and licence software centres show
sed -e "s/@VERSION@/$(cut -d" " -f1 VERSION | sed s/^v//)/" -e "s/@DATE@/$(date -u +%Y-%m-%d)/" \
	io.github.bassquake.tdxd.metainfo.xml | install -Dm644 /dev/stdin "$PREFIX/share/metainfo/io.github.bassquake.tdxd.metainfo.xml"
# FreeCAD add-on: the keys follow the active workbench (tdx-freecad-addon puts
# its loader into each user's FreeCAD folder, at login via tdx-focus)
install -Dm755 bin/tdx-freecad-addon "$PREFIX/bin/tdx-freecad-addon"
install -Dm644 freecad/tdxd_freecad.py "$PREFIX/share/tdxd/freecad/tdxd_freecad.py"
install -Dm644 freecad/TdxdSpaceMouse/InitGui.py "$PREFIX/share/tdxd/freecad/TdxdSpaceMouse/InitGui.py"
# settings panel (Python 3 + GTK 3: python3-gi, gir1.2-gtk-3.0, python3-gi-cairo)
install -Dm755 bin/tdx-settings "$PREFIX/bin/tdx-settings"
install -Dm755 lib/tdx-settings-helper "$PREFIX/lib/tdxd/tdx-settings-helper"
install -Dm755 lib/tdx-focus-kwin "$PREFIX/lib/tdxd/tdx-focus-kwin"   # focus tracking on KDE Plasma
install -Dm755 bin/tdx-extract-3dxware-ui "$PREFIX/bin/tdx-extract-3dxware-ui"
# LCD Manager (Linux LCDMon), its window, the applet library and a demo applet
install -Dm755 bin/tdx-lcdmgr "$PREFIX/bin/tdx-lcdmgr"
install -Dm755 bin/tdx-lcdmanager "$PREFIX/bin/tdx-lcdmanager"
install -Dm755 bin/tdx-lcd-demo "$PREFIX/bin/tdx-lcd-demo"
install -Dm644 lib/python/tdxlcd.py "$PREFIX/lib/tdxd/python/tdxlcd.py"
install -Dm644 lib/python/lcdapplet.py "$PREFIX/lib/tdxd/python/lcdapplet.py"
for a in tdx-lcd-functionkeys tdx-lcd-rss tdx-lcd-pictures; do
	install -Dm755 "bin/$a" "$PREFIX/bin/$a"
done
install -Dm644 lib/libtdxlcd.a "$PREFIX/lib/libtdxlcd.a"
install -Dm755 lib/libtdxlcd.so.1 "$PREFIX/lib/libtdxlcd.so.1"
ln -sf libtdxlcd.so.1 "$PREFIX/lib/libtdxlcd.so"
install -Dm644 include/tdxlcd.h "$PREFIX/include/tdxlcd.h"
sed "s#/usr/local/bin#$PREFIX/bin#" tdx-lcdmgr.service > /etc/systemd/user/tdx-lcdmgr.service
chmod 644 /etc/systemd/user/tdx-lcdmgr.service
ldconfig 2>/dev/null || true
for d in tdx-settings tdx-lcdmanager tdx-lcd-functionkeys tdx-lcd-rss tdx-lcd-pictures; do
	install -Dm644 "$d.desktop" "$PREFIX/share/applications/$d.desktop"
done
sed "s#/usr/local/lib/tdxd#$PREFIX/lib/tdxd#" org.tdxd.settings.policy \
	| install -Dm644 /dev/stdin /usr/share/polkit-1/actions/org.tdxd.settings.policy
mkdir -p /etc/systemd/user /etc/tdxd.d
sed "s#/usr/local/bin#$PREFIX/bin#" tdx-focus.service > /etc/systemd/user/tdx-focus.service
chmod 644 /etc/systemd/user/tdx-focus.service
sed "s#/usr/local/bin#$PREFIX/bin#" tdx-tray.service > /etc/systemd/user/tdx-tray.service
chmod 644 /etc/systemd/user/tdx-tray.service
install -Dm644 blender.conf.example /etc/tdxd.d/blender.conf.example
[ -e /etc/tdxd.d/freecad.conf ] || install -Dm644 freecad.conf /etc/tdxd.d/freecad.conf  # FreeCAD's view keys
install -Dm644 70-3dconnexion-tdxd.rules /etc/udev/rules.d/70-3dconnexion-tdxd.rules
rm -f /etc/udev/rules.d/99-3dconnexion-tdxd.rules   # its name before 0.3.2: too late for uaccess
sed "s#/usr/local/bin/tdxd#$PREFIX/bin/tdxd#" tdxd.service > /etc/systemd/system/tdxd.service
chmod 644 /etc/systemd/system/tdxd.service
[ -e /etc/tdxd.conf ] || install -Dm644 tdxd.conf /etc/tdxd.conf

modprobe uinput 2>/dev/null || true
udevadm control --reload
udevadm trigger
udevadm settle 2>/dev/null || true
# libinput reads LIBINPUT_IGNORE_DEVICE only when a device is added: re-add the
# 3D mouse event nodes the rule now hides, so the cursor stops following the cap
for ev in /sys/class/input/event*; do
	udevadm info -q property -p "$ev" 2>/dev/null | grep -q "^LIBINPUT_IGNORE_DEVICE=1" || continue
	case "$(cat "$ev/device/name" 2>/dev/null)" in *3Dconnexion*|*SpaceMouse*|*SpacePilot*|*SpaceNavigator*) ;; *) continue ;; esac
	udevadm trigger --action=remove "$ev" 2>/dev/null || true
	udevadm trigger --action=add "$ev" 2>/dev/null || true
done
systemctl daemon-reload

echo "installed tdxd $(cat VERSION 2>/dev/null) to $PREFIX/bin"
echo "  try it in the foreground:  sudo $PREFIX/bin/tdxd -v -v"
echo "  or as a service:           sudo systemctl enable --now tdxd"
echo "  (stop spacenavd first if it is running: sudo systemctl disable --now spacenavd)"
echo "  calibrate (re-zero) the cap:   tdxctl calibrate"
echo "  per-application profiles:      /etc/tdxd.d/*.conf (see blender.conf.example)"
echo "  focus tracking (KDE Plasma, sway, X11), as your user:  systemctl --user enable --now tdx-focus"
echo "  the icon in the system tray, as your user:            systemctl --user enable --now tdx-tray"
echo "  settings panel:                tdx-settings   (needs python3-gi, gir1.2-gtk-3.0, python3-gi-cairo)"
echo "  LCD Manager (LCD applets), as your user:  systemctl --user enable --now tdx-lcdmgr"
echo "  3DxWare look for the panel:    sudo tdx-extract-3dxware-ui 3DxWare64_v10-9-14_b745.exe   (needs 7z)"
