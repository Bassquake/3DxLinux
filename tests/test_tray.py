#!/usr/bin/env python3
"""Tests for tdx-tray, the system tray icon: run on a private session bus with
a stand-in tray (StatusNotifierWatcher) and notification service.

    dbus-run-session -- python3 tests/test_tray.py
"""
import importlib.machinery
import importlib.util
import os
import subprocess
import sys
import tempfile
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("FAIL:", what)


WATCHER_XML = """<node><interface name="org.kde.StatusNotifierWatcher">
 <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
</interface></node>"""
NOTIFY_XML = """<node><interface name="org.freedesktop.Notifications">
 <method name="Notify">
  <arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/>
  <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/>
  <arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="out"/>
 </method>
</interface></node>"""


def load_tray():
    loader = importlib.machinery.SourceFileLoader("tray", os.path.join(ROOT, "gui", "tdx-tray"))
    spec = importlib.util.spec_from_loader("tray", loader)
    m = importlib.util.module_from_spec(spec)
    loader.exec_module(m)
    return m


def wait(ctx, cond, secs=10):
    end = time.time() + secs
    while not cond() and time.time() < end:
        ctx.iteration(False)
        time.sleep(0.02)
    return cond()


def main():
    t = load_tray()
    d = tempfile.mkdtemp(prefix="tdx-tray-")

    # ---- the parts that need no bus
    lay = t.menu_layout().unpack()
    labels = [c[1]["label"] for c in lay[1][2]]
    check(labels == ["Open 3Dconnexion Settings", "Write Log File"], "menu: %s" % labels)
    os.makedirs(os.path.join(d, "Desktop"))
    fake_ctl = os.path.join(d, "tdxctl")
    with open(fake_ctl, "w") as f:
        f.write("#!/bin/sh\necho \"fake tdxctl $*\"\n")
    os.chmod(fake_ctl, 0o755)
    path = t.write_log(os.path.join(d, "Desktop"), tdxctl=fake_ctl)
    text = open(path).read()
    check(os.path.basename(path).startswith("tdxd-log-") and path.endswith(".txt"), "log name %s" % path)
    check("==== tdxctl status ====\nfake tdxctl status" in text and "==== /etc/tdxd.conf ====" in text
          and "==== tdxd messages (this boot) ====" in text, "log sections")
    again = t.write_log(os.path.join(d, "Desktop"), tdxctl=fake_ctl)
    check(again != path and os.path.exists(path), "a second log in the same second: its own file")
    os.environ["HOME"] = d
    os.environ.pop("XDG_CONFIG_HOME", None)
    check(t.log_dir() == os.path.join(d, "Desktop"), "log on the Desktop: %s" % t.log_dir())

    # ---- on the bus: a stand-in tray and notification service
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    except GLib.Error:
        print("no session bus: run with dbus-run-session; bus tests skipped")
        print("%d/%d checks passed" % (checks - fails, checks))
        return 1 if fails else 0
    registered, notes = [], []

    def watcher_call(_b, _s, _p, _i, method, params, inv):
        registered.append(params.unpack()[0])
        inv.return_value(None)

    def notify_call(_b, _s, _p, _i, method, params, inv):
        notes.append(params.unpack())
        inv.return_value(GLib.Variant("(u)", (7,)))

    bus.register_object("/StatusNotifierWatcher", Gio.DBusNodeInfo.new_for_xml(WATCHER_XML).interfaces[0],
                        watcher_call, None, None)
    bus.register_object("/org/freedesktop/Notifications",
                        Gio.DBusNodeInfo.new_for_xml(NOTIFY_XML).interfaces[0], notify_call, None, None)
    for n in ("org.kde.StatusNotifierWatcher", "org.freedesktop.Notifications"):
        bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                      "RequestName", GLib.Variant("(su)", (n, 4)), None, Gio.DBusCallFlags.NONE, -1, None)
    opened = os.path.join(d, "opened")
    fake_settings = os.path.join(d, "settings")
    with open(fake_settings, "w") as f:
        f.write("#!/bin/sh\ntouch %s\n" % opened)
    os.chmod(fake_settings, 0o755)
    env = dict(os.environ, TDX_TRAY_SETTINGS=fake_settings, PATH=d + ":" + os.environ["PATH"])
    p = subprocess.Popen([sys.executable, os.path.join(ROOT, "gui", "tdx-tray")], env=env)
    ctx = GLib.MainContext.default()
    try:
        check(wait(ctx, lambda: registered), "registers with the tray")
        name = registered[0] if registered else ""
        check(name.startswith("org.kde.StatusNotifierItem-"), "item name %s" % name)

        def prop(path, iface, prop_):
            return bus.call_sync(name, path, "org.freedesktop.DBus.Properties", "Get",
                                 GLib.Variant("(ss)", (iface, prop_)), None,
                                 Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]

        sni = "org.kde.StatusNotifierItem"
        check(prop("/StatusNotifierItem", sni, "Title") == "3D Mouse Settings (tdxd)", "title")
        check(prop("/StatusNotifierItem", sni, "IconName") in ("tdxd-3dxware", "input-mouse"), "icon")
        check(prop("/StatusNotifierItem", sni, "Menu") == "/MenuBar", "menu object")
        lay = bus.call_sync(name, "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
                            GLib.Variant("(iias)", (0, -1, [])), None, Gio.DBusCallFlags.NONE,
                            5000, None).unpack()
        check([c[1]["label"] for c in lay[1][2]] == ["Open 3Dconnexion Settings", "Write Log File"],
              "GetLayout over the bus")

        def click(i):
            bus.call_sync(name, "/MenuBar", "com.canonical.dbusmenu", "Event",
                          GLib.Variant("(isvu)", (i, "clicked", GLib.Variant("s", ""), 0)), None,
                          Gio.DBusCallFlags.NONE, 5000, None)

        click(1)
        check(wait(ctx, lambda: os.path.exists(opened)), "Open 3Dconnexion Settings runs tdx-settings")
        os.remove(opened)
        bus.call_sync(name, "/StatusNotifierItem", sni, "Activate", GLib.Variant("(ii)", (0, 0)),
                      None, Gio.DBusCallFlags.NONE, 5000, None)
        check(wait(ctx, lambda: os.path.exists(opened)), "left click opens the settings too")
        before = set(os.listdir(os.path.join(d, "Desktop")))
        click(2)
        check(wait(ctx, lambda: notes, 20), "Write Log File: a notification")
        new = set(os.listdir(os.path.join(d, "Desktop"))) - before
        check(len(new) == 1 and notes and notes[0][3] == "Log file written" and
              notes[0][4] == os.path.join(d, "Desktop", sorted(new)[0]), "the log and its path: %r %r" %
              (notes[:1], new))
    finally:
        p.terminate()
        p.wait(5)
    print("%d/%d checks passed" % (checks - fails, checks))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
