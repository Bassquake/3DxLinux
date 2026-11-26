# SPDX-License-Identifier: MIT
"""Shared plumbing for the SpacePilot PRO LCD applets (Linux ports of 3Dconnexion's
3DxLcdApplets: Function Keys, RSS Feeds, Picture Viewer).

An applet subclasses Applet, sets NAME / ART / CONFIG_DEFAULTS and implements
draw(cr), and optionally on_buttons(pressed) and configure_dialog(). The base
class connects to 3dxlinux-lcdmgr (reconnecting when it restarts), sends the
program-list icon, renders 320x240 frames with cairo and pushes them, and
runs "Configure..." (LCD Manager) as a GTK dialog in a separate process.
"""

import json
import math
import os
import re
import subprocess
import sys
import threading
import time

import cairo
import gi

# WSLg shows a Wayland window with a generic taskbar icon (it sends none for
# it), but passes an X11 window's own icon through - so use X11 there.
if os.path.isdir("/mnt/wslg") and "GDK_BACKEND" not in os.environ:
    os.environ["GDK_BACKEND"] = "x11"

gi.require_version("Pango", "1.0")
gi.require_version("PangoCairo", "1.0")
from gi.repository import Pango, PangoCairo  # noqa: E402

_here = os.path.dirname(os.path.realpath(__file__))
sys.path[0:0] = [_here, os.path.join(_here, "..", "lib", "3dxlinux", "python")]
import lcd_3dxlinux  # noqa: E402

W, H = 320, 240
FONT = "Arial, Liberation Sans, Arimo, DejaVu Sans, sans-serif"

LEFT, RIGHT, OK, CANCEL, UP, DOWN, MENU = (lcd_3dxlinux.BUTTON_LEFT, lcd_3dxlinux.BUTTON_RIGHT, lcd_3dxlinux.BUTTON_OK,
                                           lcd_3dxlinux.BUTTON_CANCEL, lcd_3dxlinux.BUTTON_UP,
                                           lcd_3dxlinux.BUTTON_DOWN, lcd_3dxlinux.BUTTON_MENU)


def asset_dirs():
    env = os.environ.get("DXLINUX_UI_ASSETS")
    return ([env] if env else []) + [
        os.path.expanduser("~/.local/share/3dxlinux/3dxware-ui"),
        os.path.join(_here, "..", "share", "3dxlinux", "3dxware-ui"),
        "/usr/local/share/3dxlinux/3dxware-ui", "/usr/share/3dxlinux/3dxware-ui"]


def config_dir():
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "3dxlinux")


class Art:
    """PNG artwork extracted from 3DxLcdApplets64.msi (tools/extract_3dxware_ui.py)."""

    def __init__(self, sub):
        self.dir = None
        for d in asset_dirs():
            p = os.path.join(d, "lcdapplets", sub)
            if os.path.isdir(p):
                self.dir = p
                break
        self.cache = {}

    def get(self, name):
        if name not in self.cache:
            p = os.path.join(self.dir, name + ".png") if self.dir else None
            try:
                self.cache[name] = cairo.ImageSurface.create_from_png(p) if p and os.path.isfile(p) else None
            except (cairo.Error, OSError):
                self.cache[name] = None
        return self.cache[name]

    def path(self, name):
        p = os.path.join(self.dir, name) if self.dir else None
        return p if p and os.path.exists(p) else None

    def paint(self, cr, name, x=0, y=0):
        s = self.get(name)
        if s:
            cr.set_source_surface(s, x, y)
            cr.paint()
        return s is not None


# ---- text helpers (Pango)

def layout(cr, text, size, bold=False, width=None, align=None, wrap=False, lines=None):
    lay = PangoCairo.create_layout(cr)
    fd = Pango.FontDescription("%s %s %dpx" % (FONT, "Bold" if bold else "", size))
    lay.set_font_description(fd)
    lay.set_text(text or "", -1)
    if width:
        lay.set_width(int(width * Pango.SCALE))
        if wrap:
            lay.set_wrap(Pango.WrapMode.WORD_CHAR)
            if lines:
                lay.set_height(-lines)
        lay.set_ellipsize(Pango.EllipsizeMode.END)
    if align == "center":
        lay.set_alignment(Pango.Alignment.CENTER)
    elif align == "right":
        lay.set_alignment(Pango.Alignment.RIGHT)
    return lay


