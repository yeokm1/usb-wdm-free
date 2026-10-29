#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Test-only SIMULATED USB serial bridges (FTDI, PL2303, CP210x) for usbcom's
chip modules (docs/design/serial.md 6.3), exported to QEMU over localhost
usbredir exactly as acmemu.py exports its CDC-ACM device.

Each part answers the vendor requests the design names, decodes the rate and
framing it was given back into numbers and records them (JSONL), echoes what
it receives with the part's own receive framing (FTDI: two status bytes per
packet), and loops its outputs back like a test plug: DTR -> DSR, RTS -> DCD
and CTS, and a sent break comes back as a received one. The protocol facts
are the ones the design takes from Linux v6.12 (facts only); no code was
taken from anywhere. Nothing here touches a physical device: the strings say
SIMULATED, and the ids are the parts' own only so that USBCOM.INF matches.
"""
import argparse
import collections
import hashlib
import os
import socket
import struct
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import acmemu  # noqa: E402  (the usbredir framing and the Model base)

FT_STATUS0 = 0x01          # FTDI status byte 0: the low nibble reads 1
FT_IDLE1 = 0x60            # FTDI status byte 1: THRE and TEMT
FT_BI = 0x10

# part: (family, vid, pid, bcdUSB, bcdDevice, device class, speed, ports, extra)
PARTS = {
    'ftx':      ('ftdi', 0x0403, 0x6015, 0x0200, 0x1000, 0, 'full', 1, {}),
    'ft232h':   ('ftdi', 0x0403, 0x6014, 0x0200, 0x0900, 0, 'high', 1, {}),
    'ft4232h':  ('ftdi', 0x0403, 0x6011, 0x0200, 0x0800, 0, 'high', 4, {}),
    'pl2303h':  ('pl2303', 0x067B, 0x2303, 0x0110, 0x0202, 0, 'full', 1, {'type': 'H', 'mp0': 8}),
    'pl2303hx': ('pl2303', 0x067B, 0x2303, 0x0110, 0x0300, 0, 'full', 1, {'type': 'HX'}),
    'pl2303hxd': ('pl2303', 0x067B, 0x2303, 0x0110, 0x0400, 0, 'full', 1, {'type': 'HXD'}),
    'pl2303ta': ('pl2303', 0x067B, 0x2303, 0x0200, 0x0300, 0, 'full', 1, {'type': 'TA'}),
    'pl2303tb': ('pl2303', 0x067B, 0x2304, 0x0200, 0x0500, 0, 'full', 1, {'type': 'TB'}),
    'pl2303g':  ('pl2303', 0x067B, 0x23A3, 0x0200, 0x0100, 0, 'full', 1, {'type': 'G'}),
    'cp2105':   ('cp210x', 0x10C4, 0xEA70, 0x0200, 0x0100, 0, 'full', 2, {'partnum': 5}),
    'cp2108':   ('cp210x', 0x10C4, 0xEA71, 0x0200, 0x0100, 0, 'full', 4, {'partnum': 8}),
}


def ftdi_rate(div, h_part):
    """The rate an FTDI divisor selects: bits 0-13 the whole divisor, bits
    14-16 the eighths as a sub-divisor code, bit 17 the H parts' 12 MHz
    clock (else 3 MHz); 0 and 1 are the clock and two thirds of it."""
    base = 12000000 if h_part and div & 0x20000 else 3000000
    d = div & 0x1FFFF
    if d == 0:
        return base
    if d == 1:
        return base * 2 // 3
    eighths = {0: 0, 3: 1, 2: 2, 4: 3, 1: 4, 5: 5, 6: 6, 7: 7}[(d >> 14) & 7]
    return round(base * 8 / ((d & 0x3FFF) * 8 + eighths))


PL_DIRECT = (75, 150, 300, 600, 1200, 1800, 2400, 3600, 4800, 7200, 9600, 14400, 19200,
             28800, 38400, 57600, 115200, 230400, 460800, 614400, 921600, 1228800,
             2457600, 3000000, 6000000)


def pl2303_rate(b, alt):
    if not b[3] & 0x80:
        return struct.unpack('<I', b[:4])[0]
    if alt:
        mantissa = b[0] | (b[1] & 0x07) << 8
        exponent = ((b[1] >> 4) & 0x0E) | (b[2] & 1)
        return (384000000 // mantissa) >> exponent
    mantissa = b[0] | (b[1] & 0x01) << 8
    exponent = (b[1] >> 1) & 0x07
    return (384000000 // mantissa) >> (2 * exponent)


class Port:
    """One port of the part: its interface, endpoints and line state."""

    def __init__(self, index, family, high):
        self.index = index
        self.comm = self.data = index           # interface number
        if family == 'ftdi':
            self.inp, self.out, self.intr = 0x81 + 2 * index, 0x02 + 2 * index, None
        elif family == 'pl2303':
            self.inp, self.out, self.intr = 0x83, 0x02, 0x81
        else:
            self.inp, self.out, self.intr = 0x81 + index, 0x01 + index, None
        self.lines = 0                          # bit 0 DTR, bit 1 RTS
        self.brk = False
        self.pending_break = False
        self.flow = False
        self.enabled = False                    # CP210x IFC_ENABLE
        self.rts_flow = False                   # CP210x RTS in flow-control mode
        self.rate = 0
        self.framing = None
        self.status_due = True                  # FTDI: send a status packet
        self.echo = collections.deque()
        self.queued = 0
        self.interrupt = False
        self.notify_q = collections.deque()
        self.told_state = False
        self.last_state, self.sent_period = 0, False
        self.flow_block = bytearray(16)
        struct.pack_into('<IIII', self.flow_block, 0, 0x01, 0x40, 128, 128)

    def clear(self):
        self.notify_q.clear()
        self.told_state = False
        self.echo.clear()
        self.queued = 0
        self.status_due = True

    @property
    def dtr(self):
        return bool(self.lines & 1)

    @property
    def rts(self):
        return bool(self.lines & 2)


class ChipModel(acmemu.Model):
    def __init__(self, connection, trace, part, caps=acmemu.CAPS, speed=None):
        (self.family, self.vid, self.pid, self.bcd_usb, self.bcd_device, self.dev_class,
         native, nports, extra) = PARTS[part]
        # --speed high presents a Full Speed part at High Speed: QEMU's EHCI
        # has no companion or transaction translator for Full Speed devices,
        # so the EHCI legs (NUSB) need it. A simulation artifact: the real
        # parts are Full Speed.
        speed = speed or native
        self.part, self.extra = part, extra
        super().__init__(connection, trace, speed=speed, profile='uno', mirror=True, caps=caps)
        self.bulk_max = 512 if self.high else 64
        self.fns = [Port(k, self.family, self.high) for k in range(nports)]
        self.interfaces = nports
        self.device = self.device_desc()
        self.config_desc = self.config_block(self.bulk_max, 2)
        self.other = self.config_block(64 if self.high else 512, 7)
        self.regs = collections.defaultdict(int)  # PL2303 vendor registers
        self.pl_seq = []                          # PL2303 vendor reads and writes, in order
        self.pl_started = False                   # the start sequence ran (not G)

    # ---- descriptors -----------------------------------------------------
    def device_desc(self):
        return struct.pack('<BBHBBBBHHHBBBB', 18, 1, self.bcd_usb, self.dev_class, 0, 0,
                           self.extra.get('mp0', 64),
                           self.vid, self.pid, self.bcd_device, 1, 2, 0, 1)

    def config_block(self, bulk, kind):
        body = bytearray()
        for f in self.fns:
            neps = 3 if f.intr else 2
            body += bytes([9, 4, f.comm, 0, neps, 0xFF, 0xFF, 0xFF, 2])
            if f.intr:
                body += bytes([7, 5, f.intr, 3, 10, 0, 1])
            body += bytes([7, 5, f.out, 2, bulk & 255, bulk >> 8, 0])
            body += bytes([7, 5, f.inp, 2, bulk & 255, bulk >> 8, 0])
        total = 9 + len(body)
        return bytes([9, kind, total & 255, total >> 8, self.interfaces, 1, 0, 0x80, 50]) + bytes(body)

    def strings(self, number):
        text = {1: 'usb-wdm-free simulation', 2: 'SIMULATED %s (usbcom test)' % self.part.upper()}
        if number == 0:
            return b'\x04\x03\x09\x04'
        return acmemu.string(text[number]) if number in text else None

    def info(self):
        types, intervals, interfaces = bytearray([255] * 32), bytearray(32), bytearray(32)
        packets = [0] * 32
        rows = [(0, 0, 0, 0, 64), (16, 0, 0, 0, 64)]
        for f in self.fns:
            rows += [(f.out & 15, 2, 0, f.comm, self.bulk_max), (16 + (f.inp & 15), 2, 0, f.comm, self.bulk_max)]
            if f.intr:
                rows += [(16 + (f.intr & 15), 3, 1, f.comm, 10)]
        for index, kind, interval, interface, packet in rows:
            types[index], intervals[index], interfaces[index], packets[index] = kind, interval, interface, packet
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << 4):
            data += struct.pack('<32H', *packets)
        self.send(5, data=data)
        numbers, classes, subclasses, protocols = bytearray(32), bytearray(32), bytearray(32), bytearray(32)
        for f in self.fns:
            numbers[f.comm], classes[f.comm], subclasses[f.comm], protocols[f.comm] = f.comm, 0xFF, 0xFF, 0xFF
        self.send(4, data=struct.pack('<I', self.interfaces) + numbers + classes + subclasses + protocols)

    # ---- line state and status -------------------------------------------
    def lines_changed(self, f):
        self.log('line_state', dtr=int(f.dtr), rts=int(f.rts), function=f.index)
        f.status_due = True
        if self.family == 'pl2303':
            self.notification(f)
        self.drain_fn(f)

    def uart_state(self, f):
        """PL2303 UART state byte: DCD 0x01, DSR 0x02, break 0x04, CTS 0x80."""
        state = (0x02 if f.dtr else 0) | (0x01 | 0x80 if f.rts else 0)
        if f.pending_break:
            state |= 0x04
            f.pending_break = False
        return state

    def notification(self, f=None):
        f = f or self.fns[0]
        if len(f.notify_q) < 16:
            f.notify_q.append(self.uart_state(f))
        self.flush_notifications(f)

    def flush_notifications(self, f=None):
        f = f or self.fns[0]
        while f.interrupt and f.notify_q:
            state = f.notify_q.popleft()
            message = bytes([0xA1, 0x20, 0, 0, 0, 0, 2, 0, state, 0])
            self.send(103, data=struct.pack('<BBH', f.intr, 0, len(message)) + message)
            f.told_state = True
            f.last_state, f.sent_period = state, True
            self.log('serial_state', state=state, function=f.index)

    def mdmsts(self, f):
        """CP210x GET_MDMSTS: DTR 0x01, RTS 0x02, CTS 0x10, DSR 0x20, DCD 0x80."""
        return (0x01 if f.dtr else 0) | (0x02 if f.rts else 0) | \
            (0x20 if f.dtr else 0) | (0x10 | 0x80 if f.rts else 0)

    def ftdi_status(self, f):
        s0 = FT_STATUS0 | (0x20 if f.dtr else 0) | (0x10 | 0x80 if f.rts else 0)
        return s0

    # ---- bulk IN with the part's framing ---------------------------------
    def drain_fn(self, f):
        while True:
            oldest = next(((i, v) for i, v in self.pending.items() if v[0] == f.inp), None)
            if oldest is None:
                break
            ident, (endpoint, length) = oldest
            if not length:
                del self.pending[ident]
                self.send(101, ident, self.bulk_header(endpoint, 0, 0))
                self.log('bulk_in_zero', ident=ident, pending=len(self.pending))
                continue
            if self.family == 'ftdi':
                data = self.ftdi_frame(f, length)
                if data is None:
                    break
            else:
                if not f.echo or (f.flow and not f.rts):
                    break                                 # CTS handshake: the plug holds it
                frame = f.echo.popleft()
                data = frame[:length]
                f.queued -= len(data)
                if len(frame) > len(data):
                    f.echo.appendleft(frame[len(data):])
                elif data and len(data) % self.bulk_max == 0:
                    f.echo.appendleft(b'')
            del self.pending[ident]
            self.send(101, ident, self.bulk_header(endpoint, 0, len(data)) + data)
            self.log('bulk_in', ident=ident, requested=length, length=len(data),
                     sha256=hashlib.sha256(data).hexdigest(), function=f.index)

    def ftdi_frame(self, f, length):
        """Packets of two status bytes and up to wMaxPacketSize - 2 bytes of
        data, as many as fit the request; None if there is nothing to send."""
        held = f.flow and not f.rts
        if (not f.echo or held) and not f.status_due and not f.pending_break:
            return None
        out = bytearray()
        s0 = self.ftdi_status(f)
        while len(out) + 2 <= length:
            room = min(self.bulk_max, length - len(out)) - 2
            if f.pending_break:
                out += bytes([s0, FT_IDLE1 | FT_BI, 0])   # the break's NUL
                f.pending_break = False
                break
            chunk = bytearray()
            while f.echo and len(chunk) < room and not (f.flow and not f.rts):
                frame = f.echo.popleft()
                take = frame[:room - len(chunk)]
                chunk += take
                f.queued -= len(take)
                if len(frame) > len(take):
                    f.echo.appendleft(frame[len(take):])
            if not chunk and out:
                break
            out += bytes([s0, FT_IDLE1]) + chunk
            if len(chunk) < room:
                break                                     # a short packet ends it
        f.status_due = len(out) % self.bulk_max == 0      # end on a short packet next
        return bytes(out)

    # ---- control ---------------------------------------------------------
    def control(self, ident, packet):
        if len(packet) < 10:
            raise ValueError('short control header')
        endpoint, request, reqtype, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        payload, result, status = packet[10:], b'', 0
        if not reqtype & 0x80 and len(payload) != length:
            raise ValueError('control OUT length mismatch')
        kick = None
        if reqtype & 0x60 == 0:
            if request == 6 and reqtype == 0x80:
                dtype, number = value >> 8, value & 255
                result = self.device if dtype == 1 and number == 0 else \
                    self.config_desc if dtype == 2 and number == 0 else \
                    self.strings(number) if dtype == 3 else \
                    bytes([10, 6, 0, 2, 0, 0, 0, 64, 1, 0]) if dtype == 6 and self.high else \
                    self.other if dtype == 7 and number == 0 and self.high else None
                if result is None:
                    result, status = b'', 4
            elif request == 0:
                result = b'\x00\x00'
            elif request in (1, 3):
                if reqtype & 31 != 2 or value != 0:
                    status = 4
            elif request == 8:
                result = bytes((self.config,))
            elif request == 10:
                result = b'\x00'
            else:
                status = 4
        elif self.config != 1:
            status = 4
        else:
            handler = getattr(self, 'control_' + self.family)
            status, result, kick = handler(request, reqtype, value, index, length, payload)
        result = result[:length]
        actual = len(result) if reqtype & 0x80 else (0 if status else length)
        header = struct.pack('<BBBBHHH', endpoint, request, reqtype, status, value, index, actual)
        self.send(100, ident, header + result)
        self.log('control', request=request, request_type=reqtype, value=value, index=index,
                 requested=length, actual=actual, status=status)
        if kick is not None:
            self.lines_changed(kick)

    def control_ftdi(self, request, reqtype, value, index, length, payload):
        h_part = self.bcd_device in (0x0700, 0x0800, 0x0900)
        channel = index & 0xFF
        f = self.fns[channel - 1] if 1 <= channel <= len(self.fns) else None
        if reqtype != 0x40 and not (reqtype == 0xC0 and request in (5, 0x0A)):
            return 4, b'', None
        if f is None:
            self.log('bad_channel', request=request, index=index)
            return 4, b'', None
        if request == 0:
            # RESET: 0 resets the port (its buffers go, flow control off),
            # 1 purges what waits for the host, 2 what waits for the UART.
            self.log('ftdi_reset', value=value, function=f.index, dropped=f.queued)
            if value in (0, 1):
                f.echo.clear()
                f.queued = 0
            if value == 0:                                # DTR and RTS low too (ftdi_sio.h)
                f.flow = False
                f.status_due = True
                if f.lines:
                    f.lines = 0
                    return 0, b'', f
            return 0, b'', None
        if request == 1:
            mask, bits = value >> 8, value & 0xFF
            if f.flow:                                    # RTS is the handshake's: ignored
                if mask & 2:
                    self.log('rts_ignored', function=f.index)
                mask &= ~2
            f.lines = (f.lines & ~mask) | (bits & mask)
            return 0, b'', f
        if request == 2:
            f.flow = bool(index & 0x0100)
            self.log('flow', rtscts=int(f.flow), function=f.index)
            return 0, b'', None
        if request == 3:
            div = value | ((index >> 8) & 0xFF) << 16
            f.rate = ftdi_rate(div, h_part)
            self.log('rate', rate=f.rate, divisor=div, function=f.index)
            return 0, b'', None
        if request == 4:
            bits, parity, stop, brk = value & 0xFF, (value >> 8) & 7, (value >> 11) & 3, bool(value & 0x4000)
            if bits not in (7, 8) or parity > 4 or stop not in (0, 2):
                return 4, b'', None
            f.framing = (bits, parity, stop)
            self.log('framing', bits=bits, parity=parity, stop=stop, brk=int(brk), function=f.index)
            if brk and not f.brk:
                f.pending_break = True
            f.brk = brk
            if brk:
                return 0, b'', f
            return 0, b'', None
        if request == 5:
            return 0, bytes([self.ftdi_status(f), FT_IDLE1]), None
        if request in (9, 0x0A):
            return 0, b'\x10' if request == 0x0A else b'', None
        return 4, b'', None

    def control_pl2303(self, request, reqtype, value, index, length, payload):
        kind = self.extra['type']
        f = self.fns[0]
        if reqtype == 0xC0 and request in (0x01, 0x81):
            if (request == 0x81) != (kind == 'G'):
                return 4, b'', None
            self.pl_seq.append(('r', value))
            if value == 0x8080 and kind not in ('TA', 'TB'):
                return 4, b'', None
            reg = value & 0x7F if request == 0x01 else value
            return 0, bytes([self.regs[reg] & 0xFF]), None
        if reqtype == 0x40 and request in (0x01, 0x80):
            if (request == 0x80) != (kind == 'G'):
                return 4, b'', None
            self.regs[value] = index
            self.log('vendor_write', reg=value, value=index)
            self.pl_seq.append(('w', value, index))
            if kind != 'G' and value == 2:
                # The start sequence, as the design lists it (Linux
                # pl2303_startup), must have come just before this write.
                want = [('r', 0x8484), ('w', 0x0404, 0), ('r', 0x8484), ('r', 0x8383),
                        ('r', 0x8484), ('w', 0x0404, 1), ('r', 0x8484), ('r', 0x8383),
                        ('w', 0, 1), ('w', 1, 0), ('w', 2, 0x24 if kind == 'H' else 0x44)]
                got = self.pl_seq[-len(want):]
                self.pl_started = self.pl_started or got == want
                self.log('startup', ok=int(got == want), got=[list(x) for x in got])
            if (kind != 'G' and value in (8, 9)) or (kind == 'G' and value == 7):
                f.echo.clear()                            # the pipe resets of an open
                f.queued = 0
            if (kind == 'G' and value == 0x0A) or (kind != 'G' and value == 0):
                mask, rts = (0x1C, 0x18) if kind == 'G' else (0xF0, 0x40 if kind == 'H' else 0x60)
                f.flow = (index & mask) == rts
                self.log('flow', rtscts=int(f.flow), function=0)
            return 0, b'', None
        if reqtype in (0x21, 0xA1) and index != 0:
            self.log('bad_index', request=request, index=index)
            return 4, b'', None
        if reqtype == 0x21 and request == 0x20 and length == 7:
            if kind != 'G' and not self.pl_started:
                # The start sequence must have run first (the driver's
                # initialise, design 6.3); the model refuses until it has.
                self.log('not_started', request=request)
                return 4, b'', None
            if kind == 'G' and payload[3] & 0x80:
                return 4, b'', None                       # the G series takes no divisor
            baud = pl2303_rate(payload, kind in ('TA', 'TB'))
            if not payload[3] & 0x80 and kind != 'G' and baud not in PL_DIRECT:
                self.log('rate_not_direct', requested=baud)
                baud = 9600                               # Linux: an unlisted direct rate runs at 9600
            stop, parity, bits = payload[4], payload[5], payload[6]
            if stop > 2 or parity > 4 or bits not in (5, 6, 7, 8):
                return 4, b'', None
            f.rate, f.framing, self.line = baud, (bits, parity, stop), payload
            self.log('rate', rate=baud, raw=payload[:4].hex(), function=0)
            self.log('framing', bits=bits, parity=parity, stop=stop, function=0)
            return 0, b'', None
        if reqtype == 0xA1 and request == 0x21:
            return 0, self.line, None
        if reqtype == 0x21 and request == 0x22 and length == 0:
            f.lines = value & 3
            return 0, b'', f
        if reqtype == 0x21 and request == 0x23 and length == 0:
            self.log('break', duration=value, function=0)
            if value:
                f.pending_break = True
                self.notification(f)
            return 0, b'', None
        return 4, b'', None

    def control_cp210x(self, request, reqtype, value, index, length, payload):
        if reqtype == 0xC0 and request == 0xFF:
            if value == 0x370B:
                return 0, bytes([self.extra['partnum']]), None
            if value == 0x000E:
                return 0, b'\x01\x00\x00', None
            return 4, b'', None
        if reqtype not in (0x41, 0xC1) or index >= len(self.fns):
            return 4, b'', None
        # Each request has one direction; the wrong one stalls.
        if (reqtype == 0xC1) != (request in (0x04, 0x08, 0x14, 0x1D)):
            return 4, b'', None
        f = self.fns[index]
        if request == 0x00:
            f.enabled = bool(value & 1)
            self.log('ifc_enable', value=value, function=f.index)
            if not f.enabled:
                f.echo.clear()
                f.queued = 0
            return 0, b'', None
        if not f.enabled and request != 0x12:
            self.log('not_enabled', request=request, function=f.index)
            return 4, b'', None
        if request == 0x1E and length == 4:
            f.rate = struct.unpack('<I', payload)[0]
            self.log('rate', rate=f.rate, function=f.index)
            return 0, b'', None
        if request == 0x1D:
            return 0, struct.pack('<I', f.rate), None
        if request == 0x03:
            bits, parity, stop = (value >> 8) & 0x0F, (value >> 4) & 0x0F, value & 0x0F
            if bits not in (5, 6, 7, 8) or parity > 4 or stop not in (0, 2):
                return 4, b'', None
            if self.extra['partnum'] == 5 and f.index == 1 and (bits != 8 or parity > 2 or stop != 0):
                return 4, b'', None                       # the CP2105's second interface
            f.framing = (bits, parity, stop)
            self.log('framing', bits=bits, parity=parity, stop=stop, function=f.index)
            return 0, b'', None
        if request == 0x05:
            self.log('break', duration=value, function=f.index)
            if value and self.extra['partnum'] == 5 and f.index == 1:
                return 4, b'', None
            return 0, b'', None
        if request == 0x07:
            lines = f.lines
            if value & 0x0100:
                lines = (lines & ~1) | (value & 1)
            if value & 0x0200:
                if f.rts_flow:                            # RTS is the handshake's: SET_FLOW only
                    self.log('rts_ignored', function=f.index)
                else:
                    lines = (lines & ~2) | (value & 2)
            f.lines = lines
            return 0, b'', f
        if request == 0x08:
            return 0, bytes([self.mdmsts(f)]), None
        if request == 0x14:
            return 0, bytes(f.flow_block), None
        if request == 0x13 and length == 16:
            f.flow_block[:] = payload
            ctl_hs, repl = struct.unpack_from('<II', payload)
            dtr = 1 if ctl_hs & 3 == 1 else 0
            rts = 2 if (repl >> 6) & 3 in (1, 2) else 0
            f.rts_flow = (repl >> 6) & 3 == 2
            f.flow = bool(ctl_hs & 0x08)
            self.log('flow', rtscts=int(f.flow), control=ctl_hs, replace=repl, function=f.index)
            f.lines = dtr | rts
            return 0, b'', f
        if request == 0x12:
            self.log('purge', value=value, function=f.index, dropped=f.queued)
            f.echo.clear()
            f.queued = 0
            return 0, b'', None
        return 4, b'', None

    # ---- usbredir --------------------------------------------------------
    def handle(self, kind, ident, packet):
        """A CP210x interface not enabled takes no data (its bulk OUT data
        is dropped and logged), so the driver's open order shows. A reset or
        a configuration starts a PL2303's audit afresh."""
        if kind in (3, 6):
            self.pl_started = False
            self.pl_seq = []
        if kind == 101 and self.family == 'cp210x' and len(packet) >= 8:
            endpoint = packet[0]
            f = self.fn_by('out', endpoint)
            if f is not None and not f.enabled:
                size = 10 if self.len32 else 8
                length = struct.unpack('<H', packet[2:4])[0]
                if self.len32:
                    length |= struct.unpack('<H', packet[8:10])[0] << 16
                self.send(101, ident, self.bulk_header(endpoint, 0, length))
                self.log('dropped_disabled', length=len(packet) - size, function=f.index)
                return
        super().handle(kind, ident, packet)

    def run(self):
        self.send(0, data=b'usb-wdm-free chip simulation 0.1'.ljust(64, b'\x00') + struct.pack('<I', self.caps))
        kind, length, ident = struct.unpack('<III', self.exact(12))
        if kind != 0 or ident or length < 64 or length > 4096:
            raise ValueError('invalid hello')
        hello = self.exact(length)
        self.peer_caps = struct.unpack('<I', hello[64:68])[0] if length >= 68 else 0
        self.log('hello', peer=hello[:64].split(b'\x00')[0].decode('ascii', 'replace'),
                 capabilities=self.peer_caps)
        self.info()
        connect = struct.pack('<BBBBHH', 2 if self.high else 1, self.dev_class, 0, 0, self.vid, self.pid)
        if self.peer_caps & (1 << 1):
            connect += struct.pack('<H', self.bcd_device)
        self.send(1, data=connect)
        self.log('connected', model='SIMULATED ' + self.part, vid='%04X' % self.vid, pid='%04X' % self.pid,
                 bcd_device='%04X' % self.bcd_device, speed=self.speed, ports=len(self.fns))
        while True:
            if self.ids64:
                kind, length, ident = struct.unpack('<IIQ', self.exact(16))
            else:
                kind, length, ident = struct.unpack('<III', self.exact(12))
            if length > 1048586:
                raise ValueError('message exceeds test bound')
            self.handle(kind, ident, self.exact(length))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--log', type=Path, required=True)
    ap.add_argument('--part', choices=sorted(PARTS), required=True)
    ap.add_argument('--speed', choices=('full', 'high'),
                    help="override the part's own speed (EHCI legs: high; an artifact)")
    args = ap.parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'{args.part} simulation listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            model = ChipModel(connection, trace, args.part, speed=args.speed)
            try:
                model.run()
            except (EOFError, ConnectionResetError, BrokenPipeError):
                model.log('disconnected')


if __name__ == '__main__':
    main()
