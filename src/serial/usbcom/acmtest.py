#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host protocol tests; actual USB/Windows results are separate."""
import io
import struct
import unittest
import acmemu

# The framing of the earlier legs: 32-bit ids and 16-bit bulk lengths.
LEGACY = acmemu.CAPS & ~(acmemu.CAP_64BIT_IDS | acmemu.CAP_32BIT_BULK)


class Connection:
    def __init__(self):
        self.data = bytearray()

    def sendall(self, data):
        self.data.extend(data)

    def messages(self):
        result = []
        while self.data:
            kind, length, ident = struct.unpack('<III', self.data[:12])
            assert len(self.data) >= 12 + length
            result.append((kind, ident, bytes(self.data[12:12 + length])))
            del self.data[:12 + length]
        return result


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.connection = Connection()
        self.model = acmemu.Model(self.connection, io.StringIO())
        self.model.peer_caps = LEGACY

    def configure(self):
        self.model.handle(6, 27, b'\x01')
        messages = self.connection.messages()
        self.assertEqual([(m[0], m[1]) for m in messages], [(5, 0), (4, 0), (8, 27)])
        self.assertEqual(messages[-1][2], b'\x00\x01')

    def control(self, request, reqtype, value=0, length=0, data=b''):
        header = struct.pack('<BBBBHHH', 0x80 if reqtype & 0x80 else 0,
                             request, reqtype, 0, value, 0, length)
        self.model.handle(100, 41, header + data)
        messages = self.connection.messages()
        kind, ident, reply = messages[-1]
        self.assertEqual((kind, ident), (100, 41))
        return struct.unpack('<BBBBHHH', reply[:10]), reply[10:]

    def test_descriptor_truncation_and_unknown_stall(self):
        header, data = self.control(6, 0x80, 0x0100, 8)
        self.assertEqual(header[3], 0)
        self.assertEqual(header[-1], 8)
        self.assertEqual(data, acmemu.DEVICE[:8])
        header, data = self.control(6, 0x80, 0x0600, 10)
        self.assertEqual((header[3], data), (4, b''))

    def test_configuration_grammar(self):
        self.assertEqual(len(acmemu.DEVICE), 18)
        config = acmemu.CONFIG
        self.assertEqual(len(config), struct.unpack('<H', config[2:4])[0])
        offset, endpoints, interfaces = 0, [], []
        while offset < len(config):
            length, kind = config[offset:offset + 2]
            self.assertGreaterEqual(length, 2)
            self.assertLessEqual(offset + length, len(config))
            if kind == 4:
                interfaces.append(config[offset + 2])
            if kind == 5:
                endpoints.append(config[offset + 2])
            offset += length
        self.assertEqual(interfaces, [0, 1])
        self.assertEqual(set(endpoints), {2, 0x81, 0x83})

    def test_config_info_precedes_status(self):
        self.configure()

    def test_line_coding_roundtrip_and_invalid_write(self):
        self.configure()
        coding = struct.pack('<IBBB', 9600, 0, 0, 8)
        header, data = self.control(0x20, 0x21, length=7, data=coding)
        self.assertEqual((header[3], header[-1], data), (0, 7, b''))
        header, data = self.control(0x21, 0xa1, length=7)
        self.assertEqual(data, coding)
        header, _ = self.control(0x20, 0x21, length=7, data=struct.pack('<IBBB', 9600, 0, 0, 1))
        self.assertEqual(header[3], 4)
        _, data = self.control(0x21, 0xa1, length=7)
        self.assertEqual(data, coding)

    def test_pending_echo_segmentation_and_ids(self):
        self.configure()
        for ident in (101, 102):
            self.model.handle(101, ident, struct.pack('<BBHI', 0x81, 0, 3, 0))
        self.assertFalse(self.connection.messages())
        self.model.handle(101, 100, struct.pack('<BBHI', 2, 0, 6, 0) + b'ABCDEF')
        replies = self.connection.messages()
        self.assertEqual([r[1] for r in replies], [100, 101, 102])
        self.assertEqual([r[2][8:] for r in replies], [b'', b'ABC', b'DEF'])

    def test_cancel_then_data_does_not_complete_id_twice(self):
        self.configure()
        self.model.handle(101, 123, struct.pack('<BBHI', 0x81, 0, 64, 0))
        self.model.handle(21, 123, b'')
        replies = self.connection.messages()
        self.assertEqual(len(replies), 1)
        self.assertEqual((replies[0][1], replies[0][2][1]), (123, 1))
        self.model.handle(21, 123, b'')
        self.model.handle(101, 124, struct.pack('<BBHI', 2, 0, 1, 0) + b'X')
        self.assertEqual([r[1] for r in self.connection.messages()], [124])

    def test_full_packet_echo_flushes_separate_zlp_before_next_frame(self):
        self.configure()
        self.model.handle(101, 1, struct.pack('<BBHI', 2, 0, 64, 0) + b'A' * 64)
        self.connection.messages()
        for ident in (11, 12):
            self.model.handle(101, ident, struct.pack('<BBHI', 0x81, 0, 512, 0))
        replies = self.connection.messages()
        self.assertEqual([r[1] for r in replies], [11, 12])
        self.assertEqual([r[2][8:] for r in replies], [b'A' * 64, b''])
        self.model.handle(101, 2, struct.pack('<BBHI', 2, 0, 1, 0) + b'B')
        self.connection.messages()
        self.model.handle(101, 13, struct.pack('<BBHI', 0x81, 0, 512, 0))
        self.assertEqual(self.connection.messages()[0][2][8:], b'B')
        self.assertEqual(self.model.queued,0)

    def test_no_flush_zlp_sends_full_packet_echo_without_zlp(self):
        self.make(flush_zlp=False)
        self.model.handle(101, 1, struct.pack('<BBHI', 2, 0, 64, 0) + b'A' * 64)
        self.connection.messages()
        for ident in (11, 12):
            self.model.handle(101, ident, struct.pack('<BBHI', 0x81, 0, 512, 0))
        replies = self.connection.messages()
        self.assertEqual([r[1] for r in replies], [11])
        self.assertEqual(replies[0][2][8:], b'A' * 64)
        self.model.handle(101, 2, struct.pack('<BBHI', 2, 0, 1, 0) + b'B')
        replies = self.connection.messages()
        self.assertEqual([r[1] for r in replies], [2, 12])
        self.assertEqual(replies[1][2][8:], b'B')

    def test_packet_sized_requests_preserve_zlp_before_next_payload(self):
        self.configure()
        for ident, data in ((1, b'A' * 64), (2, b'B' * 65)):
            self.model.handle(101, ident, struct.pack('<BBHI', 2, 0, len(data), 0) + data)
            self.connection.messages()
        for ident in (11, 12, 13, 14, 15):
            self.model.handle(101, ident, struct.pack('<BBHI', 0x81, 0, 64, 0))
        replies = self.connection.messages()
        self.assertEqual([r[1] for r in replies], [11, 12, 13, 14])
        self.assertEqual([r[2][8:] for r in replies], [b'A' * 64, b'', b'B' * 64, b'B'])
        self.assertEqual(list(self.model.pending), [15])
        self.assertEqual((self.model.queued, len(self.model.echo)), (0, 0))

    def test_partial_final_packet_needs_no_extra_flush(self):
        self.configure()
        self.model.handle(101, 1, struct.pack('<BBHI', 2, 0, 65, 0) + b'A' * 65)
        self.connection.messages()
        for ident in (11, 12, 13):
            self.model.handle(101, ident, struct.pack('<BBHI', 0x81, 0, 64, 0))
        replies=self.connection.messages()
        self.assertEqual([r[2][8:] for r in replies],[b'A' * 64,b'A'])
        self.assertEqual(list(self.model.pending),[13])
        self.assertEqual(self.model.queued,0)

    def test_reset_cancels_pending_and_discards_echo(self):
        self.configure()
        self.model.handle(101, 123, struct.pack('<BBHI', 0x81, 0, 64, 0))
        self.model.handle(3, 0, b'')
        self.assertEqual(self.connection.messages()[0][2][1], 1)
        self.assertEqual((self.model.config, len(self.model.pending), len(self.model.echo)), (0, 0, 0))

    def test_interrupt_serial_state(self):
        self.configure()
        self.model.handle(15, 12, b'\x83')
        replies = self.connection.messages()
        self.assertEqual(replies[0], (17, 12, b'\x00\x83'))
        self.assertEqual(replies[1][2][4:], b'\xa1\x20\x00\x00\x00\x00\x02\x00\x03\x00')

    def make(self, **options):
        self.connection = Connection()
        self.model = acmemu.Model(self.connection, io.StringIO(), **options)
        self.model.peer_caps = LEGACY
        self.configure()

    def walk(self, config):
        self.assertEqual(len(config), struct.unpack('<H', config[2:4])[0])
        offset, items = 0, []
        while offset < len(config):
            length, kind = config[offset:offset + 2]
            self.assertGreaterEqual(length, 2)
            self.assertLessEqual(offset + length, len(config))
            items.append(bytes(config[offset:offset + length]))
            offset += length
        return items

    def test_high_speed_descriptors_and_packet_flush(self):
        self.make(speed='high')
        header, data = self.control(6, 0x80, 0x0200, 255)
        bulk = [d for d in self.walk(data) if d[1] == 5 and d[3] == 2]
        self.assertEqual([struct.unpack('<H', d[4:6])[0] for d in bulk], [512, 512])
        notify = [d for d in self.walk(data) if d[1] == 5 and d[3] == 3]
        self.assertEqual(notify[0][6], 8)
        header, data = self.control(6, 0x80, 0x0600, 10)
        self.assertEqual((header[3], data[:2]), (0, b'\x0a\x06'))
        header, data = self.control(6, 0x80, 0x0700, 255)
        self.assertEqual(data[1], 7)
        other = [d for d in self.walk(data) if d[1] == 5 and d[3] == 2]
        self.assertEqual(struct.unpack('<H', other[0][4:6])[0], 64)
        # A 512-byte echo is flushed by a ZLP; a 64-byte one is not.
        self.model.handle(101, 1, struct.pack('<BBHI', 2, 0, 512, 0) + b'A' * 512)
        self.model.handle(101, 2, struct.pack('<BBHI', 2, 0, 64, 0) + b'B' * 64)
        self.connection.messages()
        for ident in (11, 12, 13, 14):
            self.model.handle(101, ident, struct.pack('<BBHI', 0x81, 0, 512, 0))
        replies = self.connection.messages()
        self.assertEqual([r[2][8:] for r in replies], [b'A' * 512, b'', b'B' * 64])

    def test_full_speed_has_no_qualifier(self):
        header, data = self.control(6, 0x80, 0x0600, 10)
        self.assertEqual((header[3], data), (4, b''))

    def test_pico_profile_iad_and_three_interfaces(self):
        self.make(profile='pico')
        device = self.model.device
        self.assertEqual(device[4:7], b'\xef\x02\x01')
        self.assertEqual(struct.unpack('<HH', device[8:12]), (0x2E8A, 0x000A))
        header, data = self.control(6, 0x80, 0x0200, 255)
        items = self.walk(data)
        self.assertEqual(data[4], 3)
        self.assertEqual(items[1][:4], b'\x08\x0b\x00\x02')
        self.assertEqual([d[2] for d in items if d[1] == 4], [0, 1, 2])
        self.assertEqual([d[5] for d in items if d[1] == 4], [2, 10, 0xFF])
        self.model.handle(9, 5, b'\x02\x00')
        self.assertEqual(self.connection.messages()[-1], (11, 5, b'\x00\x02\x00'))
        self.model.handle(9, 6, b'\x03\x00')
        self.assertEqual(self.connection.messages()[-1], (11, 6, b'\x04\x03\x00'))

    def test_dual_profile_two_functions(self):
        self.make(profile='dual', mirror=True)
        header, data = self.control(6, 0x80, 0x0200, 512)
        items = self.walk(data)
        self.assertEqual(data[4], 5)
        self.assertEqual([d[2:4] for d in items if d[1] == 11], [b'\x00\x02', b'\x02\x02'])
        self.assertEqual([d[2] for d in items if d[1] == 4], [0, 1, 2, 3, 4])
        self.assertEqual([d[5] for d in items if d[1] == 4], [2, 10, 2, 10, 0xFF])
        self.assertEqual([d[2] for d in items if d[1] == 5], [0x83, 0x02, 0x81, 0x86, 0x05, 0x84])
        unions = [d for d in items if d[1] == 0x24 and d[2] == 6]
        self.assertEqual([d[3:5] for d in unions], [b'\x00\x01', b'\x02\x03'])
        # Line coding per communication interface.
        coding = struct.pack('<IBBB', 9600, 0, 0, 8)
        for index in (2, 0):
            self.model.handle(100, 41, struct.pack('<BBBBHHH', 0, 0x20, 0x21, 0, 0, index, 7) + coding)
            self.assertEqual(self.connection.messages()[-1][2][3], 0)
            coding = struct.pack('<IBBB', 115200, 0, 0, 8)
        self.assertEqual(self.model.fns[1].line[:4], struct.pack('<I', 9600))
        self.assertEqual(self.model.fns[0].line[:4], struct.pack('<I', 115200))
        self.model.handle(100, 41, struct.pack('<BBBBHHH', 0, 0x20, 0x21, 0, 0, 1, 7) + coding)
        self.assertEqual(self.connection.messages()[-1][2][3], 4)        # a data interface: stall
        # SERIAL_STATE on each function's own endpoint with its own wIndex.
        self.model.handle(15, 12, b'\x86')
        self.connection.messages()
        self.model.handle(100, 41, struct.pack('<BBBBHHH', 0, 0x22, 0x21, 0, 1, 2, 0))
        notices = [m for m in self.connection.messages() if m[0] == 103]
        self.assertEqual(len(notices), 1)
        self.assertEqual(notices[0][2][0], 0x86)
        self.assertEqual(struct.unpack('<HH', notices[0][2][4 + 4:4 + 8]), (2, 2))
        self.assertEqual(struct.unpack('<H', notices[0][2][-2:])[0], 2)
        self.model.handle(100, 41, struct.pack('<BBBBHHH', 0, 0x22, 0x21, 0, 3, 0, 0))
        self.assertEqual([m for m in self.connection.messages() if m[0] == 103], [])   # 0x83 not receiving
        # Independent echo: each IN endpoint returns only its own OUT data.
        for endpoint, data in ((0x05, b'second port'), (0x02, b'first port')):
            self.model.handle(101, 60 + endpoint, struct.pack('<BBHI', endpoint, 0, len(data), 0) + data)
        self.connection.messages()
        self.model.handle(101, 70, struct.pack('<BBHI', 0x81, 0, 64, 0))
        self.model.handle(101, 71, struct.pack('<BBHI', 0x84, 0, 64, 0))
        replies = {m[1]: m[2][8:] for m in self.connection.messages() if m[0] == 101}
        self.assertEqual(replies, {70: b'first port', 71: b'second port'})
        # A pending IN on one port never waits behind the other's.
        self.model.handle(101, 72, struct.pack('<BBHI', 0x81, 0, 64, 0))
        self.model.handle(101, 73, struct.pack('<BBHI', 0x84, 0, 64, 0))
        self.model.handle(101, 74, struct.pack('<BBHI', 0x05, 0, 2, 0) + b'hi')
        replies = [m for m in self.connection.messages() if m[0] == 101]
        self.assertEqual([(m[1], m[2][8:]) for m in replies], [(74, b''), (73, b'hi')])
        self.assertIn(72, self.model.pending)
        self.model.handle(9, 5, b'\x04\x00')
        self.assertEqual(self.connection.messages()[-1], (11, 5, b'\x00\x04\x00'))

    def test_dual_profile_flush_zlp_per_function(self):
        # The ZLP flush (and --no-flush-zlp) apply to each function's own IN endpoint.
        for flush, expected in ((True, [b'A' * 64, b'']), (False, [b'A' * 64])):
            self.make(profile='dual', flush_zlp=flush)
            self.model.handle(101, 1, struct.pack('<BBHI', 0x05, 0, 64, 0) + b'A' * 64)
            self.connection.messages()
            for ident in (11, 12):
                self.model.handle(101, ident, struct.pack('<BBHI', 0x84, 0, 512, 0))
            replies = [m for m in self.connection.messages() if m[0] == 101]
            self.assertEqual([r[2][8:] for r in replies], expected)
            self.assertEqual([r[2][0] for r in replies], [0x84] * len(expected))
            self.assertEqual(self.model.fns[1].queued, 0)
            self.assertFalse(self.model.fns[0].echo)

    def test_dual_profile_without_bulk32(self):
        # --no-bulk32: 64-bit ids stay, bulk headers are 8 bytes, on function 1 too.
        self.connection = Connection()
        self.model = acmemu.Model(self.connection, io.StringIO(), profile='dual',
                                  caps=acmemu.CAPS & ~acmemu.CAP_32BIT_BULK)
        self.model.peer_caps = acmemu.CAPS
        self.assertTrue(self.model.ids64)
        self.assertFalse(self.model.len32)
        self.model.handle(6, 27, b'\x01')
        self.connection.data.clear()
        self.model.handle(101, 9, struct.pack('<BBHI', 0x84, 0, 64, 0))
        self.model.handle(101, 7, struct.pack('<BBHI', 0x05, 0, 3, 0) + b'xyz')
        data = bytes(self.connection.data)
        replies = []
        while data:
            kind, length, ident = struct.unpack('<IIQ', data[:16])
            replies.append((kind, ident, data[16:16 + length]))
            data = data[16 + length:]
        self.assertEqual([(r[0], r[1]) for r in replies], [(101, 7), (101, 9)])
        self.assertEqual(replies[1][2], struct.pack('<BBHI', 0x84, 0, 3, 0) + b'xyz')

    def state_after(self, request, value):
        self.model.handle(100, 41, struct.pack('<BBBBHHH', 0, request, 0x21, 0, value, 0, 0))
        notices = [m for m in self.connection.messages() if m[0] == 103]
        return [struct.unpack('<H', m[2][-2:])[0] for m in notices]

    def test_mirror_lines_and_break(self):
        self.make(mirror=True)
        self.model.handle(15, 12, b'\x83')
        self.assertEqual(self.connection.messages()[1][2][-2:], b'\x00\x00')
        for value, want in ((1, 2), (3, 3), (2, 1), (0, 0)):
            self.assertEqual(self.state_after(0x22, value), [want])
        self.assertEqual(self.state_after(0x23, 0xFFFF), [4])
        self.assertEqual(self.state_after(0x23, 0), [])
        self.assertEqual(self.state_after(0x22, 1), [2])

    def test_notifications_wait_for_receiving_and_are_not_repeated(self):
        self.make(mirror=True)
        self.model.handle(15, 12, b'\x83')
        self.assertEqual(len([m for m in self.connection.messages() if m[0] == 103]), 1)
        # Stopped (QEMU's EHCI retires an idle queue): changes are held.
        self.model.handle(16, 13, b'\x83')
        self.assertEqual(self.state_after(0x22, 1), [])
        self.assertEqual(self.state_after(0x23, 0xFFFF), [])
        # Restarted: each held state once, in order; nothing new is made up.
        self.model.handle(15, 14, b'\x83')
        notices = [m for m in self.connection.messages() if m[0] == 103]
        self.assertEqual([struct.unpack('<H', m[2][-2:])[0] for m in notices], [2, 6])
        # The next restart repeats the last state once, without the
        # one-shot break (QEMU may have dropped it); a restart after a quiet
        # period repeats nothing.
        self.model.handle(16, 15, b'\x83')
        self.model.handle(15, 16, b'\x83')
        notices = [m for m in self.connection.messages() if m[0] == 103]
        self.assertEqual([struct.unpack('<H', m[2][-2:])[0] for m in notices], [2])
        self.model.handle(16, 17, b'\x83')
        self.model.handle(15, 18, b'\x83')
        self.assertFalse([m for m in self.connection.messages() if m[0] == 103])

    def test_default_state_unchanged_without_mirror(self):
        self.model.handle(6, 27, b'\x01')
        self.connection.messages()
        self.model.handle(15, 12, b'\x83')
        self.connection.messages()
        self.assertEqual(self.state_after(0x22, 0), [3])

    def test_xhci_framing_64bit_ids_and_32bit_bulk_lengths(self):
        self.model.peer_caps = acmemu.CAPS
        self.model.handle(6, 27, b'\x01')
        data = bytes(self.connection.data)
        kind, length, ident = struct.unpack('<IIQ', data[:16])
        self.assertEqual(kind, 5)
        self.connection.data.clear()
        self.model.handle(101, 0x1122334455, struct.pack('<BBHIH', 0x81, 0, 0x1000, 0, 0))
        self.model.handle(101, 7, struct.pack('<BBHIH', 2, 0, 3, 0, 0) + b'xyz')
        data = bytes(self.connection.data)
        replies = []
        while data:
            kind, length, ident = struct.unpack('<IIQ', data[:16])
            replies.append((kind, ident, data[16:16 + length]))
            data = data[16 + length:]
        self.assertEqual([(r[0], r[1]) for r in replies], [(101, 7), (101, 0x1122334455)])
        self.assertEqual(replies[1][2], struct.pack('<BBHIH', 0x81, 0, 3, 0, 0) + b'xyz')
        with self.assertRaises(ValueError):
            self.model.handle(101, 8, struct.pack('<BBHI', 0x81, 0, 64, 0))

    def test_zero_length_in_completes_at_once_without_data(self):
        self.configure()
        self.model.handle(101, 9, struct.pack('<BBHI', 0x81, 0, 0, 0))
        replies = self.connection.messages()
        self.assertEqual([(r[0], r[1]) for r in replies], [(101, 9)])
        self.assertEqual(replies[0][2], struct.pack('<BBHI', 0x81, 0, 0, 0))
        self.assertFalse(self.model.pending)
        # Data sent afterwards waits for a real request.
        self.model.handle(101, 10, struct.pack('<BBHI', 2, 0, 2, 0) + b'AB')
        self.model.handle(101, 11, struct.pack('<BBHI', 0x81, 0, 64, 0))
        replies = self.connection.messages()
        self.assertEqual([r[1] for r in replies], [10, 11])
        self.assertEqual(replies[1][2][8:], b'AB')

    def test_zero_length_in_behind_a_request_completes_in_order(self):
        self.configure()
        self.model.handle(101, 20, struct.pack('<BBHI', 0x81, 0, 64, 0))
        self.model.handle(101, 21, struct.pack('<BBHI', 0x81, 0, 0, 0))
        self.model.handle(101, 22, struct.pack('<BBHI', 0x81, 0, 64, 0))
        self.assertFalse(self.connection.messages())
        self.model.handle(101, 23, struct.pack('<BBHI', 2, 0, 4, 0) + b'WXYZ')
        replies = self.connection.messages()
        self.assertEqual([r[1] for r in replies], [23, 20, 21])
        self.assertEqual(replies[1][2][8:], b'WXYZ')
        self.assertEqual(replies[2][2], struct.pack('<BBHI', 0x81, 0, 0, 0))
        self.assertEqual(list(self.model.pending), [22])

    def test_zero_length_in_completes_when_the_older_request_is_cancelled(self):
        self.configure()
        self.model.handle(101, 30, struct.pack('<BBHI', 0x81, 0, 64, 0))
        self.model.handle(101, 31, struct.pack('<BBHI', 0x81, 0, 0, 0))
        self.assertFalse(self.connection.messages())
        self.model.handle(21, 30, b'')
        replies = self.connection.messages()
        self.assertEqual([(r[1], r[2][1]) for r in replies], [(30, 1), (31, 0)])
        self.assertFalse(self.model.pending)

    def test_malformed_out_does_not_queue(self):
        self.configure()
        with self.assertRaises(ValueError):
            self.model.handle(101, 1, struct.pack('<BBHI', 2, 0, 6, 0) + b'X')
        self.assertFalse(self.model.echo)


