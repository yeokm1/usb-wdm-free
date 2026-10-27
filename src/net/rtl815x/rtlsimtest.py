#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests of rtlemu.py: the SIMULATED RTL815x driven by rtl815x's own
chip core (rtlchip.c and netchip.c, built into
out/net/rtl815x/hostabi/rtlcore.dll by host.ps1) through the model's
usbredir control, bulk and interrupt paths, and the newer chips' model
(RTL8153D, 8156, 8156B, 8157, 8159) driven by register sequences written
from the facts in docs/testing/2026-10-27-rtl815x-sim-newchips.md. No
physical USB device.

The driver-core tests of the newer chips are expected failures while the
core does not know them (PENDING_CORE): remove a chip from that set once
the core's tables for it land, and its tests must then pass.
Run: python -B rtlsimtest.py
"""
import ctypes as c
import io
import json
import socket
import struct
import sys
import threading
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rtlemu as r  # noqa: E402

DLL_PATH = Path(__file__).resolve().parents[3] / 'out/net/rtl815x/hostabi/rtlcore.dll'

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
    ('TxEnd', c.CFUNCTYPE(c.c_int, c.c_void_p, U8P, P(c.c_ulong), c.c_ulong, c.c_ulong))]})

FILTER_DIRECTED, FILTER_MULTICAST, FILTER_ALL_MULTICAST, FILTER_BROADCAST, FILTER_PROMISCUOUS = 1, 2, 4, 8, 0x20


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
    """The driver's chip core (NetPackageChip) over the model."""

    @classmethod
    def setUpClass(cls):
        cls.chip = NetChip.in_dll(c.CDLL(str(DLL_PATH)), 'NetPackageChip')

    # -- the driver core over the model's usbredir control path ---------------
    def setup(self, chip='8153', speed='high', **kw):
        self.wire, self.trace, self.clock = Wire(), io.StringIO(), Clock()
        self.m = r.Model(self.wire, self.trace, chip, speed, now=self.clock, **kw)
        self.m.peer_caps = r.CAPS & ~(1 << r.CAP_ID64)
        self.m.connect()
        self.m.handle(6, 1, b'\x01')
        self.wire.messages.clear()
        self.state = (c.c_ubyte * 256)()
        self.ident = 100

        def control(_, inp, request, value, index, data, length, actual):
            self.ident += 1
            rt = 0xc0 if inp else 0x40
            payload = b'' if inp else c.string_at(data, length)
            self.m.control(self.ident, struct.pack('<BBBBHHH', 0x80 if inp else 0, request, rt, 0,
                                                   value, index, length) + payload)
            kind, ident, reply = self.wire.messages.pop()
            status = reply[3]
            if (kind, ident) != (100, self.ident) or status:
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

        self.io = NetIo(None, CONTROL(control), SLEEP(sleep), 0, 0)
        self.probe = self.probe_model()
        self.params = NetParams()
        return self.chip.Start(self.state, c.byref(self.io), c.byref(self.probe), c.byref(self.params))

    def probe_model(self):
        dev = buf(self.m.device)
        cfg = buf(self.m.config_desc)
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
        steps = int(seconds / 0.05) + 1
        for _ in range(steps):
            self.clock.t += 0.05
            self.m.pump()

    def tx(self, *frames):
        """The driver's TxHeader framing, one bulk-OUT transfer."""
        out = bytearray()
        size, align = self.params.TxHeader or 8, self.params.TxAlign or 4
        for frame in frames:
            if len(frame) < 60:
                frame += bytes(60 - len(frame))
            h = (c.c_ubyte * size)()
            self.chip.TxHeader(self.state, h, len(frame), self.probe.OutMax, len(frames) == 1)
            out += bytes(h) + frame
            while len(out) % align:
                out += b'\0'
        self.m.handle(101, 7, struct.pack('<BBHIH', r.EP_OUT, 0, len(out), 0, 0) + bytes(out))
        self.assertEqual(self.wire.take(101)[-1][2][:2], bytes((r.EP_OUT, 0)))

    def rx(self, length=16384):
        """One bulk-IN request, parsed by the driver's RxParse."""
        self.m.handle(101, 9, struct.pack('<BBHIH', r.EP_IN, 0, length & 0xffff, 0, length >> 16))
        replies = self.wire.take(101)
        if not replies:
            return None
        data = replies[-1][2][10:]
        frames, stats = [], RxStats()
        emit = EMIT(lambda _, p, n: frames.append(c.string_at(p, n)))
        rc = self.chip.RxParse(self.state, buf(data), len(data), emit, None, c.byref(stats))
        self.assertEqual(rc, len(frames))
        return frames


