#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Test-only USB keyboard simulator with media keys, exported to QEMU over
localhost usbredir (docs/design/mediakeys.md, the readings).

A boot keyboard (interface 03/01/01, the boot report of HID 1.11 appendix
B.1, a key array of usages 0..255 so that the Keyboard page's own Mute,
Volume Up and Volume Down, 7F..81, can be sent) plus, per profile, the HID
Consumer page (0x0C) "Consumer Control" collection that keyboards with
volume and media keys carry. The descriptors are this file's own, written
from the HID 1.11 specification and the HID Usage Tables 1.12; the strings
say SIMULATED.

Profiles (all Full Speed, VID FFFF):

  boot     the boot keyboard alone, no consumer keys (FFFF:0040)
  twoif    interface 0 the boot keyboard; interface 1 (03/00/00) a
           Consumer Control collection, no report ID, one 16-bit usage
           array (0..0x3FF) (FFFF:0041)
  twoifid  as twoif, interface 1 with report IDs: 1 Consumer Control
           (the same array), 2 System Control (Generic Desktop 0x80:
           Power Down, Sleep, Wake Up, bit fields) (FFFF:0042)
  oneif    one interface (03/01/01) with report IDs: 1 the keyboard
           (8 bytes after the ID), 2 Consumer Control; in the boot
           protocol the keyboard report has no ID and consumer keys are
           not sent (FFFF:0043)
  bitmap   as twoif, the Consumer Control collection as 16 one-bit
           variable fields (Volume Increment, Volume Decrement, Mute,
           Play/Pause, Scan Next, Scan Previous, Stop, AL Email Reader,
           AC Home, AC Back, AC Forward, AC Refresh, AC Search,
           AC Bookmarks, AL Calculator, AL Local Machine Browser; no
           bit for AL Consumer Control Configuration or AC Stop)
           (FFFF:0044)

--pid gives the device another product id, so that a test INF can name it.

The model keeps the logical state (the keys held now) and encodes each
report from it in the active protocol when it sends or answers. GET_REPORT
answers that input report and the last LED output report; an undeclared
report type or ID stalls. SET_IDLE is kept per report ID; a non-zero
duration reports the held state again each time it passes with no change,
from the moment the host starts polling (HID 1.11 7.2.4).

