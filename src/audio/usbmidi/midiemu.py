#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Test-only USB MIDI1 loopback over localhost usbredir; no physical device.

Original model, protocol facts from SPICE usb-redirection-protocol.txt v0.7
and USB-IF MIDI1.0. FFFF:000x are local simulation identifiers, not a claim
to a registered product. The C codec is required, not replaced in Python.

Profiles (what the guest sees):
  acms   FFFF:0001  AudioControl + MIDIStreaming, one cable each way
  multi  FFFF:0002  AudioControl + MIDIStreaming, two cables each way on one
                    endpoint pair, External jacks named "Port A"/"Port B"
  lone   FFFF:0003  a single MIDIStreaming interface and nothing else, so the
                    device's compatible id is USB\\Class_01&SubClass_03
  umtab  0582:0168  the Roland UM-ONE mk2's own TAB-mode descriptors
                    (testdata/umtab, captured 2026-10-21): USB 1.1, one cable,
                    bulk OUT 02 / IN 81, embedded jacks 40/20 hex
  quad   FFFF:0004  four cables each way on one bulk endpoint pair
  wide   FFFF:0005  two bulk endpoint pairs (01/81 and 02/82), three cables
                    each: six ports each way
  intr   FFFF:0006  one cable each way on interrupt endpoints (01/81,
                    bInterval 1), which MIDI 1.0 allows beside bulk