@unittest.skipUnless(DLL_PATH.exists(), 'run host.ps1 first')
class Tests(CoreCase):
    def test_probe_and_bring_up_every_chip(self):
        for chip, speed, version, mbps in (('8153', 'high', 0x5c10, 1000), ('8152', 'high', 0x4c10, 100),
                                           ('8153b', 'high', 0x6010, 1000), ('8153', 'full', 0x5c10, 1000)):
            with self.subTest(chip=chip, speed=speed):
                self.assertEqual(self.setup(chip, speed), 0)
                p = self.probe
                self.assertEqual((p.In, p.Out, p.Interrupt, p.Product, p.Speed),
                                 (0x81, 0x02, 0x83, self.m.chip.pid, 2 if speed == 'high' else 1))
                self.assertEqual(bytes(self.params.Mac), r.BACKUP_MAC)
                self.assertEqual(self.params.Version, version)
                self.assertEqual((self.params.TxHeader, self.params.TxAlign), (8, 4))
                self.assertEqual(self.io.Status, 0)
                self.assertGreater(self.m.totals['vendor_requests'], 50)
                self.assertGreaterEqual(self.m.chip.autoneg, 1)
                # The link is down until autonegotiation completes.
                self.assertFalse(self.link().Up)
                self.assertEqual(self.chip.Poll(self.state, c.byref(self.io)), 0)
                self.settle(1.1)
                self.assertEqual(self.chip.Poll(self.state, c.byref(self.io)), 1)
                link = self.link()
                self.assertEqual((link.Up, link.Mbps, link.FullDuplex), (1, mbps, 1))
                self.assertTrue(self.m.chip.receiving())
                events = [json.loads(x) for x in self.trace.getvalue().splitlines()]
                self.assertFalse([e for e in events if e['event'] == 'vendor_refused'])

    def test_interrupt_reports_link_changes(self):
        self.assertEqual(self.setup(flap_after=3.0, flap_for=1.0), 0)
        self.m.handle(15, 3, bytes((r.EP_INT,)))
        self.assertEqual(self.wire.take(17)[-1][2], bytes((0, r.EP_INT)))
        self.settle(1.1)
        irqs = self.wire.take(103)
        self.assertTrue(irqs)
        data = irqs[-1][2][4:]
        self.assertEqual(self.chip.Status(self.state, buf(data), len(data)), 1)
        self.assertTrue(self.link().Up)
        self.settle(3.1)                              # cable out
        # The report's link bit is the link state: clear when down.
        data = self.wire.take(103)[-1][2][4:]
        self.assertEqual(data, b'\0\0')
        self.assertEqual(self.chip.Status(self.state, buf(data), len(data)), 0)
        self.assertFalse(self.link().Up)
        self.assertFalse(self.m.chip.receiving() and self.m.chip.pla[r.MR_LINK])
        self.settle(1.1)                              # cable in
        data = self.wire.take(103)[-1][2][4:]
        self.assertEqual(self.chip.Status(self.state, buf(data), len(data)), 1)
        self.assertTrue(self.link().Up)
        events = [json.loads(x)['why'] for x in self.trace.getvalue().splitlines() if '"link"' in x]
        self.assertEqual(events[-3:], ['autoneg_done', 'cable_out', 'cable_in'])

    def test_arp_icmp_dhcp_through_the_driver_framing(self):
        self.assertEqual(self.setup(), 0)
        self.settle(1.1)
        self.link()
        self.assertEqual(self.chip.SetFilter(self.state, c.byref(self.io),
                                             FILTER_DIRECTED | FILTER_BROADCAST, None, 0), 0)
        mac = r.BACKUP_MAC
        guest = r.GUEST_IP
        arp = b'\xff' * 6 + mac + b'\x08\x06' + struct.pack('!HHBBH', 1, 0x0800, 6, 4, 1) + \
            mac + guest + bytes(6) + r.HOST_IP
        self.tx(arp)
        frames = self.rx()
        self.assertEqual(len(frames), 1)
        reply = frames[0]
        self.assertEqual((reply[:6], reply[6:12], reply[12:14]), (mac, r.HOST_MAC, b'\x08\x06'))
        self.assertEqual(reply[14 + 6:14 + 8], b'\0\2')
        self.assertEqual(len(reply), 60)
        # ICMP echo, then DHCP DISCOVER.
        icmp = b'\x08\0\0\0' + b'\x12\x34\0\1' + b'usb-wdm-free' * 4
        icmp = icmp[:2] + struct.pack('!H', r.checksum(icmp)) + icmp[4:]
        ping = r.HOST_MAC + mac + b'\x08\x00' + r.ipv4(guest, r.HOST_IP, 1, icmp)
        boot = bytearray(240)
        boot[0:4], boot[4:8], boot[28:34] = b'\x01\x01\x06\x00', b'\xaa\xbb\xcc\xdd', mac
        boot[236:240] = b'\x63\x82\x53\x63'
        boot += bytes((53, 1, 1, 255))
        udp = struct.pack('!HHHH', 68, 67, 8 + len(boot), 0) + bytes(boot)
        discover = b'\xff' * 6 + mac + b'\x08\x00' + r.ipv4(b'\0' * 4, b'\xff' * 4, 17, udp)
        self.tx(ping, discover)                     # two frames, one transfer
        frames = self.rx()                          # both answers, one aggregate
        self.assertEqual(len(frames), 2)
        echo, offer = frames
        self.assertEqual(echo[14 + 20], 0)          # echo reply
        self.assertEqual(r.checksum(echo[14 + 20:14 + 20 + len(icmp)]), 0)
        self.assertEqual(echo[14 + 28:14 + 20 + len(icmp)], icmp[8:])
        self.assertEqual(offer[14 + 28 + 16:14 + 28 + 20], guest)     # yiaddr
        self.assertIn(bytes((53, 1, 2)), offer)
        self.assertEqual(self.m.totals['rx_transfers'], 2)

    def test_receive_filter_follows_the_driver(self):
        self.assertEqual(self.setup(), 0)
        self.settle(1.1)
        self.link()
        group = bytes.fromhex('01005e000001')
        other = bytes.fromhex('01005e7ffffa')
        self.assertEqual(self.chip.SetFilter(self.state, c.byref(self.io),
                                             FILTER_DIRECTED | FILTER_BROADCAST | FILTER_MULTICAST,
                                             buf(group), 1), 0)
        acc = self.m.chip.accepts
        self.assertTrue(acc(b'\xff' * 6))
        self.assertTrue(acc(r.BACKUP_MAC))
        self.assertTrue(acc(group))
        self.assertFalse(acc(other))
        self.assertFalse(acc(bytes.fromhex('020000000009')))
        self.assertEqual(self.chip.SetFilter(self.state, c.byref(self.io), FILTER_PROMISCUOUS, None, 0), 0)
        self.assertTrue(acc(other) and acc(bytes.fromhex('020000000009')))
        self.assertEqual(self.chip.SetFilter(self.state, c.byref(self.io), FILTER_DIRECTED, None, 0), 0)
        # Directed only: multicast and foreign unicast refused; broadcast
        # stays accepted (the core always sets ACC_BCAST).
        self.assertFalse(acc(group) or acc(bytes.fromhex('020000000009')))
        self.assertTrue(acc(b'\xff' * 6))
        # SetMac moves the directed address.
        new = bytes.fromhex('02aabbccddee')
        self.assertEqual(self.chip.SetMac(self.state, c.byref(self.io), buf(new)), 0)
        self.assertTrue(acc(new) and not acc(r.BACKUP_MAC))

    def test_stop_halts_receive(self):
        self.assertEqual(self.setup(), 0)
        self.settle(1.1)
        self.link()
        self.assertEqual(self.chip.Stop(self.state, c.byref(self.io)), 0)
        self.assertFalse(self.m.chip.receiving())
        self.m.deliver(b'\xff' * 6 + r.HOST_MAC + b'\x08\x06' + bytes(46))
        self.assertEqual(self.m.totals['rx_filtered'], 1)
        # Resume is the whole bring-up again.
        self.assertEqual(self.chip.Resume(self.state, c.byref(self.io), c.byref(self.probe),
                                          c.byref(self.params)), 0)

    def test_transmit_framing_refused_when_malformed(self):
        self.assertEqual(self.setup(), 0)
        bad = struct.pack('<II', 0xc0000000 | 2000, 0) + bytes(100)
        with self.assertRaises(ValueError):
            self.m.transmit(bad)
        with self.assertRaises(ValueError):
            self.m.transmit(struct.pack('<II', 60, 0) + bytes(60))      # no first/last segment bits

    def test_aggregate_respects_request_length(self):
        self.assertEqual(self.setup(), 0)
        self.settle(1.1)
        self.link()
        self.chip.SetFilter(self.state, c.byref(self.io), FILTER_BROADCAST, None, 0)
        for n in range(5):
            self.m.deliver(b'\xff' * 6 + r.HOST_MAC + b'\x08\x00' + bytes([n]) * 1000)
        first = self.rx(2048)          # one 1014-byte frame record is 1048 bytes, aligned 1048
        self.assertEqual(len(first), 1)
        rest = self.rx(16384)
        self.assertEqual(len(rest), 4)
        self.assertEqual([f[14] for f in first + rest], [0, 1, 2, 3, 4])

    def test_wire_64bit_ids_and_32bit_lengths(self):
        server, client = socket.socketpair()
        model = r.Model(server, io.StringIO(), '8152')
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
        client.settimeout(3)
        worker.start()
        try:
            kind, length, _ = struct.unpack('<III', exact(12))
            exact(length)
            hello = b'test'.ljust(64, b'\0') + struct.pack('<I', r.CAPS)
            client.sendall(struct.pack('<III', 0, len(hello), 0) + hello)
            kinds = []
            for _ in range(3):
                kind, length, _ = struct.unpack('<IIQ', exact(16))
                body = exact(length)
                kinds.append(kind)
            self.assertEqual(kinds, [5, 4, 1])
            self.assertEqual(body[4:8], struct.pack('<HH', 0x0bda, 0x8152))
            request = struct.pack('<BBBBHHH', 0x80, 6, 0x80, 0, 0x100, 0, 18)
            client.sendall(struct.pack('<IIQ', 100, len(request), 1 << 33) + request)
            kind, length, ident = struct.unpack('<IIQ', exact(16))
            self.assertEqual((kind, ident, exact(length)[10:]), (100, 1 << 33, model.device))
        finally:
            client.close()
            worker.join(3)
            server.close()
        self.assertEqual(errors, [])


