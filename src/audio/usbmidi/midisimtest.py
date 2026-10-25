#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host-only tests of original MIDI usbredir model and compiled C codec."""
import io
from pathlib import Path
import struct
import unittest
import midiemu


class Connection:
    def __init__(self):
        self.data = bytearray()

    def sendall(self, data):
        self.data.extend(data)

    def take(self):
        result = []
        while self.data:
            kind, length, ident = struct.unpack('<III', self.data[:12])
            assert len(self.data) >= 12 + length
            result.append((kind, ident, bytes(self.data[12:12 + length])))
            del self.data[:12 + length]
        return result


class MidiSimulation(unittest.TestCase):
    def setUp(self):
        path = Path(__file__).resolve().parents[3] / 'out/audio/usbmidi/host/umcodec.dll'
        self.codec = midiemu.Codec(path)
        self.connection = Connection()
        self.model = midiemu.Model(self.connection, io.StringIO(), self.codec)
        self.model.peer_caps = midiemu.CAPS & ~(midiemu.CAP_64BIT_IDS | midiemu.CAP_32BIT_BULK)

    def configure(self):
        self.model.handle(6, 1, b'\1')
        replies = self.connection.take()
        self.assertEqual([r[0] for r in replies], [5, 4, 8])
        self.assertEqual(replies[-1], (8, 1, b'\0\1'))

    def bulk(self, endpoint, ident, data=b'', length=None):
        count = len(data) if length is None else length
        self.model.handle(101, ident, struct.pack('<BBHI', endpoint, 0, count, 0) + data)

    def test_descriptor_identity_and_boundaries(self):
        self.assertEqual(len(midiemu.CONFIG), 101)
        self.assertEqual(struct.unpack('<H', midiemu.CONFIG[2:4])[0], 101)
        self.assertEqual(struct.unpack('<HH', midiemu.DEVICE[8:12]), (0xffff, 1))
        offset, endpoint, interfaces = 0, [], []
        while offset < len(midiemu.CONFIG):
            length, kind = midiemu.CONFIG[offset:offset + 2]
            self.assertGreaterEqual(length, 2)
            self.assertLessEqual(offset + length, len(midiemu.CONFIG))
            if kind == 4:
                interfaces.append(midiemu.CONFIG[offset + 2])
            if kind == 5:
                endpoint.append(midiemu.CONFIG[offset + 2])
            offset += length
        self.assertEqual(interfaces, [0, 1])
        self.assertEqual(endpoint, [1, 129])

    def test_short_and_raw_running_status(self):
        self.assertEqual(self.codec.echo(bytes.fromhex('09903c40')), bytes.fromhex('09903c40'))
        # Raw single-byte events reconstruct running status in the C encoder.
        self.assertEqual(self.codec.echo(bytes.fromhex('0f3d00000f410000')), bytes.fromhex('09903d41'))

    def test_sysex_every_length_and_realtime(self):
        for count in range(301):
            self.codec.reset()
            stream = bytes([0xf0, 0x7d]) + bytes(i & 127 for i in range(count)) + b'\xf7'
            raw = b''.join(bytes((15, b, 0, 0)) for b in stream)
            echo = self.codec.echo(raw)
            recovered = bytearray()
            for off in range(0, len(echo), 4):
                cin = echo[off] & 15
                size = 3 if cin == 4 else cin - 4
                self.assertIn(size, (1, 2, 3))
                recovered.extend(echo[off + 1:off + 1 + size])
            self.assertEqual(bytes(recovered), stream)
        self.codec.reset()
        self.assertEqual(self.codec.echo(bytes.fromhex('0ff000000ff800000ff70000')),
                         bytes.fromhex('0ff8000006f0f700'))

    def test_reject_unaligned_reserved_and_unmapped_cable(self):
        for data in (b'123', bytes.fromhex('00903c40'), bytes.fromhex('19903c40'),
                     bytes.fromhex('0990ff40')):
            with self.assertRaises(ValueError):
                self.codec.echo(data)

    def test_pending_cancel_and_repeated_reset(self):
        self.configure()
        for cycle in range(5):
            self.bulk(129, 40, length=64)
            self.model.handle(21, 40, b'')
            reply = self.connection.take()
            self.assertEqual(reply, [(101, 40, struct.pack('<BBHI', 129, 1, 0, 0))])
            self.bulk(1, 41, bytes.fromhex('09903c40'))
            self.assertEqual(len(self.model.queue), 4)
            self.model.handle(3, 0, b'')
            self.assertEqual(len(self.model.queue), 0)
            self.assertEqual(len(self.model.pending), 0)
            self.connection.take()
            self.configure()

    def test_echo_short_packet_and_event_boundaries(self):
        self.configure()
        self.bulk(129, 100, length=64)
        data = bytes.fromhex('09903c40') * 32
        self.bulk(1, 200, data)
        replies = self.connection.take()
        self.assertEqual([r[1] for r in replies], [200, 100])
        self.assertEqual(replies[1][2][8:], data[:60])
        received = bytearray(replies[1][2][8:])
        for ident in (101, 102):
            self.bulk(129, ident, length=64)
            reply = self.connection.take()
            self.assertEqual(len(reply), 1)
            self.assertEqual(len(reply[0][2][8:]) % 4, 0)
            self.assertLess(len(reply[0][2][8:]), 64)
            received.extend(reply[0][2][8:])
        self.assertEqual(bytes(received), data)

    def test_unknown_control_stall_and_truncated_descriptor(self):
        for value, expected in ((0x0100, midiemu.DEVICE[:8]), (0x0600, b'')):
            header = struct.pack('<BBBBHHH', 128, 6, 128, 0, value, 0, 8)
            self.model.handle(100, 3, header)
            reply = self.connection.take()[0]
            self.assertEqual(reply[2][10:], expected)
            self.assertEqual(reply[2][3], 0 if expected else 4)

    def test_pending_bound_and_malformed_request(self):
        self.configure()
        for ident in range(256):
            self.bulk(129, ident, length=64)
        with self.assertRaises(ValueError):
            self.bulk(129, 256, length=64)
        with self.assertRaises(ValueError):
            self.bulk(129, 0, length=64)
        with self.assertRaises(ValueError):
            self.bulk(129, 300, length=3)

    def test_profiles_parse_shapes(self):
        for name, profile in midiemu.PROFILES.items():
            for speed in ('full', 'high'):
                model = midiemu.Model(Connection(), io.StringIO(), self.codec, profile, speed)
                config = model.config
                self.assertEqual(struct.unpack('<H', config[2:4])[0], len(config))
                off, sizes = 0, []
                while off < len(config):
                    if config[off + 1] == 5:
                        sizes.append(struct.unpack('<H', config[off + 4:off + 6])[0])
                    off += config[off]
                bulk = profile.kind == 'bulk'
                self.assertEqual(sizes, [512 if speed == 'high' and bulk else 64] * 2 * len(profile.pairs))
                self.assertEqual(model.descriptor(6, 0) is not None, speed == 'high')
        lone = midiemu.PROFILES['lone'].config
        self.assertEqual((lone[4], lone[9 + 5], lone[9 + 6]), (1, 1, 3))

    def test_multi_cable_echo_keeps_cables(self):
        path = Path(__file__).resolve().parents[3] / 'out/audio/usbmidi/host/umcodec.dll'
        codec = midiemu.Codec(path, 2)
        self.assertEqual(codec.echo(bytes.fromhex('09903c40' '19913d41')), bytes.fromhex('09903c40' '19913d41'))
        # Running status is kept per cable.
        self.assertEqual(codec.echo(bytes.fromhex('0f900000' '1f920000' '0f3c0000' '1f3e0000' '0f400000' '1f410000')),
                         bytes.fromhex('09903c40' '19923e41'))
        with self.assertRaises(ValueError):
            codec.echo(bytes.fromhex('29903c40'))


