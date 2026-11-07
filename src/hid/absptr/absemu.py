#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Test-only absolute pointing device simulator for absptr, exported to QEMU
over localhost usbredir (docs/design/absptr.md, test plan).

Profiles (each a HID class device, interface 03/00/00 unless said; the
descriptors are this file's own, written from the HID 1.11 specification,
the HID Usage Tables 1.12 and Microsoft's published digitizer guidance,
shaped like the device families named; strings say SIMULATED):

  tablet      a KVM-style absolute mouse, QEMU usb-tablet's shape: Generic
              Desktop Mouse/Pointer, 3 buttons, X/Y 0..32767 in 16 bits,
              a relative wheel (FFFF:0030)
  kvm16       the same with X/Y Logical 0..65535 in 16 bits (FFFF:0031)
  kvm12       the same with X/Y Logical 0..4095 in packed 12-bit fields
              (FFFF:0032)
  pointer     the tablet with a top-level usage of Pointer, not Mouse
              (FFFF:0033)
  touch       a single-touch HID touch screen: Digitizer page, Touch Screen,
              one Finger with Tip Switch and In Range, X/Y 0..4095
              (FFFF:0034)
  touchmouse  a touch screen with two top-level collections, as many
              controllers have for older systems: report 1 the Digitizer
              Touch Screen, report 2 an absolute Generic Desktop Mouse;
              each event is sent on both (FFFF:0035)
  mtouch      a two-contact multi-touch screen in Windows 7's form: Contact
              Identifier, Tip Switch, X/Y per contact, Contact Count, a
              Contact Count Maximum feature report (FFFF:0036)
  pen         a pen digitizer: Digitizer page Pen, In Range, Tip Switch,
              Barrel Switch, Eraser, Invert, X 0..20000, Y 0..15000, Tip
              Pressure 0..1023 (FFFF:0037)
  penpartner  the Wacom PenPartner as QEMU's usb-wacom-tablet models it
              (056A:0000, interface 03/01/02, QEMU's report descriptor; in
              its first mode report 1 as that descriptor declares it, a
              relative mouse the pen moves (INFERRED; QEMU's model sent four
              bytes without the report ID, which no HID parser takes, and
              no movement); a SET_REPORT of feature report 2 with value 2
              switches to the tablet mode (a GET_REPORT returns the current
              input report, as QEMU's model does), whose reports are 02, X
              (le16, 0..5040), Y (le16, 0..3780), status (0x20 eraser, 0x40
              side switch), pressure (signed, -127 = none), and a 00 to the
              descriptor's length), the protocol Linux's
              wacom_penpartner_irq reads (drivers/hid/wacom_wac.c)

Input is driven over a second TCP port (--ctl), one JSON object a line:
  {"x": 0.25, "y": 0.75}            move (fractions of the full range)
  {"tip": 1} / {"tip": 0}           touch / pen tip, or the left button
  {"buttons": 5}                    button bits (1 left, 2 right, 4 middle)
  {"range": 0}                      pen or finger out of range
  {"pressure": 0.5}                 pen tip pressure
  {"barrel": 1} {"eraser": 1}       pen switches
  {"quit": 1}                       end the simulation (the test host's unplug)
Each command sends one report with the new state. The answer is a JSON
line: {"ok": true, "sent": n}.

Real devices of these families are Full Speed; --speed high presents the
same device at High Speed so that a guest's EHCI driver carries it (a test
shape, not a real device's). No physical USB device is accessed. The
usbredir framing follows this repository's acmemu.py and xpademu.py.
"""
import argparse
import json
import select
import socket
import struct
import time
from pathlib import Path

CAP_64BIT_IDS = 1 << 5
CAP_32BIT_BULK = 1 << 6
CAPS = (1 << 1) | (1 << 4) | CAP_64BIT_IDS | CAP_32BIT_BULK

HELLO, CONNECT, RESET, IFACE_INFO, EP_INFO = 0, 1, 3, 4, 5
SET_CONFIG, GET_CONFIG, CONFIG_STATUS = 6, 7, 8
SET_ALT, GET_ALT, ALT_STATUS = 9, 10, 11
START_INT, STOP_INT, INT_STATUS = 15, 16, 17
CANCEL, CONTROL, BULK, INTERRUPT = 21, 100, 101, 103


def h(text):
    return bytes.fromhex(text.replace(' ', ''))


# ---------------------------------------------------------------- descriptors

MOUSE_BODY = ('09 01 a1 00 05 09 19 01 29 03 15 00 25 01 95 03 75 01 81 02 95 01 75 05 81 01 '
              '05 01 09 30 09 31 {xy} 81 02 '
              '09 38 15 81 25 7f 35 00 45 00 75 08 95 01 81 06 c0')
XY15 = '15 00 26 ff 7f 35 00 46 ff 7f 75 10 95 02'
XY16 = '15 00 27 ff ff 00 00 35 00 47 ff ff 00 00 75 10 95 02'
XY12 = '15 00 26 ff 0f 35 00 46 ff 0f 75 0c 95 02'

TABLET = h('05 01 09 02 a1 01 ' + MOUSE_BODY.format(xy=XY15) + ' c0')
KVM16 = h('05 01 09 02 a1 01 ' + MOUSE_BODY.format(xy=XY16) + ' c0')
KVM12 = h('05 01 09 02 a1 01 ' + MOUSE_BODY.format(xy=XY12) + ' c0')
POINTER = h('05 01 09 01 a1 01 ' + MOUSE_BODY.format(xy=XY15) + ' c0')

# X and Y of a finger or contact carry a physical size (Unit: SI linear
# length, centimetres; Unit Exponent -2: Physical 0..4095 is 0..40.95 cm;
# HID 1.11 6.2.2.7), as Microsoft's digitizer guidance asks of X and Y.
# Windows 10 takes the touch without them (the inbox reference runs),
# and Windows 7 x86 with or without them once the restart its touch
# support asks for is done (docs/testing/2026-11-06-absptr.md,
# 2026-11-07). X and Y share one range,
# so the surface is 40.95 cm square at 100 counts a centimetre. The global
# Unit, Unit Exponent and Physical extent are reset after the Input item,
# so no later item (Tip Switch of the next contact, Contact Count, the
# mouse collection) inherits a size.
UNITS = '65 11 55 0e'
NO_UNITS = '65 00 55 00 35 00 45 00'
# One finger: Tip Switch, In Range, 6 bits pad, X, Y (16-bit, 0..4095).
FINGER = ('09 22 a1 02 09 42 09 32 15 00 25 01 75 01 95 02 81 02 95 06 81 03 '
          '05 01 09 30 09 31 15 00 26 ff 0f 35 00 46 ff 0f {units} 75 10 95 02 81 02 {nounits} 05 0d c0'.format(units=UNITS, nounits=NO_UNITS))
TOUCH = h('05 0d 09 04 a1 01 ' + FINGER + ' c0')
TOUCHMOUSE = h('05 0d 09 04 a1 01 85 01 ' + FINGER + ' c0 '
               '05 01 09 02 a1 01 85 02 ' + MOUSE_BODY.format(xy=XY15) + ' c0')
# Two contacts (Tip, 7 pad, Contact Id 8 bits, X, Y), Contact Count, then the
# Contact Count Maximum feature (report 2).
CONTACT = ('09 22 a1 02 09 42 15 00 25 01 75 01 95 01 81 02 95 07 81 03 '
           '09 51 25 0f 75 08 95 01 81 02 '
           '05 01 09 30 09 31 15 00 26 ff 0f 35 00 46 ff 0f {units} 75 10 95 02 81 02 {nounits} 05 0d c0'.format(units=UNITS, nounits=NO_UNITS))
MTOUCH = h('05 0d 09 04 a1 01 85 01 ' + CONTACT + ' ' + CONTACT +
           ' 09 54 15 00 25 02 75 08 95 01 81 02 '
           '85 02 09 55 25 02 75 08 95 01 b1 02 c0')
PEN = h('05 0d 09 02 a1 01 09 20 a1 00 '
        '09 42 09 44 09 45 09 3c 09 32 15 00 25 01 75 01 95 05 81 02 95 03 81 03 '
        '05 01 09 30 15 00 26 20 4e 35 00 46 20 4e 75 10 95 01 81 02 '
        '09 31 26 98 3a 46 98 3a 81 02 '
        '05 0d 09 30 26 ff 03 75 10 95 01 81 02 c0 c0')
# The PENPARTNER descriptor below is QEMU's (hw/usb/dev-wacom.c, QEMU v11.1.0),
# carried as data, under that file's notice:
#   Copyright (c) 2006 Openedhand Ltd.
#   Author: Andrzej Zaborowski <balrog@zabor.org>
#   Based on hw/usb-hid.c: Copyright (c) 2005 Fabrice Bellard
#   Permission is hereby granted, free of charge, to any person obtaining a copy
#   of this software and associated documentation files (the "Software"), to deal
#   in the Software without restriction, including without limitation the rights
#   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
#   copies of the Software, and to permit persons to whom the Software is
#   furnished to do so, subject to the following conditions:
#   The above copyright notice and this permission notice shall be included in
#   all copies or substantial portions of the Software.
#   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
#   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
#   THE SOFTWARE.
# qemu_wacom_hid_report_descriptor, byte for byte, so the profile presents
# what QEMU's model presents.
PENPARTNER = h('05 01 09 02 a1 01 85 01 09 01 a1 00 05 09 19 01 29 03 15 00 25 01 95 03 75 01 '
               '81 02 95 01 75 05 81 01 05 01 09 30 09 31 09 38 15 81 25 7f 75 08 95 03 81 06 '
               '95 03 81 01 c0 c0 05 0d 09 01 a1 01 85 02 a1 00 06 00 ff 09 01 15 00 26 ff 00 '
               '75 08 95 07 81 02 c0 09 01 85 63 95 07 81 02 09 01 85 02 95 01 b1 02 09 01 85 03 '
               '95 01 b1 02 c0')

PP_MOUSE_DIV = 4                          # INFERRED: PenPartner mouse mode, tablet units per count

PROFILES = {
    'tablet': dict(pid=0x0030, desc=TABLET, packet=8, product='Absolute mouse (KVM shape)'),
    'kvm16': dict(pid=0x0031, desc=KVM16, packet=8, product='Absolute mouse, 0..65535'),
    'kvm12': dict(pid=0x0032, desc=KVM12, packet=8, product='Absolute mouse, 12-bit'),
    'pointer': dict(pid=0x0033, desc=POINTER, packet=8, product='Absolute pointer (Pointer usage)'),
    'touch': dict(pid=0x0034, desc=TOUCH, packet=8, product='HID touch screen'),
    'touchmouse': dict(pid=0x0035, desc=TOUCHMOUSE, packet=8, product='HID touch screen with mouse'),
    'mtouch': dict(pid=0x0036, desc=MTOUCH, packet=16, product='HID multi-touch screen'),
    'pen': dict(pid=0x0037, desc=PEN, packet=16, product='HID pen digitizer'),
    'penpartner': dict(vid=0x056A, pid=0x0000, bcd=0x4210, desc=PENPARTNER, packet=8, sub=1, proto=2,
                       product='Wacom PenPartner (QEMU model shape)'),
}


def device_descriptor(p):
    # endpoint 0: 64 bytes at High Speed (USB 2.0 5.5.3), 8 at Full Speed
    return struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x0200 if p.get('high') else 0x0110, 0, 0, 0,
                       64 if p.get('high') else 8,
                       p.get('vid', 0xFFFF), p['pid'], p.get('bcd', 0x0100), 1, 2, 3, 1)


def hid_descriptor(p):
    d = p['desc']
    return bytes([9, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, len(d) & 255, len(d) >> 8])


def config_descriptor(p, high, kind=2):
    interval = 4 if high else 10        # 1 ms microframe exponent 4 = 1 ms; FS 10 ms
    body = bytes([9, 4, 0, 0, 1, 3, p.get('sub', 0), p.get('proto', 0), 0]) + hid_descriptor(p)
    body += struct.pack('<BBBBHB', 7, 5, 0x81, 3, p['packet'], interval)
    total = 9 + len(body)
    return struct.pack('<BBHBBBBB', 9, kind, total, 1, 1, 0, 0xA0, 50) + body


def string(text):
    data = text.encode('utf-16le')
    return bytes((len(data) + 2, 3)) + data


# ---------------------------------------------------------------- reports


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def scale(f, top):
    return clamp(int(round(f * top)), 0, top)


def mouse_report(s, xy):
    b = (s['buttons'] | (1 if s['tip'] else 0)) & 7
    if xy == 'xy12':
        x, y = scale(s['x'], 4095), scale(s['y'], 4095)
        packed = x | (y << 12)
        return bytes([b]) + packed.to_bytes(3, 'little') + b'\0'
    top = 65535 if xy == 'xy16' else 32767
    return struct.pack('<BHHb', b, scale(s['x'], top), scale(s['y'], top), 0)


def finger(s):
    bits = (1 if s['tip'] else 0) | (2 if s['range'] else 0)
    return struct.pack('<BHH', bits, scale(s['x'], 4095), scale(s['y'], 4095))


def reports(profile, s, state, advance=True):
    """The interrupt IN packets for one state (advance False: a GET_REPORT,
    which reports the state without using up the PenPartner's motion)."""
    if profile in ('tablet', 'pointer'):
        return [mouse_report(s, 'xy15')]
    if profile == 'kvm16':
        return [mouse_report(s, 'xy16')]
    if profile == 'kvm12':
        return [mouse_report(s, 'xy12')]
    if profile == 'touch':
        return [finger(s)]
    if profile == 'touchmouse':
        return [b'\x01' + finger(s), b'\x02' + mouse_report(s, 'xy15')]
    if profile == 'mtouch':
        c1 = struct.pack('<BBHH', 1 if s['tip'] else 0, 0, scale(s['x'], 4095), scale(s['y'], 4095))
        c2 = struct.pack('<BBHH', 0, 1, 0, 0)
        return [b'\x01' + c1 + c2 + bytes([1 if s['tip'] else 0])]
    if profile == 'pen':
        bits = (1 if s['tip'] else 0) | (2 if s['barrel'] else 0) | (4 if s['eraser'] else 0) | \
            (8 if s['eraser'] else 0) | (16 if s['range'] else 0)
        return [struct.pack('<BHHH', bits, scale(s['x'], 20000), scale(s['y'], 15000),
                            scale(s['pressure'] if s['tip'] else 0, 1023))]
    if profile == 'penpartner':
        x, y = scale(s['x'], 5040), scale(s['y'], 3780)
        if state['mode'] != 2:
            # Before a mode switch, report 1 as the report descriptor declares
            # it: the report ID, three buttons, relative X, Y and wheel, and
            # three constant bytes (QEMU's model sent four bytes without the
            # ID, which a HID parser drops). INFERRED: the pen moves it as a
            # mouse, the change in tablet units divided by PP_MOUSE_DIV, in
            # steps of at most 127; the tip is the left button.
            buttons = (s['buttons'] | (1 if s['tip'] else 0)) & 7
            px, py = state.get('px', x), state.get('py', y)
            dx, dy = (x - px) // PP_MOUSE_DIV, (y - py) // PP_MOUSE_DIV
            if advance:
                state['px'], state['py'] = px + dx * PP_MOUSE_DIV, py + dy * PP_MOUSE_DIV
            out = []
            while True:
                sx, sy = clamp(dx, -127, 127), clamp(dy, -127, 127)
                out.append(struct.pack('<BBbbb3x', 1, buttons, sx, sy, 0))
                dx, dy = dx - sx, dy - sy
                if not (dx or dy) or not advance:
                    return out
        status = (0x20 if s['eraser'] else 0) | (0x40 if s['barrel'] else 0)
        if s['tip']:
            press = clamp(int(round(-127 + s['pressure'] * 254)), -126, 127)
        else:
            press = -127
        # Report 2 is declared with seven data bytes after its ID; Linux's
        # wacom_penpartner_irq reads the first six (WACOM_PKGLEN_PENPRTN 7
        # with the ID), so the seventh is sent as 00.
        return [struct.pack('<BHHBbx', 2, x, y, status, press)]
    raise ValueError(profile)


# ---------------------------------------------------------------- the model


class Model:
    def __init__(self, connection, trace, profile, speed):
        self.connection, self.trace, self.profile = connection, trace, profile
        self.p = dict(PROFILES[profile])
        self.high = speed == 'high'
        self.p['high'] = self.high
        self.device = device_descriptor(self.p)
        self.config_desc = config_descriptor(self.p, self.high)
        self.other = config_descriptor(self.p, not self.high, 7)
        self.strings = {0: b'\x04\x03\x09\x04', 1: string('usb-wdm-free simulation'),
                        2: string(self.p['product'] + ' - SIMULATED'), 3: string('SIM0001')}
        self.peer_caps = 0
        self.config = 0
        self.receiving = False
        self.state = dict(mode=1, idle=0)
        self.s = dict(x=0.5, y=0.5, tip=False, buttons=0, range=True, pressure=0.6, barrel=False,
                      eraser=False)
        self.rx = bytearray()
        self.sent = 0

    def log(self, event, **fields):
        self.trace.write(json.dumps(dict(time=time.time(), event=event, **fields)) + '\n')
        self.trace.flush()

    @property
    def ids64(self):
        return bool(self.peer_caps & CAPS & CAP_64BIT_IDS)

    def send(self, kind, ident=0, data=b''):
        header = '<IIQ' if self.ids64 else '<III'
        self.connection.sendall(struct.pack(header, kind, len(data), ident) + data)

    def info(self):
        types, intervals, interfaces = bytearray([255] * 32), bytearray(32), bytearray(32)
        packets = [0] * 32
        for index in (0, 16):
            types[index], packets[index] = 0, 64 if self.high else 8
        if self.config == 1:
            index = 0x11
            types[index], intervals[index], interfaces[index] = 3, (4 if self.high else 10), 0
            packets[index] = self.p['packet']
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << 4):
            data += struct.pack('<32H', *packets)
        self.send(EP_INFO, data=data)
        numbers, classes, subs, protos = bytearray(32), bytearray(32), bytearray(32), bytearray(32)
        classes[0], subs[0], protos[0] = 3, self.p.get('sub', 0), self.p.get('proto', 0)
        self.send(IFACE_INFO, data=struct.pack('<I', 1) + numbers + classes + subs + protos)

    def push(self):
        """Send the current state's reports; count them."""
        n = 0
        if self.config != 1 or not self.receiving:
            self.log('report_dropped', reason='not receiving')
            return 0
        for r in reports(self.profile, self.s, self.state):
            self.send(INTERRUPT, 0, struct.pack('<BBH', 0x81, 0, len(r)) + r)
            n += 1
        self.sent += n
        return n

    def command(self, cmd):
        for key in ('x', 'y', 'pressure'):
            if key in cmd:
                self.s[key] = clamp(float(cmd[key]), 0.0, 1.0)
        for key in ('tip', 'range', 'barrel', 'eraser'):
            if key in cmd:
                self.s[key] = bool(cmd[key])
        if 'buttons' in cmd:
            self.s['buttons'] = int(cmd['buttons']) & 7
        n = self.push()
        self.log('command', cmd=cmd, sent=n, mode=self.state['mode'])
        return n

    def control(self, ident, packet):
        endpoint, request, reqtype, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        payload, result, status = packet[10:], b'', 0
        if reqtype == 0x81 and request == 6 and value >> 8 == 0x22:
            result = self.p['desc']
            self.log('hid_report_descriptor', requested=length)
        elif reqtype == 0x81 and request == 6 and value >> 8 == 0x21:
            result = hid_descriptor(self.p)
        elif reqtype & 0x60 == 0:
            if request == 6 and reqtype == 0x80:
                dtype, number = value >> 8, value & 255
                result = self.device if dtype == 1 and number == 0 else \
                    self.config_desc if dtype == 2 and number == 0 else \
                    self.strings.get(number) if dtype == 3 else \
                    bytes([10, 6, 0x00, 0x02, 0, 0, 0, 8, 1, 0]) if dtype == 6 and self.high else \
                    self.other if dtype == 7 and self.high else None
                if result is None:
                    result, status = b'', 4
            elif request == 0:
                result = b'\x00\x00'
            elif request in (1, 3):
                pass
            elif request == 8:
                result = bytes((self.config,))
            elif request == 10:
                result = b'\x00'
            else:
                status = 4
        elif reqtype == 0x21 and request == 0x0A:                    # SET_IDLE
            self.state['idle'] = value >> 8
        elif reqtype == 0x21 and request == 0x0B:                    # SET_PROTOCOL
            pass
        elif reqtype == 0xA1 and request == 0x02:                    # GET_IDLE
            result = bytes([self.state['idle']])
        elif reqtype == 0x21 and request == 0x09:                    # SET_REPORT
            rtype, rid = value >> 8, value & 255
            self.log('set_report', type=rtype, id=rid, data=payload.hex())
            if self.profile == 'penpartner' and payload:
                # QEMU's model takes the first data byte as the new mode.
                self.state['mode'] = payload[0]
                self.log('penpartner_mode', mode=payload[0])
        elif reqtype == 0xA1 and request == 0x01:                    # GET_REPORT
            rtype, rid = value >> 8, value & 255
            if self.profile == 'penpartner':
                # QEMU's model answers a GET_REPORT (A1 01) with its current
                # input report, whatever report was asked for (dev-wacom.c,
                # HID_GET_REPORT); its "00 mode" answer is for the vendor
                # request 21 01 only.
                result = reports(self.profile, self.s, self.state, advance=False)[0]
            elif self.profile == 'mtouch' and rtype == 3 and rid == 2:
                result = bytes([2, 2])
            else:
                r = reports(self.profile, self.s, self.state, advance=False)[0]
                result = r
            self.log('get_report', type=rtype, id=rid, length=length)
        else:
            status = 4
            self.log('control_unknown', request=request, request_type=reqtype, value=value, index=index)
        result = result[:length]
        actual = len(result) if reqtype & 0x80 else (0 if status else length)
        self.send(CONTROL, ident, struct.pack('<BBBBHHH', endpoint, request, reqtype, status, value, index, actual)
                  + result)
        if request == 6 and reqtype == 0x80:
            self.log('get_descriptor', type=value >> 8, number=value & 255, requested=length, actual=actual,
                     status=status)
        else:
            self.log('control', request=request, request_type=reqtype, value=value, index=index,
                     requested=length, status=status)

    def handle(self, kind, ident, packet):
        if kind == RESET:
            self.config, self.receiving = 0, False
            self.state['mode'] = 1
            self.log('reset')
        elif kind in (SET_CONFIG, GET_CONFIG):
            if kind == SET_CONFIG:
                if len(packet) != 1 or packet[0] not in (0, 1):
                    self.send(CONFIG_STATUS, ident, b'\x04\x00')
                    return
                self.config, self.receiving = packet[0], False
                self.info()
                self.log('configuration', value=self.config)
            self.send(CONFIG_STATUS, ident, bytes((0, self.config)))
        elif kind == SET_ALT:
            number, alt = packet[0], packet[1]
            ok = number == 0 and alt == 0
            self.send(ALT_STATUS, ident, bytes((0 if ok else 4, number, 0)))
        elif kind == GET_ALT:
            self.send(ALT_STATUS, ident, bytes((0, packet[0], 0)))
        elif kind in (START_INT, STOP_INT):
            endpoint = packet[0]
            ok = self.config == 1 and endpoint == 0x81
            self.receiving = bool(ok and kind == START_INT)
            self.send(INT_STATUS, ident, bytes((0 if ok else 4, endpoint)))
            self.log('interrupt_receiving', endpoint=endpoint, on=kind == START_INT, ok=bool(ok))
            if self.receiving:
                self.push()
        elif kind == INTERRUPT:
            endpoint = packet[0]
            self.send(INTERRUPT, ident, struct.pack('<BBH', endpoint, 4, 0))
        elif kind == CONTROL:
            self.control(ident, packet)
        elif kind == CANCEL:
            self.log('cancel', ident=ident)
        elif kind == BULK:
            endpoint = packet[0]
            self.send(BULK, ident, struct.pack('<BBHI', endpoint, 4, 0, 0) +
                      (b'\0\0' if self.peer_caps & CAP_32BIT_BULK else b''))
        else:
            raise ValueError(f'unsupported usbredir message {kind}')

    def take(self):
        size = 16 if self.ids64 else 12
        while len(self.rx) >= size:
            if self.ids64:
                kind, length, ident = struct.unpack('<IIQ', self.rx[:16])
            else:
                kind, length, ident = struct.unpack('<III', self.rx[:12])
            if length > 65536:
                raise ValueError('message exceeds test bound')
            if len(self.rx) < size + length:
                return
            body = bytes(self.rx[size:size + length])
            del self.rx[:size + length]
            self.handle(kind, ident, body)

    def hello(self):
        self.send(HELLO, data=b'usb-wdm-free absptr simulation 0.1'.ljust(64, b'\x00') + struct.pack('<I', CAPS))

    def connect(self):
        data = struct.pack('<BBBBHH', 2 if self.high else 1, 0, 0, 0, self.p.get('vid', 0xFFFF), self.p['pid'])
        if self.peer_caps & (1 << 1):
            data += struct.pack('<H', self.p.get('bcd', 0x0100))
        self.info()
        self.send(CONNECT, data=data)
        self.log('connected', model='SIMULATED absolute pointer', profile=self.profile,
                 speed='high' if self.high else 'full')

    def handshake(self):
        self.hello()
        self.connection.settimeout(5)
        head = b''
        while len(head) < 12:
            chunk = self.connection.recv(12 - len(head))
            if not chunk:
                raise EOFError
            head += chunk
        kind, length, _ = struct.unpack('<III', head)
        if kind != HELLO or length < 64 or length > 4096:
            raise ValueError('invalid hello')
        body = b''
        while len(body) < length:
            chunk = self.connection.recv(length - len(body))
            if not chunk:
                raise EOFError
            body += chunk
        self.peer_caps = struct.unpack('<I', body[64:68])[0] if length >= 68 else 0
        self.log('hello', capabilities=self.peer_caps)
        self.connect()
        self.connection.setblocking(False)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, required=True, help='usbredir port QEMU connects to')
    ap.add_argument('--ctl', type=int, required=True, help='control port for the leg script')
    ap.add_argument('--log', type=Path, required=True)
    ap.add_argument('--profile', choices=sorted(PROFILES), default='tablet')
    ap.add_argument('--speed', choices=('full', 'high'), default='full')
    args = ap.parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server, socket.socket() as ctl:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        ctl.bind(('127.0.0.1', args.ctl))
        ctl.listen(4)
        print(f'absptr simulation ({args.profile}, {args.speed}) usbredir 127.0.0.1:{args.port}, '
              f'control 127.0.0.1:{args.ctl}', flush=True)
        connection, _ = server.accept()
        model = Model(connection, trace, args.profile, args.speed)
        clients = {}
        try:
            model.handshake()
            while True:
                rl = [connection, ctl] + list(clients)
                ready, _, _ = select.select(rl, [], [], 1.0)
                for r in ready:
                    if r is connection:
                        chunk = connection.recv(65536)
                        if not chunk:
                            raise EOFError
                        model.rx.extend(chunk)
                        model.take()
                    elif r is ctl:
                        c, _ = ctl.accept()
                        clients[c] = b''
                    else:
                        chunk = r.recv(4096)
                        if not chunk:
                            del clients[r]
                            r.close()
                            continue
                        clients[r] += chunk
                        while b'\n' in clients[r]:
                            line, clients[r] = clients[r].split(b'\n', 1)
                            try:
                                cmd = json.loads(line)
                                if cmd.get('quit'):
                                    r.sendall(b'{"ok": true, "quit": true}' + bytes([10]))
                                    raise EOFError
                                n = model.command(cmd)
                                r.sendall(json.dumps(dict(ok=True, sent=n, mode=model.state['mode'])).encode() + b'\n')
                            except (ValueError, KeyError) as e:
                                r.sendall(json.dumps(dict(ok=False, error=str(e))).encode() + b'\n')
        except (EOFError, ConnectionResetError, BrokenPipeError, ConnectionAbortedError):
            model.log('disconnected', sent=model.sent)


if __name__ == '__main__':
    main()
