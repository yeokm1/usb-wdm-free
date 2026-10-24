#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests of rtlemu.py: the SIMULATED RTL8152/8153 driven by
rtl815x's own chip core (rtlchip.c and netchip.c, built into
out/net/rtl815x/hostabi/rtlcore.dll by host.ps1) through the model's
usbredir control, bulk and interrupt paths. No physical USB device.
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


@unittest.skipUnless(DLL_PATH.exists(), 'run host.ps1 first')
class Tests(unittest.TestCase):
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
        for frame in frames:
            if len(frame) < 60:
                frame += bytes(60 - len(frame))
            h = (c.c_ubyte * 8)()
            self.chip.TxHeader(self.state, h, len(frame), self.probe.OutMax, len(frames) == 1)
            out += bytes(h) + frame
            while len(out) & 3:
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

    # -- tests -------------------------------------------------------------
    def test_probe_and_bring_up_every_chip(self):
        for chip, speed, version, mbps in (('8153', 'high', 4, 1000), ('8152', 'high', 2, 100),
                                           ('8153b', 'high', 9, 1000), ('8153', 'full', 4, 1000)):
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
        self.assertFalse(self.link().Up)
        self.assertFalse(self.m.chip.receiving() and self.m.chip.pla[r.PLA_PHYSTATUS])
        self.settle(1.1)                              # cable in
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
        # stays accepted (r8152 _rtl8152_set_rx_mode always sets RCR_AB).
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
            self.m.transmit(struct.pack('<II', 60, 0) + bytes(60))      # no TX_FS|TX_LS

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


if __name__ == '__main__':
    unittest.main()