import ctypes  # noqa: E402
DESC_DLL = Path(__file__).resolve().parents[3] / 'out/audio/usbmidi/host/umdesc.dll'


class UmEndpoint(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ubyte) for n in ('Address', 'Type', 'Interval', 'Cables')] + \
        [('Jack', ctypes.c_ubyte * 16), ('Name', ctypes.c_ubyte * 16), ('MaxPacket', ctypes.c_ushort)]


class UmInterface(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ubyte) for n in ('Number', 'Alternate', 'Count', 'Declared')] + \
        [('Endpoint', UmEndpoint * 30)]


class UmPort(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ubyte) for n in ('Pipe', 'Cable', 'Name')]


class UmPortMap(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint) for n in ('Outputs', 'Inputs', 'OutPipes', 'InPipes')] + \
        [('OutPipe', ctypes.c_ubyte * 4), ('InPipe', ctypes.c_ubyte * 4), ('Out', UmPort * 32), ('In', UmPort * 32)]


@unittest.skipUnless(DESC_DLL.exists(), 'run host.ps1 first')
class DriverParserTests(unittest.TestCase):
    """Every profile through the driver's own msdesc.c."""

    @classmethod
    def setUpClass(cls):
        cls.dll = ctypes.CDLL(str(DESC_DLL))
        cls.codec = midiemu.Codec(Path(__file__).resolve().parents[3] / 'out/audio/usbmidi/host/umcodec.dll')

    def parse(self, config):
        iface, ports = UmInterface(), UmPortMap()
        blob = ctypes.create_string_buffer(config, len(config))
        self.assertEqual(self.dll.UmFindInterface(blob, len(config), ctypes.byref(iface)), 1)
        filters = self.dll.UmMapPorts(ctypes.byref(iface), ctypes.byref(ports))
        return iface, ports, filters

    def test_every_profile_maps_its_ports(self):
        for name, profile in midiemu.PROFILES.items():
            for speed in ('full', 'high'):
                model = midiemu.Model(Connection(), io.StringIO(), self.codec, profile, speed)
                iface, ports, filters = self.parse(model.config)
                want = sum(c for _, _, c in profile.pairs)
                self.assertEqual((ports.Outputs, ports.Inputs, filters), (want, want, want), (name, speed))
                self.assertEqual((ports.OutPipes, ports.InPipes), (len(profile.pairs), len(profile.pairs)))
                eps = {iface.Endpoint[i].Address: iface.Endpoint[i] for i in range(iface.Count)}
                for out_ep, in_ep, cables in profile.pairs:
                    for ep in (out_ep, in_ep):
                        self.assertEqual(eps[ep].Cables, cables)
                        self.assertEqual(eps[ep].Type, 2 if profile.kind == 'bulk' else 3)
        quad = self.parse(midiemu.PROFILES['quad'].config)[1]
        self.assertEqual([quad.Out[i].Name for i in range(4)], [4, 5, 6, 7])
        umtab = self.parse(midiemu.UMTAB)[0]
        self.assertEqual([umtab.Endpoint[i].Jack[0] for i in range(umtab.Count)], [0x40, 0x20])

    def test_umtab_identity(self):
        model = midiemu.Model(Connection(), io.StringIO(), self.codec, midiemu.PROFILES['umtab'])
        self.assertEqual(struct.unpack('<HHH', model.device[8:14]), (0x0582, 0x0168, 0x0110))
        self.assertEqual(model.device[14:17], b'\1\2\0')
        self.assertEqual(model.config, midiemu.UMTAB)
        high = midiemu.Model(Connection(), io.StringIO(), self.codec, midiemu.PROFILES['umtab'], 'high')
        self.assertEqual(struct.unpack('<H', high.device[2:4])[0], 0x200)

    def test_wide_pipes_echo_on_their_own_endpoints(self):
        conn = Connection()
        model = midiemu.Model(conn, io.StringIO(), self.codec, midiemu.PROFILES['wide'])
        model.peer_caps = midiemu.CAPS & ~(midiemu.CAP_64BIT_IDS | midiemu.CAP_32BIT_BULK)
        model.handle(6, 1, b'\1')
        conn.take()
        for ep, data in ((2, bytes.fromhex('29903c40')), (1, bytes.fromhex('19913d41'))):
            model.handle(101, 10 + ep, struct.pack('<BBHI', ep, 0, len(data), 0) + data)
        model.handle(101, 20, struct.pack('<BBHI', 0x81, 0, 64, 0))
        model.handle(101, 21, struct.pack('<BBHI', 0x82, 0, 64, 0))
        replies = {r[1]: r[2][8:] for r in conn.take() if r[0] == 101}
        self.assertEqual(replies[20], bytes.fromhex('19913d41'))
        self.assertEqual(replies[21], bytes.fromhex('29903c40'))
        with self.assertRaises(ValueError):          # cable 3 does not exist on a 3-cable pipe
            model.handle(101, 30, struct.pack('<BBHI', 1, 0, 4, 0) + bytes.fromhex('39903c40'))

    def test_interrupt_profile(self):
        conn = Connection()
        model = midiemu.Model(conn, io.StringIO(), self.codec, midiemu.PROFILES['intr'], 'high')
        model.peer_caps = midiemu.CAPS & ~(midiemu.CAP_64BIT_IDS | midiemu.CAP_32BIT_BULK)
        model.handle(6, 1, b'\1')
        conn.take()
        note = bytes.fromhex('09903c40')
        model.handle(103, 5, struct.pack('<BBH', 1, 0, 4) + note)
        self.assertEqual(conn.take(), [(103, 5, struct.pack('<BBH', 1, 0, 4))])   # nothing until receiving
        model.handle(15, 6, b'\x81')
        replies = conn.take()
        self.assertEqual(replies[0], (17, 6, b'\0\x81'))
        self.assertEqual(replies[1], (103, 0, struct.pack('<BBH', 0x81, 0, 4) + note))
        model.handle(16, 7, b'\x81')
        model.handle(103, 8, struct.pack('<BBH', 1, 0, 4) + note)
        self.assertEqual([r[0] for r in conn.take()], [17, 103])
        self.assertEqual(len(model.queue), 4)


