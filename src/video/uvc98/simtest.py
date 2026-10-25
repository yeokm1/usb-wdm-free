#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host integration tests: synthetic USB wire model through owned C cores."""
import ctypes as c
import io
import json
import struct
import unittest
from pathlib import Path
import uvcemu as u


class Connection:
    def __init__(self):
        self.messages = []

    def sendall(self, message):
        kind, count, ident = struct.unpack('<III', message[:12])
        assert count == len(message) - 12
        self.messages.append((kind, ident, message[12:]))


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = Path(__file__).resolve().parents[3] / 'out/video/uvc98/hostabi/uvccore.dll'
        cls.dll = c.CDLL(str(path))
        cls.callback_type = c.CFUNCTYPE(None, c.c_void_p, c.c_void_p, c.c_uint, c.c_uint)
        cls.dll.UvcDescParse.argtypes = [c.c_void_p, c.c_uint, c.c_uint, c.c_uint, c.c_void_p]
        cls.dll.UvcDescParse.restype = c.c_int
        cls.dll.UvcPayInit.argtypes = [c.c_void_p, c.c_void_p, c.c_uint, c.c_uint,
                                       cls.callback_type, c.c_void_p]
        cls.dll.UvcPayInit.restype = c.c_int
        cls.dll.UvcPayFeed.argtypes = [c.c_void_p, c.c_void_p, c.c_uint]
        cls.dll.UvcPayFeed.restype = None
        cls.dll.UvcTestPaySize.restype = cls.dll.UvcTestDescSize.restype = c.c_uint

    def model(self):
        connection, log = Connection(), io.StringIO()
        return u.Model(connection, log), connection, log

    def control(self, model, request, rt, value, index=1, payload=b'', length=None):
        if length is None:
            length = len(payload)
        model.control(42, struct.pack('<BBBBHHH', 0x80 if rt & 128 else 0,
            request, rt, 0, value, index, length) + payload)
        return model.connection.messages[-1][2]

    def test_descriptors_through_c(self):
        self.assertEqual(len(u.DEFAULT), 34)
        self.assertEqual(int.from_bytes(u.CONFIG[2:4], 'little'), len(u.CONFIG))
        model = c.create_string_buffer(self.dll.UvcTestDescSize())
        blob = c.create_string_buffer(u.CONFIG)
        self.assertEqual(self.dll.UvcDescParse(blob, len(u.CONFIG), 0, 1, model), 0)
        self.assertNotEqual(self.dll.UvcDescParse(blob, len(u.CONFIG), 1, 1, model), 0)

    def test_control_negotiation(self):
        m, conn, _ = self.model()
        m.handle(6, 1, b'\1')
        response = self.control(m, 0x85, 0xa1, 0x100, length=2)
        self.assertEqual(response[3], 0)
        self.assertEqual(response[10:], b'\x22\0')
        wanted = bytearray(u.DEFAULT)
        wanted[4:8] = struct.pack('<I', 333333)
        wanted[18:26] = bytes(8)
        self.assertEqual(self.control(m, 1, 0x21, 0x100, payload=wanted)[3], 0)
        self.assertEqual(self.control(m, 1, 0x21, 0x200, payload=wanted)[3], 4)
        self.assertFalse(m.committed)
        actual = self.control(m, 0x81, 0xa1, 0x100, length=34)[10:]
        self.assertEqual(actual, u.DEFAULT)
        self.assertEqual(self.control(m, 1, 0x21, 0x200, payload=actual)[3], 0)
        self.assertTrue(m.committed)
        wanted[2] = 2
        self.assertEqual(self.control(m, 1, 0x21, 0x100, payload=wanted)[3], 4)
        self.assertEqual(self.control(m, 1, 0x21, 0x100, payload=bytes(26))[3], 4)
        self.assertFalse(any(kind == 101 for kind, _, _ in conn.messages))

    def test_three_frames_through_c(self):
        for request_size in (64, 128, 512, u.PAYLOAD, 4096, 65535):
            m, conn, _ = self.model()
            m.config, m.committed = 1, True
            assembled = []
            callback = self.callback_type(lambda _, pointer, count, still:
                assembled.append((c.string_at(pointer, count), still)))
            state = c.create_string_buffer(self.dll.UvcTestPaySize())
            frame = c.create_string_buffer(u.FRAME_BYTES)
            self.assertEqual(self.dll.UvcPayInit(state, frame, u.FRAME_BYTES,
                u.FRAME_BYTES, callback, None), 1)
            payload = bytearray()
            for ident in range(1, 10000):
                m.handle(101, ident, struct.pack('<BBHI', 0x81, 0, request_size, 0))
                m.drain(now=ident / 1000)
                while conn.messages:
                    kind, _, packet = conn.messages.pop(0)
                    self.assertEqual(kind, 101)
                    ep, status, actual, stream = struct.unpack('<BBHI', packet[:8])
                    self.assertEqual((ep, status, stream), (0x81, 0, 0))
                    self.assertEqual(actual, len(packet) - 8)
                    self.assertLessEqual(actual, request_size)
                    payload.extend(packet[8:])
                    self.assertLessEqual(len(payload), u.PAYLOAD)
                    if len(payload) == u.PAYLOAD or actual < request_size:
                        if payload:
                            self.assertEqual((len(payload) - payload[0]) % 4, 0)
                            wire = c.create_string_buffer(bytes(payload))
                            self.dll.UvcPayFeed(state, wire, len(payload))
                        payload.clear()
                if m.sequence == 3:
                    break
            self.assertEqual(m.sequence, 3)
            self.assertEqual(assembled, [(u.picture(1), 0), (u.picture(2), 0)])
            self.assertNotEqual(u.picture(1), u.picture(2))

    def test_cancel_reset_and_bounds(self):
        m, conn, _ = self.model()
        m.config = 1
        request = struct.pack('<BBHI', 0x81, 0, 4096, 0)
        for ident in range(256):
            m.handle(101, ident, request)
        with self.assertRaises(ValueError):
            m.handle(101, 256, request)
        m.cancel(10)
        self.assertNotIn(10, m.pending)
        self.assertEqual(conn.messages[-1][2][:4], b'\x81\1\0\0')
        m.reset()
        self.assertFalse(m.pending)
        self.assertFalse(m.committed)
        for _, _, message in conn.messages:
            self.assertEqual(message[1], 1)

    def test_descriptor_only_no_commit(self):
        m, conn, log = self.model()
        m.config = 1
        self.control(m, 6, 0x80, 0x200, index=0, length=65535)
        m.handle(101, 1, struct.pack('<BBHI', 0x81, 0, 4096, 0))
        m.drain(now=100)
        self.assertEqual(len(m.pending), 1)
        self.assertEqual(conn.messages[-1][2][10:], u.CONFIG)
        self.assertFalse(any(json.loads(line)['event'].startswith('frame') for line in log.getvalue().splitlines()))

    def test_unsupported_small_unaligned_reads(self):
        m, conn, _ = self.model()
        m.config, m.committed = 1, True
        for ident, length in enumerate((1, 2, 63, 65, 1021)):
            m.handle(101, ident, struct.pack('<BBHI', 0x81, 0, length, 0))
            m.drain(now=100)
            self.assertFalse(m.pending)
            self.assertEqual(conn.messages[-1][2], struct.pack('<BBHI', 0x81, 4, 0, 0))

    def test_exact_short_tail_read(self):
        m, conn, _ = self.model()
        m.config, m.committed = 1, True
        data = bytearray()
        for ident, length in enumerate([64] * 15 + [62]):
            m.handle(101, ident, struct.pack('<BBHI', 0x81, 0, length, 0))
            m.drain(now=100)
            reply = conn.messages[-1][2]
            self.assertEqual(reply[1], 0)
            self.assertEqual(len(reply) - 8, length)
            data.extend(reply[8:])
        self.assertEqual(data[:2], b'\x02\x80')
        self.assertEqual(data[2:], u.picture(0)[:1020])
        self.assertFalse(m.payload)


if __name__ == '__main__':
    unittest.main()
