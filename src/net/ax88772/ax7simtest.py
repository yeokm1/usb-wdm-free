#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests of ax7emu.py: the SIMULATED AX88772B/C driven by ax88772's
own chip core (ax7chip.c and netchip.c, built into
out/net/ax88772/hostabi/ax7chipcore.dll by simhost.ps1) through the
model's usbredir control, bulk and interrupt paths, as axsimtest.py does
for ax88179. No physical USB device.
Run: python -B ax7simtest.py
"""
import ctypes as c
import io
import json
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ax7emu as a  # noqa: E402

DLL_PATH = Path(__file__).resolve().parents[3] / 'out/net/ax88772/hostabi/ax7chipcore.dll'
STATE_SIZE = 2048                     # NET_CHIP_STATE for this package

U8P = c.POINTER(c.c_ubyte)
CONTROL = c.CFUNCTYPE(c.c_long, c.c_void_p, c.c_int, c.c_ubyte, c.c_ushort, c.c_ushort,
                      c.c_void_p, c.c_ushort, c.POINTER(c.c_ushort))
SLEEP = c.CFUNCTYPE(None, c.c_void_p, c.c_ulong)
EMIT = c.CFUNCTYPE(None, c.c_void_p, U8P, c.c_ulong)


class NetIo(c.Structure):
    _fields_ = [('Context', c.c_void_p), ('Control', CONTROL), ('Sleep', SLEEP),
                ('Status', c.c_long), ('Requests', c.c_ulong)]


class NetProbe(c.Structure):
    _fields_ = [(n, c.c_ubyte) for n in ('ConfigIndex', 'ConfigValue', 'Interface', 'Alternate',
                                         'NumInterfaces', 'In', 'Out', 'Interrupt')] + \
        [(n, c.c_ushort) for n in ('InMax', 'OutMax', 'IntMax')] + \
        [('IntInterval', c.c_ubyte), ('Speed', c.c_ubyte)] + \
        [(n, c.c_ushort) for n in ('Vendor', 'Product', 'Revision')]


class NetParams(c.Structure):
    _fields_ = [('Mac', c.c_ubyte * 6)] + [(n, c.c_ulong) for n in (
        'RxSize', 'RxCount', 'TxSize', 'TxSlots', 'TxBatch', 'TxHeader', 'TxAlign', 'Version',
        'RxConfig', 'RxConfigRead')]


class NetLink(c.Structure):
    _fields_ = [('Up', c.c_int), ('Mbps', c.c_ulong), ('FullDuplex', c.c_int), ('RxLength', c.c_ulong)]


class RxStats(c.Structure):
    _fields_ = [(n, c.c_ulong) for n in ('Frames', 'Dropped', 'FcsStripped')]


P = c.POINTER
NetChip = type('NetChip', (c.Structure,), {'_fields_': [
    ('Name', c.c_char_p), ('Description', c.c_char_p), ('DriverVersion', c.c_ushort),
    ('Probe', c.CFUNCTYPE(c.c_long, c.c_void_p, c.c_ulong, P(c.c_void_p), P(c.c_ulong), c.c_ulong, P(NetProbe))),
    ('Start', c.CFUNCTYPE(c.c_long, c.c_void_p, P(NetIo), P(NetProbe), P(NetParams))),
    ('SetMac', c.CFUNCTYPE(c.c_long, c.c_void_p, P(NetIo), U8P)),
    ('Link', c.CFUNCTYPE(c.c_long, c.c_void_p, P(NetIo), P(NetLink))),
    ('Poll', c.CFUNCTYPE(c.c_int, c.c_void_p, P(NetIo))),
    ('SetFilter', c.CFUNCTYPE(c.c_long, c.c_void_p, P(NetIo), c.c_ulong, U8P, c.c_ulong)),
    ('Stop', c.CFUNCTYPE(c.c_long, c.c_void_p, P(NetIo))),
    ('Suspend', c.CFUNCTYPE(c.c_long, c.c_void_p, P(NetIo))),
    ('Resume', c.CFUNCTYPE(c.c_long, c.c_void_p, P(NetIo), P(NetProbe), P(NetParams))),
    ('Status', c.CFUNCTYPE(c.c_int, c.c_void_p, U8P, c.c_ulong)),
    ('RxParse', c.CFUNCTYPE(c.c_long, c.c_void_p, U8P, c.c_ulong, EMIT, c.c_void_p, P(RxStats))),
    ('TxHeader', c.CFUNCTYPE(None, c.c_void_p, U8P, c.c_ulong, c.c_ulong, c.c_int)),
    ('TxEnd', c.CFUNCTYPE(c.c_int, c.c_void_p, U8P, P(c.c_ulong), c.c_ulong, c.c_ulong)),
    ('RxReset', c.CFUNCTYPE(None, c.c_void_p))]})

FILTER_DIRECTED, FILTER_MULTICAST, FILTER_ALL_MULTICAST, FILTER_BROADCAST, FILTER_PROMISCUOUS = 1, 2, 4, 8, 0x20
SPEEDS = {'high': 2, 'full': 1}


class Wire:
    def __init__(self):
        self.messages = []

    def sendall(self, raw):
        kind, length, ident = struct.unpack('<III', raw[:12])
        assert length == len(raw) - 12, 'wire framing'
        self.messages.append((kind, ident, raw[12:]))

    def take(self, kind):
        out = [m for m in self.messages if m[0] == kind]
        self.messages = [m for m in self.messages if m[0] != kind]
        return out


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def buf(data):
    return (c.c_ubyte * max(1, len(data))).from_buffer_copy(data or b'\0')


def arp_request(mac):
    body = struct.pack('!HHBBH', 1, 0x0800, 6, 4, 1) + mac + bytes((192, 168, 77, 2)) + bytes(6) + a.HOST_IP
    return b'\xff' * 6 + mac + b'\x08\x06' + body


def icmp_echo(mac, size, ident=1):
    payload = bytes((i * 3) & 0xff for i in range(size))
    data = b'\x08\x00\x00\x00' + struct.pack('!HH', ident, 1) + payload
    data = data[:2] + struct.pack('!H', a.checksum(data)) + data[4:]
    return a.HOST_MAC + mac + b'\x08\x00' + a.ipv4(bytes((192, 168, 77, 2)), a.HOST_IP, 1, data)


class CoreCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.chip = NetChip.in_dll(c.CDLL(str(DLL_PATH)), 'NetPackageChip')

    def setup(self, speed='high', **kw):
        self.wire, self.trace, self.clock = Wire(), io.StringIO(), Clock()
        self.m = a.Model(self.wire, self.trace, speed, now=self.clock, **kw)
        self.m.peer_caps = a.CAPS & ~(1 << a.CAP_ID64)
        self.m.connect()
        self.m.handle(6, 1, b'\x01')
        self.wire.messages.clear()
        self.state = (c.c_ubyte * STATE_SIZE)()
        self.ident = 100

        def control(_, inp, request, value, index, data, length, actual):
            self.ident += 1
            rt = 0xc0 if inp else 0x40
            payload = b'' if inp else c.string_at(data, length)
            self.m.control(self.ident, struct.pack('<BBBBHHH', 0x80 if inp else 0, request, rt, 0,
                                                   value, index, length) + payload)
            kind, ident, reply = [m for m in self.wire.messages if m[0] == 100][-1]
            self.wire.messages = [m for m in self.wire.messages if m[0] != 100]
            if ident != self.ident or reply[3]:
                return -5
            got = reply[10:]
            if inp:
                c.memmove(data, got, len(got))
                actual[0] = len(got)
            else:
                actual[0] = struct.unpack('<H', reply[8:10])[0]
            return 0

        def sleep(_, ms):
            self.clock.t += ms / 1000.0
            self.m.pump()

        self.cb = (CONTROL(control), SLEEP(sleep))
        self.io = NetIo(None, self.cb[0], self.cb[1], 0, 0)
        self.probe = self.probe_model()
        self.params = NetParams()
        return self.chip.Start(self.state, c.byref(self.io), c.byref(self.probe), c.byref(self.params))

    def probe_model(self):
        dev, cfg = buf(self.m.device), buf(self.m.config_desc)
        configs = (c.c_void_p * 1)(c.cast(cfg, c.c_void_p))
        sizes = (c.c_ulong * 1)(len(self.m.config_desc))
        p = NetProbe()
        self.keep = (dev, cfg, configs, sizes)
        self.assertEqual(self.chip.Probe(c.cast(dev, c.c_void_p), len(self.m.device), configs, sizes, 1,
                                         c.byref(p)), 0)
        return p

    def link(self):
        link = NetLink()
        self.assertEqual(self.chip.Link(self.state, c.byref(self.io), c.byref(link)), 0)
        return link

    def settle(self, seconds):
        for _ in range(int(seconds / 0.05) + 1):
            self.clock.t += 0.05
            self.m.pump()

    def events(self, name):
        return [e for e in (json.loads(x) for x in self.trace.getvalue().splitlines()) if e['event'] == name]

    def tx(self, frame):
        """The driver's framing: TxHeader, the frame, TxEnd."""
        if len(frame) < 60:
            frame += bytes(60 - len(frame))
        out = (c.c_ubyte * (4 + 1514 + 4))()
        self.chip.TxHeader(self.state, out, len(frame), self.probe.OutMax, 1)
        c.memmove(c.byref(out, 4), frame, len(frame))
        total = c.c_ulong(4 + len(frame))
        self.assertEqual(self.chip.TxEnd(self.state, out, c.byref(total), len(out), self.probe.OutMax), 0)
        data = bytes(out)[:total.value]
        self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, len(data), 0, 0) + data)
        self.assertEqual(self.wire.take(101)[-1][2][:2], bytes((a.EP_OUT, 0)))
        return data

    def rx_all(self):
        """Bulk-IN requests of the driver's size until the model has nothing, each parsed by RxParse."""
        frames, dropped, transfers = [], 0, 0
        emit = EMIT(lambda _, p, n: frames.append(c.string_at(p, n)))
        while True:
            self.m.handle(101, 9, struct.pack('<BBHIH', a.EP_IN, 0, self.params.RxSize, 0, 0))
            replies = self.wire.take(101)
            if not replies:
                self.m.handle(21, 9, b'')
                self.wire.take(101)
                return frames, dropped, transfers
            data = replies[-1][2][10:]
            self.assertLessEqual(len(data), self.params.RxSize)
            stats = RxStats()
            rc = self.chip.RxParse(self.state, buf(data), len(data), emit, None, c.byref(stats))
            self.assertGreaterEqual(rc, 0)
            dropped += stats.Dropped
            transfers += 1

    def up(self, speed='high', **kw):
        self.assertEqual(self.setup(speed, **kw), 0)
        self.settle(1.1)
        link = self.link()
        self.assertTrue(link.Up)
        self.assertEqual(self.chip.SetFilter(self.state, c.byref(self.io), FILTER_DIRECTED | FILTER_BROADCAST,
                                             None, 0), 0)
        return link


