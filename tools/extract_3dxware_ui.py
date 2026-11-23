#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Extract the 3DxWare control-panel artwork from YOUR copy of the installer.

tdx-settings looks like the 3DxWare 10 panel when these files are present.
The artwork belongs to 3Dconnexion and is not shipped with tdxd; this tool
copies it out of the installer you downloaded, for use on your own machine.

    tools/extract_3dxware_ui.py 3DxWare64_v10-9-14_b745.exe [OUTDIR]

OUTDIR defaults to ~/.local/share/tdxd/3dxware-ui (or, as root,
/usr/local/share/tdxd/3dxware-ui). Needs 7z (p7zip-full / p7zip-plugins).
With the default OUTDIR the programs' window icons (the 3Dconnexion logo, the
LCD Manager's) also go into the hicolor icon theme as tdxd-3dxware and
tdxd-lcdmanager, and tdx-settings.desktop is pointed at the logo, so the
desktop's taskbar shows them like Windows does.

What it does, without running any installer code:
  installer.exe --7z--> 3DxWinCore64.msi --7z--> Data1.cab + MSI tables
  !File table (via _StringPool/_StringData) maps cab keys to file names
  3DxSmartUi.exe, 3DxPair.exe: .NET .resources blobs are parsed for the named PNGs
  Base.xml: device image and support status per USB product id
  <Application>.xml + en-US.xml: each application's key settings -> profiles/
  FreeCAD_Navlib.xml: FreeCAD's per-workbench keys -> freecad/buttons.json
"""

import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zlib

# installer files copied by name (device photos, button / macro icons)
FILE_PATTERNS = [r".*_Head\.png$", r"V3DK_.*\.png$", r"KB_.*\.png$", r"Macro_.*\.png$",
                 r"Driver_.*\.png$", r"HIDMouse_.*\.png$", r"3Dx\.png$", r"Keyboard\.png$",
                 r"Settings\.png$", r"TaskSwitcher\.png$", r"Unknown3DxImage\.png$",
                 r"Command-Icon_Calibrate_24x24\.svg$"]


def run7z(*args):
    exe = shutil.which("7z") or shutil.which("7za") or shutil.which("7zz")
    if not exe:
        sys.exit("7z not found: install p7zip-full (Debian/Ubuntu) or p7zip-plugins (Fedora)")
    p = subprocess.run([exe] + list(args), capture_output=True, text=True)
    if p.returncode:
        sys.exit("7z failed: %s" % (p.stderr or p.stdout)[-400:])


def msi_strings(d):
    pool = open(os.path.join(d, "!_StringPool"), "rb").read()
    data = open(os.path.join(d, "!_StringData"), "rb").read()
    codepage, = struct.unpack_from("<I", pool, 0)
    wide = bool(codepage & 0x80000000)
    enc = {65001: "utf-8", 1252: "cp1252", 0: "cp1252"}.get(codepage & 0xFFFF, "cp1252")
    strings, off, i = [None], 0, 4
    while i + 4 <= len(pool):
        size, refs = struct.unpack_from("<HH", pool, i)
        i += 4
        if size == 0 and refs != 0:  # long string: real size follows
            size, = struct.unpack_from("<I", pool, i)
            i += 4
        strings.append(data[off:off + size].decode(enc, "replace"))
        off += size
    return strings, (3 if wide else 2)


def msi_files(d, cabdir=None):
    """{cab key: long file name} from the column-major !File table."""
    strings, sref = msi_strings(d)
    raw = open(os.path.join(d, "!File"), "rb").read()
    # File, Component_, FileName, FileSize(i4), Version, Language, Attributes(i2),
    # Sequence (i2 in older schemas, i4 in newer). Both can divide the table size,
    # so keep the layout whose FileSize column matches the extracted cab files.
    best, best_score = {}, -1
    for seq in (4, 2):
        cand = _msi_files_layout(raw, strings, [sref, sref, sref, 4, sref, sref, 2, seq])
        if cand is None:
            continue
        files, sizes = cand
        score = sum(1 for k, s in sizes.items()
                    if cabdir and os.path.isfile(os.path.join(cabdir, k))
                    and os.path.getsize(os.path.join(cabdir, k)) == s)
        if score > best_score:
            best, best_score = files, score
    return best


def _msi_files_layout(raw, strings, widths):
    """({key: name}, {key: size}) for one column layout, or None if it doesn't fit."""
    if len(raw) % sum(widths):
        return None
    rows = len(raw) // sum(widths)
    cols, off = [], 0
    for w in widths:
        cols.append([int.from_bytes(raw[off + r * w:off + (r + 1) * w], "little")
                     for r in range(rows)])
        off += rows * w
    if any(i >= len(strings) for c in (cols[0], cols[2]) for i in c):
        return None
    out, sizes = {}, {}
    for r in range(rows):
        key, name = strings[cols[0][r]], strings[cols[2][r]]
        if not key or not name:
            return None
        out[key] = name.split("|")[-1]
        sizes[key] = cols[3][r] ^ 0x80000000  # MSI stores integers with the sign bit flipped
    return out, sizes


def r7(d, p):
    v = s = 0
    while True:
        b = d[p]
        p += 1
        v |= (b & 0x7F) << s
        s += 7
        if not b & 0x80:
            return v, p