Input is driven over a second TCP port (--ctl), one JSON object a line:
  {"key": "volup"}          press and release a consumer key by name
                            (CONSUMER below), held "hold" ms (default 100)
  {"usage": 233}            press and release a consumer usage by number
  {"kbd": 4}                press and release a Keyboard page usage
                            (4 = a; 127/128/129 = the Keyboard page's
                            Mute, Volume Up, Volume Down)
  {"sys": "sleep"}          press and release a System Control usage
                            (twoifid only)
  {"hold": 500}             with any of the above: the hold time in ms
  {"quit": 1}               end the simulation (the test host's unplug)
The answer is a JSON line: {"ok": true, "sent": n}.

No physical USB device is accessed. The usbredir framing follows this
repository's acmemu.py, xpademu.py and absemu.py.
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
ENDPOINT_INTERVAL = 0.010               # bInterval 10 of every interrupt IN endpoint (Full Speed, ms)

# Consumer page usages (HID Usage Tables 1.12, section 15).
CONSUMER = {
    'volup': 0xE9, 'voldown': 0xEA, 'mute': 0xE2, 'play': 0xCD, 'next': 0xB5, 'prev': 0xB6,
    'stop': 0xB7, 'mail': 0x18A, 'home': 0x223, 'back': 0x224, 'fwd': 0x225, 'refresh': 0x227,
    'bstop': 0x226, 'search': 0x221, 'favs': 0x22A, 'calc': 0x192, 'mycomp': 0x194, 'media': 0x183,
}
# The bitmap profile's field order (bit 0 first).
BITMAP = ['volup', 'voldown', 'mute', 'play', 'next', 'prev', 'stop', 'mail', 'home', 'back', 'fwd',
          'refresh', 'search', 'favs', 'calc', 'mycomp']
SYSTEM = {'power': 0, 'sleep': 1, 'wake': 2}       # bit numbers of Power Down 81, Sleep 82, Wake Up 83


def h(text):
    return bytes.fromhex(text.replace(' ', ''))


# ---------------------------------------------------------------- descriptors

# Keyboard body: modifiers E0..E7, reserved byte, LED output report (5
# bits + 3 pad), six-key array of usages 0..255.
KBD_BODY = ('05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02 '
            '95 01 75 08 81 01 '
            '05 08 19 01 29 05 95 05 75 01 91 02 95 01 75 03 91 01 '
            '05 07 19 00 29 ff 15 00 26 ff 00 95 06 75 08 81 00')
KBD = h('05 01 09 06 a1 01 ' + KBD_BODY + ' c0')
KBD_ID1 = h('05 01 09 06 a1 01 85 01 ' + KBD_BODY + ' c0')
# Consumer Control, one 16-bit usage array 0..0x3FF.
CC_ARRAY = '05 0c 09 01 a1 01 {rid}15 00 26 ff 03 19 00 2a ff 03 75 10 95 01 81 00 c0'
CC = h(CC_ARRAY.format(rid=''))
SYSCTL = '05 01 09 80 a1 01 85 02 19 81 29 83 15 00 25 01 75 01 95 03 81 02 95 05 81 01 c0'
CC_SYS = h(CC_ARRAY.format(rid='85 01 ') + ' ' + SYSCTL)
ONEIF = KBD_ID1 + h(CC_ARRAY.format(rid='85 02 '))
CC_BITMAP = h('05 0c 09 01 a1 01 15 00 25 01 75 01 95 10 ' +
              ' '.join('0a %02x %02x' % (CONSUMER[n] & 255, CONSUMER[n] >> 8) for n in BITMAP) +
              ' 81 02 c0')

PROFILES = {
    'boot': dict(pid=0x0040, ifaces=[dict(sub=1, proto=1, desc=KBD, packet=8)],
                 product='USB keyboard, no media keys'),
    'twoif': dict(pid=0x0041, ifaces=[dict(sub=1, proto=1, desc=KBD, packet=8),
                                      dict(sub=0, proto=0, desc=CC, packet=4)],
                  product='USB keyboard, media keys on interface 1'),
    'twoifid': dict(pid=0x0042, ifaces=[dict(sub=1, proto=1, desc=KBD, packet=8),
                                        dict(sub=0, proto=0, desc=CC_SYS, packet=4)],
                    product='USB keyboard, media and system keys on interface 1'),
    'oneif': dict(pid=0x0043, ifaces=[dict(sub=1, proto=1, desc=ONEIF, packet=9)],
                  product='USB keyboard, media keys by report ID'),
    'bitmap': dict(pid=0x0044, ifaces=[dict(sub=1, proto=1, desc=KBD, packet=8),
                                       dict(sub=0, proto=0, desc=CC_BITMAP, packet=4)],
                   product='USB keyboard, media keys as bits'),
}


def device_descriptor(p):
    return struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x0110, 0, 0, 0, 8, 0xFFFF, p['pid'], 0x0100, 1, 2, 3, 1)


def hid_descriptor(i):
    d = i['desc']
    return bytes([9, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, len(d) & 255, len(d) >> 8])


def config_descriptor(p):
    body = b''
    for n, i in enumerate(p['ifaces']):
        body += bytes([9, 4, n, 0, 1, 3, i['sub'], i['proto'], 0]) + hid_descriptor(i)
        body += struct.pack('<BBBBHB', 7, 5, 0x81 + n, 3, i['packet'], 10)
    total = 9 + len(body)
    return struct.pack('<BBHBBBBB', 9, 2, total, len(p['ifaces']), 1, 0, 0xA0, 50) + body


