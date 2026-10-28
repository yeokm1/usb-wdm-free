#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests of the original UAC2 usbredir model and its profiles; no
physical USB. The profile tests run the driver's own descriptor core
(uacdesc.c) from the DLL host.ps1 builds. Run: python -B uacsimtest.py
"""
import ctypes as c
import hashlib
import io
import json
import socket
import struct
import threading
import unittest
from pathlib import Path
import uacemu


class Wire:
    def __init__(self):
        self.messages = []

    def sendall(self, raw):
        kind, length, ident = struct.unpack('<III', raw[:12])
        if length != len(raw) - 12:
            raise AssertionError('wire framing')
        self.messages.append((kind, ident, raw[12:]))


class ModelTest(unittest.TestCase):
    def setUp(self):
        self.time = 100.0
        self.wire, self.trace = Wire(), io.StringIO()
        self.model = uacemu.Model(self.wire, self.trace, lambda: self.time)
        self.model.peer_caps = uacemu.CAPS

    def control(self, request, rt, value, index, length, payload=b''):
        self.model.control(777, struct.pack('<BBBBHHH', 0, request, rt, 0,
                                          value, index, length) + payload)
        kind, ident, data = self.wire.messages[-1]
        self.assertEqual((kind, ident), (100, 777))
        header = struct.unpack('<BBBBHHH', data[:10])
        return header[3], header[-1], data[10:]

    def configure(self):
        self.model.handle(6, 1, b'\x01')
        self.assertEqual(self.wire.messages[-1], (8, 1, b'\0\1'))
        self.model.handle(9, 2, b'\x04\x01')
        self.assertEqual(self.wire.messages[-1], (11, 2, b'\0\4\1'))

    def test_descriptor_identity_and_sanitization(self):
        self.model.handle(100, 98, struct.pack('<BBBBHHH', 0, 6, 0x80, 0, 0x100, 0, 18))
        self.assertEqual(self.wire.messages[-1][0:2], (100, 98))
        self.assertEqual(self.wire.messages[-1][2][10:], uacemu.DEVICE)
        self.assertEqual(hashlib.sha256(uacemu.CONFIG).hexdigest(),
                         '89f0b2b70a9be2ddfd9bacb587db7426b8ac5d14727f7e42b6b21111fdafa7e7')
        self.assertEqual(uacemu.DEVICE[8:12], b'\x1e\x04\x78\x32')
        self.assertEqual(uacemu.DEVICE[16], 0)
        for number in range(1, 17):
            status, actual, data = self.control(6, 0x80, 0x300 | number, 0x409, 255)
            self.assertEqual(status, 0)
            self.assertEqual(actual, len(data))
            self.assertIn('SIMULATED', data[2:].decode('utf-16le'))
        self.assertEqual(self.control(6, 0x80, 0x3ff, 0x409, 255)[0], 4)
        self.assertEqual(self.control(6, 0x80, 0x200, 0, 9)[2], uacemu.CONFIG[:9])

    def test_endpoint_info_tracks_active_alternate(self):
        self.model.info()
        info = self.wire.messages[-2][2]
        self.assertEqual(len(info), 160)
        self.assertEqual(info[1], 255)
        self.configure()
        info = self.wire.messages[-3][2]
        self.assertEqual(info[1], 1)
        self.assertEqual(info[19], 1)
        self.assertEqual(info[32 + 1], 1)
        self.assertEqual(info[32 + 19], 4)
        self.assertEqual(struct.unpack_from('<H', info, 96 + 2)[0], 112)
        self.assertEqual(struct.unpack_from('<H', info, 96 + 38)[0], 4)
        self.model.handle(9, 3, b'\x04\x02')
        self.assertEqual(self.wire.messages[-1][2], b'\4\4\2')
        self.assertEqual(self.model.alts[4], 1)

    def test_clock_requests_exact_and_bounded(self):
        self.assertEqual(self.control(2, 0xa1, 0x100, 0x2103, 2)[0], 4)
        self.configure()
        self.assertEqual(self.control(2, 0xa1, 0x100, 0x2103, 2), (0, 2, b'\1\0'))
        self.assertEqual(self.control(2, 0xa1, 0x100, 0x2103, 14),
                         (0, 14, struct.pack('<HIII', 1, 48000, 48000, 0)))
        self.assertEqual(self.control(1, 0xa1, 0x100, 0x2103, 4)[2], struct.pack('<I', 48000))
        self.assertEqual(self.control(1, 0xa1, 0x200, 0x2103, 1)[2], b'\1')
        self.assertEqual(self.control(1, 0xa1, 0x100, 0x2203, 1)[2], b'\1')
        self.assertEqual(self.control(1, 0x21, 0x100, 0x2103, 4, struct.pack('<I', 48000))[0], 0)
        self.assertEqual(self.control(1, 0x21, 0x100, 0x2103, 4, struct.pack('<I', 44100))[0], 4)
        self.assertEqual(self.control(1, 0xa1, 0x100, 0x2104, 4)[0], 4)
        # FU 1 channel 8 declares volume only (bmaControls 0x0c): mute stalls.
        self.assertEqual(self.control(1, 0xa1, 0x108, 0x0103, 1)[0], 4)
        self.assertEqual(self.control(1, 0xa1, 0x100, 0x0103, 1)[2], b'\0')
        self.assertEqual(self.control(1, 0xa1, 0x208, 0x0103, 2)[2], b'\0\0')
        self.assertEqual(self.control(1, 0x21, 0x100, 0x0103, 1, b'\1')[0], 4)

    def test_iso_out_evidence_and_feedback_clock(self):
        self.model.pcm = io.BytesIO()
        self.model.handle(12, 1, bytes((1, 32, 4)))
        self.assertEqual(self.wire.messages[-1][2], b'\4\1')
        self.configure()
        self.model.handle(12, 1, bytes((1, 32, 4)))
        self.model.handle(12, 2, bytes((0x83, 8, 2)))
        self.model.feedback()
        self.assertEqual(self.wire.messages[-1], (102, 0, struct.pack('<BBHI', 0x83, 0, 4, 6 << 16)))
        before = len(self.wire.messages)
        self.time += 0.0005
        self.model.feedback()
        self.assertEqual(before, len(self.wire.messages))
        self.time += 0.0075
        self.model.feedback()
        self.assertGreaterEqual(self.model.feedback_packets, 8)
        self.assertLessEqual(self.model.feedback_packets, 9)
        payload = struct.pack('<12h', *range(-6, 6))
        for _ in range(32):
            self.model.handle(102, 0, struct.pack('<BBH', 1, 0, 24) + payload)
            self.time += 0.000125
        self.assertEqual(self.model.out_bytes, 768)
        self.assertEqual(self.model.out_packets, 32)
        self.assertGreater(self.model.nonzero, 0)
        self.assertEqual(self.model.digest.hexdigest(), hashlib.sha256(payload * 32).hexdigest())
        self.assertEqual(self.model.pcm.getvalue(), payload * 32)
        self.model.report('test')
        event = json.loads(self.trace.getvalue().splitlines()[-1])
        self.assertTrue(event['simulated'])
        self.assertEqual(event['lengths'], {'24': 32})
        self.assertAlmostEqual(event['span_seconds'], 31 / 8000)
        self.model.handle(13, 3, b'\x83')
        before = len(self.wire.messages)
        self.time += 1
        self.model.feedback()
        self.assertEqual(before, len(self.wire.messages))
        self.model.handle(9, 4, b'\4\0')
        self.assertFalse(self.model.streams)
        self.model.handle(9, 5, b'\4\1')
        self.model.handle(12, 6, bytes((1, 32, 4)))
        self.model.pcm_bytes = 4 * 1024 * 1024
        with self.assertRaises(ValueError):
            self.model.handle(102, 0, struct.pack('<BBH', 1, 0, 24) + payload)

    def test_malformed_wire_fails_closed(self):
        for kind, packet in ((3, b'1'), (6, b''), (9, b'4'), (12, b'1'),
                             (13, b''), (21, b'x'), (100, b'x'),
                             (102, b'x'), (255, b'')):
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                self.model.handle(kind, 1, packet)
        with self.assertRaises(ValueError):
            self.control(1, 0x21, 0x100, 0x2103, 4, b'x')
        self.configure()
        self.model.handle(12, 1, bytes((1, 32, 4)))
        for ep, length, data in ((2, 24, b'\0' * 24), (1, 23, b'\0' * 23),
                                 (1, 116, b'\0' * 116), (1, 24, b'\0' * 20)):
            with self.assertRaises(ValueError):
                self.model.handle(102, 1, struct.pack('<BBH', ep, 0, length) + data)
        for value in (b'', b'\x09\x02\0\0' + bytes(5), uacemu.CONFIG[:-1]):
            with self.assertRaises(ValueError):
                uacemu.layout(value)

    def test_fragmented_real_socket_handshake_and_dispatch(self):
        server, client = socket.socketpair()
        errors = []
        model = uacemu.Model(server, io.StringIO())

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
            # A peer without 64-bit ids keeps 12-byte headers.
            hello = b'test'.ljust(64, b'\0') + struct.pack('<I', uacemu.CAPS & ~(1 << uacemu.CAP_ID64))
            wire = struct.pack('<III', 0, len(hello), 0) + hello
            for byte in wire:
                client.sendall(bytes((byte,)))
            self.assertEqual([receive()[0] for _ in range(3)], [5, 4, 1])
            request = struct.pack('<BBBBHHH', 0, 6, 0x80, 0, 0x100, 0, 18)
            client.sendall(struct.pack('<III', 100, len(request), 123) + request)
            kind, ident, response = receive()
            self.assertEqual((kind, ident, response[10:]), (100, 123, uacemu.DEVICE))
        finally:
            client.close()
            worker.join(2)
            server.close()
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])


# ---------------------------------------------------------------------------
# Profiles, through the driver's own descriptor core (uacdesc.c, built into
# out/audio/uac2/hostabi/uaccore.dll by host.ps1).

DLL_PATH = Path(__file__).resolve().parents[3] / 'out/audio/uac2/hostabi/uaccore.dll'


class UacAlt(c.Structure):
    _fields_ = [(n, c.c_ubyte) for n in ('Interface', 'Alternate', 'Terminal', 'Channels', 'Subslot',
                                         'Bits', 'DataEndpoint', 'FeedbackEndpoint', 'Attributes',
                                         'Interval', 'FeedbackInterval')] + \
        [('MaxPacket', c.c_ushort), ('FeedbackMaxPacket', c.c_ushort), ('ChannelConfig', c.c_uint)]


class UacFormat(c.Structure):
    _fields_ = [(n, c.c_ubyte) for n in ('Channels', 'Subslot', 'Bits', 'Terminal')] + [('ChannelConfig', c.c_uint)]


class UacClockPath(c.Structure):
    _fields_ = [('Source', c.c_ubyte), ('Count', c.c_ubyte), ('Id', c.c_ubyte * 16),
                ('Kind', c.c_ubyte * 16), ('Pin', c.c_ubyte * 16)]


class UacDesc(c.Structure):
    _fields_ = [('Bytes', c.c_void_p), ('Length', c.c_uint), ('Control', c.c_ubyte),
                ('First', c.c_ubyte), ('Count', c.c_ubyte), ('Entity', c.c_ushort * 256),
                ('AltCount', c.c_uint), ('Skipped', c.c_uint), ('Alt', UacAlt * 64)]


BUILT = [n for n in uacemu.PROFILE_NAMES if not n.startswith('x4')]


@unittest.skipUnless(DLL_PATH.exists(), 'run host.ps1 first')
class ProfileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dll = c.CDLL(str(DLL_PATH))
        u8p = c.POINTER(c.c_uint)
        cls.dll.UacWholeFirst.argtypes = [c.c_void_p, c.c_uint, u8p]
        cls.dll.UacFunction.argtypes = [c.c_void_p, c.c_uint, c.c_uint, u8p, u8p]
        cls.dll.UacParse.argtypes = [c.c_void_p, c.c_uint, c.c_uint, c.c_uint, c.c_uint, c.c_void_p]
        cls.dll.UacClock.argtypes = [c.c_void_p, c.c_ubyte, c.c_void_p, c.POINTER(c.c_ubyte)]
        cls.dll.UacClockPath.argtypes = [c.c_void_p, c.c_ubyte, c.c_void_p, c.c_void_p]
        cls.dll.UacPlanFormats.argtypes = [c.c_void_p, c.c_int, c.c_int, c.c_void_p, c.c_uint]
        cls.dll.UacPlanFormats.restype = c.c_uint
        cls.dll.UacPlanTerminal.argtypes = [c.c_void_p, c.c_int, c.c_int]
        cls.dll.UacPlanTerminal.restype = c.c_uint
        cls.dll.UacPlanPick.argtypes = [c.c_void_p, c.c_int, c.c_uint, c.c_uint, c.c_uint, c.c_uint,
                                        c.c_uint, c.c_uint, c.c_int, c.POINTER(c.c_uint)]
        cls.dll.UacPlanPacketBytes.argtypes = [c.c_uint, c.c_int, c.c_uint, c.c_uint, c.c_int]
        cls.dll.UacPlanPacketBytes.restype = c.c_uint

    def setUp(self):
        self.time = 100.0

    def parse(self, config):
        blob = c.create_string_buffer(config, len(config))
        self.keep = blob
        first, control, count = c.c_uint(), c.c_uint(), c.c_uint()
        self.assertEqual(self.dll.UacWholeFirst(blob, len(config), c.byref(first)), 0)
        self.assertEqual(self.dll.UacFunction(blob, len(config), first.value, c.byref(control),
                                              c.byref(count)), 0)
        d = UacDesc()
        rc = self.dll.UacParse(blob, len(config), control.value, first.value, count.value, c.byref(d))
        return rc, d

    def driver_clock(self, d, terminal, selections=None):
        sel = (c.c_ubyte * 256)()
        for k, v in (selections or {}).items():
            sel[k] = v
        source = c.c_ubyte()
        return self.dll.UacClock(c.byref(d), terminal, sel, c.byref(source)), source.value

    def model(self, name, **kw):
        wire, trace = Wire(), io.StringIO()
        m = uacemu.Model(wire, trace, lambda: self.time, profile=uacemu.Profile(name), **kw)
        m.peer_caps = uacemu.CAPS
        return m, wire, trace

    def control(self, m, request, rt, value, index, length, payload=b''):
        m.control(5, struct.pack('<BBBBHHH', 0, request, rt, 0, value, index, length) + payload)
        header = struct.unpack('<BBBBHHH', m.connection.messages[-1][2][:10])
        return header[3], m.connection.messages[-1][2][10:]

    def configure(self, m, *alts):
        m.handle(6, 1, b'\x01')
        for interface, alternate in alts:
            m.handle(9, 2, bytes((interface, alternate)))
            self.assertEqual(m.connection.messages[-1][2], bytes((0, interface, alternate)))

    def test_driver_parser_sees_every_alternate(self):
        for name in uacemu.PROFILE_NAMES:
            with self.subTest(profile=name):
                p = uacemu.Profile(name)
                rc, d = self.parse(p.config)
                self.assertEqual(rc, 0)
                driver = {(d.Alt[i].Interface, d.Alt[i].Alternate):
                          (d.Alt[i].Channels, d.Alt[i].Subslot, d.Alt[i].Bits, d.Alt[i].DataEndpoint,
                           d.Alt[i].FeedbackEndpoint, d.Alt[i].MaxPacket) for i in range(d.AltCount)}
                model = {}
                for key in p.alts:
                    info = p.stream_alt(*key)
                    if info:
                        model[key] = (info['channels'], info['subslot'], info['bits'],
                                      info['data']['address'],
                                      info['feedback']['address'] if info['feedback'] else 0,
                                      info['data']['max'])
                self.assertEqual(driver, model)
                # The X4 has one Type III (non-PCM) alternate; both skip it.
                self.assertEqual(d.Skipped, len(p.alts) - len(model))
                self.assertEqual(d.Skipped, 1 if name.startswith('x4') else 0)

    def test_built_shapes(self):
        shape = {}
        for name in BUILT:
            p = uacemu.Profile(name)
            self.assertEqual((p.device[8:12], p.speed), (struct.pack('<HH', 0xffff, p.pid),
                                                         'full' if name == 'fs' else 'high'))
            infos = [p.stream_alt(*k) for k in sorted(p.alts)]
            out = [i for i in infos if not i['data']['address'] & 0x80]
            inn = [i for i in infos if i['data']['address'] & 0x80]
            shape[name] = (sorted({(i['channels'], i['subslot'], i['bits']) for i in out}),
                           sorted({(i['channels'], i['subslot'], i['bits']) for i in inn}))
            for i in infos:
                self.assertLessEqual(i['data']['max'], 1024 if p.speed == 'high' else 1023)
        self.assertEqual(shape['hs24'], ([(2, 2, 16), (2, 3, 24), (2, 4, 32)], [(2, 2, 16), (2, 3, 24)]))
        self.assertEqual(shape['multi'], ([(6, 3, 24), (8, 2, 16)], []))
        self.assertEqual(shape['mic'], ([], [(1, 2, 16), (1, 3, 24)]))
        self.assertEqual(shape['fs'], ([(2, 2, 16)], [(1, 2, 16)]))
        self.assertEqual(shape['adaptive'], ([(2, 2, 16)], [(2, 2, 16)]))
        # The CX31993 model: synchronous playback only, the listed packet sizes.
        self.assertEqual(shape['cx31993'], ([(2, 2, 16), (2, 3, 24), (2, 4, 32)], []))
        self.assertEqual([uacemu.Profile('cx31993').stream_alt(1, a)['data']['max'] for a in (1, 2, 3)],
                         [192, 288, 384])
        self.assertIsNone(uacemu.Profile('cx31993').stream_alt(1, 1)['feedback'])
        # Adaptive playback has no feedback endpoint; async playback has one.
        self.assertIsNone(uacemu.Profile('adaptive').stream_alt(1, 1)['feedback'])
        self.assertEqual(uacemu.Profile('hs24').stream_alt(1, 1)['feedback']['address'], 0x89)
        self.assertEqual(uacemu.Profile('fs').stream_alt(1, 1)['feedback']['max'], 3)

    def test_clock_paths_through_driver(self):
        p = uacemu.Profile('clocks')
        rc, d = self.parse(p.config)
        self.assertEqual(rc, 0)
        # Playback terminal 1 behind the two-input selector 0x12.
        self.assertEqual(self.driver_clock(d, 1)[0], 5)            # UAC_CLOCK_SELECTION
        self.assertEqual(self.driver_clock(d, 1, {0x12: 1}), (0, 0x10))
        self.assertEqual(self.driver_clock(d, 1, {0x12: 2}), (0, 0x11))
        # Capture terminal 5 on multiplier 0x13 (1/1) of source 0x11.
        self.assertEqual(self.driver_clock(d, 5), (0, 0x11))
        path = UacClockPath()
        self.assertEqual(self.dll.UacClockPath(c.byref(d), 5, None, c.byref(path)), 0)
        self.assertEqual((path.Source, path.Count, path.Id[0], path.Kind[0]), (0x11, 1, 0x13, 12))
        for name in ('hs24', 'mic', 'fs', 'adaptive', 'multi'):
            rc, d = self.parse(uacemu.Profile(name).config)
            for i in range(d.AltCount):
                self.assertEqual(self.driver_clock(d, d.Alt[i].Terminal), (0, 0x10), name)

    def test_plan_every_profile(self):
        # The driver's planning (uacplan.c) offers each profile's formats on
        # its first terminal and finds an alternate for each at every rate
        # its clock path can run, at the profile's speed.
        for name in BUILT:
            p = uacemu.Profile(name)
            hs = 1 if p.speed == 'high' else 0
            rc, d = self.parse(p.config)
            self.assertEqual(rc, 0, name)
            for capture in (0, 1):
                fmts = (UacFormat * 16)()
                n = self.dll.UacPlanFormats(c.byref(d), capture, hs, fmts, 16)
                infos = [p.stream_alt(*k) for k in sorted(p.alts)]
                want = {(i['channels'], i['subslot'], i['bits']) for i in infos
                        if bool(i['data']['address'] & 0x80) == bool(capture)}
                got = {(fmts[k].Channels, fmts[k].Subslot, fmts[k].Bits) for k in range(n)}
                self.assertEqual(got, want, (name, capture))
                term = self.dll.UacPlanTerminal(c.byref(d), capture, hs)
                self.assertEqual(term != 0, bool(want), (name, capture))
                for k in range(n):
                    if fmts[k].Terminal != term:
                        continue
                    rates = set()
                    for cid, e in p.entities.items():
                        if e['kind'] == 'cs':
                            rates |= set(p.rates[cid])
                    for rate in sorted(rates):
                        index = c.c_uint()
                        rc = self.dll.UacPlanPick(c.byref(d), capture, term, fmts[k].Channels,
                                                  fmts[k].Subslot, fmts[k].Bits, fmts[k].ChannelConfig,
                                                  rate, hs, c.byref(index))
                        need = self.dll.UacPlanPacketBytes(rate, hs, 1, fmts[k].Channels * fmts[k].Subslot, 1)
                        alts = [i for i in infos if (i['channels'], i['bits']) == (fmts[k].Channels, fmts[k].Bits)
                                and bool(i['data']['address'] & 0x80) == bool(capture)]
                        if any(i['data']['max'] >= need for i in alts):
                            self.assertEqual(rc, 0, (name, capture, rate))
                            a = d.Alt[index.value]
                            self.assertEqual((a.Channels, a.Bits, a.Terminal), (fmts[k].Channels, fmts[k].Bits, term))

    def test_clock_requests_and_rates(self):
        m, wire, _ = self.model('clocks')
        self.configure(m)
        st, data = self.control(m, 2, 0xa1, 0x100, 0x1000, 256)
        self.assertEqual(st, 0)
        self.assertEqual(struct.unpack('<H', data[:2])[0], 3)
        self.assertEqual([struct.unpack_from('<I', data, 2 + 12 * i)[0] for i in range(3)], uacemu.FAMILY_44)
        self.assertEqual(self.control(m, 1, 0xa1, 0x100, 0x1200, 1), (0, b'\1'))
        self.assertEqual(m.clock, 44100)
        self.assertEqual(self.control(m, 1, 0x21, 0x100, 0x1200, 1, b'\2')[0], 0)
        self.assertEqual(m.clock, 48000)
        self.assertEqual(self.control(m, 1, 0x21, 0x100, 0x1200, 1, b'\3')[0], 4)
        self.assertEqual(self.control(m, 1, 0x21, 0x100, 0x1100, 4, struct.pack('<I', 96000))[0], 0)
        self.assertEqual(self.control(m, 1, 0x21, 0x100, 0x1100, 4, struct.pack('<I', 44100))[0], 4)
        self.assertEqual(m.clock, 96000)
        self.assertEqual(self.control(m, 1, 0xa1, 0x100, 0x1300, 2), (0, b'\1\0'))   # numerator
        self.assertEqual(self.control(m, 1, 0xa1, 0x200, 0x1300, 2), (0, b'\1\0'))   # denominator
        self.assertEqual(self.control(m, 1, 0xa1, 0x200, 0x1100, 1), (0, b'\1'))     # valid
        # A fixed clock (adaptive profile) reads but does not set.
        m, _, _ = self.model('adaptive')
        self.configure(m)
        self.assertEqual(self.control(m, 1, 0x21, 0x100, 0x1000, 4, struct.pack('<I', 48000))[0], 4)
        self.assertEqual(self.control(m, 1, 0xa1, 0x100, 0x1000, 4), (0, struct.pack('<I', 48000)))

    def test_feature_unit(self):
        m, _, _ = self.model('multi')
        self.configure(m)
        for ch in (0, 8):
            self.assertEqual(self.control(m, 1, 0x21, 0x100 | ch, 0x0200, 1, b'\1')[0], 0)
            self.assertEqual(self.control(m, 1, 0xa1, 0x100 | ch, 0x0200, 1), (0, b'\1'))
            self.assertEqual(self.control(m, 1, 0x21, 0x200 | ch, 0x0200, 2, struct.pack('<h', -0x0a00))[0], 0)
            self.assertEqual(self.control(m, 1, 0xa1, 0x200 | ch, 0x0200, 2), (0, struct.pack('<h', -0x0a00)))
        self.assertEqual(self.control(m, 1, 0xa1, 0x109, 0x0200, 1)[0], 4)              # no channel 9
        self.assertEqual(self.control(m, 2, 0xa1, 0x200, 0x0200, 8),
                         (0, struct.pack('<Hhhh', 1, -0x6000, 0, 0x100)))
        self.assertEqual(self.control(m, 1, 0x21, 0x200, 0x0200, 2, struct.pack('<h', 0x100))[0], 4)

    def test_feature_unit_masks(self):
        # cx31993: master mute only (0x03), channel volume only (0x0c);
        # absent controls stall, present ones answer.
        m, _, _ = self.model('cx31993')
        self.configure(m)
        self.assertEqual(self.control(m, 1, 0x21, 0x100, 0x0200, 1, b'')[0], 0)
        self.assertEqual(self.control(m, 1, 0xa1, 0x100, 0x0200, 1), (0, b''))
        self.assertEqual(self.control(m, 1, 0xa1, 0x200, 0x0200, 2)[0], 4)        # no master volume
        self.assertEqual(self.control(m, 2, 0xa1, 0x200, 0x0200, 8)[0], 4)
        for ch in (1, 2):
            self.assertEqual(self.control(m, 1, 0xa1, 0x100 | ch, 0x0200, 1)[0], 4)   # no channel mute
            self.assertEqual(self.control(m, 1, 0x21, 0x100 | ch, 0x0200, 1, b'')[0], 4)
            self.assertEqual(self.control(m, 1, 0x21, 0x200 | ch, 0x0200, 2, struct.pack('<h', -0x0a00))[0], 0)
            self.assertEqual(self.control(m, 1, 0xa1, 0x200 | ch, 0x0200, 2), (0, struct.pack('<h', -0x0a00)))

    def test_playback_formats_and_feedback(self):
        for name, alt, frame, value in (('hs24', 2, 6, struct.pack('<I', 6 << 16)),
                                         ('fs', 1, 4, struct.pack('<I', 48 << 14)[:3])):
            m, wire, _ = self.model(name)
            self.configure(m, (1, alt))
            m.handle(12, 1, bytes((1, 8, 4)))
            fb = 0x89
            m.handle(12, 2, bytes((fb, 8, 2)))
            self.assertEqual(wire.messages[-1][2], bytes((0, fb)))
            m.feedback()
            self.assertEqual(wire.messages[-1], (102, 0, struct.pack('<BBH', fb, 0, len(value)) + value))
            m.handle(102, 0, struct.pack('<BBH', 1, 0, frame * 7) + b'\1' * (frame * 7))
            with self.assertRaises(ValueError):
                m.handle(102, 0, struct.pack('<BBH', 1, 0, frame * 7 + 2) + b'\1' * (frame * 7 + 2))
            self.assertEqual(m.out_bytes, frame * 7)
        # 44.1 kHz at High Speed: 5.5125 samples per microframe in 16.16.
        m, wire, _ = self.model('hs24')
        self.configure(m, (1, 1))
        self.control(m, 1, 0x21, 0x100, 0x1000, 4, struct.pack('<I', 44100))
        m.handle(12, 2, bytes((0x89, 8, 2)))
        m.feedback()
        self.assertEqual(struct.unpack('<I', wire.messages[-1][2][4:])[0], round(44100 / 8000 * 65536))

    def test_feedback_drift_and_loss(self):
        m, wire, _ = self.model('hs24', fb_ppm=500, fb_zero_every=4, fb_stop_after=0.010)
        self.configure(m, (1, 1))
        m.handle(12, 2, bytes((0x89, 8, 2)))
        packets = []
        for _ in range(20):
            m.feedback()
            packets += [d for k, _, d in wire.messages if k == 102]
            wire.messages.clear()
            self.time += 0.001
        lengths = [struct.unpack('<BBH', d[:4])[2] for d in packets]
        self.assertEqual(len(packets), 20)
        self.assertEqual(lengths[:10], [4, 4, 4, 0, 4, 4, 4, 0, 4, 4])
        self.assertEqual(set(lengths[10:]), {0})                          # stopped after 10 ms
        self.assertEqual(struct.unpack('<I', packets[0][4:])[0], round(6.003 * 65536))
        self.assertEqual((m.feedback_packets, m.zero_feedback), (8, 12))

    def test_capture_streams(self):
        for name, alt, rate, frame in (('mic', 2, 48000, 3), ('hs24', 1, 96000, 4), ('fs', 1, 48000, 2)):
            m, wire, _ = self.model(name)
            interface = 1 if name == 'mic' else 2
            self.configure(m, (interface, alt))
            if rate != 48000:
                self.control(m, 1, 0x21, 0x100, 0x1000, 4, struct.pack('<I', rate))
            ep = 0x80 | interface
            m.handle(12, 3, bytes((ep, 8, 4)))
            self.assertEqual(wire.messages[-1][2], bytes((0, ep)))
            for _ in range(100):
                m.capture()
                self.time += 0.000125 if name != 'fs' else 0.001
            data = [d[4:] for k, _, d in wire.messages if k == 102]
            samples = sum(len(x) for x in data) // frame
            seconds = len(data) * (0.000125 if name != 'fs' else 0.001)
            self.assertAlmostEqual(samples / seconds, rate, delta=rate * 0.02, msg=name)
            self.assertTrue(all(len(x) % frame == 0 for x in data))
            pcm = b''.join(data)
            self.assertNotEqual(pcm.count(0), len(pcm))
        # Stopping the stream stops the packets.
        m.handle(13, 4, bytes((ep,)))
        wire.messages.clear()
        self.time += 0.01
        m.capture()
        self.assertEqual(wire.messages, [])

    def test_x4full_opens_every_alternate(self):
        m, wire, _ = self.model('x4full')
        m.handle(6, 1, b'\x01')
        for interface, alternate in sorted(m.p.alts):
            m.handle(9, 2, bytes((interface, alternate)))
            self.assertEqual(wire.messages[-1][2], bytes((0, interface, alternate)))
        m, wire, _ = self.model('x4')
        m.handle(6, 1, b'\x01')
        m.handle(9, 2, b'\x04\x03')
        self.assertEqual(wire.messages[-1][2], b'\4\4\3')
        m.handle(9, 2, b'\x05\x01')
        self.assertEqual(wire.messages[-1][2], b'\4\5\1')

    def test_64bit_ids_after_both_hellos(self):
        server, client = socket.socketpair()
        model = uacemu.Model(server, io.StringIO(), profile=uacemu.Profile('mic'))
        errors = []

        def run():
            try:
                model.run()
            except EOFError:
                pass
            except Exception as error:
                errors.append(error)

        def exact(n):
            data = b''
            while len(data) < n:
                data += client.recv(n - len(data))
            return data

        worker = threading.Thread(target=run)
        client.settimeout(2)
        worker.start()
        try:
            kind, length, _ = struct.unpack('<III', exact(12))
            exact(length)
            hello = b'test'.ljust(64, b'\0') + struct.pack('<I', uacemu.CAPS)
            client.sendall(struct.pack('<III', 0, len(hello), 0) + hello)
            kinds = []
            for _ in range(3):
                kind, length, _ = struct.unpack('<IIQ', exact(16))
                body = exact(length)
                kinds.append(kind)
            self.assertEqual(kinds, [5, 4, 1])
            self.assertEqual(body[0], 2)                                 # High Speed
            self.assertEqual(body[4:8], struct.pack('<HH', 0xffff, 0x0203))
            request = struct.pack('<BBBBHHH', 0, 6, 0x80, 0, 0x100, 0, 18)
            client.sendall(struct.pack('<IIQ', 100, len(request), 1 << 40) + request)
            kind, length, ident = struct.unpack('<IIQ', exact(16))
            self.assertEqual((kind, ident, exact(length)[10:]), (100, 1 << 40, model.p.device))
        finally:
            client.close()
            worker.join(2)
            server.close()
        self.assertEqual(errors, [])


if __name__ == '__main__':
    unittest.main()
