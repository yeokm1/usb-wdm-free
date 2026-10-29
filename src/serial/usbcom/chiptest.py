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
        self.assertEqual(self.control(m, 0x41, 0x1E, 0, 1, 4, struct.pack('<I', 9600))[0], 4)   # before enable
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


if __name__ == '__main__':
    unittest.main()
