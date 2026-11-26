#!/usr/bin/env python3
"""Tests for the LCD applet ports (gui/3dxlinux-lcd-functionkeys, -rss, -pictures):
feed parsing, LCDCfg.xml reading, picture scanning, transitions and rendering,
with and without the extracted artwork (DXLINUX_UI_ASSETS).

    python3 tests/test_lcdapplets.py [build/ARCH]
"""
import importlib.machinery
import importlib.util
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "gui"))
sys.dont_write_bytecode = True

checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("FAIL:", what)


def load(name):
    p = os.path.join(ROOT, "gui", name)
    l = importlib.machinery.SourceFileLoader(name.replace("-", "_"), p)
    s = importlib.util.spec_from_loader(l.name, l)
    m = importlib.util.module_from_spec(s)
    l.exec_module(m)
    return m


RSS2 = b"""<?xml version="1.0"?><rss version="2.0"><channel><title>News &amp; Views</title>
<image><url>http://x/logo.png</url></image>
<item><title>One</title><pubDate>Mon, 17 Nov 2026</pubDate>
<description>&lt;p&gt;Hello &lt;b&gt;world&lt;/b&gt;&lt;/p&gt;</description><link>http://x/1</link></item>
<item><title>Two</title><description>2</description></item></channel></rss>"""
RDF = b"""<?xml version="1.0"?><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
xmlns="http://purl.org/rss/1.0/" xmlns:dc="http://purl.org/dc/elements/1.1/">
<channel><title>RDF feed</title></channel>
<item><title>R1</title><dc:date>2026-11-17</dc:date><description>d1</description><link>http://r/1</link></item>
</rdf:RDF>"""
ATOM = b"""<?xml version="1.0"?><feed xmlns="http://www.w3.org/2005/Atom"><title>Atom feed</title>
<logo>http://a/l.png</logo><entry><title>A1</title><updated>2026-11-17T10:00:00Z</updated>
<summary>s1</summary><link href="http://a/1"/></entry></feed>"""


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", os.uname().machine)
    d = tempfile.mkdtemp(prefix="tdx-applets-")
    os.environ["XDG_CONFIG_HOME"] = d
    real_assets = os.environ.get("DXLINUX_UI_ASSETS")
    os.environ["DXLINUX_UI_ASSETS"] = os.path.join(d, "no-assets")

    rss = load("3dxlinux-lcd-rss")
    t, logo, items = rss.parse_feed(RSS2)
    check(t == "News & Views" and logo == "http://x/logo.png" and len(items) == 2, "RSS 2.0 channel")
    check(items[0] == ("One", "Mon, 17 Nov 2026", "Hello world", "http://x/1"), "RSS 2.0 item: %r" % (items[0],))
    t, _l, items = rss.parse_feed(RDF)
    check(t == "RDF feed" and items == [("R1", "2026-11-17", "d1", "http://r/1")], "RSS 1.0 (RDF)")
    t, logo, items = rss.parse_feed(ATOM)
    check(t == "Atom feed" and logo == "http://a/l.png" and items[0][3] == "http://a/1", "Atom")
    try:
        rss.parse_feed(b"<html><body/></html>")
        ok = False
    except ValueError:
        ok = True
    check(ok, "a web page is not a feed")

    feed = os.path.join(d, "feed.xml")
    open(feed, "wb").write(RSS2)
    # every applet must start from a saved settings file (Configure... wrote one)
    import json
    os.makedirs(os.path.join(d, "3dxlinux"), exist_ok=True)
    saved = {"rss.json": {"feeds": [{"name": "", "url": "file://" + feed}], "duration": 7},
             "pictures.json": {"interval": 3}, "functionkeys.json": {"page": 1}}
    for name, data in saved.items():
        json.dump(data, open(os.path.join(d, "3dxlinux", name), "w"))
    started = []
    for mod, cls in ((rss, "Rss"), (load("3dxlinux-lcd-pictures"), "Pictures"),
                     (load("3dxlinux-lcd-functionkeys"), "FunctionKeys")):
        try:
            started.append(getattr(mod, cls)().cfg)
        except Exception as e:  # noqa: BLE001
            print("  %s: %r" % (cls, e))
    check(len(started) == 3 and started[0]["duration"] == 7 and started[1]["interval"] == 3,
          "applets start with a saved settings file")
    for name in saved:
        os.unlink(os.path.join(d, "3dxlinux", name))
    a = rss.Rss()
    a.cfg["feeds"] = [{"name": "", "url": "file://" + feed},
                      {"name": "Broken", "url": "file://" + os.path.join(d, "missing.xml")}]
    a.fetch("file://" + feed)
    a.fetch("file://" + os.path.join(d, "missing.xml"))
    check(a.feeds["file://" + feed]["title"] == "News & Views", "feed downloaded")
    check(a.feeds["file://" + os.path.join(d, "missing.xml")]["error"] == "Download failed",
          "download error reported")
    a.on_buttons(rss.la.DOWN, rss.la.DOWN)
    check(a.ii == 1, "DOWN: next item")
    a.on_buttons(rss.la.RIGHT, rss.la.RIGHT)
    check(a.fi == 1 and a.ii == 0, "RIGHT: next feed")
    a.snapshot(os.path.join(d, "rss.png"))
    check(os.path.getsize(os.path.join(d, "rss.png")) > 1000, "RSS renders without artwork")

    # Function Keys from the LCDCfg.xml 3dxlinuxd writes
    fk = load("3dxlinux-lcd-functionkeys")
    xml = os.path.join(d, "LCDCfg.xml")
    open(xml, "w").write("""<?xml version="1.0"?><LCDCfg><Device><Name>SpacePilot PRO</Name>
<Filters><Rotation>true</Rotation><PanZoom>false</PanZoom><Dominant>false</Dominant></Filters>
<InfoLine><Text>blender</Text><Slider>125</Slider><ButtonPressed/>
<ApplicationName>blender</ApplicationName><Image><Source/></Image></InfoLine>
<Button><V3DK>V3DK_1</V3DK><Text>Copy</Text></Button>
<Button><V3DK>V3DK_MINUS</V3DK><Text>Decrease Speed</Text></Button>
<Button><V3DK>V3DK_PLUS</V3DK><Text>Increase Speed</Text></Button></Device></LCDCfg>""")
    os.environ["DXLINUX_LCDCFG"] = xml
    f = fk.FunctionKeys()
    check(f.tick(time.time()), "LCDCfg.xml read")
    check(f.info["app"] == "blender" and not f.info["panzoom"] and f.info["slider"] == 125,
          "info line and filters")
    check(f.label("V3DK_1") == "Copy" and f.label("SPEED") == "Speed", "button texts")
    check(not f.tick(time.time()), "unchanged file: no redraw")
    f.on_buttons(fk.la.RIGHT, fk.la.RIGHT)
    f.on_buttons(fk.la.RIGHT, fk.la.RIGHT)
    f.on_buttons(fk.la.RIGHT, fk.la.RIGHT)
    check(f.page == 2, "RIGHT pages up to the last page")
    f.on_buttons(fk.la.LEFT, fk.la.LEFT)
    check(f.page == 1, "LEFT pages back")
    f.snapshot(os.path.join(d, "fk.png"))
    check(os.path.getsize(os.path.join(d, "fk.png")) > 1000, "Function Keys renders")
    # header icon: from the program's .desktop entry, SVG rendered at 32 px
    import cairo
    xdg = os.path.join(d, "xdg")
    os.makedirs(os.path.join(xdg, "applications"))
    os.makedirs(os.path.join(xdg, "icons", "hicolor", "scalable", "apps"))
    with open(os.path.join(xdg, "applications", "org.example.Cad.desktop"), "w") as fh:
        fh.write("[Desktop Entry]\nName=Cad\nExec=/opt/cad/bin/CadApp %F\nIcon=org.example.Cad\n")
    svg = os.path.join(xdg, "icons", "hicolor", "scalable", "apps", "org.example.Cad.svg")
    s = cairo.SVGSurface(svg, 100, 100)
    cr = cairo.Context(s)
    cr.set_source_rgb(0.9, 0.4, 0.1)
    cr.arc(50, 50, 40, 0, 6.3)
    cr.fill()
    s.finish()
    old_dirs = os.environ.get("XDG_DATA_DIRS")
    os.environ["XDG_DATA_DIRS"] = xdg
    p = fk.app_icon("CadApp", "/opt/cad/bin/CadApp")
    icon = fk.load_icon(p) if p else None
    check(p == svg and icon is not None and icon.get_width() == 32,
          "app icon via the .desktop entry, SVG at 32 px: %s" % p)
    check(fk.app_icon("NoSuchApp", "") is None, "no icon for an unknown program")
    # FreeCAD from Flathub: org.freecad.FreeCAD.desktop runs "flatpak run ..."; the
    # entry and icon live in Flatpak's exports folder (here: XDG_DATA_HOME's)
    fp = os.path.join(d, "home", "flatpak", "exports", "share")
    os.makedirs(os.path.join(fp, "applications"))
    os.makedirs(os.path.join(fp, "icons", "hicolor", "scalable", "apps"))
    with open(os.path.join(fp, "applications", "org.freecad.FreeCAD.desktop"), "w") as fh:
        fh.write("[Desktop Entry]\nName=FreeCAD\nExec=/usr/bin/flatpak run --branch=stable "
                 "--arch=x86_64 --command=FreeCAD --file-forwarding org.freecad.FreeCAD @@ %F @@\n"
                 "Icon=org.freecad.FreeCAD\nX-Flatpak=org.freecad.FreeCAD\n")
    fsvg = os.path.join(fp, "icons", "hicolor", "scalable", "apps", "org.freecad.FreeCAD.svg")
    import shutil
    shutil.copy(svg, fsvg)
    old_home = os.environ.get("XDG_DATA_HOME")
    os.environ["XDG_DATA_HOME"] = os.path.join(d, "home")
    fk._icon_cache.clear()
    p = fk.app_icon("FreeCAD", "/app/bin/FreeCAD")  # /proc/PID/exe inside the sandbox
    check(p == fsvg, "Flatpak FreeCAD's icon: %s" % p)
    check(fk.desktop_names("org.freecad.FreeCAD.desktop", open(os.path.join(
        fp, "applications", "org.freecad.FreeCAD.desktop")).read()) >= {"freecad", "org.freecad.freecad"},
        "names of a Flatpak entry")
    if old_home is None:
        del os.environ["XDG_DATA_HOME"]
    else:
        os.environ["XDG_DATA_HOME"] = old_home
    if old_dirs is None:
        del os.environ["XDG_DATA_DIRS"]
    else:
        os.environ["XDG_DATA_DIRS"] = old_dirs

    # Picture Viewer
    pv = load("3dxlinux-lcd-pictures")
    pics = os.path.join(d, "pics")
    os.makedirs(os.path.join(pics, "sub"))
    import cairo
    for i, (name, col) in enumerate((("a.png", (1, 0, 0)), ("b.png", (0, 1, 0)),
                                     ("sub/c.png", (0, 0, 1)))):
        s = cairo.ImageSurface(cairo.FORMAT_RGB24, 64, 48)
        cr = cairo.Context(s)
        cr.set_source_rgb(*col)
        cr.paint()
        s.write_to_png(os.path.join(pics, name))
    open(os.path.join(pics, "notes.txt"), "w").write("x")
    p = pv.Pictures()
    p.cfg.update({"folder": pics, "subfolders": False})
    p.scan()
    check(len(p.files) == 2, "pictures in the folder (not notes.txt, not subfolders)")
    p.cfg["subfolders"] = True
    p.scan()
    check(len(p.files) == 3, "include subfolders")
    first = p.idx
    p.on_buttons(pv.la.RIGHT, pv.la.RIGHT)
    check(p.idx == (first + 1) % 3 and p.prev is not None, "RIGHT: next picture with a transition")
    for kind in pv.TRANSITIONS:
        p.t_kind = kind
        p.t_start = time.time() - pv.TRANSITION_S / 2
        p.snapshot(os.path.join(d, "t.png"))
    check(True, "all transitions render")
    p.on_buttons(pv.la.OK, pv.la.OK)
    check(p.paused and p.overlay_until, "OK pauses")
    # device mini menu: MENU opens, back cancels, OK runs Configure...
    calls = []
    p._on_configure = lambda: calls.append(1)
    idx = p.idx
    p.on_buttons(pv.la.MENU, pv.la.MENU)
    p.on_buttons(pv.la.RIGHT, pv.la.RIGHT)
    check(p.menu_open and p.idx == idx, "MENU opens the mini menu; other keys wait")
    p.snapshot(os.path.join(d, "menu.png"))
    p.on_buttons(pv.la.CANCEL, pv.la.CANCEL)
    check(not p.menu_open and not calls, "back arrow cancels")
    p.on_buttons(pv.la.MENU, pv.la.MENU)
    p.on_buttons(pv.la.OK, pv.la.OK)
    check(not p.menu_open and calls == [1] and p.paused, "OK runs Configure... (not pause)")
    p.cfg.update({"folder": os.path.join(d, "empty"), "subfolders": False})
    os.makedirs(os.path.join(d, "empty"))
    p.scan()
    p.snapshot(os.path.join(d, "none.png"))
    check(p.files == [], "empty folder: 'No pictures found' screen")

    # installed layout (make install, install.sh, .deb/.rpm): programs in bin/,
    # the Python modules in lib/3dxlinux/python/ - every program must start from there
    import shutil
    inst = os.path.join(d, "installed")
    os.makedirs(os.path.join(inst, "bin"))
    os.makedirs(os.path.join(inst, "lib", "3dxlinux", "python"))
    for m in ("lcd_3dxlinux.py", "lcdapplet.py"):
        shutil.copy(os.path.join(ROOT, "gui", m), os.path.join(inst, "lib", "3dxlinux", "python", m))
    progs = ("3dxlinux-lcd-functionkeys", "3dxlinux-lcd-rss", "3dxlinux-lcd-pictures", "3dxlinux-lcdmanager", "3dxlinux-settings")
    for p in progs:
        shutil.copy(os.path.join(ROOT, "gui", p), os.path.join(inst, "bin", p))
    for p in progs:
        args = (["--snapshot", os.path.join(d, p + "-inst.png")] if p.startswith("3dxlinux-lcd-")
                else ["--version"])
        r = subprocess.run([sys.executable, "-B", os.path.join(inst, "bin", p)] + args,
                           capture_output=True, text=True, timeout=60)
        check(r.returncode == 0, "%s starts in the installed layout: %s"
              % (p, (r.stderr.strip().splitlines() or [""])[-1]))

    if real_assets:  # same with the 3DxWare artwork
        os.environ["DXLINUX_UI_ASSETS"] = real_assets
        for mod, cls in ((rss, "Rss"), (fk, "FunctionKeys"), (pv, "Pictures")):
            o = getattr(mod, cls)()
            check(o.art.dir is not None, "%s finds its artwork" % cls)
            o.snapshot(os.path.join(d, cls + "-art.png"))

    # applets talk to 3dxlinux-lcdmgr end to end (virtual LCD)
    mgr_bin = os.path.join(build, "3dxlinux-lcdmgr")
    if os.access(mgr_bin, os.X_OK):
        sock, ppm = os.path.join(d, "lcd.sock"), os.path.join(d, "lcd.ppm")
        mgr = subprocess.Popen([mgr_bin, "-N", "-s", sock, "-d", os.path.join(d, "db.ini"),
                                "-F", ppm, "-t", os.path.join(d, "none")],
                               stderr=subprocess.DEVNULL)
        env = dict(os.environ, DXLINUX_LCD_SOCKET=sock)
        procs = [subprocess.Popen([sys.executable, os.path.join(ROOT, "gui", n)], env=env,
                                  stderr=subprocess.DEVNULL)
                 for n in ("3dxlinux-lcd-functionkeys", "3dxlinux-lcd-rss", "3dxlinux-lcd-pictures")]
        import lcd_3dxlinux
        end = time.time() + 15
        names = []
        while time.time() < end:
            try:
                names = sorted(q["name"] for q in lcd_3dxlinux.status(sock)["programs"] if q["running"])
            except OSError:
                pass
            if len(names) == 3:
                break
            time.sleep(0.3)
        check(names == ["Function Keys", "Picture Viewer", "RSS Feeds"],
              "the three applets connect: %s" % names)
        # an applet callback that fails (RSS: no xdg-open) must not drop the
        # connection - that made the applet reconnect and appear twice in the list
        import contextlib
        import io

        def boom(_b):
            raise FileNotFoundError("xdg-open")
        c = lcd_3dxlinux.Connection("Crashy", path=sock)
        cd = c.open(lcd_3dxlinux.DEVICE_QVGA, on_buttons=boom)
        cd.update(bytes(320 * 240 * 4))
        i = [q["index"] for q in lcd_3dxlinux.status(sock)["programs"] if q["name"] == "Crashy"][0]
        lcd_3dxlinux.admin("select %d" % i, sock)
        cd.set_foreground(True)
        time.sleep(0.3)
        with contextlib.redirect_stderr(io.StringIO()):
            lcd_3dxlinux.admin("press 400 0", sock)
            time.sleep(0.5)
        alive = True
        try:
            c.enumerate()
        except lcd_3dxlinux.LcdError:
            alive = False
        n = sum(q["name"] == "Crashy" for q in lcd_3dxlinux.status(sock)["programs"] if q["running"])
        check(alive and n == 1, "a failing button callback keeps the connection (alive=%s, listed %d)"
              % (alive, n))
        c.close()

        # first run: 3dxlinux-lcdmgr registers and starts the applets itself (on its
        # own socket), and stops them again when it exits
        d2 = os.path.join(d, "autostart")
        os.makedirs(d2)
        sock2 = os.path.join(d2, "lcd.sock")
        mgr2 = subprocess.Popen([mgr_bin, "-v", "-s", sock2, "-d", os.path.join(d2, "db.ini"),
                                 "-F", os.path.join(d2, "lcd.ppm"), "-t", os.path.join(d2, "none")],
                                stderr=subprocess.PIPE, text=True,
                                env=dict(os.environ, XDG_CONFIG_HOME=d2))
        end = time.time() + 20
        pids = []
        while time.time() < end:
            try:
                st = lcd_3dxlinux.status(sock2)["programs"]
                if sum(q["running"] for q in st) == 3:
                    break
            except OSError:
                pass
            time.sleep(0.3)
        check(sum(q["running"] for q in st) == 3, "autostarted applets connect to 3dxlinux-lcdmgr's socket")
        mgr2.terminate()
        log = mgr2.communicate(timeout=10)[1]
        import re
        pids = [int(x) for x in re.findall(r"started .*, pid (\d+)", log)]
        time.sleep(1)
        gone = []
        for p in pids:
            try:
                os.kill(p, 0)
                os.kill(p, 15)  # do not leave it behind
            except OSError:
                gone.append(p)
        check(len(pids) == 3 and gone == pids, "3dxlinux-lcdmgr stops the applets it started: %s of %s"
              % (gone, pids))

        # an applet port that moved (install.sh's /usr/local/bin -> a package's
        # /usr/bin): its entry follows it instead of listing a dead program
        d3 = os.path.join(d, "moved")
        os.makedirs(d3)
        old = os.path.join(d3, "gone", "3dxlinux-lcd-rss")
        with open(os.path.join(d3, "db.ini"), "w") as fh:
            fh.write('[ClientApplications]\n0="%s","RSS Feeds",E1,A0,S1,L1,C2\n' % old)
        mgr3 = subprocess.Popen([mgr_bin, "-s", os.path.join(d3, "lcd.sock"),
                                 "-d", os.path.join(d3, "db.ini"), "-F", os.path.join(d3, "lcd.ppm"),
                                 "-t", os.path.join(d3, "none")], stderr=subprocess.DEVNULL)
        end = time.time() + 10
        progs = []
        while time.time() < end and not progs:
            try:
                progs = lcd_3dxlinux.status(os.path.join(d3, "lcd.sock"))["programs"]
            except OSError:
                time.sleep(0.2)
        mgr3.terminate()
        mgr3.wait(timeout=10)
        want = os.path.realpath(os.path.join(ROOT, "gui", "3dxlinux-lcd-rss"))
        check([(q["name"], q["path"]) for q in progs] == [("RSS Feeds", want)],
              "a moved applet's entry follows it: %s" % [(q["name"], q["path"]) for q in progs])
        for q in procs:
            q.terminate()
        mgr.terminate()
    print("%d/%d checks passed" % (checks - fails, checks))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