def dotnet_images(pe):
    """Named PNGs from every .NET .resources blob in a PE (parsed, never executed)."""
    d = open(pe, "rb").read()
    found, p = {}, 0
    while True:
        p = d.find(b"\xce\xca\xef\xbe", p)
        if p < 0:
            return found
        try:
            base = p
            q = p + 8
            skip, = struct.unpack_from("<I", d, q)
            q += 4 + skip
            _ver, nres, ntypes = struct.unpack_from("<III", d, q)
            q += 12
            for _ in range(ntypes):
                n, q = r7(d, q)
                q += n
            while (q - base) % 8:
                q += 1
            q += 8 * nres
            dataoff, = struct.unpack_from("<I", d, q)
            q += 4
            ents = []
            for _ in range(nres):
                n, q = r7(d, q)
                name = d[q:q + n].decode("utf-16le")
                q += n
                off, = struct.unpack_from("<I", d, q)
                q += 4
                ents.append((base + dataoff + off, name))
            ents.sort()
            for i, (at, name) in enumerate(ents):
                end = ents[i + 1][0] if i + 1 < len(ents) else min(len(d), at + 8_000_000)
                chunk = d[at:end]
                k = chunk.find(b"\x89PNG\r\n\x1a\n")
                if k >= 0 and re.fullmatch(r"[A-Za-z0-9_.\-]+", name):
                    e = chunk.find(b"IEND", k)
                    if e > 0:
                        found[name] = chunk[k:e + 8]
        except (struct.error, IndexError, UnicodeDecodeError):
            pass
        p += 4


def pe_resources(path):
    """[(type, name, data)] from a PE's .rsrc (types/names: int id or str)."""
    d = open(path, "rb").read()
    pe, = struct.unpack_from("<I", d, 0x3C)
    nsec, = struct.unpack_from("<H", d, pe + 6)
    opt_size, = struct.unpack_from("<H", d, pe + 20)
    magic, = struct.unpack_from("<H", d, pe + 24)
    dd = pe + 24 + (112 if magic == 0x20B else 96)
    rva, size = struct.unpack_from("<II", d, dd + 2 * 8)  # data directory 2 = resources
    secs = []
    for i in range(nsec):
        s = pe + 24 + opt_size + i * 40
        vsz, va, raw_sz, raw = struct.unpack_from("<IIII", d, s + 8)
        secs.append((va, max(vsz, raw_sz), raw))

    def off(r):
        for va, sz, raw in secs:
            if va <= r < va + sz:
                return raw + r - va
        raise ValueError("rva %x" % r)
    base = off(rva)

    def name_of(entry_name):
        if entry_name & 0x80000000:
            o = base + (entry_name & 0x7FFFFFFF)
            n, = struct.unpack_from("<H", d, o)
            return d[o + 2:o + 2 + 2 * n].decode("utf-16le")
        return entry_name

    def walk(o, depth, path, out):
        nnamed, nid = struct.unpack_from("<HH", d, o + 12)
        for i in range(nnamed + nid):
            ename, eoff = struct.unpack_from("<II", d, o + 16 + i * 8)
            p = path + [name_of(ename)]
            if eoff & 0x80000000:
                walk(base + (eoff & 0x7FFFFFFF), depth + 1, p, out)
            else:
                drva, dsz = struct.unpack_from("<II", d, base + eoff)
                out.append((p[0], p[1] if len(p) > 1 else None, d[off(drva):off(drva) + dsz]))
    out = []
    walk(base, 0, [], out)
    return out


def unpack_msi(installer, msi_name, t):
    """{long file name: extracted path} for one MSI inside the installer."""
    run7z("x", "-y", "-o" + t, installer, msi_name)
    msi = os.path.join(t, msi_name)
    if not os.path.exists(msi):
        return None
    md = os.path.join(t, msi_name + ".d")
    run7z("x", "-y", "-o" + md, msi)
    if not os.path.exists(os.path.join(md, "!_StringPool")):
        # 7z opened the embedded cabinet directly: files are named by key (LCDMon_exe)
        return {re.sub(r"_([A-Za-z0-9]+)$", r".\1", k): os.path.join(md, k)
                for k in os.listdir(md) if os.path.isfile(os.path.join(md, k))}
    cd = os.path.join(t, msi_name + ".cab")
    for c in [f for f in os.listdir(md) if f.lower().endswith(".cab")]:
        run7z("x", "-y", "-o" + cd, os.path.join(md, c))
    by_name = {}
    for key, name in msi_files(md, cd).items():
        path = os.path.join(cd, key)
        if os.path.isfile(path):
            by_name.setdefault(name, path)
    return by_name


def lcdmon_images(lcdmon, outdir):
    """LCDMon.exe's named PNG / bitmap resources (sidebar icons, panes, textures)."""
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for typ, name, data in pe_resources(lcdmon):
        if not isinstance(name, str):
            continue
        fn = re.sub(r"[^A-Za-z0-9_.-]", "_", name)
        if data[:8] == b"\x89PNG\r\n\x1a\n":
            open(os.path.join(outdir, fn + ".png"), "wb").write(data)
            n += 1
        elif typ == 2:  # RT_BITMAP: add the file header
            hsz, = struct.unpack_from("<I", data, 0)
            bpp, = struct.unpack_from("<H", data, 14)
            ncol, = struct.unpack_from("<I", data, 32) if hsz >= 36 else (0,)
            pal = (ncol or (1 << bpp if bpp <= 8 else 0)) * 4
            hdr = b"BM" + struct.pack("<IHHI", 14 + len(data), 0, 0, 14 + hsz + pal)
            open(os.path.join(outdir, fn + ".bmp"), "wb").write(hdr + data)
            n += 1
    return n