def string(text):
    data = text.encode('utf-16le')
    return bytes((len(data) + 2, 3)) + data


# ---------------------------------------------------------------- the model


class Model:
    def __init__(self, connection, trace, profile):
        self.connection, self.trace, self.profile = connection, trace, profile
        self.p = PROFILES[profile]
        self.device = device_descriptor(self.p)
        self.config_desc = config_descriptor(self.p)
        self.strings = {0: b'\x04\x03\x09\x04', 1: string('usb-wdm-free simulation'),
                        2: string(self.p['product'] + ' - SIMULATED'), 3: string('SIM0001')}
        self.peer_caps = 0
        self.config = 0
        self.receiving = set()
        self.protocol = [1] * len(self.p['ifaces'])       # report protocol (HID 1.11 7.2.6)
        self.idle = {}                  # (interface, report id) -> duration in 4 ms units (HID 1.11 7.2.4)
        self.keys = dict(kbd=0, cc=0, sys=None)   # the logical state: what is held now
        self.deadline = {}              # (interface, report id) -> time of the next idle report
        self.leds = {}                  # interface -> the LED bits of the last output report
        self.last_sent = {}             # (interface, report id) -> time of its last delivered report
        self.pending = set()            # (interface, report id) changed while not polled
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
            types[index], packets[index] = 0, 8
        if self.config == 1:
            for n, i in enumerate(self.p['ifaces']):
                index = 0x10 + 1 + n
                types[index], intervals[index], interfaces[index] = 3, 10, n
                packets[index] = i['packet']
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << 4):
            data += struct.pack('<32H', *packets)
        self.send(EP_INFO, data=data)
        numbers, classes, subs, protos = bytearray(32), bytearray(32), bytearray(32), bytearray(32)
        for n, i in enumerate(self.p['ifaces']):
            numbers[n], classes[n], subs[n], protos[n] = n, 3, i['sub'], i['proto']
        self.send(IFACE_INFO, data=struct.pack('<I', len(self.p['ifaces'])) + numbers + classes + subs + protos)

    # Report IDs (HID 1.11 5.6): which input IDs each interface declares and
    # the keyboard's output (LED) report ID, both by the active protocol.
    def input_ids(self, n):
        if self.profile == 'oneif':
            return (1, 2) if self.protocol[0] == 1 else (0,)
        if n == 1 and self.profile == 'twoifid':
            return (1, 2)
        return (0,)

    def output_id(self, n):
        if n != 0:
            return None
        return 1 if self.profile == 'oneif' and self.protocol[0] == 1 else 0

    def usage16(self):
        return struct.pack('<H', self.keys['cc'])

    def encode(self, n, rid):
        """The input report (interface n, report ID rid) of the state held now, in the active protocol."""
        if n == 0:
            keys = bytes([0, 0, self.keys['kbd'] & 255, 0, 0, 0, 0, 0])
            if self.profile == 'oneif' and self.protocol[0] == 1:
                return b'\x01' + keys if rid == 1 else b'\x02' + self.usage16()
            return keys
        if self.profile == 'twoifid':
            if rid == 2:
                bit = self.keys['sys']
                return bytes([2, (1 << bit) if bit is not None else 0])
            return b'\x01' + self.usage16()
        if self.profile == 'bitmap':
            bits = 0
            for i, name in enumerate(BITMAP):
                if self.keys['cc'] and CONSUMER[name] == self.keys['cc']:
                    bits |= 1 << i
            return struct.pack('<H', bits)
        return self.usage16()

    def target(self, kind, value):
        """The report (interface, report ID) a key of this kind changes; refuses keys the profile lacks."""
        if kind == 'kbd':
            return 0, (1 if self.profile == 'oneif' and self.protocol[0] == 1 else 0)
        if kind == 'cc':
            if self.profile == 'boot':
                raise ValueError('profile boot has no consumer keys')
            if self.profile == 'oneif':
                if self.protocol[0] == 0:
                    raise ValueError('boot protocol: no consumer report')
                return 0, 2
            if self.profile == 'bitmap' and value and value not in [CONSUMER[b] for b in BITMAP]:
                raise ValueError('usage %#x has no bit in the bitmap profile' % value)
            return 1, (1 if self.profile == 'twoifid' else 0)
        if self.profile != 'twoifid':
            raise ValueError('system keys only in twoifid')
        return 1, 2

    def idle_of(self, n, rid):
        return self.idle.get((n, rid), self.idle.get((n, 0), 0))

    def period(self, n, rid):
        """The idle period in seconds, never below the endpoint's 10 ms interval (HID 1.11 7.2.4); 0 = off."""
        d = self.idle_of(n, rid)
        return max(d * 0.004, ENDPOINT_INTERVAL) if d else 0

    def schedule(self, n, rid, base):
        p = self.period(n, rid)
        self.deadline[(n, rid)] = base + p if p else None

    def send_input(self, n, rid):
        """Send one input report of the held state; the state itself never depends on delivery.
        A report that cannot be delivered now stays pending and goes out when polling resumes."""
        endpoint, data = 0x81 + n, self.encode(n, rid)
        now = time.time()
        self.schedule(n, rid, now)
        if self.config != 1 or endpoint not in self.receiving:
            self.pending.add((n, rid))
            self.log('report_dropped', endpoint=endpoint, data=data.hex())
            return 0
        self.pending.discard((n, rid))
        self.last_sent[(n, rid)] = now
        self.send(INTERRUPT, 0, struct.pack('<BBH', endpoint, 0, len(data)) + data)
        self.sent += 1
        self.log('report', endpoint=endpoint, data=data.hex())
        return 1

    def polling_started(self, n):
        """START_INT: deliver what changed while not polled, then start the idle timing."""
        for rid in self.input_ids(n):
            if (n, rid) in self.pending:
                self.send_input(n, rid)
            else:
                self.schedule(n, rid, time.time())

    def idle_tick(self):
        """At each idle duration with no change, report the held state again (HID 1.11 7.2.4); 0 = only on change."""
        now = time.time()
        for n in range(len(self.p['ifaces'])):
            if 0x81 + n not in self.receiving:
                continue
            for rid in self.input_ids(n):
                if not self.period(n, rid):
                    continue
                due = self.deadline.get((n, rid))
                if due is None:
                    self.schedule(n, rid, now)
                elif now >= due:
                    self.send_input(n, rid)

    def idle_wait(self):
        """Time to the next idle report (at most 1 s)."""
        dues = [t for t in self.deadline.values() if t is not None]
        if not any(self.idle.values()):
            return 1.0
        if not dues:
            return 0.004
        return max(0.001, min(1.0, min(dues) - time.time()))

    def command(self, cmd):
        hold = float(cmd.get('hold', 100)) / 1000.0
        if 'key' in cmd or 'usage' in cmd:
            kind, value = 'cc', CONSUMER[cmd['key']] if 'key' in cmd else int(cmd['usage'])
        elif 'kbd' in cmd:
            kind, value = 'kbd', int(cmd['kbd'])
        elif 'sys' in cmd:
            kind, value = 'sys', SYSTEM[cmd['sys']]
        else:
            raise ValueError('no key in command')
        n_if, rid = self.target(kind, value)
        self.keys[kind] = value
        sent = self.send_input(n_if, rid)
        self.pump(hold)
        self.keys[kind] = None if kind == 'sys' else 0
        n_if, rid = self.target(kind, 0)        # the protocol may have changed during the hold
        sent += self.send_input(n_if, rid)
        self.log('command', cmd=cmd, sent=sent)
        return sent

    def pump(self, seconds):
        """Serve the host for a while (a held key must not stall control traffic)."""
        end = time.time() + seconds
        while True:
            left = end - time.time()
            if left <= 0:
                return
            ready, _, _ = select.select([self.connection], [], [], min(left, self.idle_wait()))
            self.idle_tick()
            if ready:
                chunk = self.connection.recv(65536)
                if not chunk:
                    raise EOFError
                self.rx.extend(chunk)
                self.take()

    def control(self, ident, packet):
        endpoint, request, reqtype, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        payload, result, status = packet[10:], b'', 0
        n = index & 255
        ifaces = self.p['ifaces']
        if reqtype == 0x81 and request == 6 and value >> 8 in (0x21, 0x22):
            if n >= len(ifaces):
                status = 4
            elif value >> 8 == 0x22:
                result = ifaces[n]['desc']
                self.log('hid_report_descriptor', interface=n, requested=length)
            else:
                result = hid_descriptor(ifaces[n])
        elif reqtype & 0x60 == 0:
            if request == 6 and reqtype == 0x80:
                dtype, number = value >> 8, value & 255
                result = self.device if dtype == 1 and number == 0 else \
                    self.config_desc if dtype == 2 and number == 0 else \
                    self.strings.get(number) if dtype == 3 else None
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
            elif request == 11:
                pass
            else:
                status = 4
        elif n >= len(ifaces):
            status = 4
        elif reqtype == 0x21 and request == 0x0A:                    # SET_IDLE
            if (value & 255) == 0:
                for key in [k for k in self.idle if k[0] == n]:
                    del self.idle[key]
            self.idle[(n, value & 255)] = value >> 8
            # The affected reports only, timed from their last report (HID 1.11 7.2.4).
            for rid in self.input_ids(n):
                if (value & 255) in (0, rid):
                    self.schedule(n, rid, self.last_sent.get((n, rid), time.time()))
        elif reqtype == 0x21 and request == 0x0B:                    # SET_PROTOCOL
            self.protocol[n] = value & 1
            for key in [k for k in self.deadline if k[0] == n]:
                del self.deadline[key]
            # A change still pending is re-sent as every report the new protocol has.
            if any(k[0] == n for k in self.pending):
                self.pending = {k for k in self.pending if k[0] != n} | {(n, r) for r in self.input_ids(n)}
            self.log('set_protocol', interface=n, protocol=value & 1)
        elif reqtype == 0xA1 and request == 0x03:                    # GET_PROTOCOL
            result = bytes([self.protocol[n]])
        elif reqtype == 0xA1 and request == 0x02:                    # GET_IDLE
            rid = value & 255
            result = bytes([self.idle.get((n, rid), self.idle.get((n, 0), 0))])
        elif reqtype == 0x21 and request == 0x09:                    # SET_REPORT: the LED output report only
            rtype, rid = value >> 8, value & 255
            if rtype == 2 and rid == self.output_id(n) and payload:
                self.leds[n] = payload[-1]
            else:
                status = 4
            self.log('set_report', interface=n, type=rtype, id=rid, data=payload.hex(), status=status)
        elif reqtype == 0xA1 and request == 0x01:                    # GET_REPORT: the current state
            rtype, rid = value >> 8, value & 255
            if rtype == 1 and rid in self.input_ids(n):
                result = self.encode(n, rid)
            elif rtype == 2 and rid == self.output_id(n):
                result = (bytes([rid]) if rid else b'') + bytes([self.leds.get(n, 0)])
            else:
                status = 4                                           # no such report: stall
            self.log('get_report', interface=n, type=rtype, id=rid, length=length, status=status)
        else:
            status = 4
            self.log('control_unknown', request=request, request_type=reqtype, value=value, index=index)
        result = result[:length]
        actual = len(result) if reqtype & 0x80 else (0 if status else length)
        self.send(CONTROL, ident, struct.pack('<BBBBHHH', endpoint, request, reqtype, status, value, index, actual)
                  + result)
        self.log('control', request=request, request_type=reqtype, value=value, index=index,
                 requested=length, actual=actual, status=status)

    def handle(self, kind, ident, packet):
        if kind == RESET:
            self.config, self.receiving = 0, set()
            self.protocol = [1] * len(self.p['ifaces'])
            self.idle, self.deadline, self.leds, self.last_sent = {}, {}, {}, {}
            self.pending = set()
            self.keys = dict(kbd=0, cc=0, sys=None)
            self.log('reset')
        elif kind in (SET_CONFIG, GET_CONFIG):
            if kind == SET_CONFIG:
                if len(packet) != 1 or packet[0] not in (0, 1):
                    self.send(CONFIG_STATUS, ident, b'\x04\x00')
                    return
                self.config, self.receiving = packet[0], set()
                self.info()
                self.log('configuration', value=self.config)
            self.send(CONFIG_STATUS, ident, bytes((0, self.config)))
        elif kind == SET_ALT:
            number, alt = packet[0], packet[1]
            ok = number < len(self.p['ifaces']) and alt == 0
            self.send(ALT_STATUS, ident, bytes((0 if ok else 4, number, 0)))
        elif kind == GET_ALT:
            self.send(ALT_STATUS, ident, bytes((0, packet[0], 0)))
        elif kind in (START_INT, STOP_INT):
            endpoint = packet[0]
            ok = self.config == 1 and 0x81 <= endpoint < 0x81 + len(self.p['ifaces'])
            if ok and kind == START_INT:
                self.receiving.add(endpoint)
            else:
                self.receiving.discard(endpoint)
            self.send(INT_STATUS, ident, bytes((0 if ok else 4, endpoint)))
            self.log('interrupt_receiving', endpoint=endpoint, on=kind == START_INT, ok=bool(ok))
            if ok and kind == START_INT:
                self.polling_started(endpoint - 0x81)
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
        self.send(HELLO, data=b'usb-wdm-free media keys simulation 0.1'.ljust(64, b'\x00') + struct.pack('<I', CAPS))

    def connect(self):
        data = struct.pack('<BBBBHH', 1, 0, 0, 0, 0xFFFF, self.p['pid'])
        if self.peer_caps & (1 << 1):
            data += struct.pack('<H', 0x0100)
        self.info()
        self.send(CONNECT, data=data)
        self.log('connected', model='SIMULATED keyboard', profile=self.profile)

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
    ap.add_argument('--profile', choices=sorted(PROFILES), default='twoif')
    ap.add_argument('--pid', type=lambda v: int(v, 0), help='a product id in place of the profile one (a test INF names it)')
    args = ap.parse_args()
    if args.pid is not None:
        PROFILES[args.profile] = dict(PROFILES[args.profile], pid=args.pid)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server, socket.socket() as ctl:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        ctl.bind(('127.0.0.1', args.ctl))
        ctl.listen(4)
        print(f'media keys simulation ({args.profile}) usbredir 127.0.0.1:{args.port}, '
              f'control 127.0.0.1:{args.ctl}', flush=True)
        connection, _ = server.accept()
        model = Model(connection, trace, args.profile)
        clients = {}
        try:
            model.handshake()
            while True:
                rl = [connection, ctl] + list(clients)
                ready, _, _ = select.select(rl, [], [], model.idle_wait())
                model.idle_tick()
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
                        try:
                            chunk = r.recv(4096)
                        except ConnectionError:
                            chunk = b''
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
                                    r.sendall(b'{"ok": true, "quit": true}\n')
                                    raise EOFError
                                n = model.command(cmd)
                                r.sendall(json.dumps(dict(ok=True, sent=n)).encode() + b'\n')
                            except (ValueError, KeyError) as e:
                                r.sendall(json.dumps(dict(ok=False, error=str(e))).encode() + b'\n')
        except (EOFError, ConnectionResetError, BrokenPipeError, ConnectionAbortedError):
            model.log('disconnected', sent=model.sent)


if __name__ == '__main__':
    main()
