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
import collections
import ctypes as c
import io
import json
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ax7emu as a  # noqa: E402
import tcphost  # noqa: E402

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
        [(n, c.c_ushort) for n in ('Vendor', 'Product', 'Revision')] + \
        [('Settings', c.POINTER(c.c_long))]   # NULL: the keywords' defaults (ethernet.md 4.10)


class NetParams(c.Structure):
    _fields_ = [('Mac', c.c_ubyte * 6)] + [(n, c.c_ulong) for n in (
        'RxSize', 'RxCount', 'TxSize', 'TxSlots', 'TxBatch', 'TxHeader', 'TxAlign', 'Version',
        'RxConfig', 'RxConfigRead', 'Offload', 'LsoMax', 'Ignored')]


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

    def test_lost_transfer_realigns(self):
        """A transfer lost in the middle of continuous traffic whose records do
        not line up with the 2048-byte transfers: the stream stays out of step
        until Poll and Link restart receive, then frames flow again."""
        self.up()
        emit = EMIT(lambda _, p, n: None)

        def transfer():
            self.m.handle(101, 9, struct.pack('<BBHIH', a.EP_IN, 0, 2048, 0, 0))
            replies = self.wire.take(101)
            return replies[-1][2][10:] if replies else None

        def parse(data):
            stats = RxStats()
            return self.chip.RxParse(self.state, buf(data), len(data), emit, None, c.byref(stats))

        frame = b'\xff' * 6 + a.HOST_MAC + b'\x88\xb5' + bytes(61 - 14)  # 66-byte records
        for _ in range(300):
            self.m.deliver(frame)
        self.assertGreaterEqual(parse(transfer()), 1)
        transfer()                                        # lost on the way
        self.chip.RxReset(self.state)
        bad = 0
        for _ in range(4):
            if parse(transfer()) < 0:
                bad += 1
        self.assertEqual(bad, 4)
        self.assertEqual(self.chip.Poll(self.state, c.byref(self.io)), 1)
        self.assertTrue(self.link().Up)                   # receive off and on: the buffer is emptied
        self.assertEqual(len(self.m.stream), 0)
        self.assertEqual(self.chip.Poll(self.state, c.byref(self.io)), 0)
        for _ in range(100):
            self.m.deliver(frame)
        frames, dropped, transfers = self.rx_all()
        self.assertEqual((len(frames), dropped), (100, 0))
        self.assertGreater(transfers, 3)

    def test_sr9800(self):
        self.assertEqual(self.setup(chip='sr9800'), 0)
        self.assertEqual((self.probe.Vendor, self.probe.Product), (0x0fe6, 0x9800))
        self.assertEqual(self.params.RxSize, 4096)
        self.assertEqual(self.m.chip.qct, (0x8100, 0x8147))
        self.assertEqual(self.m.chip.rx_ctl, 0x0188)
        self.assertEqual(self.m.totals['vendor_refused'], 0)
        self.settle(1.1)
        link = self.link()
        self.assertEqual((link.Up, link.Mbps, link.FullDuplex, link.RxLength), (1, 100, 1, 4096))
        self.assertEqual(self.m.chip.medium, 0x0336)
        self.chip.SetFilter(self.state, c.byref(self.io), FILTER_DIRECTED | FILTER_BROADCAST, None, 0)
        self.tx(arp_request(a.MODEL_MAC))
        frames, dropped, _ = self.rx_all()
        self.assertEqual(len(frames), 1)
        for size, replies in ((1472, 1), (4000, 3), (8000, 6)):
            self.tx_fragmented(a.MODEL_MAC, size)
            frames, dropped, transfers = self.rx_all()
            self.assertEqual((len(frames), dropped), (replies, 0), size)
        # sr_tx_fixup's rule: the pad word at (len + 4) % 511 == 0, nothing at 512.
        frame = a.HOST_MAC + a.MODEL_MAC + b'\x88\xb5' + bytes(507 - 14)
        self.assertEqual(len(self.tx(frame)), 515)
        self.assertEqual(len(self.tx(frame + b'\0')), 512)
        self.assertEqual(self.m.totals['tx_padded'], 1)

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

    def test_supereal_sr9800_requests(self):
        """Supereal's SR9800 1.1.7.0 (docs/testing/2026-11-07-model-fixes-ref4.md):
        SET_FEATURE(DEVICE_REMOTE_WAKEUP) first, then WRITE_MONITOR_MODE 0x1D."""
        self.setup(chip='sr9800')
        self.wire.messages.clear()
        self.m.handle(100, 3, struct.pack('<BBBBHHH', 0x00, 0x03, 0x00, 0, 1, 0, 0))
        self.assertEqual(self.wire.take(100)[-1][2][3], 0)
        self.assertTrue(self.m.remote_wakeup)
        self.m.handle(100, 4, struct.pack('<BBBBHHH', 0x00, 0x01, 0x00, 0, 1, 0, 0))
        self.assertEqual(self.wire.take(100)[-1][2][3], 0)
        self.assertFalse(self.m.remote_wakeup)
        self.m.handle(100, 5, struct.pack('<BBBBHHH', 0x00, 0x03, 0x00, 0, 2, 0, 0))   # TEST_MODE: refused
        self.assertEqual(self.wire.take(100)[-1][2][3], 4)
        self.m.handle(100, 6, struct.pack('<BBBBHHH', 0x00, 0x1d, 0x40, 0, 0x0000, 0, 0))
        self.assertEqual(self.wire.take(100)[-1][2][3], 0)
        self.m.handle(100, 7, struct.pack('<BBBBHHH', 0x00, 0x1d, 0x40, 0, 0x0006, 0, 0))
        self.m.handle(100, 8, struct.pack('<BBBBHHH', 0x80, 0x1c, 0xc0, 0, 0, 0, 1))
        reply = self.wire.take(100)[-1][2]
        self.assertEqual((reply[3], reply[10:]), (0, b'\x06'))
        self.assertEqual(self.m.config_desc[7], 0xa0)                    # remote wakeup advertised

    def test_vendor_requests_unconfigured(self):
        """ASIX's XP driver 5.4.3.0 sets configuration 0 and then talks on
        endpoint 0 (docs/testing/2026-11-06-reference-drivers.md): vendor
        requests are answered in any state; bulk transfers still need a
        configuration."""
        self.setup()
        self.m.handle(6, 2, b'\x00')
        self.wire.messages.clear()
        self.m.handle(100, 3, struct.pack('<BBBBHHH', 0x80, 0x09, 0xc0, 0, 0, 0, 1))
        reply = self.wire.take(100)[-1][2]
        self.assertEqual(reply[3], 0)                                 # status: answered
        self.assertEqual(reply[10] & 0x70, a.CHIPCODE_772B)
        self.m.handle(100, 4, struct.pack('<BBBBHHH', 0x00, 0x20, 0x40, 0, 0x48, 0, 0))
        self.assertEqual(self.wire.take(100)[-1][2][3], 0)
        self.m.handle(101, 5, struct.pack('<BBHIH', a.EP_IN, 0, 512, 0, 0))
        self.assertEqual(self.wire.take(101)[-1][2][1], 4)            # no configuration: STALL

    def test_reference_7_requests(self):
        """What ASIX's 7 driver 5.12.12.0 reads and writes beyond Linux's set."""
        self.setup()
        chip = self.m.chip
        self.assertEqual(a.Chip(0x003b1861).ipg, bytes((0x15, 0x0c, 0x12)))   # power-up
        # ax7chip.c writes Linux's value (IPG0 | IPG1 = 0x1d, IPG2 0x12) at bring-up.
        self.assertEqual(chip.read(a.READ_IPG012, 0, 0, 3), bytes((0x1d, 0x00, 0x12)))
        self.assertTrue(chip.write(a.WRITE_IPG0, 0x0c15, 0x0e, b''))
        self.assertEqual(chip.read(a.READ_IPG012, 0, 0, 3), bytes((0x15, 0x0c, 0x0e)))
        self.assertTrue(chip.write(a.SW_PHY_SELECT, 0x0001, 0, b''))
        self.assertEqual(chip.read(a.SW_PHY_STATUS, 0, 0, 1), b'\x01')
        for request in (0x26, 0x2c, 0x2e):
            self.assertTrue(chip.write(request, 0x0307, 0, b''))
        self.assertTrue(chip.write(0x24, 0x0008, 0, bytes(4)))
        self.assertEqual(chip.read(0x2d, 0, 0, 4), bytes(4))
        self.assertIsNone(chip.read(0x2d, 0, 0, 2))

    def test_reference_7_transmit_form(self):
        """11-bit complement, flags in bits 11-15, several records in one
        transfer on even offsets; with TXCOE set, a record without bit 15
        gets its IPv4 and UDP checksums."""
        self.up()
        self.assertTrue(self.m.chip.write(a.WRITE_TXCOE, 0x0307, 0, b''))
        arp = arp_request(a.MODEL_MAC) + bytes(18)                    # 60 bytes
        odd = arp + b'\0'                                             # 61 bytes
        recs = b''
        for frame, flags in ((arp, 0x8000), (odd, 0x4000), (arp, 0)):
            n = len(frame)
            recs += struct.pack('<I', ((n ^ 0x7ff) << 16) | flags | n) + frame + (b'\0' if n & 1 else b'')
        self.m.transmit(recs)
        self.assertEqual(self.m.totals['tx_records_asix_form'], 3)
        self.assertEqual(self.m.totals['tx_frames'], 3)
        # A flagged IPv4 UDP record through the parser: the frame handed on carries
        # a correct UDP checksum over UDP's own length, not the trailing bytes.
        udp = a.ipv4(bytes((192, 168, 77, 2)), a.HOST_IP, 17,
                     struct.pack('!HHHH', 68, 67, 12, 0x1234) + b'abcd' + b'zz')
        frame = a.HOST_MAC + a.MODEL_MAC + b'\x08\x00' + udp
        n = len(frame)
        sent = []
        self.m.transmit_frame, keep = (lambda f, p: sent.append(f) or f), self.m.transmit_frame
        try:
            self.m.transmit(struct.pack('<I', ((n ^ 0x7ff) << 16) | 0x4000 | n) + frame + (b'\0' if n & 1 else b''))
        finally:
            self.m.transmit_frame = keep
        seg = sent[0][34:34 + 12]
        pseudo = sent[0][26:34] + struct.pack('!BBH', 0, 17, 12)
        self.assertEqual(a.checksum(pseudo + seg), 0)
        self.assertEqual(a.checksum(sent[0][14:34]), 0)               # the IPv4 header too
        # Truncated or non-IPv4 headers are left as sent.
        bad = frame[:14] + bytes((0x4f,)) + frame[15:34]
        self.assertEqual(self.m.fill_checksums(bad, 0x0307), bad)
        with self.assertRaises(ValueError):                          # a broken complement
            self.m.transmit(struct.pack('<I', (0x123 << 16) | 60) + arp)

    def reference_rx(self, rx_ctl, qct):
        """The receive settings ASIX's drivers write, then two pings' replies
        (one fragmented) as the bulk-IN transfers a 32 KiB request gets."""
        self.up()
        self.assertTrue(self.m.chip.write(a.WRITE_RX_CTL, rx_ctl, 0, b''))
        self.assertTrue(self.m.chip.write(a.QCTCTRL, qct[0], qct[1], b''))
        self.tx(arp_request(a.MODEL_MAC))
        self.tx(icmp_echo(a.MODEL_MAC, 32, 2))
        frame = icmp_echo(a.MODEL_MAC, 3000, 3)
        out = []
        for ip in a.ipv4_fragments(bytes((192, 168, 77, 2)), a.HOST_IP, 1, frame[34:], 0x99):
            out.append(frame[:14] + ip)
        for f in out:
            self.m.transmit_frame(f, False)
        transfers = []
        while True:
            self.m.handle(101, 9, struct.pack('<BBHIH', a.EP_IN, 0, 32768, 0, 0))
            replies = self.wire.take(101)
            if not replies:
                self.m.handle(21, 9, b'')
                self.wire.take(101)
                return transfers
            transfers.append(replies[-1][2][10:])

    def test_reference_receive_extension(self):
        """RX_CTL bit 9 (RH2M) adds 2 zero bytes after the length word; with
        QCTCTRL wValue bit 15 clear each transfer holds one whole record."""
        transfers = self.reference_rx(0x0398, (0x008f, 0xb500))
        self.assertEqual(len(transfers), 5)                         # ARP, echo, three fragments
        for data in transfers:
            n = struct.unpack_from('<H', data)[0] & 0x7ff
            self.assertEqual(struct.unpack_from('<H', data, 2)[0] & 0x7ff, n ^ 0x7ff)
            self.assertEqual(data[4:6], b'\0\0')
            self.assertEqual(len(data), 6 + n + (n & 1))
        self.assertEqual(self.m.records, collections.deque())

    def test_rh1m_alone_and_linux_bursts(self):
        """RH1M alone (sr9800's default) keeps the 4-byte header, and QCTCTRL
        0x8000 keeps Linux's 2 KiB bursts that cut records."""
        transfers = self.reference_rx(0x0188, (0x8000, 0x8001))
        self.assertTrue(all(len(d) <= 2048 for d in transfers))
        stream = b''.join(transfers)
        offset, frames = 0, 0
        while offset < len(stream):
            n = struct.unpack_from('<H', stream, offset)[0] & 0x7ff
            self.assertEqual(struct.unpack_from('<H', stream, offset + 2)[0] & 0x7ff, n ^ 0x7ff)
            offset += 4 + n + (n & 1)
            frames += 1
        self.assertEqual((offset, frames), (len(stream), 5))
        self.assertTrue(any(struct.unpack_from('<H', d)[0] & 0x7ff != len(d) - 4 for d in transfers))

    def test_bmcr_reports_resolved_mode(self):
        """After autonegotiation BMCR reads with the resolved speed and duplex;
        before it, and with autonegotiation off, as written."""
        self.setup()
        chip = self.m.chip
        chip.sw_mii = True
        self.assertEqual(chip.phy_read(a.BMCR) & 0x2100, 0)
        self.settle(1.1)
        self.assertTrue(chip.link)
        self.assertEqual(chip.phy_read(a.BMCR) & 0x2100, 0x2100)
        chip.half = True
        self.assertEqual(chip.phy_read(a.BMCR) & 0x2100, 0x2000)
        chip.mbps, chip.half = 10, False
        self.assertEqual(chip.phy_read(a.BMCR) & 0x2100, 0x0100)
        chip.phy[a.BMCR] = 0x0100                                   # forced 10 full
        self.assertEqual(chip.phy_read(a.BMCR), 0x0100)

    def test_transmit_offload(self):
        """Without bit 15 a record gets the checksums TXCOE asks for (a
        fragment only its IPv4 header); bit 15, TXCOE 0 and Linux's form
        leave it as sent."""
        self.up()
        udp = a.ipv4(bytes((192, 168, 77, 2)), a.HOST_IP, 17, struct.pack('!HHHH', 68, 67, 12, 0x1234) + b'abcd')
        frame = bytearray(a.HOST_MAC + a.MODEL_MAC + b'\x08\x00' + udp)
        frame[24:26] = b'\x12\x34'                                   # a wrong IPv4 header checksum
        frame = bytes(frame)
        frag = bytearray(frame)
        frag[20:22] = b'\x20\x00'                                    # MF: a first fragment

        def sent(data, flags, coe):
            self.m.chip.txcoe = coe
            got = []
            self.m.transmit_frame, keep = (lambda f, p: got.append(f) or f), self.m.transmit_frame
            try:
                n = len(data)
                self.m.transmit(struct.pack('<I', ((n ^ 0x7ff) << 16) | flags | n) + data + (b'\0' if n & 1 else b''))
            finally:
                self.m.transmit_frame = keep
            return a.checksum_state(got[0])
        self.assertEqual(sent(frame, 0, 0x0307), 'ip-ok-l4-ok')
        self.assertEqual(sent(frame, 0x4000, 0x0307), 'ip-ok-l4-ok')
        self.assertEqual(sent(frame, 0, 0x0001), 'ip-ok-l4-bad')
        self.assertEqual(sent(frame, 0x8000, 0x0307), 'ip-bad-l4-bad')
        self.assertEqual(sent(frame, 0, 0), 'ip-bad-l4-bad')
        self.assertEqual(sent(bytes(frag), 0, 0x0307), 'ip-ok')
        self.assertEqual(self.m.totals['tx_as_sent_8000_ip-bad-l4-bad'], 1)

    def test_tcp_host(self):
        """The simulated host's TCP through the model: a SYN to the echo port
        is answered SYN-ACK with MSS 1460; another port answers RST."""
        self.up()
        guest = bytes((192, 168, 77, 2))

        def syn(port):
            head = struct.pack('!HHIIBBHHH', 4000, port, 100, 0, 6 << 4, 0x02, 65535, 0, 0) + \
                struct.pack('!BBH', 2, 4, 1460)
            pseudo = guest + a.HOST_IP + struct.pack('!BBH', 0, 6, len(head))
            head = head[:16] + struct.pack('!H', a.checksum(pseudo + head)) + head[18:]
            return a.HOST_MAC + a.MODEL_MAC + b'\x08\x00' + a.ipv4(guest, a.HOST_IP, 6, head)
        answers = []
        self.m.deliver, keep = (lambda f: answers.append(f)), self.m.deliver
        try:
            self.m.transmit_frame(syn(7), False)
            self.m.transmit_frame(syn(23), False)
        finally:
            self.m.deliver = keep
        self.assertEqual(len(answers), 2)
        flags = [f[14 + 20 + 13] for f in answers]
        self.assertEqual(flags, [0x12, 0x14])
        self.assertEqual(struct.unpack_from('!I', answers[0], 14 + 20 + 8)[0], 101)
        self.assertEqual(answers[0][14 + 20 + 20:14 + 20 + 24], struct.pack('!BBH', 2, 4, 1460))
        self.assertEqual(self.events('tcp_open')[0]['port'], 7)

    def packets(self, data, zlp=True):
        """One transfer as a UHCI host at Full Speed passes it: a usbredir
        packet per 64 bytes, and a zero-length one after a full last packet."""
        for at in range(0, len(data), 64):
            piece = data[at:at + 64]
            self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, len(piece), 0, 0) + piece)
        if zlp and len(data) % 64 == 0:
            self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, 0, 0, 0))

    def driver_transfer(self, frame):
        out = (c.c_ubyte * (4 + 1514 + 4))()
        self.chip.TxHeader(self.state, out, len(frame), self.probe.OutMax, 1)
        c.memmove(c.byref(out, 4), frame, len(frame))
        total = c.c_ulong(4 + len(frame))
        self.assertEqual(self.chip.TxEnd(self.state, out, c.byref(total), len(out), self.probe.OutMax), 0)
        return bytes(out)[:total.value]

    def test_full_speed_packet_at_a_time(self):
        """ax772f's transfers a packet at a time (UHCI at Full Speed) are joined
        up until the short packet; 60 bytes + 4 on a boundary carry the pad word."""
        for chip in ('772b', 'sr9800'):
            self.up('full', chip=chip)
            sent = []
            real = self.m.transmit
            self.m.transmit = lambda data: sent.append(data) or real(data)
            sizes = (60, 61, 124, 333, 1514)
            for size in sizes:
                frame = a.HOST_MAC + a.MODEL_MAC + b'\x88\xb5' + bytes((size & 255,)) * (size - 14)
                data = self.driver_transfer(frame)
                self.packets(data, zlp=False)
                self.assertEqual(sent[-1], data, (chip, size))
            self.assertEqual(self.m.totals['tx_frames'], len(sizes), chip)
            self.assertEqual(bytes(self.m.tx_buf), b'')

    def test_packet_join_asix_form_and_limits(self):
        """The 7 driver's records a packet at a time, a ZLP after a full last
        packet, a reselected interface dropping a part-joined transfer, a
        refused one keeping it, and the bound."""
        self.up('full')
        arp = arp_request(a.MODEL_MAC) + bytes(18)                    # 60 bytes: a 64-byte record
        long = arp + bytes(64)                                        # 124 bytes: a 128-byte record
        recs = b''
        for frame in (arp, long):
            n = len(frame)
            recs += struct.pack('<I', ((n ^ 0x7ff) << 16) | 0x8000 | n) + frame
        self.assertEqual(len(recs), 192)
        self.packets(recs[:64], zlp=False)                            # a whole record: taken at once
        self.assertEqual(self.m.totals['tx_records_asix_form'], 1)
        self.packets(recs[64:128], zlp=False)                         # half a record: joined
        self.assertEqual(self.m.totals['tx_records_asix_form'], 1)
        self.packets(recs[128:], zlp=True)                            # its end, then a ZLP
        self.assertEqual(self.m.totals['tx_records_asix_form'], 2)
        self.assertEqual(bytes(self.m.tx_buf), b'')
        self.packets(recs[64:128], zlp=False)
        self.m.handle(9, 4, b'\0\1')                                 # refused: the transfer stays
        self.packets(recs[128:])
        self.assertEqual(self.m.totals['tx_records_asix_form'], 3)
        self.packets(recs[64:128], zlp=False)
        self.m.handle(9, 4, b'\0\0')                                 # accepted: it is dropped
        self.assertEqual(bytes(self.m.tx_buf), b'')
        self.tx(arp_request(a.MODEL_MAC))
        self.assertEqual(self.m.totals['tx_frames'], 4)
        n = 60
        linux = struct.pack('<I', ((n ^ 0xffff) << 16) | n) + bytes(60)  # 64 bytes, then more: too long
        self.packets(linux[:64], zlp=False)
        with self.assertRaises(ValueError):
            self.packets(bytes(64), zlp=False)
        self.m.tx_buf = bytearray(a.TX_JOIN_MAX - 32)
        with self.assertRaises(ValueError):                          # the bound
            self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, 64, 0, 0) + recs[:64])

    def test_high_speed_whole_transfers_on_a_boundary(self):
        """At High Speed each transfer comes whole: an ASIX-form transfer of
        exactly 512 bytes with no ZLP is taken at once, again and again."""
        self.up()
        n = 508
        frame = a.HOST_MAC + a.MODEL_MAC + b'\x88\xb5' + bytes(n - 14)
        rec = struct.pack('<I', ((n ^ 0x7ff) << 16) | 0x8000 | n) + frame
        self.assertEqual(len(rec), 512)
        for i in range(200):
            self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, len(rec), 0, 0) + rec)
        self.assertEqual(self.m.totals['tx_records_asix_form'], 200)
        self.assertEqual(bytes(self.m.tx_buf), b'')
        m = 300
        short = struct.pack('<I', ((m ^ 0x7ff) << 16) | 0x8000 | m) + frame[:m]
        two = short + rec                                           # 304 + 512: cut at 512, wait
        self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, 512, 0, 0) + two[:512])
        self.assertEqual(self.m.totals['tx_records_asix_form'], 200)
        self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, len(two) - 512, 0, 0) + two[512:])
        self.assertEqual(self.m.totals['tx_records_asix_form'], 202)
        pad = b'\x00\x00\xff\xff'
        for size in (512, 64):                                      # 512 + 4 at High Speed, 64 + 4 at Full
            if size == 64:
                self.up('full')
            k = size - 4
            r = struct.pack('<I', ((k ^ 0x7ff) << 16) | 0x8000 | k) + frame[:k]
            before = self.m.totals['tx_records_asix_form'], self.m.totals['tx_padded']
            self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, size, 0, 0) + r)
            self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, 4, 0, 0) + pad)
            self.assertEqual((self.m.totals['tx_records_asix_form'], self.m.totals['tx_padded']),
                             (before[0] + 1, before[1] + 1), size)
            self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, size, 0, 0) + r)   # no pad: fine too
            self.assertEqual(self.m.totals['tx_records_asix_form'], before[0] + 2)
        self.tx(arp_request(a.MODEL_MAC))                           # another transfer in between
        with self.assertRaises(ValueError):                         # then a pad word alone
            self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, 4, 0, 0) + pad)


