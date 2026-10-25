#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Test-only Xbox pad simulator for xpad98, exported to QEMU over localhost usbredir.

Profiles (docs/design/gamepad.md sections 1 and 5):

  x360      a wired Xbox 360-protocol pad, 2DC8:3106 (an 8BitDo id outside
            x360c's list, in xp98me.inf and xp2k.inf); vendor class
            interfaces ff/5d/01, ff/5d/03, ff/5d/02, ff/fd/13
  xone      an Xbox One S pad, 045E:02EA (GIP, ff/47/d0)
  series    an Xbox Series X|S pad, 045E:0B12 (GIP)
  receiver  the Xbox 360 wireless receiver, 045E:0719: four slots of
            ff/5d/81 (pad) and ff/5d/82 (headset); one pad on slot 0

Original implementation from the packet layouts that xpad98's own cores
(xusb.c, gip.c, adapted from Linux xpad.c) parse and build, the USB 2.0
chapter 9 facts and SPICE's usb-redirection-protocol.txt; the usbredir
framing follows this repository's acmemu.py. The descriptors have the
shape of the real devices (interfaces, classes, endpoints) with
simulation strings; class-specific descriptor bodies are not modelled. A
real pad is Full Speed; --speed high presents the same device at High
Speed so that a guest's EHCI driver carries it (a test device shape, not
a real pad's). No physical USB device is accessed.

Behaviour:
  x360      answers the start request (C1 01 0100, 20 bytes), echoes an
            LED command (01 03 cc) as the pad's LED status report, logs
            rumble (00 08 00 SS WW ...), reports XUSB input (00 14 ...)
  GIP       sends ANNOUNCE (02 20) until the host's power-on (05 20 .. 00),
            then input (20 00 ..); Guide as a virtual key report (07 30)
            repeated until acknowledged (01 20 .. 07); logs every host
            packet and checks the host's sequence numbers
  receiver  answers the presence inquiry (08 00 0F C0) with 08 80 on
            the slots with a pad (--pads N: slots 0..N-1, default 1) and
            08 00 on the empty ones, logs LED (.. 08 4x), rumble and
            power-off, reports input (00 01 00 F0 + XUSB) on every slot
            with a pad. --join SLOT:SECONDS switches a pad on later and
            --leave SLOT:SECONDS off (an unsolicited 08 80 / 08 00 status
            change, xpad360w_process_packet's connect/disconnect), seconds
            after the configuration

Input follows a fixed pattern (PATTERN below) that walks every button,
the four d-pad directions, both sticks to both ends and both triggers,
then repeats; JOYTEST.EXE in the guest records what WinMM saw.
"""
import argparse
import collections
import json
import socket
import struct
import time
from pathlib import Path

CAP_64BIT_IDS = 1 << 5
CAP_32BIT_BULK = 1 << 6
CAPS = (1 << 1) | (1 << 4) | CAP_64BIT_IDS | CAP_32BIT_BULK

# usbredir message types used here
HELLO, CONNECT, RESET, IFACE_INFO, EP_INFO = 0, 1, 3, 4, 5
SET_CONFIG, GET_CONFIG, CONFIG_STATUS = 6, 7, 8
SET_ALT, GET_ALT, ALT_STATUS = 9, 10, 11
START_INT, STOP_INT, INT_STATUS = 15, 16, 17
CANCEL, CONTROL, BULK, INTERRUPT = 21, 100, 101, 103

# Normalised buttons, as xpcore.h XP_B_*.
A, B, X, Y, LB, RB, BACK, START, LS, RS, GUIDE = (1 << i for i in range(11))
UP, DOWN, LEFT, RIGHT = 1, 2, 4, 8

# ---------------------------------------------------------------- descriptors


def ep(address, packet, interval, kind=3):
    return (address, kind, packet, interval)


def iface(number, cls, sub, proto, endpoints, alt=0, extra=b''):
    return dict(number=number, alt=alt, cls=cls, sub=sub, proto=proto, endpoints=endpoints, extra=extra)


def x360_ifaces():
    return [
        iface(0, 0xFF, 0x5D, 0x01, [ep(0x81, 32, 4), ep(0x01, 32, 8)],
              extra=bytes([17, 0x21, 0x10, 0x01, 0x01, 0x25, 0x81, 0x14, 0x03, 0x03, 0x03, 0x04, 0x13, 0x01, 0x08, 0x03, 0x03])),
        iface(1, 0xFF, 0x5D, 0x03, [ep(0x82, 32, 2), ep(0x02, 32, 4), ep(0x83, 32, 64), ep(0x03, 32, 16)]),
        iface(2, 0xFF, 0x5D, 0x02, [ep(0x84, 32, 16)]),
        iface(3, 0xFF, 0xFD, 0x13, [], extra=bytes([6, 0x41, 0x00, 0x01, 0x01, 0x03])),
    ]


def gip_ifaces():
    return [
        iface(0, 0xFF, 0x47, 0xD0, [ep(0x02, 64, 4), ep(0x82, 64, 4)]),
        iface(1, 0xFF, 0x47, 0xD0, []),
        iface(1, 0xFF, 0x47, 0xD0, [ep(0x03, 228, 1, 1), ep(0x83, 64, 1, 1)], alt=1),
        iface(2, 0xFF, 0x47, 0xD0, []),
        iface(2, 0xFF, 0x47, 0xD0, [ep(0x04, 64, 0, 2), ep(0x84, 64, 0, 2)], alt=1),
    ]


# A plain HID class game pad for Windows' own hidusb.sys (reference profile
# 'hidpad', not an Xbox pad): the shape 1990s USB pads use, 8-bit axes,
# one hat, 12 buttons. It tells whether a guest's joystick path works at
# all, apart from xpad98.
HIDPAD_REPORT_DESC = bytes([
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x35, 0x00, 0x46, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x04, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02,
    0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0C, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0C, 0x81, 0x02,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0xC0])


# xpad98's own report descriptor (layout 0, Guide on, as report.c builds it
# after the signed-axis change) served by Windows' hidusb.sys instead: it
# separates "the descriptor" from "the minidriver" on a guest.
XPAD_REPORT_DESC = bytes.fromhex(
    '05010905a101160080 26ff7f 7510 9505 0930 0931 0932 0933 0934 8102'
    '0939 1500 2507 3500 463b01 6514 7504 9501 8142 6500 4500 8103'
    '0509 1901 290b 1500 2501 7501 950b 8102 7505 9501 8103 c0'.replace(' ', ''))


# Test variants of XPAD_REPORT_DESC for the 'hidxpad' profile (--variant),
# to find which item a guest's joystick path refuses. Each is a set of
# (old, new) hex replacements on the descriptor; the report layout is the same.
VARIANTS = {
    '': [],
    'rz': [('09330934', '09350936')],                         # Rx/Ry as Rz/Slider
    'phys': [('26ff7f', '26ff7f360080 46ff7f'.replace(' ', ''))],  # physical = logical
    'joy': [('0905a101', '0904a101')],                         # top usage Joystick
    'nohat': [('0939150025073500463b016514750495018142650045008103', '750895018103')],
    'four': [('9505', '9504'), ('09300931093209330934', '0930093109320935')],  # 4 axes X Y Z Rz
    # Rz/Slider and Logical 0..32767 (values (v + 32768) >> 1): a range that
    # reads the same signed or unsigned in 16 bits.
    'rz15': [('09330934', '09350936'), ('16008026ff7f', '150026ff7f')],
}


def variant_desc(name):
    h = XPAD_REPORT_DESC.hex()
    for old, new in VARIANTS[name]:
        assert old in h, (name, old)
        h = h.replace(old, new)
    return bytes.fromhex(h)


def hid_ifaces(desc, packet):
    hid = bytes([9, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, len(desc) & 255, len(desc) >> 8])
    return [iface(0, 0x03, 0x00, 0x00, [ep(0x81, packet, 10)], extra=hid)]


def hidpad_ifaces():
    return hid_ifaces(HIDPAD_REPORT_DESC, 8)


VARIANT = ['']      # set by --variant before the model starts


def hidxpad_ifaces():
    return hid_ifaces(variant_desc(VARIANT[0]), 16)


def receiver_ifaces():
    out = []
    for slot in range(4):
        n = slot * 2
        out.append(iface(n, 0xFF, 0x5D, 0x81, [ep(0x81 + n, 32, 1), ep(0x01 + n, 32, 8)]))
        out.append(iface(n + 1, 0xFF, 0x5D, 0x82, [ep(0x82 + n, 32, 2), ep(0x02 + n, 32, 4)]))
    return out


PROFILES = {
    'x360': dict(vid=0x2DC8, pid=0x3106, family='x360', cls=(0xFF, 0xFF, 0xFF), ep0=8, ifaces=x360_ifaces,
                 product='Xbox 360-protocol pad - SIMULATED'),
    'xone': dict(vid=0x045E, pid=0x02EA, family='gip', cls=(0xFF, 0x47, 0xD0), ep0=64, ifaces=gip_ifaces,
                 product='Xbox One S pad (GIP) - SIMULATED'),
    'series': dict(vid=0x045E, pid=0x0B12, family='gip', cls=(0xFF, 0x47, 0xD0), ep0=64, ifaces=gip_ifaces,
                   product='Xbox Series X|S pad (GIP) - SIMULATED'),
    'receiver': dict(vid=0x045E, pid=0x0719, family='receiver', cls=(0xFF, 0xFF, 0xFF), ep0=64,
                     ifaces=receiver_ifaces, product='Xbox 360 wireless receiver - SIMULATED'),
    'hidpad': dict(vid=0xFFFF, pid=0x0004, family='hid', cls=(0x00, 0x00, 0x00), ep0=8,
                   ifaces=hidpad_ifaces, product='HID class game pad (reference) - SIMULATED'),
    'hidxpad': dict(vid=0xFFFF, pid=0x0005, family='hid', cls=(0x00, 0x00, 0x00), ep0=8,
                    ifaces=hidxpad_ifaces, product='HID pad, xpad98 descriptor (reference) - SIMULATED'),
}


def hs_interval(fs_ms, kind):
    """A Full Speed interrupt bInterval in ms as a High Speed exponent
    (2^(b-1) microframes), the nearest not longer."""
    if kind != 3:
        return fs_ms
    b = 1
    while b < 16 and (1 << b) <= fs_ms * 8:
        b += 1
    return b


def device_descriptor(profile, high):
    p = PROFILES[profile]
    cls, sub, proto = p['cls']
    return struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x0200, cls, sub, proto, p['ep0'],
                       p['vid'], p['pid'], 0x0114, 1, 2, 3, 1)


def config_descriptor(profile, high, kind=2):
    p = PROFILES[profile]
    body = bytearray()
    numbers = set()
    for f in p['ifaces']():
        numbers.add(f['number'])
        body += bytes([9, 4, f['number'], f['alt'], len(f['endpoints']), f['cls'], f['sub'], f['proto'], 0])
        body += f['extra']
        for address, ekind, packet, interval in f['endpoints']:
            if high and ekind == 2:
                packet = 512
            if high:
                interval = hs_interval(interval, ekind)
            body += struct.pack('<BBBBHB', 7, 5, address, ekind, packet, interval)
    total = 9 + len(body)
    return struct.pack('<BBHBBBBB', 9, kind, total, len(numbers), 1, 0, 0xA0, 250) + bytes(body)


def qualifier(profile):
    d = device_descriptor(profile, False)
    return bytes([10, 6, 0x00, 0x02, d[4], d[5], d[6], d[7], 1, 0])


def string(text):
    data = text.encode('utf-16le')
    return bytes((len(data) + 2, 3)) + data


# ---------------------------------------------------------------- input


def pad(buttons=0, dpad=0, lx=0, ly=0, rx=0, ry=0, lt=0, rt=0):
    """A normalised pad state. ly/ry are raw: positive is up (both XUSB and
    GIP send up as positive; the driver inverts). lt/rt are 0..1 (fraction)."""
    return dict(buttons=buttons, dpad=dpad, lx=lx, ly=ly, rx=rx, ry=ry, lt=lt, rt=rt)


MAXV, MINV = 32767, -32768


def build_pattern():
    steps = [('neutral', pad(), 0.6)]
    for name, bit in (('A', A), ('B', B), ('X', X), ('Y', Y), ('LB', LB), ('RB', RB), ('Back', BACK),
                      ('Start', START), ('LS', LS), ('RS', RS), ('Guide', GUIDE)):
        steps += [(name, pad(buttons=bit), 0.5), ('release', pad(), 0.3)]
    for name, bit in (('up', UP), ('right', RIGHT), ('down', DOWN), ('left', LEFT)):
        steps += [('dpad ' + name, pad(dpad=bit), 0.5), ('release', pad(), 0.3)]
    for axis in ('lx', 'ly', 'rx', 'ry'):
        for name, value in (('min', MINV), ('max', MAXV)):
            steps += [(axis + ' ' + name, pad(**{axis: value}), 0.5)]
        steps += [('centre', pad(), 0.3)]
    for trig in ('lt', 'rt'):
        steps += [(trig + ' full', pad(**{trig: 1.0}), 0.5), ('release', pad(), 0.3)]
    return steps


PATTERN = build_pattern()


def xusb_report(s):
    """XUSB input, 20 bytes (the layout xusb.c X360Body reads)."""
    b2 = s['dpad'] & 0x0F
    b = s['buttons']
    b2 |= (0x10 if b & START else 0) | (0x20 if b & BACK else 0) | (0x40 if b & LS else 0) | (0x80 if b & RS else 0)
    b3 = (0x10 if b & A else 0) | (0x20 if b & B else 0) | (0x40 if b & X else 0) | (0x80 if b & Y else 0)
    b3 |= (0x01 if b & LB else 0) | (0x02 if b & RB else 0) | (0x04 if b & GUIDE else 0)
    return struct.pack('<BBBBBBhhhh6x', 0x00, 0x14, b2, b3, round(s['lt'] * 255), round(s['rt'] * 255),
                       s['lx'], s['ly'], s['rx'], s['ry'])


def receiver_report(s):
    """Receiver pad data: 00 01 00 F0, then the XUSB body, 29 bytes."""
    return bytes([0x00, 0x01, 0x00, 0xF0]) + xusb_report(s) + bytes(5)


def hidpad_report(s):
    """The reference HID pad's 8-byte report: X Y Z Rz (0..255, centre 128),
    hat (0-7, 8 centred) in the low nibble, 12 buttons."""
    def ax(v):
        return max(0, min(255, (v + 32768) >> 8))
    b, d = s['buttons'], s['dpad']
    dx = (1 if d & RIGHT else 0) - (1 if d & LEFT else 0)
    dy = (1 if d & DOWN else 0) - (1 if d & UP else 0)
    hat = {(0, -1): 0, (1, -1): 1, (1, 0): 2, (1, 1): 3, (0, 1): 4, (-1, 1): 5, (-1, 0): 6, (-1, -1): 7}.get((dx, dy), 8)
    z = 128 + round(s['lt'] * 127) - round(s['rt'] * 128)
    order = (A, B, X, Y, LB, RB, BACK, START, LS, RS, GUIDE)
    bits = sum(1 << i for i, bit in enumerate(order) if b & bit)
    return struct.pack('<BBBBBH', ax(s['lx']), ax(-s['ly']), max(0, min(255, z)), ax(s['rx']), hat, bits) + b'\0'


def hidxpad_report(s):
    """The 13-byte report report.c builds (layout 0, Guide on): X Y Z Rx Ry
    signed 16-bit (Y down positive, Z = LT - RT), hat, 11 buttons."""
    def c(v):
        return max(-32768, min(32767, v))
    b, d = s['buttons'], s['dpad']
    dx = (1 if d & RIGHT else 0) - (1 if d & LEFT else 0)
    dy = (1 if d & DOWN else 0) - (1 if d & UP else 0)
    hat = {(0, -1): 0, (1, -1): 1, (1, 0): 2, (1, 1): 3, (0, 1): 4, (-1, 1): 5, (-1, 0): 6, (-1, -1): 7}.get((dx, dy), 8)
    z = c(round(s['lt'] * 32767) - round(s['rt'] * 32767))
    order = (A, B, X, Y, LB, RB, BACK, START, LS, RS, GUIDE)
    bits = sum(1 << i for i, bit in enumerate(order) if b & bit)
    return struct.pack('<hhhhhBH', c(s['lx']), c(~s['ly']), z, c(s['rx']), c(~s['ry']), hat, bits)


def gip_input(s, seq):
    """GIP_CMD_INPUT, 18 bytes (gip.c XpGipParse)."""
    b = s['buttons']
    b4 = (0x04 if b & START else 0) | (0x08 if b & BACK else 0) | (0x10 if b & A else 0) | \
         (0x20 if b & B else 0) | (0x40 if b & X else 0) | (0x80 if b & Y else 0)
    b5 = (s['dpad'] & 0x0F) | (0x10 if b & LB else 0) | (0x20 if b & RB else 0) | \
         (0x40 if b & LS else 0) | (0x80 if b & RS else 0)
    return struct.pack('<BBBBBBHHhhhh', 0x20, 0x00, seq & 255, 0x0E, b4, b5,
                       round(s['lt'] * 1023), round(s['rt'] * 1023), s['lx'], s['ly'], s['rx'], s['ry'])


def gip_guide(pressed, seq, ack_wanted):
    return bytes([0x07, 0x30 if ack_wanted else 0x20, seq & 255, 0x02, 0x01 if pressed else 0x00, 0x5B])


def gip_announce(profile, seq):
    p = PROFILES[profile]
    payload = bytes([0x7E, 0xED, 0x82, 0x4C, 0x13, 0x0B, 0x00, 0x00]) + struct.pack('<HH', p['vid'], p['pid']) + \
        bytes([0x05, 0x00, 0x11, 0x00, 0x82, 0x0C, 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00])
    return bytes([0x02, 0x20, seq & 255, len(payload)]) + payload


# ---------------------------------------------------------------- the model


class Model:
    def __init__(self, connection, trace, profile='x360', speed='full', hold=1.0, clock=time.monotonic,
                 pads=1, events=()):
        self.connection, self.trace, self.profile = connection, trace, profile
        self.p = PROFILES[profile]
        self.family = self.p['family']
        self.speed, self.high = speed, speed == 'high'
        self.hold, self.clock = hold, clock
        self.device = device_descriptor(profile, self.high)
        self.config_desc = config_descriptor(profile, self.high)
        self.other = config_descriptor(profile, not self.high, 7)
        self.strings = {0: b'\x04\x03\x09\x04', 1: string('usb-wdm-free simulation'),
                        2: string(self.p['product']), 3: string('SIM0001')}
        self.ifaces = self.p['ifaces']()
        self.peer_caps = 0
        self.config = 0
        self.alt = {}
        self.receiving = set()          # IN endpoints with interrupt receiving on
        self.out_seq = {}               # GIP: last host sequence per command
        self.seq = 0                    # GIP: device-to-host sequence
        self.powered = False            # GIP: host sent power-on
        self.slot_known = [False] * 4   # receiver: presence answered per slot
        self.pads = pads                # receiver: slots 0..pads-1 hold a pad
        self.present = set(range(pads))
        self.events = sorted(events)    # receiver: (seconds, slot, present)
        self.pending_events = []
        self.config_t = None
        self.guide_unacked = 0.0        # GIP: time the unacknowledged Guide report went out
        self.guide_state = False
        self.counts = collections.Counter()
        self.step, self.step_t, self.state, self.sent = 0, None, PATTERN[0][1], None
        self.next_announce = 0.0
        self.rx = bytearray()

    # -- plumbing (as acmemu.py)
    def log(self, event, **fields):
        self.trace.write(json.dumps(dict(time=time.time(), event=event, **fields)) + '\n')
        self.trace.flush()

    @property
    def ids64(self):
        return bool(self.peer_caps & CAPS & CAP_64BIT_IDS)

    def send(self, kind, ident=0, data=b''):
        header = '<IIQ' if self.ids64 else '<III'
        self.connection.sendall(struct.pack(header, kind, len(data), ident) + data)

    def endpoints(self):
        """Endpoints of the current alternate settings: address -> (kind, packet, interval, interface)."""
        out = {}
        for f in self.ifaces:
            if f['alt'] != self.alt.get(f['number'], 0):
                continue
            for address, kind, packet, interval in f['endpoints']:
                if self.high and kind == 2:
                    packet = 512
                if self.high:
                    interval = hs_interval(interval, kind)
                out[address] = (kind, packet, interval, f['number'])
        return out

    def info(self):
        types, intervals, interfaces = bytearray([255] * 32), bytearray(32), bytearray(32)
        packets = [0] * 32
        for index in (0, 16):
            types[index], packets[index] = 0, self.p['ep0']
        for address, (kind, packet, interval, number) in self.endpoints().items():
            index = (address & 0x0F) | (0x10 if address & 0x80 else 0)
            types[index], intervals[index], interfaces[index], packets[index] = kind, interval, number, packet
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << 4):
            data += struct.pack('<32H', *packets)
        self.send(EP_INFO, data=data)
        numbers, classes, subs, protos = bytearray(32), bytearray(32), bytearray(32), bytearray(32)
        alts = [f for f in self.ifaces if f['alt'] == 0]
        for i, f in enumerate(alts):
            numbers[i], classes[i], subs[i], protos[i] = f['number'], f['cls'], f['sub'], f['proto']
        self.send(IFACE_INFO, data=struct.pack('<I', len(alts)) + numbers + classes + subs + protos)

    def interrupt_in(self, endpoint, data):
        """One interrupt IN packet; sent only while QEMU is receiving."""
        if endpoint not in self.receiving:
            self.counts['dropped_in'] += 1
            return False
        self.send(INTERRUPT, 0, struct.pack('<BBH', endpoint, 0, len(data)) + data)
        self.counts['in'] += 1
        return True

    # -- the pad's side
    def pad_in(self):
        if self.family in ('x360', 'hid'):
            return 0x81
        if self.family == 'gip':
            return 0x82
        return 0x81     # receiver slot 0

    def report(self, s):
        if self.family == 'x360':
            return self.interrupt_in(0x81, xusb_report(s))
        if self.family == 'hid':
            if self.profile != 'hidxpad':
                return self.interrupt_in(0x81, hidpad_report(s))
            r = hidxpad_report(s)
            if VARIANT[0] == 'four':
                r = r[:8] + r[10:]          # X Y Z Rz(=Rx), no Ry
            if VARIANT[0] == 'rz15':
                axes = struct.unpack('<5h', r[:10])
                r = struct.pack('<5H', *[(a + 32768) >> 1 for a in axes]) + r[10:]
            return self.interrupt_in(0x81, r)
        if self.family == 'receiver':
            ok = True
            for slot in sorted(self.present):
                if self.slot_known[slot]:
                    ok = self.interrupt_in(0x81 + slot * 2, receiver_report(s)) and ok
            return ok
        if not self.powered:
            return False
        ok = True
        guide = bool(s['buttons'] & GUIDE)
        if guide != self.guide_state:
            ok = self.interrupt_in(0x82, gip_guide(guide, self.next_seq(), True))
            if ok:
                self.guide_state, self.guide_unacked = guide, self.clock()
        return self.interrupt_in(0x82, gip_input(s, self.next_seq())) and ok

    def slot_events(self, now):
        """Receiver: pads switched on or off on their slots, on schedule."""
        if self.config_t is None:
            return
        while self.pending_events and now - self.config_t >= self.pending_events[0][0]:
            _, slot, on = self.pending_events.pop(0)
            if on:
                self.present.add(slot)
            else:
                self.present.discard(slot)
            sent = self.interrupt_in(0x81 + slot * 2, bytes([0x08, 0x80 if on else 0x00]))
            self.log('receiver_join' if on else 'receiver_leave', slot=slot, sent=sent)
            self.sent = None                     # every present pad reports its state again

    def next_seq(self):
        self.seq = (self.seq + 1) & 255 or 1
        return self.seq

    def tick(self):
        """Advance the input pattern; send what changed."""
        now = self.clock()
        if self.config != 1 or self.pad_in() not in self.receiving:
            return
        if self.family == 'receiver':
            self.slot_events(now)
        if self.family == 'gip' and not self.powered:
            if now >= self.next_announce:
                if self.interrupt_in(0x82, gip_announce(self.profile, self.next_seq())):
                    self.log('gip_announce')
                self.next_announce = now + 2.0
            return
        if self.family == 'gip' and self.guide_unacked and now - self.guide_unacked > 0.25:
            # The pad repeats the virtual key report until it is acknowledged.
            if self.interrupt_in(0x82, gip_guide(self.guide_state, self.next_seq(), True)):
                self.guide_unacked = now
                self.counts['guide_repeat'] += 1
        if self.step_t is None:
            self.step_t = now
        name, state, length = PATTERN[self.step]
        if now - self.step_t >= length * self.hold:
            self.step = (self.step + 1) % len(PATTERN)
            self.step_t = now
            name, state, _ = PATTERN[self.step]
            if self.step == 0:
                self.counts['cycles'] += 1
                self.log('pattern_cycle', cycles=self.counts['cycles'])
        if state != self.sent and self.report(state):
            self.sent = state
            self.log('input', step=name)

    def host_packet(self, endpoint, data):
        """An interrupt OUT packet from the driver."""
        hexdata = data.hex()
        if self.family == 'x360':
            if len(data) == 3 and data[0] == 0x01 and data[1] == 0x03:
                self.log('x360_led', command=data[2])
                self.interrupt_in(0x81, bytes([0x01, 0x03, data[2]]))   # the pad's LED status
            elif len(data) == 8 and data[0] == 0x00 and data[1] == 0x08:
                self.log('x360_rumble', strong=data[3], weak=data[4])
            else:
                self.log('x360_out_unknown', data=hexdata)
        elif self.family == 'receiver':
            slot = (endpoint & 0x0F) // 2
            if len(data) == 12 and data[0] == 0x08 and data[2] == 0x0F and data[3] == 0xC0:
                self.log('receiver_presence', slot=slot)
                self.slot_known[slot] = True
                self.interrupt_in(0x81 + slot * 2, bytes([0x08, 0x80 if slot in self.present else 0x00]))
                self.sent = None
            elif len(data) == 12 and data[2] == 0x08 and data[3] & 0xF0 == 0x40:
                self.log('receiver_led', slot=slot, command=data[3] & 0x0F)
            elif len(data) == 12 and data[2] == 0x08 and data[3] == 0xC0:
                self.log('receiver_poweroff', slot=slot)
            elif len(data) == 12 and data[1] == 0x01 and data[2] == 0x0F and data[3] == 0xC0:
                self.log('receiver_rumble', slot=slot, strong=data[5], weak=data[6])
            else:
                self.log('receiver_out_unknown', slot=slot, data=hexdata)
        else:
            if len(data) < 4:
                self.log('gip_out_short', data=hexdata)
                return
            command, seq = data[0], data[2]
            if command == 0x01:
                # An acknowledgement carries the sequence number of the
                # packet it acknowledges (the pad's), not the host's own.
                in_order = True
            else:
                previous = self.out_seq.get('host')
                self.out_seq['host'] = seq
                in_order = previous is None or seq == (previous + 1) & 255
            self.log('gip_out', command=command, seq=seq, in_order=in_order, data=hexdata)
            if not in_order:
                self.counts['gip_seq_gap'] += 1
            if command == 0x05 and data[3] == 1 and len(data) >= 5 and data[4] == 0x00:
                if not self.powered:
                    self.log('gip_power_on')
                self.powered = True
                self.sent = None
            elif command == 0x01 and len(data) >= 6 and data[5] == 0x07:
                self.log('gip_guide_ack', seq=seq)
                self.guide_unacked = 0.0
            elif command == 0x09:
                self.log('gip_rumble', data=hexdata)

    # -- usbredir requests from QEMU
    def control(self, ident, packet):
        endpoint, request, reqtype, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        payload, result, status = packet[10:], b'', 0
        if reqtype == 0x81 and request == 6 and value >> 8 == 0x22 and self.family == 'hid':
            result = variant_desc(VARIANT[0]) if self.profile == 'hidxpad' else HIDPAD_REPORT_DESC
            self.log('hid_report_descriptor', requested=length)
        elif reqtype & 0x60 == 0:
            if request == 6 and reqtype == 0x80:
                dtype, number = value >> 8, value & 255
                result = self.device if dtype == 1 and number == 0 else \
                    self.config_desc if dtype == 2 and number == 0 else \
                    self.strings.get(number) if dtype == 3 else \
                    qualifier(self.profile) if dtype == 6 and number == 0 and self.high else \
                    self.other if dtype == 7 and number == 0 and self.high else None
                if result is None:
                    result, status = b'', 4
            elif request == 0:
                result = b'\x00\x00'
            elif request in (1, 3):
                pass
            elif request == 8:
                result = bytes((self.config,))
            elif request == 10:
                result = bytes((self.alt.get(index & 255, 0),))
            else:
                status = 4
        elif reqtype == 0x21 and request in (0x0A, 0x0B) and self.family == 'hid':
            self.log('hid_class_request', request=request, value=value)      # SET_IDLE, SET_PROTOCOL
        elif reqtype == 0xC1 and request == 0x01 and value == 0x0100 and self.family == 'x360':
            # The start request; Linux and xpad98 ignore the answer.
            result = bytes([0x00, 0x14]) + bytes(18)
            self.log('x360_start_request', index=index, length=length)
        else:
            status = 4
            self.log('control_unknown', request=request, request_type=reqtype, value=value, index=index)
        result = result[:length]
        actual = len(result) if reqtype & 0x80 else (0 if status else length)
        self.send(CONTROL, ident, struct.pack('<BBBBHHH', endpoint, request, reqtype, status, value, index, actual) + result)
        if request == 6 and reqtype == 0x80:
            self.log('get_descriptor', type=value >> 8, number=value & 255, requested=length, actual=actual, status=status)
        else:
            self.log('control', request=request, request_type=reqtype, value=value, index=index,
                     requested=length, status=status)
        del payload

    def handle(self, kind, ident, packet):
        if kind == RESET:
            self.config, self.receiving = 0, set()
            self.powered, self.slot_known = False, [False] * 4
            self.sent, self.guide_unacked, self.guide_state = None, 0.0, False
            self.out_seq.clear()
            self.log('reset')
        elif kind in (SET_CONFIG, GET_CONFIG):
            if kind == SET_CONFIG:
                if len(packet) != 1 or packet[0] not in (0, 1):
                    self.send(CONFIG_STATUS, ident, b'\x04\x00')
                    return
                self.config = packet[0]
                self.alt, self.receiving = {}, set()
                self.powered, self.slot_known, self.sent = False, [False] * 4, None
                self.present = set(range(self.pads))
                self.pending_events = list(self.events)
                self.config_t = self.clock()
                self.info()
                self.log('configuration', value=self.config)
            self.send(CONFIG_STATUS, ident, bytes((0, self.config)))
        elif kind == SET_ALT:
            number, alt = packet[0], packet[1]
            known = any(f['number'] == number and f['alt'] == alt for f in self.ifaces)
            if known:
                self.alt[number] = alt
                self.info()
            self.log('alt_setting', interface=number, alt=alt, ok=known)
            self.send(ALT_STATUS, ident, bytes((0 if known else 4, number, self.alt.get(number, 0))))
        elif kind == GET_ALT:
            self.send(ALT_STATUS, ident, bytes((0, packet[0], self.alt.get(packet[0], 0))))
        elif kind in (START_INT, STOP_INT):
            endpoint = packet[0]
            eps = self.endpoints()
            ok = self.config == 1 and endpoint in eps and eps[endpoint][0] == 3 and endpoint & 0x80
            if ok and kind == START_INT:
                self.receiving.add(endpoint)
                self.sent = None            # resend the current state
            else:
                self.receiving.discard(endpoint)
            self.send(INT_STATUS, ident, bytes((0 if ok else 4, endpoint)))
            self.log('interrupt_receiving', endpoint=endpoint, on=kind == START_INT, ok=bool(ok))
        elif kind == INTERRUPT:
            endpoint, _, length = struct.unpack('<BBH', packet[:4])
            data = packet[4:]
            if endpoint & 0x80 or len(data) != length or endpoint not in self.endpoints():
                self.send(INTERRUPT, ident, struct.pack('<BBH', endpoint, 4, 0))
                self.log('interrupt_out_bad', endpoint=endpoint, length=length)
                return
            self.send(INTERRUPT, ident, struct.pack('<BBH', endpoint, 0, length))
            self.counts['out'] += 1
            self.host_packet(endpoint, data)
        elif kind == CONTROL:
            self.control(ident, packet)
        elif kind == CANCEL:
            self.log('cancel', ident=ident)
        elif kind == BULK:
            endpoint = packet[0]
            self.send(BULK, ident, struct.pack('<BBHI', endpoint, 4, 0, 0) + (b'\0\0' if self.peer_caps & CAP_32BIT_BULK else b''))
            self.log('bulk_refused', endpoint=endpoint)
        else:
            raise ValueError(f'unsupported usbredir message {kind}')

    # -- the connection
    def take(self):
        """Complete messages from the receive buffer."""
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
        self.send(HELLO, data=b'usb-wdm-free xpad simulation 0.1'.ljust(64, b'\x00') + struct.pack('<I', CAPS))

    def connect(self):
        cls, sub, proto = self.p['cls']
        data = struct.pack('<BBBBHH', 2 if self.high else 1, cls, sub, proto, self.p['vid'], self.p['pid'])
        if self.peer_caps & (1 << 1):
            data += struct.pack('<H', 0x0114)
        self.info()
        self.send(CONNECT, data=data)
        self.log('connected', model='SIMULATED Xbox pad', profile=self.profile, speed=self.speed,
                 vid='%04X' % self.p['vid'], pid='%04X' % self.p['pid'])

    def run(self):
        self.hello()
        self.connection.settimeout(5)
        head = b''
        while len(head) < 12:
            chunk = self.connection.recv(12 - len(head))
            if not chunk:
                raise EOFError
            head += chunk
        kind, length, ident = struct.unpack('<III', head)
        if kind != HELLO or length < 64 or length > 4096:
            raise ValueError('invalid hello')
        body = b''
        while len(body) < length:
            chunk = self.connection.recv(length - len(body))
            if not chunk:
                raise EOFError
            body += chunk
        self.peer_caps = struct.unpack('<I', body[64:68])[0] if length >= 68 else 0
        self.log('hello', peer=body[:64].split(b'\x00')[0].decode('ascii', 'replace'), capabilities=self.peer_caps)
        self.connect()
        self.connection.settimeout(0.02)
        last_counts = 0.0
        while True:
            try:
                chunk = self.connection.recv(65536)
                if not chunk:
                    raise EOFError
                self.rx.extend(chunk)
                self.take()
            except socket.timeout:
                pass
            self.tick()
            if time.time() - last_counts > 10:
                last_counts = time.time()
                self.log('counts', **self.counts)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--log', type=Path, required=True)
    ap.add_argument('--profile', choices=sorted(PROFILES), default='x360')
    ap.add_argument('--speed', choices=('full', 'high'), default='full')
    ap.add_argument('--hold', type=float, default=1.0, help='scale of each pattern step (1.0: 0.3-0.6 s)')
    ap.add_argument('--variant', choices=sorted(VARIANTS), default='', help='hidxpad: descriptor test variant')
    ap.add_argument('--pads', type=int, choices=(0, 1, 2, 3, 4), default=1, help='receiver: pads on slots 0..N-1')
    ap.add_argument('--join', action='append', default=[], metavar='SLOT:SECONDS',
                    help='receiver: a pad switches on on SLOT, SECONDS after configuration')
    ap.add_argument('--leave', action='append', default=[], metavar='SLOT:SECONDS',
                    help='receiver: the pad on SLOT switches off')
    args = ap.parse_args()
    VARIANT[0] = args.variant
    events = [(float(t), int(sl), True) for sl, t in (x.split(':') for x in args.join)] + \
        [(float(t), int(sl), False) for sl, t in (x.split(':') for x in args.leave)]
    if any(not 0 <= e[1] <= 3 for e in events):
        ap.error('slots are 0-3')
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'xpad simulation ({args.profile}, {args.speed}) listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            model = Model(connection, trace, args.profile, args.speed, args.hold, pads=args.pads, events=events)
            try:
                model.run()
            except (EOFError, ConnectionResetError, BrokenPipeError, ConnectionAbortedError):
                model.log('disconnected', **model.counts)


if __name__ == '__main__':
    main()
