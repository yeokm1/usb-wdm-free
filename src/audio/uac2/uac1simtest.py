#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests of the UAC 1.0 usbredir model (uac1emu.py) and every one of
its profiles; no physical USB. Run: python -B uac1simtest.py
"""
import io
import json
import math
import socket
import struct
import subprocess
import sys
import threading
import unittest
from pathlib import Path

import uac1emu
import uacemu


class Wire:
    def __init__(self):
        self.messages = []

    def sendall(self, raw):
        kind, length, ident = struct.unpack('<III', raw[:12])
        if length != len(raw) - 12:
            raise AssertionError('wire framing')
        self.messages.append((kind, ident, raw[12:]))


def walk(config):
    """Every descriptor of a configuration, framing checked."""
    off, out = 0, []
    while off < len(config):
        size = config[off]
        if size < 2 or off + size > len(config):
            raise AssertionError('descriptor framing')
        out.append(config[off:off + size])
        off += size
    return out


class Base(unittest.TestCase):
    profile = 'headset'
    kw = {}

    def setUp(self):
        self.time = 100.0
        self.wire, self.trace = Wire(), io.StringIO()
        self.p = uac1emu.Profile1(self.profile, **self.kw)
        self.model = uac1emu.Model1(self.wire, self.trace, lambda: self.time, profile=self.p)
        self.model.peer_caps = uacemu.CAPS

    def control(self, request, rt, value, index, length, payload=b''):
        self.model.handle(100, 777, struct.pack('<BBBBHHH', 0, request, rt, 0, value, index, length) + payload)
        kind, ident, data = self.wire.messages[-1]
        self.assertEqual((kind, ident), (100, 777))
        header = struct.unpack('<BBBBHHH', data[:10])
        return header[3], header[-1], data[10:]

    def configure(self, value=1):
        self.model.handle(6, 1, bytes((value,)))
        self.assertEqual(self.wire.messages[-1], (8, 1, bytes((0, value))))

    def alt(self, interface, alternate):
        self.model.handle(9, 2, bytes((interface, alternate)))
        self.assertEqual(self.wire.messages[-1], (11, 2, bytes((0, interface, alternate))))

    def events(self, name):
        return [e for e in map(json.loads, self.trace.getvalue().splitlines()) if e['event'] == name]

    def out(self, ep, frames, frame_bytes, count=1, step=0.001):
        for _ in range(count):
            data = struct.pack('<%dh' % (frames * frame_bytes // 2), *([1000] * (frames * frame_bytes // 2))) \
                if frame_bytes % 2 == 0 else bytes(frames * frame_bytes)
            self.model.handle(102, 0, struct.pack('<BBH', ep, 0, len(data)) + data)
            self.time += step


BUILT = [n for n in uac1emu.PROFILE_NAMES if n not in uac1emu.REPLAY]
TAGS = {'pcm8': 2, 'float': 3, 'alaw': 4, 'mulaw': 5, 'pcm8in': 2, 'floatin': 3, 'mpoout': 2}


class DescriptorTest(unittest.TestCase):
    def test_every_profile_is_well_formed_uac1(self):
        for name in BUILT:
            with self.subTest(profile=name):
                p = uac1emu.Profile1(name)
                descriptors = walk(p.config)
                self.assertEqual(int.from_bytes(p.config[2:4], 'little'), len(p.config))
                self.assertEqual(p.config[4], len(p.interfaces))
                self.assertEqual(p.device[13] if False else p.device[17], 2 if name == 'dual' else 1)
                header = [d for d in descriptors if d[1] == 0x24 and d[2] == 1 and len(d) >= 8 and
                          d[3:5] == b'\x00\x01']
                self.assertEqual(len(header), 1)
                h = header[0]
                ac_len = int.from_bytes(h[5:7], 'little')
                # wTotalLength of the class-specific AC descriptors (4.3.2).
                start = p.config.index(h)
                total, off = 0, start
                while off < len(p.config) and p.config[off + 1] == 0x24:
                    total += p.config[off]
                    off += p.config[off]
                self.assertEqual(ac_len, total)
                self.assertEqual(list(h[8:8 + h[7]]), sorted({i for i, a in p.alts}))
                # Every AS alternate: general, Type I, a 9-byte endpoint and its class part.
                for (i, a), alt in p.alts.items():
                    info = p.stream_alt(i, a)
                    self.assertIsNotNone(info)
                    self.assertEqual(alt['format_tag'], TAGS.get(name, 1))
                    data = info['data']
                    self.assertEqual(data['attributes'] & 3, 1)
                    self.assertEqual(data['interval'], 4 if p.speed == 'high' else 1)
                    top = alt['rates'][1] if isinstance(alt['rates'], tuple) else max(alt['rates'])
                    self.assertGreaterEqual(data['max'], (top // 1000 + 1) * info['frame'])
                    self.assertEqual(data['cs_attributes'] & 1, 1)
                    if data['attributes'] & 0x0c == 0x04 and not data['address'] & 0x80:
                        self.assertEqual(info['feedback']['address'], data['synch'])
                        self.assertEqual(info['feedback']['max'], 3)
                    elif data['attributes'] & 0x0c == 0x08 and data['address'] & 0x80:
                        # An adaptive source: its sync endpoint is OUT.
                        self.assertEqual(info['feedback']['address'], data['synch'])
                        self.assertFalse(data['synch'] & 0x80)
                    else:
                        self.assertIsNone(info['feedback'])
                for d in descriptors:
                    if d[1] == 5:
                        self.assertEqual(len(d), 9 if d[3] & 3 == 1 else 7)
                self.assertIsNone(p.qualifier) if p.speed == 'full' else self.assertEqual(len(p.qualifier), 10)
                self.assertEqual(p.device[2:4], (b'\x00\x02' if p.speed == 'high' else b'\x10\x01'))

    def test_feature_units_cover_their_cluster(self):
        # UAC 1.0 4.3.2.5: bmaControls(0) master, then one per channel of the
        # cluster the unit takes.
        for name in BUILT:
            p = uac1emu.Profile1(name)
            m = uac1emu.Model1(Wire(), io.StringIO(), profile=p)
            for uid, e in p.entities.items():
                if e['kind'] == 'fu':
                    with self.subTest(profile=name, unit=uid):
                        self.assertEqual(e['channels'], m.cluster(e['source']))

    def test_speed_override_and_refusals(self):
        p = uac1emu.Profile1('headset', speed='high')
        self.assertEqual(p.speed, 'high')
        self.assertTrue(all(p.stream_alt(i, a)['data']['interval'] == 4 for i, a in p.alts))
        for bad in (dict(name='nope'), dict(name='hs', speed='full'), dict(name='async', refresh=0),
                    dict(name='async', sync_attr=0x02)):
            with self.subTest(**bad), self.assertRaises(ValueError):
                uac1emu.Profile1(**bad)

    def test_continuous_and_discrete_tables(self):
        self.assertEqual(uac1emu.Profile1('dacc').alts[(1, 1)]['rates'], (8000, 48000))
        self.assertEqual(uac1emu.Profile1('dacd').alts[(1, 1)]['rates'], [32000, 44100, 48000])
        hs = uac1emu.Profile1('hs')
        self.assertEqual((hs.alts[(1, 2)]['subslot'], hs.alts[(1, 2)]['bits']), (3, 24))
        self.assertEqual(hs.stream_alt(1, 2)['data']['max'], 97 * 6)

    def test_cammic_shape(self):
        p = uac1emu.Profile1('cammic')
        self.assertEqual(p.device[4:7], b'\xef\x02\x01')
        d = walk(p.config)
        self.assertEqual(d[1][1], 4)                         # interface 0 first: the vendor stand-in
        self.assertEqual((d[1][2], d[1][5]), (0, 0xff))
        self.assertEqual(d[2][:5], bytes((8, 0x0b, 1, 2, 1)))  # IAD over interfaces 1-2
        self.assertEqual(p.ac, 1)

    def test_headset_hid(self):
        p = uac1emu.Profile1('headset')
        self.assertEqual(p.hid, {3: len(uac1emu.HID_CONSUMER)})
        self.assertEqual(p.layout[(3, 0)][1], [(0x83, 3, 10, 4)])


class HeadsetTest(Base):
    profile = 'headset'

    def test_feature_unit_volume_mute_agc(self):
        self.assertEqual(self.control(0x81, 0xa1, 0x0200, 0x0500, 2)[0], 4)     # unconfigured
        self.configure()
        # FU 5: master mute only, channel volume.
        self.assertEqual(self.control(0x81, 0xa1, 0x0100, 0x0500, 1), (0, 1, b'\0'))
        self.assertEqual(self.control(0x81, 0xa1, 0x0200, 0x0500, 2)[0], 4)
        self.assertEqual(self.control(0x82, 0xa1, 0x0201, 0x0500, 2)[2], struct.pack('<h', -11520))
        self.assertEqual(self.control(0x83, 0xa1, 0x0202, 0x0500, 2)[2], struct.pack('<h', 0))
        self.assertEqual(self.control(0x84, 0xa1, 0x0201, 0x0500, 2)[2], struct.pack('<h', 384))
        self.assertEqual(self.control(0x81, 0xa1, 0x0201, 0x0500, 2)[2], struct.pack('<h', -3840))
        self.assertEqual(self.control(0x01, 0x21, 0x0201, 0x0500, 2, struct.pack('<h', -768))[0], 0)
        self.assertEqual(self.control(0x81, 0xa1, 0x0201, 0x0500, 2)[2], struct.pack('<h', -768))
        # Between steps: kept at the nearest step; out of range: clamped.
        self.assertEqual(self.control(0x01, 0x21, 0x0202, 0x0500, 2, struct.pack('<h', -700))[0], 0)
        self.assertEqual(self.control(0x81, 0xa1, 0x0202, 0x0500, 2)[2], struct.pack('<h', -768))
        self.assertEqual(self.control(0x01, 0x21, 0x0202, 0x0500, 2, struct.pack('<h', 900))[0], 0)
        self.assertEqual(self.control(0x81, 0xa1, 0x0202, 0x0500, 2)[2], struct.pack('<h', 0))
        self.assertEqual(len(self.events('feature_adjusted')), 2)
        self.assertEqual(self.control(0x01, 0x21, 0x0100, 0x0500, 1, b'\1')[0], 0)
        self.assertEqual(self.model.channel_gain([5], 1), (-768 / 256, True))
        self.assertEqual(self.control(0x82, 0xa1, 0x0100, 0x0500, 1)[0], 4)     # mute: CUR only
        self.assertEqual(self.control(0x01, 0x21, 0x0100, 0x0500, 1, b'\2')[0], 4)
        self.assertEqual(self.control(0x01, 0x21, 0x0100, 0x0500, 2, b'\1\0')[0], 4)
        # FU 7 (capture): AGC.
        self.assertEqual(self.control(0x81, 0xa1, 0x0700, 0x0700, 1), (0, 1, b'\0'))
        self.assertEqual(self.control(0x01, 0x21, 0x0700, 0x0700, 1, b'\1')[0], 0)
        self.assertEqual(self.control(0x81, 0xa1, 0x0700, 0x0700, 1)[2], b'\1')
        self.assertEqual(self.control(0x81, 0xa1, 0x0300, 0x0700, 1)[0], 4)     # no bass
        self.assertEqual(self.control(0x81, 0xa1, 0x0201, 0x0700, 2)[0], 4)     # mono: channel 1 none
        self.assertEqual(self.control(0x81, 0xa1, 0x0200, 0x0900, 2)[0], 4)     # no entity 9
        self.assertEqual(self.control(0x02, 0x21, 0x0200, 0x0700, 2, b'\0\0')[0], 4)   # SET_MIN
        self.assertEqual(self.control(0xff, 0xa1, 0x0000, 0x0700, 1)[0], 4)     # GET_STAT
        self.assertEqual(self.control(0x81, 0xa1, 0x02ff, 0x0500, 4)[0], 4)     # all channels

    def test_strict_volume(self):
        self.p.strict_volume = True
        self.configure()
        self.assertEqual(self.control(0x01, 0x21, 0x0201, 0x0500, 2, struct.pack('<h', -700))[0], 4)
        self.assertEqual(self.control(0x01, 0x21, 0x0201, 0x0500, 2, struct.pack('<h', -32768))[0], 0)
        self.assertEqual(self.model.channel_gain([5], 1)[1], True)

    def test_mixer_sidetone(self):
        self.configure()
        # Inputs 1-2 the stream, 3 the sidetone; outputs 1-2.
        self.assertEqual(self.control(0x81, 0xa1, 0x0101, 0x0400, 2)[2], struct.pack('<h', 0))
        self.assertEqual(self.control(0x81, 0xa1, 0x0102, 0x0400, 2)[2], struct.pack('<h', -32768))
        self.assertEqual(self.control(0x81, 0xa1, 0x0301, 0x0400, 2)[2], struct.pack('<h', -32768))
        self.assertEqual(self.control(0x82, 0xa1, 0x0301, 0x0400, 2)[2], struct.pack('<h', -11520))
        self.assertEqual(self.control(0x01, 0x21, 0x0302, 0x0400, 2, struct.pack('<h', -3840))[0], 0)
        self.assertEqual(self.control(0x81, 0xa1, 0x0302, 0x0400, 2)[2], struct.pack('<h', -3840))
        self.assertEqual(self.control(0x01, 0x21, 0x0101, 0x0400, 2, struct.pack('<h', -256))[0], 4)
        self.assertEqual(self.control(0x81, 0xa1, 0x0401, 0x0400, 2)[0], 4)
        self.assertEqual(self.control(0x81, 0xa1, 0x0103, 0x0400, 2)[0], 4)

    def test_endpoint_sampling_frequency(self):
        self.configure()
        self.assertEqual(self.control(0x81, 0xa2, 0x0100, 0x0001, 3)[2], uac1emu.u24(48000))
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(44100))[0], 0)
        self.assertEqual(self.control(0x81, 0xa2, 0x0100, 0x0001, 3)[2], uac1emu.u24(44100))
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(32000))[0], 4)
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0082, 3, uac1emu.u24(16000))[0], 0)
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(16000))[0], 4)
        self.assertEqual(self.control(0x82, 0xa2, 0x0100, 0x0001, 3)[0], 4)     # discrete: CUR only
        self.assertEqual(self.control(0x81, 0xa2, 0x0200, 0x0001, 1)[0], 4)     # no pitch
        self.assertEqual(self.control(0x81, 0xa2, 0x0100, 0x0005, 3)[0], 4)     # no endpoint 5
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0001, 2, b'\0\0')[0], 4)
        self.assertEqual(self.events('ep_rate')[0]['rate'], 44100)
        self.assertEqual(len(self.events('ep_rate_refused')), 2)

    def test_hid_answers_its_report(self):
        self.configure()
        status, actual, data = self.control(6, 0x81, 0x2200, 3, 255)
        self.assertEqual((status, data), (0, uac1emu.HID_CONSUMER))
        self.assertEqual(self.control(6, 0x81, 0x2100, 3, 9)[2][:2], b'\x09\x21')
        self.assertEqual(self.control(0x0a, 0x21, 0, 3, 0)[0], 0)              # SET_IDLE

    def test_playback_validation_44100(self):
        self.configure()
        self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(44100))
        self.alt(1, 1)
        self.model.handle(12, 3, bytes((1, 8, 4)))
        self.out(1, 0, 4, count=3)                       # priming zero-length packets
        for _ in range(100):                             # nine 44 and one 45: 44.1 a packet
            for n in [44] * 9 + [45]:
                self.out(1, n, 4)
        s = self.model.ostate[1]
        self.assertEqual((s['packets'], s['frames'], s['lead_zero']), (1000, 44100, 3))
        self.assertEqual((s['outside'], s['under'], s['over'], s['zero']), (0, 0, 0, 0))
        self.model.handle(13, 4, b'\x01')
        verdict = self.events('ostream_verdict')[-1]
        self.assertTrue(verdict['ok'])
        self.assertEqual(verdict['frames_per_packet'], {'44': 900, '45': 100})
        self.assertEqual(verdict['drift_frames'], 0)

    def test_playback_short_driver_underruns(self):
        self.configure()
        self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(44100))
        self.alt(1, 1)
        self.model.handle(12, 3, bytes((1, 8, 4)))
        self.out(1, 44, 4, count=2000)                   # never the 45th frame
        s = self.model.ostate[1]
        self.assertGreater(s['under'], 0)
        self.assertEqual(s['over'], 0)
        self.out(1, 47, 4, count=1)                      # three frames over: outside +-1
        self.out(1, 0, 4, count=1)
        self.model.handle(102, 0, struct.pack('<BBH', 1, 0, 6) + bytes(6))   # not whole frames
        self.model.handle(102, 0, struct.pack('<BBH', 1, 0, 200) + bytes(200))   # over wMaxPacketSize
        self.model.handle(13, 4, b'\x01')
        v = self.events('ostream_verdict')[-1]
        self.assertFalse(v['ok'])
        self.assertEqual((v['outside_pm1'], v['zero_length'], v['misframed']), (2, 1, 2))
        self.assertEqual(len(self.events('ostream_misframed')), 2)
        self.assertLess(v['drift_frames'], -190)

    def test_overrun(self):
        self.configure()
        self.alt(1, 1)
        self.model.handle(12, 3, bytes((1, 8, 4)))
        self.out(1, 49, 4, count=200)                    # one frame a packet too many at 48 kHz
        self.assertGreater(self.model.ostate[1]['over'], 0)

    def test_capture_mono_rate(self):
        self.configure()
        self.control(0x01, 0x22, 0x0100, 0x0082, 3, uac1emu.u24(16000))
        self.alt(2, 1)
        self.model.handle(12, 3, bytes((0x82, 8, 4)))
        for _ in range(10):
            self.model.capture()
            self.time += 0.001
        packets = [m for m in self.wire.messages if m[0] == 102]
        self.assertEqual(len(packets), 10)
        self.assertTrue(all(struct.unpack('<BBH', m[2][:4])[2] == 32 for m in packets))
        self.assertTrue(any(m[2][4:] != bytes(32) for m in packets))

    def test_rendered_level_follows_units(self):
        self.configure()
        self.alt(1, 1)
        self.model.handle(12, 3, bytes((1, 8, 4)))
        self.control(0x01, 0x21, 0x0201, 0x0500, 2, struct.pack('<h', -1536))
        self.out(1, 48, 4, count=5)
        levels = self.model.levels()
        self.assertEqual(levels['units'], [5])
        self.assertAlmostEqual(levels['unit_gain_db'][0], -6.0)
        self.assertAlmostEqual(levels['rendered_peak_dbfs'][0] - levels['received_peak_dbfs'][0], -6.0, 1)

    def test_reset_and_configuration_reads(self):
        self.configure()
        self.assertEqual(self.control(8, 0x80, 0, 0, 1)[2], b'\1')
        self.model.handle(3, 0, b'')
        self.assertEqual(self.model.config, 0)
        self.assertEqual(self.control(8, 0x80, 0, 0, 1)[2], b'\0')
        self.model.handle(6, 5, b'\x02')
        self.assertEqual(self.wire.messages[-1], (8, 5, b'\4\0'))
        # Full Speed: no device qualifier.
        self.assertEqual(self.control(6, 0x80, 0x0600, 0, 10)[0], 4)


class DacTest(Base):
    profile = 'dacc'

    def test_continuous_range(self):
        self.configure()
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(22050))[0], 0)
        self.assertEqual(self.control(0x81, 0xa2, 0x0100, 0x0001, 3)[2], uac1emu.u24(22050))
        self.assertEqual(self.control(0x82, 0xa2, 0x0100, 0x0001, 3)[2], uac1emu.u24(8000))
        self.assertEqual(self.control(0x83, 0xa2, 0x0100, 0x0001, 3)[2], uac1emu.u24(48000))
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(96000))[0], 4)
        # No master, no mute: per-channel volume only.
        self.assertEqual(self.control(0x81, 0xa1, 0x0200, 0x0200, 2)[0], 4)
        self.assertEqual(self.control(0x81, 0xa1, 0x0101, 0x0200, 1)[0], 4)
        self.assertEqual(self.control(0x81, 0xa1, 0x0202, 0x0200, 2)[2], struct.pack('<h', -2560))
        self.alt(1, 1)
        self.model.handle(12, 3, bytes((1, 8, 4)))
        for _ in range(40):
            self.out(1, 22, 4)
            self.out(1, 22, 4)
            self.out(1, 22, 4)
            self.out(1, 22, 4)
            self.out(1, 23, 4)                       # 22.05 is 22 x 19 + 23 in 20
        s = self.model.ostate[1]
        self.assertEqual(s['outside'], 0)
        self.assertAlmostEqual(s['nominal'] if 'nominal' in s else self.model.nominal(s['info']), 22.05)


class DacdTest(Base):
    profile = 'dacd'

    def test_discrete_and_master(self):
        self.configure()
        for rate, status in ((32000, 0), (44100, 0), (48000, 0), (22050, 4), (96000, 4)):
            with self.subTest(rate=rate):
                self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(rate))[0], status)
        self.assertEqual(self.control(0x83, 0xa2, 0x0100, 0x0001, 3)[0], 4)
        self.assertEqual(self.control(0x81, 0xa1, 0x0100, 0x0200, 1)[2], b'\0')
        self.assertEqual(self.control(0x81, 0xa1, 0x0200, 0x0200, 2)[2], struct.pack('<h', -2560))
        self.assertEqual(self.control(0x81, 0xa1, 0x0201, 0x0200, 2)[0], 4)


class AsyncTest(Base):
    profile = 'async'

    def test_feedback_10_14_at_brefresh(self):
        d = [x for x in walk(self.p.config) if x[1] == 5]
        self.assertEqual(d[0][3] & 0x0c, 0x04)
        self.assertEqual(d[0][8], 0x81)                  # bSynchAddress
        self.assertEqual((d[1][2], d[1][3], d[1][4], d[1][7]), (0x81, 0x01, 3, 2))   # bRefresh 2
        self.model.fb_at_refresh = True
        self.configure()
        self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(44100))
        self.alt(1, 1)
        self.model.handle(12, 3, bytes((0x81, 8, 4)))
        self.model.handle(12, 4, bytes((1, 8, 4)))
        self.model.feedback()
        fb = [m for m in self.wire.messages if m[0] == 102]
        self.assertEqual(fb[-1][2], struct.pack('<BBH', 0x81, 0, 3) + struct.pack('<I', round(44.1 * 16384))[:3])
        before = len(fb)
        self.time += 0.003
        self.model.feedback()
        self.assertEqual(len([m for m in self.wire.messages if m[0] == 102]), before)
        self.time += 0.0011
        self.model.feedback()
        self.assertEqual(len([m for m in self.wire.messages if m[0] == 102]), before + 1)

    def test_feedback_every_frame_and_ppm(self):
        self.model.fb_ppm = 10000.0
        self.configure()
        self.alt(1, 2)
        self.model.handle(12, 3, bytes((0x81, 8, 4)))
        self.model.handle(12, 4, bytes((1, 8, 4)))
        for _ in range(4):
            self.model.feedback()
            self.time += 0.001
        fb = [m for m in self.wire.messages if m[0] == 102]
        self.assertEqual(len(fb), 4)
        self.assertEqual(fb[0][2][4:], struct.pack('<I', round(48.48 * 16384))[:3])
        # 24-bit: frames of 6 bytes; the nominal follows the fed-back rate.
        for n in [48] * 50 + [49] * 50:
            self.out(1, n, 6)
        s = self.model.ostate[1]
        self.assertAlmostEqual(self.model.nominal(s['info']), 48.48)
        self.assertEqual((s['outside'], s['misframed']), (0, 0))

    def test_two_byte_controls(self):
        self.configure()
        self.assertEqual(self.control(0x82, 0xa1, 0x0300, 0x0200, 1)[2], struct.pack('<b', -128))
        self.assertEqual(self.control(0x01, 0x21, 0x0500, 0x0200, 1, struct.pack('<b', 12))[0], 0)
        self.assertEqual(self.control(0x81, 0xa1, 0x0500, 0x0200, 1)[2], struct.pack('<b', 12))
        self.assertEqual(self.control(0x01, 0x21, 0x0a00, 0x0200, 1, b'\1')[0], 0)
        self.assertEqual(self.control(0x81, 0xa1, 0x0400, 0x0200, 1)[0], 4)      # no mid
        self.assertEqual(self.control(0x84, 0xa1, 0x0200, 0x0200, 2)[2], struct.pack('<h', 128))
        self.assertEqual(self.control(0x83, 0xa1, 0x0200, 0x0200, 2)[2], struct.pack('<h', 1536))

    def test_sync_attr_option(self):
        p = uac1emu.Profile1('async', sync_attr=0x11, refresh=5)
        fb = p.stream_alt(1, 1)['feedback']
        self.assertEqual((fb['attributes'], fb['refresh']), (0x11, 5))


class HighSpeedTest(Base):
    profile = 'hs'

    def test_24bit_96k(self):
        self.configure()
        self.assertEqual(self.control(6, 0x80, 0x0600, 0, 10)[0], 0)
        self.alt(1, 2)
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(96000))[0], 0)
        self.model.handle(12, 3, bytes((1, 8, 4)))
        info = self.wire.messages
        self.out(1, 96, 6, count=100)
        s = self.model.ostate[1]
        self.assertEqual((s['frames'], s['outside'], s['under'], s['over']), (9600, 0, 0, 0))
        self.assertEqual(self.model.period(s['info']['data']), 0.001)
        self.alt(2, 1)
        self.control(0x01, 0x22, 0x0100, 0x0082, 3, uac1emu.u24(96000))
        self.model.handle(12, 5, bytes((0x82, 8, 4)))
        self.model.capture()
        last = [m for m in info if m[0] == 102][-1][2]
        self.assertEqual(struct.unpack('<BBH', last[:4]), (0x82, 0, 96 * 6))


class MicTest(Base):
    profile = 'mic'

    def test_capture_only(self):
        self.configure()
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0081, 3, uac1emu.u24(32000))[0], 0)
        self.assertEqual(self.control(0x01, 0x21, 0x0700, 0x0200, 1, b'\1')[0], 0)
        self.assertEqual(self.control(0x81, 0xa1, 0x0200, 0x0200, 2)[2], struct.pack('<h', 0))
        self.alt(1, 1)
        self.model.handle(12, 3, bytes((0x81, 8, 4)))
        self.model.capture()
        last = [m for m in self.wire.messages if m[0] == 102][-1][2]
        self.assertEqual(struct.unpack('<BBH', last[:4]), (0x81, 0, 64))


class CammicTest(Base):
    profile = 'cammic'

    def test_requests_reach_interface_1(self):
        self.configure()
        self.assertEqual(self.control(0x81, 0xa1, 0x0200, 0x0201, 2)[2], struct.pack('<h', 3072))
        self.assertEqual(self.control(0x81, 0xa1, 0x0200, 0x0200, 2)[0], 4)   # interface 0 is not audio
        self.alt(2, 1)
        self.model.handle(12, 3, bytes((0x81, 8, 4)))
        self.model.capture()
        last = [m for m in self.wire.messages if m[0] == 102][-1][2]
        self.assertEqual(struct.unpack('<BBH', last[:4]), (0x81, 0, 96))


class DualTest(Base):
    profile = 'dual'

    def test_two_configurations(self):
        self.assertEqual(self.p.config[5], 1)
        self.assertEqual(self.p.config2[5], 2)
        self.assertEqual(self.control(6, 0x80, 0x0201, 0, 4096)[2], self.p.config2)
        self.configure(1)
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(44100))[0], 0)
        self.assertEqual(self.control(8, 0x80, 0, 0, 1)[2], b'\1')
        self.configure(2)
        self.assertEqual(self.control(8, 0x80, 0, 0, 1)[2], b'\2')
        # UAC 2.0 now: the hs24 clock source 0x10 answers, the UAC 1.0 endpoint request stalls.
        self.assertEqual(self.control(2, 0xa1, 0x100, 0x1000, 2)[0], 0)
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(44100))[0], 4)
        self.alt(1, 1)
        self.model.handle(12, 3, bytes((1, 8, 4)))
        self.assertEqual(self.wire.messages[-1][2], b'\0\1')
        self.configure(1)
        self.assertEqual(self.control(0x81, 0xa2, 0x0100, 0x0001, 3)[2], uac1emu.u24(48000))
        self.assertEqual([e['value'] for e in self.events('set_configuration')], [1, 2, 1])


class Play2Test(Base):
    profile = 'play2'

    def test_replayed_identity(self):
        self.assertEqual(self.p.config, (uac1emu.TESTDATA / 'play2/config0.bin').read_bytes())
        self.assertEqual(len(self.p.config), 273)
        self.assertEqual(self.p.device[8:12], b'\x1e\x04\x3d\x32')
        self.assertEqual(self.p.device[16], 0)                          # serial index zero
        self.assertEqual(self.control(6, 0x80, 0x0303, 0x409, 255)[0], 4)
        self.assertIn('SIMULATED', self.control(6, 0x80, 0x0302, 0x409, 255)[2][2:].decode('utf-16le'))
        self.assertEqual(self.control(6, 0x80, 0x0600, 0, 10)[0], 4)   # qualifier: stall, as the device
        self.assertEqual(self.control(6, 0x80, 0x03ee, 0, 18)[0], 4)   # OS string: stall, as the device

    def test_answers_as_captured(self):
        self.configure()
        # FU 6 (capture) master volume: F100 / 0F00 / 00C0 / 0000.
        self.assertEqual([self.control(r, 0xa1, 0x0200, 0x0600, 2)[2] for r in (0x82, 0x83, 0x84, 0x81)],
                         [b'\x00\xf1', b'\x00\x0f', b'\xc0\x00', b'\x00\x00'])
        # FU 5 (speaker) channel volume: CC00 / 0000 / 0100 / F800; master mute.
        self.assertEqual([self.control(r, 0xa1, 0x0201, 0x0500, 2)[2] for r in (0x82, 0x83, 0x84, 0x81)],
                         [b'\x00\xcc', b'\x00\x00', b'\x00\x01', b'\x00\xf8'])
        self.assertEqual(self.control(0x81, 0xa1, 0x0100, 0x0500, 1)[2], b'\0')
        self.assertEqual(self.control(0x81, 0xa1, 0x0000, 0x0a00, 1)[2], b'\1')   # selector 10
        self.assertEqual(self.control(0x81, 0xa1, 0x0000, 0x0c00, 1)[0], 4)        # selector 12 stalls
        self.assertEqual(self.control(0x81, 0xa1, 0x0101, 0x0900, 2)[0], 4)        # mixer 9 stalls
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(48000))[0], 0)
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0081, 3, uac1emu.u24(44100))[0], 0)
        self.assertEqual(self.control(6, 0x81, 0x2200, 3, 0xad)[2],
                         (uac1emu.TESTDATA / 'play2/hidreport.bin').read_bytes())
        self.assertEqual(self.control(0x09, 0x21, 0x0202, 3, 2, b'\x02\x00')[0], 0)  # SET_REPORT, as 10 sent
        self.assertEqual(len(self.events('unit_stalled')), 2)

    def feedback_run(self, frames):
        self.configure()
        self.alt(2, 1)
        self.model.handle(12, 3, bytes((0x83, 8, 4)))
        self.model.handle(12, 4, bytes((1, 8, 4)))
        for _ in range(frames):
            self.model.feedback()
            self.time += 0.001
        return [m[2][4:] for m in self.wire.messages if m[0] == 102]

    def test_feedback_every_frame_by_default(self):
        fb = self.p.stream_alt(2, 1)['feedback']
        self.assertEqual((fb['address'], fb['attributes'], fb['max'], fb['refresh']), (0x83, 0x01, 3, 5))
        sent = self.feedback_run(64)
        self.assertEqual(len(sent), 64)
        self.assertEqual(set(sent), {b'\x00\x00\x0c'})                  # 48.0 in 10.14

    def test_refresh_override_is_a_rig_option(self):
        p = uac1emu.Profile1('play2', refresh=3)
        self.assertEqual(p.stream_alt(2, 1)['feedback']['refresh'], 3)
        diff = [i for i, (a, b) in enumerate(zip(p.config, self.p.config)) if a != b]
        self.assertEqual(len(diff), 1)                                  # only that byte
        self.assertEqual(self.p.stream_alt(2, 1)['feedback']['refresh'], 5)

    def test_feedback_at_refresh_and_lead_empty(self):
        self.model.fb_at_refresh = True
        self.model.fb_lead_empty = 1
        sent = self.feedback_run(64)
        self.assertEqual(sent, [b'', b'\x00\x00\x0c'])                  # every 32 ms, the first empty
        self.assertEqual(self.model.lead_empty_sent, 1)


class LogiTest(Base):
    profile = 'logi'

    def test_replayed_quirks(self):
        self.assertEqual(self.p.config, (uac1emu.TESTDATA / 'logi0a64/CONFIG.BIN').read_bytes())
        self.assertEqual(self.p.device[4:7], b'\xef\x02\x01')           # IAD class codes, no IAD
        self.assertEqual(self.p.entities[4]['channels'], 1)             # input terminal: 1 channel
        self.assertEqual(self.p.alts[(2, 1)]['channels'], 2)            # its format: 2
        self.assertEqual(self.p.entities[2]['controls'], [0x43])        # mono unit: master only
        self.assertEqual(self.p.alts[(2, 1)]['rates'][-1], 192000)
        self.assertEqual(self.p.stream_alt(2, 1)['data']['max'], 768)
        self.configure()
        self.assertEqual(self.control(6, 0x80, 0x0306, 0x409, 255)[2][2:].decode('utf-16le'), 'Sidetone')
        self.assertEqual(self.control(0x81, 0xa1, 0x0200, 0x0200, 2)[2], struct.pack('<h', -3838))
        self.assertEqual(self.control(0x84, 0xa1, 0x0200, 0x0b00, 2)[2], struct.pack('<h', 384))
        self.assertEqual(self.control(0x83, 0xa1, 0x0200, 0x0b00, 2)[2], struct.pack('<h', -3072))
        self.assertEqual(self.control(0x81, 0xa1, 0x0700, 0x0200, 1)[2], b'\0')
        self.assertEqual(self.control(0x81, 0xa1, 0x0101, 0x0c00, 2)[0], 4)
        self.assertEqual(len(self.control(6, 0x81, 0x2200, 3, 206)[2]), 142)

    def test_192k_fills_the_packet(self):
        self.configure()
        self.alt(2, 1)
        self.assertEqual(self.control(0x01, 0x22, 0x0100, 0x0001, 3, uac1emu.u24(192000))[0], 0)
        self.model.handle(12, 3, bytes((1, 8, 4)))
        self.out(1, 192, 4, count=10)
        self.out(1, 193, 4, count=1)                                    # 772 bytes: over wMaxPacketSize
        s = self.model.ostate[1]
        self.assertEqual((s['packets'], s['misframed'], s['outside']), (10, 1, 0))


class AliasTest(unittest.TestCase):
    def test_uac1_plan_names(self):
        for alias, name in uac1emu.ALIASES.items():
            self.assertEqual(uac1emu.Profile1(alias).name, name)

    def test_cli_takes_the_aliases(self):
        # mic1 (cammic) is High Speed only: the refusal comes from the
        # profile, so the alias passed the command line's choices.
        script = str(Path(uac1emu.__file__).resolve())
        run = subprocess.run([sys.executable, '-I', '-B', script, '--log', 'unused', '--profile', 'mic1',
                              '--speed', 'full'], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 2)
        self.assertIn('High Speed only', run.stderr)


class HighSpeedAsyncTest(Base):
    profile = 'async'
    kw = dict(speed='high')

    def test_feedback_four_bytes(self):
        fb = self.p.stream_alt(1, 1)['feedback']
        self.assertEqual((fb['max'], fb['interval']), (4, 4))
        self.configure()
        self.alt(1, 1)
        self.model.handle(12, 3, bytes((0x81, 8, 4)))
        self.model.feedback()
        sent = [m for m in self.wire.messages if m[0] == 102]
        self.assertEqual(sent[-1][2], struct.pack('<BBHI', 0x81, 0, 4, 6 << 16))   # 6.0 a microframe


class DualDescriptorTest(Base):
    profile = 'dual'

    def test_device_descriptors_survive_configuration_2(self):
        before = [self.control(6, 0x80, v, 0, 4096)[2] for v in (0x0100, 0x0200, 0x0201, 0x0600, 0x0302)]
        self.configure(2)
        after = [self.control(6, 0x80, v, 0, 4096)[2] for v in (0x0100, 0x0200, 0x0201, 0x0600, 0x0302)]
        self.assertEqual(before, after)
        self.assertEqual(after[0][17], 2)
        self.assertEqual((after[1][5], after[2][5]), (1, 2))


class NegativeFormatTest(Base):
    """uac1's G3 fixtures of 0.1.0.1 (formats it refused; run since
    docs/design/uac2.md 26.12)."""
    profile = 'pcm8'

    def general_and_format(self, p):
        d = walk(p.config)
        general = [x for x in d if x[1] == 0x24 and x[2] == 1 and len(x) == 7]
        fmt = [x for x in d if x[1] == 0x24 and x[2] == 2 and len(x) >= 8 and x[3] == 1]
        return general[0], fmt[-1]          # the AS format

    def test_pcm8(self):
        general, fmt = self.general_and_format(self.p)
        self.assertEqual(struct.unpack('<H', general[5:7])[0], 2)            # wFormatTag PCM8
        self.assertEqual((fmt[4], fmt[5], fmt[6]), (2, 1, 8))               # 2 channels, 1 byte, 8 bits
        self.assertEqual(self.p.stream_alt(1, 1)['data']['max'], 49 * 2)
        self.configure()
        self.alt(1, 1)
        self.model.handle(12, 3, bytes((1, 8, 4)))
        payload = bytes([128 + 64, 128 - 64] * 48)                         # half scale, unsigned
        self.model.handle(102, 0, struct.pack('<BBH', 1, 0, len(payload)) + payload)
        self.assertAlmostEqual(self.model.levels()['received_peak_dbfs'][0], -6.02, 1)
        self.assertEqual(self.model.ostate[1]['frames'], 48)

    def test_float(self):
        p = uac1emu.Profile1('float')
        general, fmt = self.general_and_format(p)
        self.assertEqual(struct.unpack('<H', general[5:7])[0], 3)            # wFormatTag IEEE_FLOAT
        self.assertEqual((fmt[4], fmt[5], fmt[6]), (2, 4, 32))
        self.assertEqual(p.stream_alt(1, 1)['data']['max'], 49 * 8)
        model = uac1emu.Model1(self.wire, self.trace, lambda: self.time, profile=p)
        model.peer_caps = uacemu.CAPS
        model.handle(6, 1, b'\x01')
        model.handle(9, 2, b'\x01\x01')
        model.handle(12, 3, bytes((1, 8, 4)))
        payload = struct.pack('<96f', *([0.25, -0.25] * 47 + [float('nan'), 0.0]))
        model.handle(102, 0, struct.pack('<BBH', 1, 0, len(payload)) + payload)
        self.assertAlmostEqual(model.levels()['received_peak_dbfs'][0], -12.04, 1)
        self.assertEqual(model.ostate[1]['frames'], 48)
        # Infinities clamp to full scale instead of ending the model.
        payload = struct.pack('<96f', *([float('inf'), float('-inf')] * 48))
        model.handle(102, 0, struct.pack('<BBH', 1, 0, len(payload)) + payload)
        self.assertAlmostEqual(model.levels()['received_peak_dbfs'][1], 0.0, 1)
        # One NaN, then 96 infinities, of 192 samples: counted against the stream.
        self.assertEqual((model.float_bad[1], model.float_samples[1]), (97, 192))
        model.report('iso_out_totals')
        self.assertEqual(json.loads(self.trace.getvalue().splitlines()[-1])['float_out_of_range'], {'01': [97, 192]})

    def test_adaptive_source_sync_out(self):
        p = uac1emu.Profile1('adsrc')
        eps = [x for x in walk(p.config) if x[1] == 5]
        self.assertEqual((eps[0][2], eps[0][3] & 0x0c, eps[0][8]), (0x81, 0x08, 0x01))   # ADAPTIVE IN, synch 01
        self.assertEqual((eps[1][2], eps[1][3], eps[1][4], eps[1][7]), (0x01, 0x01, 3, 2))  # sync OUT, bRefresh 2
        model = uac1emu.Model1(self.wire, self.trace, lambda: self.time, profile=p)
        model.peer_caps = uacemu.CAPS
        model.handle(6, 1, b'\x01')
        model.handle(9, 2, b'\x01\x01')
        model.handle(12, 3, bytes((0x81, 8, 4)))
        model.handle(12, 4, bytes((0x01, 8, 4)))
        self.assertEqual(model.fb, {})                    # the model sends nothing on a sync OUT endpoint
        model.capture()
        sent = [m for m in self.wire.messages if m[0] == 102]
        self.assertEqual(struct.unpack('<BBH', sent[-1][2][:4]), (0x81, 0, 96))
        for value in (b'\x00\x00\x0c', b'\x66\x06\x0c', b''):
            model.handle(102, 0, struct.pack('<BBH', 0x01, 0, len(value)) + value)
        sync = model.synch_out[0x01]
        self.assertEqual((sync['packets'], sync['empty']), (3, 1))
        self.assertEqual(sorted(sync['values']), [48.0, round(0x0c0666 / 16384, 4)])
        model.handle(13, 5, b'\x81')
        stop = [json.loads(l) for l in self.trace.getvalue().splitlines() if '"iso_stop"' in l][-1]
        self.assertEqual(stop['synch_out']['01']['packets'], 3)


class ConvertedFormatTest(unittest.TestCase):
    """docs/design/uac2.md 26.12: the formats uac1 converts, MaxPacketsOnly
    both ways and an adaptive source fed Ff."""

    def make(self, name):
        self.time = 100.0
        self.wire, self.trace = Wire(), io.StringIO()
        p = uac1emu.Profile1(name)
        model = uac1emu.Model1(self.wire, self.trace, lambda: self.time, profile=p)
        model.peer_caps = uacemu.CAPS
        model.handle(6, 1, b'\x01')
        return p, model

    def events(self, name):
        return [e for e in map(json.loads, self.trace.getvalue().splitlines()) if e['event'] == name]

    def test_g711_tables(self):
        self.assertEqual((uac1emu.MULAW[0xff], uac1emu.MULAW[0x80], uac1emu.MULAW[0x00]), (0, 32124, -32124))
        self.assertEqual((uac1emu.ALAW[0xd5], uac1emu.ALAW[0x55], uac1emu.ALAW[0xaa], uac1emu.ALAW[0x2a]),
                         (8, -8, 32256, -32256))
        for code in range(256):
            self.assertEqual(uac1emu.ALAW[uac1emu.g711(4, uac1emu.ALAW[code])], uac1emu.ALAW[code])

    def play(self, name, ep=1, packets=200, rate=48000):
        p, model = self.make(name)
        model.handle(9, 2, b'\x01\x01')
        model.handle(12, 3, bytes((ep, 8, 4)))
        info = p.stream_alt(1, 1)
        for k in range(packets):
            data = uac1emu.encode_sine(rate // 1000, info, rate, k * (rate // 1000))
            model.handle(102, 0, struct.pack('<BBH', ep, 0, len(data)) + data)
            self.time += 0.001
        model.report('iso_out_totals')
        return p, model, self.events('iso_out_totals')[-1]

    def test_playback_formats_level_and_tone(self):
        for name in ('pcm8', 'float', 'alaw', 'mulaw'):
            with self.subTest(profile=name):
                p, model, totals = self.play(name)
                self.assertAlmostEqual(totals['received_peak_dbfs'][0], -6.02, delta=0.3)
                self.assertAlmostEqual(totals['tone_hz']['01'], 1000, delta=10)
                self.assertEqual(model.ostate[1]['misframed'], 0)

    def capture(self, name, interface, ep, ms=20):
        p, model = self.make(name)
        model.handle(9, 2, bytes((interface, 1)))
        model.handle(12, 3, bytes((ep, 8, 4)))
        for _ in range(ms):
            model.capture()
            self.time += 0.001
        info = p.stream_alt(interface, 1)
        packets = [m[2][4:] for m in self.wire.messages if m[0] == 102 and m[2][0] == ep]
        return p, model, info, packets

    def test_capture_formats(self):
        for name, interface, ep in (('alaw', 2, 0x82), ('mulaw', 2, 0x82), ('pcm8in', 1, 0x81),
                                    ('floatin', 1, 0x81)):
            with self.subTest(profile=name):
                p, model, info, packets = self.capture(name, interface, ep)
                self.assertEqual(len(packets), 20)
                values = uac1emu.decode16(b''.join(packets), info['format_tag'], info['subslot'], info['bits'])
                peak = max(abs(v) for v in values)
                self.assertAlmostEqual(20 * math.log10(peak / 32768), -6.02, delta=0.3)
                if name == 'pcm8in':
                    self.assertEqual(info['subslot'], 2)
                    self.assertTrue(all(packets[0][i] == 0 for i in range(0, len(packets[0]), 2)))

    def test_mpo_capture_pads_with_2_2_1_sizes(self):
        sizes = []
        real = uac1emu.encode_sine

        def counting(samples, info, rate, phase):
            sizes.append(samples)
            return real(samples, info, rate, phase)
        uac1emu.encode_sine = counting
        try:
            p, model, info, packets = self.capture('mpoin48', 1, 0x81, ms=12)
        finally:
            uac1emu.encode_sine = real
        self.assertTrue(info['mpo'])
        self.assertTrue(all(len(x) == 52 * 4 for x in packets))
        # Exactly 2.2.1's 48, 47, 49, 48 at 48 kHz (never 50: the jitter
        # does not carry into the accumulator).
        self.assertEqual(sizes, [48, 47, 49, 48] * 3)
        for x, n in zip(packets, sizes):
            self.assertEqual(x[n * 4:], bytes(len(x) - n * 4))

    def test_mpo_playback_audio_and_bad_length(self):
        p, model = self.make('mpoout')
        model.handle(9, 2, b'\x01\x01')
        model.handle(12, 3, b'\x01\x08\x04')
        info = p.stream_alt(1, 1)
        self.assertTrue(info['mpo'])
        audio = bytes([0x90, 0x70] * 48) + bytes(2 * 2)
        model.handle(102, 0, struct.pack('<BBH', 1, 0, len(audio)) + audio)
        self.assertEqual(model.ostate[1]['frames'], 48)
        self.assertEqual(model.ostate[1]['mpo_bad'], 0)
        short = bytes([0x90, 0x70] * 48)
        model.handle(102, 0, struct.pack('<BBH', 1, 0, len(short)) + short)
        self.assertEqual((model.ostate[1]['frames'], model.ostate[1]['mpo_bad']), (96, 1))
        model.stop(1)
        self.assertEqual((model.closed[-1]['mpo_bad_length'], model.closed[-1]['ok']), (1, False))

    def test_mpo_playback_padded_stream_passes(self):
        p, model = self.make('mpoout')
        model.handle(9, 2, b'')
        model.handle(12, 3, b'')
        audio = bytes([0x90, 0x70] * 48) + bytes(2 * 2)
        for _ in range(4):
            model.handle(102, 0, struct.pack('<BBH', 1, 0, len(audio)) + audio)
        model.stop(1)
        self.assertEqual((model.closed[-1]['mpo_bad_length'], model.closed[-1]['ok']), (0, True))

    def test_mpo_playback_pcm8_full_scale_negative_is_audio(self):
        # PCM8 0x00 is -1.0: a padded packet of 48 such frames is 48 frames
        # of audio (the cadence's whole frames), not an empty packet.
        p, model = self.make('mpoout')
        model.handle(9, 2, b'')
        model.handle(12, 3, b'')
        audio = bytes(50 * 2)
        for _ in range(4):
            model.handle(102, 0, struct.pack('<BBH', 1, 0, len(audio)) + audio)
        self.assertEqual((model.ostate[1]['frames'], model.ostate[1]['mpo_zero']), (4 * 48, 4 * 48))
        tail = bytes([0x90, 0x70] * 45) + bytes(5 * 2)    # 45 frames, then 3 of 0x00, then padding
        model.handle(102, 0, struct.pack('<BBH', 1, 0, len(tail)) + tail)
        self.assertEqual(model.ostate[1]['frames'], 5 * 48)
        model.stop(1)
        self.assertEqual((model.closed[-1]['mpo_zero_audio'], model.closed[-1]['ok']), (4 * 48 + 3, True))

    def test_mpo_playback_pcm8_full_scale_negative_44100(self):
        # At 44.1 kHz the ambiguous packets follow the cadence (nine 44,
        # one 45): a second of them is 44100 frames, no FIFO drift.
        p, model = self.make('mpoout')
        model.handle(100, 7, struct.pack('<BBBBHHH', 0, 0x01, 0x22, 0, 0x0100, 0x0001, 3) + uac1emu.u24(44100))
        model.handle(9, 2, b'')
        model.handle(12, 3, b'')
        audio = bytes(50 * 2)
        for _ in range(1000):
            model.handle(102, 0, struct.pack('<BBH', 1, 0, len(audio)) + audio)
        s = model.ostate[1]
        self.assertEqual((s['frames'], s['under'], s['over'], s['outside']), (44100, 0, 0, 0))
        model.stop(1)
        self.assertEqual((model.closed[-1]['rate'], model.closed[-1]['ok']), (44100, True))

    def test_adaptive_source_follows_ff(self):
        p, model = self.make('adsrc')
        model.handle(9, 2, b'\x01\x01')
        model.handle(12, 3, bytes((0x81, 8, 4)))
        model.handle(12, 4, bytes((0x01, 8, 4)))
        for value in (0x0c0000, 0x0b0000):
            data = value.to_bytes(3, 'little')
            model.handle(102, 0, struct.pack('<BBH', 0x01, 0, 3) + data)
        sync = model.synch_out[0x01]
        self.assertEqual((sync['off'], sync['expected'], sync['last']), (1, 48.0, 44.0))
        model.capture()
        sent = [m for m in self.wire.messages if m[0] == 102]
        self.assertEqual(struct.unpack('<BBH', sent[-1][2][:4]), (0x81, 0, 88))    # 44 samples


class SocketTest(unittest.TestCase):
    def test_handshake_full_speed(self):
        server, client = socket.socketpair()
        errors = []
        model = uac1emu.Model1(server, io.StringIO(), profile=uac1emu.Profile1('dacd'))

        def run():
            try:
                model.run()
            except EOFError:
                pass
            except Exception as error:
                errors.append(error)

        def exact(length):
            data = b''
            while len(data) < length:
                data += client.recv(length - len(data))
            return data

        def receive():
            kind, length, ident = struct.unpack('<III', exact(12))
            return kind, ident, exact(length)

        worker = threading.Thread(target=run)
        client.settimeout(2)
        worker.start()
        try:
            self.assertEqual(receive()[0], 0)
            hello = b'test'.ljust(64, b'\0') + struct.pack('<I', uacemu.CAPS & ~(1 << uacemu.CAP_ID64))
            client.sendall(struct.pack('<III', 0, len(hello), 0) + hello)
            kinds = [receive() for _ in range(3)]
            self.assertEqual([k[0] for k in kinds], [5, 4, 1])
            self.assertEqual(kinds[2][2][0], 1)          # usb_redir_speed_full
            self.assertEqual(kinds[2][2][4:8], struct.pack('<HH', 0xffff, 0x0301))
        finally:
            client.close()
            worker.join(2)
            server.close()
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])


if __name__ == '__main__':
    unittest.main()