# ---- image codecs (pure Python: PNG read/write, BMP/ICO DIB read, PAM write)

def png_read(data):
    """8-bit PNG (grey, RGB, palette, grey+alpha, RGBA; not interlaced) -> (w, h, RGBA)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    o, idat, pal, trns = 8, b"", None, None
    while o < len(data):
        n, = struct.unpack_from(">I", data, o)
        typ = data[o + 4:o + 8]
        body = data[o + 8:o + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype, _c, _f, inter = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            pal = body
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat += body
        o += 12 + n
    if depth != 8 or inter:
        raise ValueError("unsupported PNG (depth %d, interlace %d)" % (depth, inter))
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * ch
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        for x in range(w):
            q = (y * w + x) * 4
            if ctype == 6:
                out[q:q + 4] = line[x * 4:x * 4 + 4]
            elif ctype == 2:
                out[q:q + 3] = line[x * 3:x * 3 + 3]
                out[q + 3] = 255
            elif ctype == 0:
                out[q:q + 3] = bytes([line[x]]) * 3
                out[q + 3] = 255
            elif ctype == 4:
                out[q:q + 3] = bytes([line[x * 2]]) * 3
                out[q + 3] = line[x * 2 + 1]
            else:
                i = line[x]
                out[q:q + 3] = pal[i * 3:i * 3 + 3]
                out[q + 3] = trns[i] if trns and i < len(trns) else 255
    return w, h, bytes(out)


def png_write(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def pam_write(path, w, h, rgba):
    with open(path, "wb") as f:
        f.write(b"P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" % (w, h))
        f.write(rgba)


def dib_read(data, icon=False):
    """BITMAPINFOHEADER DIB (1/4/8/24/32 bpp, BI_RGB) -> (w, h, RGBA). Icons carry an AND mask."""
    hsz, w, h, _pl, bpp, comp = struct.unpack_from("<IiiHHI", data, 0)
    ncol, = struct.unpack_from("<I", data, 32)
    if icon:
        h //= 2
    top_down = h < 0
    h = abs(h)
    if comp not in (0, 3):
        raise ValueError("compressed DIB")
    npal = ncol or (1 << bpp if bpp <= 8 else 0)
    pal = data[hsz:hsz + 4 * npal]
    o = hsz + 4 * npal + (12 if comp == 3 and hsz == 40 else 0)
    stride = ((w * bpp + 31) // 32) * 4
    out = bytearray(w * h * 4)
    any_alpha = False
    for y in range(h):
        row = data[o + y * stride:o + (y + 1) * stride]
        ty = y if top_down else h - 1 - y
        for x in range(w):
            q = (ty * w + x) * 4
            if bpp == 32:
                b, g, r, a = row[x * 4:x * 4 + 4]
                any_alpha |= a != 0
            elif bpp == 24:
                b, g, r = row[x * 3:x * 3 + 3]
                a = 255
            else:
                bits = row[x * bpp // 8] >> (8 - bpp - (x * bpp) % 8) & ((1 << bpp) - 1)
                b, g, r = pal[bits * 4:bits * 4 + 3]
                a = 255
            out[q:q + 4] = bytes((r, g, b, a))
    if bpp == 32 and not any_alpha:
        for i in range(3, len(out), 4):
            out[i] = 255
    if icon and bpp < 32:  # 1-bpp AND mask follows the colour bits
        mo = o + stride * h
        mstride = ((w + 31) // 32) * 4
        for y in range(h):
            ty = h - 1 - y
            for x in range(w):
                if data[mo + y * mstride + x // 8] >> (7 - x % 8) & 1:
                    out[(ty * w + x) * 4 + 3] = 0
    return w, h, bytes(out)


def save_both(base, w, h, rgba):
    png_write(base + ".png", w, h, rgba)
    pam_write(base + ".pam", w, h, rgba)


def applet_art(exe, outdir):
    """An applet's bitmaps, PNG resources and main icon -> outdir/*.png (+ .pam)."""
    os.makedirs(outdir, exist_ok=True)
    n = 0
    icons = {}
    groups = []
    for typ, name, data in pe_resources(exe):
        try:
            if typ == 2:
                save_both(os.path.join(outdir, "bitmap_%s" % name), *dib_read(data))
                n += 1
            elif data[:8] == b"\x89PNG\r\n\x1a\n" and typ != 3:
                save_both(os.path.join(outdir, "png_%s" % name), *png_read(data))
                n += 1
            elif typ == 3:
                icons[name] = data
            elif typ == 14:
                groups.append((name, data))
        except (ValueError, struct.error, zlib.error, KeyError):
            pass
    # the executable's icon: 32x32 for the program list, every size for windows
    sizes = exe_icon_sizes(icons, groups)
    if 32 in sizes:
        save_both(os.path.join(outdir, "icon"), *sizes[32])
    for size, img in sizes.items():
        png_write(os.path.join(outdir, "icon_%d.png" % size), *img)
    return n


PNG_SIG = b"\x89PNG\r\n\x1a\n"


def exe_icon_sizes(icons, groups):
    """The executable's icon (first icon group, lowest id) -> {size: (w, h, rgba)},
    the deepest colour entry of each size."""
    if not groups:
        return {}
    groups.sort(key=lambda g: (isinstance(g[0], str), g[0]))
    gdata = groups[0][1]
    cnt, = struct.unpack_from("<H", gdata, 4)
    best = {}
    for i in range(cnt):
        w, _h, _c, _r, _p, bpp, _sz, iid = struct.unpack_from("<BBBBHHIH", gdata, 6 + i * 14)
        size = w or 256
        if iid in icons and (size not in best or bpp > best[size][0]):
            best[size] = (bpp, iid)
    out = {}
    for size, (_bpp, iid) in best.items():
        d = icons.get(iid)
        try:
            out[size] = png_read(d) if d[:8] == PNG_SIG else dib_read(d, icon=True)
        except (TypeError, ValueError, struct.error, zlib.error, KeyError):
            pass
    return out


