#!/usr/bin/env python3
"""Integration test for tdx-lcdmgr with a virtual LCD (-F frame.ppm):
the C demo applet (libtdxlcd) and a Python applet (tdxlcd.py) connect, the
program list, menu, switching algorithms, alerts, enable/disable, Configure
and the LGLCDDB-format database are checked.

    python3 tests/test_lcdmgr.py [build/ARCH]
"""
import os
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "gui"))
sys.dont_write_bytecode = True
import tdxlcd  # noqa: E402

checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("FAIL:", what)


def wait(pred, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return False


def frame_pixel(path, x, y):
    with open(path, "rb") as f:
        data = f.read()
    # header "P6\n320 240\n255\n"
    off = data.index(b"255\n") + 4
    i = off + (y * 320 + x) * 3
    return data[i], data[i + 1], data[i + 2]


def solid(rgb):
    return bytes(rgb) * (320 * 240)


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", os.uname().machine)
    d = tempfile.mkdtemp(prefix="tdx-lcdmgr-test-")
    sock, dbp, ppm = (os.path.join(d, n) for n in ("lcd.sock", "lcdmgr.ini", "lcd.ppm"))
    mgr = subprocess.Popen([os.path.join(build, "tdx-lcdmgr"), "-v", "-s", sock, "-d", dbp,
                            "-F", ppm, "-t", os.path.join(d, "no-tdxd.sock")],
                           stderr=subprocess.PIPE, text=True)
    log = []
    threading.Thread(target=lambda: log.extend(mgr.stderr), daemon=True).start()
    os.environ["TDX_LCD_SOCKET"] = sock
    try:
        check(wait(lambda: os.path.exists(sock)), "manager socket appears")
        check(wait(lambda: os.path.exists(ppm)), "idle screen rendered")
        st = tdxlcd.status(sock)
        check(st["present"] and st["programs"] == [], "virtual device present, no programs")

        # --- C applet through libtdxlcd
        demo = subprocess.Popen([os.path.join(build, "tdx-lcd-demo"), "-n", "Demo C", "-a"],
                                env=dict(os.environ, TDX_LCD_SOCKET=sock))
        check(wait(lambda: any(p["running"] for p in tdxlcd.status(sock)["programs"])),
              "C demo connected via libtdxlcd")
        check(wait(lambda: frame_pixel(ppm, 160, 230) == (0x20, 0x30, 0x40)),
              "C demo frame on the LCD")

        # --- Python applet
        events = {"buttons": [], "configure": 0, "notify": []}
        conn = tdxlcd.Connection("Py Red", persistent=True,
                                 on_configure=lambda: events.__setitem__(
                                     "configure", events["configure"] + 1),
                                 on_notify=lambda *a: events["notify"].append(a[0]))
        check(conn.enumerate(0)["width"] == 320, "enumerate: 320 wide")
        dev = conn.open(tdxlcd.DEVICE_QVGA, on_buttons=events["buttons"].append)
        dev.update_rgb(solid((255, 0, 0)), sync=True)
        st = tdxlcd.status(sock)
        names = [p["name"] for p in st["programs"]]
        check(names == ["Demo C", "Py Red"], "both programs listed in order: %s" % names)
        red = names.index("Py Red")
        # manual mode, nothing chosen yet: the first program stays shown
        check(st["shown"] == 0, "manual mode shows the first program")

        # --- on-LCD menu: LCD key opens, DOWN, OK runs the second program
        tdxlcd.admin("press 80000000 0 2000 0 400 0", sock)
        check(wait(lambda: tdxlcd.status(sock)["shown"] == red), "menu DOWN+OK switched")
        check(wait(lambda: frame_pixel(ppm, 160, 120) == (255, 0, 0)), "red frame shown")
        check(tdxlcd.status(sock)["manual"] == red, "ManualModeApplet updated")

        # --- buttons go to the shown program (not the LCD key)
        tdxlcd.admin("press 1000 0", sock)
        check(wait(lambda: 0x1000 in events["buttons"]), "UP forwarded to the shown applet")
        check(dev.read_soft_buttons() == 0, "lgLcdReadSoftButtons after release")

        # --- menu CANCEL leaves the selection alone
        tdxlcd.admin("press 80000000 0 2000 0 800 0", sock)
        time.sleep(0.2)
        check(tdxlcd.status(sock)["shown"] == red, "CANCEL keeps the program")

        # --- quick switch: LCD key goes straight to the next program
        tdxlcd.admin("set quickswitch 1", sock)
        tdxlcd.admin("press 80000000 0", sock)
        check(wait(lambda: tdxlcd.status(sock)["shown"] == 0), "quick switch to next")
        tdxlcd.admin("set quickswitch 0", sock)

        # --- alerts pre-empt, then fall back
        dev.update_rgb(solid((0, 255, 0)), priority=tdxlcd.PRIORITY_ALERT, sync=True)
        check(wait(lambda: tdxlcd.status(sock)["shown"] == red), "alert shown immediately")
        tdxlcd.admin("set alerts 0", sock)
        check(wait(lambda: tdxlcd.status(sock)["shown"] == 0), "alerts off: back to manual")
        tdxlcd.admin("set alerts 1", sock)
        dev.update_rgb(solid((0, 0, 255)), priority=tdxlcd.PRIORITY_NORMAL)

        # --- rotation every Delay seconds
        tdxlcd.admin("set algorithm 2", sock)
        tdxlcd.admin("set delay 1", sock)
        seen = set()
        end = time.time() + 3.5
        while time.time() < end:
            seen.add(tdxlcd.status(sock)["shown"])
            time.sleep(0.1)
        check(seen >= {0, red}, "rotation visits both programs: %s" % seen)
        tdxlcd.admin("set algorithm 1", sock)

        # --- foreground request
        tdxlcd.admin("select 0", sock)
        dev.set_foreground(True)
        check(wait(lambda: tdxlcd.status(sock)["shown"] == red), "foreground request honoured")
        tdxlcd.admin("program %d foreground 0" % red, sock)
        check(wait(lambda: tdxlcd.status(sock)["shown"] == 0), "L flag off: request ignored")
        tdxlcd.admin("program %d foreground 1" % red, sock)
        dev.set_foreground(False)

        # --- Configure... and disable/enable notifications
        check(tdxlcd.admin("program %d configure" % red, sock).startswith("ok"), "configure ok")
        check(wait(lambda: events["configure"] == 1), "configure callback ran")
        tdxlcd.admin("program %d enabled 0" % red, sock)
        check(wait(lambda: tdxlcd.NOTIFICATION_APPLET_DISABLED in events["notify"]),
              "APPLET_DISABLED notification")
        try:
            dev.update_rgb(solid((1, 2, 3)))
            ok = False
        except tdxlcd.LcdError as e:
            ok = e.code == tdxlcd.ERROR_DEVICE_NOT_CONNECTED
        check(ok, "disabled applet can no longer draw")
        tdxlcd.admin("program %d enabled 1" % red, sock)
        check(wait(lambda: tdxlcd.NOTIFICATION_APPLET_ENABLED in events["notify"]),
              "APPLET_ENABLED notification")

        # --- identify / defaults / brightness
        check(tdxlcd.admin("identify", sock).startswith("ok"), "identify")
        check(wait(lambda: frame_pixel(ppm, 2, 2) == (255, 255, 255)), "identify screen")
        tdxlcd.admin("set brightness 40", sock)
        tdxlcd.admin("set delay 15", sock)
        check(tdxlcd.status(sock)["settings"]["brightness"] == 40, "brightness set")
        tdxlcd.admin("defaults", sock)
        s = tdxlcd.status(sock)["settings"]
        check(s["delay"] == 8 and s["algorithm"] == 1 and s["brightness"] == 100,
              "defaults restore Delay 8, manual, brightness 100: %s" % s)

        # --- database in LGLCDDB format
        text = open(dbp).read()
        check("[ClientApplications]" in text and '"Py Red",E1,A0,S1,L1,C2' in text,
              "program saved like LGLCDDB.TXT")
        check("[DEVICE_0]" in text and "FamilyId=32" in text and "Delay=8" in text,
              "device section saved")

        # --- disconnects
        dev.close()
        conn.close()
        demo.terminate()
        demo.wait(5)
        check(wait(lambda: not any(p["running"] for p in tdxlcd.status(sock)["programs"])),
              "applets gone from the running list")
        check(wait(lambda: frame_pixel(ppm, 160, 14)[2] > 0x90), "idle screen again")
        names = [p["name"] for p in tdxlcd.status(sock)["programs"]]
        check(names == ["Demo C", "Py Red"], "persistent programs stay listed")
    finally:
        mgr.terminate()
        try:
            mgr.wait(5)
        except subprocess.TimeoutExpired:
            mgr.kill()
    if fails:
        print("".join(log[-30:]))
    print("%d/%d checks passed" % (checks - fails, checks))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
