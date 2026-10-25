#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""SIMULATED CDC-NCM Ethernet function over localhost usbredir (test aid only).

QEMU has no NCM device (its usb-net is ECM and RNDIS), and no NCM device is
on the bench, so this is cdcnet's NCM vehicle. Original model from the
USB CDC NCM 1.0 specification (functional descriptor, class requests
0x80-0x8A, NTB16 with NTH16/NDP16, notifications), CDC ECM 1.2 (Ethernet
functional descriptor, SET_ETHERNET_PACKET_FILTER 0x43,
SET_ETHERNET_MULTICAST_FILTERS 0x40, NETWORK_CONNECTION,
CONNECTION_SPEED_CHANGE), USB 2.0 chapter 9 and SPICE's
usb-redirection-protocol.txt v0.7. No physical device is accessed.

The device: class 02 at device level (no IAD), interface 0 CDC NCM
(02/0D/00) with an interrupt-IN notification endpoint, interface 1 data
(0A/00/01) with an endpoint-less alternate 0 and bulk IN/OUT in alternate
1. IDs default to 0525:A4A2 (QEMU usb-net's), which the cdcnet test INFs
(CDCT98.INF, CDCTEST.INF) bind; every string says SIMULATED.

Options shape what cdcnet must cope with: --caps (bmNetworkCapabilities),
--ntb32 (offer NTB32 too, so the host must SET_NTB_FORMAT), --out-divisor/
--out-remainder/--out-alignment (the host's transmit layout), --in-layout
front|end|split (where the device puts its NDP16s), --mc-filters
(wNumberMCFilters), --flap-after/--flap-for (NETWORK_CONNECTION 0 then 1),
--speed high|full. Behind the function sits the same SIMULATED host as
rtlemu.py (192.168.77.1: ARP, ICMP echo, DHCP). Every NTB the host sends
is checked against NCM 1.0's rules and the advertised parameters; a
violation is logged as ntb_out_bad with its reason.
"""
import argparse
import collections
import hashlib
import json
import select
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'rtl815x'))
from rtlemu import Network, ether_crc, HOST_MAC  # noqa: E402  (the SIMULATED host)

CAP_VERSION, CAP_EP_MAX, CAP_ID64, CAP_BULK32 = 1, 4, 5, 6
CAPS = (1 << CAP_VERSION) | (1 << CAP_EP_MAX) | (1 << CAP_ID64) | (1 << CAP_BULK32)
NTH16, NDP16 = 0x484D434E, 0x304D434E
EP_IN, EP_OUT, EP_INT = 0x81, 0x02, 0x83
GUEST_MAC = bytes.fromhex('020000770002')
FILTER_PROMISCUOUS, FILTER_ALL_MULTICAST, FILTER_DIRECTED, FILTER_BROADCAST, FILTER_MULTICAST = 1, 2, 4, 8, 16


def usb_string(text):
    raw = text.encode('utf-16le')
    return bytes((len(raw) + 2, 3)) + raw


class Function:
    def __init__(self, speed='high', caps=0x21, ntb32=False, in_max=16384, out_max=16384,
                 out_divisor=4, out_remainder=0, out_alignment=4, out_datagrams=0, mc_filters=0,
                 vid=0x0525, pid=0xa4a2, int_max=16):
        self.speed, self.caps, self.ntb32 = speed, caps, ntb32
        self.in_max, self.out_max = in_max, out_max
        self.out_divisor, self.out_remainder, self.out_alignment = out_divisor, out_remainder, out_alignment
        self.out_datagrams, self.mc_filters = out_datagrams, mc_filters
        self.vid, self.pid = vid, pid
        self.int_max = int_max
        self.bulk = 512 if speed == 'high' else 64
        self.device = struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x0200, 2, 0, 0, 64, vid, pid, 0x0100, 1, 2, 0, 1)
        self.config = {s: self.configuration(s) for s in ('high', 'full')}
        self.strings = {0: b'\x04\x03\x09\x04', 1: usb_string('usb-wdm-free SIMULATED'),
                        2: usb_string('CDC NCM Ethernet - SIMULATED'),
                        3: usb_string(GUEST_MAC.hex().upper())}

    def configuration(self, speed, kind=2):
        bulk = 512 if speed == 'high' else 64
        body = bytes((9, 4, 0, 0, 1, 2, 0x0d, 0, 0))
        body += bytes((5, 0x24, 0x00)) + struct.pack('<H', 0x0120)          # header, CDC 1.20
        body += bytes((5, 0x24, 0x06, 0, 1))                                  # union
        body += bytes((13, 0x24, 0x0f, 3)) + struct.pack('<IHHB', 0, 1514, self.mc_filters, 0)
        body += bytes((6, 0x24, 0x1a)) + struct.pack('<HB', 0x0100, self.caps)  # NCM 1.0
        body += bytes((7, 5, EP_INT, 3)) + struct.pack('<HB', self.int_max, 9 if speed == 'high' else 32)
        body += bytes((9, 4, 1, 0, 0, 0x0a, 0, 1, 0))
        body += bytes((9, 4, 1, 1, 2, 0x0a, 0, 1, 0))
        body += bytes((7, 5, EP_IN, 2)) + struct.pack('<HB', bulk, 0)
        body += bytes((7, 5, EP_OUT, 2)) + struct.pack('<HB', bulk, 0)
        return bytes((9, kind)) + struct.pack('<HBBBBB', 9 + len(body), 2, 1, 0, 0x80, 50) + body

    def ntb_parameters(self):
        return struct.pack('<HHIHHHHIHHHH', 28, 3 if self.ntb32 else 1, self.in_max, 4, 0, 4, 0,
                           self.out_max, self.out_divisor, self.out_remainder, self.out_alignment,
                           self.out_datagrams)


def build_ntb(frames, sequence, layout='front', divisor=4):
    """An NTB16 holding frames. layout: front (NDP after NTH16), end (NDP
    after the datagrams), split (one NDP before, one after, half the
    datagrams each)."""
    if layout == 'split' and len(frames) < 2:
        layout = 'front'                    # one datagram: one NDP
    groups = [frames] if layout != 'split' else [frames[:len(frames) // 2], frames[len(frames) // 2:]]
    out = bytearray(12)
    ndp_sizes = [8 + 4 * (len(g) + 1) for g in groups]

    def align(n):
        return (n + divisor - 1) // divisor * divisor

    entries, ndp_at = [[] for _ in groups], []
    if layout in ('front', 'split'):
        ndp_at.append(len(out))
        out += bytes(ndp_sizes[0])
    for gi, group in enumerate(groups):
        for frame in group:
            at = align(len(out))
            out += bytes(at - len(out)) + frame
            entries[gi].append((at, len(frame)))
    if layout == 'end' or layout == 'split':
        while len(out) & 3:
            out += b'\0'
        ndp_at.append(len(out))
        out += bytes(ndp_sizes[-1])
    for gi, at in enumerate(ndp_at):
        nxt = ndp_at[gi + 1] if gi + 1 < len(ndp_at) else 0
        table = struct.pack('<IHH', NDP16, ndp_sizes[gi], nxt) + \
            b''.join(struct.pack('<HH', a, n) for a, n in entries[gi]) + b'\0\0\0\0'
        out[at:at + len(table)] = table
    struct.pack_into('<IHHHH', out, 0, NTH16, 12, sequence & 0xffff, len(out), ndp_at[0])
    return bytes(out)


def parse_out_ntb(b, f):
    """Check a host NTB16 against NCM 1.0 and the advertised parameters.
    Returns (datagrams, problems)."""
    problems = []
    if len(b) < 12 or struct.unpack_from('<I', b)[0] != NTH16:
        return [], ['NTH16 signature']
    sig, hlen, seq, block, ndp = struct.unpack_from('<IHHHH', b)
    if hlen != 12:
        problems.append('wHeaderLength %d' % hlen)
    if block != len(b):
        problems.append('wBlockLength %d for a %d-byte transfer' % (block, len(b)))
    if len(b) > f.out_max:
        problems.append('NTB exceeds dwNtbOutMaxSize')
    datagrams, seen = [], set()
    while ndp:
        if ndp in seen or ndp + 16 > len(b) or ndp % f.out_alignment:
            problems.append('NDP16 at %d (alignment %d)' % (ndp, f.out_alignment))
            break
        seen.add(ndp)
        nsig, nlen, nxt = struct.unpack_from('<IHH', b, ndp)
        if nsig != NDP16 or nlen < 16 or nlen % 4 or ndp + nlen > len(b):
            problems.append('NDP16 header at %d' % ndp)
            break
        terminated = False
        for e in range((nlen - 8) // 4):
            at, n = struct.unpack_from('<HH', b, ndp + 8 + 4 * e)
            if not at or not n:
                terminated = True
                break
            if at + n > len(b) or not 14 <= n <= 1514:
                problems.append('datagram %d+%d out of bounds' % (at, n))
                continue
            if at % f.out_divisor != f.out_remainder:
                problems.append('datagram at %d not %d mod %d' % (at, f.out_remainder, f.out_divisor))
            if ndp <= at < ndp + nlen or at <= ndp < at + n:
                problems.append('datagram overlaps NDP16')
            datagrams.append(bytes(b[at:at + n]))
        if not terminated:
            problems.append('NDP16 without a terminating entry')
        ndp = nxt
    limit = f.out_datagrams or 0xffff
    if len(datagrams) > limit:
        problems.append('more than wNtbOutMaxDatagrams')
    return datagrams, problems


class Model:
    def __init__(self, connection, trace, function=None, now=time.monotonic, in_layout='front',
                 flap_after=None, flap_for=3.0):
        self.connection, self.trace, self.now = connection, trace, now
        self.f = function or Function()
        self.speed = self.f.speed
        self.in_layout, self.flap_after, self.flap_for = in_layout, flap_after, flap_for
        self.net = Network(self.log)
        self.peer_caps = 0
        self.config = self.alt = 0
        self.interrupt = False
        self.pending = collections.OrderedDict()
        self.rx = collections.deque()
        self.notify = collections.deque()
        self.totals = collections.Counter()
        self.reset_function()

    def reset_function(self):
        self.ntb_format = 0
        self.in_size = self.f.in_max
        self.max_datagram = 1514
        self.crc = 0
        self.filter = FILTER_DIRECTED | FILTER_BROADCAST | FILTER_ALL_MULTICAST
        self.mc = []
        self.sequence = 0
        self.connected = False
        self.flap_at = self.restore_at = None

    # -- plumbing ----------------------------------------------------------
    def log(self, event, **fields):
        self.trace.write(json.dumps(dict(time=time.time(), monotonic=self.now(), event=event,
                                        simulated=True, **fields)) + '\n')
        self.trace.flush()

    @property
    def id64(self):
        return bool(self.peer_caps & CAPS & (1 << CAP_ID64))

    @property
    def len32(self):
        return bool(self.peer_caps & CAPS & (1 << CAP_BULK32))

    def send(self, kind, ident=0, data=b''):
        self.connection.sendall(struct.pack('<IIQ' if self.id64 else '<III', kind, len(data), ident) + data)

    def bulk_header(self, endpoint, status, length):
        header = struct.pack('<BBHI', endpoint, status, length & 0xffff, 0)
        return header + struct.pack('<H', length >> 16) if self.len32 else header

    def info(self):
        types, intervals, interfaces = bytearray([255] * 32), bytearray(32), bytearray(32)
        packets = [0] * 32
        types[0] = types[16] = 0
        packets[0] = packets[16] = 64
        if self.config:
            slot = 16 + (EP_INT & 15)
            types[slot], intervals[slot], interfaces[slot], packets[slot] = \
                3, 9 if self.speed == 'high' else 32, 0, self.f.int_max
            if self.alt:
                for slot in (16 + (EP_IN & 15), EP_OUT):
                    types[slot], interfaces[slot], packets[slot] = 2, 1, self.f.bulk
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << CAP_EP_MAX):
            data += struct.pack('<32H', *packets)
        self.send(5, data=data)
        numbers, classes, subclasses, protocols = (bytearray(32) for _ in range(4))
        count = 0
        if self.config:
            count = 2
            numbers[:2], classes[:2], subclasses[:2], protocols[:2] = b'\0\1', b'\x02\x0a', b'\x0d\0', b'\0\1'
        self.send(4, data=struct.pack('<I', count) + numbers + classes + subclasses + protocols)

    # -- notifications -----------------------------------------------------
    def notify_link(self, up, why):
        self.connected = up
        self.notify.append(struct.pack('<BBHHH', 0xa1, 0x00, 1 if up else 0, 0, 0))
        if up:
            rate = 480000000 if self.speed == 'high' else 12000000
            self.notify.append(struct.pack('<BBHHHII', 0xa1, 0x2a, 0, 0, 8, rate, rate))
        self.log('link', up=up, why=why)
        self.flush_notify()

    def flush_notify(self):
        while self.interrupt and self.notify:
            data = self.notify.popleft()
            self.send(103, data=struct.pack('<BBH', EP_INT, 0, len(data)) + data)
            self.totals['notifications'] += 1

    def pump(self):
        now = self.now()
        if self.flap_at is not None and now >= self.flap_at:
            self.flap_at = None
            self.notify_link(False, 'cable_out')
            self.restore_at = now + self.flap_for
        if self.restore_at is not None and now >= self.restore_at:
            self.restore_at = None
            self.notify_link(True, 'cable_in')
        self.drain()

    # -- data --------------------------------------------------------------
    def accepts(self, frame):
        dest, flt = frame[:6], self.filter
        if flt & FILTER_PROMISCUOUS:
            return True
        if dest == b'\xff' * 6:
            return bool(flt & FILTER_BROADCAST)
        if dest[0] & 1:
            return bool(flt & FILTER_ALL_MULTICAST) or (bool(flt & FILTER_MULTICAST) and dest in self.mc)
        return bool(flt & FILTER_DIRECTED) and dest == GUEST_MAC

    def deliver(self, frame):
        if not (self.alt and self.connected and self.accepts(frame)):
            self.totals['rx_filtered'] += 1
            return
        if len(self.rx) < 256:
            self.rx.append(frame)
        self.drain()

    def drain(self):
        while self.pending and self.rx:
            ident, length = next(iter(self.pending.items()))
            limit = min(length, self.in_size)
            frames, size = [], 12 + 8 + 4 + 16
            while self.rx and len(frames) < 32:
                need = len(self.rx[0]) + 8 + 4 + 4
                if frames and size + need > limit:
                    break
                frames.append(self.rx.popleft())
                size += need
            ntb = build_ntb(frames, self.sequence, self.in_layout)
            self.sequence += 1
            if len(ntb) > limit:
                raise ValueError('NTB exceeds the host input size')
            del self.pending[ident]
            self.send(101, ident, self.bulk_header(EP_IN, 0, len(ntb)) + ntb)
            self.totals['rx_ntbs'] += 1
            self.totals['rx_datagrams'] += len(frames)
            self.log('ntb_in', length=len(ntb), datagrams=len(frames), layout=self.in_layout)

    def transmit(self, data):
        datagrams, problems = parse_out_ntb(data, self.f)
        self.totals['tx_ntbs'] += 1
        if problems:
            self.totals['tx_bad'] += 1
            self.log('ntb_out_bad', length=len(data), problems=problems[:8])
        self.log('ntb_out', length=len(data), datagrams=len(datagrams))
        for frame in datagrams:
            self.totals['tx_datagrams'] += 1
            if self.connected:
                for reply in self.net.handle(frame):
                    self.deliver(reply)
        return datagrams, problems

    # -- control -------------------------------------------------------------
    def class_request(self, request, rt, value, length, payload):
        f = self.f
        if rt == 0xa1 and request == 0x80:
            return 0, f.ntb_parameters()
        if rt == 0xa1 and request == 0x81:
            return 0, GUEST_MAC
        if rt == 0xa1 and request == 0x83:
            return 0, struct.pack('<H', self.ntb_format)
        if rt == 0x21 and request == 0x84 and value in (0, 1) and (value == 0 or f.ntb32) and not self.alt:
            self.ntb_format = value
            self.log('ntb_format', value=value)
            return 0, b''
        if rt == 0xa1 and request == 0x85:
            return 0, struct.pack('<I', self.in_size) + (struct.pack('<HH', 0, 0) if length >= 8 else b'')
        if rt == 0x21 and request == 0x86 and len(payload) in (4, 8) and not self.alt:
            if len(payload) == 8 and not f.caps & 0x20:
                return 4, b''
            size = struct.unpack_from('<I', payload)[0]
            if not 2048 <= size <= f.in_max:
                return 4, b''
            self.in_size = size
            self.log('ntb_input_size', size=size, bytes=len(payload))
            return 0, b''
        if rt == 0xa1 and request == 0x87 and f.caps & 0x08:
            return 0, struct.pack('<H', self.max_datagram)
        if rt == 0x21 and request == 0x88 and f.caps & 0x08 and len(payload) == 2:
            size = struct.unpack('<H', payload)[0]
            if not 1514 <= size <= 1514:
                return 4, b''
            self.max_datagram = size
            return 0, b''
        if rt == 0xa1 and request == 0x89 and f.caps & 0x10:
            return 0, struct.pack('<H', self.crc)
        if rt == 0x21 and request == 0x8a and f.caps & 0x10 and value in (0, 1):
            self.crc = value
            self.log('crc_mode', value=value)
            return 0, b''
        if rt == 0x21 and request == 0x43 and f.caps & 0x01:
            self.filter = value
            self.log('packet_filter', value=value)
            return 0, b''
        if rt == 0x21 and request == 0x40 and f.mc_filters and len(payload) == 6 * value and value <= f.mc_filters:
            self.mc = [payload[i:i + 6] for i in range(0, len(payload), 6)]
            self.log('multicast_filters', count=value)
            return 0, b''
        return 4, b''

    def control(self, ident, packet):
        if len(packet) < 10:
            raise ValueError('short control header')
        ep, request, rt, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        payload = packet[10:]
        if len(payload) != (0 if rt & 128 else length):
            raise ValueError('control length mismatch')
        status, data = 4, b''
        f = self.f
        if rt == 0x80 and request == 6:
            kind, number = value >> 8, value & 255
            other = 'full' if self.speed == 'high' else 'high'
            data = f.device if kind == 1 and not number else \
                f.config[self.speed] if kind == 2 and not number else \
                f.strings.get(number) if kind == 3 else \
                (struct.pack('<BBHBBBBBB', 10, 6, 0x200, 2, 0, 0, 64, 1, 0) if kind == 6 and not number else
                 b'\x09\x07' + f.config[other][2:] if kind == 7 and not number else None)
            status, data = (0, data) if data is not None else (4, b'')
        elif request == 0 and rt in (0x80, 0x81, 0x82):
            status, data = 0, b'\0\0'
        elif request == 8 and rt == 0x80:
            status, data = 0, bytes((self.config,))
        elif request == 10 and rt == 0x81:
            status, data = 0, bytes((self.alt if index == 1 else 0,))
        elif request in (1, 3) and rt == 2 and value == 0:
            status = 0
        elif rt in (0x21, 0xa1) and index == 0 and self.config:
            status, data = self.class_request(request, rt, value, length, payload)
        data = data[:length]
        actual = len(data) if rt & 128 else (0 if status else length)
        self.send(100, ident, struct.pack('<BBBBHHH', ep, request, rt, status, value, index, actual) + data)
        self.log('control', request=request, request_type=rt, value=value, index=index,
                 requested=length, status=status)

    # -- usbredir messages -----------------------------------------------------
    def cancel_all(self):
        for ident in list(self.pending):
            self.send(101, ident, self.bulk_header(EP_IN, 1, 0))
        self.pending.clear()

    def handle(self, kind, ident, packet):
        if kind == 3:
            if packet:
                raise ValueError('reset length')
            self.cancel_all()
            self.rx.clear()
            self.config = self.alt = 0
            self.interrupt = False
            self.reset_function()
            self.log('reset')
        elif kind in (6, 7):
            if len(packet) != (1 if kind == 6 else 0):
                raise ValueError('configuration length')
            status = 0
            if kind == 6:
                status = 0 if packet[0] in (0, 1) else 4
                if not status:
                    self.cancel_all()
                    self.rx.clear()
                    self.config, self.alt = packet[0], 0
                    self.reset_function()
                    self.info()
                    self.log('configuration', value=self.config)
            self.send(8, ident, bytes((status, self.config)))
        elif kind in (9, 10):
            if len(packet) != (2 if kind == 9 else 1):
                raise ValueError('alternate length')
            interface = packet[0]
            alternate = packet[1] if kind == 9 else (self.alt if interface == 1 else 0)
            ok = self.config and (interface == 0 and alternate == 0 or interface == 1 and alternate in (0, 1))
            if kind == 9 and ok and interface == 1:
                self.cancel_all()
                self.rx.clear()
                self.alt = alternate
                self.info()
                self.log('alternate', interface=1, alternate=alternate)
                if alternate:
                    # NCM 1.0 7.1: after selecting the data alternate the
                    # function reports the connection and its speed.
                    self.notify_link(True, 'data_alternate')
                    if self.flap_after is not None:
                        self.flap_at, self.flap_after = self.now() + self.flap_after, None
                else:
                    self.connected = False
            self.send(11, ident, bytes((0 if ok else 4, interface, alternate)))
        elif kind in (15, 16):
            if len(packet) != 1:
                raise ValueError('interrupt header')
            status = 0 if packet[0] == EP_INT and self.config else 4
            self.interrupt = kind == 15 and not status
            self.send(17, ident, bytes((status, packet[0])))
            self.flush_notify()
        elif kind == 21:
            if packet:
                raise ValueError('cancel length')
            if ident in self.pending:
                del self.pending[ident]
                self.send(101, ident, self.bulk_header(EP_IN, 1, 0))
        elif kind == 24:
            pass
        elif kind == 100:
            self.control(ident, packet)
        elif kind == 101:
            size = 10 if self.len32 else 8
            if len(packet) < size:
                raise ValueError('short bulk header')
            endpoint, _, length, stream = struct.unpack('<BBHI', packet[:8])
            if self.len32:
                length |= struct.unpack('<H', packet[8:10])[0] << 16
            if stream or not self.alt or endpoint not in (EP_IN, EP_OUT):
                self.send(101, ident, self.bulk_header(endpoint, 4, 0))
            elif endpoint == EP_OUT:
                data = packet[size:]
                if len(data) != length:
                    raise ValueError('bulk OUT length mismatch')
                self.send(101, ident, self.bulk_header(EP_OUT, 0, length))
                if data:
                    self.transmit(data)
                else:
                    self.totals['tx_zlp'] += 1
                self.drain()
            else:
                if len(packet) != size or ident in self.pending or len(self.pending) >= 256:
                    raise ValueError('invalid bulk IN request')
                self.pending[ident] = length
                self.drain()
        else:
            raise ValueError(f'unsupported usbredir message {kind}')

    def connect(self):
        self.info()
        data = struct.pack('<BBBBHH', 2 if self.speed == 'high' else 1, 2, 0, 0, self.f.vid, self.f.pid)
        if self.peer_caps & (1 << CAP_VERSION):
            data += struct.pack('<H', 0x0100)
        self.send(1, data=data)
        self.log('connected', model='SIMULATED CDC NCM', speed=self.speed, caps=self.f.caps,
                 config_sha256=hashlib.sha256(self.f.config[self.speed]).hexdigest())

    def run(self):
        self.send(0, data=b'usb-wdm-free NCM SIMULATED'.ljust(64, b'\0') + struct.pack('<I', CAPS))
        buffer, hello = bytearray(), False
        next_report = self.now() + 5
        while True:
            self.pump()
            if self.now() >= next_report:
                next_report = self.now() + 5
                self.log('totals', **self.totals)
            readable, _, _ = select.select([self.connection], [], [], 0.02)
            if not readable:
                continue
            data = self.connection.recv(65536)
            if not data:
                raise EOFError
            buffer.extend(data)
            while True:
                size = 16 if self.id64 else 12
                if len(buffer) < size:
                    break
                kind, length, ident = struct.unpack('<IIQ' if self.id64 else '<III', buffer[:size])
                if length > 65546 + 65536:
                    raise ValueError('message bound')
                if len(buffer) < size + length:
                    break
                packet = bytes(buffer[size:size + length])
                del buffer[:size + length]
                if not hello:
                    if kind or ident or not 64 <= length <= 4096:
                        raise ValueError('hello framing')
                    self.peer_caps = int.from_bytes(packet[64:68], 'little') if length >= 68 else 0
                    self.log('hello', peer=packet[:64].rstrip(b'\0').decode('ascii', 'replace'),
                             peer_caps=self.peer_caps)
                    self.connect()
                    hello = True
                else:
                    self.handle(kind, ident, packet)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--log', type=Path, required=True, help='new JSONL trace (must not exist)')
    ap.add_argument('--speed', choices=('high', 'full'), default='high')
    ap.add_argument('--caps', type=lambda s: int(s, 0), default=0x21, help='bmNetworkCapabilities')
    ap.add_argument('--ntb32', action='store_true', help='also offer NTB32')
    ap.add_argument('--out-divisor', type=int, default=4)
    ap.add_argument('--out-remainder', type=int, default=0)
    ap.add_argument('--out-alignment', type=int, default=4)
    ap.add_argument('--in-layout', choices=('front', 'end', 'split'), default='front')
    ap.add_argument('--mc-filters', type=int, default=0)
    ap.add_argument('--int-max', type=int, choices=(8, 16, 32, 64), default=16,
                    help='notification endpoint wMaxPacketSize (a 16-byte SPEED_CHANGE is a full packet at 16)')
    ap.add_argument('--flap-after', type=float)
    ap.add_argument('--flap-for', type=float, default=3.0)
    ap.add_argument('--vid', type=lambda s: int(s, 16), default=0x0525)
    ap.add_argument('--pid', type=lambda s: int(s, 16), default=0xa4a2)
    args = ap.parse_args()
    function = Function(args.speed, args.caps, args.ntb32, out_divisor=args.out_divisor,
                        out_remainder=args.out_remainder, out_alignment=args.out_alignment,
                        mc_filters=args.mc_filters, vid=args.vid, pid=args.pid, int_max=args.int_max)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'SIMULATED CDC NCM listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            model = Model(connection, trace, function, in_layout=args.in_layout,
                          flap_after=args.flap_after, flap_for=args.flap_for)
            try:
                model.run()
            except (EOFError, ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
                model.log('disconnected', **model.totals)
            except Exception as error:
                model.log('fatal', error=repr(error), **model.totals)
                raise


if __name__ == '__main__':
    main()