--speed high presents the device as High Speed (512-byte bulk endpoints,
device qualifier), for an EHCI bus; full (default) is 64-byte Full Speed.
Every cable echoes what it receives back on the same cable.
"""
import argparse
import collections
import ctypes
import hashlib
import json
import socket
import struct
import time
from pathlib import Path

# connect_device_version, ep_info_max_packet_size, 64bits_ids, 32bits_bulk_length:
# QEMU refuses a usb-redir peer on a qemu-xhci port without the last three.
CAP_64BIT_IDS, CAP_32BIT_BULK = 1 << 5, 1 << 6
CAPS = (1 << 1) | (1 << 4) | CAP_64BIT_IDS | CAP_32BIT_BULK


def device(pid, bcd_usb=0x200, cls=0):
    return struct.pack('<BBHBBBBHHHBBBB', 18, 1, bcd_usb, cls, 0, 0, 64,
                       0xffff, pid, 0x100, 1, 2, 0, 1)


DEVICE = device(1)
CONFIG = bytes([
    9, 2, 101, 0, 2, 1, 0, 128, 50,
    9, 4, 0, 0, 0, 1, 1, 0, 0,
    9, 0x24, 1, 0, 1, 9, 0, 1, 1,
    9, 4, 1, 0, 2, 1, 3, 0, 0,
    7, 0x24, 1, 0, 1, 65, 0,
    6, 0x24, 2, 1, 1, 0, 6, 0x24, 2, 2, 2, 0,
    9, 0x24, 3, 1, 3, 1, 2, 1, 0, 9, 0x24, 3, 2, 4, 1, 1, 1, 0,
    9, 5, 1, 2, 64, 0, 0, 0, 0, 5, 0x25, 1, 1, 1,
    9, 5, 0x81, 2, 64, 0, 0, 0, 0, 5, 0x25, 1, 1, 3,
])
# Same bytes as umtest.c's "multi" fixture.
MULTI = bytes([
    9, 2, 133, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 0, 1, 1, 0, 0,
    9, 0x24, 1, 0, 1, 9, 0, 1, 1,
    9, 4, 1, 0, 2, 1, 3, 0, 0,
    7, 0x24, 1, 0, 1, 67, 0,
    6, 0x24, 2, 1, 1, 0, 6, 0x24, 2, 1, 2, 0,
    9, 0x24, 3, 2, 3, 1, 1, 1, 4, 9, 0x24, 3, 2, 4, 1, 2, 1, 5,
    6, 0x24, 2, 2, 5, 4, 6, 0x24, 2, 2, 6, 5,
    9, 0x24, 3, 1, 7, 1, 5, 1, 0, 9, 0x24, 3, 1, 8, 1, 6, 1, 0,
    9, 5, 1, 2, 64, 0, 0, 0, 0, 6, 0x25, 1, 2, 1, 2,
    9, 5, 0x81, 2, 64, 0, 0, 0, 0, 6, 0x25, 1, 2, 7, 8,
])
# MIDIStreaming interface 0 alone (umtest.c's "fixture", renumbered).
LONE = bytes([
    9, 2, 83, 0, 1, 1, 0, 128, 50,
    9, 4, 0, 0, 2, 1, 3, 0, 0,
    7, 0x24, 1, 0, 1, 37, 0,
    6, 0x24, 2, 1, 1, 0, 6, 0x24, 2, 2, 2, 0,
    9, 0x24, 3, 1, 3, 1, 2, 1, 0, 9, 0x24, 3, 2, 4, 1, 1, 1, 0,
    9, 5, 1, 2, 64, 0, 0, 0, 0, 5, 0x25, 1, 1, 1,
    9, 5, 0x81, 2, 64, 0, 0, 0, 0, 5, 0x25, 1, 1, 3,
])


def usb_string(text):
    raw = text.encode('utf-16le')
    return bytes((len(raw) + 2, 3)) + raw


def ms_function(pairs, kind='bulk', names=()):
    """An AC + MIDIStreaming configuration (MIDI 1.0 section 6): pairs is
    [(out endpoint, in endpoint, cables)]; each cable has an embedded and
    an external jack each way. names: iJack string indexes for the
    external jacks, in cable order (0 = none)."""
    jacks, eps, jid, names = b'', b'', 1, list(names)
    attr = 2 if kind == 'bulk' else 3
    interval = 0 if kind == 'bulk' else 1
    for out_ep, in_ep, cables in pairs:
        emb_in, emb_out = [], []
        for c in range(cables):
            name = names.pop(0) if names else 0
            ext_in, e_in, e_out, ext_out = jid, jid + 1, jid + 2, jid + 3
            jid += 4
            jacks += bytes((6, 0x24, 2, 2, ext_in, name))            # IN jack, external
            jacks += bytes((6, 0x24, 2, 1, e_in, 0))                 # IN jack, embedded
            jacks += bytes((9, 0x24, 3, 1, e_out, 1, ext_in, 1, 0))  # OUT embedded <- external IN
            jacks += bytes((9, 0x24, 3, 2, ext_out, 1, e_in, 1, name))  # OUT external <- embedded IN
            emb_in.append(e_in)
            emb_out.append(e_out)
        eps += bytes((9, 5, out_ep, attr, 64, 0, interval, 0, 0)) + bytes((4 + cables, 0x25, 1, cables)) + bytes(emb_in)
        eps += bytes((9, 5, in_ep, attr, 64, 0, interval, 0, 0)) + bytes((4 + cables, 0x25, 1, cables)) + bytes(emb_out)
    ms_total = 7 + len(jacks) + len(eps)
    nep = 2 * len(pairs)
    body = bytes((9, 4, 0, 0, 0, 1, 1, 0, 0)) + bytes((9, 0x24, 1, 0, 1, 9, 0, 1, 1))
    body += bytes((9, 4, 1, 0, nep, 1, 3, 0, 0)) + bytes((7, 0x24, 1, 0, 1)) + struct.pack('<H', ms_total)
    body += jacks + eps
    return bytes((9, 2)) + struct.pack('<HBBBBB', 9 + len(body), 2, 1, 0, 0x80, 50) + body


FIXTURE = Path(__file__).resolve().parent / 'testdata'
UMTAB = (FIXTURE / 'umtab/CONFIG.BIN').read_bytes()
UMTAB_DEVICE = (FIXTURE / 'umtab/DEVICE.BIN').read_bytes()

Profile = collections.namedtuple('Profile', 'name pid config product cables ms_interface interfaces strings '
                                            'pairs kind vid device')
AC_MS = ((0, 1, 1), (1, 1, 3))
PROFILES = {
    'acms': Profile('acms', 1, CONFIG, 'MIDI1 loopback - SIMULATED', 1, 1, AC_MS, {},
                    ((1, 0x81, 1),), 'bulk', 0xffff, None),
    'multi': Profile('multi', 2, MULTI, 'MIDI 2x2 SIM', 2, 1, AC_MS, {4: 'Port A', 5: 'Port B'},
                     ((1, 0x81, 2),), 'bulk', 0xffff, None),
    'lone': Profile('lone', 3, LONE, 'MIDI lone SIM', 1, 0, ((0, 1, 3),), {},
                    ((1, 0x81, 1),), 'bulk', 0xffff, None),
    'umtab': Profile('umtab', 0x0168, UMTAB, 'UM-ONE layout - SIMULATED', 1, 1, AC_MS, {},
                     ((2, 0x81, 1),), 'bulk', 0x0582, UMTAB_DEVICE),
    'quad': Profile('quad', 4, ms_function(((1, 0x81, 4),), names=(4, 5, 6, 7)), 'MIDI 4x4 SIM', 4, 1, AC_MS,
                    {4: 'Port 1', 5: 'Port 2', 6: 'Port 3', 7: 'Port 4'}, ((1, 0x81, 4),), 'bulk', 0xffff, None),
    'wide': Profile('wide', 5, ms_function(((1, 0x81, 3), (2, 0x82, 3))), 'MIDI 6x6 SIM', 3, 1, AC_MS, {},
                    ((1, 0x81, 3), (2, 0x82, 3)), 'bulk', 0xffff, None),
    'intr': Profile('intr', 6, ms_function(((1, 0x81, 1),), kind='interrupt'), 'MIDI interrupt SIM', 1, 1,
                    AC_MS, {}, ((1, 0x81, 1),), 'interrupt', 0xffff, None),
}


def high_speed(config):
    """Bulk wMaxPacketSize 64 -> 512 (USB 2.0 9.6.6, Table 9-13); an
    interrupt endpoint's bInterval 1 ms -> 4 (2^3 microframes)."""
    data, off = bytearray(config), 0
    while off < len(data):
        if data[off + 1] == 5 and data[off + 3] & 3 == 2:
            struct.pack_into('<H', data, off + 4, 512)
        if data[off + 1] == 5 and data[off + 3] & 3 == 3 and data[off + 6] == 1:
            data[off + 6] = 4
        off += data[off]
    return bytes(data)


