#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests of ncmemu.py, the SIMULATED CDC-NCM function, against
cdcnet's own descriptor probe and NCM cores (cdcdesc.c, cdcncm.c, built into
out/net/cdcnet/hostabi/cdccore.dll by host.ps1). No physical USB device.
Run: python -B ncmsimtest.py
"""
import ctypes as c
import io
import json
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ncmemu as n  # noqa: E402
from rtlemu import HOST_MAC, HOST_IP, GUEST_IP, ipv4, checksum  # noqa: E402

DLL_PATH = Path(__file__).resolve().parents[3] / 'out/net/cdcnet/hostabi/cdccore.dll'
U8P = c.POINTER(c.c_ubyte)
EMIT = c.CFUNCTYPE(None, c.c_void_p, U8P, c.c_ulong)


class Fn(c.Structure):
    _fields_ = [(x, c.c_ubyte) for x in ('Protocol', 'ConfigValue', 'Control', 'Data', 'DataAlt',
                                         'DataAltZero', 'In', 'Out', 'Interrupt', 'IntInterval')] + \
        [(x, c.c_ushort) for x in ('InMax', 'OutMax', 'IntMax')] + \
        [(x, c.c_ubyte) for x in ('MacIndex', 'NcmCaps', 'HasUnion', 'NumInterfaces')] + \
        [(x, c.c_ushort) for x in ('MaxSegment', 'McFilters')]


class Params(c.Structure):
    _fields_ = [('Formats', c.c_ushort), ('InMaxSize', c.c_ulong)] + \
        [(x, c.c_ushort) for x in ('InDivisor', 'InRemainder', 'InAlignment')] + \
        [('OutMaxSize', c.c_ulong)] + \
        [(x, c.c_ushort) for x in ('OutDivisor', 'OutRemainder', 'OutAlignment', 'OutMaxDatagrams')]


class Wire:
    def __init__(self):
        self.messages = []

    def sendall(self, raw):
        kind, length, ident = struct.unpack('<III', raw[:12])
        assert length == len(raw) - 12
        self.messages.append((kind, ident, raw[12:]))

    def take(self, kind):
        out = [m for m in self.messages if m[0] == kind]
        self.messages = [m for m in self.messages if m[0] != kind]
        return out


class Clock:
    def __init__(self):
        self.t = 50.0

    def __call__(self):
        return self.t


def buf(data):
    return (c.c_ubyte * max(1, len(data))).from_buffer_copy(data or b'\0')


@unittest.skipUnless(DLL_PATH.exists(), 'run host.ps1 first')
class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        d = c.CDLL(str(DLL_PATH))
        d.CdcProbeConfig.argtypes = [c.c_void_p, c.c_ulong, c.c_uint, c.POINTER(Fn)]
        d.CdcParseMacString.argtypes = [c.c_void_p, c.c_ulong, U8P]
        d.CdcNcmParams.argtypes = [c.c_void_p, c.c_ulong, c.c_ulong, c.c_ulong, c.POINTER(Params)]
        d.CdcNcmRxParse.argtypes = [c.c_void_p, c.c_ulong, EMIT, c.c_void_p]
        d.CdcNcmTxPlan.argtypes = [c.POINTER(Params), c.POINTER(c.c_ulong), c.c_ulong, c.c_ulong,
                                   c.POINTER(c.c_ulong), c.POINTER(c.c_ulong), c.POINTER(c.c_ulong)]
        d.CdcNcmTxPlan.restype = c.c_ulong
        d.CdcNcmTxFinish.argtypes = [c.c_void_p, c.c_ushort, c.POINTER(c.c_ulong), c.POINTER(c.c_ulong),
                                     c.c_ulong, c.c_ulong, c.c_ulong]
        d.CdcNcmTxFinish.restype = None
        for f in ('CdcProbeConfig', 'CdcParseMacString', 'CdcNcmParams', 'CdcNcmRxParse'):
            getattr(d, f).restype = c.c_long
        cls.d = d

    def model(self, **kw):
        layout = kw.pop('in_layout', 'front')
        flap = kw.pop('flap_after', None)
        self.wire, self.trace, self.clock = Wire(), io.StringIO(), Clock()
        m = n.Model(self.wire, self.trace, n.Function(**kw), now=self.clock, in_layout=layout, flap_after=flap)
        m.peer_caps = n.CAPS & ~(1 << n.CAP_ID64)
        m.connect()
        self.wire.messages.clear()
        return m

    def control(self, m, request, rt, value=0, index=0, length=0, payload=b''):
        m.control(5, struct.pack('<BBBBHHH', 0x80 if rt & 128 else 0, request, rt, 0, value, index, length) + payload)
        reply = self.wire.take(100)[-1][2]
        return reply[3], reply[10:]

    def probe(self, config):
        f = Fn()
        rc = self.d.CdcProbeConfig(buf(config), len(config), 1 << 3, c.byref(f))
        return rc, f

    def params(self, m):
        st, data = self.control(m, 0x80, 0xa1, length=28)
        self.assertEqual(st, 0)
        p = Params()
        self.assertEqual(self.d.CdcNcmParams(buf(data), len(data), 16384, 16384, c.byref(p)), 0)
        return p

    def bring_up(self, m):
        """The host side as cdcnet's NcmSetup orders it, then the data alternate."""
        m.handle(6, 1, b'\x01')
        p = self.params(m)
        caps = m.f.caps
        if p.Formats & 2:
            self.assertEqual(self.control(m, 0x84, 0x21)[0], 0)
        size = struct.pack('<I', p.InMaxSize) + (bytes(4) if caps & 0x20 else b'')
        self.assertEqual(self.control(m, 0x86, 0x21, length=len(size), payload=size)[0], 0)
        if caps & 0x10:
            self.assertEqual(self.control(m, 0x8a, 0x21)[0], 0)
        if caps & 0x08:
            st, got = self.control(m, 0x87, 0xa1, length=2)
            self.assertEqual((st, got), (0, struct.pack('<H', 1514)))
            self.assertEqual(self.control(m, 0x88, 0x21, length=2, payload=got)[0], 0)
        m.handle(15, 2, bytes((n.EP_INT,)))
        m.handle(9, 3, b'\x01\x01')
        self.assertEqual(self.wire.take(11)[-1][2], b'\0\1\1')
        return p

    def rx(self, m, length=16384):
        m.handle(101, 9, struct.pack('<BBHIH', n.EP_IN, 0, length & 0xffff, 0, length >> 16))
        got = self.wire.take(101)
        if not got:
            return None
        ntb = got[-1][2][10:]
        frames = []
        emit = EMIT(lambda _, p, k: frames.append(c.string_at(p, k)))
        rc = self.d.CdcNcmRxParse(buf(ntb), len(ntb), emit, None)
        self.assertEqual(rc, len(frames), ntb[:32].hex())
        return frames

    def tx(self, m, p, frames, sequence=1):
        """cdcnet's own TxPlan/TxFinish, then the model's NCM check."""
        k = len(frames)
        lengths = (c.c_ulong * k)(*[len(f) for f in frames])
        offsets, ndp, total = (c.c_ulong * k)(), c.c_ulong(), c.c_ulong()
        used = self.d.CdcNcmTxPlan(c.byref(p), lengths, k, 16384, offsets, c.byref(ndp), c.byref(total))
        self.assertEqual(used, k)
        b = (c.c_ubyte * total.value)()
        for i, f in enumerate(frames):
            c.memmove(c.addressof(b) + offsets[i], f, len(f))
        self.d.CdcNcmTxFinish(b, sequence, lengths, offsets, k, ndp.value, total.value)
        data = bytes(b)
        m.handle(101, 7, struct.pack('<BBHIH', n.EP_OUT, 0, len(data), 0, 0) + data)
        self.assertEqual(self.wire.take(101)[-1][2][:2], bytes((n.EP_OUT, 0)))
        return data

    # -- tests -------------------------------------------------------------
    def test_driver_probe_takes_ncm(self):
        for speed in ('high', 'full'):
            for caps, mc in ((0x21, 0), (0x00, 0), (0x08, 16), (0x3b, 4)):
                m = self.model(speed=speed, caps=caps, mc_filters=mc)
                rc, f = self.probe(m.f.config[speed])
                self.assertEqual(rc, 0, (speed, caps))
                self.assertEqual((f.Protocol, f.Control, f.Data, f.DataAlt, f.DataAltZero),
                                 (3, 0, 1, 1, 1))
                self.assertEqual((f.In, f.Out, f.Interrupt, f.MacIndex, f.NcmCaps, f.McFilters, f.MaxSegment),
                                 (0x81, 0x02, 0x83, 3, caps, mc, 1514))
                self.assertEqual(f.InMax, 512 if speed == 'high' else 64)
        mac = (c.c_ubyte * 6)()
        s = m.f.strings[3]
        self.assertEqual(self.d.CdcParseMacString(buf(s), len(s), mac), 0)
        self.assertEqual(bytes(mac), n.GUEST_MAC)

    def test_parameters_and_setup_requests(self):
        m = self.model(caps=0x3b, ntb32=True, out_divisor=512, out_remainder=14, out_alignment=16)
        p = self.bring_up(m)
        self.assertEqual((p.Formats, p.InMaxSize, p.OutDivisor, p.OutRemainder, p.OutAlignment, p.OutMaxDatagrams),
                         (3, 16384, 512, 14, 16, 32))
        events = [json.loads(x)['event'] for x in self.trace.getvalue().splitlines()]
        for e in ('ntb_format', 'ntb_input_size', 'crc_mode', 'alternate', 'link'):
            self.assertIn(e, events)
        # Requests the function refuses: SET_NTB_FORMAT 1 without NTB32,
        # input size below the 2048 minimum, an 8-byte input size without D5.
        m = self.model(caps=0x00)
        m.handle(6, 1, b'\x01')
        self.assertEqual(self.control(m, 0x84, 0x21, value=1)[0], 4)
        self.assertEqual(self.control(m, 0x86, 0x21, length=4, payload=struct.pack('<I', 1024))[0], 4)
        self.assertEqual(self.control(m, 0x86, 0x21, length=8, payload=bytes(8))[0], 4)
        self.assertEqual(self.control(m, 0x43, 0x21, value=0x0c)[0], 4)     # no filter without D0

    def test_notifications_after_the_data_alternate(self):
        m = self.model(flap_after=5.0)
        self.bring_up(m)
        notes = [x[2][4:] for x in self.wire.take(103)]
        self.assertEqual(notes[0], struct.pack('<BBHHH', 0xa1, 0, 1, 0, 0))
        self.assertEqual(notes[1][:8], struct.pack('<BBHHH', 0xa1, 0x2a, 0, 0, 8))
        self.assertEqual(struct.unpack('<II', notes[1][8:]), (480000000, 480000000))
        self.clock.t += 5.1
        m.pump()
        self.clock.t += 3.1
        m.pump()
        notes = [x[2][4:] for x in self.wire.take(103)]
        self.assertEqual([x[1:4] for x in notes], [b'\0\0\0', b'\0\1\0', b'\x2a\0\0'])

    def test_receive_layouts_through_the_driver_parser(self):
        for layout in ('front', 'end', 'split'):
            m = self.model(in_layout=layout)
            self.bring_up(m)
            self.wire.messages.clear()
            frames = [b'\xff' * 6 + HOST_MAC + b'\x08\x00' + bytes([i]) * (46 + 300 * i) for i in range(5)]
            for f in frames:
                m.deliver(f)
            got = self.rx(m)
            self.assertEqual(got, frames, layout)
            m.deliver(frames[0])                 # a single datagram too
            self.assertEqual(self.rx(m), frames[:1], layout)
        # The NTB respects the host's input size.
        m = self.model()
        self.bring_up(m)
        size = struct.pack('<I', 2048)
        m.handle(9, 3, b'\x01\x00')
        self.assertEqual(self.control(m, 0x86, 0x21, length=4, payload=size)[0], 0)
        m.handle(9, 3, b'\x01\x01')
        for i in range(3):
            m.deliver(b'\xff' * 6 + HOST_MAC + b'\x08\x00' + bytes(1000))
        self.assertEqual(len(self.rx(m)), 1)
        self.assertEqual(len(self.rx(m)), 1)

    def test_transmit_layout_from_the_driver(self):
        for div, rem, align in ((4, 0, 4), (512, 14, 16), (8, 2, 8), (64, 0, 32)):
            m = self.model(out_divisor=div, out_remainder=rem, out_alignment=align)
            p = self.bring_up(m)
            frames = [n.GUEST_MAC + HOST_MAC + b'\x08\x00' + bytes([k]) * (46 + 200 * k) for k in range(6)]
            self.tx(m, p, frames)
            bad = [json.loads(x) for x in self.trace.getvalue().splitlines() if '"ntb_out_bad"' in x]
            self.assertEqual(bad, [], (div, rem, align))
            self.assertEqual(m.totals['tx_datagrams'], 6)
        # A broken NTB is reported, not passed silently.
        datagrams, problems = n.parse_out_ntb(b'NCMH' + bytes(20), m.f)
        self.assertTrue(problems)

    def test_arp_icmp_dhcp_round_trip(self):
        m = self.model(caps=0x21)
        p = self.bring_up(m)
        self.assertEqual(self.control(m, 0x43, 0x21, value=0x0c)[0], 0)      # directed + broadcast
        mac = n.GUEST_MAC
        arp = b'\xff' * 6 + mac + b'\x08\x06' + struct.pack('!HHBBH', 1, 0x0800, 6, 4, 1) + \
            mac + GUEST_IP + bytes(6) + HOST_IP
        icmp = b'\x08\0\0\0\0\1\0\1' + b'NCM' * 100
        icmp = icmp[:2] + struct.pack('!H', checksum(icmp)) + icmp[4:]
        ping = HOST_MAC + mac + b'\x08\x00' + ipv4(GUEST_IP, HOST_IP, 1, icmp)
        self.tx(m, p, [arp, ping])
        frames = self.rx(m)
        self.assertEqual(len(frames), 2)
        self.assertEqual(frames[0][12:14] + frames[0][20:22], b'\x08\x06\0\2')
        self.assertEqual(frames[1][14 + 20], 0)
        # Multicast is refused with the filter as set.
        m.deliver(bytes.fromhex('01005e000001') + HOST_MAC + b'\x08\x00' + bytes(46))
        self.assertEqual(m.totals['rx_filtered'], 1)


if __name__ == '__main__':
    unittest.main()
