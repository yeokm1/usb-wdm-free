#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests of axemu.py: the SIMULATED AX88179 driven by ax88179's own
chip core (axchip.c, axframe.c and netchip.c, built into
out/net/ax88179/hostabi/axchipcore.dll by simhost.ps1) through the model's
usbredir control, bulk and interrupt paths, as rtlsimtest.py does for
rtl815x. No physical USB device.
Run: python -B axsimtest.py
"""
import ctypes as c
import io
import json
import struct
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import axemu as a  # noqa: E402

DLL_PATH = Path(__file__).resolve().parents[3] / 'out/net/ax88179/hostabi/axchipcore.dll'

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
        'RxConfig', 'RxConfigRead', 'Offload', 'LsoMax')]


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
    ('TxEnd', c.CFUNCTYPE(c.c_int, c.c_void_p, U8P, P(c.c_ulong), c.c_ulong, c.c_ulong))]})

FILTER_DIRECTED, FILTER_MULTICAST, FILTER_ALL_MULTICAST, FILTER_BROADCAST, FILTER_PROMISCUOUS = 1, 2, 4, 8, 0x20
SPEEDS = {'super': 3, 'high': 2, 'full': 1}           # NET_PROBE Speed


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


class CoreCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.chip = NetChip.in_dll(c.CDLL(str(DLL_PATH)), 'NetPackageChip')

    def setup(self, speed='super', **kw):
        self.wire, self.trace, self.clock = Wire(), io.StringIO(), Clock()
        self.m = a.Model(self.wire, self.trace, speed, now=self.clock, **kw)
        self.m.peer_caps = a.CAPS & ~(1 << a.CAP_ID64)
        self.m.connect()
        self.m.handle(6, 1, b'\x01')
        self.wire.messages.clear()
        self.state = (c.c_ubyte * 512)()
        self.ident = 100

        def control(_, inp, request, value, index, data, length, actual):
            self.ident += 1
            rt = 0xc0 if inp else 0x40
            payload = b'' if inp else c.string_at(data, length)
            self.m.control(self.ident, struct.pack('<BBBBHHH', 0x80 if inp else 0, request, rt, 0,
                                                   value, index, length) + payload)
            kind, ident, reply = self.wire.messages.pop()
            if (kind, ident) != (100, self.ident) or reply[3]:
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

        self.io = NetIo(None, CONTROL(control), SLEEP(sleep), 0, 0)
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
        p.Speed = SPEEDS[self.m.speed]       # the bus reports the speed; the probe reads descriptors only
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
        """The driver's TxHeader framing: one frame per bulk-OUT transfer."""
        if len(frame) < 60:
            frame += bytes(60 - len(frame))
        h = (c.c_ubyte * 8)()
        self.chip.TxHeader(self.state, h, len(frame), self.probe.OutMax, 1)
        out = bytes(h) + frame
        self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, len(out), 0, 0) + out)
        self.assertEqual(self.wire.take(101)[-1][2][:2], bytes((a.EP_OUT, 0)))

    def rx(self, length=None):
        """One bulk-IN request of the driver's size, parsed by its RxParse."""
        length = length or self.params.RxSize
        self.m.handle(101, 9, struct.pack('<BBHIH', a.EP_IN, 0, length & 0xffff, 0, length >> 16))
        replies = self.wire.take(101)
        if not replies:
            return None, None
        data = replies[-1][2][10:]
        frames, stats = [], RxStats()
        emit = EMIT(lambda _, p, n: frames.append(c.string_at(p, n)))
        rc = self.chip.RxParse(self.state, buf(data), len(data), emit, None, c.byref(stats))
        self.assertEqual(rc, len(frames))
        return frames, stats

    def up(self, speed='super', **kw):
        self.assertEqual(self.setup(speed, **kw), 0)
        self.settle(1.1)
        link = self.link()
        self.assertTrue(link.Up)
        return link


def arp_request(mac):
    return b'\xff' * 6 + mac + b'\x08\x06' + struct.pack('!HHBBH', 1, 0x0800, 6, 4, 1) + \
        mac + a.GUEST_IP + bytes(6) + a.HOST_IP