class Codec:
    def __init__(self, path, cables=1):
        self.path = Path(path).resolve(strict=True)
        self.cables = cables
        self.dll = ctypes.CDLL(str(self.path))
        self.dll.UmEncoderInit.argtypes = (ctypes.c_void_p, ctypes.c_uint)
        self.dll.UmEncoderInit.restype = ctypes.c_int
        self.dll.UmEncodeByte.argtypes = (ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p)
        self.dll.UmEncodeByte.restype = ctypes.c_int
        self.dll.UmDecodePacket.argtypes = (ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p)
        self.dll.UmDecodePacket.restype = ctypes.c_uint
        self.reset()

    def reset(self):
        self.state = []
        for cable in range(self.cables):
            state = ctypes.create_string_buffer(9)
            if not self.dll.UmEncoderInit(state, cable):
                raise ValueError('codec initialization failed')
            self.state.append(state)

    def echo(self, packets):
        if len(packets) % 4:
            raise ValueError('USB MIDI bulk OUT is not event-aligned')
        result = bytearray()
        for offset in range(0, len(packets), 4):
            payload = ctypes.create_string_buffer(3)
            packet = ctypes.create_string_buffer(packets[offset:offset + 4])
            count = self.dll.UmDecodePacket(packet, 4, (1 << self.cables) - 1, payload)
            if not count:
                raise ValueError('invalid MIDI event or unmapped cable')
            cable = packets[offset] >> 4
            for byte in payload.raw[:count]:
                out = ctypes.create_string_buffer(4)
                status = self.dll.UmEncodeByte(self.state[cable], byte, out)
                if status < 0:
                    raise ValueError('unsupported raw MIDI byte')
                if status:
                    result.extend(out.raw)
        return bytes(result)


class Pipe:
    """One OUT/IN endpoint pair with its cables and its own codec."""

    def __init__(self, out_ep, in_ep, cables, codec):
        self.out_ep, self.in_ep, self.cables, self.codec = out_ep, in_ep, cables, codec
        self.pending = collections.OrderedDict()
        self.queue = bytearray()
        self.receiving = False      # interrupt IN: QEMU is polling


