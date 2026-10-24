#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests of uvcsim.py: the model's descriptors through the driver's own
descriptor parser, its probe/commit, and its isochronous payload stream
through the driver's own payload assembler (out/video/uvc98/hostabi, built by
host.ps1). MJPEG frames are also decoded by FFmpeg when it is installed (an
independent decoder). Run: python -I -B uvcsimtest.py
"""
import ctypes as c
import io
import json
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import uvcsim as u  # noqa: E402


class Stream(c.Structure):
    _fields_ = [(n, c.c_uint) for n in ('Interface', 'Endpoint', 'Terminal', 'StillMethod')]


class Frame(c.Structure):
    _fields_ = [(n, c.c_uint) for n in ('Stream', 'Format', 'Frame', 'Kind', 'Width', 'Height')] + \
        [(n, c.c_ulong) for n in ('FrameBytes', 'DescriptorMaxBytes', 'DefaultInterval')] + \
        [(n, c.c_uint) for n in ('IntervalOffset', 'IntervalCount')]


class Alt(c.Structure):
    _fields_ = [(n, c.c_uint) for n in ('Stream', 'Alternate', 'Endpoint', 'Type', 'Interval',
                                         'PacketBytes', 'Transactions', 'Capacity', 'ServiceMicroframes')]


class Desc(c.Structure):
    _fields_ = [('Bytes', c.c_void_p)] + \
        [(n, c.c_uint) for n in ('Length', 'Valid', 'Revision', 'Control', 'Speed',
                                 'StreamCount', 'FrameCount', 'AltCount')] + \
        [('Stream', Stream * 8), ('Frame', Frame * 128), ('Alt', Alt * 128)]


class Ctrl(c.Structure):
    _fields_ = [(n, c.c_uint) for n in ('Hint', 'Format', 'Frame')] + [('Interval', c.c_ulong)] + \
        [(n, c.c_uint) for n in ('KeyRate', 'PFrameRate', 'Quality', 'Window', 'Delay')] + \
        [(n, c.c_ulong) for n in ('FrameBytes', 'PayloadBytes', 'Clock')] + \
        [(n, c.c_uint) for n in ('Framing', 'Preferred', 'Minimum', 'Maximum')] + \
        [('Tail', c.c_ubyte * 14)]


class NegResult(c.Structure):
    _fields_ = [('Control', Ctrl), ('DeviceControl', Ctrl)] + \
        [(n, c.c_uint) for n in ('FrameIndex', 'AltIndex', 'Requests')]


class Pay(c.Structure):
    _fields_ = [('Buffer', c.c_void_p)] + [(n, c.c_uint) for n in (
        'Capacity', 'Expected', 'Used', 'HaveFid', 'LastFid', 'Active', 'Bad', 'Still', 'Unsynced')] + \
        [(n, c.c_ulong) for n in ('Frames', 'Dropped', 'InvalidPackets', 'ErrorPackets', 'Overflows')] + \
        [('Deliver', c.c_void_p), ('Context', c.c_void_p)] + \
        [(n, c.c_ulong) for n in ('Eofs', 'FidFlips', 'DropBad', 'DropSize')] + [('LastDropUsed', c.c_uint)]


class Connection:
    def __init__(self):
        self.messages, self.raw = [], bytearray()

    def sendall(self, data):
        self.raw.extend(data)
        while len(self.raw) >= 12:
            kind, count, ident = struct.unpack('<III', self.raw[:12])
            if len(self.raw) < 12 + count:
                break
            self.messages.append((kind, ident, bytes(self.raw[12:12 + count])))
            del self.raw[:12 + count]

    def take(self, kind=None):
        out = [m for m in self.messages if kind is None or m[0] == kind]
        self.messages = [m for m in self.messages if not (kind is None or m[0] == kind)]
        return out


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


DLL_PATH = Path(__file__).resolve().parents[3] / 'out/video/uvc98/hostabi/uvccore.dll'
FFMPEG = shutil.which('ffmpeg')


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dll = c.CDLL(str(DLL_PATH))
        cls.callback_type = c.CFUNCTYPE(None, c.c_void_p, c.c_void_p, c.c_uint, c.c_uint)
        cls.dll.UvcDescParse.argtypes = [c.c_void_p, c.c_uint, c.c_uint, c.c_uint, c.c_void_p]
        cls.dll.UvcDescParse.restype = c.c_int
        cls.dll.UvcPayInit.argtypes = [c.c_void_p, c.c_void_p, c.c_uint, c.c_uint,
                                       cls.callback_type, c.c_void_p]
        cls.dll.UvcPayInit.restype = c.c_int
        cls.dll.UvcPayFeed.argtypes = [c.c_void_p, c.c_void_p, c.c_uint]
        cls.dll.UvcPayFeed.restype = None
        cls.dll.UvcPayLost.argtypes = [c.c_void_p]
        cls.neg_type = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_uint, c.c_uint, c.c_uint, c.c_void_p, c.c_uint)
        cls.dll.UvcNegotiateLimited.argtypes = [c.c_void_p, c.c_uint, c.c_ulong, c.c_ulong,
                                                cls.neg_type, c.c_void_p, c.c_void_p]
        cls.dll.UvcNegotiateLimited.restype = c.c_int
        cls.dll.UvcTestPaySize.restype = cls.dll.UvcTestDescSize.restype = c.c_uint
        assert cls.dll.UvcTestDescSize() == c.sizeof(Desc), 'UVC_DESC layout'
        assert cls.dll.UvcTestPaySize() == c.sizeof(Pay), 'UVC_PAY layout'

    # -- helpers -----------------------------------------------------------
    def parse(self, config, speed):
        desc = Desc()
        blob = c.create_string_buffer(config)
        self.keep = blob
        result = self.dll.UvcDescParse(blob, len(config), 0, 2 if speed == 'high' else 1, c.byref(desc))
        return result, desc

    def model(self, speed='high', uvc=0x110, **kw):
        cam = u.Camera(speed, uvc, fps=kw.pop('fps', None), dht=kw.pop('dht', True),
                       h264=kw.pop('h264', 'frame'))
        conn, log, clock = Connection(), io.StringIO(), Clock()
        m = u.Model(conn, log, cam, now=clock, **kw)
        m.peer_caps = u.CAPS
        m.connect()
        conn.take()
        return m, conn, log, clock

    def control(self, m, request, rt, value, index=0x0001, payload=b'', length=None):
        if length is None:
            length = len(payload)
        m.control(42, struct.pack('<BBBBHHH', 0x80 if rt & 128 else 0, request, rt, 0,
                                  value, index, length) + payload)
        kind, _, reply = m.connection.messages.pop()
        self.assertEqual(kind, u.CONTROL)
        return reply[3], reply[10:]

    def negotiate(self, m, fmt, frame, iv, lower=None):
        size = m.cam.probe_size
        probe = bytearray(size)
        struct.pack_into('<HBBI', probe, 0, 1, fmt, frame, iv)
        self.assertEqual(self.control(m, 1, 0x21, 0x100, payload=bytes(probe))[0], 0)
        status, answer = self.control(m, 0x81, 0xa1, 0x100, length=size)
        self.assertEqual((status, len(answer)), (0, size))
        commit = bytearray(answer)
        if lower:
            struct.pack_into('<I', commit, 22, lower)
        self.assertEqual(self.control(m, 1, 0x21, 0x200, payload=bytes(commit))[0], 0)
        return answer

    def assembler(self, expected, capacity):
        frames = []
        callback = self.callback_type(lambda _, p, n, still: frames.append(c.string_at(p, n)))
        state = Pay()
        buf = c.create_string_buffer(capacity)
        self.assertEqual(self.dll.UvcPayInit(c.byref(state), buf, capacity, expected, callback, None), 1)
        return state, frames, (callback, buf)

    def stream(self, m, conn, clock, fmt, frame, seconds, alt=None, lower=None, iv=None):
        f = m.cam.formats[m.cam.speed][(fmt, frame)]
        iv = iv or u.interval(f['default'])
        answer = self.negotiate(m, fmt, frame, iv, lower)
        payload = lower or struct.unpack('<I', answer[22:26])[0]
        if alt is None:
            caps = [u.capacity(a) for a in u.ALTS[m.cam.speed]]
            alt = 1 + min((i for i, cap in enumerate(caps) if cap >= payload), key=lambda i: caps[i])
        m.handle(u.SET_ALT, 7, bytes((1, alt)))
        self.assertEqual(conn.take(u.ALT_STATUS)[-1][2], bytes((0, 1, alt)))
        m.handle(u.START_ISO, 0, bytes((u.VS_EP, 8, 6)))
        self.assertEqual(conn.take(u.ISO_STATUS)[-1][2], bytes((0, u.VS_EP)))
        packets = []
        step = 0.001
        for _ in range(int(seconds / step)):
            clock.t += step
            m.pump()
            for kind, ident, body in conn.take(u.ISO):
                ep, status, length = struct.unpack('<BBH', body[:4])
                self.assertEqual((ep, status, length), (u.VS_EP, 0, len(body) - 4))
                packets.append((ident, body[4:]))
        return packets, payload, alt

    def configure(self, m, conn):
        m.handle(u.SET_CONFIG, 1, b'\1')
        self.assertEqual(conn.take(u.CONFIG_STATUS)[-1][2], b'\0\1')

    # -- descriptors -------------------------------------------------------
    def test_descriptors_through_driver_parser(self):
        for speed in ('high', 'full'):
            for uvc in (0x100, 0x110):
                cam = u.Camera(speed, uvc)
                config = cam.config[speed]
                self.assertEqual(int.from_bytes(config[2:4], 'little'), len(config))
                result, d = self.parse(config, speed)
                self.assertEqual(result, 0, (speed, uvc))
                self.assertEqual((d.Valid, d.Revision, d.StreamCount, d.FrameCount),
                                 (1, uvc, 1, 6))
                frames = [(d.Frame[i].Kind, d.Frame[i].Width, d.Frame[i].Height,
                           d.Frame[i].DefaultInterval) for i in range(6)]
                want = [(k, w, h, u.interval(cam.formats[speed][(fmt, i + 1)]['default']))
                        for fmt, k in ((1, 1), (2, 2)) for i, (w, h) in enumerate(u.SIZES)]
                self.assertEqual(frames, want)
                caps = sorted(d.Alt[i].Capacity for i in range(d.AltCount) if d.Alt[i].Type == 1)
                self.assertEqual(caps, sorted(u.capacity(a) for a in u.ALTS[speed]))
                # The other-speed configuration parses at the other speed.
                other = 'full' if speed == 'high' else 'high'
                self.assertEqual(self.parse(cam.config[other], other)[0], 0)

    def test_bandwidth_shape(self):
        """HS: single-transaction alternates carry every size to 320x240 at
        30 fps; only YUY2 640x480 at 30 fps needs a multi-transaction one.
        FS: every default fits the 1023-byte alternate."""
        for speed in ('high', 'full'):
            cam = u.Camera(speed)
            single = max(u.capacity(a) for a in u.ALTS[speed] if a[1] == 1)
            for (fmt, frame), f in cam.formats[speed].items():
                for rate in f['rates']:
                    _, _, payload = cam.answer(fmt, frame, u.interval(rate))
                    multi = payload > single
                    if speed == 'full' or f['width'] <= 320 or fmt == 2 or rate <= 10:
                        self.assertFalse(multi, (speed, fmt, frame, rate, payload))
                    self.assertLessEqual(payload, max(u.capacity(a) for a in u.ALTS[speed]))
        self.assertGreater(u.Camera('high').answer(1, 3, 333333)[2], 1024)

    def test_standard_requests(self):
        m, conn, _, _ = self.model()
        st, dev = self.control(m, 6, 0x80, 0x100, index=0, length=18)
        self.assertEqual((st, dev[4:7], dev[8:12]), (0, b'\xef\x02\x01', b'\x6d\x04\x4c\x09'))
        st, q = self.control(m, 6, 0x80, 0x600, index=0, length=10)
        self.assertEqual((st, q[1]), (0, 6))
        st, other = self.control(m, 6, 0x80, 0x700, index=0, length=4096)
        self.assertEqual((st, other[1]), (0, 7))
        self.assertEqual(other[2:], m.cam.config['full'][2:])
        st, s2 = self.control(m, 6, 0x80, 0x302, index=0x409, length=255)
        self.assertIn('SIMULATED'.encode('utf-16le'), s2)
        self.assertEqual(self.control(m, 6, 0x80, 0x303, index=0x409, length=255)[0], 4)
        # Class requests stall before SET_CONFIGURATION.
        self.assertEqual(self.control(m, 0x81, 0xa1, 0x100, length=34)[0], 4)

    def test_probe_commit(self):
        for uvc, size in ((0x100, 26), (0x110, 34)):
            m, conn, log, _ = self.model(uvc=uvc)
            self.configure(m, conn)
            self.assertEqual(self.control(m, 0x85, 0xa1, 0x100, length=2), (0, struct.pack('<H', size)))
            self.assertEqual(self.control(m, 0x86, 0xa1, 0x100, length=1), (0, b'\3'))
            for req in (0x82, 0x83, 0x87, 0x84):
                st, data = self.control(m, req, 0xa1, 0x100, length=size)
                self.assertEqual((st, len(data)), (0, size))
            self.assertEqual(struct.unpack('<I', self.control(m, 0x82, 0xa1, 0x100, length=size)[1][4:8])[0], 333333)
            # Every frame at every rate negotiates; the answer carries the frame size.
            for (fmt, frame), f in m.cam.formats['high'].items():
                for rate in f['rates']:
                    answer = self.negotiate(m, fmt, frame, u.interval(rate))
                    hint, af, afr, iv = struct.unpack('<HBBI', answer[:8])
                    fs, pay = struct.unpack('<II', answer[18:26])
                    self.assertEqual((af, afr, iv), (fmt, frame, u.interval(rate)))
                    self.assertEqual(fs, f['width'] * f['height'] * 2)
                    if size == 34:
                        self.assertEqual(struct.unpack('<I', answer[26:30])[0], u.CLOCK)
            # An unlisted interval is adjusted to the nearest listed one.
            probe = bytearray(size)
            struct.pack_into("<HBBI", probe, 0, 1, 1, 2, 450000)
            self.control(m, 1, 0x21, 0x100, payload=bytes(probe))
            self.assertEqual(struct.unpack('<I', self.control(m, 0x81, 0xa1, 0x100, length=size)[1][4:8])[0], 500000)
            # Bad format, wrong length, raised payload and unadjusted interval stall.
            struct.pack_into('<HBBI', probe, 0, 1, 3, 1, 333333)
            self.assertEqual(self.control(m, 1, 0x21, 0x100, payload=bytes(probe))[0], 4)
            self.assertEqual(self.control(m, 0x81, 0xa1, 0x600, index=0x0001, length=1), (0, b'\7'))
            self.assertEqual(self.control(m, 1, 0x21, 0x100, payload=bytes(size - 1))[0], 4)
            answer = bytearray(self.negotiate(m, 1, 1, 333333))
            struct.pack_into('<I', answer, 22, 4000)
            self.assertEqual(self.control(m, 1, 0x21, 0x200, payload=bytes(answer))[0], 4)
            struct.pack_into('<II', answer, 4, 400000, 0)
            struct.pack_into('<I', answer, 22, 192)
            self.assertEqual(self.control(m, 1, 0x21, 0x200, payload=bytes(answer))[0], 4)
            # A lowered payload (uvc98's Brio quirk) commits, and GET_CUR(COMMIT) returns it.
            self.negotiate(m, 1, 2, 333333, lower=612)
            st, cur = self.control(m, 0x81, 0xa1, 0x200, length=size)
            self.assertEqual(struct.unpack('<I', cur[22:26])[0], 612)
            events = [json.loads(line) for line in log.getvalue().splitlines()]
            self.assertTrue(any(e['event'] == 'commit' and e['differs_from_probe'] for e in events))

    def test_vc_controls(self):
        m, conn, _, _ = self.model()
        self.configure(m, conn)
        pu = 0x0200
        self.assertEqual(self.control(m, 0x86, 0xa1, 0x200, index=pu, length=1), (0, b'\3'))
        self.assertEqual(self.control(m, 0x83, 0xa1, 0x200, index=pu, length=2), (0, struct.pack('<h', 64)))
        self.assertEqual(self.control(m, 1, 0x21, 0x200, index=pu, payload=struct.pack('<h', -5))[0], 0)
        self.assertEqual(self.control(m, 0x81, 0xa1, 0x200, index=pu, length=2), (0, struct.pack('<h', -5)))
        self.assertEqual(self.control(m, 1, 0x21, 0x200, index=pu, payload=struct.pack('<h', 99))[0], 4)
        self.assertEqual(self.control(m, 0x81, 0xa1, 0x200, index=0, length=1), (0, b'\4'))
        self.assertEqual(self.control(m, 0x81, 0xa1, 0x300, index=pu, length=2)[0], 4)

    # -- streaming ---------------------------------------------------------
    def check_stream(self, speed, fmt, frame, seconds, lower=None, iv=None, idle='header', uvc=0x110):
        m, conn, log, clock = self.model(speed, uvc, idle=idle)
        self.configure(m, conn)
        packets, payload, alt = self.stream(m, conn, clock, fmt, frame, seconds, lower=lower, iv=iv)
        f = m.cam.formats[speed][(fmt, frame)]
        fb = f['frame_bytes']
        state, frames, keep = self.assembler(fb if m.cam.uncompressed(fmt) else 0, fb)
        alt_cap = u.capacity(u.ALTS[speed][alt - 1])
        fids, pts_by_fid, last = [], {}, None
        for i, (ident, body) in enumerate(packets):
            self.assertEqual(ident, i)              # consecutive iso ids
            self.assertLessEqual(len(body), min(payload, alt_cap))
            if body:
                self.assertEqual(body[0], 12)
                flags = body[1]
                self.assertEqual(flags & 0xcc, 0x8c if len(body) > 12 or flags & 2 else 0x88)
                if len(body) > 12:
                    pts = struct.unpack('<I', body[2:6])[0]
                    key = (len(fids), flags & 1)
                    if fmt == 1:
                        self.assertEqual((len(body) - 12) % 4, 0)
                    if not fids or fids[-1] != flags & 1:
                        fids.append(flags & 1)
                    pts_by_fid.setdefault(len(fids), set()).add(pts)
                last = body
                self.dll.UvcPayFeed(c.byref(state), c.create_string_buffer(body), len(body))
        # One slot per (micro)frame: the number of packets is real time.
        self.assertAlmostEqual(len(packets), seconds * u.SLOTS[speed], delta=u.SLOTS[speed] * 0.01 + 2)
        for a, b in zip(fids, fids[1:]):
            self.assertNotEqual(a, b)
        for values in pts_by_fid.values():
            self.assertEqual(len(values), 1)      # one PTS per frame
        events = [json.loads(line) for line in log.getvalue().splitlines()]
        sent = [e for e in events if e['event'] == 'frame']
        self.assertEqual(state.DropBad + state.DropSize + state.InvalidPackets + state.Overflows, 0)
        # The assembler delivers every sent frame but the first (it syncs on it).
        self.assertEqual(len(frames), len(sent) - 1)
        for k, data in enumerate(frames, 1):
            self.assertEqual(data, m.cam.picture(fmt, frame, k))
        self.assertGreaterEqual(len(frames), 2)
        self.assertEqual(len({bytes(x) for x in frames}), len(frames))   # content changes
        rate = 10000000 / (iv or u.interval(f['default']))
        return m, frames, len(sent) / seconds, rate, payload, alt

    def test_yuy2_high_speed_every_size(self):
        for frame, wanted_alt in ((1, 1), (2, 3), (3, 7)):
            m, frames, achieved, rate, payload, alt = self.check_stream('high', 1, frame, 0.5)
            self.assertEqual(alt, wanted_alt)
            self.assertGreaterEqual(achieved, rate * 0.9)

    def test_yuy2_brio_quirk_payload(self):
        # uvc98 commits ((fb + 7999) // 8000) * fps + 12 for 046D:094C.
        for frame, lower in ((1, 162), (2, 612), (3, 2322)):
            m, frames, achieved, rate, payload, alt = self.check_stream('high', 1, frame, 0.5, lower=lower)
            self.assertEqual(payload, lower)
            self.assertGreaterEqual(achieved, rate * 0.9)

    def test_yuy2_full_speed(self):
        for frame, seconds in ((1, 0.5), (2, 1.0)):
            m, frames, achieved, rate, payload, alt = self.check_stream('full', 1, frame, seconds)
            self.assertLessEqual(payload, 1023)
        self.check_stream('full', 1, 3, 3.5)

    def test_low_rate_and_empty_idle(self):
        m, frames, achieved, rate, payload, alt = self.check_stream('high', 1, 3, 1.0, iv=u.interval(5), idle='empty')
        self.assertLessEqual(payload, 1024)
        self.assertAlmostEqual(achieved, 5, delta=1.1)

    def test_uvc10_stream(self):
        self.check_stream('high', 1, 2, 0.3, uvc=0x100)

    def test_mjpeg_streams(self):
        for speed in ('high', 'full'):
            for frame in (1, 2, 3):
                m, frames, achieved, rate, payload, alt = self.check_stream(speed, 2, frame, 0.3)
                for data in frames:
                    self.assertEqual((data[:2], data[-2:]), (b'\xff\xd8', b'\xff\xd9'))

    def test_overrun_skips_frames(self):
        # A committed payload too small for the rate: frames slip, never tear.
        m, conn, log, clock = self.model()
        self.configure(m, conn)
        packets, payload, alt = self.stream(m, conn, clock, 1, 2, 1.0, alt=1, lower=192)
        self.assertGreater(m.totals['skipped_frames'], 0)
        state, frames, keep = self.assembler(153600, 153600)
        for _, body in packets:
            self.dll.UvcPayFeed(c.byref(state), c.create_string_buffer(body), len(body))
        self.assertEqual(state.DropSize + state.DropBad, 0)
        self.assertGreaterEqual(len(frames), 1)

    def test_alternate_change_and_restart(self):
        m, conn, log, clock = self.model()
        self.configure(m, conn)
        self.stream(m, conn, clock, 1, 1, 0.1)
        m.handle(u.SET_ALT, 8, bytes((1, 0)))
        info = conn.take(u.EPINFO)[-1][2]
        self.assertEqual(info[16 + 1], 255)       # 0x81 gone at alternate 0
        conn.take()
        clock.t += 0.1
        m.pump()
        self.assertFalse(conn.take(u.ISO))
        self.assertEqual(self.control(m, 0x81, 0xa1, 0x200, length=34)[0], 0)
        # Iso start on alternate 0 stalls; a new size streams after a new commit.
        m.handle(u.START_ISO, 0, bytes((u.VS_EP, 8, 6)))
        self.assertEqual(conn.take(u.ISO_STATUS)[-1][2], bytes((4, u.VS_EP)))
        packets, payload, alt = self.stream(m, conn, clock, 2, 3, 0.2)
        self.assertTrue(any(len(b) > 12 for _, b in packets))
        # stop_iso_stream stops packets; set_alt to an unknown alternate stalls.
        m.handle(u.STOP_ISO, 0, bytes((u.VS_EP,)))
        conn.take()
        clock.t += 0.1
        m.pump()
        self.assertFalse(conn.take(u.ISO))
        m.handle(u.SET_ALT, 9, bytes((1, 99)))
        self.assertEqual(conn.take(u.ALT_STATUS)[-1][2][0], 4)
        # Endpoint info carries the raw wMaxPacketSize with its mult bits.
        m.handle(u.SET_ALT, 10, bytes((1, 7)))
        info = conn.take(u.EPINFO)[-1][2]
        self.assertEqual(struct.unpack('<H', info[96 + 2 * 17:96 + 2 * 18])[0], 800 | 2 << 11)
        # SET_CONFIGURATION and RESET stop streaming and forget the commit.
        m.handle(u.START_ISO, 0, bytes((u.VS_EP, 8, 6)))
        m.handle(u.RESET, 0, b'')
        self.assertIsNone(m.stream)
        self.assertIsNone(m.committed)

    def test_interrupt_endpoint(self):
        m, conn, _, _ = self.model()
        self.configure(m, conn)
        m.handle(u.START_INT, 3, bytes((u.VC_EP,)))
        self.assertEqual(conn.take(u.INT_STATUS)[-1], (u.INT_STATUS, 3, bytes((0, u.VC_EP))))
        m.handle(u.STOP_INT, 4, bytes((u.VC_EP,)))
        self.assertEqual(conn.take(u.INT_STATUS)[-1][2], bytes((0, u.VC_EP)))

    def test_unplug_and_replug(self):
        m, conn, log, clock = self.model(unplug_after=0.2, replug_after=0.5)
        self.configure(m, conn)
        packets, _, _ = self.stream(m, conn, clock, 1, 1, 0.3)
        kinds = [k for k, _, _ in conn.messages]
        self.assertIn(u.DISCONNECT, kinds)
        self.assertFalse(m.connected)
        conn.take()
        # While removed, requests are not answered and nothing streams.
        m.handle(u.CONTROL, 5, struct.pack('<BBBBHHH', 0x80, 6, 0x80, 0, 0x100, 0, 18))
        clock.t += 1.0
        m.pump()
        self.assertFalse(conn.messages)           # waits for the ack
        m.handle(u.DISCONNECT_ACK, 0, b'')
        m.pump()
        kinds = [k for k, _, _ in conn.take()]
        self.assertEqual(kinds, [u.EPINFO, u.IFINFO, u.CONNECT])
        self.assertTrue(m.connected)
        self.assertEqual(m.config, 0)
        # The replugged device stays: no second timed unplug, also after a new stream.
        self.configure(m, conn)
        self.stream(m, conn, clock, 1, 1, 0.5)
        clock.t += 5
        m.pump()
        self.assertTrue(m.connected)
        self.assertNotIn(u.DISCONNECT, [k for k, _, _ in conn.messages])

    def test_eof_on_socket_close(self):
        a, b = socket.socketpair()
        with a, b:
            log = io.StringIO()
            m = u.Model(a, log, u.Camera())
            b.sendall(struct.pack('<III', 0, 68, 0) + b'qemu'.ljust(64, b'\0') + struct.pack('<I', u.CAPS))
            b.shutdown(socket.SHUT_WR)
            with self.assertRaises(EOFError):
                m.run()
            events = [json.loads(line)['event'] for line in log.getvalue().splitlines()]
            self.assertEqual(events[:2], ['hello', 'connected'])

    def wire(self, peer_caps, requests):
        """Run the model on a socket as QEMU would: hello, then requests."""
        a, b = socket.socketpair()
        with a, b:
            m = u.Model(a, io.StringIO(), u.Camera())
            wide = bool(peer_caps & u.CAPS & (1 << u.CAP_ID64))
            out = struct.pack('<III', 0, 68, 0) + b'qemu'.ljust(64, b'\0') + struct.pack('<I', peer_caps)
            for kind, ident, data in requests:
                out += struct.pack('<IIQ' if wide else '<III', kind, len(data), ident) + data
            b.sendall(out)
            b.shutdown(socket.SHUT_WR)
            with self.assertRaises(EOFError):
                m.run()
            a.shutdown(socket.SHUT_WR)
            raw = bytearray()
            b.settimeout(1)
            while True:
                part = b.recv(65536)
                if not part:
                    break
                raw.extend(part)
        kind, n, ident = struct.unpack('<III', raw[:12])  # hello: 32-bit ids
        self.assertEqual((kind, ident), (u.HELLO, 0))
        caps = struct.unpack('<I', raw[12 + 64:12 + 68])[0]
        del raw[:12 + n]
        size = 16 if wide else 12
        messages = []
        while raw:
            kind, n, ident = struct.unpack('<IIQ' if wide else '<III', raw[:size])
            messages.append((kind, ident, bytes(raw[size:size + n])))
            del raw[:size + n]
        return caps, messages

    def test_xhci_capabilities_and_64bit_ids(self):
        # qemu-xhci ports need these three, or QEMU rejects the device.
        for cap in (u.CAP_EP_MAX, u.CAP_ID64, u.CAP_BULK32):
            self.assertTrue(u.CAPS & 1 << cap)
        ident = 0x1122334455
        caps, messages = self.wire(u.CAPS, [(u.CONTROL, ident, struct.pack(
            '<BBBBHHH', 0x80, 6, 0x80, 0, 0x100, 0, 18))])
        self.assertEqual(caps, u.CAPS)
        self.assertEqual([k for k, _, _ in messages], [u.EPINFO, u.IFINFO, u.CONNECT, u.CONTROL])
        self.assertEqual(messages[-1][1], ident)
        self.assertEqual(messages[-1][2][10:12], b'\x12\x01')
        # A peer without 64-bit ids keeps 32-bit headers.
        _, messages = self.wire(u.CAPS & ~(1 << u.CAP_ID64), [(u.CONTROL, 7, struct.pack(
            '<BBBBHHH', 0x80, 6, 0x80, 0, 0x100, 0, 18))])
        self.assertEqual((messages[-1][0], messages[-1][1]), (u.CONTROL, 7))

    # -- UVC 1.5 -----------------------------------------------------------
    def subtypes(self, config):
        out, o = [], 9
        while o < len(config):
            if config[o + 1] == 0x24:
                out.append((config[o + 2], config[o]))
            o += config[o]
        return out

    def driver_negotiate(self, m, frame_index, d, interval=0, limit=0):
        """Runs the driver's own UvcNegotiateLimited (uvcneg.c) against the
        model, each class request a control transfer to it."""
        def xfer(_, request, selector, iface, buf, n):
            if request == 0x01:
                status, _ = self.control(m, 1, 0x21, selector << 8, index=iface,
                                         payload=c.string_at(buf, n))
                return -1 if status else n
            status, data = self.control(m, 0x81, 0xa1, selector << 8, index=iface, length=n)
            if status:
                return -1
            c.memmove(buf, data, len(data))
            return len(data)
        cb = self.neg_type(xfer)
        res = NegResult()
        rc = self.dll.UvcNegotiateLimited(c.byref(d), frame_index, interval, limit, cb, None,
                                          c.byref(res))
        return rc, res

    def test_uvc15_descriptors_through_driver_parser(self):
        for speed in ('high', 'full'):
            for h264 in ('frame', 'none'):
                cam = u.Camera(speed, 0x150, h264=h264)
                config = cam.config[speed]
                self.assertEqual(int.from_bytes(config[2:4], 'little'), len(config))
                kinds = self.subtypes(config)
                self.assertIn((5, 13), kinds)                 # PU, bControlSize 3
                self.assertIn((7, 13), kinds)                 # encoding unit
                self.assertEqual((0x10, 28) in kinds, h264 == 'frame')
                result, d = self.parse(config, speed)
                self.assertEqual(result, 0, (speed, h264))
                self.assertEqual((d.Valid, d.Revision, d.StreamCount, d.FrameCount), (1, 0x150, 1, 9))
                got = [(d.Frame[i].Format, d.Frame[i].Kind, d.Frame[i].Width, d.Frame[i].FrameBytes,
                        d.Frame[i].IntervalCount) for i in range(9)]
                want = []
                for fmt, kind in ((1, 1), (2, 2), (3, 3)):
                    for index, (w, h) in enumerate(u.SIZES, 1):
                        f = cam.formats[speed][(fmt, index)]
                        want.append((fmt, kind, w, 0 if kind == 2 else f['frame_bytes'],
                                     0 if kind == 3 else len(f['rates'])))
                self.assertEqual(got, want)
                self.assertEqual(self.parse(cam.config['full' if speed == 'high' else 'high'],
                                            'full' if speed == 'high' else 'high')[0], 0)

    def test_uvc15_native_h264_parser(self):
        """A UVC 1.5 camera that offers VS_FORMAT_H264 (0x13) beside YUY2, as
        UVC 1.5 H.264 cameras do: the parser counts the format and skips it
        (uvcdesc.h). Until branch uvc15-fix (576fc46) the parser did not
        count 0x13/0x15/0x16/0x18 and refused the whole camera; this test
        fails against a DLL built from older uvcdesc.c."""
        for h264 in ('native', 'both'):
            cam = u.Camera('high', 0x150, h264=h264)
            kinds = self.subtypes(cam.config['high'])
            self.assertIn((0x13, 52), kinds)
            self.assertIn((0x14, 44 + 4 * 4), kinds)
            result, d = self.parse(cam.config['high'], 'high')
            self.assertEqual((result, d.FrameCount), (0, 9), h264)

    def test_uvc15_probe_commit(self):
        m, conn, log, _ = self.model(uvc=0x150)
        self.configure(m, conn)
        self.assertEqual(self.control(m, 0x85, 0xa1, 0x100, length=2), (0, struct.pack('<H', 48)))
        for req in (0x81, 0x82, 0x83, 0x87, 0x84):
            st, data = self.control(m, req, 0xa1, 0x100, length=48)
            self.assertEqual((st, len(data)), (0, 48))
        answer = self.negotiate(m, 1, 2, 333333)
        self.assertEqual(struct.unpack('<IBBBB', answer[26:34]), (u.CLOCK, 3, 1, 1, 1))
        self.assertEqual(struct.unpack('<BBBBHQ', answer[34:48]), (1, 8, 0, 1, 0, 0))
        # A 34-byte probe is the wrong length for a 1.5 camera.
        self.assertEqual(self.control(m, 1, 0x21, 0x100, payload=bytes(34))[0], 4)
        # NV12: continuous intervals keep an interval in range, clamp outside.
        for asked, kept in ((450000, 450000), (100000, 333333), (20000000, 10000000)):
            answer = self.negotiate(m, 3, 1, asked)
            iv, = struct.unpack('<I', answer[4:8])
            fs, = struct.unpack('<I', answer[18:22])
            self.assertEqual((iv, fs), (kept, 160 * 120 * 3 // 2))
        # The frame-based H.264 format negotiates too (its frame size is the
        # largest access unit).
        answer = self.negotiate(m, 4, 3, 333333)
        self.assertEqual(struct.unpack('<I', answer[18:22])[0], u.h264_max(640, 480))

    def test_uvc15_driver_negotiation(self):
        for speed in ('high', 'full'):
            m, conn, log, _ = self.model(speed, uvc=0x150)
            self.configure(m, conn)
            result, d = self.parse(m.cam.config[speed], speed)
            self.assertEqual(result, 0)
            for i in range(d.FrameCount):
                if d.Frame[i].Kind == 2:
                    continue
                rc, res = self.driver_negotiate(m, i, d)
                self.assertEqual((rc, res.Requests, res.FrameIndex), (0, 3, i), (speed, i))
                cur = self.control(m, 0x81, 0xa1, 0x200, length=48)[1]
                self.assertEqual(cur[34:36], b'\1\x08')        # 1.5 tail kept through COMMIT
                alt = d.Alt[res.AltIndex]
                self.assertGreaterEqual(alt.Capacity, res.Control.PayloadBytes)
            # The NV12 continuous range: an interval between listed rates.
            nv = next(i for i in range(d.FrameCount) if d.Frame[i].Kind == 3)
            rc, res = self.driver_negotiate(m, nv, d, interval=450000)
            self.assertEqual((rc, res.Control.Interval), (0, 450000))
        events = [json.loads(line) for line in log.getvalue().splitlines()]
        self.assertTrue(any(e['event'] == 'commit' and e['kind'] == 'nv12' for e in events))

    def test_uvc15_streams(self):
        self.check_stream('high', 1, 2, 0.3, uvc=0x150)
        m, frames, achieved, rate, payload, alt = self.check_stream('high', 3, 3, 0.5, uvc=0x150)
        self.assertEqual(len(frames[0]), 640 * 480 * 3 // 2)
        y = frames[0][:640 * 480]
        self.assertEqual(y[640 * 20 + 4], u.cell(0, 20, 1)[0])
        self.check_stream('full', 3, 1, 0.5, uvc=0x150)
        m, frames, *_ = self.check_stream('high', 4, 1, 0.3, uvc=0x150)
        self.assertTrue(all(f.startswith(b'\0\0\0\1\x09\xf0') for f in frames))

    # -- MJPEG content -----------------------------------------------------
    def test_jpeg_structure(self):
        for dht in (True, False):
            for w, h in u.SIZES:
                data = u.Jpeg(w, h, dht).frame(5)
                self.assertEqual(data.count(b'\xff\xc4') > 0, dht)
                markers = [data[i + 1] for i in range(len(data) - 1)
                           if data[i] == 0xff and data[i + 1] in range(0xd0, 0xd8)]
                self.assertEqual(len(markers), h // 8 - 1)
                self.assertEqual(markers[:9], [0xd0 + i % 8 for i in range(min(9, len(markers)))])

    @unittest.skipUnless(FFMPEG, 'FFmpeg not installed')
    def test_jpeg_decodes_in_ffmpeg(self):
        with tempfile.TemporaryDirectory() as tmp:
            for dht in (True, False):
                for w, h in u.SIZES:
                    seq = 0x5a5a5
                    path = Path(tmp) / f'f{w}{dht}.jpg'
                    path.write_bytes(u.Jpeg(w, h, dht).frame(seq))
                    out = subprocess.run([FFMPEG, '-v', 'error', '-i', str(path), '-f', 'rawvideo',
                                          '-pix_fmt', 'yuv422p', '-'], capture_output=True, check=True,
                                         timeout=60).stdout
                    self.assertEqual(len(out), w * h * 2, (w, dht))
                    luma = out[:w * h]
                    for row in (3, 20, h - 1):
                        for x16 in range(w // 16):
                            y = u.cell(x16, row, seq)[0]
                            got = luma[row * w + x16 * 16 + 4]
                            self.assertLessEqual(abs(got - y), 2, (w, dht, row, x16, got, y))


if __name__ == '__main__':
    unittest.main()