class TcpHostTests(unittest.TestCase):
    """tcphost.Host segment by segment: the recovery paths a guest leg seldom takes."""
    GUEST = bytes((192, 168, 77, 2))

    def setUp(self):
        self.t = [0.0]
        self.logs = []
        self.h = tcphost.Host(lambda ev, **f: self.logs.append((ev, f)), lambda: self.t[0])

    def seg(self, port, seq, ack, flags, payload=b'', win=65535):
        head = struct.pack('!HHIIBBHHH', 4000, port, seq, ack, 5 << 4, flags, win, 0, 0) + payload
        pseudo = self.GUEST + a.HOST_IP + struct.pack('!BBH', 0, 6, len(head))
        head = head[:16] + struct.pack('!H', a.checksum(pseudo + head)) + head[18:]
        out = self.h.handle(a.HOST_MAC + a.MODEL_MAC + b'\x08\x00' + a.ipv4(self.GUEST, a.HOST_IP, 6, head))
        return [struct.unpack_from('!IIBBH', f, 38) + (f[54:14 + struct.unpack_from('!H', f, 16)[0]],)
                for f in out]

    def open(self, port, win=65535):
        (iss, ack, _, flags, _, _), = self.seg(port, 1000, 0, tcphost.SYN)
        self.assertEqual((flags, ack), (0x12, 1001))
        return iss + 1, self.seg(port, 1001, iss + 1, tcphost.ACK, win=win)

    def test_echo_window_bounds_the_queue(self):
        una, _ = self.open(tcphost.ECHO, win=0)                  # the guest reads nothing
        seq, offered = 1001, []
        for _ in range(8):
            out = self.seg(tcphost.ECHO, seq, una, tcphost.ACK, bytes(40000), win=0)
            seq = out[-1][1]
            offered.append(out[-1][4])
        conn = next(iter(self.h.conns.values()))
        self.assertEqual(len(conn.out), tcphost.ECHO_QUEUE)
        self.assertEqual(seq, 1001 + tcphost.ECHO_QUEUE)
        self.assertEqual(offered[-1], 0)

    def test_zero_window_probe(self):
        una, _ = self.open(tcphost.ECHO, win=0)
        self.seg(tcphost.ECHO, 1001, una, tcphost.ACK, b'hello', win=0)
        self.t[0] += 1.5
        probe = self.h.poll()
        self.assertEqual(len(probe), 1)
        self.assertEqual(probe[0][54:55], b'h')
        out = self.seg(tcphost.ECHO, 1006, una, tcphost.ACK, win=4096)  # the window opens
        self.assertEqual(b''.join(o[5] for o in out), b'ello')
        self.assertEqual(self.logs[-1][0], 'tcp_window_probe')

    def test_stale_reset_ignored(self):
        una, _ = self.open(tcphost.DISCARD)
        self.seg(tcphost.DISCARD, 1001 + 200000, 0, tcphost.RST)
        self.assertEqual(len(self.h.conns), 1)
        self.seg(tcphost.DISCARD, 1001, 0, tcphost.RST)
        self.assertEqual(len(self.h.conns), 0)

    def test_time_wait_acks_a_repeated_fin(self):
        una, _ = self.open(tcphost.HTTP)
        req = b'GET /source?n=10 HTTP/1.1\r\n\r\n'
        out = self.seg(tcphost.HTTP, 1001, una, tcphost.ACK, req)
        data = b''.join(o[5] for o in out)
        self.assertTrue(data.endswith(tcphost.source_pattern(0, 10)))
        fin_seq = una + len(data)
        self.assertTrue(out[-1][3] & tcphost.FIN)
        seq = 1001 + len(req)
        self.seg(tcphost.HTTP, seq, fin_seq + 1, tcphost.ACK)
        self.seg(tcphost.HTTP, seq, fin_seq + 1, tcphost.FIN | tcphost.ACK)
        self.assertEqual(len(self.h.conns), 0)
        self.assertEqual(self.seg(tcphost.HTTP, 1000, 0, tcphost.SYN), [])       # a delayed duplicate SYN
        self.assertEqual(len(self.h.conns), 0)
        again = self.seg(tcphost.HTTP, seq, fin_seq + 1, tcphost.FIN | tcphost.ACK)   # its ACK was lost
        self.assertEqual([(o[0], o[1], o[3]) for o in again], [(fin_seq + 1, seq + 1, tcphost.ACK)])
        self.t[0] += tcphost.TIME_WAIT_S + 1
        late = self.seg(tcphost.HTTP, seq, fin_seq + 1, tcphost.FIN | tcphost.ACK)
        self.assertEqual(late[0][3], tcphost.RST)


if __name__ == '__main__':
    unittest.main()
