#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Test-only CDC-ACM echo device, exported to QEMU over localhost usbredir.

Original implementation from the USB chapter-9/CDC ACM protocol facts and
SPICE's usb-redirection-protocol.txt v0.7. No library/sample code copied.
The test VID/PID intentionally exercises USBCDC.INF's Uno model; the product
string clearly identifies simulation. It does not emulate Arduino firmware,
UART timing, DTR reset, or a Pico. No physical USB device is accessed.
"""
import argparse
import collections
import hashlib
import json
import socket
import struct
import time
from pathlib import Path

# usbredir capabilities: device version, endpoint maximum packet size,
# 64-bit packet ids and 32-bit bulk lengths. QEMU refuses a peer without the
# last three on an xHCI port ("usb-redir-host lacks capabilities needed for
# use with XHCI"). A capability is in use when both sides announce it.
CAP_64BIT_IDS = 1 << 5
CAP_32BIT_BULK = 1 << 6
CAPS = (1 << 1) | (1 << 4) | CAP_64BIT_IDS | CAP_32BIT_BULK

# Profiles: 'uno' is one CDC-ACM function on its own (device class 02), as
# the Uno R3's 16U2 firmware presents it. 'pico' is the shape of the
# pico-sdk stdio device: device class EF/02/01, an Interface Association
# Descriptor over the CDC pair, and a third, vendor-class interface with no
# endpoints (its reset interface). Only the descriptor shape is modelled;
# no Pico firmware behaviour (BOOTSEL, the 1200-baud touch) is. 'dual'
# is the same IDs and shape with two CDC-ACM functions, each behind its
# own IAD (interfaces 0-1 and 2-3), as a pico-sdk/TinyUSB build with two
# CDC ports presents itself; the reset interface is 4. Each function
# echoes on its own and has its own line coding, line state and
# SERIAL_STATE notifications.
PROFILES = {'uno': (0x2341, 0x0043), 'pico': (0x2E8A, 0x000A), 'dual': (0x2E8A, 0x000A)}
# Per function: (communication interface, data interface, interrupt IN,
# bulk OUT, bulk IN).
FUNCTIONS = ((0, 1, 0x83, 0x02, 0x81), (2, 3, 0x86, 0x05, 0x84))


def device_descriptor(profile):
    vid, pid = PROFILES[profile]
    cls, sub, proto = (0xEF, 2, 1) if profile in ('pico', 'dual') else (2, 0, 0)
    return struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x0200, cls, sub, proto, 64,
                       vid, pid, 0x0100, 1, 2, 0, 1)


def config_descriptor(profile, bulk, interval, kind=2):
    """Configuration descriptor; kind 7 is the other-speed configuration."""
    body = bytearray()
    functions = FUNCTIONS[:2 if profile == 'dual' else 1]
    interfaces = 2 * len(functions)
    for comm, data, intr, out, inp in functions:
        if profile in ('pico', 'dual'):
            body += bytes([8, 0x0B, comm, 2, 2, 2, 0, 0])   # IAD over comm-data
        body += bytes([
            9, 4, comm, 0, 1, 2, 2, 1, 0,
            5, 0x24, 0, 0x10, 1,
            4, 0x24, 2, 6,
            5, 0x24, 6, comm, data,
            5, 0x24, 1, 0, data,
            7, 5, intr, 3, 16, 0, interval,
            9, 4, data, 0, 2, 10, 0, 0, 0,
            7, 5, out, 2, bulk & 255, bulk >> 8, 0,
            7, 5, inp, 2, bulk & 255, bulk >> 8, 0,
        ])
    if profile in ('pico', 'dual'):
        body += bytes([9, 4, interfaces, 0, 0, 0xFF, 0, 1, 0])   # vendor reset interface
        interfaces += 1
    total = 9 + len(body)
    return bytes([9, kind, total & 255, total >> 8, interfaces, 1, 0, 0x80, 50]) + bytes(body)


def qualifier(profile):
    device = device_descriptor(profile)
    return bytes([10, 6, 0x00, 0x02, device[4], device[5], device[6], 64, 1, 0])


# Full Speed defaults, kept as module names for the host tests.
DEVICE = device_descriptor('uno')
CONFIG = config_descriptor('uno', 64, 16)


def string(text):
    data = text.encode('utf-16le')
    return bytes((len(data) + 2, 3)) + data


STRINGS = {0: b'\x04\x03\x09\x04', 1: string('usb-wdm-free simulation'),
           2: string('CDC ACM echo - SIMULATED device')}


class Fn:
    """One CDC-ACM function's state."""

    def __init__(self, index):
        self.index = index
        self.comm, self.data, self.intr, self.out, self.inp = FUNCTIONS[index]
        self.pending_break = False
        self.notify_q = collections.deque()
        self.told_state = False
        self.last_state, self.sent_period = 0, False
        self.line = struct.pack('<IBBB', 115200, 0, 0, 8)
        self.lines = 0
        self.interrupt = False
        self.echo = collections.deque()
        self.queued = 0

    def clear(self):
        self.notify_q.clear()
        self.told_state = False
        self.echo.clear()
        self.queued = 0


