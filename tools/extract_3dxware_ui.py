#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Extract the 3DxWare control-panel artwork from YOUR copy of the installer.

tdx-settings looks like the 3DxWare 10 panel when these files are present.
The artwork belongs to 3Dconnexion and is not shipped with tdxd; this tool
copies it out of the installer you downloaded, for use on your own machine.

    tools/extract_3dxware_ui.py 3DxWare64_v10-9-14_b745.exe [OUTDIR]

OUTDIR defaults to ~/.local/share/tdxd/3dxware-ui (or, as root,
/usr/local/share/tdxd/3dxware-ui). Needs 7z (p7zip-full / p7zip-plugins).

What it does, without running any installer code:
  installer.exe --7z--> 3DxWinCore64.msi --7z--> Data1.cab + MSI tables
  !File table (via _StringPool/_StringData) maps cab keys to file names
  3DxSmartUi.exe: .NET .resources blobs are parsed for the named PNGs
  Base.xml: device image and support status per USB product id
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
    # the executable's icon = first icon group (lowest id), its 32x32 entry
    if groups:
        groups.sort(key=lambda g: (isinstance(g[0], str), g[0]))
        gdata = groups[0][1]
        cnt, = struct.unpack_from("<H", gdata, 4)
        best = None
        for i in range(cnt):
            w, h, _c, _r, _p, bpp, _sz, iid = struct.unpack_from("<BBBBHHIH", gdata, 6 + i * 14)
            score = (w or 256) == 32, bpp
            if best is None or score > best[0]:
                best = (score, iid)
        d = icons.get(best[1]) if best else None
        if d:
            img = png_read(d) if d[:8] == b"\x89PNG\r\n\x1a\n" else dib_read(d, icon=True)
            save_both(os.path.join(outdir, "icon"), *img)
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


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    installer = os.path.abspath(sys.argv[1])
    if len(sys.argv) > 2:
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
        else:
            print("LADPInst.msi / LCDMon.exe not found: LCD Manager artwork skipped")

        base = by_name.get("Base.xml")
        if base:
            with open(os.path.join(out, "devices.json"), "w") as f:
                json.dump(devices_json(base), f, indent=1, sort_keys=True)
        with open(os.path.join(out, "SOURCE"), "w") as f:
            f.write("Extracted from %s for local use by tdx-settings.\n"
                    "Artwork (c) 3Dconnexion; do not redistribute.\n" % os.path.basename(installer))
    print("wrote", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
