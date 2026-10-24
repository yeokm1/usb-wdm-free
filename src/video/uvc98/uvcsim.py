#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""SIMULATED isochronous UVC camera over localhost usbredir (test aid only).

Original model from USB-IF UVC 1.1 (class-specific descriptors, probe and
commit, payload headers), the UVC 1.1 uncompressed and MJPEG payload
documents, USB 2.0 chapter 9, ITU-T T.81 (baseline JPEG, Annex K tables)
and SPICE usb-redirection-protocol v0.7. No physical camera, microphone,
file recording, kernel driver or image decoder is accessed.

The camera: one IAD function (video control interface 0 with a status
interrupt endpoint, video streaming interface 1), YUY2 and MJPEG at
160x120, 320x240 and 640x480, isochronous alternates for Full Speed (one
1023-byte maximum) and High Speed (single-transaction up to 1024 bytes,
then two- and three-transaction alternates). At High Speed every size to
320x240 at 30 fps fits a single-transaction alternate; YUY2 640x480 at
30 fps needs a three-transaction one (lower rates do not).

UVC 1.5 (--uvc 1.5, from the UVC 1.5 class specification and its
uncompressed, frame-based and H.264 payload documents): bcdUVC 0x0150, the
48-byte probe/commit (bUsage 1, bBitDepthLuma 8), a 13-byte processing unit
(bControlSize 3) and an encoding unit between it and the output terminal,
and two more formats: NV12 at the same sizes with continuous frame
intervals, and H.264 (--h264: frame-based with the H264 GUID, the 1.5
VS_FORMAT_H264 family, both, or none) carrying SIMULATED access units, not
decodable pictures. --uvc 1.0 gives the 26-byte probe and 1.0 descriptors.

