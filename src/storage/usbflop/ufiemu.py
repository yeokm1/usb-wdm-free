#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""SIMULATED USB floppy drive (UFI command set over CBI) on localhost
usbredir (test aid only).

usbflop is an INF that binds Windows' own storage driver to UFI floppies
by class (08/04/00 CBI, 08/04/01 CB). The owner's Mitsumi is listed by
Microsoft's INFs, so it never exercises the automatic class match this
package exists for, and it is Full Speed only. This model presents an
UNLISTED drive (default FFFF:0020) of class 08/04/00 or, with --cb,
08/04/01, at Full Speed or (as a test shape no real floppy has) High
Speed, with a 1.44 MB medium.

Original model from the USB Mass Storage Class UFI Command Specification
1.0 (INQUIRY, READ FORMAT CAPACITIES, READ CAPACITY, MODE SENSE(10) with
the flexible disk page, READ/WRITE(10/12), REQUEST SENSE, TEST UNIT READY,
START STOP, PREVENT ALLOW, VERIFY, FORMAT UNIT, MODE SELECT), the CBI
Transport Specification 1.1 (ADSC on endpoint 0, bulk data phase, the UFI
two-byte ASC/ASCQ interrupt status), USB 2.0 chapter 9 and SPICE's
usb-redirection-protocol.txt v0.7. No physical drive is accessed.

