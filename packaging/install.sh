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
	rm -f "$PREFIX/bin/tdxd" "$PREFIX/bin/tdx-lcd" "$PREFIX/bin/tdxctl" "$PREFIX/bin/tdx-focus" \
	      "$PREFIX/bin/tdx-settings" "$PREFIX/lib/tdxd/tdx-settings-helper" \
	      "$PREFIX/bin/tdx-extract-3dxware-ui" \
	      "$PREFIX/bin/tdx-lcdmgr" "$PREFIX/bin/tdx-lcdmanager" "$PREFIX/bin/tdx-lcd-demo" \
	      "$PREFIX/lib/tdxd/python/tdxlcd.py" "$PREFIX/lib/libtdxlcd.a" "$PREFIX/lib/libtdxlcd.so" \
	      "$PREFIX/lib/libtdxlcd.so.1" "$PREFIX/include/tdxlcd.h" /etc/systemd/user/tdx-lcdmgr.service \
	      "$PREFIX/share/applications/tdx-settings.desktop" \
	      /usr/share/polkit-1/actions/org.tdxd.settings.policy \
	      /etc/udev/rules.d/99-3dconnexion-tdxd.rules /etc/systemd/system/tdxd.service \
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
# settings panel (Python 3 + GTK 3: python3-gi, gir1.2-gtk-3.0, python3-gi-cairo)
install -Dm755 bin/tdx-settings "$PREFIX/bin/tdx-settings"
install -Dm755 lib/tdx-settings-helper "$PREFIX/lib/tdxd/tdx-settings-helper"
install -Dm755 bin/tdx-extract-3dxware-ui "$PREFIX/bin/tdx-extract-3dxware-ui"
# LCD Manager (Linux LCDMon), its window, the applet library and a demo applet
install -Dm755 bin/tdx-lcdmgr "$PREFIX/bin/tdx-lcdmgr"
install -Dm755 bin/tdx-lcdmanager "$PREFIX/bin/tdx-lcdmanager"
install -Dm755 bin/tdx-lcd-demo "$PREFIX/bin/tdx-lcd-demo"
install -Dm644 lib/python/tdxlcd.py "$PREFIX/lib/tdxd/python/tdxlcd.py"
install -Dm644 lib/libtdxlcd.a "$PREFIX/lib/libtdxlcd.a"
install -Dm755 lib/libtdxlcd.so.1 "$PREFIX/lib/libtdxlcd.so.1"
ln -sf libtdxlcd.so.1 "$PREFIX/lib/libtdxlcd.so"
install -Dm644 include/tdxlcd.h "$PREFIX/include/tdxlcd.h"
sed "s#/usr/local/bin#$PREFIX/bin#" tdx-lcdmgr.service > /etc/systemd/user/tdx-lcdmgr.service
chmod 644 /etc/systemd/user/tdx-lcdmgr.service
ldconfig 2>/dev/null || true
install -Dm644 tdx-settings.desktop "$PREFIX/share/applications/tdx-settings.desktop"
sed "s#/usr/local/lib/tdxd#$PREFIX/lib/tdxd#" org.tdxd.settings.policy \
	| install -Dm644 /dev/stdin /usr/share/polkit-1/actions/org.tdxd.settings.policy
mkdir -p /etc/systemd/user /etc/tdxd.d
sed "s#/usr/local/bin#$PREFIX/bin#" tdx-focus.service > /etc/systemd/user/tdx-focus.service
chmod 644 /etc/systemd/user/tdx-focus.service
install -Dm644 blender.conf.example /etc/tdxd.d/blender.conf.example
install -Dm644 99-3dconnexion-tdxd.rules /etc/udev/rules.d/99-3dconnexion-tdxd.rules
sed "s#/usr/local/bin/tdxd#$PREFIX/bin/tdxd#" tdxd.service > /etc/systemd/system/tdxd.service
chmod 644 /etc/systemd/system/tdxd.service
[ -e /etc/tdxd.conf ] || install -Dm644 tdxd.conf /etc/tdxd.conf

modprobe uinput 2>/dev/null || true
udevadm control --reload
udevadm trigger
systemctl daemon-reload

echo "installed tdxd $(cat VERSION 2>/dev/null) to $PREFIX/bin"
echo "  try it in the foreground:  sudo $PREFIX/bin/tdxd -v -v"
echo "  or as a service:           sudo systemctl enable --now tdxd"
echo "  (stop spacenavd first if it is running: sudo systemctl disable --now spacenavd)"
echo "  calibrate (re-zero) the cap:   tdxctl calibrate"
echo "  per-application profiles:      /etc/tdxd.d/*.conf (see blender.conf.example)"
echo "  focus tracking (X11 / sway), as your user:  systemctl --user enable --now tdx-focus"
echo "  settings panel:                tdx-settings   (needs python3-gi, gir1.2-gtk-3.0, python3-gi-cairo)"
echo "  LCD Manager (LCD applets), as your user:  systemctl --user enable --now tdx-lcdmgr"
echo "  3DxWare look for the panel:    sudo tdx-extract-3dxware-ui 3DxWare64_v10-9-14_b745.exe   (needs 7z)"