def app_icons(exe, outdir, key):
    """A program's window icon in every size -> outdir/<key>_<size>.png."""
    icons, groups = {}, []
    for typ, name, data in pe_resources(exe):
        if typ == 3:
            icons[name] = data
        elif typ == 14:
            groups.append((name, data))
    sizes = exe_icon_sizes(icons, groups)
    os.makedirs(outdir, exist_ok=True)
    for size, img in sizes.items():
        png_write(os.path.join(outdir, "%s_%d.png" % (key, size)), *img)
    return sorted(sizes)


# .desktop entry -> its icon (icon-theme name tdxd-<key>)
DESKTOP_ICONS = {"tdx-settings": "3dxware", "tdx-lcdmanager": "lcdmanager",
                 "tdx-lcd-functionkeys": "functionkeys", "tdx-lcd-rss": "rss",
                 "tdx-lcd-pictures": "pictures"}


def theme_icon_files(out):
    """(key, size, path) of every window icon extracted to out."""
    found = []
    appicons = os.path.join(out, "appicons")
    for f in sorted(os.listdir(appicons)) if os.path.isdir(appicons) else []:
        m = re.match(r"(\w+)_(\d+)\.png$", f)
        if m:
            found.append((m.group(1), int(m.group(2)), os.path.join(appicons, f)))
    for key in ("functionkeys", "rss", "pictures"):
        d = os.path.join(out, "lcdapplets", key)
        for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            m = re.match(r"icon_(\d+)\.png$", f)
            if m:
                found.append((key, int(m.group(1)), os.path.join(d, f)))
    return found


def install_theme_icons(out, theme_root, apps_dir, system_apps=("/usr/local/share", "/usr/share")):
    """Put the window icons into the hicolor icon theme as tdxd-<key> and point
    the installed .desktop entries at the largest one. Wayland desktops (GNOME, KDE, WSLg)
    take a window's taskbar icon from the .desktop entry named like its app id
    (tdx-settings, tdx-lcdmanager, tdx-lcd-rss, ...), never from the window."""
    n = 0
    largest = {}  # key -> (size, installed path)
    for key, size, path in theme_icon_files(out):
        d = os.path.join(theme_root, "hicolor", "%dx%d" % (size, size), "apps")
        os.makedirs(d, exist_ok=True)
        dst = os.path.join(d, "tdxd-%s.png" % key)
        shutil.copyfile(path, dst)
        if size > largest.get(key, (0, None))[0]:
            largest[key] = (size, os.path.abspath(dst))
        n += 1
    if n and shutil.which("gtk-update-icon-cache") and \
            os.path.isfile(os.path.join(theme_root, "hicolor", "index.theme")):
        subprocess.call(["gtk-update-icon-cache", "-q", "-f", os.path.join(theme_root, "hicolor")])
    for entry, key in DESKTOP_ICONS.items():
        if key not in largest:
            continue
        for base in system_apps:
            src = os.path.join(base, "applications", entry + ".desktop")
            if not os.path.isfile(src):
                continue
            # an absolute path, not the theme name: WSLg (and some docks) only
            # search /usr/share/icons, but every desktop accepts a file here
            with open(src) as f:
                text = re.sub(r"(?m)^Icon=.*$", "Icon=" + largest[key][1], f.read())
            # this copy outlives an uninstall (nothing removes files in $HOME):
            # TryExec makes the menu hide it once the program is gone
            if not re.search(r"(?m)^TryExec=", text):
                text = re.sub(r"(?m)^(Exec=(\S+).*)$", r"\1\nTryExec=\2", text, count=1)
            # as root this rewrites the installed entry; as a user it is the
            # ~/.local/share/applications override, which wins over the system one
            os.makedirs(apps_dir, exist_ok=True)
            with open(os.path.join(apps_dir, entry + ".desktop"), "w") as f:
                f.write(text)
            break
    return n


def devices_json(base_xml):
    root = ET.parse(base_xml).getroot()
    out = {}
    for dev in root.iter("Device"):
        pid = (dev.findtext("ProductID") or "").lower()
        if not pid:
            continue
        src = dev.findtext("Image/Source") or ""
        m = re.search(r"([A-Za-z0-9_]+\.png)", src)
        out[pid] = {"name": dev.findtext("Name") or "",
                    "image": m.group(1) if m else None,
                    "support": dev.findtext("SupportStatus") or None}
    return out