# ---- the newer chips ---------------------------------------------------------

# Chips the driver's core does not know yet (uwf-rtl-new, 2026-10-27): their
# driver-core tests are expected to fail until the core's tables land.
PENDING_CORE = {'8157', '8159'}

MAC, USB = r.RTL_SPACE_MAC, 0


class Regs:
    """Vendor register access through the model's control path, in the
    request forms of rtlchip.h: whole-dword reads, one-dword writes with
    the register's byte lanes, the PHY through MR_PHY_PAGE and the window."""

    def __init__(self, model):
        self.m, self.n, self.page = model, 0, None

    def request(self, out, value, index, length, payload=b''):
        self.n += 1
        self.m.control(self.n, struct.pack('<BBBBHHH', 0 if out else 0x80, 5, 0x40 if out else 0xc0, 0,
                                           value, index, length) + payload)
        kind, ident, reply = self.m.connection.messages.pop()
        assert (kind, ident, reply[3]) == (100, self.n, 0), 'vendor request refused'
        return reply[10:]

    def get(self, space, reg, size=2):
        data = self.request(False, reg & ~3, space, 4)
        word = struct.unpack('<I', data)[0]
        return word if size == 4 else word >> 8 * (reg & 3) & (0xffff if size == 2 else 0xff)

    def put(self, space, reg, size, value):
        lanes = 0xff if size == 4 else (0x33 << (reg & 2)) if size == 2 else (0x11 << (reg & 3))
        shift = 0 if size == 4 else 8 * (reg & 3)
        self.request(True, reg & ~3, space | lanes, 4, struct.pack('<I', (value << shift) & 0xffffffff))

    def edit(self, space, reg, size, clear, set_):
        self.put(space, reg, size, (self.get(space, reg, size) & ~clear) | set_)

    def window(self, reg):
        if self.page != reg & 0xf000:
            self.page = reg & 0xf000
            self.put(MAC, r.MR_PHY_PAGE, 2, self.page)
        return 0xb000 | (reg & 0x0fff)

    def phy(self, reg, value=None):
        at = self.window(reg)
        if value is None:
            return self.get(MAC, at, 2)
        self.put(MAC, at, 2, value)

    def adv(self, ip, addr, value=None):
        self.put(USB, r.UR_ADV_ADDR, 2, addr)
        if value is not None:
            self.put(USB, r.UR_ADV_DATA, 4, value)
        self.put(USB, r.UR_ADV_CMD, 2, r.ADV_BUSY | (r.ADV_IP if ip else 0) |
                 (r.ADV_WRITE if value is not None else 0))
        assert not self.get(USB, r.UR_ADV_CMD) & r.ADV_BUSY
        return None if value is not None else self.get(USB, r.UR_ADV_DATA, 4)


