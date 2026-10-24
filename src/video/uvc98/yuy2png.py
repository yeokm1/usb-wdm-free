#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""Convert one raw YUY2 frame to PNG for viewing (host-side file tool).

  python -B yuy2png.py WIDTH HEIGHT IN.YUY OUT.PNG

BT.601 limited-range YUV to RGB, standard library only. Reads and writes files.
"""
import struct, sys, zlib


def clamp(v):
    return 0 if v < 0 else 255 if v > 255 else int(v)


def main():
    w, h, src, dst = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
    data = open(src, 'rb').read()
    if len(data) != w * h * 2:
        raise SystemExit('size %d is not %dx%d YUY2' % (len(data), w, h))
    rows = []
    for y in range(h):
        row = bytearray([0])
        base = y * w * 2
        for x in range(0, w, 2):
            y0, u, y1, v = data[base + x * 2: base + x * 2 + 4]
            for yy in (y0, y1):
                c, d, e = 1.164 * (yy - 16), u - 128, v - 128
                row += bytes((clamp(c + 1.596 * e), clamp(c - 0.392 * d - 0.813 * e), clamp(c + 2.017 * d)))
        rows.append(bytes(row))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)


if __name__ == '__main__':
    main()