class Raw:
    def __init__(self):
        self.data = bytearray()

    def sendall(self, data):
        self.data.extend(data)


class XhciFraming(unittest.TestCase):
    """qemu-xhci ports need 64-bit ids and 32-bit bulk lengths."""

    def test_64bit_ids_and_32bit_bulk_lengths(self):
        codec = midiemu.Codec(Path(__file__).resolve().parents[3] / 'out/audio/usbmidi/host/umcodec.dll')
        raw = Raw()
        m = midiemu.Model(raw, io.StringIO(), codec)
        m.peer_caps = midiemu.CAPS
        m.handle(6, 1 << 40, b'')
        raw.data.clear()
        note = bytes.fromhex('09903c40')
        m.handle(101, 1 << 41, struct.pack('<BBHIH', 1, 0, 4, 0, 0) + note)
        kind, length, ident = struct.unpack('<IIQ', raw.data[:16])
        self.assertEqual((kind, ident, raw.data[16:26]), (101, 1 << 41, struct.pack('<BBHIH', 1, 0, 4, 0, 0)))
        raw.data.clear()
        m.handle(101, 1 << 42, struct.pack('<BBHIH', 0x81, 0, 64, 0, 0))
        kind, length, ident = struct.unpack('<IIQ', raw.data[:16])
        self.assertEqual((ident, raw.data[16:26], raw.data[26:]), (1 << 42, struct.pack('<BBHIH', 0x81, 0, 4, 0, 0), note))


if __name__ == '__main__':
    unittest.main()
