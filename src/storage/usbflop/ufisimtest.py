#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests for ufiemu.py (the SIMULATED UFI floppy). No QEMU, no device."""
import io
import struct
import unittest
import zlib

import ufiemu


class Wire:
    def __init__(self):
        self.out = bytearray()

    def sendall(self, data):
        self.out += data


def messages(model, wire):
    out, msgs = bytes(wire.out), []
    wire.out.clear()
    size = 16 if model.id64 else 12
    while out:
        kind, length, ident = struct.unpack('<IIQ' if model.id64 else '<III', out[:size])
        msgs.append((kind, ident, out[size:size + length]))
        out = out[size + length:]
    return msgs


def fat12_read(img, name):
    """A minimal FAT12 reader: the root entry, then the cluster chain."""
    bps, spc, reserved, nfats, nroot = struct.unpack_from('<HBHBH', img, 11)
    spf = struct.unpack_from('<H', img, 22)[0]
    root = (reserved + nfats * spf) * bps
    data = root + nroot * 32
    fat = img[reserved * bps:(reserved + spf) * bps]
    base, ext = name.split('.')
    key = base.ljust(8).encode() + ext.ljust(3).encode()
    for i in range(nroot):
        e = img[root + i * 32:root + i * 32 + 32]
        if e[0:11] == key:
            cluster, size = struct.unpack_from('<HI', e, 26)
            out = bytearray()
            while cluster < 0xff8:
                off = data + (cluster - 2) * spc * bps
                out += img[off:off + spc * bps]
                o = cluster * 3 // 2
                v = fat[o] | fat[o + 1] << 8
                cluster = v >> 4 if cluster & 1 else v & 0xfff
            return bytes(out[:size])
    raise KeyError(name)


class Base(unittest.TestCase):
    cb = False
    speed = 'full'

    def setUp(self):
        self.wire = Wire()
        self.image = ufiemu.fat12_image(ufiemu.default_files())
        self.drive = ufiemu.Drive(self.image)
        self.model = ufiemu.Model(self.wire, io.StringIO(), self.drive, self.speed, self.cb)
        self.model.peer_caps = ufiemu.CAPS
        self.model.connect()
        messages(self.model, self.wire)
        self.model.handle(6, 1, bytes((1,)))
        if not self.cb:
            self.model.handle(15, 2, bytes((ufiemu.EP_INT,)))
        messages(self.model, self.wire)
        self.ident = 100

    def next_id(self):
        self.ident += 1
        return self.ident

    def adsc(self, cdb):
        cdb = bytes(cdb).ljust(12, b'\0')
        self.model.handle(100, self.next_id(), struct.pack('<BBBBHHH', 0, 0, 0x21, 0, 0, 0, 12) + cdb)

    def bulk_in(self, length):
        self.model.handle(101, self.next_id(), struct.pack('<BBHIH', ufiemu.EP_IN, 0, length & 0xffff, 0, length >> 16))

    def bulk_out(self, data):
        n = len(data)
        self.model.handle(101, self.next_id(), struct.pack('<BBHIH', ufiemu.EP_OUT, 0, n & 0xffff, 0, n >> 16) + data)

    def run_in(self, cdb, length):
        self.adsc(cdb)
        self.bulk_in(length)
        msgs = messages(self.model, self.wire)
        control = [m for m in msgs if m[0] == 100]
        self.assertEqual(control[0][2][3], 0)
        data = b''.join(m[2][10:] for m in msgs if m[0] == 101)
        status = [m[2][4:] for m in msgs if m[0] == 103]
        return data, status


