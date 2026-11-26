#!/bin/sh
# Install a prebuilt 3DxLinux package (run from the unpacked directory, as root).
#   sudo ./install.sh            install / upgrade
#   sudo ./install.sh --remove   uninstall (keeps /etc/3dxlinux.conf)
set -eu
cd "$(dirname "$0")"
PREFIX=${PREFIX:-/usr/local}

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root (sudo ./install.sh)" >&2
	exit 1
fi

if [ "${1:-}" = "--remove" ]; then
	systemctl disable --now 3dxlinuxd 2>/dev/null || true
	rm -rf "$PREFIX/share/3dxlinux/freecad"
	rm -f "$PREFIX/bin/3dxlinuxd" "$PREFIX/bin/3dxlinux-lcd" "$PREFIX/bin/3dxlinuxctl" "$PREFIX/bin/3dxlinux-focus" \
	      "$PREFIX/bin/3dxlinux-tray" "$PREFIX/bin/3dxlinux-radial" "$PREFIX/bin/3dxlinux-numpad" "$PREFIX/bin/3dxlinux-osd" /etc/systemd/user/3dxlinux-tray.service \
	      "$PREFIX/share/metainfo/io.github.bassquake._3DxLinux.metainfo.xml" \
	      "$PREFIX/bin/3dxlinux-freecad-addon" \
	      "$PREFIX/bin/3dxlinux-settings" "$PREFIX/lib/3dxlinux/3dxlinux-settings-helper" \
	      "$PREFIX/lib/3dxlinux/3dxlinux-focus-kwin" \
	      "$PREFIX/bin/3dxlinux-extract-3dxware-ui" \
	      "$PREFIX/bin/3dxlinux-lcdmgr" "$PREFIX/bin/3dxlinux-lcdmanager" "$PREFIX/bin/3dxlinux-lcd-demo" \
	      "$PREFIX/lib/3dxlinux/python/lcd_3dxlinux.py" "$PREFIX/lib/3dxlinux/python/lcdapplet.py" \
	      "$PREFIX/bin/3dxlinux-lcd-functionkeys" "$PREFIX/bin/3dxlinux-lcd-rss" "$PREFIX/bin/3dxlinux-lcd-pictures" \
	      "$PREFIX/lib/lib3dxlinuxlcd.a" "$PREFIX/lib/lib3dxlinuxlcd.so" \
	      "$PREFIX/lib/lib3dxlinuxlcd.so.1" "$PREFIX/include/3dxlinuxlcd.h" /etc/systemd/user/3dxlinux-lcdmgr.service \
	      "$PREFIX/share/applications/3dxlinux-settings.desktop" \
	      "$PREFIX/share/applications/3dxlinux-lcdmanager.desktop" \
	      "$PREFIX/share/applications/3dxlinux-lcd-functionkeys.desktop" \
	      "$PREFIX/share/applications/3dxlinux-lcd-rss.desktop" \
	      "$PREFIX/share/applications/3dxlinux-lcd-pictures.desktop" \
	      /usr/share/polkit-1/actions/io.github.bassquake.3dxlinux.settings.policy \
	      /etc/udev/rules.d/70-3dconnexion-3dxlinux.rules \
	      /etc/systemd/system/3dxlinuxd.service \
	      /etc/systemd/user/3dxlinux-focus.service /etc/3dxlinux.d/blender.conf.example
	rmdir "$PREFIX/lib/3dxlinux/python" "$PREFIX/lib/3dxlinux" 2>/dev/null || true
	udevadm control --reload 2>/dev/null || true
	systemctl daemon-reload 2>/dev/null || true
	echo "removed (kept /etc/3dxlinux.conf and any profiles in /etc/3dxlinux.d)"
	exit 0
fi

install -Dm755 bin/3dxlinuxd    "$PREFIX/bin/3dxlinuxd"
install -Dm755 bin/3dxlinux-lcd "$PREFIX/bin/3dxlinux-lcd"
install -Dm755 bin/3dxlinuxctl  "$PREFIX/bin/3dxlinuxctl"
install -Dm755 bin/3dxlinux-focus "$PREFIX/bin/3dxlinux-focus"
install -Dm755 bin/3dxlinux-tray "$PREFIX/bin/3dxlinux-tray"   # the 3Dconnexion icon in the system tray
install -Dm755 bin/3dxlinux-radial "$PREFIX/bin/3dxlinux-radial"   # the Common Tools radial menu
install -Dm755 bin/3dxlinux-numpad "$PREFIX/bin/3dxlinux-numpad"   # the Virtual NumPad
install -Dm755 bin/3dxlinux-osd "$PREFIX/bin/3dxlinux-osd"   # the CadMouse On-Screen Display
# AppStream: the name (3DxLinux), developer and licence software centres show
sed -e "s/@VERSION@/$(cut -d" " -f1 VERSION | sed s/^v//)/" -e "s/@DATE@/$(date -u +%Y-%m-%d)/" \
	io.github.bassquake._3DxLinux.metainfo.xml | install -Dm644 /dev/stdin "$PREFIX/share/metainfo/io.github.bassquake._3DxLinux.metainfo.xml"