def text(cr, x, y, s, size=13, rgb=(1, 1, 1), bold=False, width=None, align=None, wrap=False,
         lines=None, valign_h=None, baseline=False):
    """Draw text with its top at y, or (baseline=True) its first baseline at y."""
    lay = layout(cr, s, size, bold, width, align, wrap, lines)
    if baseline:
        y -= lay.get_baseline() / Pango.SCALE
    elif valign_h:
        _w, h = lay.get_pixel_size()
        y += (valign_h - h) / 2
    cr.set_source_rgb(*rgb)
    cr.move_to(x, y)
    PangoCairo.show_layout(cr, lay)
    return lay.get_pixel_size()


def rgb(hexv):
    return ((hexv >> 16) & 255) / 255.0, ((hexv >> 8) & 255) / 255.0, (hexv & 255) / 255.0


def rounded(cr, x, y, w, h, r):
    cr.new_sub_path()
    cr.arc(x + w - r, y + r, r, -math.pi / 2, 0)
    cr.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
    cr.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
    cr.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
    cr.close_path()


def default_header(cr, title, h=36):
    """The applets' dark title bar when no artwork is available."""
    g = cairo.LinearGradient(0, 0, 0, h)
    g.add_color_stop_rgb(0, 0.36, 0.36, 0.36)
    g.add_color_stop_rgb(1, 0.06, 0.06, 0.06)
    cr.set_source(g)
    cr.rectangle(0, 0, W, h)
    cr.fill()
    text(cr, 56, 0, title, 15, (0.9, 0.9, 0.9), bold=True, width=W - 64, valign_h=h)


# ---- the applet