def parse_rx(data, wide):
    """Frames of one bulk-IN transfer, by the descriptor facts: 24-byte
    header, length in bits 0-14, records 8-byte aligned; or (8157/8159)
    16-byte header, length in bits 17-31, records 16-byte aligned. The
    length counts the FCS, which is checked and removed."""
    desc, align = (16, 16) if wide else (24, 8)
    frames, off = [], 0
    while off + desc <= len(data):
        opts1 = struct.unpack_from('<I', data, off)[0]
        n = opts1 >> 17 if wide else opts1 & 0x7fff
        if n < 60 or off + desc + n > len(data):
            break
        frame = data[off + desc:off + desc + n - 4]
        assert struct.unpack_from('<I', data, off + desc + n - 4)[0] == zlib.crc32(frame), 'FCS'
        frames.append(frame)
        off = (off + desc + n + align - 1) & ~(align - 1)
    return frames


def tx_record(frame, wide):
    """One transmit record by the descriptor facts (see transmit())."""
    if wide:
        return struct.pack('<IIII', r.TX_FIRST_LAST | len(frame), 0, len(frame) << 4, r.TX_SIGNATURE) + frame
    return struct.pack('<II', r.TX_FIRST_LAST | len(frame), 0) + frame


def parse_config(data):
    """(interface class, [(address, attributes, max packet, companion)])."""
    out, iclass, off = [], None, 0
    while off < len(data):
        n, kind = data[off], data[off + 1]
        if kind == 4:
            iclass = data[off + 5]
        elif kind == 5:
            out.append([data[off + 2], data[off + 3], struct.unpack_from('<H', data, off + 4)[0], None])
        elif kind == 0x30:
            out[-1][3] = bytes(data[off:off + n])
        off += n
    return iclass, out