def freecad_buttons(navlib_xml):
    """3DxWare's FreeCAD key setup (FreeCAD_Navlib.xml, for FreeCAD 0.22+ through
    NavLib) -> what the tdxd FreeCAD add-on needs:
      banks:   workbench (FreeCAD's name, "Default" otherwise) -> {V3DK id: action id}
      actions: action id -> {name, command, index} (FreeCAD command, a group
               command's entry number) or {name, menu} (a radial menu)
      menus:   menu id -> {name, items: [action ids]}
    Only FreeCAD commands and radial menus: 3DxWare's own functions on a key
    (or a key a workbench leaves out) stay with tdxd's configuration."""
    root = ET.parse(navlib_xml).getroot()
    names = {a.findtext("ID"): a.findtext("Name") or "" for a in root.iter("ButtonAction")}
    menu_of = {a.findtext("ID"): a.findtext("MenuID") for a in root.iter("ButtonAction")
               if a.findtext("MenuID")}
    actions, menus, banks = {}, {}, {}

    def action(aid):
        if aid in actions:
            return True
        if aid.startswith("AppCommand_"):
            parts = aid[len("AppCommand_"):].split("|")
            if len(parts) < 2 or not parts[1]:
                return False
            idx = int(parts[2]) if len(parts) > 2 and parts[2].isdigit() else None
            actions[aid] = {"name": names.get(aid) or parts[1], "command": parts[1], "index": idx}
            return True
        if aid.startswith("RadialMenu_") and menu_of.get(aid):
            mid = menu_of[aid]
            m = next((m for m in root.iter("Menu") if m.findtext("ID") == mid), None)
            if m is None:
                return False
            items = [b.findtext("Output/ActionID") for b in m.findall("Button")]
            items = [i for i in items if i and action(i)]
            menus[mid] = {"name": m.findtext("Name") or names.get(aid, ""), "items": items}
            actions[aid] = {"name": names.get(aid) or menus[mid]["name"], "menu": mid}
            return True
        return False

    for dev in root.iter("Device"):
        if dev.findtext("ID") != "ID_Standard_3D_Mouse":
            continue
        for bank in dev.iter("ButtonBank"):
            keys = {}
            for b in bank.findall("Button"):
                key, aid = b.findtext("Input/ActionID"), b.findtext("Output/ActionID")
                if key and key.startswith("V3DK_") and aid and action(aid):
                    keys[key] = aid
            banks[bank.findtext("ID") or "Default"] = keys
    return {"banks": banks, "actions": actions, "menus": menus}


# ---- 3DxWare's application configurations -> tdxd profiles -------------------

# USB HID keyboard usage (what 3DxWare's macros store) -> Linux KEY_ name
HID_KEYS = {0x28: "ENTER", 0x29: "ESC", 0x2A: "BACKSPACE", 0x2B: "TAB", 0x2C: "SPACE",
            0x2D: "MINUS", 0x2E: "EQUAL", 0x2F: "LEFTBRACE", 0x30: "RIGHTBRACE",
            0x31: "BACKSLASH", 0x32: "BACKSLASH", 0x33: "SEMICOLON", 0x34: "APOSTROPHE",
            0x35: "GRAVE", 0x36: "COMMA", 0x37: "DOT", 0x38: "SLASH", 0x39: "CAPSLOCK",
            0x46: "SYSRQ", 0x47: "SCROLLLOCK", 0x48: "PAUSE", 0x49: "INSERT", 0x4A: "HOME",
            0x4B: "PAGEUP", 0x4C: "DELETE", 0x4D: "END", 0x4E: "PAGEDOWN", 0x4F: "RIGHT",
            0x50: "LEFT", 0x51: "DOWN", 0x52: "UP", 0x53: "NUMLOCK", 0x54: "KPSLASH",
            0x55: "KPASTERISK", 0x56: "KPMINUS", 0x57: "KPPLUS", 0x58: "KPENTER",
            0x62: "KP0", 0x63: "KPDOT", 0x64: "102ND", 0x65: "COMPOSE", 0x67: "KPEQUAL",
            0x85: "KPCOMMA", 0xE0: "LEFTCTRL", 0xE1: "LEFTSHIFT", 0xE2: "LEFTALT",
            0xE3: "LEFTMETA", 0xE4: "RIGHTCTRL", 0xE5: "RIGHTSHIFT", 0xE6: "RIGHTALT",
            0xE7: "RIGHTMETA"}
HID_KEYS.update({0x04 + i: chr(ord("A") + i) for i in range(26)})
HID_KEYS.update({0x1E + i: str((i + 1) % 10) for i in range(10)})
HID_KEYS.update({0x3A + i: "F%d" % (i + 1) for i in range(12)})
HID_KEYS.update({0x59 + i: "KP%d" % (i + 1) for i in range(9)})
HID_KEYS.update({0x68 + i: "F%d" % (i + 13) for i in range(12)})
MODIFIERS = {"control": "LEFTCTRL", "ctrl": "LEFTCTRL", "shift": "LEFTSHIFT", "alt": "LEFTALT",
             "win": "LEFTMETA", "windows": "LEFTMETA", "meta": "LEFTMETA"}
# typed text (a macro's <Text>) on a US layout
_SHIFTED = dict(zip('!@#$%^&*()_+{}|:"<>?~', "1234567890") )
_SHIFTED.update(dict(zip('_+{}|:"<>?~', ["MINUS", "EQUAL", "LEFTBRACE", "RIGHTBRACE", "BACKSLASH",
                                          "SEMICOLON", "APOSTROPHE", "COMMA", "DOT", "SLASH",
                                          "GRAVE"])))
_PLAIN = {" ": "SPACE", "-": "MINUS", "=": "EQUAL", "[": "LEFTBRACE", "]": "RIGHTBRACE",
          "\\": "BACKSLASH", ";": "SEMICOLON", "'": "APOSTROPHE", ",": "COMMA", ".": "DOT",
          "/": "SLASH", "`": "GRAVE", "\n": "ENTER", "\t": "TAB"}
