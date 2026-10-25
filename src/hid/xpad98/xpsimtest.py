#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests of xpademu.py against the driver's own cores (xpcore.dll,
built by simtest.ps1 from xusb.c, gip.c, xpcfg.c, devtab.c, report.c).
They check that what the simulator sends is what xpad98 parses and that it
answers what xpad98 sends. Results in a guest are separate."""
import ctypes
import io
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xpademu as xe  # noqa: E402

LIB = ctypes.CDLL(os.environ['XPCORE_DLL'])
U8, U16, S16 = ctypes.c_ubyte, ctypes.c_ushort, ctypes.c_short


class Pad(ctypes.Structure):
    _fields_ = [('LX', S16), ('LY', S16), ('RX', S16), ('RY', S16), ('LT', U16), ('RT', U16),
                ('TriggerMax', U16), ('Buttons', U16), ('Dpad', U8), ('Present', U8)]


class Iface(ctypes.Structure):
    _fields_ = [('Number', U8), ('Class', U8), ('SubClass', U8), ('Protocol', U8), ('NumEndpoints', U8),
                ('EpAddr', U8 * 8), ('EpAttr', U8 * 8), ('EpMaxPacket', U16 * 8)]


class Config(ctypes.Structure):
    _fields_ = [('ConfigValue', U8), ('NumInterfaces', ctypes.c_uint), ('Iface', Iface * 16),
                ('AllVendor', ctypes.c_int), ('Family', ctypes.c_uint), ('NumChannels', ctypes.c_uint),
                ('ChanIface', U8 * 4), ('ChanIn', U8 * 4), ('ChanOut', U8 * 4)]


FAM = {'x360': 1, 'receiver': 2, 'gip': 3}
EV_DATA, EV_CONNECT, EV_DISCONNECT, EV_ACK, EV_ANNOUNCE = 1, 2, 4, 8, 16


def buf(data):
    return (U8 * max(len(data), 1)).from_buffer_copy(bytes(data) or b'\0')


def x360_parse(data, pad):
    return LIB.XpX360Parse(buf(data), len(data), ctypes.byref(pad))


def x360w_parse(data, pad):
    return LIB.XpX360wParse(buf(data), len(data), ctypes.byref(pad))


def gip_parse(data, pad):
    ack = (U8 * 64)()
    n = ctypes.c_uint()
    ev = LIB.XpGipParse(buf(data), len(data), ctypes.byref(pad), ack, ctypes.byref(n))
    return ev, bytes(ack[:n.value])


def gip_init(vid, pid):
    cursor = ctypes.c_uint(0)
    out, packets = (U8 * 64)(), []
    while True:
        n = LIB.XpGipNextInit(vid, pid, ctypes.byref(cursor), out)
        if not n:
            return packets
        packets.append(bytes(out[:n]))


def expected(state, trigger_max):
    """The XP_PAD the driver should hold for a pattern state."""
    buttons = state['buttons']
    if trigger_max == 1023:
        pass        # GIP: Guide comes from its own report, checked separately
    return dict(LX=state['lx'], LY=~state['ly'], RX=state['rx'], RY=~state['ry'],
                LT=round(state['lt'] * trigger_max), RT=round(state['rt'] * trigger_max),
                Buttons=buttons, Dpad=state['dpad'])


def pad_dict(pad):
    return dict(LX=pad.LX, LY=pad.LY, RX=pad.RX, RY=pad.RY, LT=pad.LT, RT=pad.RT, Buttons=pad.Buttons, Dpad=pad.Dpad)


class Conn:
    def __init__(self):
        self.data = bytearray()

    def sendall(self, data):
        self.data.extend(data)

    def take(self):
        out = []
        while self.data:
            kind, length, ident = struct.unpack('<III', self.data[:12])
            out.append((kind, ident, bytes(self.data[12:12 + length])))
            del self.data[:12 + length]
        return out


class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


class Descriptors(unittest.TestCase):
    def test_every_profile_and_speed_parses_as_the_driver_does(self):
        for name, p in xe.PROFILES.items():
            if p['family'] == 'hid':
                continue        # the reference HID pad is not xpad98's
            for high in (False, True):
                with self.subTest(profile=name, high=high):
                    cfg = xe.config_descriptor(name, high)
                    self.assertEqual(struct.unpack('<H', cfg[2:4])[0], len(cfg))
                    c = Config()
                    self.assertEqual(LIB.XpParseConfig(buf(cfg), len(cfg), ctypes.byref(c)), 0)
                    self.assertEqual(c.AllVendor, 1)
                    self.assertEqual(c.Family, FAM[p['family']])
                    self.assertEqual(c.ConfigValue, 1)
                    if p['family'] == 'receiver':
                        self.assertEqual(c.NumChannels, 4)
                        self.assertEqual(list(c.ChanIn), [0x81, 0x83, 0x85, 0x87])
                        self.assertEqual(list(c.ChanOut), [0x01, 0x03, 0x05, 0x07])
                    else:
                        self.assertEqual(c.NumChannels, 1)
                        want = (0x81, 0x01) if p['family'] == 'x360' else (0x82, 0x02)
                        self.assertEqual((c.ChanIn[0], c.ChanOut[0]), want)
                    dev = xe.device_descriptor(name, high)
                    self.assertEqual(len(dev), 18)
                    vid, pid = struct.unpack('<HH', dev[8:12])
                    self.assertEqual((vid, pid), (p['vid'], p['pid']))
                    self.assertTrue(LIB.XpFindDevice(vid, pid))

    def test_high_speed_intervals_and_other_speed(self):
        self.assertEqual(xe.hs_interval(4, 3), 6)   # 4 ms = 32 microframes = 2^(6-1)
        self.assertEqual(xe.hs_interval(1, 3), 4)
        self.assertEqual(xe.hs_interval(8, 3), 7)
        m = xe.Model(Conn(), io.StringIO(), 'xone', 'high')
        self.assertEqual(m.other[1], 7)
        self.assertEqual(xe.qualifier('xone')[1], 6)

    def test_strings_name_a_simulation(self):
        for name in xe.PROFILES:
            m = xe.Model(Conn(), io.StringIO(), name)
            self.assertIn('SIMULATED', m.strings[2][2:].decode('utf-16le'))


class Session(unittest.TestCase):
    def model(self, profile, speed='full', **kw):
        self.conn, self.clock = Conn(), Clock()
        self.trace = io.StringIO()
        m = xe.Model(self.conn, self.trace, profile, speed, clock=self.clock, **kw)
        m.peer_caps = xe.CAPS & ~(xe.CAP_64BIT_IDS | xe.CAP_32BIT_BULK)
        m.handle(xe.SET_CONFIG, 1, b'\x01')
        kinds = [k for k, _, _ in self.conn.take()]
        self.assertEqual(kinds, [xe.EP_INFO, xe.IFACE_INFO, xe.CONFIG_STATUS])
        return m

    def start(self, m, endpoint):
        m.handle(xe.START_INT, 2, bytes([endpoint]))
        replies = self.conn.take()
        self.assertEqual(replies[-1], (xe.INT_STATUS, 2, bytes([0, endpoint])))

    def out(self, m, endpoint, data, ident=9):
        m.handle(xe.INTERRUPT, ident, struct.pack('<BBH', endpoint, 0, len(data)) + data)
        replies = self.conn.take()
        self.assertEqual(replies[0], (xe.INTERRUPT, ident, struct.pack('<BBH', endpoint, 0, len(data))))
        return replies[1:]

    def ins(self, replies, endpoint):
        out = []
        for kind, _, data in replies:
            if kind == xe.INTERRUPT:
                ep, status, length = struct.unpack('<BBH', data[:4])
                self.assertEqual((ep, status, length), (endpoint, 0, len(data) - 4))
                out.append(data[4:])
        return out

    def run_pattern(self, m, endpoint, parse, trigger_max, pad=None):
        """Drives the clock through two pattern cycles; every state sent
        parses into the state the pattern meant."""
        pad, seen = pad or Pad(), set()
        for _ in range(2 * len(xe.PATTERN) + 4):
            m.tick()
            for data in self.ins(self.conn.take(), endpoint):
                ev = parse(data, pad)
                if ev & EV_DATA and data[0] in (0x00, 0x20):
                    seen.add(m.step)
                    want = expected(xe.PATTERN[m.step][1], trigger_max)
                    got = pad_dict(pad)
                    if trigger_max == 1023:
                        got['Buttons'] &= ~xe.GUIDE
                        want['Buttons'] &= ~xe.GUIDE
                    self.assertEqual(got, want, xe.PATTERN[m.step][0])
            self.clock.t += 0.61
        return seen

    def test_x360_start_led_and_pattern(self):
        m = self.model('x360')
        # The start request (xpad_start_input) is answered with 20 bytes.
        m.handle(xe.CONTROL, 5, struct.pack('<BBBBHHH', 0x80, 1, 0xC1, 0, 0x0100, 0, 20))
        (kind, ident, data), = self.conn.take()
        self.assertEqual((kind, ident, data[3], len(data) - 10), (xe.CONTROL, 5, 0, 20))
        self.start(m, 0x81)
        led = (U8 * 64)()
        n = LIB.XpX360Led(led, LIB.XpLedForPlayer(0))
        echo = self.ins(self.out(m, 0x01, bytes(led[:n])), 0x81)
        self.assertEqual(echo, [bytes([1, 3, 2])])
        pad = Pad()
        self.assertEqual(x360_parse(echo[0], pad), 0)       # status reports are not pad data
        seen = self.run_pattern(m, 0x81, x360_parse, 255)
        self.assertGreater(len(seen), 30)
        self.assertIn('"x360_led"', self.trace.getvalue())

    def test_x360_nothing_sent_before_receiving(self):
        m = self.model('x360')
        m.tick()
        self.assertEqual(self.conn.take(), [])

    def test_gip_announce_then_init_then_input_and_guide_ack(self):
        m = self.model('xone')
        self.start(m, 0x82)
        m.tick()
        ann = self.ins(self.conn.take(), 0x82)
        self.assertEqual(len(ann), 1)
        pad = Pad()
        self.assertEqual(gip_parse(ann[0], pad)[0], EV_ANNOUNCE)
        self.clock.t += 1
        m.tick()
        self.assertEqual(self.conn.take(), [])               # nothing until power-on
        # The driver's init sequence for 045E:02EA, its sequence numbers 1, 2, ...
        packets = gip_init(0x045E, 0x02EA)
        self.assertEqual([p[0] for p in packets], [0x05, 0x05, 0x0A, 0x06])
        for i, p in enumerate(packets):
            p = bytearray(p)
            p[2] = i            # the driver numbers from 0 (2000 SP4 leg, 2026-10-23)
            self.out(m, 0x02, bytes(p))
        self.assertTrue(m.powered)
        self.assertEqual(m.counts['gip_seq_gap'], 0)
        pad, acks = Pad(), 0
        guide_seen = False
        for _ in range(2 * len(xe.PATTERN) + 4):
            m.tick()
            for data in self.ins(self.conn.take(), 0x82):
                ev, ack = gip_parse(data, pad)
                if data[0] == 0x07:
                    self.assertTrue(ev & EV_ACK)
                    guide_seen = guide_seen or bool(pad.Buttons & xe.GUIDE)
                    acks += 1
                    self.out(m, 0x02, ack, ident=40 + acks)   # the driver's acknowledgement
                    self.assertEqual(m.guide_unacked, 0.0)
                elif data[0] == 0x20:
                    want = expected(xe.PATTERN[m.step][1], 1023)
                    got = pad_dict(pad)
                    got['Buttons'] &= ~xe.GUIDE
                    want['Buttons'] &= ~xe.GUIDE
                    self.assertEqual(got, want, xe.PATTERN[m.step][0])
            self.clock.t += 0.61
        self.assertTrue(guide_seen)
        self.assertEqual(m.counts['gip_seq_gap'], 0)         # acknowledgements echo the pad's numbers
        self.assertGreaterEqual(acks, 4)                     # press and release, two cycles
        self.assertEqual(m.counts['guide_repeat'], 0)

    def test_gip_guide_repeats_until_acknowledged(self):
        m = self.model('series')
        self.start(m, 0x82)
        self.out(m, 0x02, bytes([0x05, 0x20, 1, 1, 0]))
        m.state = None
        m.step = next(i for i, s in enumerate(xe.PATTERN) if s[0] == 'Guide')
        m.step_t = self.clock.t
        m.tick()
        first = [d for d in self.ins(self.conn.take(), 0x82) if d[0] == 0x07]
        self.assertEqual(len(first), 1)
        self.assertEqual(first[0][1], 0x30)
        self.clock.t += 0.3
        m.tick()
        again = [d for d in self.ins(self.conn.take(), 0x82) if d[0] == 0x07]
        self.assertEqual(len(again), 1)
        self.assertEqual(m.counts['guide_repeat'], 1)

    def test_gip_sequence_gap_is_counted(self):
        m = self.model('xone')
        self.start(m, 0x82)
        self.out(m, 0x02, bytes([0x05, 0x20, 1, 1, 0]))
        self.out(m, 0x02, bytes([0x0A, 0x20, 3, 3, 0, 1, 0x14]))
        self.assertEqual(m.counts['gip_seq_gap'], 1)

    def test_receiver_presence_connect_led_and_pattern(self):
        m = self.model('receiver')
        for slot in range(4):
            self.start(m, 0x81 + slot * 2)
        m.tick()
        self.assertEqual(self.conn.take(), [])               # no data before the inquiry
        pads = [Pad() for _ in range(4)]
        presence = (U8 * 64)()
        n = LIB.XpX360wPresence(presence)
        for slot in range(4):
            replies = self.out(m, 0x01 + slot * 2, bytes(presence[:n]))
            data, = self.ins(replies, 0x81 + slot * 2)
            ev = x360w_parse(data, pads[slot])
            self.assertEqual(ev, EV_CONNECT if slot == 0 else 0)
        led = (U8 * 64)()
        n = LIB.XpX360wLed(led, LIB.XpLedForPlayer(0))
        self.out(m, 0x01, bytes(led[:n]))
        self.assertIn('"receiver_led"', self.trace.getvalue())
        pad = pads[0]
        seen = self.run_pattern(m, 0x81, x360w_parse, 255, pad)
        self.assertGreater(len(seen), 30)

    def receiver_up(self, m):
        for slot in range(4):
            self.start(m, 0x81 + slot * 2)
        pads = [Pad() for _ in range(4)]
        presence = (U8 * 64)()
        n = LIB.XpX360wPresence(presence)
        events = []
        for slot in range(4):
            replies = self.out(m, 0x01 + slot * 2, bytes(presence[:n]))
            data, = self.ins(replies, 0x81 + slot * 2)
            events.append(x360w_parse(data, pads[slot]))
        return pads, events

    def by_slot(self, replies):
        out = {}
        for kind, _, data in replies:
            if kind == xe.INTERRUPT:
                ep, status, length = struct.unpack('<BBH', data[:4])
                out.setdefault((ep - 0x81) // 2, []).append(data[4:])
        return out

    def test_receiver_four_pads(self):
        m = self.model('receiver', pads=4)
        pads, events = self.receiver_up(m)
        self.assertEqual(events, [EV_CONNECT] * 4)
        seen = [set() for _ in range(4)]
        for _ in range(2 * len(xe.PATTERN) + 4):
            m.tick()
            for slot, packets in self.by_slot(self.conn.take()).items():
                for data in packets:
                    ev = x360w_parse(data, pads[slot])
                    if ev & EV_DATA:
                        seen[slot].add(m.step)
                        self.assertEqual(pad_dict(pads[slot]), expected(xe.PATTERN[m.step][1], 255))
            self.clock.t += 0.61
        for slot in range(4):
            self.assertGreater(len(seen[slot]), 30, slot)

    def test_receiver_join_and_leave(self):
        m = self.model('receiver', pads=1, events=[(5.0, 2, True), (12.0, 0, False), (20.0, 0, True)])
        pads, events = self.receiver_up(m)
        self.assertEqual(events, [EV_CONNECT, 0, 0, 0])
        log = []
        for _ in range(45):
            m.tick()
            for slot, packets in self.by_slot(self.conn.take()).items():
                for data in packets:
                    ev = x360w_parse(data, pads[slot])
                    if ev & (EV_CONNECT | EV_DISCONNECT):
                        log.append((slot, 'connect' if ev & EV_CONNECT else 'disconnect', round(self.clock.t - 100.0)))
                    if ev & EV_DATA:
                        self.assertTrue(pads[slot].Present)
            self.clock.t += 0.61
        self.assertEqual([(s, e) for s, e, _ in log], [(2, 'connect'), (0, 'disconnect'), (0, 'connect')])
        self.assertTrue(all(abs(t - want) <= 1 for (_, _, t), want in zip(log, (5, 12, 20))))
        self.assertEqual([p.Present for p in pads], [1, 0, 1, 0])
        # A pad that left reports centred, nothing pressed.
        self.assertIn('"receiver_leave"', self.trace.getvalue())

    def test_reset_and_stop_receiving(self):
        m = self.model('x360')
        self.start(m, 0x81)
        m.handle(xe.STOP_INT, 3, b'\x81')
        self.assertEqual(self.conn.take(), [(xe.INT_STATUS, 3, bytes([0, 0x81]))])
        m.tick()
        self.assertEqual(self.conn.take(), [])
        m.handle(xe.RESET, 0, b'')
        self.assertEqual(m.config, 0)

    def test_refusals(self):
        m = self.model('x360')
        m.handle(xe.START_INT, 4, b'\x01')                  # an OUT endpoint
        self.assertEqual(self.conn.take(), [(xe.INT_STATUS, 4, bytes([4, 1]))])
        m.handle(xe.INTERRUPT, 5, struct.pack('<BBH', 0x01, 0, 3) + b'\x01\x03')  # length mismatch
        self.assertEqual(self.conn.take(), [(xe.INTERRUPT, 5, struct.pack('<BBH', 0x01, 4, 0))])
        m.handle(xe.CONTROL, 6, struct.pack('<BBBBHHH', 0x80, 6, 0x80, 0, 0x0300 | 9, 0x409, 255))
        (kind, ident, data), = self.conn.take()
        self.assertEqual(data[3], 4)                         # no string 9

    def test_reference_hid_pad(self):
        m = self.model('hidpad')
        m.handle(xe.CONTROL, 7, struct.pack('<BBBBHHH', 0x80, 6, 0x81, 0, 0x2200, 0, 255))
        (kind, ident, data), = self.conn.take()
        self.assertEqual(data[10:], xe.HIDPAD_REPORT_DESC)
        cfg = xe.config_descriptor('hidpad', False)
        self.assertEqual(cfg[9 + 5], 3)                      # interface class HID
        self.start(m, 0x81)
        m.tick()
        report, = self.ins(self.conn.take(), 0x81)
        self.assertEqual(report[:5], bytes([128, 128, 128, 128, 8]))

    def test_pattern_covers_every_control(self):
        names = [s[0] for s in xe.PATTERN]
        for n in ('A', 'B', 'X', 'Y', 'LB', 'RB', 'Back', 'Start', 'LS', 'RS', 'Guide',
                  'dpad up', 'dpad right', 'dpad down', 'dpad left',
                  'lx min', 'lx max', 'ly min', 'ly max', 'rx min', 'rx max', 'ry min', 'ry max',
                  'lt full', 'rt full'):
            self.assertIn(n, names)


if __name__ == '__main__':
    unittest.main(verbosity=1)