def _fn0(name):
    """A Model attribute that is function 0's (the single-function API the
    host tests and older callers use)."""
    return property(lambda self: getattr(self.fns[0], name),
                    lambda self, value: setattr(self.fns[0], name, value))


class Model:
    pending_break, notify_q, told_state = _fn0('pending_break'), _fn0('notify_q'), _fn0('told_state')
    last_state, sent_period, line = _fn0('last_state'), _fn0('sent_period'), _fn0('line')
    lines, interrupt, echo, queued = _fn0('lines'), _fn0('interrupt'), _fn0('echo'), _fn0('queued')

    def __init__(self, connection, trace, speed='full', profile='uno', mirror=False, caps=CAPS,
                 flush_zlp=True):
        self.connection, self.trace = connection, trace
        self.speed, self.profile, self.mirror = speed, profile, mirror
        self.flush_zlp = flush_zlp
        self.high = speed == 'high'
        self.bulk_max = 512 if self.high else 64
        # Interrupt bInterval: 16 ms at Full Speed; 2^(8-1) microframes at High.
        self.device = device_descriptor(profile)
        self.config_desc = config_descriptor(profile, self.bulk_max, 8 if self.high else 16)
        self.other = config_descriptor(profile, 64 if self.high else 512, 16 if self.high else 8, 7)
        self.fns = [Fn(k) for k in range(2 if profile == 'dual' else 1)]
        self.interfaces = 2 * len(self.fns) + (1 if profile in ('pico', 'dual') else 0)
        self.config = 0
        self.pending = collections.OrderedDict()
        self.peer_caps = 0
        self.caps = caps

    def fn_by(self, field, value):
        return next((f for f in self.fns if getattr(f, field) == value), None)

    def log(self, event, **fields):
        self.trace.write(json.dumps(dict(time=time.time(), event=event, **fields)) + '\n')
        self.trace.flush()

    def send(self, kind, ident=0, data=b''):
        header = '<IIQ' if self.ids64 else '<III'
        self.connection.sendall(struct.pack(header, kind, len(data), ident) + data)

    @property
    def ids64(self):
        return bool(self.peer_caps & self.caps & CAP_64BIT_IDS)

    @property
    def len32(self):
        return bool(self.peer_caps & self.caps & CAP_32BIT_BULK)

    def bulk_header(self, endpoint, status, length, stream=0):
        header = struct.pack('<BBHI', endpoint, status, length & 0xFFFF, stream)
        return header + struct.pack('<H', length >> 16) if self.len32 else header

    def exact(self, length):
        data = bytearray()
        while len(data) < length:
            chunk = self.connection.recv(length - len(data))
            if not chunk:
                raise EOFError
            data.extend(chunk)
        return bytes(data)

    def info(self):
        types, intervals, interfaces = bytearray([255] * 32), bytearray(32), bytearray(32)
        packets = [0] * 32
        rows = [(0, 0, 0, 0, 64), (16, 0, 0, 0, 64)]
        for f in self.fns:
            rows += [(f.out & 15, 2, 0, f.data, self.bulk_max), (16 + (f.inp & 15), 2, 0, f.data, self.bulk_max),
                     (16 + (f.intr & 15), 3, 8 if self.high else 16, f.comm, 16)]
        for index, kind, interval, interface, packet in rows:
            types[index], intervals[index], interfaces[index], packets[index] = kind, interval, interface, packet
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << 4):
            data += struct.pack('<32H', *packets)
        self.send(5, data=data)
        numbers, classes, subclasses, protocols = bytearray(32), bytearray(32), bytearray(32), bytearray(32)
        n = 2 * len(self.fns)
        for f in self.fns:
            numbers[f.comm], classes[f.comm], subclasses[f.comm], protocols[f.comm] = f.comm, 2, 2, 1
            numbers[f.data], classes[f.data] = f.data, 0x0a
        numbers[n], classes[n], protocols[n] = n, 0xff, 1
        self.send(4, data=struct.pack('<I', self.interfaces) + numbers + classes + subclasses + protocols)

    def notification(self, f=None):
        """Queues one PSTN SERIAL_STATE notification and sends what QEMU
        will take. Default state: receiver and transmitter carrier set.
        --mirror: a loopback plug's wiring, DTR -> DSR (bTxCarrier, D1),
        RTS -> DCD (bRxCarrier, D0), and a sent break comes back once as a
        received break (bBreak, D2).

        QEMU also discards interrupt packets it had buffered when it stops
        receiving, so a state sent just before a stop can be lost: at each
        restart the last state sent in the previous receiving period is sent
        again, without the one-shot break bit (a repeated state changes
        nothing in the driver; a repeated break would be a new break).

        Like a real device's endpoint, a notification waits until the host
        is receiving: QEMU drops interrupt packets sent while interrupt
        receiving is stopped, and its EHCI model stops and restarts it
        whenever it retires an idle queue (98 SE / NUSB guest leg,
        2026-10-22, where the old resend-on-every-start flooded the guest
        and lost the one-shot break)."""
        f = f or self.fns[0]
        if self.mirror:
            state = (2 if f.lines & 1 else 0) | (1 if f.lines & 2 else 0)
            if f.pending_break:
                state |= 4
                f.pending_break = False
        else:
            state = 3
        if len(f.notify_q) < 16:
            f.notify_q.append(state)
        self.flush_notifications(f)

    def flush_notifications(self, f=None):
        f = f or self.fns[0]
        while f.interrupt and f.notify_q:
            state = f.notify_q.popleft()
            message = struct.pack('<BBHHH', 0xa1, 0x20, 0, f.comm, 2) + struct.pack('<H', state)
            self.send(103, data=struct.pack('<BBH', f.intr, 0, len(message)) + message)
            f.told_state = True
            f.last_state, f.sent_period = state, True
            if self.mirror:
                self.log('serial_state', state=state, function=f.index)

    def cancel(self, ident):
        pending = self.pending.pop(ident, None)
        if pending:
            endpoint, _ = pending
            self.send(101, ident, self.bulk_header(endpoint, 1, 0))
            self.log('cancelled', ident=ident)

    def drain(self):
        for f in self.fns:
            self.drain_fn(f)

    def drain_fn(self, f):
        while True:
            oldest = next(((i, v) for i, v in self.pending.items() if v[0] == f.inp), None)
            if oldest is None:
                break
            ident, (endpoint, length) = oldest
            if not length:
                # A zero-length IN request carries no data: it completes with
                # none once it is the oldest, and takes no echo frame. QEMU
                # completes an endpoint's packets in order only (an earlier
                # completion aborted QEMU: usb_packet_complete_one).
                del self.pending[ident]
                self.send(101, ident, self.bulk_header(endpoint, 0, 0))
                self.log('bulk_in_zero', ident=ident, pending=len(self.pending))
                continue
            if not f.echo:
                break
            del self.pending[ident]
            frame = f.echo.popleft()
            data = frame[:length]
            f.queued -= len(data)
            if len(frame) > len(data):
                f.echo.appendleft(frame[len(data):])
            elif data and len(data) % self.bulk_max == 0 and self.flush_zlp:
                # Flush a full-packet echo with a separate short packet.
                # usbredir requests can be packet-sized even when the
                # guest's parent URB is larger: length cannot identify
                # whether the guest still needs a terminating ZLP.
                f.echo.appendleft(b'')
            self.send(101, ident, self.bulk_header(endpoint, 0, len(data)) + data)
            self.log('bulk_in', ident=ident, requested=length, length=len(data), sha256=hashlib.sha256(data).hexdigest())

    def control(self, ident, packet):
        if len(packet) < 10:
            raise ValueError('short control header')
        endpoint, request, reqtype, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        payload, result, status = packet[10:], b'', 0
        if not reqtype & 0x80 and len(payload) != length:
            raise ValueError('control OUT length mismatch')
        if reqtype & 0x60 == 0:
            if request == 6 and reqtype == 0x80:
                dtype, number = value >> 8, value & 255
                result = self.device if dtype == 1 and number == 0 else \
                    self.config_desc if dtype == 2 and number == 0 else \
                    STRINGS.get(number) if dtype == 3 else \
                    qualifier(self.profile) if dtype == 6 and number == 0 and self.high else \
                    self.other if dtype == 7 and number == 0 and self.high else None
                if result is None:
                    result, status = b'', 4
            elif request == 0:  # GET_STATUS
                result = b'\x00\x00'
            elif request in (1, 3):  # clear/set endpoint feature
                if reqtype & 31 != 2 or value != 0:
                    status = 4
            elif request == 8:
                result = bytes((self.config,))
            elif request == 10:
                result = b'\x00'
            else:
                status = 4
        elif reqtype & 0x60 == 0x20 and self.fn_by('comm', index) and self.config == 1:
            f = self.fn_by('comm', index)
            if request == 0x20 and reqtype == 0x21 and length == 7:
                baud, stop, parity, bits = struct.unpack('<IBBB', payload)
                if stop > 2 or parity > 4 or bits not in (5, 6, 7, 8):
                    status = 4
                else:
                    f.line = payload
                    self.log('line_coding', baud=baud, stop=stop, parity=parity, bits=bits, function=f.index)
            elif request == 0x21 and reqtype == 0xa1:
                result = f.line
            elif request == 0x22 and reqtype == 0x21 and length == 0 and value <= 3:
                f.lines = value
                self.log('line_state', value=value, function=f.index)
                self.notification(f)
            elif request == 0x23 and reqtype == 0x21 and length == 0:
                self.log('break', duration=value, function=f.index)
                if self.mirror and value:
                    f.pending_break = True
                    self.notification(f)
            else:
                status = 4
        else:
            status = 4
        result = result[:length]
        actual = len(result) if reqtype & 0x80 else (0 if status else length)
        header = struct.pack('<BBBBHHH', endpoint, request, reqtype, status, value, index, actual)
        self.send(100, ident, header + result)
        self.log('control', request=request, request_type=reqtype, value=value, index=index,
                 requested=length, actual=actual, status=status)

    def handle(self, kind, ident, packet):
        if kind == 3:  # reset has no response
            for pending in list(self.pending):
                self.cancel(pending)
            self.config = 0
            for f in self.fns:
                f.interrupt = False
                f.clear()
            self.log('reset')
        elif kind in (6, 7):
            if kind == 6:
                if len(packet) != 1 or packet[0] not in (0, 1):
                    self.send(8, ident, b'\x04\x00')
                    return
                for pending in list(self.pending):
                    self.cancel(pending)
                self.config = packet[0]
                for f in self.fns:
                    f.clear()
                self.info()
                self.log('configuration', value=self.config)
            self.send(8, ident, bytes((0, self.config)))
        elif kind in (9, 10):
            expected = 2 if kind == 9 else 1
            if len(packet) != expected:
                raise ValueError('bad alternate command')
            interface = packet[0]
            status = 0 if interface < self.interfaces and (kind == 10 or packet[1] == 0) else 4
            if not status and kind == 9:
                self.info()
            self.send(11, ident, bytes((status, interface, 0)))
        elif kind in (15, 16):
            if len(packet) != 1:
                raise ValueError('bad interrupt command')
            endpoint = packet[0]
            f = self.fn_by('intr', endpoint)
            status = 0 if f and self.config == 1 else 4
            if f:
                f.interrupt = kind == 15 and status == 0
            self.send(17, ident, bytes((status, endpoint)))
            if f and f.interrupt and not f.told_state and not f.notify_q:
                self.notification(f)         # the state, once per configuration
            elif f and f.interrupt and f.sent_period and not f.notify_q:
                f.notify_q.append(f.last_state & ~4)   # not the one-shot break
                if self.mirror:
                    self.log('serial_state_resend', state=f.last_state, function=f.index)
                self.flush_notifications(f)
                f.sent_period = False        # a repeat is not a new state
            if f:
                self.flush_notifications(f)
        elif kind == 21:
            if packet:
                raise ValueError('bad cancellation')
            if ident not in self.pending:
                # A cancel for a packet already completed (e.g. an OUT the
                # model took): recorded, since QEMU may then run it again.
                self.log('cancel_late', ident=ident)
            self.cancel(ident)
            # A zero-length request behind the cancelled one may now be the
            # oldest: complete it (and any data) in order.
            self.drain()
        elif kind == 100:
            self.control(ident, packet)
        elif kind == 101:
            size = 10 if self.len32 else 8
            if len(packet) < size:
                raise ValueError('short bulk header')
            endpoint, _, length, stream = struct.unpack('<BBHI', packet[:8])
            if self.len32:
                length |= struct.unpack('<H', packet[8:10])[0] << 16
            out_fn, in_fn = self.fn_by('out', endpoint), self.fn_by('inp', endpoint)
            if stream or self.config != 1 or not (out_fn or in_fn):
                self.send(101, ident, self.bulk_header(endpoint, 4, 0, stream))
            elif out_fn:
                f = out_fn
                data = packet[size:]
                if len(data) != length:
                    raise ValueError('bulk OUT length mismatch')
                if f.queued + length > 1048576 or len(f.echo) >= 1024:
                    raise ValueError('echo queue exceeds test bound')
                if data:
                    f.echo.append(data)
                    f.queued += length
                self.send(101, ident, self.bulk_header(endpoint, 0, length))
                self.log('bulk_out', length=length, ident=ident, function=f.index,
                         sha256=hashlib.sha256(data).hexdigest())
                self.drain()
            else:
                # A zero-length IN request is valid: qemu-xhci sends one for a
                # TD that xHCI98 cancelled by rewriting it as a No Op (98 SE
                # guest leg, 2026-10-22, x17). Raising on it dropped the
                # connection and the guest lost the device. drain() completes
                # it in order.
                if len(packet) != size or ident in self.pending or len(self.pending) >= 1024:
                    self.log('bad_bulk_in', ident=ident, header=len(packet), length=length,
                             duplicate=ident in self.pending, pending=len(self.pending),
                             pending_ids=list(self.pending)[:32])
                    raise ValueError('invalid/too many bulk IN requests')
                self.log('bulk_in_request', ident=ident, length=length, pending=len(self.pending))
                self.pending[ident] = (endpoint, length)
                self.drain()
        else:
            raise ValueError(f'unsupported usbredir message {kind}')

    def run(self):
        self.send(0, data=b'usb-wdm-free CDC simulation 0.1'.ljust(64, b'\x00') + struct.pack('<I', self.caps))
        kind, length, ident = struct.unpack('<III', self.exact(12))
        if kind != 0 or ident or length < 64 or length > 4096:
            raise ValueError('invalid hello')
        hello = self.exact(length)
        self.peer_caps = struct.unpack('<I', hello[64:68])[0] if length >= 68 else 0
        self.log('hello', peer=hello[:64].split(b'\x00')[0].decode('ascii', 'replace'),
                 capabilities=self.peer_caps)
        self.info()
        vid, pid = PROFILES[self.profile]
        cls, sub, proto = self.device[4], self.device[5], self.device[6]
        connect = struct.pack('<BBBBHH', 2 if self.high else 1, cls, sub, proto, vid, pid)
        if self.peer_caps & (1 << 1):
            connect += struct.pack('<H', 0x0100)
        self.send(1, data=connect)
        self.log('connected', model='SIMULATED CDC echo', caps=self.caps, vid='%04X' % vid, pid='%04X' % pid,
                 speed=self.speed, profile=self.profile, mirror=self.mirror)
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
    ap.add_argument('--port', type=int, default=44410)
    ap.add_argument('--log', type=Path, required=True)
    ap.add_argument('--speed', choices=('full', 'high'), default='full')
    ap.add_argument('--profile', choices=sorted(PROFILES), default='uno')
    ap.add_argument('--mirror', action='store_true',
                    help='DTR->DSR, RTS->DCD and a sent break looped back in SERIAL_STATE')
    ap.add_argument('--no-flush-zlp', action='store_true',
                    help='send no zero-length packet after a full-packet echo: after one, no '
                         'further IN request from Windows 2000 usbser.sys appeared during the '
                         'failing read (2000 legs use this)')
    ap.add_argument('--no-bulk32', action='store_true',
                    help='do not offer 32-bit bulk lengths: QEMU then does not pipeline '
                         '(combine) bulk IN packets; not usable on xHCI ports')
    args = ap.parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'CDC simulation listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            caps = CAPS & ~CAP_32BIT_BULK if args.no_bulk32 else CAPS
            model = Model(connection, trace, args.speed, args.profile, args.mirror, caps,
                          flush_zlp=not args.no_flush_zlp)
            try:
                model.run()
            except (EOFError, ConnectionResetError, BrokenPipeError):
                model.log('disconnected')


if __name__ == '__main__':
    main()