# 3DxWare's keyboard keys (KB_*) and driver functions -> tdxd
KB_KEYS = {"KB_Esc": "ESC", "KB_Alt": "LEFTALT", "KB_Ctrl": "LEFTCTRL", "KB_Shift": "LEFTSHIFT",
           "KB_Enter": "ENTER", "KB_Tab": "TAB", "KB_Space": "SPACE", "KB_Delete": "DELETE",
           "KB_Home": "HOME", "KB_End": "END", "KB_PageUp": "PAGEUP", "KB_PageDown": "PAGEDOWN",
           "KB_LeftArrow": "LEFT", "KB_RightArrow": "RIGHT", "KB_UpArrow": "UP",
           "KB_DownArrow": "DOWN"}
KB_KEYS.update({"KB_F%d" % i: "F%d" % i for i in range(1, 13)})
DRIVER_ACTIONS = {"Driver_ShowDriverGUI": "settings", "Driver_ToggleRotations": "rotation",
                  "Driver_ToggleTranslations": "panzoom", "Driver_ToggleDominantFilter": "dominant",
                  "Driver_IncreaseAppScale": "speed_up", "Driver_DecreaseAppScale": "speed_down",
                  "Driver_RezeroDevice": "calibrate"}
MAX_KEYS = 32  # tdxd's TDX_MAX_KEYS
# program names that on Linux are many programs (Creo and Cyclone run as java)
GENERIC_EXES = {"java", "javaw", "javaws", "python", "pythonw", "dllhost", "rundll32",
                "explorer", "desktop", "shellui", "applicationframehost"}
# configurations for Windows itself and 3DxWare's own tools: nothing on Linux
WINDOWS_ONLY = re.compile(r"(?i)^(3dx|appdefcfg|desktop|explorer|windows|taskmanager|"
                          r"controlpanel|searchandcortana|guisync|defnews|global|sharedcfg|"
                          r"ignoredapps|.*template)")


def _xid(e):
    return (e.findtext("ID") or "").strip()


def _text_chords(text):
    out = []
    for ch in text:
        if ch.isalpha() and ch.isascii():
            out.append(["LEFTSHIFT", ch.upper()] if ch.isupper() else [ch.upper()])
        elif ch.isdigit():
            out.append([ch])
        elif ch in _PLAIN:
            out.append([_PLAIN[ch]])
        elif ch in _SHIFTED:
            out.append(["LEFTSHIFT", _SHIFTED[ch]])
        else:
            return None  # not on a US keyboard
    return out


def _hid(code_text):
    try:
        return HID_KEYS.get(int(code_text.strip(), 16))
    except (ValueError, AttributeError):
        return None


def macro_chords(entry, macros, depth=0):
    """A 3DxWare MacroEntry -> list of chords (each a list of KEY_ names without
    the KEY_ prefix), or None if tdxd can't send it (mouse clicks, odd keys)."""
    if depth > 4:
        return None
    ks = entry.find("KeyStroke")
    if ks is not None:
        if ks.find("MouseButton") is not None:
            return None
        chord = []
        for m in ks.iter("Modifier"):
            name = MODIFIERS.get((m.text or "").strip().lower())
            if not name:
                return None
            chord.append(name)
        key = _hid(ks.findtext("Key") or "")
        if not key:
            return None
        return [chord + [key]]
    seq = entry.find("Sequence")
    if seq is None:
        return None
    chords, held, fresh = [], [], False
    for step in seq:
        tag = step.tag
        if tag == "KeyPress":
            k = _hid(step.text or "")
            if not k:
                return None
            if k not in held:
                held.append(k)
            fresh = True
        elif tag == "KeyRelease":
            k = _hid(step.text or "")
            if not k:
                return None
            if fresh and held:
                chords.append(list(held))
            fresh = False
            if k in held:
                held.remove(k)
        elif tag == "Text":
            t = _text_chords(step.text or "")
            if t is None:
                return None
            chords += [list(held) + c for c in t]
        elif tag == "ActionID":
            sub = macros.get((step.text or "").strip())
            subc = macro_chords(sub, macros, depth + 1) if sub is not None else None
            if subc is None:
                return None
            chords += subc
        elif tag == "Pause":
            continue  # tdxd sends the chords at once
        else:
            return None  # mouse buttons and moves
    if fresh and held:
        chords.append(list(held))
    return chords or None


def _string_table(xml_path):
    if not xml_path:
        return {}, {}
    root = ET.parse(xml_path).getroot()
    strings = {}
    for e in root.iter("StringEntry"):
        k, v = e.findtext("Key"), e.findtext("Value")
        if k and v is not None:
            strings[k.strip()] = v.strip()
    macros = {_xid(m): m for m in root.iter("MacroEntry") if _xid(m)}
    return strings, macros


def _match_names(root):
    """ExecutableName regexes (BLENDER\\.exe) -> Linux process names (blender)."""
    out = []
    for e in root.iter("ExecutableName"):
        n = (e.text or "").strip()
        n = re.sub(r"(?i)(\\\.|\.)exe\$?$", "", n).lstrip("^")
        n = n.replace("\\.", ".").replace(".*", "*").replace("\\ ", " ")
        if not n or re.search(r"[\\\[\]()|+?{}$^ ]", n):
            continue  # a regex tdxd's name globs can't express, or a name with spaces
        n = n.lower()
        if len(n.replace("*", "")) < 3 or n in GENERIC_EXES:
            continue  # would catch other programs on Linux
        if n not in out:
            out.append(n)
    return out


def _profile_name(filename):
    n = re.sub(r"[^a-z0-9]+", "-", os.path.splitext(filename)[0].lower()).strip("-")
    return "3dx-" + n if n else None


