#!/usr/bin/env python3
"""Tests for gui/tdx-settings: config editing, the GUI handlers, and that tdxd
accepts every file the GUI writes. Needs GTK 3 and a display (X11/Wayland).

    python3 tests/test_settings.py [path/to/tdxd]
"""
import importlib.machinery
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
loader = importlib.machinery.SourceFileLoader("tdx_settings",
                                              os.path.join(ROOT, "gui", "tdx-settings"))
spec = importlib.util.spec_from_loader("tdx_settings", loader)
ts = importlib.util.module_from_spec(spec)
loader.exec_module(ts)

checks = fails = 0


def chord_norm(v):
    """'KEY_LEFTCTRL + KEY_N' and 'KEY_LEFTCTRL+KEY_N' compare equal."""
    return (v or "").replace(" ", "").upper()


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("FAIL:", what)


def conf_file_tests(d):
    p = os.path.join(d, "a.conf")
    with open(p, "w") as f:
        f.write("# header\nsensitivity = 1.0   # global speed\n"
                "# scale_tz = 0.5\nkey_V3DK_ESC = KEY_ESC\nkey_v3dk_esc = KEY_TAB\n"
                "led_ring_color = #FF0000\n")
    c = ts.ConfFile(p)
    check(c.get("sensitivity") == "1.0", "inline comment stripped")
    check(c.get("led_ring_color") == "#FF0000", "#RRGGBB kept like the C parser")
    check(c.get("key_V3DK_ESC") == "KEY_TAB", "button ids case-insensitive, last wins")
    c.set("sensitivity", "2.5")
    check("sensitivity = 2.5   # global speed" in c.lines, "comment preserved on set")
    c.set("scale_tz", "0")
    check(c.lines[c.lines.index("# scale_tz = 0.5") + 1] == "scale_tz = 0",
          "new key placed under its commented example")
    c.set("key_V3DK_ESC", "KEY_ESC")
    check(sum(1 for k, _ in c.items() if k.lower() == "key_v3dk_esc") == 1,
          "duplicates collapsed on set")
    c.remove("led_ring_color")
    check(not c.has("led_ring_color") and c.lines[0] == "# header", "remove keeps comments")


