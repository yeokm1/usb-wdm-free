#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests of chipemu.py, the simulated bridges of the chip module legs.
The request values are the ones acmhost.c expects the driver to send
(design 6.3); actual USB and Windows results are separate."""
import io
import json
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import acmemu   # noqa: E402
import chipemu  # noqa: E402

LEGACY = acmemu.CAPS & ~(acmemu.CAP_64BIT_IDS | acmemu.CAP_32BIT_BULK)


class Connection:
    def __init__(self):
        self.data = bytearray()

    def sendall(self, data):
        self.data.extend(data)

    def messages(self):
        result = []
        while self.data:
            kind, length, ident = struct.unpack('<III', self.data[:12])
            result.append((kind, ident, bytes(self.data[12:12 + length])))
            del self.data[:12 + length]
        return result


class ChipTests(unittest.TestCase):
    def model(self, part):
        self.connection = Connection()
        self.trace = io.StringIO()
        m = chipemu.ChipModel(self.connection, self.trace, part)
        m.peer_caps = LEGACY
        m.handle(6, 1, b'\x01')
        self.connection.messages()
        return m

    def control(self, m, reqtype, request, value=0, index=0, length=0, data=b''):
        header = struct.pack('<BBBBHHH', 0x80 if reqtype & 0x80 else 0, request, reqtype, 0, value, index, length)
        m.handle(100, 41, header + data)
        kind, ident, reply = [x for x in self.connection.messages() if x[0] == 100][-1]
        return struct.unpack('<BBBBHHH', reply[:10])[3], reply[10:]

    def events(self, name):
        return [json.loads(line) for line in self.trace.getvalue().splitlines() if json.loads(line)['event'] == name]

    def bulk_out(self, m, endpoint, data):
        m.handle(101, 7, struct.pack('<BBHI', endpoint, 0, len(data), 0) + data)

    def bulk_in(self, m, endpoint, length, ident=9):
        m.handle(101, ident, struct.pack('<BBHI', endpoint, 0, length, 0))
        replies = [x for x in self.connection.messages() if x[0] == 101 and x[1] == ident]
        return replies[-1][2][8:] if replies else None

    def test_ftdi_rates(self):
        self.assertEqual(chipemu.ftdi_rate(0x4138, False), 9600)
        self.assertEqual(chipemu.ftdi_rate(0x001A, False), 115385)
        self.assertEqual(chipemu.ftdi_rate(0, False), 3000000)
        self.assertEqual(chipemu.ftdi_rate(1, False), 2000000)
        self.assertEqual(chipemu.ftdi_rate(0x2C068, True), 115246)
        self.assertEqual(chipemu.ftdi_rate(0x20000, True), 12000000)

    def test_ftdi_channel_line_and_framing(self):
        m = self.model('ft4232h')
        self.assertEqual(self.control(m, 0x40, 0, 0, 3)[0], 0)              # RESET, channel 3
        self.assertEqual(self.control(m, 0x40, 4, 0x1207, 3)[0], 0)         # 7E2
        self.assertEqual(self.control(m, 0x40, 3, 0xC068, 0x0203)[0], 0)    # 115200, H divisor
        self.assertEqual(self.events('rate')[-1]['rate'], 115246)
        self.assertEqual(self.events('framing')[-1]['bits'], 7)
        self.assertEqual(self.control(m, 0x40, 4, 0x0006, 3)[0], 4)         # 6 bits refused
        self.assertEqual(self.control(m, 0x40, 1, 0x0303, 3)[0], 0)         # DTR RTS
        port = m.fns[2]
        self.assertTrue(port.dtr and port.rts)
        status = self.bulk_in(m, port.inp, 512)
        self.assertEqual(status, bytes([0xB1, 0x60]))                     # CTS DSR DCD, idle
        self.bulk_out(m, port.out, b'x' * 600)
        data = self.bulk_in(m, port.inp, 4096, ident=10)
        self.assertEqual(len(data), 604)                                  # 510 + 90 with two headers
        self.assertEqual(data[:2], bytes([0xB1, 0x60]))
        self.assertEqual(data[512:514], bytes([0xB1, 0x60]))
        self.assertEqual(self.control(m, 0x40, 1, 0x0300, 0)[0], 4)         # no channel 0 on an H part

    def test_ftdi_break_and_flow(self):
        m = self.model('ftx')
        port = m.fns[0]
        self.control(m, 0x40, 1, 0x0303, 1)
        self.bulk_in(m, port.inp, 64)
        self.control(m, 0x40, 4, 0x4008, 1)                               # break on
        data = self.bulk_in(m, port.inp, 64, ident=11)
        self.assertEqual(data, bytes([0xB1, 0x70, 0]))
        self.control(m, 0x40, 1, 0x0201, 1)                               # RTS low: CTS low
        self.control(m, 0x40, 2, 0, 0x0101)                               # RTS/CTS on
        self.bulk_out(m, port.out, b'held')
        data = self.bulk_in(m, port.inp, 64, ident=12)
        self.assertEqual(data[2:], b'')
        self.control(m, 0x40, 1, 0x0303, 1)                               # ignored in flow mode
        self.assertFalse(port.rts)
        self.assertEqual(self.events('rts_ignored')[-1]['function'], 0)
        self.control(m, 0x40, 2, 0, 0x0001)                               # off, then RTS again
        self.control(m, 0x40, 1, 0x0303, 1)
        data = self.bulk_in(m, port.inp, 64, ident=13)
        self.assertEqual(data[2:], b'held')
        self.control(m, 0x40, 0, 0, 1)                                    # RESET drops the lines
        self.assertFalse(port.dtr or port.rts)

    def test_pl2303_startup(self):
        m = self.model('pl2303hx')
        seq = [('r', 0x8484), ('w', 0x0404, 0), ('r', 0x8484), ('r', 0x8383), ('r', 0x8484),
               ('w', 0x0404, 1), ('r', 0x8484), ('r', 0x8383), ('w', 0, 1), ('w', 1, 0), ('w', 2, 0x44)]
        for op in seq:
            if op[0] == 'r':
                self.control(m, 0xC0, 0x01, op[1], 0, 1)
            else:
                self.control(m, 0x40, 0x01, op[1], op[2])
        self.assertEqual(self.events('startup')[-1]['ok'], 1)
        self.control(m, 0xC0, 0x01, 0x8485, 0, 1)                        # a wrong read before the end
        self.control(m, 0x40, 0x01, 2, 0x44)
        self.assertEqual(self.events('startup')[-1]['ok'], 0)
        m.handle(6, 2, b'\x01')                                           # a new configuration
        self.connection.messages()
        self.assertEqual(self.control(m, 0x21, 0x20, 0, 0, 7, struct.pack('<IBBB', 9600, 0, 0, 8))[0], 4)
        n = self.model('pl2303hx')                                        # no start sequence at all
        self.assertEqual(self.control(n, 0x21, 0x20, 0, 0, 7, struct.pack('<IBBB', 9600, 0, 0, 8))[0], 4)
        self.assertEqual(self.events('not_started')[-1]['request'], 0x20)
        h = self.model('pl2303h')                                         # the H: 8-byte EP0, register 2 = 0x24
        self.assertEqual(h.device[7], 8)
        for op in seq[:-1] + [('w', 2, 0x24)]:
            if op[0] == 'r':
                self.control(h, 0xC0, 0x01, op[1], 0, 1)
            else:
                self.control(h, 0x40, 0x01, op[1], op[2])
        self.assertEqual(self.events('startup')[-1]['ok'], 1)
        self.control(h, 0x40, 0x01, 0, 0x41)
        self.assertTrue(h.fns[0].flow)
        tb = self.model('pl2303tb')
        self.assertEqual((tb.pid, tb.bcd_device), (0x2304, 0x0500))
        self.assertEqual(self.control(tb, 0xC0, 0x01, 0x8080, 0, 1)[0], 0)

    def startup(self, m, last=0x44):
        for op in [('r', 0x8484), ('w', 0x0404, 0), ('r', 0x8484), ('r', 0x8383), ('r', 0x8484),
                   ('w', 0x0404, 1), ('r', 0x8484), ('r', 0x8383), ('w', 0, 1), ('w', 1, 0), ('w', 2, last)]:
            if op[0] == 'r':
                self.control(m, 0xC0, 0x01, op[1], 0, 1)
            else:
                self.control(m, 0x40, 0x01, op[1], op[2])

    def test_pl2303(self):
        m = self.model('pl2303hx')
        self.startup(m)
        self.assertEqual(self.control(m, 0xC0, 0x01, 0x8080, 0, 1)[0], 4)    # no HX status read on HX
        self.assertEqual(self.control(m, 0x40, 0x01, 0x0002, 0x44)[0], 0)
        self.assertEqual(m.regs[2], 0x44)
        self.assertEqual(self.control(m, 0x21, 0x20, 0, 1, 7, bytes(7))[0], 4)   # wIndex must be 0
        self.control(m, 0x21, 0x20, 0, 0, 7, struct.pack('<IBBB', 100000, 0, 0, 8))
        self.assertEqual(self.events('rate')[-1]['rate'], 9600)           # not a direct rate
        line = bytes([240, 4, 0, 0x80, 0, 0, 8])                          # 100000 by divisor
        self.assertEqual(self.control(m, 0x21, 0x20, 0, 0, 7, line)[0], 0)
        self.assertEqual(self.events('rate')[-1]['rate'], 100000)
        self.assertEqual(self.control(m, 0x21, 0x20, 0, 0, 7, struct.pack('<IBBB', 9600, 1, 3, 7))[0], 0)
        self.assertEqual(self.events('rate')[-1]['rate'], 9600)
        self.assertEqual(self.control(m, 0xC0, 0x01, 0x80, 0, 1)[1], b'\x01')   # register 0 = 1 after the start
        self.control(m, 0x40, 0x01, 0, 0x61)
        self.assertTrue(m.fns[0].flow)
        g = self.model('pl2303g')
        self.assertEqual(self.control(g, 0x21, 0x20, 0, 0, 7, bytes([240, 4, 0, 0x80, 0, 0, 8]))[0], 4)
        self.control(g, 0x21, 0x20, 0, 0, 7, struct.pack('<IBBB', 100000, 0, 0, 8))
        self.assertEqual(self.events('rate')[-1]['rate'], 100000)         # any direct rate on a G
        self.assertEqual(self.control(g, 0xC0, 0x01, 0x8080, 0, 1)[0], 4)
        self.assertEqual(self.control(g, 0x40, 0x80, 0x07, 3)[0], 0)
        self.control(g, 0x40, 0x80, 0x0A, 0x18)
        self.assertTrue(g.fns[0].flow)
        ta = self.model('pl2303ta')
        self.assertEqual(self.control(ta, 0xC0, 0x01, 0x8080, 0, 1)[0], 0)
        self.assertEqual(chipemu.pl2303_rate(bytes([0x80, 0x07, 1, 0x80]), True), 100000)

    def test_lifecycle(self):
        m = self.model('ftx')
        port = m.fns[0]
        self.bulk_out(m, port.out, b'stale')
        self.control(m, 0x40, 0, 0, 1)                                   # RESET drops it
        self.control(m, 0x40, 1, 0x0303, 1)
        data = self.bulk_in(m, port.inp, 64, ident=20)
        self.assertEqual(data[2:], b'')
        c = self.model('cp2108')
        p0 = c.fns[0]
        self.bulk_out(c, p0.out, b'early')                               # before IFC_ENABLE
        self.assertEqual(self.events('dropped_disabled')[-1]['length'], 5)
        self.control(c, 0x41, 0x00, 1, 0)
        self.bulk_out(c, p0.out, b'kept')
        self.control(c, 0x41, 0x12, 0x0F, 0)                             # PURGE drops it
        self.bulk_out(c, p0.out, b'echo')
        self.assertEqual(self.bulk_in(c, p0.inp, 64, ident=21), b'echo')
        self.assertEqual(self.control(c, 0xC1, 0x03, 0x0800, 0)[0], 4)    # SET_LINE_CTL as IN
        self.assertEqual(self.control(c, 0x41, 0x08, 0, 0)[0], 4)         # GET_MDMSTS as OUT

    def test_cp210x(self):
        m = self.model('cp2105')
        self.assertEqual(self.control(m, 0xC0, 0xFF, 0x370B, 1, 1), (0, b'\x05'))
        self.assertEqual(self.control(m, 0x41, 0x1E, 0, 1, 4, struct.pack('<I', 9600))[0], 0)   # before enable
        self.assertEqual(self.events('before_enable')[-1]['request'], 0x1E)
        self.assertEqual(self.control(m, 0x41, 0x00, 1, 1)[0], 0)
        self.assertEqual(self.control(m, 0x41, 0x1E, 0, 1, 4, struct.pack('<I', 9600))[0], 0)
        self.assertEqual(self.control(m, 0x41, 0x03, 0x0800, 1)[0], 0)
        self.assertEqual(self.control(m, 0x41, 0x03, 0x0700, 1)[0], 4)     # 7 bits: not on the SCI
        self.assertEqual(self.control(m, 0x41, 0x03, 0x0802, 1)[0], 4)     # 2 stop bits: neither
        self.assertEqual(self.control(m, 0x41, 0x03, 0x0830, 1)[0], 4)     # mark parity: neither
        self.assertEqual(self.control(m, 0x41, 0x05, 1, 1)[0], 4)          # no break on the SCI
        self.control(m, 0x41, 0x07, 0x0303, 1)
        self.assertEqual(self.control(m, 0xC1, 0x08, 0, 1, 1)[1], bytes([0xB3]))
        status, block = self.control(m, 0xC1, 0x14, 0, 1, 16)
        self.assertEqual(len(block), 16)
        block = struct.pack('<IIII', 0x09, 0x80, 128, 128)
        self.control(m, 0x41, 0x13, 0, 1, 16, block)
        self.assertTrue(m.fns[1].flow and m.fns[1].rts)
        self.control(m, 0x41, 0x07, 0x0200, 1)                           # SET_MHS RTS low: ignored
        self.assertTrue(m.fns[1].rts)
        block = struct.pack('<IIII', 0x09, 0x00, 128, 128)                # through SET_FLOW: low
        self.control(m, 0x41, 0x13, 0, 1, 16, block)
        self.assertFalse(m.fns[1].rts)

    def test_linux_driver_requests(self):
        """What Linux 6.12's pl2303 and cp210x sent (docs/testing/2026-11-07-linux-reference.md)."""
        p = self.model('pl2303hx')
        status, line = self.control(p, 0xA1, 0x21, 0, 0, 7)                # GET_LINE_CODING first
        self.assertEqual((status, line), (0, struct.pack('<IBBB', 9600, 0, 0, 8)))
        c = self.model('cp2102')
        self.assertEqual(self.control(c, 0xC0, 0xFF, 0x370B, 0, 2)[0], 4)   # the two-byte quirk probe
        self.assertEqual(self.control(c, 0xC0, 0xFF, 0x370B, 0, 1), (0, b'\x02'))
        self.assertEqual(self.control(c, 0xC0, 0xFF, 0x370C, 0, 13)[0], 4)  # no GPIO on a CP2102
        self.control(c, 0x41, 0x00, 1, 0)                                # IFC_ENABLE
        self.assertEqual(self.control(c, 0x41, 0x15, 0xEC, 0)[0], 0)        # EMBED_EVENTS, escape EC
        self.bulk_out(c, c.fns[0].out, b'A\xecB')
        self.assertEqual(self.bulk_in(c, c.fns[0].inp, 64, ident=33), b'A\xec\x00B')
        self.control(c, 0x41, 0x05, 1, 0)                                # break: looped back
        self.control(c, 0x41, 0x05, 0, 0)
        self.assertEqual(self.bulk_in(c, c.fns[0].inp, 64, ident=34), b'\xec\x01\x10\x00')
        self.control(c, 0x41, 0x15, 0, 0)                                # events off: as before
        self.bulk_out(c, c.fns[0].out, b'\xec')
        self.assertEqual(self.bulk_in(c, c.fns[0].inp, 64, ident=35), b'\xec')
        self.control(c, 0x41, 0x15, 0xEC, 0)                             # on again, then disabled
        self.control(c, 0x41, 0x00, 0, 0)
        self.control(c, 0x41, 0x00, 1, 0)
        self.bulk_out(c, c.fns[0].out, b'\xec')
        self.assertEqual(self.bulk_in(c, c.fns[0].inp, 64, ident=36), b'\xec')
        self.control(c, 0x41, 0x15, 0xEC, 0)                             # on again, then a reset
        c.handle(3, 50, b'')
        self.assertEqual(c.fns[0].esc, 0)
        for part, size in (('cp2104', 13), ('cp2105', 15), ('cp2108', 0x49)):
            g = self.model(part)
            self.assertEqual(self.control(g, 0xC0, 0xFF, 0x370C, 0, size), (0, bytes(size)))
            self.assertEqual(self.control(g, 0xC0, 0xFF, 0x3711, 0, 2)[0], 0 if part == 'cp2105' else 4)

    def test_ref4_maker_driver_requests(self):
        """Prolific 3.8.31.1/2.1.51 and Silicon Labs 4.20 (docs/testing/2026-11-07-model-fixes-ref4.md)."""
        p = self.model('pl2303hx')
        self.assertEqual(self.control(p, 0x40, 0x01, 0x0404, 1)[0], 0)
        self.assertEqual(self.control(p, 0xC0, 0x01, 0x8484, 0, 1), (0, b'\x00'))   # not the 1 written
        self.control(p, 0x40, 0x01, 0x0606, 0xAA)
        self.assertEqual(self.control(p, 0xC0, 0x01, 0x8686, 0, 2), (0, b'\xaa\x00'))  # wLength 2
        u = chipemu.ChipModel(Connection(), io.StringIO(), 'pl2303hx')       # unconfigured
        u.peer_caps = LEGACY
        u.handle(100, 1, struct.pack('<BBBBHHH', 0, 0x01, 0x40, 0, 0x0000, 0x18, 0))
        reply = [x for x in u.connection.messages() if x[0] == 100][-1][2]
        self.assertEqual(reply[3], 0)
        c = self.model('cp2102')
        self.assertEqual(self.control(c, 0x41, 0x11, 0, 0)[0], 0)           # RESET, before IFC_ENABLE
        self.assertEqual(self.control(c, 0x41, 0x01, 0x0020, 0)[0], 0)      # SET_BAUDDIV 115200
        self.assertEqual(self.events('rate')[-1]['rate'], 115200)
        self.assertEqual(self.control(c, 0xC1, 0x02, 0, 0, 2), (0, struct.pack('<H', 0x20)))
        self.assertEqual(self.control(c, 0x41, 0x01, 0, 0)[0], 4)           # divisor 0
        self.assertEqual(self.control(c, 0x41, 0x03, 0x0741, 0)[0], 0)      # 7 bits, space, 1.5 stop

    def test_maker_driver_requests(self):
        # What the makers' own drivers sent in the 2026-11-06 reference runs.
        for part in ('ft232am', 'ft232bm', 'ft2232c', 'cp2101', 'cp2102', 'cp2103', 'cp2104'):
            self.assertIn(part, chipemu.PARTS)
        m = self.model('ft232h')
        self.assertEqual(self.control(m, 0xC0, 0x90, 0, 0x3F, 2), (0, b'\xff\xff'))  # blank EEPROM
        self.assertEqual(self.control(m, 0x40, 4, 0x0807, 0)[0], 0)          # channel 0, 7N1.5
        self.assertEqual(self.events('framing')[-1]['stop'], 1)
        self.assertEqual(self.control(m, 0x40, 6, 0x011A, 0)[0], 0)          # SET_EVENT_CHAR
        self.assertEqual(self.control(m, 0x40, 7, 0x0000, 0)[0], 0)          # SET_ERROR_CHAR
        self.assertEqual(self.control(m, 0x40, 9, 2, 0)[0], 0)               # latency 2 ms
        self.assertEqual(self.control(m, 0xC0, 0x0A, 0, 0, 1), (0, b'\x02'))
        b = chipemu.ChipModel(Connection(), io.StringIO(), 'ft232bm')        # unconfigured
        b.peer_caps = LEGACY
        b.handle(100, 1, struct.pack('<BBBBHHH', 0x80, 0x90, 0xC0, 0, 0, 1, 2))
        reply = [x for x in b.connection.messages() if x[0] == 100][-1][2]
        self.assertEqual((reply[3], reply[10:]), (0, b'\xff\xff'))
        port = m.fns[0]                                                  # the 2 ms timer set above
        clock = [1000.0]
        real, chipemu.time.monotonic = chipemu.time.monotonic, lambda: clock[0]
        try:
            self.bulk_in(m, port.inp, 64, ident=29)                      # the first status, at once
            m.handle(101, 30, struct.pack('<BBHI', port.inp, 0, 64, 0))  # then nothing to say: waits
            clock[0] += 0.0019
            m.latency_tick()
            self.assertEqual([r for r in m.connection.messages() if r[0] == 101], [])
            clock[0] += 0.0002                                          # 2.1 ms after the first
            m.latency_tick()
        finally:
            chipemu.time.monotonic = real
        replies = [r for r in self.connection.messages() if r[0] == 101 and r[1] == 30]
        self.assertEqual(replies[-1][2][8:], bytes([0x01, 0x60]))
        self.assertEqual(m.status_packets, 1)
        a = self.model('ft232am')                                        # no latency timer on the AM
        self.assertEqual(self.control(a, 0xC0, 0x0A, 0, 0, 1)[0], 4)
        c = self.model('cp2102')
        status, props = self.control(c, 0xC1, 0x0F, 0, 0, 164)              # before IFC_ENABLE
        self.assertEqual((status, len(props), props[:2]), (0, 66, b'B\x00'))
        self.assertEqual(struct.unpack_from('<I', props, 20)[0], 0x10000000 | 1000000)
        self.assertEqual(self.control(c, 0x41, 0x19, 0, 0, 6, b'\x00\x00\x00\x00\x11\x13')[0], 0)
        self.assertEqual(self.control(c, 0xC1, 0x0E, 0, 0, 6)[1], b'\x00\x00\x00\x00\x11\x13')
        self.assertEqual(self.control(c, 0x41, 0x03, 0x0501, 0)[0], 0)      # 5 bits, 1.5 stop
        self.control(c, 0x41, 0x00, 1, 0)
        status, comm = self.control(c, 0xC0, 0x10, 0, 0, 19)                # to the device
        self.assertEqual((status, len(comm)), (0, 19))
        self.assertEqual(self.control(c, 0x41, 0x06, 0x15A, 0)[0], 4)       # IMM_CHAR: a byte only
        self.control(c, 0x41, 0x07, 0x0200, 0)                           # RTS low: CTS holds
        block = struct.pack('<IIII', 0x09, 0x40, 128, 128)
        self.control(c, 0x41, 0x13, 0, 0, 16, block)                     # CTS handshake, RTS low
        self.control(c, 0x41, 0x07, 0x0200, 0)
        self.bulk_out(c, c.fns[0].out, b'ABC')
        comm = self.control(c, 0xC1, 0x10, 0, 0, 19)[1]
        self.assertEqual(struct.unpack_from('<II', comm, 8), (3, 0))     # one queue: counted as in
        self.assertEqual(self.control(c, 0x41, 0x06, 0x5A, 0)[0], 0)        # IMM_CHAR comes back
        self.control(c, 0x41, 0x07, 0x0202, 0)                           # CTS back
        self.assertEqual(self.bulk_in(c, c.fns[0].inp, 64, ident=31), b'ABC')
        self.assertEqual(self.bulk_in(c, c.fns[0].inp, 64, ident=32), b'Z')
        self.assertEqual(self.control(c, 0x41, 0x03, 0x0801, 0)[0], 0)      # 8 bits, "1.5" stop: 2
        self.assertEqual(self.events('framing')[-1]['stop'], 2)
        one = self.model('cp2101')
        self.assertEqual(self.control(one, 0x41, 0x03, 0x0700, 0)[0], 4)    # the CP2101: 8 bits only


if __name__ == '__main__':
    unittest.main()