class Cbi(Base):
    def test_configuration_descriptor_is_ufi_cbi(self):
        c = self.model.config_desc
        self.assertEqual(c[9:18], bytes((9, 4, 0, 0, 3, 8, 4, 0, 0)))
        eps = [c[i + 2] for i in range(18, len(c), 7)]
        self.assertEqual(eps, [0x81, 0x02, 0x83])
        self.assertEqual(struct.unpack_from('<H', c, 2)[0], len(c))

    def test_inquiry_and_interrupt_status(self):
        data, status = self.run_in(b'\x12\0\0\0\x24', 36)
        self.assertEqual(len(data), 36)
        self.assertEqual(data[0] & 0x1f, 0)
        self.assertEqual(data[1], 0x80)          # removable
        self.assertEqual(status, [b'\0\0'])

    def test_capacity(self):
        data, _ = self.run_in(b'\x25', 8)
        self.assertEqual(struct.unpack('>II', data), (2879, 512))
        data, _ = self.run_in(b'\x23\0\0\0\0\0\0\0\xfc', 0xfc)
        self.assertEqual(data[3], 24)
        self.assertEqual(struct.unpack('>I', data[4:8])[0], 2880)
        self.assertEqual(data[8], 2)             # formatted medium

    def test_mode_sense_flexible_disk_page(self):
        data, _ = self.run_in(b'\x5a\0\x05\0\0\0\0\0\xc0', 0xc0)
        self.assertEqual(data[2], 0x94)          # 1.44 MB medium type
        self.assertEqual(data[8:10], b'\x05\x1e')
        self.assertEqual(struct.unpack('>HBBHH', data[10:18]), (500, 2, 18, 512, 80))

    def test_read_whole_disk_matches_image(self):
        data, status = self.run_in(b'\x28\0\0\0\0\0\0\x0b\x40', 2880 * 512)
        self.assertEqual(data, self.image)
        self.assertEqual(status, [b'\0\0'])

    def test_read_in_packet_sized_pieces(self):
        self.adsc(b'\x28\0\0\0\0\x13\0\0\x02')
        for _ in range(16):
            self.bulk_in(64)
        msgs = messages(self.model, self.wire)
        data = b''.join(m[2][10:] for m in msgs if m[0] == 101)
        self.assertEqual(data, self.image[19 * 512:21 * 512])
        self.assertEqual([m for m in msgs if m[0] == 103][-1][2][4:], b'\0\0')

    def test_out_of_range_read_gives_sense(self):
        _, status = self.run_in(b'\x28\0\0\0\x0b\x40\0\0\x01', 512)
        self.assertEqual(status, [b'\x21\0'])
        data, _ = self.run_in(b'\x03\0\0\0\x12', 18)
        self.assertEqual((data[2], data[12]), (5, 0x21))

    def test_write_then_read(self):
        block = bytes(range(256)) * 2
        self.adsc(b'\x2a\0\0\0\x01\x00\0\0\x01')
        self.bulk_out(block)
        status = [m[2][4:] for m in messages(self.model, self.wire) if m[0] == 103]
        self.assertEqual(status, [b'\0\0'])
        data, _ = self.run_in(b'\x28\0\0\0\x01\x00\0\0\x01', 512)
        self.assertEqual(data, block)

    def test_write_protect(self):
        self.drive.wp = True
        data, _ = self.run_in(b'\x5a\0\x3f\0\0\0\0\0\xc0', 0xc0)
        self.assertEqual(data[3] & 0x80, 0x80)
        self.adsc(b'\x2a\0\0\0\x01\x00\0\0\x01')
        status = [m[2][4:] for m in messages(self.model, self.wire) if m[0] == 103]
        self.assertEqual(status, [b'\x27\0'])

    def test_unit_attention_once_after_reset(self):
        self.drive.ua_option = True
        self.model.handle(3, 0, b'')
        self.model.handle(6, 1, bytes((1,)))
        self.model.handle(15, 2, bytes((ufiemu.EP_INT,)))
        messages(self.model, self.wire)
        _, status = self.run_in(b'\x00', 0)
        self.assertEqual(status, [b'\x28\0'])
        _, status = self.run_in(b'\x00', 0)
        self.assertEqual(status, [b'\0\0'])

    def test_unknown_opcode(self):
        _, status = self.run_in(b'\xee', 0)
        self.assertEqual(status, [b'\x20\0'])

    def test_status_waits_for_interrupt_start(self):
        self.model.handle(16, 3, bytes((ufiemu.EP_INT,)))
        messages(self.model, self.wire)
        self.adsc(b'\x00')
        self.assertFalse([m for m in messages(self.model, self.wire) if m[0] == 103])
        self.model.handle(15, 4, bytes((ufiemu.EP_INT,)))
        self.assertEqual([m[2][4:] for m in messages(self.model, self.wire) if m[0] == 103], [b'\0\0'])

    def test_status_survives_qemu_stop(self):
        # QEMU frees buffered interrupt data on stop; the status comes again on the next start.
        self.adsc(b'\x00')
        self.assertEqual([m[2][4:] for m in messages(self.model, self.wire) if m[0] == 103], [b'\0\0'])
        self.model.handle(16, 5, bytes((ufiemu.EP_INT,)))
        self.model.handle(15, 6, bytes((ufiemu.EP_INT,)))
        self.assertEqual([m[2][4:] for m in messages(self.model, self.wire) if m[0] == 103], [b'\0\0'])
        # The next command clears it: no stale block for the next status.
        self.model.handle(16, 7, bytes((ufiemu.EP_INT,)))
        _, status = self.run_in(b'\xee', 0)
        self.model.handle(15, 8, bytes((ufiemu.EP_INT,)))
        status += [m[2][4:] for m in messages(self.model, self.wire) if m[0] == 103]
        self.assertEqual(status, [b'\x20\0'])

    def test_failed_command_stalls_the_data_pipe(self):
        # Windows 7 sends MODE SENSE(6), which UFI does not have: the host's bulk IN must stall.
        self.adsc(b'\x1a\0\x05\0\xc0')
        self.bulk_in(0xc0)
        msgs = messages(self.model, self.wire)
        self.assertEqual([m[2][1] for m in msgs if m[0] == 101], [4])
        self.assertEqual([m[2][4:] for m in msgs if m[0] == 103], [b'\x20\0'])
        # CLEAR_FEATURE(ENDPOINT_HALT) on 81, then the next command moves data again.
        self.model.handle(100, self.next_id(), struct.pack('<BBBBHHH', 0, 1, 2, 0, 0, 0x81, 0))
        messages(self.model, self.wire)
        data, status = self.run_in(b'\x25', 8)
        self.assertEqual(struct.unpack('>II', data), (2879, 512))
        self.assertEqual(status, [b'\0\0'])


