#!/usr/bin/env python3
"""Independent check of tdxd's raw-deflate encoder against Python's zlib.

test_core writes pairs NAME.raw / NAME.def into a directory; every .def must
inflate (raw deflate, wbits=-15) back to the matching .raw, and must consist of
a single final fixed-Huffman block (BFINAL=1, BTYPE=01) like the Windows driver.
"""
import os
import sys
import zlib


def main(d):
    names = sorted(f[:-4] for f in os.listdir(d) if f.endswith(".raw"))
    if not names:
        sys.exit("no vectors in " + d)
    bad = 0
    for n in names:
        raw = open(os.path.join(d, n + ".raw"), "rb").read()
        comp = open(os.path.join(d, n + ".def"), "rb").read()
        try:
            out = zlib.decompress(comp, -15)
        except zlib.error as e:
            print(f"FAIL {n}: {e}")
            bad += 1
            continue
        hdr = comp[0] & 7 if comp else -1
        ok = out == raw and hdr == 0b011  # BFINAL=1, BTYPE=01 (LSB first)
        ratio = len(comp) / max(1, len(raw))
        print(f"{'ok  ' if ok else 'FAIL'} {n}: {len(raw)} -> {len(comp)} bytes ({ratio:.1%})")
        bad += not ok
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main(sys.argv[1])