class Model:
    def __init__(self, connection, trace, codec, profile=PROFILES['acms'], speed='full'):
        self.connection, self.trace, self.codec = connection, trace, codec
        self.profile, self.speed = profile, speed
        self.packet = 512 if speed == 'high' and profile.kind == 'bulk' else 64
        self.interrupt = profile.kind == 'interrupt'
        if profile.device:
            # The captured identity: strings replaced, serial index zero, USB 2.0 at High Speed.
            d = bytearray(profile.device)
            d[14:17] = bytes((1, 2, 0))
            if speed == 'high':
                struct.pack_into('<H', d, 2, 0x200)
            self.device = bytes(d)
        else:
            self.device = device(profile.pid)
        self.config = high_speed(profile.config) if speed == 'high' else profile.config
        self.pipes = []
        for i, (out_ep, in_ep, cables) in enumerate(profile.pairs):
            c = codec if i == 0 and codec.cables == cables else Codec(codec.path, cables)
            self.pipes.append(Pipe(out_ep, in_ep, cables, c))
        self.strings = {0: b'\x04\x03\x09\x04', 1: usb_string('usb-wdm-free simulation'),
                        2: usb_string(profile.product)}
        self.strings.update({k: usb_string(v) for k, v in profile.strings.items()})
        self.configuration = self.peer_caps = 0

    @property
    def pending(self):
        return self.pipes[0].pending

    @property
    def queue(self):
        return self.pipes[0].queue

    def pipe(self, endpoint):
        return next((p for p in self.pipes if endpoint in (p.out_ep, p.in_ep)), None)

    def log(self, event, **fields):
        self.trace.write(json.dumps(dict(time=time.time(), event=event, **fields)) + '\n')
        self.trace.flush()

    @property
    def ids64(self):
        return bool(self.peer_caps & CAPS & CAP_64BIT_IDS)

    @property
    def len32(self):
        return bool(self.peer_caps & CAPS & CAP_32BIT_BULK)

    def send(self, kind, ident=0, data=b''):
        self.connection.sendall(struct.pack('<IIQ' if self.ids64 else '<III', kind, len(data), ident) + data)

    def exact(self, length):
        result = bytearray()
        while len(result) < length:
            part = self.connection.recv(length - len(result))
            if not part:
                raise EOFError
            result.extend(part)
        return bytes(result)

    def info(self):
        types, intervals, interfaces = bytearray([255] * 32), bytearray(32), bytearray(32)
        packets = [0] * 32
        ms = self.profile.ms_interface
        rows = [(0, 0, 0, 64, 0), (16, 0, 0, 64, 0)]
        kind = 3 if self.interrupt else 2
        interval = (4 if self.speed == 'high' else 1) if self.interrupt else 0
        for p in self.pipes:
            rows += [(p.out_ep & 15, kind, ms, self.packet, interval), (16 + (p.in_ep & 15), kind, ms, self.packet, interval)]
        for index, kind, interface, size, iv in rows:
            types[index], interfaces[index], packets[index], intervals[index] = kind, interface, size, iv
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << 4):
            data += struct.pack('<32H', *packets)
        self.send(5, data=data)
        numbers, classes, subclasses, protocols = (bytearray(32) for _ in range(4))
        for i, (number, cls, sub) in enumerate(self.profile.interfaces):
            numbers[i], classes[i], subclasses[i] = number, cls, sub
        self.send(4, data=struct.pack('<I', len(self.profile.interfaces)) + numbers + classes + subclasses + protocols)

    def bulk_reply(self, ident, endpoint, status, data=b'', actual=None):
        count = len(data) if actual is None else actual
        header = struct.pack('<BBHI', endpoint, status, count & 0xffff, 0)
        if self.len32:
            header += struct.pack('<H', count >> 16)
        self.send(101, ident, header + data)

    def cancel(self, ident):
        for p in self.pipes:
            if ident in p.pending:
                del p.pending[ident]
                self.bulk_reply(ident, p.in_ep, 1)
                self.log('cancelled', ident=ident)

    def reset(self):
        for p in self.pipes:
            for ident in list(p.pending):
                self.cancel(ident)
            p.queue.clear()
            p.codec.reset()
            p.receiving = False

    def drain(self):
        for p in self.pipes:
            if self.interrupt:
                # Interrupt IN: one event-aligned packet per poll while QEMU
                # receives (usbredir interrupt_packet).
                while p.receiving and p.queue:
                    count = min(self.packet, len(p.queue)) & ~3
                    data = bytes(p.queue[:count])
                    del p.queue[:count]
                    self.send(103, 0, struct.pack('<BBH', p.in_ep, 0, len(data)) + data)
                    self.log('interrupt_in', endpoint=p.in_ep, length=count, sha256=hashlib.sha256(data).hexdigest())
                continue
            while p.queue and p.pending:
                ident, length = p.pending.popitem(last=False)
                # Keep each completion short versus a larger host request, while
                # preserving four-byte USB event boundaries and bounded packets.
                count = min(length, self.packet - 4, len(p.queue))
                data = bytes(p.queue[:count])
                del p.queue[:count]
                self.bulk_reply(ident, p.in_ep, 0, data)
                self.log('bulk_in', endpoint=p.in_ep, length=count, sha256=hashlib.sha256(data).hexdigest())

    def out_data(self, p, data):
        if len(p.queue) + len(data) + 4 > 1048576:
            raise ValueError('loopback exceeds one MiB bound')
        echo = p.codec.echo(data)
        p.queue.extend(echo)
        return echo

    def descriptor(self, dtype, number):
        if dtype == 1 and not number:
            return self.device
        if dtype == 2 and not number:
            return self.config
        if dtype == 3:
            return self.strings.get(number)
        if dtype == 6 and not number and self.speed == 'high':
            return struct.pack('<BBHBBBBBB', 10, 6, 0x200, 0, 0, 0, 64, 1, 0)
        if dtype == 7 and not number and self.speed == 'high':
            return bytes((9, 7)) + self.profile.config[2:]
        return None

    def control(self, ident, packet):
        if len(packet) < 10:
            raise ValueError('short control header')
        ep, request, rt, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        data, status = b'', 4
        if len(packet) != 10 + (0 if rt & 128 else length):
            raise ValueError('control length mismatch')
        if request == 6 and rt == 0x80:
            data = self.descriptor(value >> 8, value & 255)
            status, data = (0, data) if data is not None else (4, b'')
        elif request == 0 and rt in (0x80, 0x81, 0x82):
            status, data = 0, b'\0\0'
        elif request == 1 and rt == 2 and value == 0 and self.pipe(index):
            status = 0
        elif request == 8 and rt == 0x80:
            status, data = 0, bytes((self.configuration,))
        elif request == 10 and rt == 0x81 and index in (0, 1):
            status, data = 0, b'\0'
        data = data[:length]
        actual = len(data) if rt & 128 else (0 if status else length)
        self.send(100, ident, struct.pack('<BBBBHHH', ep, request, rt, status, value, index, actual) + data)
        self.log('control', request=request, request_type=rt, value=value, status=status, actual=actual)

    def handle(self, kind, ident, packet):
        if kind == 3:
            if packet:
                raise ValueError('invalid reset')
            self.reset()
            self.configuration = 0
        elif kind in (6, 7):
            if len(packet) != (1 if kind == 6 else 0):
                raise ValueError('invalid configuration')
            status = 0
            if kind == 6:
                if packet[0] not in (0, 1):
                    status = 4
                else:
                    self.reset()
                    self.configuration = packet[0]
                    self.info()
                    self.log('configuration', value=packet[0])
            self.send(8, ident, bytes((status, self.configuration)))
        elif kind in (9, 10):
            if len(packet) != (2 if kind == 9 else 1):
                raise ValueError('invalid alternate')
            known = [i[0] for i in self.profile.interfaces]
            status = 0 if packet[0] in known and (kind == 10 or not packet[1]) else 4
            if not status and kind == 9:
                if packet[0] == self.profile.ms_interface:
                    self.reset()
                self.info()
            self.send(11, ident, bytes((status, packet[0], 0)))
        elif kind == 21:
            if packet:
                raise ValueError('invalid cancellation')
            self.cancel(ident)
        elif kind == 100:
            self.control(ident, packet)
        elif kind == 101 and not self.interrupt:
            if len(packet) < 8:
                raise ValueError('short bulk header')
            size = 10 if self.len32 else 8
            if len(packet) < size:
                raise ValueError('short bulk header')
            ep, _, length, stream = struct.unpack('<BBHI', packet[:8])
            if self.len32:
                length |= struct.unpack('<H', packet[8:10])[0] << 16
            packet = packet[:8] + packet[size:]
            p = self.pipe(ep)
            if stream or not self.configuration or p is None:
                self.bulk_reply(ident, ep, 4)
            elif ep == p.out_ep:
                if length != len(packet)-8:
                    raise ValueError('OUT length mismatch')
                data = self.out_data(p, packet[8:])
                self.bulk_reply(ident, ep, 0, actual=length)
                self.log('bulk_out', endpoint=ep, length=length, echo_length=len(data),
                         sha256=hashlib.sha256(packet[8:]).hexdigest())
                self.drain()
            else:
                if len(packet) != 8 or not length or length % 4 or ident in p.pending or len(p.pending)>=256:
                    raise ValueError('invalid/bounded IN request')
                p.pending[ident] = length
                self.drain()
        elif kind in (15, 16) and self.interrupt:
            if len(packet) != 1:
                raise ValueError('interrupt header')
            p = self.pipe(packet[0])
            ok = p is not None and packet[0] == p.in_ep and self.configuration
            if ok:
                p.receiving = kind == 15
            self.send(17, ident, bytes((0 if ok else 4, packet[0])))
            self.drain()
        elif kind == 103 and self.interrupt:
            if len(packet) < 4:
                raise ValueError('short interrupt header')
            ep, _, length = struct.unpack('<BBH', packet[:4])
            p = self.pipe(ep)
            if length != len(packet) - 4 or p is None or ep != p.out_ep:
                raise ValueError('interrupt OUT framing')
            data = self.out_data(p, packet[4:])
            self.send(103, ident, struct.pack('<BBH', ep, 0, length))
            self.log('interrupt_out', endpoint=ep, length=length, echo_length=len(data))
            self.drain()
        else:
            raise ValueError(f'unsupported usbredir message {kind}')

    def run(self):
        self.send(0, data=b'usb-wdm-free MIDI1 SIMULATED'.ljust(64, b'\0') + struct.pack('<I', CAPS))
        kind, length, ident = struct.unpack('<III', self.exact(12))
        if kind or ident or length < 64 or length > 4096:
            raise ValueError('invalid hello')
        hello = self.exact(length)
        self.peer_caps = struct.unpack('<I', hello[64:68])[0] if length>=68 else 0
        self.info()
        data = struct.pack('<BBBBHH', 2 if self.speed == 'high' else 1,
                           0, 0, 0, self.profile.vid, self.profile.pid)
        if self.peer_caps & 2:
            data += struct.pack('<H', 0x100)
        self.send(1, data=data)
        self.log('connected', model='SIMULATED MIDI1 loopback', profile=self.profile.name, speed=self.speed,
                 vid=f'{self.profile.vid:04x}', pid=f'{self.profile.pid:04x}',
                 codec_sha256=hashlib.sha256(self.codec.path.read_bytes()).hexdigest())
        while True:
            if self.ids64:
                kind, length, ident = struct.unpack('<IIQ', self.exact(16))
            else:
                kind, length, ident = struct.unpack('<III', self.exact(12))
            if length > 1048586:
                raise ValueError('message too large')
            self.handle(kind, ident, self.exact(length))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=48441)
    ap.add_argument('--profile', choices=sorted(PROFILES), default='acms')
    ap.add_argument('--speed', choices=('full', 'high'), default='full')
    ap.add_argument('--codec', type=Path, default=Path(__file__).resolve().parents[3] / 'out/audio/usbmidi/host/umcodec.dll')
    ap.add_argument('--log', type=Path, required=True)
    ap.add_argument('--pid', type=lambda v: int(v, 16), help='product id instead of the profile (FFFF:0001/0002 match the test INF)')
    args = ap.parse_args()
    profile = PROFILES[args.profile]
    if args.pid is not None:
        profile = profile._replace(pid=args.pid)
    codec = Codec(args.codec, profile.cables)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'SIMULATED MIDI1 {args.profile}/{args.speed} listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            model = Model(connection, trace, codec, profile, args.speed)
            try:
                model.run()
            except (EOFError, ConnectionResetError, BrokenPipeError, ConnectionAbortedError):
                model.log('disconnected')


if __name__ == '__main__':
    main()