class NewerModelTests(unittest.TestCase):
    """The newer chips' model, by register sequences from the facts; no
    driver core needed."""

    def make(self, chip, speed='super', **kw):
        self.wire, self.trace, self.clock = Wire(), io.StringIO(), Clock()
        self.m = r.Model(self.wire, self.trace, chip, speed, now=self.clock, **kw)
        self.m.peer_caps = r.CAPS & ~(1 << r.CAP_ID64)
        self.m.connect()
        self.m.handle(6, 1, b'\x01')
        self.wire.messages.clear()
        self.regs = Regs(self.m)
        return self.regs

    def settle(self, seconds):
        for _ in range(int(seconds / 0.05) + 1):
            self.clock.t += 0.05
            self.m.pump()

    def descriptor(self, kind):
        self.m.control(1, struct.pack('<BBBBHHH', 0x80, 6, 0x80, 0, kind << 8, 0, 4096))
        _, _, reply = self.wire.messages.pop()
        return reply[3], reply[10:]

    def test_identity_and_descriptors(self):
        ids = {'8153d': (0x7420, 0x8053, 0x0300), '8156': (0x7030, 0x8156, 0x0300),
               '8156-7020': (0x7020, 0x8156, 0x0300), '8156b': (0x7410, 0x8156, 0x0300),
               '8156b-7400': (0x7400, 0x8156, 0x0300), '8157': (0x1030, 0x8157, 0x0320),
               '8159': (0x2020, 0x815a, 0x0320)}
        self.assertEqual(set(ids), set(r.NEWER))
        for chip, (cid, pid, bcd) in sorted(ids.items()):
            with self.subTest(chip=chip):
                self.wire, self.trace = Wire(), io.StringIO()
                m = self.m = r.Model(self.wire, self.trace, chip, 'super')
                m.peer_caps = r.CAPS & ~(1 << r.CAP_ID64)
                m.connect()
                connect = [x for x in self.wire.messages if x[0] == 1][0][2]
                self.assertEqual(struct.unpack('<BBBBHH', connect[:8]), (3, 0, 0, 0, 0x0bda, pid))
                status, dev = self.descriptor(1)
                self.assertEqual(status, 0)
                self.assertEqual(struct.unpack_from('<HBBBBHH', dev, 2)[0:7],
                                 (bcd, 0, 0, 0, 9, 0x0bda, pid))
                status, cfg = self.descriptor(2)
                iclass, eps = parse_config(cfg)
                self.assertEqual(iclass, 0xff)
                self.assertEqual([(a, t, s) for a, t, s, _ in eps], [(0x81, 2, 1024), (0x02, 2, 1024), (0x83, 3, 2)])
                self.assertTrue(all(comp for *_, comp in eps))
                status, bos = self.descriptor(15)
                self.assertEqual(status, 0)
                self.assertEqual(struct.unpack_from('<H', bos, 2)[0], len(bos))
                caps = [bos[i + 2] for i in self.walk(bos)]
                self.assertEqual(caps, [2, 3] + ([0x0a] if bcd == 0x0320 else []))
                self.assertEqual(self.descriptor(6)[0], 4)            # no qualifier at SuperSpeed
                # The id reads before any configuration is chosen (Linux's
                # configuration choice reads it first).
                tcr = Regs(m).get(MAC, r.MR_TX_CONFIG, 4)
                self.assertEqual(tcr >> 16 & 0x7cf0, cid)
                self.assertTrue(tcr & r.TXC_EMPTY)
                # High Speed: a USB 2.1 device with 512-byte bulk packets and a
                # BOS of the USB 2.0 extension only.
                self.wire = Wire()
                m = self.m = r.Model(self.wire, io.StringIO(), chip, 'high')
                self.assertEqual(struct.unpack_from('<H', m.device, 2)[0], 0x0210)
                self.assertEqual([s for _, _, s, _ in parse_config(m.config_desc)[1]], [512, 512, 2])
                status, bos = self.descriptor(15)
                self.assertEqual((status, [bos[i + 2] for i in self.walk(bos)]), (0, [2]))

    @staticmethod
    def walk(bos):
        i = 5
        while i < len(bos):
            yield i
            i += bos[i]

    def test_usb2_chip_refuses_superspeed(self):
        with self.assertRaises(ValueError):
            r.Model(Wire(), io.StringIO(), '8152', 'super')

    def test_phy_sram_windows_step_by_two(self):
        g = self.make('8157')
        g.phy(r.PR_SRAM_ADDR, 0x8f9c)
        for value in (0x0005, 0x0000, 0x00ed, 0x0502, 0x0b00, 0xd401):
            g.phy(r.PR_SRAM_DATA, value)
        self.assertEqual([self.m.chip.sram[0x8f9c + 2 * i] for i in range(6)],
                         [0x0005, 0x0000, 0x00ed, 0x0502, 0x0b00, 0xd401])
        # A read leaves the address; read-modify-write at one address.
        g.phy(r.PR_SRAM2_ADDR, 0x8647)
        g.phy(r.PR_SRAM2_DATA, 0x12ab)
        g.phy(r.PR_SRAM2_ADDR, 0x8647)
        value = g.phy(r.PR_SRAM2_DATA)
        g.phy(r.PR_SRAM2_DATA, (value & 0x00ff) | 0xe600)
        self.assertEqual(self.m.chip.sram2[0x8647], 0xe6ab)
        g.phy(r.PR_SRAM_ADDR, 0x8011)
        self.assertEqual(g.phy(r.PR_SRAM_DATA), 0)

    def test_polled_bits_complete(self):
        for chip in r.NEWER:
            with self.subTest(chip=chip):
                g = self.make(chip)
                self.assertEqual(g.phy(r.PR_STATE) & 7, r.PHYST_LAN_ON)
                self.assertTrue(g.get(MAC, r.MR_BOOT) & r.BOOT_LOADED)
                self.assertTrue(g.get(MAC, r.MR_GPHY_FLASH) & r.GPHY_HAS_FLASH)
                self.assertTrue(g.get(USB, r.UR_GPHY) & r.GPHY_LOADED)
                self.assertFalse(g.get(USB, r.UR_MISC0) & r.PCUT_SEEN)
                self.assertTrue(g.get(MAC, 0xe000) & 0x0100)
                self.assertEqual(g.get(MAC, r.MR_OOB, 1) & 0x32, 0x32)
                g.put(MAC, r.MR_COMMAND, 1, r.CMD_RESET)
                self.assertFalse(g.get(MAC, r.MR_COMMAND, 1) & r.CMD_RESET)
                g.phy(r.PR_PATCH_CMD, r.PATCH_ASK)
                self.assertTrue(g.phy(r.PR_PATCH_STATE) & r.PATCH_GRANTED)
                g.phy(r.PR_PATCH_CMD, 0)
                self.assertFalse(g.phy(r.PR_PATCH_STATE) & r.PATCH_GRANTED)
                g.phy(r.PR_MII, r.MIICTL_RESET | r.MIICTL_AN_ENABLE | r.MIICTL_AN_RESTART)
                self.assertFalse(g.phy(r.PR_MII) & r.MIICTL_RESET)
                # Power-down shows in the PHY state until cleared.
                g.phy(r.PR_MII, r.MIICTL_AN_ENABLE | r.MIICTL_POWERDOWN)
                self.assertEqual(g.phy(r.PR_STATE) & 7, r.PHYST_POWER_DOWN)
                g.phy(r.PR_MII, r.MIICTL_AN_ENABLE)
                self.assertEqual(g.phy(r.PR_STATE) & 7, r.PHYST_LAN_ON)

    def test_powered_down_phy_has_no_link(self):
        g = self.make('8156b')
        self.advertise(g, 2500)
        self.settle(1.1)
        self.assertEqual(self.m.link_mbps, 2500)
        g.phy(r.PR_MII, r.MIICTL_AN_ENABLE | r.MIICTL_AN_RESTART | r.MIICTL_POWERDOWN)
        self.settle(1.1)
        self.assertEqual((self.m.link, self.m.totals['phy_not_on']), (False, 1))
        self.assertEqual(g.get(MAC, r.MR_LINK), 0)
        g.phy(r.PR_MII, r.MIICTL_AN_ENABLE)               # powered up: negotiates again
        self.settle(1.1)
        self.assertEqual(self.m.link_mbps, 2500)
        # Still in external init: no link either.
        g = self.make('8156b', ext_init=True)
        self.advertise(g, 2500)
        self.settle(1.1)
        self.assertFalse(self.m.link)

    def test_external_init_and_power_cut(self):
        g = self.make('8159', ext_init=True, power_cut=True)
        self.assertEqual(g.phy(r.PR_STATE) & 7, r.PHYST_EXT_INIT)
        self.assertTrue(g.get(USB, r.UR_MISC0) & r.PCUT_SEEN)
        self.assertTrue(g.get(USB, r.UR_GPHY) & r.GPHY_RESTORED)
        g.phy(r.PR_INIT_GATE, g.phy(r.PR_INIT_GATE) & ~0x0002)
        self.assertEqual(g.phy(r.PR_STATE) & 7, r.PHYST_EXT_INIT)    # both bits are needed
        g.phy(r.PR_INIT_GATE, g.phy(r.PR_INIT_GATE) & ~0x0008)
        self.assertEqual(g.phy(r.PR_STATE) & 7, r.PHYST_LAN_ON)

    def test_adv_window(self):
        for chip in ('8157', '8159'):
            with self.subTest(chip=chip):
                g = self.make(chip)
                self.assertEqual(g.adv(True, r.IR_U2P3), r.U2P3_V2_ON)
                self.assertEqual(g.adv(False, r.BR_RX_ZLP), r.BMU_RX_ZLP)
                g.adv(True, r.IR_U2P3, 0x12345678)
                self.assertEqual(g.adv(True, r.IR_U2P3), 0x12345678)
                self.assertEqual(g.adv(False, r.IR_U2P3), 0)                 # the BMU space is apart
                self.assertEqual(self.m.chip.adv['ip'][r.IR_U2P3], 0x12345678)

    def advertise(self, g, top, multigig=True, pause=True):
        g.phy(r.PR_MII + 8, 0x01e1 | (r.ANADV_PAUSE | 0x0800 if pause else 0))
        g.phy(r.PR_MII + 18, r.GIGCTL_1000_FULL)
        xg = 0
        if multigig:
            xg = (r.XGCTL_2500 if top >= 2500 else 0) | (r.XGCTL_5000 if top >= 5000 else 0) | \
                (r.XGCTL_10000 if top >= 10000 else 0)
        if top > 1000:
            g.phy(r.PR_XG_CTRL, xg)
        g.phy(r.PR_MII, r.MIICTL_AN_ENABLE | r.MIICTL_AN_RESTART)

    def test_link_at_each_speed(self):
        bits = {10: 0x04, 100: 0x08, 1000: 0x10, 2500: 0x0400, 5000: 0x1000, 10000: 0x4000}
        cases = [(chip, False) for chip in r.NEWER] + [('8159', True)]
        for chip, patched in cases:
            top = r.CHIPS[chip].top
            for partner in r.PARTNERS:
                with self.subTest(chip=chip, partner=partner, patched=patched):
                    g = self.make(chip, partner=partner, phy_patch=patched)
                    self.m.handle(15, 3, bytes((r.EP_INT,)))
                    self.wire.messages.clear()
                    self.advertise(g, top)
                    self.assertFalse(g.get(MAC, r.MR_LINK) & r.LNK_UP)
                    self.settle(1.1)
                    want = min(5000 if chip == '8159' and not patched else top, partner)
                    word = g.get(MAC, r.MR_LINK)
                    self.assertEqual(word, 0x02 | 0x01 | 0x60 | bits[want])
                    self.assertEqual(self.m.link_mbps, want)
                    self.assertTrue(g.phy(r.PR_MII + 2) & r.MIIST_LINK)
                    xgst = g.phy(r.PR_XG_STAT)
                    self.assertEqual(bool(xgst & r.XGST_PEER_10000), partner >= 10000)
                    self.assertEqual(bool(xgst & r.XGST_PEER_2500), partner >= 2500)
                    irqs = self.wire.take(103)
                    self.assertTrue(irqs)
                    report = irqs[-1][2][4:]
                    self.assertEqual(struct.unpack('<H', report)[0] & r.IRQ_LINK, r.IRQ_LINK)

    def test_speed_settings_refused_or_needed(self):
        g = self.make('8159')
        # Without the 2.5G-10G advertisement (as below SuperSpeed) the link is 1000.
        self.advertise(g, 10000, multigig=False)
        self.settle(1.1)
        self.assertEqual(self.m.link_mbps, 1000)
        # Forced 100 full: no autonegotiation, so no pause bits.
        g.phy(r.PR_MII, r.MIICTL_SPEED100 | r.MIICTL_FULL)
        self.settle(1.1)
        self.assertEqual(g.get(MAC, r.MR_LINK), 0x02 | 0x01 | 0x08)
        # Forced 1000 half: refused, the link stays down.
        g.phy(r.PR_MII, r.MIICTL_SPEED1000)
        self.settle(1.1)
        self.assertEqual(g.get(MAC, r.MR_LINK), 0)
        self.assertEqual(self.m.totals['no_common_speed'], 1)
        # 2.5G-10G cannot be forced: the control register has no such
        # speed, so they are only advertised; the 8153D ignores them.
        g = self.make('8153d')
        g.phy(r.PR_XG_CTRL, r.XGCTL_2500 | r.XGCTL_10000)
        self.advertise(g, 1000)
        self.settle(1.1)
        self.assertEqual(self.m.link_mbps, 1000)
        # Nothing advertised: no link.
        g = self.make('8156')
        g.phy(r.PR_MII + 8, 1)
        g.phy(r.PR_MII + 18, 0)
        g.phy(r.PR_XG_CTRL, 0)
        g.phy(r.PR_MII, r.MIICTL_AN_ENABLE | r.MIICTL_AN_RESTART)
        self.settle(1.1)
        self.assertFalse(self.m.link)
        # 2.5G advertised below SuperSpeed is counted: a diagnostic of
        # Linux's default advertisement, which has 2.5G and faster only at
        # SuperSpeed or faster (a user's explicit setting may still ask).
        g = self.make('8156', 'high')
        self.advertise(g, 2500)
        self.settle(1.1)
        self.assertEqual((self.m.link_mbps, self.m.totals['multigig_below_superspeed']), (2500, 1))

    def receiver_on(self, g, chip):
        if r.CHIPS[chip].family == 'c':
            g.edit(USB, r.UR_CTRL, 2, r.CTRL_RX_AGG_OFF | r.CTRL_RX_DESC_LONG |
                   (r.CTRL_8159_B11 if chip == '8159' else 0), 0)
            g.edit(USB, r.UR_TX_SIG_CHECK, 1, r.TXSIG_CHECK, 0)
        else:
            g.edit(USB, r.UR_CTRL, 2, r.CTRL_RX_AGG_OFF | r.CTRL_RX_ZERO, 0)
        g.put(MAC, r.MR_RX_MAX_LEN, 2, 1522)
        g.put(MAC, r.MR_RX_ACCEPT, 4, 0x0f)
        g.put(MAC, r.MR_COMMAND, 1, r.CMD_RX_ON | r.CMD_TX_ON)

    def test_framing_per_chip(self):
        for chip in r.NEWER:
            wide = r.CHIPS[chip].family == 'c'
            with self.subTest(chip=chip):
                g = self.make(chip)
                self.receiver_on(g, chip)
                self.advertise(g, r.CHIPS[chip].top)
                self.settle(1.1)
                mac = r.BACKUP_MAC
                arp = b'\xff' * 6 + mac + b'\x08\x06' + struct.pack('!HHBBH', 1, 0x0800, 6, 4, 1) + \
                    mac + r.GUEST_IP + bytes(6) + r.HOST_IP
                icmp = b'\x08\0\0\0' + b'\x12\x34\0\1' + bytes(1000)
                icmp = icmp[:2] + struct.pack('!H', r.checksum(icmp)) + icmp[4:]
                ping = r.HOST_MAC + mac + b'\x08\x00' + r.ipv4(r.GUEST_IP, r.HOST_IP, 1, icmp)
                out = bytearray()
                for frame in (arp + bytes(60 - len(arp)), ping):
                    out += tx_record(frame, wide)
                    while len(out) % (16 if wide else 4):
                        out += b'\0'
                self.m.handle(101, 7, struct.pack('<BBHIH', r.EP_OUT, 0, len(out), 0, 0) + bytes(out))
                self.assertEqual(self.m.totals['tx_frames'], 2)
                self.assertEqual(self.m.totals['tx_sig_check_left_on'], 0)
                rxbuf = r.CHIPS[chip].rxbuf
                self.m.handle(101, 9, struct.pack('<BBHIH', r.EP_IN, 0, rxbuf & 0xffff, 0, rxbuf >> 16))
                data = self.wire.take(101)[-1][2][10:]
                frames = parse_rx(data, wide)
                self.assertEqual(len(frames), 2)
                self.assertEqual(frames[0][12:14], b'\x08\x06')
                self.assertEqual(frames[1][14 + 20], 0)                       # echo reply
                second = (r.CHIPS[chip].family == 'c' and 16 or 24) + 64
                second = (second + (15 if wide else 7)) & ~(15 if wide else 7)
                self.assertEqual(struct.unpack_from('<I', data, second)[0] >> (17 if wide else 0) & 0x7fff,
                                 len(frames[1]) + 4)
                # Malformed transmit headers are refused.
                if wide:
                    bad = bytearray(tx_record(bytes(60), True))
                    bad[12:16] = bytes(4)
                    with self.assertRaises(ValueError):
                        self.m.transmit(bytes(bad))
                    bad = bytearray(tx_record(bytes(60), True))
                    bad[0] = 61
                    with self.assertRaises(ValueError):
                        self.m.transmit(bytes(bad))
                    bad[0] = 0                               # the length in the third dword only
                    with self.assertRaises(ValueError):
                        self.m.transmit(bytes(bad))
                with self.assertRaises(ValueError):
                    self.m.transmit(tx_record(bytes(60), wide)[:-1])

    def test_wide_receive_needs_the_16_byte_mode(self):
        for chip in ('8157', '8159'):
            with self.subTest(chip=chip):
                g = self.make(chip)
                self.receiver_on(g, chip)
                g.edit(USB, r.UR_CTRL, 2, 0, r.CTRL_RX_DESC_LONG)     # as before the driver's clear
                self.advertise(g, 1000)
                self.settle(1.1)
                self.m.deliver(b'\xff' * 6 + r.HOST_MAC + b'\x08\x06' + bytes(46))
                self.assertEqual((self.m.totals['rx_desc_mode'], len(self.m.rx)), (1, 0))
                g.edit(USB, r.UR_TX_SIG_CHECK, 1, 0, r.TXSIG_CHECK)
                self.m.transmit(tx_record(bytes(60), True))
                self.assertEqual(self.m.totals['tx_sig_check_left_on'], 1)

    def test_aggregation_off_gives_one_frame_per_transfer(self):
        g = self.make('8156b')
        self.receiver_on(g, '8156b')
        g.edit(USB, r.UR_CTRL, 2, 0, r.CTRL_RX_AGG_OFF)
        self.advertise(g, 2500)
        self.settle(1.1)
        for n in range(3):
            self.m.deliver(b'\xff' * 6 + r.HOST_MAC + b'\x08\x00' + bytes([n]) * 100)
        self.m.handle(101, 9, struct.pack('<BBHIH', r.EP_IN, 0, 49152 & 0xffff, 0, 0))
        self.assertEqual(len(parse_rx(self.wire.take(101)[-1][2][10:], False)), 1)

    def test_early_size_units(self):
        for chip in r.NEWER:
            kind = r.CHIPS[chip]
            desc, align = (16, 16) if kind.family == 'c' else (24, 8)
            with self.subTest(chip=chip):
                g = self.make(chip)
                self.receiver_on(g, chip)
                early = kind.rxbuf - (1522 + desc + align)
                g.put(USB, r.UR_RX_EARLY_LEN, 2, early // kind.unit)
                self.m.handle(101, 9, struct.pack('<BBHIH', r.EP_IN, 0, kind.rxbuf & 0xffff, 0, kind.rxbuf >> 16))
                self.assertEqual(self.m.totals['rx_request_below_early_size'], 0)
                # The 8156's unit (8 bytes) on an 8157 doubles the early size.
                g.put(USB, r.UR_RX_EARLY_LEN, 2, early // 8)
                self.m.handle(101, 10, struct.pack('<BBHIH', r.EP_IN, 0, kind.rxbuf & 0xffff, 0, kind.rxbuf >> 16))
                self.assertEqual(self.m.totals['rx_request_below_early_size'], 1 if kind.unit == 16 else 0)

    def test_early_size_ends_a_transfer(self):
        for chip in ('8156', '8157'):
            wide = r.CHIPS[chip].family == 'c'
            with self.subTest(chip=chip):
                g = self.make(chip)
                self.receiver_on(g, chip)
                self.advertise(g, 1000)
                self.settle(1.1)
                g.put(USB, r.UR_RX_EARLY_LEN, 2, 200 // r.CHIPS[chip].unit)    # 200 bytes
                for n in range(6):
                    self.m.deliver(b'\xff' * 6 + r.HOST_MAC + b'\x08\x00' + bytes([n]) * 46)
                sizes = []
                for ident in range(2):
                    self.m.handle(101, 20 + ident, struct.pack('<BBHIH', r.EP_IN, 0, 16384, 0, 0))
                    sizes.append(len(parse_rx(self.wire.take(101)[-1][2][10:], wide)))
                # Records of 80 (16 + 64) or 88 (24 + 64) bytes: the third
                # passes 200 bytes and ends the transfer.
                self.assertEqual(sizes, [3, 3])
                self.assertFalse(self.m.rx)


def pending(chip):
    def wrap(f):
        return unittest.expectedFailure(f) if chip in PENDING_CORE else f
    return wrap


@unittest.skipUnless(DLL_PATH.exists(), 'run host.ps1 first')
class NewerDriverTests(CoreCase):
    """The driver's core on the newer chips (expected failures while the
    chip is in PENDING_CORE)."""

    def drive(self, chip):
        kind = r.CHIPS[chip]
        wide = kind.family == 'c'
        self.assertEqual(self.setup(chip, 'super', flap_after=3.0, flap_for=1.0), 0)
        p = self.probe
        self.assertEqual((p.In, p.Out, p.Interrupt, p.Product, p.Speed, p.InMax),
                         (0x81, 0x02, 0x83, kind.pid, 3, 1024))
        self.assertEqual(self.params.Version, kind.id)
        self.assertEqual(bytes(self.params.Mac), r.BACKUP_MAC)
        self.assertEqual((self.params.TxHeader, self.params.TxAlign), (16, 16) if wide else (8, 4))
        self.assertEqual(self.io.Status, 0)
        events = [json.loads(x) for x in self.trace.getvalue().splitlines()]
        self.assertFalse([e for e in events if e['event'] == 'vendor_refused'])
        chipm = self.m.chip
        self.assertEqual(chipm.phy_state(), r.PHYST_LAN_ON)
        if wide:
            self.assertFalse(chipm.usb_word(r.UR_CTRL) & (r.CTRL_RX_DESC_LONG | r.CTRL_8159_B11))
            self.assertFalse(chipm.usb[r.UR_TX_SIG_CHECK] & r.TXSIG_CHECK)
            self.assertFalse(chipm.adv['bmu'][r.BR_RX_ZLP] & r.BMU_RX_ZLP)
        # Link at the chip's top speed (the partner offers 10G).
        self.m.handle(15, 3, bytes((r.EP_INT,)))
        self.settle(1.1)
        irqs = self.wire.take(103)
        self.assertTrue(irqs)
        data = irqs[-1][2][4:]
        self.assertEqual(self.chip.Status(self.state, buf(data), len(data)), 1)
        link = self.link()
        top = self.m.chip.top                       # the 8159: 5000 with no PHY patch
        self.assertEqual((link.Up, link.Mbps, link.FullDuplex), (1, top, 1))
        # Framing through the driver's TxHeader and RxParse.
        self.assertEqual(self.chip.SetFilter(self.state, c.byref(self.io),
                                             FILTER_DIRECTED | FILTER_BROADCAST, None, 0), 0)
        mac = r.BACKUP_MAC
        arp = b'\xff' * 6 + mac + b'\x08\x06' + struct.pack('!HHBBH', 1, 0x0800, 6, 4, 1) + \
            mac + r.GUEST_IP + bytes(6) + r.HOST_IP
        icmp = b'\x08\0\0\0' + b'\x12\x34\0\1' + b'usb-wdm-free' * 100
        icmp = icmp[:2] + struct.pack('!H', r.checksum(icmp)) + icmp[4:]
        ping = r.HOST_MAC + mac + b'\x08\x00' + r.ipv4(r.GUEST_IP, r.HOST_IP, 1, icmp)
        self.tx(arp, ping)
        self.assertEqual(self.m.totals['tx_frames'], 2)
        self.assertEqual(self.m.totals['tx_sig_check_left_on'], 0)
        frames = self.rx(self.params.RxSize)
        self.assertEqual(len(frames), 2)
        self.assertEqual(frames[1][14 + 28:14 + 20 + len(icmp)], icmp[8:])
        self.assertEqual(self.m.totals['rx_request_below_early_size'], 0)
        # Cable out and in: the interrupt report and the link state follow.
        self.settle(3.1)
        data = self.wire.take(103)[-1][2][4:]
        self.assertEqual(self.chip.Status(self.state, buf(data), len(data)), 0)
        self.assertFalse(self.link().Up)
        self.settle(1.1)
        data = self.wire.take(103)[-1][2][4:]
        self.assertEqual(self.chip.Status(self.state, buf(data), len(data)), 1)
        self.assertEqual(self.link().Mbps, top)
        # The 8159 above 5 Gb/s, as if its PHY patch had been loaded.
        if chip == '8159':
            self.assertEqual(self.setup(chip, 'super', phy_patch=True), 0)
            self.settle(1.1)
            self.assertEqual(self.link().Mbps, 10000)
        # A 1000 Mb/s partner, and High Speed (no 2.5G-10G advertised there).
        self.assertEqual(self.setup(chip, 'super', partner=1000), 0)
        self.settle(1.1)
        self.assertEqual(self.link().Mbps, 1000)
        self.assertEqual(self.setup(chip, 'high'), 0)
        self.settle(1.1)
        self.assertEqual((self.link().Mbps, self.m.totals['multigig_below_superspeed']), (1000, 0))
        self.assertEqual(self.chip.Stop(self.state, c.byref(self.io)), 0)
        self.assertFalse(self.m.chip.receiving())


for _chip in r.NEWER:
    setattr(NewerDriverTests, 'test_driver_%s' % _chip.replace('-', '_'),
            pending(_chip)(lambda self, chip=_chip: self.drive(chip)))


if __name__ == '__main__':
    unittest.main()
