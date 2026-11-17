#!/usr/bin/env python3
"""Tests for the LCD applet ports (gui/tdx-lcd-functionkeys, -rss, -pictures):
feed parsing, LCDCfg.xml reading, picture scanning, transitions and rendering,
with and without the extracted artwork (TDX_UI_ASSETS).

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
    real_assets = os.environ.get("TDX_UI_ASSETS")
    os.environ["TDX_UI_ASSETS"] = os.path.join(d, "no-assets")

    rss = load("tdx-lcd-rss")
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

    # Function Keys from the LCDCfg.xml tdxd writes
    fk = load("tdx-lcd-functionkeys")
    xml = os.path.join(d, "LCDCfg.xml")
    open(xml, "w").write("""<?xml version="1.0"?><LCDCfg><Device><Name>SpacePilot PRO</Name>
<Filters><Rotation>true</Rotation><PanZoom>false</PanZoom><Dominant>false</Dominant></Filters>
<InfoLine><Text>blender</Text><Slider>125</Slider><ButtonPressed/>
<ApplicationName>blender</ApplicationName><Image><Source/></Image></InfoLine>
<Button><V3DK>V3DK_1</V3DK><Text>Copy</Text></Button>
<Button><V3DK>V3DK_MINUS</V3DK><Text>Decrease Speed</Text></Button>
<Button><V3DK>V3DK_PLUS</V3DK><Text>Increase Speed</Text></Button></Device></LCDCfg>""")
    os.environ["TDX_LCDCFG"] = xml
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

    # Picture Viewer
    pv = load("tdx-lcd-pictures")
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
    p.cfg.update({"folder": os.path.join(d, "empty"), "subfolders": False})
    os.makedirs(os.path.join(d, "empty"))
    p.scan()
    p.snapshot(os.path.join(d, "none.png"))
    check(p.files == [], "empty folder: 'No pictures found' screen")

    if real_assets:  # same with the 3DxWare artwork
        os.environ["TDX_UI_ASSETS"] = real_assets
        for mod, cls in ((rss, "Rss"), (fk, "FunctionKeys"), (pv, "Pictures")):
            o = getattr(mod, cls)()
            check(o.art.dir is not None, "%s finds its artwork" % cls)
            o.snapshot(os.path.join(d, cls + "-art.png"))

    # applets talk to tdx-lcdmgr end to end (virtual LCD)
    mgr_bin = os.path.join(build, "tdx-lcdmgr")
    if os.access(mgr_bin, os.X_OK):
        sock, ppm = os.path.join(d, "lcd.sock"), os.path.join(d, "lcd.ppm")
        mgr = subprocess.Popen([mgr_bin, "-N", "-s", sock, "-d", os.path.join(d, "db.ini"),
                                "-F", ppm, "-t", os.path.join(d, "none")],
                               stderr=subprocess.DEVNULL)
        env = dict(os.environ, TDX_LCD_SOCKET=sock)
        procs = [subprocess.Popen([sys.executable, os.path.join(ROOT, "gui", n)], env=env,
                                  stderr=subprocess.DEVNULL)
                 for n in ("tdx-lcd-functionkeys", "tdx-lcd-rss", "tdx-lcd-pictures")]
        import tdxlcd
        end = time.time() + 15
        names = []
        while time.time() < end:
            try:
                names = sorted(q["name"] for q in tdxlcd.status(sock)["programs"] if q["running"])
            except OSError:
                pass
            if len(names) == 3:
                break
            time.sleep(0.3)
        check(names == ["Function Keys", "Picture Viewer", "RSS Feeds"],
              "the three applets connect: %s" % names)
        for q in procs:
            q.terminate()
        mgr.terminate()
    print("%d/%d checks passed" % (checks - fails, checks))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