class Cb(Base):
    cb = True

    def test_cb_has_no_interrupt_endpoint(self):
        c = self.model.config_desc
        self.assertEqual(c[9:18], bytes((9, 4, 0, 0, 2, 8, 4, 1, 0)))
        _, status = self.run_in(b'\x00', 0)
        self.assertEqual(status, [])


class HighSpeed(Base):
    speed = 'high'

    def test_high_speed_shape(self):
        self.assertEqual(self.model.device[2:4], b'\x00\x02')
        self.assertEqual(struct.unpack_from('<H', self.model.config_desc, 22)[0], 512)


class Medium(unittest.TestCase):
    def test_files_and_crc_list(self):
        img = ufiemu.fat12_image(ufiemu.default_files())
        self.assertEqual(len(img), 1474560)
        self.assertEqual(img[510:512], b'\x55\xaa')
        crc = fat12_read(img, 'CRC.TXT').decode()
        for line in crc.strip().splitlines():
            name, value, size = line.split()
            content = fat12_read(img, name)
            self.assertEqual(len(content), int(size))
            self.assertEqual('%08X' % zlib.crc32(content), value)
        self.assertEqual(fat12_read(img, 'PATTERN.BIN'), ufiemu.pattern(200 * 1024))


if __name__ == '__main__':
    unittest.main()