IDs default to 046D:094C so the per-camera uvc98 INF lines bind it, as
uacemu.py presents the X4's IDs; every string says SIMULATED, the serial
index is zero, and the descriptors are this model's own (not the Brio
100's). --vid/--pid change them.
"""
import argparse
import hashlib
import json
import select
import socket
import struct
import time
from pathlib import Path

# usbredir message types (usb-redirection-protocol.txt v0.7).
HELLO, CONNECT, DISCONNECT, RESET, IFINFO, EPINFO = 0, 1, 2, 3, 4, 5
SET_CONFIG, GET_CONFIG, CONFIG_STATUS = 6, 7, 8
SET_ALT, GET_ALT, ALT_STATUS = 9, 10, 11
START_ISO, STOP_ISO, ISO_STATUS = 12, 13, 14
START_INT, STOP_INT, INT_STATUS = 15, 16, 17
CANCEL, DISCONNECT_ACK = 21, 24
CONTROL, BULK, ISO, INTERRUPT = 100, 101, 102, 103
OK, STALL = 0, 4
CAP_VERSION, CAP_DISC_ACK, CAP_EP_MAX, CAP_ID64, CAP_BULK32 = 1, 3, 4, 5, 6
# QEMU's usb-redir refuses a device on a SuperSpeed-capable (qemu-xhci) port
# unless the peer has ep_info_max_packet_size, 32bits_bulk_length and
# 64bits_ids. This model has no bulk endpoint, so 32-bit bulk lengths are
# only advertised; 64-bit ids widen every header once both hellos agree.
CAPS = (1 << CAP_VERSION) | (1 << CAP_DISC_ACK) | (1 << CAP_EP_MAX) | (1 << CAP_ID64) | (1 << CAP_BULK32)

YUY2_GUID = bytes.fromhex('5955593200001000800000aa00389b71')
NV12_GUID = bytes.fromhex('4e56313200001000800000aa00389b71')
H264_GUID = bytes.fromhex('4832363400001000800000aa00389b71')
SIZES = ((160, 120), (320, 240), (640, 480))
CLOCK = 48000000          # dwClockFrequency: PTS/SCR units
HEADER = 12               # payload header with PTS and SCR
VS_EP, VC_EP = 0x81, 0x87
# Alternates 1..n: (packet bytes, transactions); bInterval 1 throughout.
ALTS = {'high': ((192, 1), (384, 1), (640, 1), (800, 1), (1024, 1),
                 (1024, 2), (800, 3), (1024, 3)),
        'full': ((128, 1), (256, 1), (512, 1), (800, 1), (1023, 1))}
SLOTS = {'high': 8000, 'full': 1000}       # (micro)frames per second
RATES = (30, 20, 15, 10, 5, 1)
# BT.601 studio-range bars: white yellow cyan green magenta red blue black.
BARS = ((235, 128, 128), (210, 16, 146), (170, 166, 16), (145, 54, 34),
        (106, 202, 222), (81, 90, 240), (41, 240, 110), (16, 128, 128))
WHITE, BLACK, GREY = BARS[0], BARS[7], (128, 128, 128)


def usb_string(text):
    raw = text.encode('utf-16le')
    return bytes((len(raw) + 2, 3)) + raw


def interval(fps):
    return 10000000 // fps


def need(frame_bytes, fps, speed, aligned=True):
    """Payload bytes per (micro)frame that carry one frame in its interval,
    leaving two slots of slack; YUY2 data stays on four-byte macropixels."""
    slots = SLOTS[speed] // fps - 2
    data = -(-frame_bytes // slots)
    if aligned:
        data = (data + 3) & ~3
    return data + HEADER


def capacity(alt):
    return alt[0] * alt[1]


# ---------------------------------------------------------------------------
# Pictures: 16-pixel colour bars moving one bar per frame, with the frame's
# sequence number as 32 black/white 16-pixel cells across the top eight rows.

def cell(x16, row, seq):
    if row < 8:
        return GREY if x16 >= 32 else (WHITE if seq >> x16 & 1 else BLACK)
    return BARS[(x16 + seq) % 8]


def yuy2_frame(width, height, seq):
    def line(row):
        out = bytearray()
        for x16 in range(width // 16):
            y, u, v = cell(x16, row, seq)
            out += bytes((y, u, y, v)) * 8
        return bytes(out)
    return line(0) * 8 + line(8) * (height - 8)


def nv12_frame(width, height, seq):
    """The same picture as yuy2_frame, as NV12: the Y plane, then one
    interleaved U/V pair per 2x2 block (UVC 1.5 uncompressed payload)."""
    def luma(row):
        return b''.join(bytes((cell(x16, row, seq)[0],)) * 16 for x16 in range(width // 16))

    def chroma(row):
        return b''.join(bytes(cell(x16, row, seq)[1:]) * 8 for x16 in range(width // 16))
    return luma(0) * 8 + luma(8) * (height - 8) + chroma(0) * 4 + chroma(8) * (height // 2 - 4)


H264_FILLER = 64     # picture pixels per filler byte


def h264_unit(width, height, seq):
    """A SIMULATED H.264 access unit for the frame-based and H.264 formats:
    an access unit delimiter, a user-data-unregistered SEI that carries the
    frame number as hex text, and filler data. Not a decodable picture:
    uvc98 does not stream these formats; the bytes only fill frames with
    known content."""
    text = b'usb-wdm-free SIM' + b'%08X SIMULATED' % (seq & 0xffffffff)
    sei = b'\x00\x00\x00\x01\x06\x05' + bytes((len(text),)) + text + b'\x80'
    filler = b'\x00\x00\x00\x01\x0c' + b'\xff' * (width * height // H264_FILLER + seq % 16) + b'\x80'
    return b'\x00\x00\x00\x01\x09\xf0' + sei + filler


def h264_max(width, height):
    return len(h264_unit(width, height, 15))


class BitWriter:
    def __init__(self):
        self.out, self.acc, self.bits = bytearray(), 0, 0

    def put(self, code, length):
        self.acc = (self.acc << length) | code
        self.bits += length
        while self.bits >= 8:
            self.bits -= 8
            byte = (self.acc >> self.bits) & 255
            self.out.append(byte)
            if byte == 255:
                self.out.append(0)
        self.acc &= (1 << self.bits) - 1

    def flush(self):
        if self.bits:
            self.put((1 << (8 - self.bits)) - 1, 8 - self.bits)
        return bytes(self.out)


def huffman(bits, values):
    """Canonical codes (T.81 Annex C) as {symbol: (code, length)}."""
    table, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            table[values[k]] = (code, length)
            code, k = code + 1, k + 1
        code <<= 1
    return table


def padded(bits):
    return list(bits) + [0] * (16 - len(bits))


# Annex K.3 DC tables, and prefixes of the K.5 AC tables that hold the
# standard EOB code (1010 luminance, 00 chrominance): DC-only blocks encode
# the same bits with or without a DHT segment.
DC_BITS = (padded((0, 1, 5, 1, 1, 1, 1, 1, 1)), padded((0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1)))
DC_VALS = (list(range(12)), list(range(12)))
AC_BITS = (padded((0, 2, 1, 3)), padded((0, 2, 1, 2)))
AC_VALS = ([0x01, 0x02, 0x03, 0x00, 0x04, 0x11], [0x00, 0x01, 0x02, 0x03, 0x11])
DC_CODES = [huffman(DC_BITS[i], DC_VALS[i]) for i in (0, 1)]
EOB = [huffman(AC_BITS[i], AC_VALS[i])[0] for i in (0, 1)]


class Jpeg:
    """Baseline 4:2:2 DC-only JPEG, one restart interval per MCU row."""

    def __init__(self, width, height, dht=True):
        self.width, self.height, self.dht = width, height, dht
        self.columns, self.rows = width // 16, height // 8
        self.cache = {}
        head = bytearray(b'\xff\xd8')
        for table in (0, 1):  # DC quantizer 8: a flat block's DC is Y-128
            head += b'\xff\xdb\x00\x43' + bytes((table, 8)) + bytes([16]) * 63
        head += b'\xff\xc0\x00\x11\x08' + struct.pack('>HH', height, width) + \
            bytes((3, 1, 0x21, 0, 2, 0x11, 1, 3, 0x11, 1))
        if dht:
            for cls, tables in ((0, (DC_BITS, DC_VALS)), (1, (AC_BITS, AC_VALS))):
                for t in (0, 1):
                    bits, vals = tables[0][t], tables[1][t]
                    body = bytes((cls << 4 | t,)) + bytes(bits) + bytes(vals)
                    head += b'\xff\xc4' + struct.pack('>H', len(body) + 2) + body
        head += b'\xff\xdd\x00\x04' + struct.pack('>H', self.columns)
        head += b'\xff\xda\x00\x0c\x03\x01\x00\x02\x11\x03\x11\x00\x3f\x00'
        self.head = bytes(head)

    def segment(self, row, seq):
        w, pred = BitWriter(), [0, 0, 0]
        for x16 in range(self.columns):
            y, u, v = cell(x16, row, seq)
            for comp, value in ((0, y), (0, y), (1, u), (2, v)):
                table = 0 if comp == 0 else 1
                dc = value - 128
                diff, pred[comp] = dc - pred[comp], dc
                size = abs(diff).bit_length()
                code, length = DC_CODES[table][size]
                w.put(code, length)
                if size:
                    w.put(diff if diff > 0 else diff + (1 << size) - 1, size)
                w.put(*EOB[table])
        return w.flush()

    def frame(self, seq):
        key = seq % 8
        bars = self.cache.get(key)
        if bars is None:
            bars = self.cache[key] = self.segment(8, seq)
        out = [self.head, self.segment(0, seq)]
        for row in range(1, self.rows):
            out.append(bytes((0xff, 0xd0 + (row - 1) % 8)))
            out.append(bars)
        out.append(b'\xff\xd9')
        return b''.join(out)


# ---------------------------------------------------------------------------
# Descriptors.

class Camera:
    def __init__(self, speed='high', uvc=0x110, vid=0x046d, pid=0x094c, fps=None, dht=True,
                 h264='frame'):
        self.speed, self.uvc, self.vid, self.pid = speed, uvc, vid, pid
        self.probe_size = {0x100: 26, 0x110: 34, 0x150: 48}[uvc]
        # Formats by index. UVC 1.0/1.1: YUY2 and MJPEG. UVC 1.5 adds NV12
        # with continuous frame intervals, and an H.264 format a YUY2 host
        # must skip: frame-based (VS_FORMAT_FRAME_BASED with the H264 GUID,
        # h264='frame'), the UVC 1.5 VS_FORMAT_H264 family (h264='native'),
        # both (h264='both') or none (h264='none').
        if h264 not in ('frame', 'native', 'both', 'none'):
            raise ValueError('h264')
        self.kinds = {1: 'yuy2', 2: 'mjpeg'}
        if uvc == 0x150:
            self.kinds[3] = 'nv12'
            if h264 in ('frame', 'both'):
                self.kinds[len(self.kinds) + 1] = 'h264'
            if h264 in ('native', 'both'):
                self.kinds[len(self.kinds) + 1] = 'h264n'
        self.h264 = h264
        self.formats = {}   # (format, frame) -> dict
        self.jpeg = {}
        for speed_name in ('high', 'full'):
            self.formats[speed_name] = self.frames(speed_name, fps)
        for w, h in SIZES:
            self.jpeg[(w, h)] = Jpeg(w, h, dht)
        self.mjpeg_max = {s: max(len(self.jpeg[s].frame(q)) for q in (0, 1, 255, 0xffffffff))
                          for s in SIZES}
        self.device = struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x200, 0xef, 2, 1, 64,
                                  vid, pid, 0x0001, 1, 2, 0, 1)
        self.qualifier = struct.pack('<BBHBBBBBB', 10, 6, 0x200, 0xef, 2, 1, 64, 1, 0)
        self.strings = {0: b'\4\3\x09\4', 1: usb_string('usb-wdm-free SIMULATED'),
                        2: usb_string('UVC camera - SIMULATED')}
        self.config = {s: self.configuration(s) for s in ('high', 'full')}

    def frames(self, speed, fps):
        table = {}
        for fmt, kind in self.kinds.items():
            for index, (w, h) in enumerate(SIZES, 1):
                fb = w * h * 3 // 2 if kind == 'nv12' else w * h * 2
                if kind in ('yuy2', 'nv12'):
                    limit = capacity(ALTS[speed][-1]) if speed == 'high' else 1023
                    rates = [r for r in RATES if need(fb, r, speed) <= limit]
                else:
                    rates = [30, 15, 10, 5]
                default = fps if fps in rates else rates[0]
                table[(fmt, index)] = dict(width=w, height=h, kind=kind, frame_bytes=fb,
                                           rates=rates, default=default,
                                           continuous=kind == 'nv12')
        return table

    def uncompressed(self, fmt):
        return self.kinds[fmt] in ('yuy2', 'nv12')

    def frame_bytes(self, fmt, frame):
        f = self.formats[self.speed][(fmt, frame)]
        if self.uncompressed(fmt):
            return f['frame_bytes']
        if self.kinds[fmt] == 'mjpeg':
            return self.mjpeg_max[(f['width'], f['height'])]
        return h264_max(f['width'], f['height'])

    def frame_descriptor(self, index, f):
        kind, fb, rates = f['kind'], f['frame_bytes'], f['rates']
        if f['continuous']:
            # bFrameIntervalType 0: dwMinFrameInterval, dwMaxFrameInterval,
            # dwFrameIntervalStep (1: every interval in between).
            table = bytes((0,)) + struct.pack('<III', interval(rates[0]), interval(rates[-1]), 1)
        else:
            table = bytes((len(rates),)) + b''.join(struct.pack('<I', interval(r)) for r in rates)
        bits = (fb * 8 * rates[-1], fb * 8 * rates[0])
        if kind in ('yuy2', 'nv12', 'mjpeg'):
            # VS_FRAME_UNCOMPRESSED / VS_FRAME_MJPEG: 26 + 4n (38 continuous).
            body = bytes((5 if kind != 'mjpeg' else 7, index, 0)) + \
                struct.pack('<HHIIII', f['width'], f['height'], *bits, fb, interval(f['default']))
            body += table
        elif kind == 'h264':
            # VS_FRAME_FRAME_BASED (Frame Based payload 1.5, section 3.1.2):
            # bmCapabilities, size, bit rates, default interval,
            # bFrameIntervalType, dwBytesPerLine (0), intervals: 26 + 4n.
            body = bytes((0x11, index, 0)) + \
                struct.pack('<HHIII', f['width'], f['height'], *bits, interval(f['default']))
            body += table[:1] + struct.pack('<I', 0) + table[1:]
        else:
            # VS_FRAME_H264 (H.264 payload 1.5, section 3.1.2): wSARwidth/
            # height 1:1, wProfile 0x4240 (constrained baseline), level 31,
            # bmSupportedUsages D0 + D1, then bit rates, default interval and
            # the discrete intervals: 44 + 4n.
            body = bytes((0x14, index)) + \
                struct.pack('<HHHHHBH', f['width'], f['height'], 1, 1, 0x4240, 31, 0) + \
                struct.pack('<IHII', 0x00000003, 0, 0, 0) + \
                struct.pack('<III', *bits, interval(f['default'])) + table
        return bytes((2 + len(body), 0x24)) + body

    def format_descriptor(self, fmt, count):
        kind = self.kinds[fmt]
        if kind == 'yuy2':
            return bytes((27, 0x24, 4, fmt, count)) + YUY2_GUID + bytes((16, 1, 0, 0, 0, 0))
        if kind == 'nv12':
            return bytes((27, 0x24, 4, fmt, count)) + NV12_GUID + bytes((12, 1, 0, 0, 0, 0))
        if kind == 'mjpeg':
            return bytes((11, 0x24, 6, fmt, count, 1, 1, 0, 0, 0, 0))
        if kind == 'h264':
            # VS_FORMAT_FRAME_BASED (Frame Based payload 1.5, section 3.1.1),
            # 28 bytes; bVariableSize 1.
            return bytes((28, 0x24, 0x10, fmt, count)) + H264_GUID + bytes((16, 1, 0, 0, 0, 0, 1))
        # VS_FORMAT_H264 (H.264 payload 1.5, section 3.1.1), 52 bytes: default
        # frame 1, no codec configuration delay, no slice modes, sync frame
        # type D0 (reset), no resolution scaling, one rate-control mode
        # (D0: VBR), then the twenty wMaxMBperSec words.
        return bytes((52, 0x24, 0x13, fmt, count, 1, 0, 0, 1, 0, 0, 1)) + \
            struct.pack('<20H', *([1200] + [0] * 19))

    def configuration(self, speed):
        uvc = self.uvc
        # Camera terminal 1 -> processing unit 2 (brightness only) -> output 3;
        # UVC 1.5: camera terminal 1 -> processing unit 2 -> encoding unit 4
        # (no controls) -> output 3.
        vc = bytes((18, 0x24, 2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0))
        if uvc == 0x150:
            # UVC 1.5 Table 3-8: bControlSize 3, 13 bytes.
            pu = bytes((2, 1)) + struct.pack('<H', 0) + bytes((3, 1, 0, 0, 0, 0))
        else:
            pu = bytes((2, 1)) + struct.pack('<H', 0) + bytes((2,)) + struct.pack('<H', 1) + b'\0'
            if uvc != 0x100:
                pu += b'\0'                # UVC 1.1 bmVideoStandards
        vc += bytes((len(pu) + 3, 0x24, 5)) + pu
        if uvc == 0x150:
            # UVC 1.5 Table 3-9: VC_ENCODING_UNIT, 13 bytes, bControlSize 3,
            # bmControls and bmControlsRuntime all zero.
            vc += bytes((13, 0x24, 7, 4, 2, 0, 3, 0, 0, 0, 0, 0, 0))
            vc += bytes((9, 0x24, 3, 3, 1, 1, 0, 4, 0))
        else:
            vc += bytes((9, 0x24, 3, 3, 1, 1, 0, 2, 0))
        header = bytes((13, 0x24, 1)) + struct.pack('<HHI', uvc, 13 + len(vc), CLOCK) + b'\1\1'
        vc = header + vc
        vs = b''
        frames = self.formats[speed]
        for fmt in self.kinds:
            vs += self.format_descriptor(fmt, 3)
            for index in (1, 2, 3):
                vs += self.frame_descriptor(index, frames[(fmt, index)])
            vs += bytes((6, 0x24, 13, 1, 1, 4))
        n = len(self.kinds)
        vs = bytes((13 + n, 0x24, 1, n)) + struct.pack('<H', 13 + n + len(vs)) + \
            bytes((VS_EP, 0, 3, 0, 0, 0, 1)) + bytes(n) + vs
        body = bytes((8, 11, 0, 2, 14, 3, 0, 0))
        body += bytes((9, 4, 0, 0, 1, 14, 1, 0, 0)) + vc
        body += bytes((7, 5, VC_EP, 3, 16, 0, 8 if speed == 'high' else 16)) + bytes((5, 0x25, 3, 16, 0))
        body += bytes((9, 4, 1, 0, 0, 14, 2, 0, 0)) + vs
        for n, (size, mult) in enumerate(ALTS[speed], 1):
            body += bytes((9, 4, 1, n, 1, 14, 2, 0, 0))
            body += bytes((7, 5, VS_EP, 5)) + struct.pack('<H', size | (mult - 1) << 11) + b'\1'
        return bytes((9, 2)) + struct.pack('<H', len(body) + 9) + bytes((2, 1, 0, 0x80, 250)) + body

    def picture(self, fmt, frame, seq):
        f = self.formats[self.speed][(fmt, frame)]
        kind = self.kinds[fmt]
        if kind == 'yuy2':
            return yuy2_frame(f['width'], f['height'], seq)
        if kind == 'nv12':
            return nv12_frame(f['width'], f['height'], seq)
        if kind == 'mjpeg':
            return self.jpeg[(f['width'], f['height'])].frame(seq)
        return h264_unit(f['width'], f['height'], seq)

    def answer(self, fmt, frame, iv):
        """Device-adjusted probe values for a format, frame and interval."""
        f = self.formats[self.speed][(fmt, frame)]
        intervals = [interval(r) for r in f['rates']]
        if f['continuous'] and iv:
            iv = min(max(iv, min(intervals)), max(intervals))
        elif iv not in intervals:
            iv = min(intervals, key=lambda x: (abs(x - iv), x)) if iv else interval(f['default'])
        # A listed interval keeps its nominal rate; an interval inside a
        # continuous range rounds the rate up, so the payload still carries it.
        fps = 10000000 // iv if iv in intervals else -(-10000000 // iv)
        want = need(self.frame_bytes(fmt, frame), fps, self.speed, self.kinds[fmt] == 'yuy2')
        caps = sorted(capacity(a) for a in ALTS[self.speed])
        payload = next((c for c in caps if c >= want), caps[-1])
        if self.kinds[fmt] == 'mjpeg':
            size = f['width'] * f['height'] * 2
        else:
            size = self.frame_bytes(fmt, frame)
        return iv, size, payload

    def encode(self, hint, fmt, frame, iv, frame_size, payload):
        data = struct.pack('<HBBIHHHHHII', hint, fmt, frame, iv, 0, 0, 0, 0, 0, frame_size, payload)
        if self.probe_size == 34:
            data += struct.pack('<IBBBB', CLOCK, 3, 0, 0, 0)
        elif self.probe_size == 48:
            # UVC 1.5 Table 4-75: bmFramingInfo, bPreferedVersion,
            # bMinVersion, bMaxVersion (1: payload version 1), then bUsage 1
            # (real-time), bBitDepthLuma 8, bmSettings 0,
            # bMaxNumberOfRefFramesPlus1 1, bmRateControlModes 0,
            # bmLayoutPerStream 0.
            data += struct.pack('<IBBBB', CLOCK, 3, 1, 1, 1)
            data += struct.pack('<BBBBHQ', 1, 8, 0, 1, 0, 0)
        return data


# ---------------------------------------------------------------------------
# The usbredir device.

class Model:
    def __init__(self, connection, trace, camera, now=time.monotonic, idle='header',
                 unplug_after=None, replug_after=None):
        self.connection, self.trace, self.cam, self.now = connection, trace, camera, now
        self.idle, self.unplug_after, self.replug_after = idle, unplug_after, replug_after
        self.peer_caps = 0
        self.id64 = False
        self.connected = False      # device present to the peer
        self.awaiting_ack = False
        self.unplug_at = self.replug_at = None
        self.unplugged = False      # the timed unplug happens once
        self.brightness = 0
        self.vc_error = self.vs_error = 0
        self.int_running = False
        self.totals = dict(frames=0, packets=0, data_packets=0, idle_packets=0, bytes=0,
                           late_slots=0, skipped_frames=0, streams=0)
        self.reset_device()

    # -- plumbing ----------------------------------------------------------
    def log(self, event, **fields):
        self.trace.write(json.dumps(dict(time=time.time(), monotonic=self.now(), event=event,
                                        simulated=True, **fields)) + '\n')
        self.trace.flush()

    def send(self, kind, ident=0, data=b''):
        self.connection.sendall(self.header(kind, len(data), ident) + data)

    def header(self, kind, length, ident):
        if self.id64:
            return struct.pack('<IIQ', kind, length, ident)
        return struct.pack('<III', kind, length, ident)

    def reset_device(self):
        self.stop_stream('reset')
        self.config = 0
        self.alt = 0
        self.default_probe()
        self.committed = None
        self.fid = 0
        self.seq = 0

    def default_probe(self):
        f = self.cam.formats[self.cam.speed][(1, 1)]
        iv, size, payload = self.cam.answer(1, 1, interval(f['default']))
        self.probe = dict(hint=0, fmt=1, frame=1, iv=iv, size=size, payload=payload)

    def info(self):
        types, intervals, interfaces = bytearray([255] * 32), bytearray(32), bytearray(32)
        packets = [0] * 32
        types[0] = types[16] = 0
        packets[0] = packets[16] = 64
        count = 0
        numbers, classes, subclasses, protocols = (bytearray(32) for _ in range(4))
        if self.config:
            count = 2
            numbers[:2], classes[:2], subclasses[:2] = b'\0\1', b'\x0e\x0e', b'\1\2'
            slot = 16 + (VC_EP & 15)
            types[slot], intervals[slot], interfaces[slot] = 3, 8 if self.cam.speed == 'high' else 16, 0
            packets[slot] = 16
            if self.alt:
                size, mult = ALTS[self.cam.speed][self.alt - 1]
                slot = 16 + (VS_EP & 15)
                types[slot], intervals[slot], interfaces[slot] = 1, 1, 1
                packets[slot] = size | (mult - 1) << 11
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << CAP_EP_MAX):
            data += struct.pack('<32H', *packets)
        self.send(EPINFO, data=data)
        self.send(IFINFO, data=struct.pack('<I', count) + numbers + classes + subclasses + protocols)

    def connect(self):
        self.info()
        speed = 2 if self.cam.speed == 'high' else 1
        data = struct.pack('<BBBBHH', speed, 0xef, 2, 1, self.cam.vid, self.cam.pid)
        if self.peer_caps & (1 << CAP_VERSION):
            data += struct.pack('<H', 0x0001)
        self.send(CONNECT, data=data)
        self.connected = True
        self.log('connected', model='SIMULATED UVC camera', speed=self.cam.speed,
                 uvc='%x' % self.cam.uvc, vid='%04x' % self.cam.vid, pid='%04x' % self.cam.pid,
                 config_sha256=hashlib.sha256(self.cam.config[self.cam.speed]).hexdigest())

    def disconnect(self):
        self.reset_device()
        self.send(DISCONNECT)
        self.connected = False
        self.awaiting_ack = bool(self.peer_caps & (1 << CAP_DISC_ACK))
        self.log('unplug', awaiting_ack=self.awaiting_ack, **self.totals)
        if self.replug_after is not None:
            self.replug_at = self.now() + self.replug_after

    # -- control transfers -------------------------------------------------
    def control(self, ident, packet):
        if len(packet) < 10:
            raise ValueError('short control header')
        ep, request, rt, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        payload = packet[10:]
        if len(payload) != (0 if rt & 128 else length):
            raise ValueError('control length mismatch')
        status, data = self.request(request, rt, value, index, length, payload)
        data = data[:length]
        actual = len(data) if rt & 128 else (0 if status else length)
        self.send(CONTROL, ident, struct.pack('<BBBBHHH', ep, request, rt, status,
                                            value, index, actual) + data)
        self.log('control', request=request, request_type=rt, value=value, index=index,
                 requested=length, status=status, actual=actual)

    def request(self, request, rt, value, index, length, payload):
        cam = self.cam
        if rt == 0x80 and request == 6:
            kind, number = value >> 8, value & 255
            other = 'full' if cam.speed == 'high' else 'high'
            data = cam.device if kind == 1 and not number else \
                cam.config[cam.speed] if kind == 2 and not number else \
                cam.strings.get(number) if kind == 3 else \
                cam.qualifier if kind == 6 and not number else \
                b'\x09\x07' + cam.config[other][2:] if kind == 7 and not number else None
            return (OK, data) if data is not None else (STALL, b'')
        if request == 0 and rt in (0x80, 0x81, 0x82):
            return OK, b'\0\0'
        if request == 8 and rt == 0x80:
            return OK, bytes((self.config,))
        if request in (1, 3) and rt == 2 and value == 0 and index in (VS_EP, VC_EP):
            return OK, b''
        if not self.config or rt not in (0x21, 0xa1):
            return STALL, b''
        iface, entity, selector = index & 255, index >> 8, value >> 8
        if iface == 0:
            return self.vc_request(request, rt, entity, selector, payload)
        if iface == 1 and entity == 0:
            return self.vs_request(request, rt, selector, payload)
        return STALL, b''

    def vc_request(self, request, rt, entity, selector, payload):
        if entity == 0 and selector == 2 and rt == 0xa1 and request == 0x81:
            return OK, bytes((self.vc_error,))
        if entity == 2 and selector == 2:   # PU_BRIGHTNESS_CONTROL
            values = {0x81: self.brightness, 0x82: -64, 0x83: 64, 0x84: 1, 0x87: 0}
            if rt == 0xa1 and request in values:
                self.vc_error = 0
                return OK, struct.pack('<h', values[request])
            if rt == 0xa1 and request == 0x86:
                return OK, b'\3'
            if rt == 0xa1 and request == 0x85:
                return OK, b'\2\0'
            if rt == 0x21 and request == 1 and len(payload) == 2:
                v = struct.unpack('<h', payload)[0]
                if -64 <= v <= 64:
                    self.brightness, self.vc_error = v, 0
                    return OK, b''
                self.vc_error = 4  # out of range
                return STALL, b''
        self.vc_error = 6          # invalid control
        return STALL, b''

    def decode(self, payload):
        hint, fmt, frame, iv = struct.unpack('<HBBI', payload[:8])
        size, xfer = struct.unpack('<II', payload[18:26])
        return hint, fmt, frame, iv, size, xfer

    def vs_request(self, request, rt, selector, payload):
        cam, size = self.cam, self.cam.probe_size
        if selector == 6 and rt == 0xa1 and request == 0x81:   # VS_STREAM_ERROR_CODE
            return OK, bytes((self.vs_error,))
        if selector not in (1, 2):
            return STALL, b''
        if rt == 0xa1:
            if request == 0x85:
                return OK, struct.pack('<H', size)
            if request == 0x86:
                return OK, b'\3'
            if request in (0x81, 0x82, 0x83, 0x87, 0x84):
                if request == 0x84:
                    return OK, cam.encode(0, 0, 0, 0, 0, 0)
                if request == 0x81 and selector == 2 and self.committed:
                    p = self.committed
                elif request == 0x81:
                    p = self.probe
                else:
                    f = cam.formats[cam.speed][(1, 1)]
                    rates = f['rates']
                    rate = {0x82: rates[0], 0x83: rates[-1], 0x87: f['default']}[request]
                    iv, fs, pay = cam.answer(1, 1, interval(rate))
                    p = dict(hint=0, fmt=1, frame=1, iv=iv, size=fs, payload=pay)
                return OK, cam.encode(p['hint'], p['fmt'], p['frame'], p['iv'], p['size'], p['payload'])
            return STALL, b''
        if request != 1 or len(payload) != size:
            self.vs_error = 7
            return STALL, b''
        hint, fmt, frame, iv, _, xfer = self.decode(payload)
        if (fmt, frame) not in cam.formats[cam.speed]:
            self.vs_error = 7
            return STALL, b''
        iv2, fs, pay = cam.answer(fmt, frame, iv)
        if selector == 1:
            self.probe = dict(hint=hint, fmt=fmt, frame=frame, iv=iv2, size=fs, payload=pay)
            self.log('probe', format=fmt, frame=frame, asked_interval=iv, interval=iv2,
                     frame_size=fs, payload=pay)
            return OK, b''
        # COMMIT: the values must be ones the device would answer; a host may
        # lower dwMaxPayloadTransferSize below the probe's answer (uvc98's
        # bandwidth quirk does), never raise it past the largest alternate.
        biggest = max(capacity(a) for a in ALTS[cam.speed])
        if iv != iv2 or not HEADER + 4 <= xfer <= biggest:
            self.vs_error = 7
            self.log('commit_rejected', format=fmt, frame=frame, interval=iv, payload=xfer)
            return STALL, b''
        self.committed = dict(hint=hint, fmt=fmt, frame=frame, iv=iv, size=fs, payload=xfer)
        self.vs_error = 0
        self.log('commit', format=fmt, frame=frame, kind=cam.formats[cam.speed][(fmt, frame)]['kind'],
                 width=cam.formats[cam.speed][(fmt, frame)]['width'],
                 height=cam.formats[cam.speed][(fmt, frame)]['height'], interval=iv,
                 frame_size=fs, payload=xfer, probe_payload=pay, differs_from_probe=(
                     (fmt, frame, xfer) != (self.probe['fmt'], self.probe['frame'], self.probe['payload'])))
        return OK, b''

    # -- streaming ---------------------------------------------------------
    def start_stream(self):
        c = self.committed
        if c is None:
            p = self.probe
            c = dict(p)
            self.log('stream_without_commit', **c)
        size, mult = ALTS[self.cam.speed][self.alt - 1]
        alt_cap = size * mult
        payload = min(c['payload'], alt_cap)
        if c['payload'] > alt_cap:
            self.log('payload_exceeds_alternate', payload=c['payload'], alternate=self.alt, capacity=alt_cap)
        if payload - HEADER < 4:
            payload = HEADER + 4
        t = self.now()
        self.stream = dict(fmt=c['fmt'], frame=c['frame'], iv=c['iv'], payload=payload,
                           chunk=(payload - HEADER) & ~3 if self.cam.kinds[c['fmt']] == 'yuy2' else payload - HEADER,
                           t0=t, slot=0, slot_s=1.0 / SLOTS[self.cam.speed],
                           due=t, data=b'', offset=0, pts=0, ident=0, packets=0,
                           frame_packets=0, start=t, last_report=t)
        self.totals['streams'] += 1
        if self.unplug_after is not None and self.unplug_at is None and not self.unplugged:
            self.unplug_at = t + self.unplug_after
        self.log('stream_start', format=c['fmt'], frame=c['frame'], interval=c['iv'],
                 payload=payload, alternate=self.alt, alternate_capacity=alt_cap)

    def stop_stream(self, why):
        s = getattr(self, 'stream', None)
        if s:
            self.log('stream_stop', why=why, stream_packets=s['packets'], **self.totals)
        self.stream = None

    def next_frame(self, at):
        s = self.stream
        iv_s = s['iv'] / 1e7
        # The sensor captures on a fixed grid; a frame not started by the
        # next capture time is replaced by the newest one (skipped).
        n = max(0, int((at - s['t0']) / iv_s))
        if s.get('captured') is not None and n > s['captured'] + 1:
            self.totals['skipped_frames'] += n - s['captured'] - 1
        if s.get('captured') is not None and n <= s['captured']:
            return False
        s['captured'] = n
        s['data'] = self.cam.picture(s['fmt'], s['frame'], self.seq)
        s['offset'] = 0
        s['pts'] = int((s['t0'] + n * iv_s - s['start']) * CLOCK) & 0xffffffff
        s['frame_packets'] = 0
        self.fid ^= 1
        return True

    def packet(self, at):
        s = self.stream
        slot = s['slot']
        sof = (slot // 8 if self.cam.speed == 'high' else slot) & 0x7ff
        stc = int((at - s['start']) * CLOCK) & 0xffffffff
        if not s['data']:
            self.next_frame(at)
        if s['data']:
            chunk = s['data'][s['offset']:s['offset'] + s['chunk']]
            s['offset'] += len(chunk)
            flags = 0x8c | self.fid
            if s['offset'] >= len(s['data']):
                flags |= 2
            body = struct.pack('<BBIIH', HEADER, flags, s['pts'], stc, sof) + chunk
            s['frame_packets'] += 1
            self.totals['data_packets'] += 1
            self.totals['bytes'] += len(chunk)
            if flags & 2:
                self.log('frame', seq=self.seq, fid=self.fid, bytes=len(s['data']),
                         packets=s['frame_packets'], pts=s['pts'],
                         sha256=hashlib.sha256(s['data']).hexdigest())
                self.totals['frames'] += 1
                self.seq += 1
                s['data'] = b''
        elif self.idle == 'header':
            body = struct.pack('<BBIIH', HEADER, 0x88 | self.fid, 0, stc, sof)
            self.totals['idle_packets'] += 1
        else:
            body = b''
            self.totals['idle_packets'] += 1
        s['slot'] += 1
        s['packets'] += 1
        self.totals['packets'] += 1
        message = self.header(ISO, 4 + len(body), s['ident']) + \
            struct.pack('<BBH', VS_EP, 0, len(body)) + body
        s['ident'] = (s['ident'] + 1) & 0xffffffff
        return message

    def pump(self, now=None):
        now = self.now() if now is None else now
        if self.unplug_at is not None and self.connected and now >= self.unplug_at:
            self.unplug_at, self.unplugged = None, True
            self.disconnect()
            return
        if self.replug_at is not None and not self.connected and not self.awaiting_ack and \
                now >= self.replug_at:
            self.replug_at = None
            self.connect()
            return
        s = self.stream
        if not s:
            return
        due = int((now - s['t0']) / s['slot_s']) + 1 - s['slot']
        limit = SLOTS[self.cam.speed] // 50          # 20 ms catch-up at most
        if due > limit:
            self.totals['late_slots'] += due - limit
            s['slot'] += due - limit
            due = limit
        if due <= 0:
            return
        out = [self.packet(s['t0'] + (s['slot'] + 1) * s['slot_s']) for _ in range(due)]
        self.connection.sendall(b''.join(out))
        if now - s['last_report'] >= 1.0:
            s['last_report'] = now
            self.log('stream_totals', **self.totals)

    # -- usbredir messages -------------------------------------------------
    def handle(self, kind, ident, packet):
        if not self.connected:
            if kind == DISCONNECT_ACK:
                self.awaiting_ack = False
                self.log('disconnect_ack')
            return  # a removed device answers nothing
        if kind == RESET:
            if packet:
                raise ValueError('reset length')
            self.reset_device()
            self.log('reset')
        elif kind in (SET_CONFIG, GET_CONFIG):
            if len(packet) != (1 if kind == SET_CONFIG else 0):
                raise ValueError('configuration length')
            status = OK
            if kind == SET_CONFIG:
                status = OK if packet[0] in (0, 1) else STALL
                if status == OK:
                    self.stop_stream('set_configuration')
                    self.config, self.alt, self.committed = packet[0], 0, None
                    self.default_probe()
                    self.info()
                    self.log('configuration', value=self.config)
            self.send(CONFIG_STATUS, ident, bytes((status, self.config)))
        elif kind in (SET_ALT, GET_ALT):
            if len(packet) != (2 if kind == SET_ALT else 1):
                raise ValueError('alternate length')
            interface = packet[0]
            current = self.alt if interface == 1 else 0
            alternate = packet[1] if kind == SET_ALT else current
            ok = self.config and (interface == 0 and alternate == 0 or
                                  interface == 1 and alternate <= len(ALTS[self.cam.speed]))
            status = OK if ok else STALL
            if kind == SET_ALT and ok and interface == 1:
                self.stop_stream('set_alternate')
                self.alt = alternate
                self.info()
                self.log('alternate', interface=1, alternate=alternate,
                         capacity=capacity(ALTS[self.cam.speed][alternate - 1]) if alternate else 0)
            self.send(ALT_STATUS, ident, bytes((status, interface, alternate if ok else current)))
        elif kind in (START_ISO, STOP_ISO):
            if len(packet) != (3 if kind == START_ISO else 1):
                raise ValueError('iso stream header')
            ep = packet[0]
            status = OK if ep == VS_EP and self.config and self.alt else STALL
            if kind == START_ISO and status == OK:
                if not packet[1] or not packet[2]:
                    status = STALL
                else:
                    self.stop_stream('restart')
                    self.start_stream()
            elif kind == STOP_ISO:
                self.stop_stream('stop_iso_stream')
                status = OK
            self.send(ISO_STATUS, ident, bytes((status, ep)))
            self.log('iso_stream', start=kind == START_ISO, endpoint=ep, status=status,
                     packets_per_urb=packet[1] if kind == START_ISO else 0,
                     urbs=packet[2] if kind == START_ISO else 0)
        elif kind in (START_INT, STOP_INT):
            if len(packet) != 1:
                raise ValueError('interrupt header')
            ep = packet[0]
            status = OK if ep == VC_EP and self.config else STALL
            self.int_running = kind == START_INT and status == OK
            self.send(INT_STATUS, ident, bytes((status, ep)))
            self.log('interrupt_receiving', start=kind == START_INT, endpoint=ep, status=status)
        elif kind == CONTROL:
            self.control(ident, packet)
        elif kind == CANCEL:
            if packet:
                raise ValueError('cancel length')
        elif kind == DISCONNECT_ACK:
            pass
        else:
            raise ValueError(f'unsupported usbredir message {kind}')

    def run(self):
        self.send(HELLO, data=b'usb-wdm-free UVC SIMULATED'.ljust(64, b'\0') + struct.pack('<I', CAPS))
        buffer, hello = bytearray(), False
        while True:
            self.pump()
            if self.stream:
                timeout = 0.0005
            elif self.unplug_at is not None or self.replug_at is not None:
                timeout = 0.01
            else:
                timeout = 0.05
            readable, _, _ = select.select([self.connection], [], [], timeout)
            if not readable:
                continue
            data = self.connection.recv(65536)
            if not data:
                raise EOFError
            buffer.extend(data)
            while True:
                size = 16 if self.id64 else 12
                if len(buffer) < size:
                    break
                kind, length, ident = struct.unpack('<IIQ' if self.id64 else '<III', buffer[:size])
                if length > 65546:
                    raise ValueError('message bound')
                if len(buffer) < size + length:
                    break
                packet = bytes(buffer[size:size + length])
                del buffer[:size + length]
                if not hello:
                    if kind != HELLO or ident or not 64 <= length <= 4096:
                        raise ValueError('hello framing')
                    self.peer_caps = int.from_bytes(packet[64:68], 'little') if length >= 68 else 0
                    self.id64 = bool(self.peer_caps & CAPS & (1 << CAP_ID64))
                    self.log('hello', peer=packet[:64].rstrip(b'\0').decode('ascii', 'replace'),
                             peer_caps=self.peer_caps)
                    self.connect()
                    hello = True
                else:
                    self.handle(kind, ident, packet)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=49041)
    ap.add_argument('--log', type=Path, required=True, help='new JSONL trace (must not exist)')
    ap.add_argument('--speed', choices=('high', 'full'), default='high')
    ap.add_argument('--uvc', choices=('1.0', '1.1', '1.5'), default='1.1')
    ap.add_argument('--h264', choices=('frame', 'native', 'both', 'none'), default='frame',
                    help='UVC 1.5: the H.264 format beside YUY2/MJPEG/NV12: frame-based, '
                         'VS_FORMAT_H264, both, or none')
    ap.add_argument('--vid', type=lambda s: int(s, 16), default=0x046d)
    ap.add_argument('--pid', type=lambda s: int(s, 16), default=0x094c)
    ap.add_argument('--fps', type=int, help='default frame rate where a frame lists it')
    ap.add_argument('--idle', choices=('header', 'empty'), default='header',
                    help='packets between frames: 12-byte headers or zero length')
    ap.add_argument('--no-dht', action='store_true', help='MJPEG without DHT, as many cameras send')
    ap.add_argument('--unplug-after', type=float, help='seconds after the first stream start')
    ap.add_argument('--replug-after', type=float, help='seconds after the unplug')
    args = ap.parse_args()
    camera = Camera(args.speed, {'1.0': 0x100, '1.1': 0x110, '1.5': 0x150}[args.uvc], args.vid,
                    args.pid, args.fps, not args.no_dht, args.h264)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'SIMULATED UVC camera listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            model = Model(connection, trace, camera, idle=args.idle,
                          unplug_after=args.unplug_after, replug_after=args.replug_after)
            try:
                model.run()
            except (EOFError, ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
                model.stop_stream('disconnected')
                model.log('disconnected', **model.totals)
            except Exception as error:
                model.log('fatal', error=repr(error), **model.totals)
                raise


if __name__ == '__main__':
    main()
