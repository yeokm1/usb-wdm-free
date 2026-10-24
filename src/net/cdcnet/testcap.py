#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""Check finite ECM frame boundaries and optional guest traffic in a PCAP.

The default checks QEMU netdev frame boundaries, not physical USB ZLPs.
--m1 verifies baseline DHCP/ICMP/TCP traffic; --load verifies load payloads.
Guest logs establish reception, since a netdev capture also sees undelivered
TCP segments and retransmissions during intentional removal.
"""
import argparse
import struct
from pathlib import Path

def add_segment(connection, seq, payload):
    """Retransmissions may reuse a sequence with a different segment length."""
    previous = connection.get(seq, b'')
    overlap = min(len(previous), len(payload))
    if previous[:overlap] != payload[:overlap]:
        raise SystemExit('FAIL: inconsistent TCP retransmission bytes')
    if len(payload) > len(previous):
        connection[seq] = payload

def reassemble(connection):
    stream = bytearray()
    first = min(connection)
    for seq, data in sorted(connection.items()):
        offset = seq - first
        if offset > len(stream):
            raise SystemExit('FAIL: missing TCP data')
        overlap = min(len(data), len(stream) - offset)
        if stream[offset:offset + overlap] != data[:overlap]:
            raise SystemExit('FAIL: inconsistent overlapping TCP data')
        stream.extend(data[overlap:])
    return stream

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('capture', type=Path)
ap.add_argument('--m1', action='store_true', help='also require DHCP, ICMP and deterministic TCP data')
ap.add_argument('--load', action='store_true', help='also validate bounded UDP/TCP load traffic')
args = ap.parse_args()
data = args.capture.read_bytes()
if len(data) < 24:
    raise SystemExit('FAIL: no PCAP header')
orders = {b'\xd4\xc3\xb2\xa1': '<', b'\xa1\xb2\xc3\xd4': '>'}
order = orders.get(data[:4])
if not order:
    raise SystemExit('FAIL: unsupported PCAP format')
if struct.unpack_from(order + 'I', data, 20)[0] != 1:
    raise SystemExit('FAIL: capture is not Ethernet')
frames, off = [], 24
while off < len(data):
    if len(data) - off < 16:
        raise SystemExit('FAIL: truncated PCAP packet header')
    _, _, size, actual = struct.unpack_from(order + 'IIII', data, off)
    off += 16
    if size != actual or size > len(data) - off:
        raise SystemExit('FAIL: truncated packet')
    frames.append(data[off:off + size])
    off += size
probes = [f for f in frames if len(f) >= 42 and f[:6] == b'\xff' * 6
          and f[12:14] == b'\x08\x06' and f[20:22] == b'\x00\x01'
          and f[28:32] == bytes((10, 0, 2, 15))]
if len(probes) < 2:
    raise SystemExit(f'FAIL: expected two ARP probes; saw {len(probes)}')
first, second = probes[:2]
if (len(first), first[38:42], first[-1]) != (64, bytes((10, 0, 2, 2)), 0xa1):
    raise SystemExit('FAIL: first frame length, target or marker changed')
if (len(second), second[38:42], second[-1]) != (60, bytes((10, 0, 2, 3)), 0xb2):
    raise SystemExit('FAIL: second frame length, target or marker changed')
if first[6:12] != second[6:12] or first[6:12] != first[22:28]:
    raise SystemExit('FAIL: inconsistent source MAC')
print(f'PASS: distinct ordered 64-byte and 60-byte ARP frames; {len(frames)} captured frames')

if args.m1:
    dhcp, echo_request, echo_reply, segments = set(), 0, 0, {}
    for frame in frames:
        if len(frame) < 34 or frame[12:14] != b'\x08\x00':
            continue
        ip = frame[14:]
        ihl = (ip[0] & 15) * 4
        if ihl < 20 or len(ip) < ihl:
            continue
        body = ip[ihl:struct.unpack_from('!H', ip, 2)[0]]
        if ip[9] == 17 and len(body) > 248:
            src, dst = struct.unpack_from('!HH', body)
            if {src, dst} == {67, 68} and body[244:248] == b'\x63\x82\x53\x63':
                options, offset = body[248:], 0
                while offset < len(options):
                    kind = options[offset]; offset += 1
                    if kind == 255: break
                    if kind == 0: continue
                    if offset >= len(options): break
                    size = options[offset]; offset += 1
                    value = options[offset:offset + size]; offset += size
                    if kind == 53 and size == 1: dhcp.add(value[0])
        if ip[9] == 1 and len(body) >= 8 and b'usb-wdm-free M1' in body[8:]:
            echo_request += body[0] == 8
            echo_reply += body[0] == 0
        if ip[9] == 6 and len(body) >= 20:
            src, dst, seq = struct.unpack_from('!HHI', body)
            header = (body[12] >> 4) * 4
            if src == 44342 and header >= 20 and body[header:]:
                payload = body[header:]
                connection = segments.setdefault(dst, {})
                add_segment(connection, seq, payload)
    expected = bytes((i * 37 + 11) & 255 for i in range(32768))
    if not {1, 2, 3, 5}.issubset(dhcp):
        raise SystemExit(f'FAIL: incomplete DHCP exchange: {dhcp}')
    if echo_request < 3 or echo_reply < 3:
        raise SystemExit(f'FAIL: ICMP request/reply counts {echo_request}/{echo_reply}')
    if not segments:
        raise SystemExit('FAIL: no TCP data')
    for connection in segments.values():
        stream = reassemble(connection)
        if stream != expected:
            raise SystemExit(f'FAIL: TCP data mismatch, {len(stream)} bytes')
    print(f'PASS: DHCP discover/offer/request/ACK; ICMP {echo_request}/{echo_reply}; {len(segments)} TCP transfers of 32768 verified bytes')

if args.load:
    udp_packets, load_segments = 0, {}
    for frame in frames:
        if len(frame) < 34 or frame[12:14] != b'\x08\x00':
            continue
        ip = frame[14:]
        ihl = (ip[0] & 15) * 4
        if ihl < 20 or len(ip) < ihl:
            continue
        body = ip[ihl:struct.unpack_from('!H', ip, 2)[0]]
        if ip[9] == 17 and len(body) >= 8 and struct.unpack_from('!H', body, 2)[0] == 44343:
            data = body[8:]
            if len(data) != 1400 or data[0] >= 64 or any(b != data[0] for b in data):
                raise SystemExit('FAIL: corrupted UDP load payload')
            udp_packets += 1
        if ip[9] == 6 and len(body) >= 20:
            src, dst, seq = struct.unpack_from('!HHI', body)
            header = (body[12] >> 4) * 4
            if src == 44344 and header >= 20 and body[header:]:
                connection = load_segments.setdefault(dst, {})
                data = body[header:]
                add_segment(connection, seq, data)
    if not udp_packets or not load_segments:
        raise SystemExit('FAIL: missing load traffic')
    totals = []
    for connection in load_segments.values():
        stream = reassemble(connection)
        if len(stream) < 32768 or stream != bytes((i * 37 + 11) & 255 for i in range(len(stream))):
            raise SystemExit(f'FAIL: load TCP payload mismatch or too short: {len(stream)}')
        totals.append(len(stream))
    print(f'PASS: {udp_packets} complete UDP payloads; load TCP verified prefix bytes {totals}')
