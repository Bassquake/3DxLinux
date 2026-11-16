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
	rm -f "$PREFIX/bin/tdxd" "$PREFIX/bin/tdx-lcd" \
	      /etc/udev/rules.d/99-3dconnexion-tdxd.rules /etc/systemd/system/tdxd.service
	udevadm control --reload 2>/dev/null || true
	systemctl daemon-reload 2>/dev/null || true
	echo "removed (kept /etc/tdxd.conf)"
	exit 0
fi

install -Dm755 bin/tdxd    "$PREFIX/bin/tdxd"
install -Dm755 bin/tdx-lcd "$PREFIX/bin/tdx-lcd"
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
