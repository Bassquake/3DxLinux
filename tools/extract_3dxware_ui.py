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


def msi_files(d):
    """{cab key: long file name} from the column-major !File table."""
    strings, sref = msi_strings(d)
    raw = open(os.path.join(d, "!File"), "rb").read()
    # File, Component_, FileName, FileSize(i4), Version, Language, Attributes(i2), Sequence(i4)
    widths = [sref, sref, sref, 4, sref, sref, 2, 4]
    rows = len(raw) // sum(widths)
    cols, off = [], 0
    for w in widths:
        col = []
        for r in range(rows):
            b = raw[off + r * w: off + (r + 1) * w]
            col.append(int.from_bytes(b, "little"))
        cols.append(col)
        off += rows * w
    out = {}
    for r in range(rows):
        key, name = strings[cols[0][r]], strings[cols[2][r]]
        out[key] = name.split("|")[-1]
    return out


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
        run7z("x", "-y", "-o" + t, installer, "3DxWinCore64.msi")
        msi = os.path.join(t, "3DxWinCore64.msi")
        if not os.path.exists(msi):
            sys.exit("3DxWinCore64.msi not found in %s (is this a 3DxWare 10 installer?)" % installer)
        md = os.path.join(t, "msi")
        run7z("x", "-y", "-o" + md, msi)
        cabs = [f for f in os.listdir(md) if f.lower().endswith(".cab")]
        cd = os.path.join(t, "cab")
        for c in cabs:
            run7z("x", "-y", "-o" + cd, os.path.join(md, c))
        names = msi_files(md)
        by_name = {}
        for key, name in names.items():
            path = os.path.join(cd, key)
            if os.path.isfile(path):
                by_name.setdefault(name, path)

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