class Tests(CoreCase):
    def test_start_high(self):
        self.assertEqual(self.setup(), 0)
        self.assertEqual(bytes(self.params.Mac), a.MODEL_MAC)
        self.assertEqual(bytes(self.m.chip.node_id), a.MODEL_MAC)
        self.assertEqual((self.params.RxSize, self.params.RxCount, self.params.TxHeader), (2048, 1, 4))
        self.assertEqual(self.params.Version, 0x1020)
        self.assertEqual(self.m.chip.qct, (0x8000, 0x8001))
        self.assertEqual(self.params.RxConfig, 0x0088)
        self.assertEqual(self.params.RxConfigRead, 0x0088)
        self.assertEqual(self.m.chip.phy[a.ADVERTISE], 0x0de1)
        self.assertFalse(self.m.chip.sw_mii)
        self.assertEqual(self.m.totals['vendor_refused'], 0)
        self.assertEqual(self.probe.Speed, 2)
        self.assertEqual((self.probe.In, self.probe.Out, self.probe.Interrupt), (0x82, 0x03, 0x81))

    def test_start_full_and_772c(self):
        self.assertEqual(self.setup('full', chip='772c'), 0)
        self.assertEqual(self.probe.Speed, 1)
        self.assertEqual(self.probe.Revision, 2)
        self.settle(1.1)
        link = self.link()
        self.assertEqual((link.Up, link.Mbps, link.FullDuplex), (1, 100, 1))

    def test_dlink_reads_node_id(self):
        self.assertEqual(self.setup(vid=0x2001, pid=0x1a02), 0)
        self.assertEqual(bytes(self.params.Mac), a.MODEL_MAC)  # the GPIO reload filled NODE_ID before

    def test_link_speeds(self):
        for mbps, half, pause, medium in ((100, False, True, 0x0336), (100, True, True, 0x0304),
                                          (10, False, False, 0x0106), (10, True, False, 0x0104)):
            link = self.up(mbps=mbps, half=half, partner_pause=pause)
            self.assertEqual((link.Mbps, link.FullDuplex), (mbps, int(not half)))
            self.assertEqual(self.m.chip.medium, medium, (mbps, half, pause))

    def test_status_and_flap(self):
        self.up(flap_after=2, flap_for=3)
        self.m.handle(15, 1, bytes((a.EP_INT,)))
        self.settle(2.1)
        reports = [m for m in self.wire.take(103)]
        self.assertTrue(reports)
        data = reports[-1][2][4:]
        self.assertEqual(self.chip.Status(self.state, buf(data), len(data)), 0)
        self.assertEqual(self.chip.Poll(self.state, c.byref(self.io)), 1)
        self.assertFalse(self.link().Up)
        self.assertEqual(self.m.chip.medium, 0)
        self.settle(3.1)
        data = self.wire.take(103)[-1][2][4:]
        self.assertEqual(self.chip.Status(self.state, buf(data), len(data)), 1)
        self.assertTrue(self.link().Up)

    def test_arp_icmp(self):
        self.up()
        mac = a.MODEL_MAC
        self.tx(arp_request(mac))
        frames, dropped, _ = self.rx_all()
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0][12:14], b'\x08\x06')
        for size, replies in ((32, 1), (1472, 1), (4000, 3)):
            self.tx_fragmented(mac, size)
            frames, dropped, transfers = self.rx_all()
            self.assertEqual((len(frames), dropped), (replies, 0), size)
            self.assertTrue(all(f[12:14] == b'\x08\x00' for f in frames))
            if size == 4000:
                self.assertGreater(transfers, 1)          # 1518 + 1518 + ... bytes of records: they cross
        self.assertEqual(self.m.net.refused, 0)

    def tx_fragmented(self, mac, size):
        payload = bytes((i * 3) & 0xff for i in range(size))
        data = b'\x08\x00\x00\x00' + struct.pack('!HH', 1, 1) + payload
        data = data[:2] + struct.pack('!H', a.checksum(data)) + data[4:]
        for packet in a.ipv4_fragments(bytes((192, 168, 77, 2)), a.HOST_IP, 1, data, 0x1234):
            self.tx(a.HOST_MAC + mac + b'\x08\x00' + packet)

    def test_spanning_random_cuts(self):
        for seed in range(1, 6):
            self.up(cut='random', seed=seed)
            for size in (100, 1472, 3000):
                self.tx_fragmented(a.MODEL_MAC, size)
            frames, dropped, transfers = self.rx_all()
            self.assertEqual(dropped, 0)
            self.assertEqual(len(frames), 1 + 1 + 3)       # 100 and 1472 in one packet, 3000 in three
            self.assertGreater(transfers, len(frames))

    def test_pad_word(self):
        self.up()
        # 508-byte frame: header + frame = 512, a packet boundary: the pad word follows.
        frame = a.HOST_MAC + a.MODEL_MAC + b'\x88\xb5' + bytes(508 - 14)
        data = self.tx(frame)
        self.assertEqual(len(data), 516)
        self.assertEqual(data[-4:], b'\x00\x00\xff\xff')
        self.assertEqual(self.m.totals['tx_padded'], 1)
        data = self.tx(frame + b'\0')
        self.assertEqual(len(data), 513)

    def test_filter(self):
        self.up()
        self.m.deliver(b'\x01\x00\x5e\x00\x00\x01' + a.HOST_MAC + b'\x08\x00' + bytes(46))
        self.assertEqual(self.m.totals['rx_filtered'], 1)
        group = (c.c_ubyte * 6).from_buffer_copy(b'\x01\x00\x5e\x00\x00\x01')
        self.assertEqual(self.chip.SetFilter(self.state, c.byref(self.io), FILTER_DIRECTED | FILTER_MULTICAST,
                                             group, 1), 0)
        self.m.deliver(b'\x01\x00\x5e\x00\x00\x01' + a.HOST_MAC + b'\x08\x00' + bytes(46))
        self.assertEqual(self.m.totals['rx_frames'], 1)
        self.chip.SetFilter(self.state, c.byref(self.io), FILTER_PROMISCUOUS, None, 0)
        self.m.deliver(b'\x02\x11\x22\x33\x44\x55' + a.HOST_MAC + b'\x08\x00' + bytes(46))
        self.assertEqual(self.m.totals['rx_frames'], 2)

    def test_stop_and_resume(self):
        self.up()
        self.assertEqual(self.chip.Stop(self.state, c.byref(self.io)), 0)
        self.assertEqual((self.m.chip.medium, self.m.chip.rx_ctl), (0, 0))
        self.settle(0.2)
        self.assertFalse(self.m.chip.link)
        self.assertEqual(self.chip.Resume(self.state, c.byref(self.io), c.byref(self.probe),
                                          c.byref(self.params)), 0)
        self.settle(1.1)
        self.assertTrue(self.link().Up)

    def test_rx_reset(self):
        self.up()
        self.tx_fragmented(a.MODEL_MAC, 1400)
        self.m.handle(101, 9, struct.pack('<BBHIH', a.EP_IN, 0, 1000, 0, 0))
        data = self.wire.take(101)[-1][2][10:]
        stats = RxStats()
        frames = []
        emit = EMIT(lambda _, p, n: frames.append(c.string_at(p, n)))
        self.assertEqual(self.chip.RxParse(self.state, buf(data), len(data), emit, None, c.byref(stats)), 0)
        self.chip.RxReset(self.state)                     # the rest is lost
        self.m.stream.clear()
        self.tx(arp_request(a.MODEL_MAC))
        got, dropped, _ = self.rx_all()
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0][12:14], b'\x08\x06')


if __name__ == '__main__':
    unittest.main()
