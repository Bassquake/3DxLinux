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

sys.dont_write_bytecode = True

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
loader = importlib.machinery.SourceFileLoader("tdx_settings",
                                              os.path.join(ROOT, "gui", "tdx-settings"))
spec = importlib.util.spec_from_loader("tdx_settings", loader)
ts = importlib.util.module_from_spec(spec)
loader.exec_module(ts)

checks = fails = 0


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

    # ---- restore button defaults in the base
    app.store.select(None)
    btn.on_restore(None)
    check(app.store.base.get("action_V3DK_PLUS") == "speed_up" and
          app.store.base.get("key_V3DK_ESC") == "KEY_ESC" and
          not app.store.base.has("key_V3DK_1"), "Buttons Restore Defaults")


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