def echo_request(mac, size):
    icmp = b'\x08\0\0\0' + b'\x12\x34\0\1' + bytes((i * 7) & 255 for i in range(size))
    icmp = icmp[:2] + struct.pack('!H', a.checksum(icmp)) + icmp[4:]
    return a.HOST_MAC + mac + b'\x08\x00' + a.ipv4(a.GUEST_IP, a.HOST_IP, 1, icmp), icmp


def dhcp_discover(mac, options=bytes((53, 1, 1, 255))):
    boot = bytearray(240)
    boot[0:4], boot[4:8], boot[28:34] = b'\x01\x01\x06\x00', b'\xaa\xbb\xcc\xdd', mac
    boot[236:240] = b'\x63\x82\x53\x63'
    boot += options
    udp = struct.pack('!HHHH', 68, 67, 8 + len(boot), 0) + bytes(boot)
    return b'\xff' * 6 + mac + b'\x08\x00' + a.ipv4(b'\0' * 4, b'\xff' * 4, 17, udp)


@unittest.skipUnless(DLL_PATH.exists(), 'run simhost.ps1 first')
class Tests(CoreCase):
    def test_descriptors_super_speed_are_the_capture(self):
        self.setup('super')
        cap = Path(__file__).resolve().parent / 'testdata' / 'ax179'
        self.assertEqual(self.m.device, (cap / 'device.bin').read_bytes())
        self.assertEqual(self.m.config_desc, (cap / 'config0.bin').read_bytes())

    def test_probe_and_bring_up_at_every_speed(self):
        # (USB speed, the queue row Link programs at 1000 Mb/s, axchip.c BulkIn)
        for speed, row in (('super', bytes((7, 0x4f, 0, 0x12, 0xff))), ('high', bytes((7, 0x20, 3, 0x16, 0xff))),
                           ('full', bytes((7, 0xcc, 0x4c, 0x18, 8)))):
            with self.subTest(speed=speed):
                self.assertEqual(self.setup(speed), 0)
                p = self.probe
                self.assertEqual((p.In, p.Out, p.Interrupt, p.Vendor, p.Product, p.Revision),
                                 (a.EP_IN, a.EP_OUT, a.EP_INT, 0x0b95, 0x1790, 0x0100))
                self.assertEqual(p.InMax, a.bulk_size(speed))
                self.assertEqual(bytes(self.params.Mac), a.MODEL_MAC)
                self.assertEqual(self.io.Status, 0)
                self.assertFalse(self.events('vendor_refused'))
                self.assertGreater(self.m.totals['vendor_requests'], 30)
                self.assertGreaterEqual(self.m.chip.restarts, 2)      # settle, power-up, autonegotiation
                self.assertFalse(self.m.chip.link)
                self.assertFalse(self.link().Up)
                self.settle(1.1)
                self.assertEqual(self.chip.Poll(self.state, c.byref(self.io)), 1)
                link = self.link()
                self.assertEqual((link.Up, link.Mbps, link.FullDuplex), (1, 1000, 1))
                self.assertEqual(bytes(self.m.chip.mac_regs[a.R_QCTRL:a.R_QCTRL + 5]), row)
                self.assertEqual(link.RxLength, 1024 * (row[3] + 2))
                self.assertTrue(self.m.chip.receiving())

    def test_slower_links(self):
        for mbps, row in ((100, bytes((7, 0xae, 7, 0x18, 0xff))), (10, bytes((7, 0xcc, 0x4c, 0x18, 8)))):
            with self.subTest(mbps=mbps):
                link = self.up('high', mbps=mbps)
                self.assertEqual((link.Mbps, link.FullDuplex), (mbps, 1))
                self.assertEqual(bytes(self.m.chip.mac_regs[a.R_QCTRL:a.R_QCTRL + 5]), row)

    def test_interrupt_reports_link_changes(self):
        self.assertEqual(self.setup(flap_after=3.0, flap_for=1.0), 0)
        self.m.handle(15, 3, bytes((a.EP_INT,)))
        self.assertEqual(self.wire.take(17)[-1][2], bytes((0, a.EP_INT)))
        self.settle(1.1)
        data = self.wire.take(103)[-1][2][4:]
        self.assertEqual(len(data), 8)
        self.assertEqual(self.chip.Status(self.state, buf(data), len(data)), 1)
        self.assertTrue(self.link().Up)
        self.settle(3.1)                              # cable out
        data = self.wire.take(103)[-1][2][4:]
        self.assertEqual(self.chip.Status(self.state, buf(data), len(data)), 0)
        self.assertFalse(self.link().Up)
        self.settle(1.1)                              # cable in
        data = self.wire.take(103)[-1][2][4:]
        self.assertEqual(self.chip.Status(self.state, buf(data), len(data)), 1)
        self.assertTrue(self.link().Up)
        self.assertEqual([e['why'] for e in self.events('link')][-3:], ['autoneg_done', 'cable_out', 'cable_in'])

    def test_arp_icmp_dhcp_through_the_driver_framing(self):
        self.up()
        self.assertEqual(self.chip.SetFilter(self.state, c.byref(self.io),
                                             FILTER_DIRECTED | FILTER_BROADCAST, None, 0), 0)
        mac = a.MODEL_MAC
        self.tx(arp_request(mac))
        frames, stats = self.rx()
        self.assertEqual(len(frames), 1)
        self.assertEqual((frames[0][:6], frames[0][6:12], frames[0][12:14]), (mac, a.HOST_MAC, b'\x08\x06'))
        self.assertEqual(frames[0][20:22], b'\0\2')
        ping, icmp = echo_request(mac, 1472 - 8)
        self.tx(ping)
        self.tx(dhcp_discover(mac))
        frames, stats = self.rx()                     # both answers in one transfer
        self.assertEqual(len(frames), 2)
        echo, offer = frames
        self.assertEqual(len(echo), 14 + 20 + len(icmp))            # a full 1514-byte frame
        self.assertEqual(echo[14 + 20], 0)
        self.assertEqual(echo[14 + 28:], icmp[8:])
        self.assertEqual(offer[14 + 28 + 16:14 + 28 + 20], a.GUEST_IP)
        self.assertIn(bytes((53, 1, 2)), offer)
        self.assertEqual((stats.Dropped, stats.FcsStripped), (0, 0))

    def test_aggregate_layout_and_fcs(self):
        for fcs in (False, True):
            with self.subTest(fcs=fcs):
                self.up(fcs=fcs)
                for size in (0, 18, 600):
                    self.tx(echo_request(a.MODEL_MAC, size)[0])
                frames, stats = self.rx()
                self.assertEqual(len(frames), 3)
                self.assertEqual(stats.FcsStripped, 3 if fcs else 0)
                self.assertEqual(stats.Dropped, 0)

    def test_a_transfer_never_overruns_the_request(self):
        self.up()
        for _ in range(30):
            self.tx(echo_request(a.MODEL_MAC, 1464)[0])
        got = 0
        while self.m.rx or self.m.totals['rx_frames'] > got:
            frames, _ = self.rx()
            if frames is None:
                break
            got += len(frames)
            self.assertLessEqual(len(frames) * 1520, self.params.RxSize)
            if not self.m.rx:
                break
        self.assertEqual(got, 30)

    def test_receive_filter_follows_the_driver(self):
        self.up()
        group, other = bytes.fromhex('01005e000001'), bytes.fromhex('01005e7ffffa')
        self.assertEqual(self.chip.SetFilter(self.state, c.byref(self.io),
                                             FILTER_DIRECTED | FILTER_BROADCAST | FILTER_MULTICAST,
                                             buf(group), 1), 0)
        acc = self.m.chip.accepts
        self.assertTrue(acc(b'\xff' * 6) and acc(a.MODEL_MAC) and acc(group))
        self.assertFalse(acc(other) or acc(bytes.fromhex('020000000009')))
        self.assertEqual(self.chip.SetFilter(self.state, c.byref(self.io), FILTER_PROMISCUOUS, None, 0), 0)
        self.assertTrue(acc(other) and acc(bytes.fromhex('020000000009')))
        self.assertEqual(self.chip.SetFilter(self.state, c.byref(self.io), FILTER_ALL_MULTICAST, None, 0), 0)
        self.assertTrue(acc(other) and not acc(bytes.fromhex('020000000009')))
        new = bytes.fromhex('02aabbccddee')
        self.assertEqual(self.chip.SetMac(self.state, c.byref(self.io), buf(new)), 0)
        self.assertTrue(acc(new) and not acc(a.MODEL_MAC))

    def test_padding_flag_at_a_packet_boundary(self):
        self.up('high')
        size = 512 - 8 - 14 - 20 - 8                  # header + frame = 512 bytes
        frame, _ = echo_request(a.MODEL_MAC, size)
        self.assertEqual(len(frame) + 8, 512)
        self.tx(frame)
        self.assertEqual(self.m.totals['tx_frames'], 1)
        with self.assertRaises(ValueError):           # a wrong length in the header is refused
            self.m.transmit(struct.pack('<II', 100, 0) + bytes(60))
        with self.assertRaises(ValueError):           # flags other than 0 or the padding flag
            self.m.transmit(struct.pack('<II', 504, 0x1234) + bytes(504))
        frame, _ = echo_request(a.MODEL_MAC, 504 - 14 - 20 - 8)     # an unmarked 4-aligned record
        before = self.m.totals['tx_frames']                          # that fills a packet, flags 0
        self.m.handle(101, 8, struct.pack('<BBHIH', a.EP_OUT, 0, 512, 0, 0) + struct.pack('<II', 504, 0) + frame)
        self.assertEqual(self.m.totals['tx_frames'], before + 1)

    def test_linux_pad_byte_and_packet_at_a_time(self):
        """Linux's usbnet adds one zero byte when header and frame end on a
        packet boundary; a Full Speed host on UHCI sends a packet at a time."""
        self.up('high')
        frame, _ = echo_request(a.MODEL_MAC, 512 - 8 - 14 - 20 - 8)
        self.m.transmit(struct.pack('<II', len(frame), a.TX_PAD) + frame + b'\0')
        self.assertEqual((self.m.totals['tx_frames'], self.m.totals['tx_padded']), (1, 1))
        with self.assertRaises(ValueError):           # bytes past the frame and its padding to 4
            self.m.transmit(struct.pack('<II', len(frame) - 1, 0) + frame[:-1] + bytes(6))
        self.up('full')
        for size in (64 - 8 - 14 - 20 - 8, 100, 1000):
            frame, _ = echo_request(a.MODEL_MAC, size)
            pad = (len(frame) + 8) % 64 == 0
            data = struct.pack('<II', len(frame), a.TX_PAD if pad else 0) + frame + (b'\0' if pad else b'')
            for at in range(0, len(data), 64):
                piece = data[at:at + 64]
                self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, len(piece), 0, 0) + piece)
        self.assertEqual((self.m.totals['tx_frames'], self.m.totals['tx_padded']), (3, 1))
        self.assertEqual(self.m.net.refused, 0)

    def test_asix_form_on_a_packet_boundary_is_taken_without_a_zlp(self):
        """ASIX's XP form ending exactly on a packet boundary is sent through
        handle() as one transfer with no ZLP: taken at once; a ZLP after it is
        absorbed; a record cut off at a boundary still waits for the rest."""
        for speed, size in (('high', 512), ('super', 1024)):
            self.up(speed)
            frame, _ = echo_request(a.MODEL_MAC, size - 8 - 14 - 20 - 8)
            data = struct.pack('<II', 0x80000000 | len(frame), 0) + frame
            self.assertEqual(len(data), size)
            self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, len(data), 0, 0) + data)
            self.assertEqual(self.m.totals['tx_frames'], 1)
            frames, _ = self.rx()
            self.assertEqual(len(frames), 1)          # the echo answered without a later transfer
            self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, 0, 0, 0))
            self.assertEqual(self.m.totals['tx_frames'], 1)
            self.assertFalse(self.m.tx_buf)
        self.up('full')
        frame, _ = echo_request(a.MODEL_MAC, 100)
        data = struct.pack('<II', 0x80000000 | len(frame), 0) + frame
        data += bytes((-len(data)) % 4)
        self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, 64, 0, 0) + data[:64])
        self.assertEqual(self.m.totals['tx_frames'], 0)
        rest = data[64:]
        self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, len(rest), 0, 0) + rest)
        self.assertEqual(self.m.totals['tx_frames'], 1)

    def test_interface_reselection_drops_a_cut_off_transfer(self):
        self.up('full')
        frame, _ = echo_request(a.MODEL_MAC, 150)
        data = struct.pack('<II', len(frame), 0) + frame
        self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, 64, 0, 0) + data[:64])
        self.m.handle(9, 4, b'\0\0')
        self.assertEqual(self.wire.take(11)[-1][2][0], 0)
        small, _ = echo_request(a.MODEL_MAC, 10)
        self.tx(small)
        self.assertEqual(self.m.totals['tx_frames'], 1)

    def test_joined_transfer_is_bounded(self):
        self.up('full')
        packet = struct.pack('<II', 0x80000000 | 1000, 0) + bytes(56)
        with self.assertRaises(ValueError):
            for _ in range(a.TX_JOIN_MAX // 64 + 2):
                self.m.receive_out(packet if not self.m.tx_buf else bytes(64))
        self.up('full')
        with self.assertRaises(ValueError):          # a Linux-form length no frame has
            self.m.receive_out(struct.pack('<II', 0x7fffffff, 0) + bytes(56))

    def test_padded_unmarked_records_and_checksum_insertion(self):
        """ASIX's 7 and 3.20/4.20 drivers and Windows 10's own: records
        padded to 4, bit 31 on some only, several per transfer; with TXCOE
        set the chip fills the checksums of the unmarked IPv4 ones."""
        self.up('high')
        disc = bytearray(dhcp_discover(a.MODEL_MAC))
        disc[14 + 10:14 + 12] = b'\0\0'                     # IPv4 header checksum left to the chip
        disc[14 + 20 + 6:14 + 20 + 8] = b'\x12\x34'         # a wrong UDP checksum, to be replaced
        ping, _ = echo_request(a.MODEL_MAC, 30)
        arp = arp_request(a.MODEL_MAC)

        def record(frame, mark):
            body = frame + bytes(-len(frame) % 4)
            return struct.pack('<II', len(frame) | (0x80000000 if mark else 0), 0) + body
        data = record(arp, True) + record(bytes(disc), False) + record(ping, False)
        self.m.chip.mac_regs[a.R_TXCOE] = 0x67
        self.m.handle(101, 7, struct.pack('<BBHIH', a.EP_OUT, 0, len(data), 0, 0) + data)
        self.assertEqual(self.m.totals['tx_frames'], 3)
        self.assertEqual((self.m.totals['tx_records_asix_form'], self.m.totals['tx_records_padded_unmarked']), (1, 2))
        self.assertEqual((self.m.totals['tx_csum_ip'], self.m.totals['tx_csum_udp']), (2, 1))
        self.assertEqual(self.m.net.refused, 0)
        self.assertTrue([e for e in self.events('dhcp')])
        self.m.chip.mac_regs[a.R_TXCOE] = 0x67           # UDP shorter than its IP payload: its own length
        tail = bytearray(dhcp_discover(a.MODEL_MAC))
        tail += b'\xee' * 4
        tail[14 + 2:14 + 4] = struct.pack('!H', len(tail) - 14)
        tail[14 + 10:14 + 12] = b'\0\0'
        before = self.m.net.refused
        self.m.transmit(record(bytes(tail), False))
        self.assertEqual(self.m.net.refused, before)
        self.up('high')                                  # TXCOE 0 (ax179f): nothing is filled
        self.m.transmit(record(bytes(disc), False))
        self.assertEqual((self.m.totals['tx_csum_ip'], self.m.net.refused), (0, 1))

    def test_corrupted_requests_get_no_answer(self):
        self.up()
        ping, icmp = echo_request(a.MODEL_MAC, 32)
        bad_ip = bytearray(ping)
        bad_ip[14 + 10] ^= 1                          # the IPv4 header checksum
        bad_icmp = bytearray(ping)
        bad_icmp[14 + 20 + 2] ^= 1                    # the ICMP checksum
        bad_len = bytearray(ping)
        bad_len[14 + 2:14 + 4] = struct.pack('!H', 1500)    # beyond the frame
        disc = bytearray(dhcp_discover(a.MODEL_MAC))
        disc[14 + 20 + 6:14 + 20 + 8] = b'4'          # a nonzero, wrong UDP checksum
        short_udp = bytearray(dhcp_discover(a.MODEL_MAC))
        short_udp[14 + 20 + 4:14 + 20 + 6] = b'\0\0'        # UDP length 0, IPv4 header still valid
        long_udp = bytearray(dhcp_discover(a.MODEL_MAC))
        long_udp[14 + 20 + 4:14 + 20 + 6] = b'\xff\xff'     # beyond the datagram
        truncated = dhcp_discover(a.MODEL_MAC, options=bytes((53, 1)))   # the option's value missing
        for frame in (bad_ip, bad_icmp, bad_len, disc, short_udp, long_udp, truncated):
            self.tx(bytes(frame))
        self.assertFalse(self.m.rx)
        self.assertEqual(self.m.net.refused, 7)
        self.tx(dhcp_discover(a.MODEL_MAC))          # a valid request after them is answered
        frames, _ = self.rx()
        self.assertEqual(len(frames), 1)
        self.tx(ping)                                 # the same request intact is answered
        frames, _ = self.rx()
        self.assertEqual(len(frames), 1)

    def test_fragmented_ping_is_reassembled_and_answered_in_fragments(self):
        """ping -l 4000: the guest sends three fragments; the host answers in three."""
        self.up()
        _, icmp = echo_request(a.MODEL_MAC, 4000)
        packets = a.ipv4_fragments(a.GUEST_IP, a.HOST_IP, 1, icmp, 0x1234)
        self.assertEqual(len(packets), 3)
        for packet in packets[:2]:
            self.tx(a.HOST_MAC + a.MODEL_MAC + b'\x08\x00' + packet)
            self.assertFalse(self.m.rx)                  # nothing until the datagram is whole
        self.tx(a.HOST_MAC + a.MODEL_MAC + b'\x08\x00' + packets[2])
        got = []
        while len(got) < 3:
            frames, stats = self.rx()
            self.assertIsNotNone(frames)
            got += frames
        self.assertEqual(len(got), 3)
        parts = {}
        for f in got:
            ip = f[14:]
            frag = struct.unpack('!H', ip[6:8])[0]
            self.assertEqual(a.checksum(ip[:20]), 0)
            self.assertLessEqual(len(ip), 1500)
            parts[(frag & 0x1fff) * 8] = ip[20:struct.unpack('!H', ip[2:4])[0]]
        reply = b''.join(parts[k] for k in sorted(parts))
        self.assertEqual(len(reply), len(icmp))
        self.assertEqual(reply[0], 0)                    # echo reply
        self.assertEqual(a.checksum(reply), 0)
        self.assertEqual(reply[8:], icmp[8:])
        # A lost middle fragment leaves the datagram unanswered.
        for packet in (packets[0], packets[2]):
            self.tx(a.HOST_MAC + a.MODEL_MAC + b'\x08\x00' + a.ipv4_fragments(
                a.GUEST_IP, a.HOST_IP, 1, icmp, 0x1235)[[packets[0], packets[2]].index(packet) * 2])
        self.assertFalse(self.m.rx)

    def test_reassembly_keys_expiry_and_bounds(self):
        self.up()
        _, icmp = echo_request(a.MODEL_MAC, 4000)
        eth = a.HOST_MAC + a.MODEL_MAC + b'\x08\x00'
        elsewhere = bytes((192, 168, 77, 9))
        # The first fragment to another host cannot complete the host's datagram.
        first = a.ipv4_fragments(a.GUEST_IP, elsewhere, 1, icmp, 0x2000)[0]
        rest = a.ipv4_fragments(a.GUEST_IP, a.HOST_IP, 1, icmp, 0x2000)[1:]
        for packet in [first] + rest:
            self.tx(eth + packet)
        self.assertFalse(self.m.rx)
        # A datagram left incomplete past the timer is dropped; its id reused later is whole again.
        parts = a.ipv4_fragments(a.GUEST_IP, a.HOST_IP, 1, icmp, 0x2001)
        self.tx(eth + parts[0])
        self.clock.t += 31
        for packet in parts[1:]:
            self.tx(eth + packet)
        self.assertFalse(self.m.rx)
        self.assertTrue(self.events('reassembly_timeout'))
        for packet in parts:
            self.tx(eth + packet)
        frames, _ = self.rx()
        self.assertEqual(len(frames), 3)
        # No more than REASSEMBLY_DATAGRAMS wait at once; an overlap drops the datagram.
        for ident in range(0x3000, 0x3000 + 20):
            self.tx(eth + a.ipv4_fragments(a.GUEST_IP, a.HOST_IP, 1, icmp, ident)[0])
        self.assertLessEqual(len(self.m.net.fragments), a.Network.REASSEMBLY_DATAGRAMS)
        parts = a.ipv4_fragments(a.GUEST_IP, a.HOST_IP, 1, icmp, 0x4000)
        self.tx(eth + parts[0])
        self.tx(eth + parts[0])                       # the same piece again: an overlap
        for packet in parts[1:]:
            self.tx(eth + packet)
        self.assertFalse(self.m.rx)

    def test_stop_halts_receive_and_powers_the_phy_down(self):
        self.up()
        self.assertEqual(self.chip.Stop(self.state, c.byref(self.io)), 0)
        self.assertFalse(self.m.chip.receiving())
        self.settle(0.1)
        self.assertFalse(self.m.chip.link)
        self.m.deliver(a.HOST_MAC + a.MODEL_MAC + b'\x08\x00' + bytes(50))
        self.assertFalse(self.m.rx)

    def test_unknown_vendor_requests_stall(self):
        self.setup()
        self.assertIsNone(self.m.chip.read(0x05, 0, 0, 2, 'super'))
        self.assertIsNone(self.m.chip.read(a.ACCESS_MAC, 0x10, 6, 4, 'super'))     # width and length differ
        self.assertIsNone(self.m.chip.read(a.ACCESS_PHY, 0x01, 0x11, 2, 'super'))  # not PHY address 3
        self.assertFalse(self.m.chip.write(a.ACCESS_MAC, a.R_LINK_STATUS, 1, b'\x00'))

    def test_bulk_in_record_layout(self):
        """The layout axframe.c parses, built by hand from the model: records,
        headers, trailer."""
        self.up()
        self.tx(echo_request(a.MODEL_MAC, 10)[0])
        self.m.handle(101, 9, struct.pack('<BBHIH', a.EP_IN, 0, 4096, 0, 0))
        data = self.wire.take(101)[-1][2][10:]
        count, offset = struct.unpack_from('<HH', data, len(data) - 4)
        self.assertEqual((count, offset % 8, len(data)), (1, 0, offset + 4 * count + 4))
        header = struct.unpack_from('<I', data, offset)[0]
        length = header >> 16 & 0x1fff
        self.assertEqual(header & 0xffff, 0)
        self.assertEqual(data[:2], b'\0\0')
        self.assertEqual(data[2:8], a.MODEL_MAC)
        self.assertEqual((length + 7) & ~7, offset)
        self.assertNotEqual(zlib.crc32(data[2:length]), struct.unpack_from('<I', data, length - 2)[0])

    # What ASIX's XP driver 1.4.2.0 needed (docs/testing/2026-11-06-reference-drivers.md).
    def bulk_in(self):
        self.m.handle(101, 9, struct.pack('<BBHIH', a.EP_IN, 0, 16384 & 0xffff, 0, 0))
        return self.wire.take(101)[-1][2][10:]

    def test_reference_bring_up_reads(self):
        self.setup()
        chip = self.m.chip
        self.assertNotEqual(chip.read(a.ACCESS_PHY, a.PHY_ID, a.PHY_ID1, 2, 'high'), b'\0\0')
        self.assertEqual(chip.read(a.ACCESS_EFUSE, 0, 0x20, 64, 'high'), b'\xff' * 64)
        self.assertEqual(chip.read(a.ACCESS_EEPROM, 0x3c, 1, 2, 'high'), b'\xff\xff')
        self.assertEqual(chip.read(a.ACCESS_EEPROM, 0x43, 1, 2, 'high'), b'\0\0')
        self.assertTrue(chip.write(0x91, 0xff, 0, b''))
        self.assertTrue(chip.write(0x06, 0, 0, b''))

    def test_reference_transmit_form(self):
        """Bit 31 in the first header dword, each frame padded to 4 bytes,
        several records in one bulk-OUT transfer."""
        self.up()
        frames = [echo_request(a.MODEL_MAC, n)[0] for n in (18, 601, 1001)]
        data = b''.join(struct.pack('<II', len(f) | 0x80000000, 0) + f + bytes(-len(f) % 4) for f in frames)
        self.m.transmit(data)
        self.assertEqual(self.m.totals['tx_records_asix_form'], 3)
        self.assertEqual(len(self.events('icmp_echo')), 3)
        with self.assertRaises(ValueError):
            self.m.transmit(struct.pack('<II', 100 | 0x80000000, 0) + bytes(60))
        with self.assertRaises(ValueError):          # Linux's form stays exact
            self.m.transmit(struct.pack('<II', 60, 0) + bytes(64))

    def test_receive_lead_bytes_follow_ipe(self):
        self.up()
        frame = a.MODEL_MAC + a.HOST_MAC + b'\x08\x00' + bytes(46)
        rx_ctl = self.m.chip.word(a.R_RX_CTL)
        for ipe in (True, False):
            with self.subTest(ipe=ipe):
                v = rx_ctl | a.RX_IPE if ipe else rx_ctl & ~a.RX_IPE
                self.m.chip.mac_regs[a.R_RX_CTL:a.R_RX_CTL + 2] = struct.pack('<H', v)
                self.m.deliver(frame)
                data = self.bulk_in()
                self.assertEqual(data[:8], (b'\0\0' + a.MODEL_MAC) if ipe else frame[:8])

    def test_header_array_padded_to_8(self):
        """An even number of records gets 4 bytes between the header array and
        the trailer, so every transfer is a multiple of 8 bytes; the trailer
        still gives the offset Linux reads."""
        self.up()
        for n in (1, 2, 3, 4):
            with self.subTest(records=n):
                for _ in range(n):
                    self.m.deliver(a.MODEL_MAC + a.HOST_MAC + b'\x08\x00' + bytes(46))
                data = self.bulk_in()
                count, offset = struct.unpack_from('<HH', data, len(data) - 4)
                self.assertEqual(count, n)
                self.assertEqual(len(data) % 8, 0)
                self.assertEqual(len(data), offset + 4 * n + 4 + (4 if n % 2 == 0 else 0))

    def test_exact_fit_request_is_answered(self):
        """A request exactly the size of the next aggregate gets it, for odd
        and even record counts (the pad is reserved only when it is sent)."""
        self.up()
        frame = a.MODEL_MAC + a.HOST_MAC + b'\x08\x00' + bytes(46)
        for n, size in ((1, 64 + 4 + 4), (2, 2 * 64 + 8 + 4 + 4)):
            with self.subTest(records=n):
                for _ in range(n):
                    self.m.deliver(frame)
                self.m.handle(101, 9, struct.pack('<BBHIH', a.EP_IN, 0, size, 0, 0))
                data = self.wire.take(101)[-1][2][10:]
                self.assertEqual(len(data), size)
                self.assertFalse(self.m.rx)


if __name__ == '__main__':
    unittest.main()