# The driver's own CDC-ACM core (acmcore.c), built into an x64 DLL by
# simhost.ps1, reading the simulator's descriptors and notifications.
import ctypes  # noqa: E402
from pathlib import Path  # noqa: E402
CORE = Path(__file__).resolve().parents[3] / 'out/serial/usbcom/simhost/acmcore.dll'


class AcFunc(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ubyte) for n in ('CommIf', 'CommAlt', 'DataIf', 'DataAlt', 'NotifyEp',
                                              'NotifyInterval')] + \
        [('NotifyMax', ctypes.c_ushort), ('BulkIn', ctypes.c_ubyte), ('BulkOut', ctypes.c_ubyte),
         ('InMax', ctypes.c_ushort), ('OutMax', ctypes.c_ushort), ('Caps', ctypes.c_ubyte),
         ('Flags', ctypes.c_ubyte)]


@unittest.skipUnless(CORE.exists(), 'run simhost.ps1 first')
class DriverCoreTests(unittest.TestCase):
    def setUp(self):
        self.core = ctypes.CDLL(str(CORE))

    def functions(self, config):
        out = (AcFunc * 4)()
        blob = ctypes.create_string_buffer(config, len(config))
        n = self.core.AcFindFunctions(blob, len(config), 0, out, 4)
        return [out[i] for i in range(max(n, 0))], n

    def test_driver_finds_every_profile_function(self):
        for profile, want in (('uno', 1), ('pico', 1), ('dual', 2)):
            for speed in ('full', 'high'):
                m = acmemu.Model(Connection(), io.StringIO(), speed, profile)
                fns, n = self.functions(m.config_desc)
                self.assertEqual(n, want, (profile, speed))
                got = [(f.CommIf, f.DataIf, f.NotifyEp, f.BulkIn, f.BulkOut, f.InMax) for f in fns]
                bulk = 512 if speed == 'high' else 64
                self.assertEqual(got, [(c, d, i, b_in, b_out, bulk) for c, d, i, b_out, b_in in
                                       acmemu.FUNCTIONS[:want]])

    def test_driver_reads_each_functions_serial_state(self):
        connection = Connection()
        m = acmemu.Model(connection, io.StringIO(), profile='dual', mirror=True)
        m.peer_caps = LEGACY
        m.handle(6, 1, b'\x01')
        m.handle(15, 2, b'\x86')
        connection.messages()
        m.handle(100, 3, struct.pack('<BBBBHHH', 0, 0x22, 0x21, 0, 3, 2, 0))
        note = [x for x in connection.messages() if x[0] == 103][0][2][4:]
        state = ctypes.c_ushort()
        blob = ctypes.create_string_buffer(note, len(note))
        self.assertEqual(self.core.AcParseNotify(blob, len(note), 2, ctypes.byref(state)), 1)
        self.assertEqual(state.value, 3)
        self.assertEqual(self.core.AcParseNotify(blob, len(note), 0, ctypes.byref(state)), 0)


if __name__ == '__main__':
    unittest.main()