The medium is a FAT12 1.44 MB image built in memory (--image FILE uses a
2880-sector image instead): label SIMFLOP, files README.TXT, PATTERN.BIN
(200 KiB of a known byte pattern) and CRC.TXT (the CRC-32 of each file),
so a guest read can be checked against CRC.TXT. --write-protect sets the
WP bit; --unit-attention reports a medium change (06/28/00) once after
each reset. Writes change the in-memory image only (--save FILE keeps it).
"""
import argparse
import collections
import hashlib
import json
import socket
import struct
import time
import zlib
from pathlib import Path

CAP_VERSION, CAP_EP_MAX, CAP_ID64, CAP_BULK32 = 1, 4, 5, 6
CAPS = (1 << CAP_VERSION) | (1 << CAP_EP_MAX) | (1 << CAP_ID64) | (1 << CAP_BULK32)
EP_IN, EP_OUT, EP_INT = 0x81, 0x02, 0x83
SECTORS, SECTOR = 2880, 512


def pattern(n):
    return bytes((i * 7 + (i >> 9)) & 255 for i in range(n))


def fat12_image(files, label=b'SIMFLOP    '):
    """A 1.44 MB FAT12 image (512-byte sectors, 1 sector per cluster, 2
    FATs of 9 sectors, 224 root entries) holding files {8.3 name: bytes}."""
    img = bytearray(SECTORS * SECTOR)
    boot = bytearray(512)
    boot[0:3] = b'\xeb\x3c\x90'
    boot[3:11] = b'SIMFLOP '
    struct.pack_into('<HBHBHHBHHHII', boot, 11, 512, 1, 1, 2, 224, SECTORS, 0xf0, 9, 18, 2, 0, 0)
    struct.pack_into('<BBBI', boot, 36, 0, 0, 0x29, 0x20261023)
    boot[43:54] = label
    boot[54:62] = b'FAT12   '
    boot[510:512] = b'\x55\xaa'
    img[0:512] = boot
    fat = bytearray(9 * SECTOR)
    entries = [0xff0, 0xfff]
    root = bytearray(224 * 32)
    root[0:11] = label
    root[11] = 0x08                       # volume label
    data_start = 1 + 2 * 9 + 14           # first data sector (cluster 2)
    cluster = 2
    for slot, (name, content) in enumerate(files.items(), 1):
        base, ext = name.split('.')
        e = root[slot * 32:slot * 32 + 32]
        e[0:8] = base.ljust(8).encode('ascii')
        e[8:11] = ext.ljust(3).encode('ascii')
        e[11] = 0x20
        struct.pack_into('<HHHI', e, 22, 0x6000, 0x5b57, cluster if content else 0, len(content))
        root[slot * 32:slot * 32 + 32] = e
        count = -(-len(content) // SECTOR)
        for i in range(count):
            entries.append(cluster + i + 1 if i + 1 < count else 0xfff)
            off = (data_start + cluster - 2 + i) * SECTOR
            img[off:off + SECTOR] = content[i * SECTOR:(i + 1) * SECTOR].ljust(SECTOR, b'\0')
        cluster += count
    for i, v in enumerate(entries):
        o = i * 3 // 2
        if i & 1:
            fat[o] = (fat[o] & 0x0f) | ((v << 4) & 0xf0)
            fat[o + 1] = (v >> 4) & 0xff
        else:
            fat[o] = v & 0xff
            fat[o + 1] = (fat[o + 1] & 0xf0) | ((v >> 8) & 0x0f)
    for copy in range(2):
        img[(1 + copy * 9) * SECTOR:(1 + copy * 9) * SECTOR + len(fat)] = fat
    img[19 * SECTOR:19 * SECTOR + len(root)] = root
    return img


def default_files():
    readme = (b'usb-wdm-free SIMULATED UFI floppy (ufiemu.py).\r\n'
              b'PATTERN.BIN is (i*7 + i/512) mod 256; CRC.TXT has each file\'s CRC-32.\r\n')
    pat = pattern(200 * 1024)
    crc = ('README.TXT %08X %d\r\nPATTERN.BIN %08X %d\r\n' % (
        zlib.crc32(readme), len(readme), zlib.crc32(pat), len(pat))).encode('ascii')
    return {'README.TXT': readme, 'PATTERN.BIN': pat, 'CRC.TXT': crc}


def usb_string(text):
    raw = text.encode('utf-16le')
    return bytes((len(raw) + 2, 3)) + raw


class Drive:
    """The UFI command set over a medium image."""

    def __init__(self, image, write_protect=False, unit_attention=False):
        self.image = bytearray(image)
        self.wp, self.ua_option = write_protect, unit_attention
        self.reset()

    def reset(self):
        self.sense = (0, 0, 0)
        self.attention = self.ua_option
        self.prevent = False

    def check(self, key, asc, ascq=0):
        self.sense = (key, asc, ascq)
        return asc, ascq

    def command(self, cdb):
        """Returns (direction 'in'/'out'/None, data or expected length, status)."""
        op = cdb[0]
        if op != 0x03 and self.attention:
            self.attention = False
            return None, b'', self.check(6, 0x28)
        if op == 0x03:                                    # REQUEST SENSE
            key, asc, ascq = self.sense
            self.sense = (0, 0, 0)
            data = bytes((0x70, 0, key, 0, 0, 0, 0, 10, 0, 0, 0, 0, asc, ascq, 0, 0, 0, 0))
            return 'in', data[:cdb[4]], (0, 0)
        if op == 0x12:                                    # INQUIRY
            data = bytes((0, 0x80, 0, 1, 31, 0, 0, 0)) + b'SIMULATD' + b'UFI FLOPPY SIM  ' + b'0.01'
            return 'in', data[:cdb[4]], self.ok()
        if op == 0x00:                                    # TEST UNIT READY
            return None, b'', self.ok()
        if op == 0x23:                                    # READ FORMAT CAPACITIES
            alloc = struct.unpack('>H', cdb[7:9])[0]
            d = lambda n, code: struct.pack('>IB', n, code) + (512).to_bytes(3, 'big')
            data = bytes((0, 0, 0, 24)) + d(SECTORS, 0x02) + d(SECTORS, 0) + d(1440, 0)
            return 'in', data[:alloc], self.ok()
        if op == 0x25:                                    # READ CAPACITY
            return 'in', struct.pack('>II', SECTORS - 1, SECTOR), self.ok()
        if op == 0x5a:                                    # MODE SENSE(10)
            page, alloc = cdb[2] & 0x3f, struct.unpack('>H', cdb[7:9])[0]
            flex = bytes((0x05, 0x1e)) + struct.pack('>HBBHH', 500, 2, 18, 512, 80) + bytes(10) + \
                bytes((5, 30, 0, 0, 0, 0, 0)) + bytes(2) + struct.pack('>H', 360) + bytes(2)
            flex = flex[:32].ljust(32, b'\0')
            fault = bytes((0x01, 0x0a, 0, 3)) + bytes(8)
            timer = bytes((0x1c, 0x06, 0, 5)) + bytes(4)
            pages = {0x05: flex, 0x01: fault, 0x1c: timer}
            if page == 0x3f:
                body = fault + flex + timer
            elif page in pages:
                body = pages[page]
            else:
                return None, b'', self.check(5, 0x24)
            data = struct.pack('>HBB', 6 + len(body), 0x94, 0x80 if self.wp else 0) + bytes(4) + body
            return 'in', data[:alloc], self.ok()
        if op in (0x28, 0xa8, 0x2a, 0xaa, 0x2f):
            lba = struct.unpack('>I', cdb[2:6])[0]
            count = struct.unpack('>H', cdb[7:9])[0] if op in (0x28, 0x2a, 0x2f) else struct.unpack('>I', cdb[6:10])[0]
            if lba + count > SECTORS:
                return None, b'', self.check(5, 0x21)
            if op in (0x28, 0xa8):
                return 'in', bytes(self.image[lba * SECTOR:(lba + count) * SECTOR]), self.ok()
            if op == 0x2f:
                return None, b'', self.ok()
            if self.wp:
                return None, b'', self.check(7, 0x27)
            return 'out', (lba, count * SECTOR), None
        if op in (0x1b, 0x1d, 0x01, 0x2b):                # START STOP, SEND DIAGNOSTIC, REZERO, SEEK
            return None, b'', self.ok()
        if op == 0x1e:                                    # PREVENT ALLOW
            self.prevent = bool(cdb[4] & 1)
            return None, b'', self.ok()
        if op == 0x04:                                    # FORMAT UNIT: parameter list follows
            if self.wp:
                return None, b'', self.check(7, 0x27)
            return 'out', (None, struct.unpack('>H', cdb[7:9])[0]), None
        if op == 0x55:                                    # MODE SELECT(10)
            return 'out', (None, struct.unpack('>H', cdb[7:9])[0]), None
        return None, b'', self.check(5, 0x20)

    def ok(self):
        self.sense = (0, 0, 0)
        return 0, 0

    def write(self, lba, data):
        if lba is not None:
            self.image[lba * SECTOR:lba * SECTOR + len(data)] = data
        return self.ok()


class Model:
    def __init__(self, connection, trace, drive, speed='full', cb=False, vid=0xffff, pid=0x0020,
                 now=time.monotonic):
        self.connection, self.trace, self.drive, self.now = connection, trace, drive, now
        self.speed, self.cb = speed, cb
        self.vid, self.pid = vid, pid
        self.bulk = 512 if speed == 'high' else 64
        self.device = struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x200 if speed == 'high' else 0x110, 0, 0, 0, 64,
                                  vid, pid, 0x0100, 1, 2, 3, 1)
        self.config_desc = self.configuration(speed)
        self.strings = {0: b'\x04\x03\x09\x04', 1: usb_string('usb-wdm-free SIMULATED'),
                        2: usb_string('UFI floppy - SIMULATED'), 3: usb_string('SIM0020')}
        self.peer_caps = 0
        self.verbose = False
        self.config = 0
        self.interrupt = False
        self.pending = collections.OrderedDict()
        self.status = collections.deque()
        self.inflight = None    # the status block last handed to QEMU, until the next ADSC
        self.reset_transport()
        self.totals = collections.Counter()

    def configuration(self, speed, kind=2):
        bulk = 512 if speed == 'high' else 64
        neps = 2 if self.cb else 3
        body = bytes((9, 4, 0, 0, neps, 8, 4, 1 if self.cb else 0, 0))
        body += bytes((7, 5, EP_IN, 2)) + struct.pack('<HB', bulk, 0)
        body += bytes((7, 5, EP_OUT, 2)) + struct.pack('<HB', bulk, 0)
        if not self.cb:
            body += bytes((7, 5, EP_INT, 3)) + struct.pack('<HB', 2, 8 if speed == 'high' else 32)
        return bytes((9, kind)) + struct.pack('<HBBBBB', 9 + len(body), 1, 1, 0, 0x80, 50) + body

    def reset_transport(self):
        self.halted = set()     # bulk endpoints stalled after a failed command
        self.data_in = b''
        self.out_left = 0
        self.out_lba = None
        self.out_buf = bytearray()

    # -- plumbing ----------------------------------------------------------
    def log(self, event, **fields):
        self.trace.write(json.dumps(dict(time=time.time(), event=event, simulated=True, **fields)) + '\n')
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
            for slot, kind, interval, size in ((16 + (EP_IN & 15), 2, 0, self.bulk), (EP_OUT, 2, 0, self.bulk)) + \
                    ((((16 + (EP_INT & 15), 3, 8 if self.speed == 'high' else 32, 2),) if not self.cb else ())):
                types[slot], intervals[slot], packets[slot] = kind, interval, size
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << CAP_EP_MAX):
            data += struct.pack('<32H', *packets)
        self.send(5, data=data)
        numbers, classes, subclasses, protocols = (bytearray(32) for _ in range(4))
        classes[0], subclasses[0], protocols[0] = 8, 4, 1 if self.cb else 0
        self.send(4, data=struct.pack('<I', 1 if self.config else 0) + numbers + classes + subclasses + protocols)

    # -- the CBI transport -----------------------------------------------------
    def adsc(self, cdb):
        cdb = bytes(cdb).ljust(12, b'\0')
        # A new command means the host has the previous status (or gave up on it).
        self.status.clear()
        self.inflight = None
        if self.data_in or self.out_left:
            self.log('adsc_while_busy', opcode=cdb[0])
        self.reset_transport()
        direction, data, status = self.drive.command(cdb)
        self.totals['commands'] += 1
        self.log('command', opcode='%02x' % cdb[0], cdb=cdb.hex(), direction=direction,
                 length=len(data) if direction == 'in' else (data[1] if direction == 'out' else 0),
                 status=status)
        if direction == 'in':
            self.data_in = data
            if not data:
                self.complete(status)
            else:
                self.pending_status = status
        elif direction == 'out':
            self.out_lba, self.out_left = data
            self.out_buf = bytearray()
            if not self.out_left:
                self.complete(self.drive.ok())
        else:
            if status != (0, 0):
                # CBI 1.1: a command that fails before its data phase stalls
                # the bulk pipe, so a host waiting to move data sees the
                # failure (then reads the status and clears the halt).
                self.halted = {EP_IN, EP_OUT}
                for ident in list(self.pending):
                    self.send(101, ident, self.bulk_header(EP_IN, 4, 0))
                self.pending.clear()
            self.complete(status)
        self.drain()

    def complete(self, status):
        """CBI status: the UFI interrupt data block is ASC, ASCQ."""
        if not self.cb:
            self.status.append(bytes(status))
            self.flush_status()

    def flush_status(self):
        while self.interrupt and self.status:
            data = self.status.popleft()
            self.inflight = data
            self.send(103, data=struct.pack('<BBH', EP_INT, 0, 2) + data)

    def interrupt_stopped(self):
        """QEMU frees the interrupt packets it buffered when receiving stops
        (its UHCI cancels an idle interrupt TD, and usb-redir then sends stop).
        A real device keeps its status until an IN token takes it, so the
        status handed over since the last command is offered again on the
        next start. A start only comes from a guest IN packet, and the guest
        posts one only while it waits for this status, so this cannot give
        the next command a stale block: the next ADSC clears both."""
        if self.inflight is not None and not self.status:
            self.status.append(self.inflight)
            self.log('status_requeued', status=list(self.inflight))
        self.inflight = None

    def drain(self):
        while self.pending and self.data_in:
            ident, length = next(iter(self.pending.items()))
            del self.pending[ident]
            chunk = self.data_in[:length]
            self.data_in = self.data_in[length:]
            self.send(101, ident, self.bulk_header(EP_IN, 0, len(chunk)) + chunk)
            self.totals['bytes_in'] += len(chunk)
            if not self.data_in:
                self.complete(self.pending_status)

    def bulk_out(self, data):
        if not self.out_left:
            self.log('bulk_out_unexpected', length=len(data))
            return
        take = data[:self.out_left]
        self.out_buf += take
        self.out_left -= len(take)
        if not self.out_left:
            status = self.drive.write(self.out_lba, bytes(self.out_buf))
            self.totals['bytes_out'] += len(self.out_buf)
            if self.out_lba is not None:
                self.log('write', lba=self.out_lba, length=len(self.out_buf),
                         sha256=hashlib.sha256(self.out_buf).hexdigest())
            self.complete(status)

    # -- control and usbredir ------------------------------------------------
    def control(self, ident, packet):
        if len(packet) < 10:
            raise ValueError('short control header')
        ep, request, rt, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        payload = packet[10:]
        if len(payload) != (0 if rt & 128 else length):
            raise ValueError('control length mismatch')
        status, data = 4, b''
        if rt == 0x80 and request == 6:
            kind, number = value >> 8, value & 255
            data = self.device if kind == 1 and not number else \
                self.config_desc if kind == 2 and not number else \
                self.strings.get(number) if kind == 3 else None
            status, data = (0, data) if data is not None else (4, b'')
        elif request == 0 and rt in (0x80, 0x81, 0x82):
            status, data = 0, b'\0\0'
        elif request == 8 and rt == 0x80:
            status, data = 0, bytes((self.config,))
        elif request == 10 and rt == 0x81:
            status, data = 0, b'\0'
        elif request == 1 and rt == 2 and value == 0:          # CLEAR_FEATURE(ENDPOINT_HALT)
            status = 0
            self.halted.discard(index & 0xff)
        elif rt == 0x21 and request == 0 and index == 0 and self.config:   # ADSC
            status = 0
            self.adsc(payload)
        data = data[:length]
        actual = len(data) if rt & 128 else (0 if status else length)
        self.send(100, ident, struct.pack('<BBBBHHH', ep, request, rt, status, value, index, actual) + data)

    def handle(self, kind, ident, packet):
        if self.verbose and kind != 100:
            self.log('usbredir', kind=kind, id=ident, head=packet[:12].hex())
        if kind == 3:
            for i in list(self.pending):
                self.send(101, i, self.bulk_header(EP_IN, 1, 0))
            self.pending.clear()
            self.status.clear()
            self.inflight = None
            self.reset_transport()
            self.drive.reset()
            self.config, self.interrupt = 0, False
            self.log('reset')
        elif kind in (6, 7):
            status = 0
            if kind == 6:
                status = 0 if packet[0] in (0, 1) else 4
                if not status:
                    self.config = packet[0]
                    self.info()
                    self.log('configuration', value=self.config)
            self.send(8, ident, bytes((status, self.config)))
        elif kind in (9, 10):
            ok = self.config and packet[0] == 0 and (kind == 10 or packet[1] == 0)
            self.send(11, ident, bytes((0 if ok else 4, packet[0], 0)))
        elif kind in (15, 16):
            ok = packet[0] == EP_INT and self.config and not self.cb
            if kind == 16 and self.interrupt:
                self.interrupt_stopped()
            self.interrupt = kind == 15 and bool(ok)
            self.send(17, ident, bytes((0 if ok else 4, packet[0])))
            self.flush_status()
        elif kind == 21:
            self.log('cancel', id=ident, pending=ident in self.pending)
            if ident in self.pending:
                del self.pending[ident]
                self.send(101, ident, self.bulk_header(EP_IN, 1, 0))
        elif kind == 24:
            pass
        elif kind == 100:
            self.control(ident, packet)
        elif kind == 101:
            size = 10 if self.len32 else 8
            endpoint, _, length, stream = struct.unpack('<BBHI', packet[:8])
            if self.len32:
                length |= struct.unpack('<H', packet[8:10])[0] << 16
            if not self.config or endpoint not in (EP_IN, EP_OUT):
                self.send(101, ident, self.bulk_header(endpoint, 4, 0))
            elif endpoint in self.halted:
                self.send(101, ident, self.bulk_header(endpoint, 4, 0))
            elif endpoint == EP_OUT:
                data = packet[size:]
                self.send(101, ident, self.bulk_header(EP_OUT, 0, length))
                self.bulk_out(data)
            else:
                self.pending[ident] = length
                self.drain()
        else:
            raise ValueError(f'unsupported usbredir message {kind}')

    def connect(self):
        self.info()
        data = struct.pack('<BBBBHH', 2 if self.speed == 'high' else 1, 0, 0, 0, self.vid, self.pid)
        if self.peer_caps & (1 << CAP_VERSION):
            data += struct.pack('<H', 0x0100)
        self.send(1, data=data)
        self.log('connected', model='SIMULATED UFI floppy', speed=self.speed, cb=self.cb,
                 vid='%04x' % self.vid, pid='%04x' % self.pid,
                 image_sha256=hashlib.sha256(self.drive.image).hexdigest())

    def run(self):
        self.send(0, data=b'usb-wdm-free UFI SIMULATED'.ljust(64, b'\0') + struct.pack('<I', CAPS))
        buffer, hello = bytearray(), False
        while True:
            data = self.connection.recv(65536)
            if not data:
                raise EOFError
            buffer.extend(data)
            while True:
                size = 16 if self.id64 else 12
                if len(buffer) < size:
                    break
                kind, length, ident = struct.unpack('<IIQ' if self.id64 else '<III', buffer[:size])
                if len(buffer) < size + length:
                    break
                packet = bytes(buffer[size:size + length])
                del buffer[:size + length]
                if not hello:
                    self.peer_caps = int.from_bytes(packet[64:68], 'little') if length >= 68 else 0
                    self.connect()
                    hello = True
                else:
                    self.handle(kind, ident, packet)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--log', type=Path, required=True)
    ap.add_argument('--speed', choices=('full', 'high'), default='full')
    ap.add_argument('--cb', action='store_true', help='CB (08/04/01, no interrupt endpoint) instead of CBI')
    ap.add_argument('--vid', type=lambda s: int(s, 16), default=0xffff)
    ap.add_argument('--pid', type=lambda s: int(s, 16), default=0x0020)
    ap.add_argument('--image', type=Path, help='a 1474560-byte image instead of the built one')
    ap.add_argument('--save', type=Path, help='write the image here at the end')
    ap.add_argument('--write-protect', action='store_true')
    ap.add_argument('--unit-attention', action='store_true')
    ap.add_argument('--verbose', action='store_true', help='log every usbredir message but control')
    args = ap.parse_args()
    image = args.image.read_bytes() if args.image else fat12_image(default_files())
    if len(image) != SECTORS * SECTOR:
        raise SystemExit('image must be 1474560 bytes')
    drive = Drive(image, args.write_protect, args.unit_attention)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'SIMULATED UFI floppy listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            model = Model(connection, trace, drive, args.speed, args.cb, args.vid, args.pid)
            model.verbose = args.verbose
            try:
                model.run()
            except (EOFError, ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
                model.log('disconnected', **model.totals)
            finally:
                if args.save:
                    args.save.write_bytes(drive.image)


if __name__ == '__main__':
    main()
