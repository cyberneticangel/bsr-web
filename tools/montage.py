#!/usr/bin/env python3
"""Tile harness PNG screenshots (8-bit RGB, filter 0) into one half-size contact sheet."""
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import bsrfmt  # noqa: E402


def read_png(path):
    d = open(path, 'rb').read()
    o, idat, w, h = 8, b'', 0, 0
    while o < len(d):
        n, t = struct.unpack('>I4s', d[o:o + 8])
        if t == b'IHDR':
            w, h = struct.unpack('>II', d[o + 8:o + 16])
        elif t == b'IDAT':
            idat += d[o + 8:o + 8 + n]
        o += 12 + n
    raw = zlib.decompress(idat)
    return w, h, [raw[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)] for y in range(h)]


def main(out, cols, paths):
    imgs = [read_png(p) for p in paths]
    w, h = imgs[0][0] // 2, imgs[0][1] // 2
    rows = (len(imgs) + cols - 1) // cols
    sheet = bytearray(w * cols * h * rows * 4)
    for k, (iw, ih, px) in enumerate(imgs):
        ox, oy = (k % cols) * w, (k // cols) * h
        for y in range(h):
            src = px[y * 2]
            for x in range(w):
                p = ((oy + y) * w * cols + ox + x) * 4
                sheet[p:p + 4] = src[x * 6:x * 6 + 3] + b'\xff'
    bsrfmt.write_png(out, w * cols, h * rows, bytes(sheet))


if __name__ == '__main__':
    main(sys.argv[1], int(sys.argv[2]), sys.argv[3:])