class Applet:
    NAME = "LCD Applet"
    ART = None              # artwork subdirectory (functionkeys, rss, pictures)
    CONFIG = None           # config file name in ~/.config/3dxlinux
    CONFIG_DEFAULTS = {}
    FPS = 4

    def __init__(self, argv=None):
        self.art = Art(self.ART) if self.ART else Art("none")
        self.cfg = dict(self.CONFIG_DEFAULTS)
        self.cfg_mtime = 0
        self.load_config()
        self.conn = self.dev = None
        self.dirty = True
        self.buttons = 0
        self.lock = threading.Lock()
        self.quit = False
        self.frames = 0

    # config
    def config_path(self):
        return os.path.join(config_dir(), self.CONFIG) if self.CONFIG else None

    def load_config(self):
        p = self.config_path()
        if not p:
            return
        try:
            st = os.stat(p)
            if st.st_mtime == self.cfg_mtime:
                return
            with open(p) as f:
                data = json.load(f)
            self.cfg = dict(self.CONFIG_DEFAULTS)
            self.cfg.update(data)
            self.cfg_mtime = st.st_mtime
            self.config_changed()
        except (OSError, ValueError):
            pass

    def save_config(self):
        p = self.config_path()
        os.makedirs(os.path.dirname(p), exist_ok=True)
        tmp = p + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.cfg, f, indent=1)
        os.replace(tmp, p)

    def config_changed(self):
        self.dirty = True

    # hooks
    def draw(self, cr):
        raise NotImplementedError

    def tick(self, now):
        """Called every frame; return True to redraw."""
        return False

    def on_buttons(self, pressed, held):
        pass

    def configure_dialog(self):
        """Run the Configure... dialog (GTK, separate process)."""

    def window_icon_files(self):
        """The Windows applet's icon in every size (icon_<size>.png, smallest first)."""
        found = []
        for n in os.listdir(self.art.dir) if self.art.dir else []:
            m = re.match(r"icon_(\d+)\.png$", n)
            if m:
                found.append((int(m.group(1)), os.path.join(self.art.dir, n)))
        return [p for _s, p in sorted(found)]

    def set_window_icons(self):
        """Taskbar icon of the Configure... dialog: the applet's own, as on Windows."""
        gi.require_version("Gtk", "3.0")
        gi.require_version("GdkPixbuf", "2.0")
        from gi.repository import GdkPixbuf, GLib, Gtk
        # Wayland app id = 3dxlinux-lcd-<applet>.desktop, where the taskbar icon comes from
        GLib.set_prgname(os.path.basename(os.path.realpath(sys.argv[0])))
        pbs = []
        for p in self.window_icon_files():
            try:
                pbs.append(GdkPixbuf.Pixbuf.new_from_file(p))
            except GLib.Error:
                pass
        if pbs:
            Gtk.Window.set_default_icon_list(pbs)
        else:
            Gtk.Window.set_default_icon_name("input-mouse")

    # plumbing
    def _on_buttons(self, b):
        with self.lock:
            pressed = b & ~self.buttons
            self.buttons = b
            if pressed:
                self.on_buttons(pressed, b)
                self.dirty = True

    def _on_configure(self):
        subprocess.Popen([sys.executable, os.path.realpath(sys.argv[0]), "--configure"],
                         start_new_session=True)

    def _on_notify(self, code, *_):
        if code in (lcd_3dxlinux.NOTIFICATION_CLOSE_CONNECTION, lcd_3dxlinux.NOTIFICATION_DEVICE_REMOVAL,
                    lcd_3dxlinux.NOTIFICATION_APPLET_DISABLED):
            self.dev = None
            if code == lcd_3dxlinux.NOTIFICATION_CLOSE_CONNECTION:
                self.drop_connection()
        self.dirty = True

    def drop_connection(self):
        """Give up the connection (closing it, so the manager forgets it before
        the reconnect - otherwise the program list shows this applet twice)."""
        conn, self.conn, self.dev = self.conn, None, None
        if conn is not None:
            conn.close()

    def connect(self):
        try:
            self.conn = lcd_3dxlinux.Connection(self.NAME, persistent=True, autostart=True,
                                          capabilities=lcd_3dxlinux.APPLET_CAP_QVGA,
                                          on_configure=self._on_configure,
                                          on_notify=self._on_notify)
        except (lcd_3dxlinux.LcdError, OSError):
            self.conn = None
            return False
        icon = self.art.path("icon.png")
        if icon:
            try:
                self.conn.set_icon_png(icon)
            except (lcd_3dxlinux.LcdError, OSError, cairo.Error):
                pass
        return True

    def open(self):
        try:
            self.dev = self.conn.open(lcd_3dxlinux.DEVICE_QVGA, on_buttons=self._on_buttons)
            self.dirty = True
            return True
        except lcd_3dxlinux.LcdError:
            self.dev = None
            return False
        except OSError:
            self.drop_connection()
            return False

    def render(self):
        surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)  # B G R x in memory = lglcd QVGAx32
        cr = cairo.Context(surf)
        with self.lock:
            self.draw(cr)
        surf.flush()
        return surf

    def push(self):
        surf = self.render()
        self.dev.update(bytes(surf.get_data()), lcd_3dxlinux.PRIORITY_NORMAL)
        self.frames += 1

    def run(self):
        next_cfg = 0
        while not self.quit:
            now = time.time()
            if now >= next_cfg:
                self.load_config()
                next_cfg = now + 1.0
            if self.conn is None and not self.connect():
                time.sleep(2)
                continue
            if self.dev is None and not self.open():
                time.sleep(1)
                continue
            if self.tick(now) or self.dirty:
                self.dirty = False
                try:
                    self.push()
                except lcd_3dxlinux.LcdError:
                    self.dev = None
                except OSError:
                    self.drop_connection()
            time.sleep(1.0 / self.FPS)

    def snapshot(self, path):
        self.render().write_to_png(path)


def main(cls):
    import argparse
    ap = argparse.ArgumentParser(description="%s LCD applet for 3dxlinux-lcdmgr" % cls.NAME)
    ap.add_argument("--configure", action="store_true", help="open the settings dialog")
    ap.add_argument("--snapshot", metavar="PNG", help="render one frame to a PNG and exit")
    args = ap.parse_args()
    app = cls()
    if args.configure:
        app.set_window_icons()
        app.configure_dialog()
        return 0
    if args.snapshot:
        app.tick(time.time())
        app.snapshot(args.snapshot)
        return 0
    try:
        app.run()
    except KeyboardInterrupt:
        pass
    return 0
