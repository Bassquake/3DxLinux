# SPDX-License-Identifier: MIT
"""lcd_3dxlinux - Python client for 3dxlinux-lcdmgr (the Linux LCD Manager).

Applets: the same model as Logitech's lglcd API, in Python:

    import lcd_3dxlinux
    conn = lcd_3dxlinux.Connection("My Applet", persistent=True, autostart=True,
                             capabilities=lcd_3dxlinux.APPLET_CAP_QVGA,
                             on_configure=..., on_notify=...)
    dev = conn.open(lcd_3dxlinux.DEVICE_QVGA, on_buttons=lambda buttons: ...)
    dev.update(bgra_bytes, lcd_3dxlinux.PRIORITY_NORMAL)   # 320*240*4 bytes, B G R A
    dev.update_rgb(rgb_bytes)                        # 320*240*3 bytes, R G B
    dev.set_foreground(True)
    dev.close(); conn.close()

Administration (the LCD Manager window): admin("status"), admin("set delay 5"), ...

Wire protocol: src/lcdmgr_proto.h.
"""

import os
import socket
import struct
import threading
import traceback

VERSION = 1
CONNECT, DISCONNECT, ENUMERATE, OPEN, CLOSE, UPDATE, FOREGROUND, READBUTTONS, FAMILIES, ICON = range(1, 11)
EV_BUTTONS, EV_CONFIGURE, EV_NOTIFY = 64, 65, 66
REPLY, ADMIN = 128, 200

CF_PERSISTENT, CF_AUTOSTART, CF_CONFIGURABLE, CF_NOTIFY = 1, 2, 4, 8
APPLET_CAP_BASIC, APPLET_CAP_BW, APPLET_CAP_QVGA = 0, 1, 2
DEVICE_BW, DEVICE_QVGA = 1, 2
BMP_FORMAT_160x43x1, BMP_FORMAT_QVGAx32 = 1, 3
PRIORITY_IDLE_NO_SHOW, PRIORITY_BACKGROUND, PRIORITY_NORMAL, PRIORITY_ALERT = 0, 64, 128, 255
SYNC = 0x80000000
BUTTON_LEFT, BUTTON_RIGHT, BUTTON_OK, BUTTON_CANCEL = 0x100, 0x200, 0x400, 0x800
BUTTON_UP, BUTTON_DOWN, BUTTON_MENU = 0x1000, 0x2000, 0x4000
(NOTIFICATION_DEVICE_ARRIVAL, NOTIFICATION_DEVICE_REMOVAL, NOTIFICATION_CLOSE_CONNECTION,
 NOTIFICATION_APPLET_DISABLED, NOTIFICATION_APPLET_ENABLED,
 NOTIFICATION_TERMINATE_APPLET) = range(1, 7)
ERROR_FILE_NOT_FOUND, ERROR_ACCESS_DENIED, ERROR_DEVICE_NOT_CONNECTED = 2, 5, 1167
W, H = 320, 240


class LcdError(Exception):
    def __init__(self, op, code):
        super().__init__("%s failed: error %d" % (op, code))
        self.code = code


def socket_path():
    p = os.environ.get("DXLINUX_LCD_SOCKET")
    if p:
        return p
    rt = os.environ.get("XDG_RUNTIME_DIR")
    return os.path.join(rt, "3dxlinux-lcd.sock") if rt else "/tmp/3dxlinux-lcd-%d.sock" % os.getuid()


def _recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("3dxlinux-lcdmgr closed the connection")
        buf += chunk
    return bytes(buf)