def app_profiles(by_name):
    """Every application configuration in the installer -> {profile name:
    (text of a tdxd profile, keys converted)}. What can't be done on Linux (the
    application's own NavLib commands, 3DxWare's legacy SDK commands, menus,
    mouse macros) is left out, so those keys keep tdxd's defaults."""
    strings, shared_macros = _string_table(by_name.get("en-US.xml"))
    roots, by_cfg_id = {}, {}
    for name, path in by_name.items():
        if not name.lower().endswith(".xml") or name in ("Base.xml",) or name.endswith("-DE.xml"):
            continue
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError:
            continue
        if root.tag != "AppCfg":
            continue
        roots[name] = root
        cid = (root.findtext("CfgProperties/ID") or "").strip()
        if cid:
            by_cfg_id[cid] = root

    def chain(root):
        seen = []
        while root is not None and root not in seen:
            seen.append(root)
            parent = (root.findtext("CfgProperties/InheritsFromID") or "").strip()
            root = by_cfg_id.get(parent)
        return seen

    def label(text):
        text = strings.get(text, text) or ""
        return re.sub(r"\s+", " ", text).strip()[:23]

    results = {}
    for fname, root in sorted(roots.items(), key=lambda kv: kv[0].lower()):
        match = _match_names(root)
        pname = _profile_name(fname)
        if not match or not pname or WINDOWS_ONLY.match(fname):
            continue
        cfgs = chain(root)
        macros, names = dict(shared_macros), {}
        for r in reversed(cfgs):  # the file's own win over its parents'
            for m in r.iter("MacroEntry"):
                if _xid(m):
                    macros[_xid(m)] = m
            for a in r.iter("ButtonAction"):
                if _xid(a) and a.findtext("Name"):
                    names[_xid(a)] = a.findtext("Name").strip()
        bank = None
        for dev in root.iter("Device"):
            if dev.findtext("ID") != "ID_Standard_3D_Mouse":
                continue
            banks = dev.findall("ButtonBank")
            bank = next((b for b in banks if b.get("Default") == "true"), None) or \
                next((b for b in banks if b.findtext("ID") == "Default"), None) or \
                (banks[0] if banks else None)
        if bank is None:
            continue
        out = {}
        for b in bank.findall("Button"):
            key, aid = b.findtext("Input/ActionID"), b.findtext("Output/ActionID")
            if key and aid and key.startswith("V3DK_"):
                out[key.strip()] = aid.strip()

        lines, done = [], 0

        def convert(key, aid, depth=0):
            if aid.startswith("V3DK_") and depth < 3:  # "does what key X does here"
                return convert(key, out.get(aid, ""), depth + 1) if out.get(aid) else None
            if aid in DRIVER_ACTIONS:
                return ["action_%s = %s" % (key, DRIVER_ACTIONS[aid])]
            if aid in ("Driver_Disabled", "Driver_DevNull"):
                return ["key_%s = none" % key, "action_%s = none" % key]
            if aid in KB_KEYS:
                return ["key_%s = KEY_%s" % (key, KB_KEYS[aid]), "action_%s = none" % key]
            if aid.startswith("App_"):  # the application's own function: its button only
                return ["key_%s = none" % key, "action_%s = none" % key]
            m = macros.get(aid)
            if m is not None:
                chords = macro_chords(m, macros)
                if not chords or sum(len(c) for c in chords) > MAX_KEYS:
                    return None
                text = ", ".join(" + ".join("KEY_" + k for k in c) for c in chords)
                return ["key_%s = %s" % (key, text), "action_%s = none" % key]
            return None  # NavLib / SDK commands, menus: not on Linux

        for key in sorted(out):
            got = convert(key, out[key])
            if not got:
                continue
            aid = out[key]
            name = label(names.get(aid, "")) if not aid.startswith(("Driver_", "KB_")) else ""
            if name:
                got.append("lcd_label_%s = %s" % (key, name))
            lines += got
            if any(g.startswith("key_") and not g.endswith("= none") for g in got):
                done += 1  # only 3DxWare's driver functions: tdxd's defaults already
        if not done:
            continue
        app = label((root.findtext("CfgProperties/Name") or "").strip()) or fname
        text = ("# %s: 3DxWare's key settings for %s, imported from your 3DxWare installer\n"
                "# (%s) by tdx-extract-3dxware-ui. Your own profiles in /etc/tdxd.d win.\n"
                "match = %s\n" % (pname, strings.get((root.findtext("CfgProperties/Name") or "").strip(), app),
                                  fname, ", ".join(match)))
        text += "\n".join(lines) + "\n"
        # two configurations for one program (FreeCAD.xml, FreeCAD_Navlib.xml):
        # keep the one with more keys
        dup = next((p for p, (t, n, mm) in results.items() if set(mm) & set(match)), None)
        if dup:
            if results[dup][1] >= done:
                continue
            del results[dup]
        results[pname] = (text, done, match)
    return {p: (t, n) for p, (t, n, _m) in results.items()}


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    installer = os.path.abspath(sys.argv[1])
    default_out = len(sys.argv) <= 2
    if not default_out:
        out = sys.argv[2]
    elif os.geteuid() == 0:
        out = "/usr/local/share/tdxd/3dxware-ui"
    else:
        out = os.path.expanduser("~/.local/share/tdxd/3dxware-ui")
    with tempfile.TemporaryDirectory(prefix="tdx-ui-") as t:
        print("unpacking", os.path.basename(installer))
        by_name = unpack_msi(installer, "3DxWinCore64.msi", t)
        if by_name is None:
            sys.exit("3DxWinCore64.msi not found in %s (is this a 3DxWare 10 installer?)" % installer)

        os.makedirs(os.path.join(out, "icons"), exist_ok=True)
        os.makedirs(os.path.join(out, "ui"), exist_ok=True)
        n = 0
        for name, path in sorted(by_name.items()):
            if any(re.match(p, name) for p in FILE_PATTERNS):
                shutil.copyfile(path, os.path.join(out, "icons", name))
                n += 1
        print("%d icons and device images" % n)

        smartui = by_name.get("3DxSmartUi.exe")
        if not smartui:
            sys.exit("3DxSmartUi.exe not found in the installer")
        imgs = dotnet_images(smartui)
        for name, blob in imgs.items():
            with open(os.path.join(out, "ui", name + ".png"), "wb") as f:
                f.write(blob)
        print("%d control-panel images from 3DxSmartUi.exe" % len(imgs))
        # 3DxWare's pairing program: its device pictures (Device Selection),
        # pairing steps and add / remove icons; 3DxSmartUi's win on a name clash
        pair = by_name.get("3DxPair.exe")
        if pair:
            n = 0
            for name, blob in dotnet_images(pair).items():
                dst = os.path.join(out, "ui", name + ".png")
                if name not in imgs:
                    with open(dst, "wb") as f:
                        f.write(blob)
                    n += 1
            print("%d pairing images from 3DxPair.exe" % n)
        print("window icon sizes %s from 3DxSmartUi.exe"
              % app_icons(smartui, os.path.join(out, "appicons"), "3dxware"))

        apps = unpack_msi(installer, "3DxLcdApplets64.msi", t) or {}
        appdir = os.path.join(out, "lcdapplets")
        for exe, sub_ in (("3DxLCD10SPPRO.exe", "functionkeys"), ("zz3DxLCD50RSSReader.exe", "rss"),
                          ("zz3DxLCD60Picture.exe", "pictures")):
            if exe in apps:
                print("%d images from %s" % (applet_art(apps[exe], os.path.join(appdir, sub_)), exe))
        fdir = os.path.join(appdir, "finder")
        os.makedirs(fdir, exist_ok=True)
        for name, path in apps.items():
            if name.startswith(("client_list_", "vscroll_")) and name.endswith(".png"):
                w, h, rgba = png_read(open(path, "rb").read())
                save_both(os.path.join(fdir, name[:-4]), w, h, rgba)
            elif name == "FinderConfig.txt":
                shutil.copyfile(path, os.path.join(fdir, name))
            elif re.match(r"3Dx_Images_\d+\.jpg$", name):
                os.makedirs(os.path.join(appdir, "pictures", "default"), exist_ok=True)
                shutil.copyfile(path, os.path.join(appdir, "pictures", "default", name))
        ladp = unpack_msi(installer, "LADPInst.msi", t)
        lcdmon = (ladp or {}).get("LCDMon.exe")
        if lcdmon:
            print("%d LCD Manager images from LCDMon.exe"
                  % lcdmon_images(lcdmon, os.path.join(out, "lcdmon")))
            app_icons(lcdmon, os.path.join(out, "appicons"), "lcdmanager")
        else:
            print("LADPInst.msi / LCDMon.exe not found: LCD Manager artwork skipped")

        base = by_name.get("Base.xml")
        if base:
            with open(os.path.join(out, "devices.json"), "w") as f:
                json.dump(devices_json(base), f, indent=1, sort_keys=True)
        nav = by_name.get("FreeCAD_Navlib.xml")
        if nav:  # FreeCAD's per-workbench keys, for the tdxd FreeCAD add-on
            fc = freecad_buttons(nav)
            fc["source"] = os.path.basename(installer)
            os.makedirs(os.path.join(out, "freecad"), exist_ok=True)
            with open(os.path.join(out, "freecad", "buttons.json"), "w") as f:
                json.dump(fc, f, indent=1, sort_keys=True)
            print("FreeCAD keys: %d workbenches, %d commands, %d menus"
                  % (len(fc["banks"]), len(fc["actions"]), len(fc["menus"])))
        # every application's key settings, as tdxd profiles (tdxd reads them
        # after /etc/tdxd.d; see profile.h)
        prof = app_profiles(by_name)
        pdir = os.path.join(out, "profiles")
        os.makedirs(pdir, exist_ok=True)
        for old in os.listdir(pdir):
            if old.startswith("3dx-") and old.endswith(".conf"):
                os.remove(os.path.join(pdir, old))
        for pname, (text, _n) in prof.items():
            with open(os.path.join(pdir, pname + ".conf"), "w") as f:
                f.write(text)
        print("%d application profiles (%d keys) in %s"
              % (len(prof), sum(n for _t, n in prof.values()), pdir))
        with open(os.path.join(out, "SOURCE"), "w") as f:
            f.write("Extracted from %s for local use by tdx-settings.\n"
                    "Artwork (c) 3Dconnexion; do not redistribute.\n" % os.path.basename(installer))
    if default_out:  # the taskbar icons for the desktop's icon theme
        if os.geteuid() == 0:
            theme, apps = "/usr/local/share/icons", "/usr/local/share/applications"
        else:
            theme = os.path.expanduser("~/.local/share/icons")
            apps = os.path.expanduser("~/.local/share/applications")
        print("%d taskbar icons in %s" % (install_theme_icons(out, theme, apps),
                                          theme))
    print("wrote", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
