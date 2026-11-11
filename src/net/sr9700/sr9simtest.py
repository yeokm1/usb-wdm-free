#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests of sremu.py: the SIMULATED SR9700 driven by sr9700's own chip
core (srchip.c and netchip.c, built into
out/net/sr9700/hostabi/srchipcore.dll by simhost.ps1) through the model's
usbredir control, bulk and interrupt paths, as ax7simtest.py does for
ax88772. No physical USB device.
Run: python -B sr9simtest.py
"""
import ctypes as c
import io
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(1, str(HERE.parent / 'ax88772'))
import sremu as s  # noqa: E402
import ax7simtest as t  # noqa: E402

DLL_PATH = HERE.parents[2] / 'out/net/sr9700/hostabi/srchipcore.dll'
P = c.POINTER
NetChip = type('NetChip', (c.Structure,), {'_fields_': t.NetChip._fields_[:-1]})   # no RxReset


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.chip = NetChip.in_dll(c.CDLL(str(DLL_PATH)), 'NetPackageChip')

    def setup(self, **kw):
        self.wire, self.trace, self.clock = t.Wire(), io.StringIO(), t.Clock()
        self.m = s.Model(self.wire, self.trace, now=self.clock, **kw)
        self.m.peer_caps = s.CAPS & ~(1 << s.ax7emu.CAP_ID64)
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
            kind, ident, reply = [x for x in self.wire.messages if x[0] == 100][-1]
            self.wire.messages = [x for x in self.wire.messages if x[0] != 100]
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

        self.cb = (t.CONTROL(control), t.SLEEP(sleep))
        self.io = t.NetIo(None, self.cb[0], self.cb[1], 0, 0)
        dev, cfg = t.buf(self.m.device), t.buf(self.m.config_desc)
        configs = (c.c_void_p * 1)(c.cast(cfg, c.c_void_p))
        sizes = (c.c_ulong * 1)(len(self.m.config_desc))
        self.probe = t.NetProbe()
        self.keep = (dev, cfg, configs, sizes)
        self.assertEqual(self.chip.Probe(c.cast(dev, c.c_void_p), len(self.m.device), configs, sizes, 1,
                                         c.byref(self.probe)), 0)
        self.params = t.NetParams()
        return self.chip.Start(self.state, c.byref(self.io), c.byref(self.probe), c.byref(self.params))

    def settle(self, seconds):
        for _ in range(int(seconds / 0.05) + 1):
            self.clock.t += 0.05
            self.m.pump()

    def link(self):
        link = t.NetLink()
        self.assertEqual(self.chip.Link(self.state, c.byref(self.io), c.byref(link)), 0)
        return link

    def tx(self, frame):
        if len(frame) < 60:
            frame += bytes(60 - len(frame))
        out = (c.c_ubyte * (2 + 1514 + 1))()
        self.chip.TxHeader(self.state, out, len(frame), self.probe.OutMax, 1)
        c.memmove(c.byref(out, 2), frame, len(frame))
        total = c.c_ulong(2 + len(frame))
        self.assertEqual(self.chip.TxEnd(self.state, out, c.byref(total), len(out), self.probe.OutMax), 0)
        data = bytes(out)[:total.value]
        self.m.handle(101, 7, struct.pack('<BBHIH', s.EP_OUT, 0, len(data), 0, 0) + data)
        self.assertEqual(self.wire.take(101)[-1][2][:2], bytes((s.EP_OUT, 0)))
        return data

    def rx_all(self):
        frames, transfers = [], 0
        emit = t.EMIT(lambda _, p, n: frames.append(c.string_at(p, n)))
        while True:
            self.m.handle(101, 9, struct.pack('<BBHIH', s.EP_IN, 0, self.params.RxSize, 0, 0))
            replies = self.wire.take(101)
            if not replies:
                self.m.handle(21, 9, b'')
                self.wire.take(101)
                return frames, transfers
            data = replies[-1][2][10:]
            stats = t.RxStats()
            self.assertGreaterEqual(self.chip.RxParse(self.state, t.buf(data), len(data), emit, None,
                                                      c.byref(stats)), 0)
            transfers += 1

    def up(self):
        self.assertEqual(self.setup(), 0)
        self.settle(1.1)
        self.assertTrue(self.link().Up)

    def test_start(self):
        self.assertEqual(self.setup(), 0)
        self.assertEqual((self.probe.Vendor, self.probe.Product, self.probe.Speed), (0x0fe6, 0x9700, 1))
        self.assertEqual((self.probe.In, self.probe.Out, self.probe.Interrupt), (0x81, 0x02, 0x83))
        self.assertEqual(bytes(self.params.Mac), s.MODEL_MAC)
        self.assertEqual((self.params.RxSize, self.params.RxConfigRead), (3072, 0x31))
        self.assertEqual(self.m.totals['vendor_refused'], 0)
        self.settle(1.1)
        link = self.link()
        self.assertEqual((link.Up, link.Mbps, link.FullDuplex), (1, 10, 0))

    def test_composite_9702(self):
        """The 0FE6:9702 composite (--composite): class 0 at the device, the
        driver disc's interface 0, the Ethernet on interface 1, which the
        driver's probe takes and brings up as the 0FE6:9700."""
        self.assertEqual(self.setup(composite=True), 0)
        self.assertEqual((self.m.device[4], self.m.device[5]), (0, 0))
        self.assertEqual(self.m.config_desc[4], 2)                       # bNumInterfaces
        self.assertEqual((self.probe.Vendor, self.probe.Product, self.probe.Interface), (0x0fe6, 0x9702, 1))
        self.assertEqual((self.probe.In, self.probe.Out, self.probe.Interrupt), (0x81, 0x02, 0x83))
        self.settle(1.1)
        link = self.link()
        self.assertEqual((link.Up, link.Mbps), (1, 10))
        # usb-redir: the endpoints belong to interface 1; both interfaces answer alternate 0
        self.m.info()
        ep_info = [x for x in self.wire.messages if x[0] == 5][-1][2]
        for slot in (16 + (s.EP_IN & 15), s.EP_OUT, 16 + (s.EP_INT & 15)):
            self.assertEqual(ep_info[64 + slot], 1)
        iface = [x for x in self.wire.messages if x[0] == 4][-1][2]
        self.assertEqual(iface[0], 2)
        for number in (0, 1):
            self.m.handle(10, 50 + number, bytes((number,)))
            self.assertEqual(self.wire.take(11)[-1][2], bytes((0, number, 0)))
        self.m.handle(10, 52, bytes((2,)))
        self.assertEqual(self.wire.take(11)[-1][2][0], 4)

    def test_traffic(self):
        self.up()
        self.tx(t.arp_request(s.MODEL_MAC))
        frames, _ = self.rx_all()
        self.assertEqual(len(frames), 1)
        for size, replies in ((32, 1), (1472, 1), (4000, 3)):
            payload = bytes((i * 3) & 0xff for i in range(size))
            data = b'\x08\x00\x00\x00' + struct.pack('!HH', 1, 1) + payload
            data = data[:2] + struct.pack('!H', s.checksum(data)) + data[4:]
            for packet in s.ipv4_fragments(bytes((192, 168, 77, 2)), s.HOST_IP, 1, data, 7):
                self.tx(s.HOST_MAC + s.MODEL_MAC + b'\x08\x00' + packet)
            frames, transfers = self.rx_all()
            self.assertEqual(len(frames), replies, size)
        self.assertEqual(self.m.net.refused, 0)

    def test_boundary_pad(self):
        self.up()
        frame = s.HOST_MAC + s.MODEL_MAC + b'\x88\xb5' + bytes(62 - 14)     # 2 + 62 = 64
        data = self.tx(frame)
        self.assertEqual((len(data), data[0], data[-1]), (65, 63, 0))
        self.assertEqual(len(self.tx(frame[:-1])), 63)        # 61 bytes: no boundary, no pad
        self.assertEqual(self.m.totals['tx_padded'], 1)

    def test_packet_at_a_time(self):
        """A host that sends one 64-byte packet at a time (UHCI): the
        transfer is joined up until its short packet."""
        self.up()
        sent = []
        real = self.m.transmit
        self.m.transmit = lambda data: sent.append(data) or real(data)
        for size in (61, 62, 63, 200):                # 62 + 2 = 64: the pad byte goes alone
            frame = s.HOST_MAC + s.MODEL_MAC + b'\x88\xb5' + b'\x5a' * (size - 14)
            out = (c.c_ubyte * (2 + 1514 + 1))()
            self.chip.TxHeader(self.state, out, len(frame), self.probe.OutMax, 1)
            c.memmove(c.byref(out, 2), frame, len(frame))
            total = c.c_ulong(2 + len(frame))
            self.assertEqual(self.chip.TxEnd(self.state, out, c.byref(total), len(out), self.probe.OutMax), 0)
            data = bytes(out)[:total.value]
            for at in range(0, len(data), 64):
                piece = data[at:at + 64]
                self.m.handle(101, 7, struct.pack('<BBHIH', s.EP_OUT, 0, len(piece), 0, 0) + piece)
            if len(data) % 64 == 0:
                self.m.handle(101, 7, struct.pack('<BBHIH', s.EP_OUT, 0, 0, 0, 0))
            self.assertEqual(sent[-1], data, size)
        self.assertEqual((self.m.totals['tx_frames'], self.m.totals['tx_padded']), (4, 1))
        with self.assertRaises(ValueError):           # a full packet past the header's length
            self.m.receive_out(struct.pack('<H', 30) + bytes(62))

    def test_interface_reselection_drops_a_cut_off_transfer(self):
        self.up()
        self.m.handle(101, 7, struct.pack('<BBHIH', s.EP_OUT, 0, 64, 0, 0) + struct.pack('<H', 200) + bytes(62))
        self.m.handle(9, 4, b'\0\0')
        self.tx(t.arp_request(s.MODEL_MAC))
        self.assertEqual(self.m.totals['tx_frames'], 1)
        frame = t.arp_request(s.MODEL_MAC) + bytes(40)            # 82 bytes: two packets
        data = struct.pack('<H', len(frame)) + frame
        self.m.handle(101, 7, struct.pack('<BBHIH', s.EP_OUT, 0, 64, 0, 0) + data[:64])
        self.m.handle(9, 5, b'\0\1')                                # refused: the transfer stays
        self.m.handle(101, 7, struct.pack('<BBHIH', s.EP_OUT, 0, len(data) - 64, 0, 0) + data[64:])
        self.assertEqual(self.m.totals['tx_frames'], 2)

    def test_link_at_power_on_and_transfer_limit(self):
        """CoreChips' driver writes no 0x1F and waits for the link; a receive
        transfer holds whole packets up to RX_TRANSFER_MAX bytes."""
        wire, clock = t.Wire(), t.Clock()
        m = s.Model(wire, io.StringIO(), now=clock)
        m.peer_caps = s.CAPS & ~(1 << s.ax7emu.CAP_ID64)
        m.connect()
        m.handle(6, 1, b'\x01')
        for _ in range(30):
            clock.t += 0.05
            m.pump()
        self.assertTrue(m.chip.link)                                     # no PHY reset needed
        m.chip.write(s.WRITE1, 1, s.REG_PHYRST, b'')                     # sr9700f's and Linux's reset
        m.pump()
        self.assertFalse(m.chip.link)
        m.chip.write(s.WRITE1, 0, s.REG_PHYRST, b'')
        for _ in range(30):
            clock.t += 0.05
            m.pump()
        self.assertTrue(m.chip.link)
        m.chip.regs[s.REG_RXCTL] = s.RX_ON | s.RX_PROMISC
        for _ in range(5):
            m.deliver(bytes(14) + bytes(1500))
        wire.messages.clear()
        m.handle(101, 9, struct.pack('<BBHIH', s.EP_IN, 0, 16384 & 0xffff, 0, 0))
        got = wire.take(101)[-1][2][10:]
        self.assertEqual(len(got), 2 * (3 + 1514 + 4))                  # two whole packets, not five

    def test_status_and_flap(self):
        self.assertEqual(self.setup(flap_after=2, flap_for=2), 0)
        self.m.handle(15, 1, bytes((s.EP_INT,)))
        self.settle(1.1)
        self.assertTrue(self.link().Up)
        self.settle(2.1)
        data = self.wire.take(103)[-1][2][4:]
        self.assertEqual(self.chip.Status(self.state, t.buf(data), len(data)), 0)
        self.assertEqual(self.chip.Poll(self.state, c.byref(self.io)), 1)
        self.assertFalse(self.link().Up)
        self.settle(2.1)
        self.assertTrue(self.link().Up)


if __name__ == '__main__':
    unittest.main()