class Connection:
    def __init__(self, name, persistent=False, autostart=False, capabilities=APPLET_CAP_QVGA,
                 on_configure=None, on_notify=None, path=None):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self.sock.connect(path or socket_path())
        except OSError:
            raise LcdError("connect", ERROR_FILE_NOT_FOUND)
        self.on_configure = on_configure
        self.on_notify = on_notify
        self.devices = {}
        self._lock = threading.Lock()
        self._cv = threading.Condition()
        self._reply = None
        self._dead = False
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()
        flags = ((CF_PERSISTENT if persistent else 0) | (CF_AUTOSTART if autostart else 0)
                 | (CF_CONFIGURABLE if on_configure else 0) | (CF_NOTIFY if on_notify else 0))
        self.id, = struct.unpack("<I", self._request(
            CONNECT, struct.pack("<III", VERSION, capabilities, flags) + name.encode()[:127])[:4])

    # ---- plumbing
    @staticmethod
    def _callback(fn, *args):
        """Run an applet callback; its errors must not look like a lost connection."""
        try:
            fn(*args)
        except Exception:  # noqa: BLE001
            traceback.print_exc()

    def _reader(self):
        try:
            while True:
                t, n = struct.unpack("<II", _recv_exact(self.sock, 8))
                data = _recv_exact(self.sock, n) if n else b""
                if t == REPLY:
                    with self._cv:
                        self._reply = data
                        self._cv.notify_all()
                elif t == EV_BUTTONS:
                    dev, b = struct.unpack("<II", data[:8])
                    d = self.devices.get(dev)
                    if d:
                        d.buttons = b
                        if d.on_buttons:
                            self._callback(d.on_buttons, b)
                elif t == EV_CONFIGURE and self.on_configure:
                    self._callback(self.on_configure)
                elif t == EV_NOTIFY and self.on_notify:
                    self._callback(self.on_notify, *struct.unpack("<5I", data[:20]))
        except (OSError, ConnectionError, struct.error):
            pass
        with self._cv:
            self._dead = True
            self._cv.notify_all()
        try:  # make sure the manager drops this connection too
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        if self.on_notify:
            self._callback(self.on_notify, NOTIFICATION_CLOSE_CONNECTION, 0, 0, 0, 0)

    def _request(self, t, payload=b"", op=None):
        with self._lock:
            with self._cv:
                if self._dead:
                    raise LcdError(op or str(t), 1062)
                self._reply = None
            self.sock.sendall(struct.pack("<II", t, len(payload)) + payload)
            with self._cv:
                while self._reply is None and not self._dead:
                    self._cv.wait()
                r = self._reply
            if r is None:
                raise LcdError(op or str(t), 1062)
            _req, status = struct.unpack("<II", r[:8])
            if status:
                raise LcdError(op or str(t), status)
            return r[8:]

    # ---- API
    def enumerate(self, index=0):
        d = self._request(ENUMERATE, struct.pack("<I", index), "enumerate")
        fam, w, h, bpp, sb = struct.unpack("<5I", d[:20])
        return {"family": fam, "width": w, "height": h, "bpp": bpp, "softbuttons": sb,
                "name": d[20:84].split(b"\0")[0].decode(errors="replace")}

    def set_icon(self, width, height, rgba):
        """Icon for the LCD's program list (<= 64x64, RGBA bytes)."""
        self._request(ICON, struct.pack("<II", width, height) + bytes(rgba), "icon")

    def set_icon_png(self, path):
        """set_icon() from a PNG file (needs pycairo)."""
        import cairo
        s = cairo.ImageSurface.create_from_png(path)
        w, h, st, d = s.get_width(), s.get_height(), s.get_stride(), bytes(s.get_data())
        out = bytearray(w * h * 4)
        for y in range(h):
            for x in range(w):
                b, g, r, a = d[y * st + x * 4:y * st + x * 4 + 4]  # premultiplied BGRA
                if a:
                    r, g, b = (min(255, c * 255 // a) for c in (r, g, b))
                out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((r, g, b, a))
        self.set_icon(w, h, out)

    def open(self, device_type=DEVICE_QVGA, on_buttons=None):
        dev = Device(self, on_buttons)
        srv, = struct.unpack("<I", self._request(OPEN, struct.pack("<II", 1, device_type),
                                                 "open")[:4])
        dev.id = srv
        self.devices[srv] = dev
        return dev

    def close(self):
        try:
            self._request(DISCONNECT, op="disconnect")
        except (LcdError, OSError):
            pass
        try:
            self.sock.close()
        except OSError:
            pass


class Device:
    def __init__(self, conn, on_buttons):
        self.conn = conn
        self.on_buttons = on_buttons
        self.buttons = 0
        self.id = None

    def update(self, bgra, priority=PRIORITY_NORMAL, sync=False):
        if len(bgra) != W * H * 4:
            raise ValueError("expected %d bytes of BGRA" % (W * H * 4))
        p = priority | (SYNC if sync else 0)
        self.conn._request(UPDATE, struct.pack("<III", self.id, p, BMP_FORMAT_QVGAx32)
                           + bytes(bgra), "update")

    def update_rgb(self, rgb, priority=PRIORITY_NORMAL, sync=False):
        if len(rgb) != W * H * 3:
            raise ValueError("expected %d bytes of RGB" % (W * H * 3))
        bgra = bytearray(W * H * 4)
        bgra[0::4] = rgb[2::3]
        bgra[1::4] = rgb[1::3]
        bgra[2::4] = rgb[0::3]
        bgra[3::4] = b"\xff" * (W * H)
        self.update(bgra, priority, sync)

    def set_foreground(self, yes=True):
        self.conn._request(FOREGROUND, struct.pack("<II", self.id, 1 if yes else 0), "foreground")

    def read_soft_buttons(self):
        return struct.unpack("<I", self.conn._request(READBUTTONS, struct.pack("<I", self.id),
                                                      "readbuttons")[:4])[0]

    def close(self):
        self.conn._request(CLOSE, struct.pack("<I", self.id), "close")
        self.conn.devices.pop(self.id, None)


def admin(command, path=None, timeout=3.0):
    """Send one LCD Manager administration command; returns the text reply."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(path or socket_path())
        data = command.encode()
        s.sendall(struct.pack("<II", ADMIN, len(data)) + data)
        t, n = struct.unpack("<II", _recv_exact(s, 8))
        body = _recv_exact(s, n)
        return body[8:].decode(errors="replace")
    finally:
        s.close()


def status(path=None):
    """Parsed `status`: {'device': ..., 'settings': {...}, 'general': {...}, 'programs': [...]}"""
    out = {"device": None, "present": False, "settings": {}, "general": {}, "programs": [],
           "shown": -1, "manual": -1}
    for line in admin("status", path).splitlines():
        key, _, rest = line.partition(" ")
        if key == "device":
            state, _, name = rest.partition(" ")
            out["present"] = state == "present"
            out["device"] = name
        elif key in ("settings", "general"):
            out[key] = {k: int(v) for k, v in (kv.split("=") for kv in rest.split())}
        elif key in ("shown", "manual"):
            out[key] = int(rest)
        elif key in ("owner", "database", "3dxlinux-lcdmgr"):
            out[key] = rest
        elif key == "program":
            f = rest.split("\t")
            if len(f) >= 11:
                out["programs"].append({
                    "index": int(f[0]), "name": f[1], "path": f[2], "enabled": f[3] == "1",
                    "autostart": f[4] == "1", "persistent": f[5] == "1",
                    "foreground": f[6] == "1", "caps": int(f[7]), "device": f[8] == "1",
                    "running": f[9] == "1", "configurable": f[10] == "1"})
    return out