# FreeCAD add-on: the keys follow the active workbench (3dxlinux-freecad-addon puts
# its loader into each user's FreeCAD folder, at login via 3dxlinux-focus)
install -Dm755 bin/3dxlinux-freecad-addon "$PREFIX/bin/3dxlinux-freecad-addon"
install -Dm644 freecad/freecad_3dxlinux.py "$PREFIX/share/3dxlinux/freecad/freecad_3dxlinux.py"
install -Dm644 freecad/SpaceMouse3DxLinux/InitGui.py "$PREFIX/share/3dxlinux/freecad/SpaceMouse3DxLinux/InitGui.py"
# settings panel (Python 3 + GTK 3: python3-gi, gir1.2-gtk-3.0, python3-gi-cairo)
install -Dm755 bin/3dxlinux-settings "$PREFIX/bin/3dxlinux-settings"
install -Dm755 lib/3dxlinux-settings-helper "$PREFIX/lib/3dxlinux/3dxlinux-settings-helper"
install -Dm755 lib/3dxlinux-focus-kwin "$PREFIX/lib/3dxlinux/3dxlinux-focus-kwin"   # focus tracking on KDE Plasma
install -Dm755 bin/3dxlinux-extract-3dxware-ui "$PREFIX/bin/3dxlinux-extract-3dxware-ui"
# LCD Manager (Linux LCDMon), its window, the applet library and a demo applet
install -Dm755 bin/3dxlinux-lcdmgr "$PREFIX/bin/3dxlinux-lcdmgr"
install -Dm755 bin/3dxlinux-lcdmanager "$PREFIX/bin/3dxlinux-lcdmanager"
install -Dm755 bin/3dxlinux-lcd-demo "$PREFIX/bin/3dxlinux-lcd-demo"
install -Dm644 lib/python/lcd_3dxlinux.py "$PREFIX/lib/3dxlinux/python/lcd_3dxlinux.py"
install -Dm644 lib/python/lcdapplet.py "$PREFIX/lib/3dxlinux/python/lcdapplet.py"
for a in 3dxlinux-lcd-functionkeys 3dxlinux-lcd-rss 3dxlinux-lcd-pictures; do
	install -Dm755 "bin/$a" "$PREFIX/bin/$a"
done
install -Dm644 lib/lib3dxlinuxlcd.a "$PREFIX/lib/lib3dxlinuxlcd.a"
install -Dm755 lib/lib3dxlinuxlcd.so.1 "$PREFIX/lib/lib3dxlinuxlcd.so.1"
ln -sf lib3dxlinuxlcd.so.1 "$PREFIX/lib/lib3dxlinuxlcd.so"
install -Dm644 include/3dxlinuxlcd.h "$PREFIX/include/3dxlinuxlcd.h"
sed "s#/usr/local/bin#$PREFIX/bin#" 3dxlinux-lcdmgr.service > /etc/systemd/user/3dxlinux-lcdmgr.service
chmod 644 /etc/systemd/user/3dxlinux-lcdmgr.service
ldconfig 2>/dev/null || true
for d in 3dxlinux-settings 3dxlinux-lcdmanager 3dxlinux-lcd-functionkeys 3dxlinux-lcd-rss 3dxlinux-lcd-pictures; do
	install -Dm644 "$d.desktop" "$PREFIX/share/applications/$d.desktop"
done
sed "s#/usr/local/lib/3dxlinux#$PREFIX/lib/3dxlinux#" io.github.bassquake.3dxlinux.settings.policy \
	| install -Dm644 /dev/stdin /usr/share/polkit-1/actions/io.github.bassquake.3dxlinux.settings.policy
mkdir -p /etc/systemd/user /etc/3dxlinux.d
sed "s#/usr/local/bin#$PREFIX/bin#" 3dxlinux-focus.service > /etc/systemd/user/3dxlinux-focus.service
chmod 644 /etc/systemd/user/3dxlinux-focus.service
sed "s#/usr/local/bin#$PREFIX/bin#" 3dxlinux-tray.service > /etc/systemd/user/3dxlinux-tray.service
chmod 644 /etc/systemd/user/3dxlinux-tray.service
install -Dm644 blender.conf.example /etc/3dxlinux.d/blender.conf.example
[ -e /etc/3dxlinux.d/freecad.conf ] || install -Dm644 freecad.conf /etc/3dxlinux.d/freecad.conf  # FreeCAD's view keys
install -Dm644 70-3dconnexion-3dxlinux.rules /etc/udev/rules.d/70-3dconnexion-3dxlinux.rules
sed "s#/usr/local/bin/3dxlinuxd#$PREFIX/bin/3dxlinuxd#" 3dxlinuxd.service > /etc/systemd/system/3dxlinuxd.service
chmod 644 /etc/systemd/system/3dxlinuxd.service
[ -e /etc/3dxlinux.conf ] || install -Dm644 3dxlinux.conf /etc/3dxlinux.conf

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

echo "installed 3dxlinuxd $(cat VERSION 2>/dev/null) to $PREFIX/bin"
echo "  try it in the foreground:  sudo $PREFIX/bin/3dxlinuxd -v -v"
echo "  or as a service:           sudo systemctl enable --now 3dxlinuxd"
echo "  (stop spacenavd first if it is running: sudo systemctl disable --now spacenavd)"
echo "  calibrate (re-zero) the cap:   3dxlinuxctl calibrate"
echo "  per-application profiles:      /etc/3dxlinux.d/*.conf (see blender.conf.example)"
echo "  focus tracking (KDE Plasma, sway, X11), as your user:  systemctl --user enable --now 3dxlinux-focus"
echo "  the icon in the system tray, as your user:            systemctl --user enable --now 3dxlinux-tray"
echo "  settings panel:                3dxlinux-settings   (needs python3-gi, gir1.2-gtk-3.0, python3-gi-cairo)"
echo "  LCD Manager (LCD applets), as your user:  systemctl --user enable --now 3dxlinux-lcdmgr"
echo "  3DxWare look for the panel:    sudo 3dxlinux-extract-3dxware-ui 3DxWare64_v10-9-14_b745.exe   (needs 7z)"
