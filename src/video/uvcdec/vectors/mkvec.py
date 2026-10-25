# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Makes the uvcdec host-test vectors (file-only; run with python -I).

  mkvec.py pattern W H NOISE OUT.RGB   synthetic RGB24 frame, top-down
  mkvec.py strip IN.JPG OUT.JPG        copy of a JPEG with every DHT removed

The pattern is the one jdtest.c recomputes to measure PSNR, so keep the two
in step. The JPEGs are encoded from it by FFmpeg (see README.md, "Test
vectors"); FFmpeg is an encoder independent of the libjpeg-turbo decoder under test.
No camera capture is used.
"""
import sys

BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]


def pattern(w, h, noise):
    out = bytearray(w * h * 3)
    seed = 1
    i = 0
    for y in range(h):
        for x in range(w):
            if y < h // 2:
                r, g, b = BARS[(x * 8) // w]
            else:
                r = (x * 255) // (w - 1)
                g = (y * 255) // (h - 1)
                b = 255 - r
            px = [r, g, b]
            if noise:
                for c in range(3):
                    seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
                    v = px[c] + ((seed >> 16) & 31) - 16
                    px[c] = 0 if v < 0 else (255 if v > 255 else v)
            out[i:i + 3] = bytes(px)
            i += 3
    return out


def strip(data):
    """Drops DHT (FFC4) segments before SOS; the scan is copied unchanged."""
    if data[:2] != b"\xff\xd8":
        raise ValueError("no SOI")
    out = bytearray(data[:2])
    p = 2
    removed = 0
    while p + 4 <= len(data):
        if data[p] != 0xFF:
            raise ValueError("marker expected at %d" % p)
        m = data[p + 1]
        seglen = (data[p + 2] << 8) | data[p + 3]
        if m == 0xDA:
            out += data[p:]
            break
        if m == 0xC4:
            removed += 1
        else:
            out += data[p:p + 2 + seglen]
        p += 2 + seglen
    if removed == 0:
        raise ValueError("no DHT found")
    return bytes(out)


def main(argv):
    if len(argv) == 6 and argv[1] == "pattern":
        w, h, n = int(argv[2]), int(argv[3]), int(argv[4])
        with open(argv[5], "wb") as f:
            f.write(pattern(w, h, n))
        return 0
    if len(argv) == 4 and argv[1] == "strip":
        with open(argv[2], "rb") as f:
            data = f.read()
        with open(argv[3], "wb") as f:
            f.write(strip(data))
        return 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
