#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""Test-only UVC1.1 bulk YUY2 moving bars, exported via localhost usbredir.

Original model from USB-IF UVC1.1 and SPICE usbredir v0.7 wire facts.
FFFF:0002 are local simulation IDs, not a registered-product claim.
No real camera, microphone, file recording, kernel driver or image decoder.
"""
import argparse
import collections
import hashlib
import json
import select
import socket
import struct
import time
from pathlib import Path

WIDTH, HEIGHT, INTERVAL, PAYLOAD = 160, 120, 1000000, 1022
FRAME_BYTES = WIDTH * HEIGHT * 2
CAPS = (1 << 1) | (1 << 4)
DEVICE = struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x200, 0xef, 2, 1, 64,
                     0xffff, 2, 0x100, 1, 2, 0, 1)
DEFAULT = struct.pack('<HBBIHHHHHIIIBBBB', 0, 1, 1, INTERVAL, 0, 0, 0, 0, 0,
                      FRAME_BYTES, PAYLOAD, 48000000, 0, 0, 0, 0)


def configuration():
    vc = bytes((13, 0x24, 1, 0x10, 1, 40, 0)) + struct.pack('<I', 48000000) + b'\1\1'
    vc += bytes((18, 0x24, 2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0))
    vc += bytes((9, 0x24, 3, 2, 1, 1, 0, 1, 0))
    vs = bytes((14, 0x24, 1, 1, 77, 0, 0x81, 0, 2, 0, 0, 0, 1, 0))
    vs += bytes((27, 0x24, 4, 1, 1)) + bytes.fromhex('5955593200001000800000aa00389b71')
    vs += bytes((16, 1, 0, 0, 0, 0))
    vs += bytes((30, 0x24, 5, 1, 0)) + struct.pack('<HHIIIIBI', WIDTH, HEIGHT,
        FRAME_BYTES * 8 * 10, FRAME_BYTES * 8 * 10, FRAME_BYTES, INTERVAL, 1, INTERVAL)
    vs += bytes((6, 0x24, 13, 1, 1, 4))
    body = bytes((8, 11, 0, 2, 14, 3, 0, 0, 9, 4, 0, 0, 0, 14, 1, 0, 0)) + vc
    body += bytes((9, 4, 1, 0, 1, 14, 2, 0, 0)) + vs + bytes((7, 5, 0x81, 2, 64, 0, 0))
    return bytes((9, 2)) + struct.pack('<H', len(body) + 9) + bytes((2, 1, 0, 0x80, 50)) + body


CONFIG = configuration()


def usb_string(text):
    raw = text.encode('utf-16le')
    return bytes((len(raw) + 2, 3)) + raw


STRINGS = {0: b'\4\3\x09\4', 1: usb_string('usb-wdm-free simulation'),
           2: usb_string('UVC YUY2 bars - SIMULATED')}


def picture(sequence):
    colors = ((235, 128, 128), (210, 16, 146), (170, 166, 16), (145, 54, 34),
              (106, 202, 222), (81, 90, 240), (41, 240, 110), (16, 128, 128))
    row = bytearray()
    for x in range(0, WIDTH, 2):
        y, u, v = colors[((x + sequence * 2) % WIDTH) * 8 // WIDTH]
        row.extend((y, u, y, v))
    return bytes(row) * HEIGHT


class Model:
    def __init__(self, connection, trace):
        self.connection, self.trace = connection, trace
        self.peer_caps = self.config = 0
        self.pending = collections.OrderedDict()
        self.probe = self.commit = DEFAULT
        self.committed = False
        self.sequence = self.offset = self.payload_offset = 0
        self.frame = self.payload = b''
        self.zlp = False
        self.next_frame = 0.0

    def log(self, event, **fields):
        self.trace.write(json.dumps(dict(time=time.time(), event=event, **fields)) + '\n')
        self.trace.flush()

    def send(self, kind, ident=0, data=b''):
        self.connection.sendall(struct.pack('<III', kind, len(data), ident) + data)

    def bulk_reply(self, ident, endpoint, status, data=b''):
        self.send(101, ident, struct.pack('<BBHI', endpoint, status, len(data), 0) + data)

    def cancel(self, ident):
        if ident in self.pending:
            del self.pending[ident]
            self.bulk_reply(ident, 0x81, 1)

    def reset(self):
        for ident in list(self.pending):
            self.cancel(ident)
        self.committed = False
        self.probe = self.commit = DEFAULT
        self.sequence = self.offset = self.payload_offset = 0
        self.frame = self.payload = b''
        self.zlp = False
        self.next_frame = 0.0

    def info(self):
        types, intervals, interfaces = bytearray([255] * 32), bytearray(32), bytearray(32)
        packets = [0] * 32
        for index, kind, interface in ((0, 0, 0), (16, 0, 0), (17, 2, 1)):
            types[index], interfaces[index], packets[index] = kind, interface, 64
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << 4):
            data += struct.pack('<32H', *packets)
        self.send(5, data=data)
        numbers, classes, subclasses, protocols = (bytearray(32) for _ in range(4))
        numbers[:2], classes[:2], subclasses[:2] = b'\0\1', b'\x0e\x0e', b'\1\2'
        self.send(4, data=struct.pack('<I', 2) + numbers + classes + subclasses + protocols)

    def control(self, ident, packet):
        if len(packet) < 10:
            raise ValueError('short control header')
        ep, request, rt, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        if len(packet) != 10 + (0 if rt & 128 else length):
            raise ValueError('control size mismatch')
        data, status = b'', 4
        if rt == 0x80 and request == 6:
            dtype, number = value >> 8, value & 255
            data = DEVICE if dtype == 1 and not number else CONFIG if dtype == 2 and not number \
                else STRINGS.get(number) if dtype == 3 else None
            status, data = (0, data) if data is not None else (4, b'')
        elif request == 0 and rt in (0x80, 0x81, 0x82):
            status, data = 0, b'\0\0'
        elif request == 1 and rt == 2 and value == 0 and index == 0x81:
            status = 0
        elif request == 8 and rt == 0x80:
            status, data = 0, bytes((self.config,))
        elif request == 10 and rt == 0x81 and index in (0, 1):
            status, data = 0, b'\0'
        elif self.config and index == 1 and value in (0x100, 0x200):
            if rt == 0xa1 and request in (0x81, 0x82, 0x83, 0x87):
                data, status = (self.probe if value == 0x100 else self.commit), 0
            elif rt == 0xa1 and request == 0x84:
                data, status = bytes(34), 0
            elif rt == 0xa1 and request == 0x85:
                data, status = struct.pack('<H', 34), 0
            elif rt == 0xa1 and request == 0x86:
                data, status = b'\3', 0
            elif rt == 0x21 and request == 1 and length == 34 and packet[12:14] == b'\1\1':
                adjusted = bytearray(DEFAULT)
                adjusted[:2] = packet[10:12]  # preserve caller hint bitmap
                if value == 0x100:
                    self.probe = bytes(adjusted)
                else:
                    if packet[10:] != self.probe:
                        self.send(100, ident, struct.pack('<BBBBHHH', ep, request, rt, 4, value, index, 0))
                        self.log('commit_rejected', reason='does not match negotiated probe')
                        return
                    self.commit, self.committed = self.probe, True
                    self.log('commit', width=WIDTH, height=HEIGHT, interval=INTERVAL,
                             frame_bytes=FRAME_BYTES, payload=PAYLOAD)
                status = 0
        elif self.config and rt == 0xa1 and request == 0x81 and \
                ((index == 0 and value == 0x200) or (index == 1 and value == 0x600)):
            data, status = b'\0', 0
        data = data[:length]
        actual = len(data) if rt & 128 else (length if not status else 0)
        self.send(100, ident, struct.pack('<BBBBHHH', ep, request, rt, status, value, index, actual) + data)
        self.log('control', request=request, request_type=rt, value=value, index=index,
                 requested=length, actual=actual, status=status)

    def drain(self, now=None):
        now = time.monotonic() if now is None else now
        while self.pending and self.config and self.committed:
            if not self.zlp and not self.frame:
                if now < self.next_frame:
                    break
                self.frame = picture(self.sequence)
                self.offset = 0
                self.log('frame_start', sequence=self.sequence, bytes=len(self.frame),
                         sha256=hashlib.sha256(self.frame).hexdigest())
            ident, length = self.pending.popitem(last=False)
            if self.zlp:
                self.zlp = False
                self.bulk_reply(ident, 0x81, 0)
                continue
            if not self.payload:
                # YUY2 payload data starts/ends on a four-byte macropixel.
                end = min(self.offset + PAYLOAD - 2, len(self.frame))
                flags = 0x80 | (self.sequence & 1) | (2 if end == len(self.frame) else 0)
                self.payload = bytes((2, flags)) + self.frame[self.offset:end]
                self.payload_offset = 0
            count = min(length, len(self.payload) - self.payload_offset)
            if length % 64 and length < len(self.payload) - self.payload_offset:
                # Reject a buffer that would split a USB packet, but accept
                # its final short packet (Windows also requests the 62-byte
                # tail after fifteen 64-byte reads of a 1022-byte payload).
                self.bulk_reply(ident, 0x81, 4)
                self.log('unsupported_read_size', length=length)
                continue
            data = self.payload[self.payload_offset:self.payload_offset + count]
            self.payload_offset += count
            self.bulk_reply(ident, 0x81, 0, data)
            if self.payload_offset == len(self.payload):
                self.offset += len(self.payload) - 2
                self.zlp = len(self.payload) % 64 == 0
                self.payload = b''
                if self.offset == len(self.frame):
                    self.log('frame_sent', sequence=self.sequence, bytes=self.offset)
                    self.sequence += 1
                    self.frame = b''
                    self.next_frame = now + INTERVAL / 10000000

    def handle(self, kind, ident, packet):
        if kind == 3 and not packet:
            self.reset(); self.config = 0
        elif kind in (6, 7) and len(packet) == (1 if kind == 6 else 0):
            status = 0
            if kind == 6:
                status = 0 if packet[0] in (0, 1) else 4
                if not status:
                    self.reset(); self.config = packet[0]; self.info()
            self.send(8, ident, bytes((status, self.config)))
        elif kind in (9, 10) and len(packet) == (2 if kind == 9 else 1):
            status = 0 if packet[0] in (0, 1) and (kind == 10 or not packet[1]) else 4
            self.send(11, ident, bytes((status, packet[0], 0)))
        elif kind == 21 and not packet:
            self.cancel(ident)
        elif kind == 100:
            self.control(ident, packet)
        elif kind == 101 and len(packet) == 8:
            ep, _, length, stream = struct.unpack('<BBHI', packet)
            if ep != 0x81 or not self.config or stream:
                self.bulk_reply(ident, ep, 4)
            elif not length or ident in self.pending or len(self.pending) >= 256:
                raise ValueError('invalid/bounded bulk IN')
            else:
                self.pending[ident] = length
        else:
            raise ValueError(f'unsupported usbredir message {kind}')

    def run(self):
        self.send(0, data=b'usb-wdm-free UVC SIMULATED'.ljust(64, b'\0') + struct.pack('<I', CAPS))
        incoming, hello = bytearray(), False
        while True:
            readable, _, _ = select.select([self.connection], [], [], 0.01)
            if readable:
                part = self.connection.recv(65536)
                if not part:
                    raise EOFError
                incoming.extend(part)
            while len(incoming) >= 12:
                kind, length, ident = struct.unpack('<III', incoming[:12])
                if length > 1048586:
                    raise ValueError('usbredir message too large')
                if len(incoming) < 12 + length:
                    break
                packet = bytes(incoming[12:12 + length]); del incoming[:12 + length]
                if not hello:
                    if kind or ident or not 64 <= length <= 4096:
                        raise ValueError('invalid hello')
                    self.peer_caps = struct.unpack('<I', packet[64:68])[0] if length >= 68 else 0
                    self.info()
                    data = struct.pack('<BBBBHH', 1, 0xef, 2, 1, 0xffff, 2)
                    if self.peer_caps & 2:
                        data += struct.pack('<H', 0x100)
                    self.send(1, data=data); hello = True
                    self.log('connected', model='SIMULATED UVC YUY2 bars', vid='ffff', pid='0002')
                else:
                    self.handle(kind, ident, packet)
            self.drain()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', type=int, default=44414)
    ap.add_argument('--log', type=Path, required=True)
    args = ap.parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port)); server.listen(1)
        print(f'SIMULATED UVC listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            try:
                Model(connection, trace).run()
            except (EOFError, ConnectionResetError, BrokenPipeError):
                trace.write(json.dumps(dict(time=time.time(), event='disconnected')) + '\n')


if __name__ == '__main__':
    main()