def gui_tests(d, tdxd):
    base = os.path.join(d, "tdxd.conf")
    pdir = os.path.join(d, "tdxd.d")
    os.makedirs(pdir)
    with open(os.path.join(ROOT, "packaging", "tdxd.conf")) as f:
        text = f.read()
    # keep the test daemon off the system sockets
    text = text.replace("spnav  = yes", "spnav  = no")
    text = text.replace("control_socket = /run/tdxd.sock",
                        "control_socket = " + os.path.join(d, "ctl.sock"))
    text = text.replace("profile_dir = /etc/tdxd.d", "profile_dir = " + pdir)
    with open(base, "w") as f:
        f.write(text)

    class Args:
        config, profile_dir, profile, socket = base, None, "", None
        status_file = os.path.join(d, "status.txt")
        no_save = True
    with open(Args.status_file, "w") as f:
        f.write("tdxd test\nprofile default\napplication blender (via focus)\ndevices 1\n"
                "  SpacePilot Pro [046d:c629] /dev/hidraw0 firmware 4.11\n")
    app = ts.App(Args)
    check(app.status["devices"][0]["firmware"] == "4.11", "status parsed")
    check(app.status.get("app") == "blender", "focused app parsed")
    main = ts.MainPanel(app)
    app.main = main
    adv = ts.AdvancedPanel(app)
    btn = ts.ButtonsPanel(app)
    lcd = ts.LcdPanel(app)
    ts.AboutPanel(app)

    # ---- base file edits
    main.speed.set_value(0.30103)  # log10(2)
    check(app.store.base.get("sensitivity") == "2", "speed slider -> sensitivity")
    adv.dominant.set_active(True)
    check(app.store.base.get("dominant") == "yes", "Dominant")
    adv.rotation.set_active(False)
    check(app.store.base.get("disable_rotation") == "yes", "Rotation off")
    adv.zoom_ud.set_active(True)
    check(app.store.base.get("map_ty") == "tz" and app.store.base.get("map_tz") == "ty",
          "zoom Up/Down swaps ty/tz")
    adv.zoom_fb.set_active(True)
    check(not app.store.base.has("map_ty") and not app.store.base.has("map_tz"),
          "zoom Forward/Backward removes the swap")
    act, sc, rev = adv.axis[2]
    sc.set_value(-0.30103)
    check(app.store.base.get("scale_tz") == "0.5", "axis slider -> scale_tz")
    act.set_active(False)
    check(app.store.base.get("scale_tz") == "0", "Active off -> scale 0")
    check(not sc.get_sensitive(), "slider disabled while inactive")
    act.set_active(True)
    check(app.store.base.get("scale_tz") == "0.5", "Active on restores the scale")
    rev.set_active(True)
    check(app.store.base.get("invert_tz") == "yes", "Reverse")
    adv.led.set_active(False)
    check(app.store.base.get("led") == "no", "LEDs Active")
    btn.assign("V3DK_1", "keys", ["LEFTCTRL", "C"])
    check(app.store.base.get("key_V3DK_1") == "KEY_LEFTCTRL+KEY_C", "keystroke")
    check(btn.describe("V3DK_1", "Button 1") == "Copy", "preset name shown")
    btn.assign("V3DK_FIT", "action", "calibrate")
    check(app.store.base.get("action_V3DK_FIT") == "calibrate", "driver action")
    btn.assign("V3DK_ESC", "app", None)
    check(not app.store.base.has("key_V3DK_ESC"), "back to application in base")
    lcd.labels[0].set_text("Copy")
    check(app.store.base.get("lcd_label_1") == "Copy", "LCD label")
    lcd.bright[30].set_active(True)
    check(app.store.base.get("lcd_brightness") == "30", "LCD brightness Low")
    check("# ---- motion" in app.store.base.text(), "comments kept in the base file")
    check(ts.write_file(app.store.base) is None, "base written")

    # ---- profile edits
    app.store.create_profile("blender", "blender, blender-*")
    app.on_changed()
    main.speed.set_value(0)
    check(app.store.profile.get("sensitivity") == "1" and
          app.store.base.get("sensitivity") == "2", "speed goes to the profile")
    btn.assign("V3DK_PLUS", "keys", ["KPPLUS"])
    check(app.store.profile.get("action_V3DK_PLUS") == "none" and
          app.store.profile.get("key_V3DK_PLUS") == "KEY_KPPLUS",
          "profile keystroke overrides the base action with action = none")
    btn.assign("V3DK_CTRL", "app", None)
    check(app.store.profile.get("key_V3DK_CTRL") == "none", "profile unbinds a base key")
    lcd.enable.set_active(False)
    check(app.store.base.get("lcd") == "no" and not app.store.profile.has("lcd"),
          "daemon-wide option goes to the base file")
    adv.on_restore(None)
    check(not app.store.profile.has("map_ty"), "Restore Defaults in profile")
    check(app.store.profile.get("match") == "blender, blender-*", "match kept")
    check(ts.write_file(app.store.profile) is None and ts.write_file(app.store.base) is None,
          "profile written")

    # ---- tdxd must accept both files without a warning
    if tdxd and os.access(tdxd, os.X_OK):
        for args in (["-c", base], ["-c", base]):
            p = subprocess.run(["timeout", "1", tdxd, "-v"] + args, capture_output=True,
                               text=True)
            bad = [l for l in p.stderr.splitlines()
                   if "unknown" in l or "expected" in l or "ignored" in l]
            check(not bad, "tdxd parses the GUI's files: %s" % bad)
            check("1 per-application profile" in p.stderr, "tdxd sees the new profile")
            break
    else:
        print("skip: tdxd not built")

    # ---- About: the 3DxWare look, and the importer it runs
    cmd = ts.find_extractor()
    check(cmd and cmd[-1].endswith("extract_3dxware_ui.py"), "importer found: %s" % cmd)
    old_assets = ts.ASSETS
    ts.ASSETS = ts.Assets(os.path.join(d, "none"))
    about = ts.AboutPanel(app)
    about.refresh()
    check(ts.look_source() is None and about.look.get_text().startswith("Not imported"),
          "About: not imported")
    art = os.path.join(d, "art")
    os.makedirs(os.path.join(art, "ui"))
    with open(os.path.join(art, "SOURCE"), "w") as f:
        f.write("Extracted from 3DxWare64_v10-9-14_b745.exe for local use by tdx-settings.\n")
    ts.ASSETS = ts.Assets(art)
    about.refresh()
    check(re.match(r"Imported from 3DxWare64_v10-9-14_b745\.exe on \d\d \w+ \d{4}, \d\d:\d\d\.$",
                   about.look.get_text()), "About: imported from, when: %s" % about.look.get_text())
    ts.ASSETS = old_assets
    out = ["unpacking 3DxWare64_v10-9-14_b745.exe", "96 icons and device images",
           "265 control-panel images from 3DxSmartUi.exe",
           "window icon sizes [16, 24, 32, 48, 64, 128, 256] from 3DxSmartUi.exe",
           "18 images from 3DxLCD10SPPRO.exe", "7 images from zz3DxLCD50RSSReader.exe",
           "10 images from zz3DxLCD60Picture.exe", "75 LCD Manager images from LCDMon.exe",
           "14 taskbar icons in /home/u/.local/share/icons",
           "wrote /home/u/.local/share/tdxd/3dxware-ui"]
    check(ts.import_summary(out) == "485 images into /home/u/.local/share/tdxd/3dxware-ui",
          "import summary: %s" % ts.import_summary(out))
    out2 = out[:-1] + ["66 application profiles (651 keys) in /x/profiles"] + out[-1:]
    check(ts.import_summary(out2) == "485 images and 66 application key settings into "
          "/home/u/.local/share/tdxd/3dxware-ui", "import summary: %s" % ts.import_summary(out2))

    # ---- changes apply right away, also for a root-owned /etc/tdxd.conf
    # (pkexec faked): saved in the background, then on close after a cancel
    from gi.repository import GLib
    app.store.select(None)
    orig = ts.writable, ts.write_file_data
    saved = []
    answer = [None]
    ts.writable = lambda p: False
    ts.write_file_data = lambda conf, data: (saved.append(data), answer[0])[1]
    app.args.no_save = False

    def settle(cond, secs=5):
        end = time.time() + secs
        ctx = GLib.MainContext.default()
        while time.time() < end and not cond():
            ctx.iteration(False)
            time.sleep(0.01)
        return cond()
    a, b = ("yes", "no") if app.store.base.get("led", "yes") == "no" else ("no", "yes")
    app.store.set("led", a)
    check(settle(lambda: saved and not app.saving and app.save_state.startswith("Saved")),
          "a change to a root-owned file is saved right away: %r" % app.save_state)
    check(saved and "led = " + a in saved[-1] and not app.store.base.dirty,
          "the change written once")
    answer[0] = ts.PKEXEC_CANCELLED
    n = len(saved)
    app.store.set("led", b)
    check(settle(lambda: app.pw_declined), "a cancelled password is noticed")
    app.store.set("led", a)
    settle(lambda: False, 1.5)
    check(len(saved) == n + 1 and app.store.base.dirty and
          app.save_state == "Saved when you close (password)",
          "after a cancel: no more prompts, saved on close (%d writes)" % (len(saved) - n))
    answer[0] = None
    app.flush()
    check(not app.store.base.dirty and "led = " + a in saved[-1], "saved on close")
    ts.writable, ts.write_file_data = orig
    app.args.no_save = True
    app.pw_declined = False

    # ---- restore button defaults in the base
    app.store.select(None)
    app.store.base.set("lcd_label_V3DK_TOP", "Old Top")  # an old config's name
    app.store.base.set("lcd_label_3", "Front")
    btn.restore.clicked()  # the window's button, not just its handler
    check(not app.store.base.has("lcd_label_V3DK_TOP") and app.store.base.get("lcd_label_3") == "Save",
          "Restore Defaults drops old key names")
    check(app.store.base.get("action_V3DK_PLUS") == "speed_up" and
          app.store.base.get("key_V3DK_ESC") == "KEY_ESC" and
          app.store.base.get("key_V3DK_1") == "KEY_LEFTCTRL+KEY_N" and
          app.store.base.get("lcd_label_1") == "New" and
          app.store.base.get("action_V3DK_MENU") == "settings", "Buttons Restore Defaults")
    # the panel's defaults are the shipped config's (packaging/tdxd.conf)
    ship = ts.ConfFile(ts.shipped_default("tdxd.conf"))  # an installed copy comes first
    check(all(chord_norm(ship.get("key_" + k)) == chord_norm(v) for k, v in ts.DEFAULT_KEYS.items()) and
          all(ship.get("action_" + k) == v for k, v in ts.DEFAULT_ACTIONS.items()) and
          all(ship.get("lcd_label_%d" % (i + 1)) == v for i, v in enumerate(ts.DEFAULT_LABELS)),
          "Restore Defaults matches packaging/tdxd.conf")

    # ---- About: start the tray icon and the LCD Manager with the desktop
    log = os.path.join(d, "systemctl.log")
    sysctl = os.path.join(d, "systemctl")
    with open(sysctl, "w") as f:
        f.write('#!/bin/sh\necho "$*" >> %s\n[ "$2" = is-enabled ] && echo enabled\nexit 0\n' % log)
    os.chmod(sysctl, 0o755)
    os.environ["TDX_SYSTEMCTL"] = sysctl
    about = [p for p in app.panels if isinstance(p, ts.AboutPanel)][0]
    about.refresh()
    check(about.autostart.get_active(), "Start-up ticked when not masked")
    about.autostart.set_active(False)
    about.autostart.set_active(True)
    calls = open(log).read()
    check("--user mask tdx-tray.service tdx-lcdmgr.service" in calls and
          "--user unmask tdx-tray.service tdx-lcdmgr.service" in calls and
          "--user start tdx-tray.service tdx-lcdmgr.service" in calls, "Start-up: %r" % calls)
    # opening the panel brings them back after the tray's Quit ...
    open(log, "w").close()
    check(ts.bring_back(), "asked systemd to start them")
    for _ in range(50):
        if "--user start" in open(log).read():
            break
        time.sleep(0.05)
    check("--user start tdx-tray.service tdx-lcdmgr.service" in open(log).read(), "started: %r"
          % open(log).read())
    # ... but not when Start with the Desktop is off
    with open(sysctl, "w") as f:
        f.write('#!/bin/sh\necho "$*" >> %s\n[ "$2" = is-enabled ] && echo masked\nexit 0\n' % log)
    open(log, "w").close()
    check(ts.bring_back() is False and "--user start" not in open(log).read(), "masked: left off")
    del os.environ["TDX_SYSTEMCTL"]

    # ---- the panel follows the application used last (tdxd's active profile)
    app.args.profile = None  # not started with -p
    app.store.select(None)
    app.tdxd_profile = "default"
    with open(os.path.join(pdir, "blender.conf"), "w") as f:
        f.write("match = blender\n")
    with open(Args.status_file, "w") as f:
        f.write("tdxd test\nprofile blender\napplication blender (via focus)\ndevices 1\n")
    check(app.follow_tick() is True and app.store.profile and
          os.path.basename(app.store.profile.path) == "blender.conf", "switched to Blender's profile")
    app.store.select(None)  # chosen by hand from the title menu
    app.follow_tick()
    check(app.store.profile is None, "a profile chosen by hand stays until tdxd's changes")
    with open(Args.status_file, "w") as f:
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 1\n")
    app.store.select("blender")
    app.follow_tick()
    check(app.store.profile is None, "back to All Applications")
    app.args.profile = ""
    app.follow_tick()  # started with -p: stays

    # ---- a device unplugged and plugged in again while the panel is open
    with open(Args.status_file, "w") as f:
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 0\n")
    app.follow_tick()
    check(main.dev_label.get_text() == "No device connected" and
          main.state_label.get_text() == "Connect a device", "unplugged: %s / %s"
          % (main.dev_label.get_text(), main.state_label.get_text()))
    check(main.photo.key is None and btn.photo.key is None, "photos: no device")
    check(main.plain_title.get_visible() and main.nodev_icon_box.get_visible() and
          not main.speed.get_visible() and not main.main_buttons[0].get_visible() and
          main.nodev_text.get_text().startswith("No device found."), "3DxWare's no-device page")
    with open(Args.status_file, "w") as f:
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 1\n"
                "  SpacePilot PRO [046d:c629] /dev/hidraw0 firmware 4.11\n")
    app.follow_tick()
    check(main.dev_label.get_text() == "SpacePilot Pro" and main.photo.key == ("046d", "c629") and
          btn.photo.key == ("046d", "c629"), "plugged in again: %s" % main.dev_label.get_text())
    check(not main.plain_title.get_visible() and not main.nodev_icon_box.get_visible() and
          main.speed.get_visible() and main.main_buttons[0].get_visible(), "back to the main page")
    check(not main.name_btn.get_visible() and main.dev_label.get_visible(),
          "one device: its name, no chooser")

    # ---- a Universal Receiver with a CadMouse paired (tdxd's status lines)
    rcv = ("receiver Universal Receiver [256f:c652] firmware 1.06\n"
           "paired 4 CadMouse Compact Wireless [256f:c658] id 0048f7ba8b battery %s "
           "charging no seen 2 active yes\n")
    with open(Args.status_file, "w") as f:
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 0\n"
                + rcv % "52")
    app.follow_tick()
    r = app.status["receivers"]
    check(len(r) == 1 and r[0]["pid"] == "c652" and r[0]["paired"][0]["battery"] == 52 and
          r[0]["paired"][0]["slot"] == 4 and not r[0]["paired"][0]["charging"],
          "receiver status parsed: %s" % r)
    check([e["kind"] for e in ts.device_entries(app.status)] == ["paired", "receiver"],
          "chooser lists the CadMouse, then the receiver")
    check(main.mode == "paired" and main.name_btn.get_visible() and
          not main.dev_label.get_visible() and
          main.name_btn_label.get_text() == "CadMouse Compact Wireless", "CadMouse page")
    check(main.photo.key == ("256f", "c658") and main.conn_box.get_visible() and
          not main.state_wrap.get_visible() and not main.lcd_button.get_visible() and
          main.main_buttons[1].get_visible() and main.main_buttons[1].get_sensitive() and
          not main.speed.get_visible() and main.mouse_speed.get_visible() and
          main.main_buttons[0].get_sensitive(), "CadMouse: icons, its own Speed, Buttons")
    check(ts.MOUSE_DPI_STEPS[int(main.mouse_speed.get_value())] == 1400, "Speed: 1400 dpi default")
    main.mouse_speed.set_value(ts.MOUSE_DPI_STEPS.index(7200))
    check(app.store.base.get("mouse_dpi") == "7200", "CadMouse Speed -> mouse_dpi in tdxd.conf")
    app.store.select("blender")  # a profile being edited: the mouse settings stay daemon-wide
    main.mouse_speed.set_value(ts.MOUSE_DPI_STEPS.index(400))
    check(app.store.base.get("mouse_dpi") == "400" and not app.store.profile.has("mouse_dpi"),
          "mouse_dpi always in the base file")
    app.store.select(None)
    class FakeKwin:  # KWin on KDE Plasma: one CadMouse pointer
        def __init__(self):
            self.accel, self.factor = True, 1.0

        def devices(self):
            return ["/org/kde/KWin/InputDevice/event7"]

        def acceleration(self, paths):
            return self.accel

        def set_acceleration(self, paths, on):
            self.accel = on

        def lines(self, paths):
            return round(self.factor * 3)

        def set_lines(self, paths, n):
            self.factor = n / 3.0

    real_kwin = app.kwin
    madv0 = ts.MouseAdvancedPanel(app)  # no KDE: greyed
    check(not madv0.accel.get_sensitive() and not madv0.lines_btn.get_sensitive(),
          "acceleration / wheel lines greyed without KDE")
    app.panels.remove(madv0)
    app.kwin = FakeKwin()
    madv = ts.MouseAdvancedPanel(app)
    check(madv.accel.get_sensitive() and madv.accel.get_active() and
          madv.lines_label.get_text() == "3", "KDE: acceleration and lines from KWin")
    madv.accel.set_active(False)
    madv.set_lines(6)
    check(app.kwin.accel is False and abs(app.kwin.factor - 2.0) < 1e-9 and
          madv.lines_label.get_text() == "6", "KDE: acceleration off, 6 lines (scroll factor 2)")
    check(madv.title_label.get_text() == "CadMouse Compact Wireless" and
          madv.rates[1000].get_active() and madv.smart.get_active(), "CadMouse Advanced: defaults")
    madv.rates[250].set_active(True)
    madv.smart.set_active(False)
    check(app.store.base.get("mouse_polling_rate") == "250" and
          app.store.base.get("mouse_smart_scrolling") == "no", "Polling Rate / Smart Scrolling")
    madv.on_restore(None)
    check(app.kwin.accel is True and abs(app.kwin.factor - 1.0) < 1e-9,
          "Restore Defaults: acceleration on, 3 lines")
    check(not app.store.base.has("mouse_polling_rate") and
          not app.store.base.has("mouse_smart_scrolling") and madv.rates[1000].get_active()
          and madv.smart.get_active() and app.store.base.get("mouse_dpi") == "400",
          "CadMouse Restore Defaults (Speed stays: it is the main page's)")
    app.panels.remove(madv)
    app.kwin = real_kwin
    # ---- CadMouse Buttons
    mbtn = ts.MouseButtonsPanel(app)
    check(mbtn.rows["V2DK_RADIALMENU"][1].get_text() == "Common Tools" and
          mbtn.rows["V2DK_FORWARD"][1].get_text() == "Forward Mouse Button" and
          mbtn.rows["V2DK_WHEEL"][1].get_text() == "Middle Mouse Function" and
          not mbtn.app_switch.get_sensitive(), "CadMouse Buttons: 3DxWare's defaults")
    mbtn.assign("V2DK_FORWARD", "KEY_LEFTCTRL+KEY_Z")
    mbtn.assign("V2DK_RADIALMENU", "BTN_MIDDLE")
    check(app.store.base.get("key_V2DK_FORWARD") == "KEY_LEFTCTRL+KEY_Z" and
          app.store.base.get("key_V2DK_RADIALMENU") == "BTN_MIDDLE" and
          mbtn.rows["V2DK_FORWARD"][1].get_text() == "Undo" and
          mbtn.rows["V2DK_RADIALMENU"][1].get_text() == "Middle Mouse Function",
          "CadMouse Buttons: a keystroke and a mouse function (global)")
    mbtn.swap.set_active(True)
    check(app.store.base.get("mouse_swap_buttons") == "yes", "Swap Middle / Right")
    app.store.select("blender")
    mbtn.refresh()
    check(mbtn.app_switch.get_sensitive() and not mbtn.app_switch.get_active() and
          mbtn.title_label.get_text().endswith(" - Global Settings") and
          mbtn.rows["V2DK_FORWARD"][1].get_text() == "Undo", "Blender: the global buttons")
    mbtn.app_switch.set_active(True)
    mbtn.assign("V2DK_FORWARD", None)
    check(app.store.profile.get("key_V2DK_FORWARD") == "none" and
          app.store.profile.get("key_V2DK_RADIALMENU") == "BTN_MIDDLE" and
          app.store.base.get("key_V2DK_FORWARD") == "KEY_LEFTCTRL+KEY_Z" and
          mbtn.rows["V2DK_FORWARD"][1].get_text() == "Forward Mouse Button",
          "Application Specific: Blender's own buttons start from the global ones")
    mbtn.app_switch.set_active(False)
    check(not any(app.store.profile.has(k) for k in ts.MOUSE_PROFILE_KEYS),
          "Application Specific off: back to the global buttons")
    app.store.select(None)
    for k in ts.MOUSE_PROFILE_KEYS:
        app.store.base.remove(k)
    app.store.changed()
    # ---- 3DxWare's action picker
    pk = ts.ActionPicker(mbtn, mbtn.rows["V2DK_RADIALMENU"][0], "V2DK_RADIALMENU", "Common Tools")
    heads = [w for w in pk.list.get_children() if isinstance(w, ts.Gtk.EventBox)]
    check([h.get_child().get_children()[1].get_text() for h in heads] ==
          ["3Dconnexion", "Applications", "Keyboard", "Macros", "Mouse", "Radial Menus"],
          "picker: 3DxWare's sections, closed")
    check(pk.current and pk.current["label"] == "Common Tools" and
          "tdx-pick-bold" in heads[5].get_child().get_children()[1].get_style_context()
          .list_classes(), "picker: Radial Menus bold, Common Tools current")
    pk.toggle("Radial Menus")
    btns = [w for w in pk.list.get_children() if isinstance(w, ts.Gtk.Button)]
    check(len(btns) == 3 and not btns[0].get_sensitive() and btns[1].get_sensitive() and
          btns[1].get_style_context().has_class("tdx-pick-current") and
          not btns[2].get_sensitive(), "Radial Menus: Common Macros greyed, Common Tools, New")
    pk.search.set_text("volume")
    btns = [w for w in pk.list.get_children() if isinstance(w, ts.Gtk.Button)]
    check(len(btns) == 2 and all(b.get_sensitive() for b in btns), "search: Volume Down / Up")
    pk.search.set_text("undo")
    btns = [w for w in pk.list.get_children() if isinstance(w, ts.Gtk.Button)]
    check(len(btns) == 1, "search finds Undo inside System Shortcuts")
    btns[0].clicked()
    check(app.store.base.get("key_V2DK_RADIALMENU") == "KEY_LEFTCTRL+KEY_Z" and
          mbtn.rows["V2DK_RADIALMENU"][1].get_text() == "Undo", "picker: Undo chosen")
    mbtn.pick("V2DK_BACKWARD", "action:speed_up")
    mbtn.pick("V2DK_WHEEL", "action:disabled")
    mbtn.pick("V2DK_FORWARD", "keys:MICMUTE")
    check(app.store.base.get("action_V2DK_BACKWARD") == "speed_up" and
          mbtn.rows["V2DK_BACKWARD"][1].get_text() == "Increase Speed" and
          mbtn.rows["V2DK_WHEEL"][1].get_text() == "Disabled" and
          app.store.base.get("key_V2DK_FORWARD") == "KEY_MICMUTE" and
          mbtn.rows["V2DK_FORWARD"][1].get_text() == "Mute Microphone",
          "picker: a driver action, Disabled, Mute Microphone")
    mbtn.pick("V2DK_BACKWARD", "BTN_SIDE")  # its own function again: the default
    mbtn.pick("V2DK_MIDDLE", "BTN_RIGHT")
    check(not app.store.base.has("action_V2DK_BACKWARD") and
          not app.store.base.has("key_V2DK_BACKWARD") and
          app.store.base.get("key_V2DK_MIDDLE") == "BTN_RIGHT" and
          mbtn.rows["V2DK_MIDDLE"][1].get_text() == "Secondary Mouse Function",
          "picker: back to the default, and a mouse function")
    greyed = [it["label"] for _s, it in ts.picker_items() if not it["value"]]
    check("Virtual NumPad" in greyed and "Double-Click Left" in greyed and
          "Undo" not in greyed, "picker: Windows-only entries greyed")
    for k in ts.MOUSE_PROFILE_KEYS:
        app.store.base.remove(k)
    app.store.changed()
    app.panels.remove(mbtn)
    check(ts.battery_icon(52, False) == "SmartUI_Battery_50" and
          ts.battery_icon(None, False) == "SmartUI_Battery_Unknown" and
          ts.battery_icon(100, False) == "SmartUI_Battery_100" and
          ts.battery_icon(3, False) == "SmartUI_Battery_Critical" and
          ts.battery_icon(40, True) == "SmartUI_Battery_Charging_1", "battery pictures")
    app.select_device(ts.device_entries(app.status)[1]["key"])
    check(main.mode == "receiver" and main.photo.key == ("256f", "c652") and
          main.recv_title.get_visible() and main.recv_buttons[1].get_visible() and
          not main.speed.get_visible() and not main.main_buttons[0].get_visible(),
          "receiver page")
    # ---- Add device: tdxd's pairing line under its receiver
    with open(Args.status_file, "w") as f:
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 0\n"
                "receiver Universal Receiver [256f:c652] firmware 1.06\npairing on 42\n")
    check(app.daemon.status()["receivers"][0]["pairing"] == {"state": "on", "left": 42},
          "pairing on, seconds left")
    with open(Args.status_file, "w") as f:
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 0\n"
                "receiver Universal Receiver [256f:c652] firmware 1.06\n"
                "pairing done 4 CadMouse Compact Wireless\n")
    p = app.daemon.status()["receivers"][0]["pairing"]
    check(p["state"] == "done" and p["slot"] == 4 and p["name"] == "CadMouse Compact Wireless",
          "pairing done: %s" % p)
    addp = ts.AddDevicePanel(app)
    check(addp.page == "choose" and addp.tiles.get_visible() and addp.back.get_visible() and
          not addp.close.get_visible() and addp.title_label.get_text() == "Device Selection" and
          [k for k, *_ in ts.PAIR_KINDS] == ["spacemousepro", "spacemouse", "mouse"],
          "Add device: choose the device first")
    addp.page = "pairing"
    addp.started = ts.time.monotonic()
    with open(Args.status_file, "w") as f:  # the receiver away after pairing: keep waiting
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 0\n")
    check(addp.tick() is True and addp.page == "pairing", "Add device: waits while the receiver is away")
    with open(Args.status_file, "w") as f:
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 0\n"
                "receiver Universal Receiver [256f:c652] firmware 1.06\n"
                "pairing done 4 CadMouse Compact Wireless\n")
    addp.tick()
    check(addp.page == "done" and "CadMouse Compact Wireless" in addp.text.get_text() and
          addp.title_label.get_text() == "Pairing Successful" and addp.close.get_visible(),
          "Add device: paired")
    app.panels.remove(addp)
    # ---- the receiver's Advanced Settings: the paired devices, a remove button each
    with open(Args.status_file, "w") as f:
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 0\n"
                "receiver Universal Receiver [256f:c652] firmware 1.06\n"
                "paired 4 CadMouse Compact Wireless [256f:c658] id 0048f7ba8b battery 50 "
                "charging no seen 2 active no serial Y1224194699\n")
    app.status = app.daemon.status()
    check(app.status["receivers"][0]["paired"][0]["serial"] == "Y1224194699", "serial parsed")
    remp = ts.ReceiverAdvancedPanel(app)
    tiles = remp.grid.get_children()
    check(len(tiles) == 1 and tiles[0].dev["name"] == "CadMouse Compact Wireless" and
          remp.count.get_text() == "Currently paired devices (1/5)",
          "Advanced Settings lists the CadMouse (paired, even when not heard from)")
    tiles[0].set_hover(True)
    check(tiles[0].xbtn.get_visible(), "the remove button shows on hover")
    tiles[0].set_hover(False)
    check(not tiles[0].xbtn.get_visible(), "... and goes again")
    remp.removing = 4
    with open(Args.status_file, "w") as f:  # gone from the receiver
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 0\n"
                "receiver Universal Receiver [256f:c652] firmware 1.06\n")
    app.status = app.daemon.status()
    remp.refresh()
    check(remp.removing is None and remp.msg.get_text() == "Removed." and
          not remp.grid.get_children() and
          remp.count.get_text() == "Currently paired devices (0/5)", "Remove: removed")
    app.panels.remove(remp)
    with open(Args.status_file, "w") as f:  # the battery report changes: still the receiver
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 0\n"
                + rcv % "-")
    app.follow_tick()
    check(main.mode == "receiver" and app.status["receivers"][0]["paired"][0]["battery"] is None,
          "the chosen device stays chosen")
    with open(Args.status_file, "w") as f:  # paired, but switched off / not used yet
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 0\n"
                + (rcv % "-").replace("active yes", "active no"))
    app.follow_tick()
    check([e["kind"] for e in ts.device_entries(app.status)] == ["receiver"] and
          main.mode == "receiver" and main.dev_label.get_visible() and
          not main.name_btn.get_visible() and main.dev_label.get_text() == "Universal Receiver",
          "a CadMouse that has not been heard from: just the receiver, no chooser")
    with open(Args.status_file, "w") as f:
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 1\n"
                "  SpacePilot PRO [046d:c629] /dev/hidraw0 firmware 4.11\n")
    app.follow_tick()
    check(main.mode == "3d" and main.speed.get_sensitive() and main.lcd_button.get_visible(),
          "receiver unplugged: the SpacePilot again")

    # ---- a device unplugged: the other windows close, back to the main window
    two = ("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 1\n"
           "  SpacePilot PRO [046d:c629] /dev/hidraw0 firmware 4.11\n"
           "receiver Universal Receiver [256f:c652] firmware 1.06\n"
           "paired 4 CadMouse Compact Wireless [256f:c658] id 0048f7ba8b battery 50 "
           "charging no seen 2 active yes serial Y1224194699\n")
    with open(Args.status_file, "w") as f:
        f.write(two)
    app.follow_tick()
    app.window(ts.ButtonsPanel)
    app.window(ts.ReceiverAdvancedPanel)
    check(len(app.windows) == 2, "two windows open")
    with open(Args.status_file, "w") as f:  # the receiver unplugged
        f.write(two.split("receiver ")[0])
    app.follow_tick()
    check(not app.windows and main.mode == "3d", "receiver unplugged: windows closed, main window")
    with open(Args.status_file, "w") as f:
        f.write(two)
    app.follow_tick()
    app.window(ts.AddDevicePanel)
    addw = [p for p in app.panels if isinstance(p, ts.AddDevicePanel)][0]
    addw.page = "pairing"  # pairing: the receiver drops off USB for a moment
    with open(Args.status_file, "w") as f:
        f.write(two.split("receiver ")[0])
    app.follow_tick()
    check(ts.AddDevicePanel in app.windows, "while pairing the Add device window stays open")
    addw.page = "done"
    with open(Args.status_file, "w") as f:  # a device plugged in: nothing closes
        f.write(two)
    app.follow_tick()
    check(ts.AddDevicePanel in app.windows, "a device plugged in closes nothing")
    app.windows[ts.AddDevicePanel].destroy()
    with open(Args.status_file, "w") as f:
        f.write("tdxd test\nprofile default\napplication gimp (via focus)\ndevices 1\n"
                "  SpacePilot PRO [046d:c629] /dev/hidraw0 firmware 4.11\n")
    app.follow_tick()

    # ---- More: 3DxWare's menu
    menu = app.build_more_menu()
    items = menu.get_children()
    labels = [[c for c in i.get_child().get_children() if isinstance(c, ts.Gtk.Label)][0].get_text()
              if not isinstance(i, ts.Gtk.SeparatorMenuItem) else "-" for i in items]
    check(labels == ["Import settings", "Export settings", "Reset all settings", "-", "Manuals",
                     "Support", "About"], "More menu: %s" % labels)
    check(not items[4].get_sensitive() and items[6].get_sensitive(), "Manuals greyed out")
    check(ts.AboutPanel.title == "About 3DxWare", "About 3DxWare")

    # ---- Export / Import / Reset all settings
    zpath = os.path.join(d, "exported.zip")
    with open(os.path.join(pdir, "mine.conf"), "w") as f:
        f.write("match = mine\nkey_V3DK_1 = KEY_F1\n")
    app.store.select(None)
    app.store.base.set("sensitivity", "3")
    ts.write_file(app.store.base)
    n = ts.export_settings(app.store, zpath)
    base_t, profs = ts.read_settings_archive(zpath)
    check(n == len(profs) and "sensitivity = 3" in base_t and "mine" in profs and
          "KEY_F1" in profs["mine"], "exported: base + %d profiles" % n)
    app.store.base.set("sensitivity", "1")
    ts.write_file(app.store.base)
    os.remove(os.path.join(pdir, "mine.conf"))
    check(ts.import_settings(app.store, base_t, profs) == [] and
          ts.ConfFile(base).get("sensitivity") == "3" and
          os.path.isfile(os.path.join(pdir, "mine.conf")), "imported again")
    import zipfile
    bad = os.path.join(d, "bad.zip")
    with zipfile.ZipFile(bad, "w") as z:
        z.writestr("../evil.conf", "x")
        z.writestr("tdxd.d/../../evil.conf", "x")
        z.writestr("tdxd.d/.hidden.conf", "x")
    try:
        ts.read_settings_archive(bad)
        check(False, "a zip without settings is refused")
    except ValueError:
        check(True, "")
    with open(os.path.join(pdir, "freecad.conf"), "w") as f:
        f.write("match = freecad*\n")  # changed by hand
    check(ts.reset_settings(app.store) == [], "reset ran")
    app.store.base.reload()
    ship = ts.ConfFile(ts.shipped_default("tdxd.conf"))  # an installed copy comes first
    check(app.store.base.get("sensitivity") == ship.get("sensitivity") and
          app.store.base.text() == ship.text(), "reset: the default tdxd.conf")
    check(not os.path.exists(os.path.join(pdir, "mine.conf")) and
          ts.ConfFile(os.path.join(pdir, "freecad.conf")).get("key_V3DK_TOP") ==
          ts.ConfFile(os.path.join(ROOT, "packaging", "profiles", "freecad.conf")).get("key_V3DK_TOP"),
          "reset: own profiles removed, FreeCAD's back to its default")
    # the password helper removes only profiles (refusals only: the tests may run as root)
    helper = os.path.join(ROOT, "gui", "tdx-settings-helper")
    for args in (["remove", "/etc/tdxd.conf"], ["remove", "/etc/passwd"],
                 ["remove", "/etc/tdxd.d/../passwd.conf"]):
        r = subprocess.run(["sh", helper] + args, capture_output=True, text=True)
        check(r.returncode != 0, "helper refuses %s" % " ".join(args))

    # ---- the application's own key names (tdxctl app-keys; the FreeCAD add-on)
    d0 = ts.Daemon("/nonexistent")
    d0.request = lambda *a: (0, "app FreeCAD\nV3DK_1\tCreate sketch\nV3DK_2\tPart Design Modeling\n")
    check(d0.app_keys() == ("FreeCAD", {"V3DK_1": "Create sketch", "V3DK_2": "Part Design Modeling"}),
          "app-keys parsed: %r" % (d0.app_keys(),))
    d0.request = lambda *a: (0, "app -\n")
    check(d0.app_keys() == (None, {}), "no application names")
    with open(os.path.join(pdir, "freecad.conf"), "w") as f:
        f.write("match = freecad*\nkey_V3DK_TOP = KEY_2\nlcd_label_V3DK_TOP = Top View\n")
    app.daemon.app_keys = lambda: ("FreeCAD", {"V3DK_1": "Create sketch", "V3DK_6": "Create a local"})
    app.store.select("freecad")
    btn.refresh()
    check(btn.rows["V3DK_1"][0].get_text() == "Create sketch" and
          btn.rows["V3DK_6"][0].get_text() == "Create a local", "FreeCAD's names on its keys")
    check(btn.rows["V3DK_TOP"][0].get_text().startswith("Top View"), "the profile's own keys unchanged")
    app.store.select(None)
    btn.refresh()
    check(btn.rows["V3DK_1"][0].get_text() != "Create sketch", "not on All Applications")

    # ---- a profile imported from 3DxWare: shown, copied to profile_dir on change
    imp = os.path.join(d, "imported")
    os.makedirs(imp)
    with open(os.path.join(imp, "3dx-firefox.conf"), "w") as f:
        f.write("# imported\nmatch = firefox\nkey_V3DK_3 = KEY_LEFTCTRL + KEY_LEFTSHIFT + KEY_R\n"
                "lcd_label_V3DK_3 = Refresh\n")
    orig_dirs = ts.Store.__dict__["import_dirs"]  # the staticmethod itself
    ts.Store.import_dirs = staticmethod(lambda: [imp])
    app.daemon.app_keys = lambda: (None, {})
    app.store.select("3dx-firefox")
    p = app.store.profile
    check(p.imported_from and not p.exists and p.path == os.path.join(pdir, "3dx-firefox.conf"),
          "imported profile, not yet in profile_dir")
    btn.refresh()
    check(btn.rows["V3DK_3"][0].get_text().startswith("Refresh"), "imported keys shown")
    p.set("key_V3DK_4", "KEY_F5")
    check(ts.write_file(p) is None and os.path.isfile(p.path), "first change saves a copy")
    with open(p.path) as f:
        saved = f.read()
    check("match = firefox" in saved and "lcd_label_V3DK_3 = Refresh" in saved and "KEY_F5" in saved,
          "the copy keeps 3DxWare's keys")
    # Restore Defaults on an imported profile: back to 3DxWare's keys
    btn.restore.clicked()
    check(p.get("key_V3DK_3") == "KEY_LEFTCTRL + KEY_LEFTSHIFT + KEY_R" and not p.has("key_V3DK_4") and
          p.get("lcd_label_V3DK_3") == "Refresh", "Restore Defaults: the imported keys")
    ts.Store.import_dirs = orig_dirs
    # ... and on tdxd's FreeCAD profile: its view keys, not nothing
    with open(os.path.join(pdir, "freecad.conf"), "w") as f:
        f.write("match = freecad*\n")  # what an earlier Restore Defaults left
    app.store.select("freecad")
    btn.restore.clicked()
    ship = ts.ConfFile(os.path.join(ROOT, "packaging", "profiles", "freecad.conf"))
    check(app.store.profile.get("key_V3DK_TOP") == ship.get("key_V3DK_TOP") and
          app.store.profile.get("key_V3DK_FIT") == ship.get("key_V3DK_FIT") and
          app.store.profile.get("lcd_label_V3DK_TOP") == "Top View", "Restore Defaults: FreeCAD's view keys")
    # a profile you made yourself: no keys of its own (the base's apply)
    app.store.create_profile("mine", "mine")
    app.store.profile.set("key_V3DK_1", "KEY_F1")
    btn.restore.clicked()
    check(not app.store.profile.keys_with_prefix("key_"), "Restore Defaults: your own profile")


def main():
    tdxd = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build",
                                                              os.uname().machine, "tdxd")
    with tempfile.TemporaryDirectory() as d:
        conf_file_tests(d)
        gui_tests(d, tdxd)
    print("%d/%d checks passed" % (checks - fails, checks))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
