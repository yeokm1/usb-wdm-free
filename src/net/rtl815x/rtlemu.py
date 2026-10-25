#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""SIMULATED Realtek RTL8152/RTL8153 USB Ethernet adapter over localhost
usbredir (test aid only).

No RTL815x is on the bench, so this is the vehicle for rtl815x's legs. It
models what rtl815x's own chip core (rtlchip.c) reads and writes, not the
chip's firmware: the vendor register request (request 0x05, bmRequestType
0xC0/0x40, wValue the register address, wIndex the space, MAC 0x0100 or
USB 0x0000, with byte lanes; rtlchip.h), the MAC and USB register spaces,
the PHY window at MAC 0xB000-0xBFFF behind MR_PHY_PAGE, the chip id in
MR_TX_CONFIG, the bits the bring-up polls (CMD_RESET self-clearing,
OOB_LIST_READY, OOB_FIFOS_EMPTY, TXC_EMPTY, BOOT_LOADED, the PHY reset bit,
PATCH_GRANTED), the MAC address in MR_NODE_ID_SAVED (and MR_NODE_ID for the
4C00), link in MR_LINK with an interrupt-IN report (IRQ_LINK) on each
change, receive framing (24-byte record header, frame length with FCS,
8-byte alignment, several frames per bulk-IN transfer) and transmit framing
(8-byte header, 4-byte alignment). Register semantics follow rtltest.c's
register-file model.

Behind the adapter sits a SIMULATED network of one host, 192.168.77.1
(02:00:5E:77:00:01): it answers ARP, ICMP echo and DHCP (offering
192.168.77.2/24, router 192.168.77.1), and logs every frame. Frames reach
the guest only while the receiver is on (MR_COMMAND CMD_RX_ON) and the receive
configuration (MR_RX_ACCEPT, MR_HASH) accepts them.

Chips (--chip): 8152 (id 4C10, 0BDA:8152, 100 Mb/s), 8153 (id 5C10,
0BDA:8153, 1000 Mb/s, default), 8153b (id 6010, 0BDA:8153). --speed
high|full; --link-after seconds after the PHY's autonegotiation restart
(default 1.0); --flap-after/--flap-for take the cable out once. No
physical USB device is accessed.
"""
import argparse
import collections
import hashlib
import json
import select
import socket
import struct
import time
import zlib
from pathlib import Path

CAP_VERSION, CAP_EP_MAX, CAP_ID64, CAP_BULK32 = 1, 4, 5, 6
CAPS = (1 << CAP_VERSION) | (1 << CAP_EP_MAX) | (1 << CAP_ID64) | (1 << CAP_BULK32)

# Register names and values: rtlchip.h.
RTL_SPACE_MAC = 0x0100
RTL_LANES_ALL = 0xff
MR_NODE_ID, MR_RX_ACCEPT, MR_HASH, MR_NODE_ID_SAVED = 0xc000, 0xc010, 0xcd00, 0xd000
MR_BOOT, MR_TX_CONFIG, MR_COMMAND, MR_OOB = 0xe004, 0xe610, 0xe813, 0xe84f
MR_CPLUS, MR_PHY_PAGE, MR_LINK = 0xe854, 0xe86c, 0xe908
UR_CTRL = 0xd406
PR_MII, PR_STATE, PR_PATCH_STATE, PR_PATCH_CMD = 0xa400, 0xa420, 0xb800, 0xb820
MIICTL_RESET, MIICTL_AN_RESTART = 0x8000, 0x0200
PATCH_ASK, PATCH_GRANTED = 0x0010, 0x0040
CMD_RESET, CMD_RX_ON, CMD_TX_ON = 0x10, 0x08, 0x04
OOB_LIST_READY, OOB_FIFOS_EMPTY, OOB_ACTIVE = 0x02, 0x30, 0x80
TXC_EMPTY, BOOT_LOADED, PHYST_LAN_ON = 0x0800, 0x0002, 3
ACC_ALL_PHYS, ACC_MY_PHYS, ACC_MCAST, ACC_BCAST = 1, 2, 4, 8
LNK_1000, LNK_100, LNK_UP, LNK_FULL = 0x10, 0x08, 0x02, 0x01
IRQ_LINK = 0x04
RX_DESC_LEN, TX_DESC_LEN = 24, 8

CHIPS = {'8152': (0x4c10, 0x8152, LNK_100), '8153': (0x5c10, 0x8153, LNK_1000),
         '8153b': (0x6010, 0x8153, LNK_1000)}
BACKUP_MAC = bytes.fromhex('00e04c770002')
HOST_MAC, HOST_IP = bytes.fromhex('02005e770001'), bytes((192, 168, 77, 1))
GUEST_IP = bytes((192, 168, 77, 2))
EP_IN, EP_OUT, EP_INT = 0x81, 0x02, 0x83


class Chip:
    """The register file (rtltest.c's MODEL in Python)."""

    def __init__(self, chip='8153'):
        self.tcr0, self.pid, self.rate_bit = CHIPS[chip]
        self.pla, self.usb = bytearray(65536), bytearray(65536)
        self.phy = [0] * 32768
        self.pla[MR_OOB] = OOB_LIST_READY | OOB_FIFOS_EMPTY
        self.pla[MR_TX_CONFIG + 1] = TXC_EMPTY >> 8
        self.pla[MR_BOOT] = BOOT_LOADED
        self.pla[0xe000 + 1] = 0x01                  # MR_POWER_STATE bit 8, polled after ALDPS off
        self.phy[PR_STATE // 2] = PHYST_LAN_ON
        self.pla[MR_NODE_ID_SAVED:MR_NODE_ID_SAVED + 6] = BACKUP_MAC
        self.pla[MR_NODE_ID:MR_NODE_ID + 6] = BACKUP_MAC if self.tcr0 != 0x4c00 else bytes.fromhex('00e04c770003')
        self.pla[MR_RX_ACCEPT] = ACC_ALL_PHYS | ACC_MY_PHYS | ACC_MCAST | ACC_BCAST
        self.usb[UR_CTRL] = 0x90
        self.pla[MR_CPLUS] = 0x40
        self.autoneg = 0                             # restarts seen
        self.writes = self.reads = 0

    def phy_address(self, pla):
        base = self.pla[MR_PHY_PAGE] | self.pla[MR_PHY_PAGE + 1] << 8
        return (base | (pla & 0x0fff)) & 0xffff

    def phy_written(self, addr, value):
        if addr == PR_MII:                     # BMCR
            if value & MIICTL_AN_RESTART:
                self.autoneg += 1
            value &= ~MIICTL_RESET                     # the reset completes
        if addr == PR_PATCH_CMD:
            st = self.phy[PR_PATCH_STATE // 2]
            self.phy[PR_PATCH_STATE // 2] = st | PATCH_GRANTED if value & PATCH_ASK else st & ~PATCH_GRANTED
        self.phy[addr // 2] = value & 0xffff

    def read(self, value, index, length):
        if value & 3 or not length or length & 3 or value + length > 0x10000 or index & ~0x1ff or length > 64:
            return None
        self.reads += 1
        space = self.pla if index & RTL_SPACE_MAC else self.usb
        out = bytearray(length)
        for k in range(length):
            a = value + k
            if index & RTL_SPACE_MAC and 0xb000 <= a < 0xc000:
                w = self.phy[self.phy_address(a & ~1) // 2]
                out[k] = (w >> 8 if a & 1 else w) & 255
            else:
                out[k] = space[a]
        if index & RTL_SPACE_MAC and value == MR_TX_CONFIG and length >= 4:
            out[2], out[3] = self.tcr0 & 255, self.tcr0 >> 8
        return bytes(out)

    def write(self, value, index, data):
        length, byen = len(data), index & 0xff
        if value & 3 or not length or length & 3 or value + length > 0x10000 or index & ~0x1ff or \
                length > 512 or (byen != RTL_LANES_ALL and (length != 4 or not byen)):
            return False
        self.writes += 1
        space = self.pla if index & RTL_SPACE_MAC else self.usb
        for k in range(length):
            a = value + k
            if byen != RTL_LANES_ALL and not byen & (1 << (k & 3)):
                continue
            if index & RTL_SPACE_MAC and 0xb000 <= a < 0xc000:
                pa = self.phy_address(a & ~1)
                if a & 1:
                    self.phy_written(pa, (self.phy[pa // 2] & 0x00ff) | data[k] << 8)
                else:
                    self.phy[pa // 2] = (self.phy[pa // 2] & 0xff00) | data[k]
                continue
            space[a] = data[k]
        self.pla[MR_COMMAND] &= ~CMD_RESET & 0xff
        return True

    def dword(self, a):
        return struct.unpack_from('<I', self.pla, a)[0]

    @property
    def mac(self):
        return bytes(self.pla[MR_NODE_ID:MR_NODE_ID + 6])

    def receiving(self):
        return bool(self.pla[MR_COMMAND] & CMD_RX_ON) and not self.pla[MR_OOB] & OOB_ACTIVE

    def accepts(self, frame):
        """MR_RX_ACCEPT/MR_HASH receive decision for a destination address."""
        rcr, dest = self.dword(MR_RX_ACCEPT), frame[:6]
        if rcr & ACC_ALL_PHYS:
            return True
        if dest == b'\xff' * 6:
            return bool(rcr & ACC_BCAST)
        if dest[0] & 1:
            if not rcr & ACC_MCAST:
                return False
            bit = ether_crc(dest) >> 26
            # MR_HASH: bins 32-63 in bytes 0-3, bins 0-31 in bytes 4-7, each
            # half a big-endian 32-bit word.
            mc = [int.from_bytes(self.pla[MR_HASH + 4:MR_HASH + 8], 'big'),
                  int.from_bytes(self.pla[MR_HASH:MR_HASH + 4], 'big')]
            return bool(mc[bit >> 5] >> (bit & 31) & 1)
        return bool(rcr & ACC_MY_PHYS) and dest == self.mac


def ether_crc(data):
    """The bit-reversed, non-inverted CRC-32 of data (netchip.c NcEtherCrc)."""
    crc = 0xffffffff
    for byte in data:
        for bit in range(8):
            carry = (crc >> 31) ^ (byte >> bit & 1)
            crc = (crc << 1) & 0xffffffff
            if carry:
                crc ^= 0x04c11db7
    return crc


def checksum(data):
    if len(data) & 1:
        data += b'\0'
    s = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff


def ipv4(src, dst, proto, payload):
    head = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), 0, 0x4000, 64, proto, 0, src, dst)
    return head[:10] + struct.pack('!H', checksum(head)) + head[12:] + payload


class Network:
    """One SIMULATED host behind the adapter."""

    def __init__(self, log):
        self.log = log
        self.lease = None

    def frame(self, dest, ethertype, payload):
        return dest + HOST_MAC + struct.pack('!H', ethertype) + payload

    def handle(self, frame):
        """A frame the guest sent; returns the frames the host answers."""
        if len(frame) < 14:
            return []
        dest, src, ethertype = frame[:6], frame[6:12], struct.unpack('!H', frame[12:14])[0]
        body = frame[14:]
        if ethertype == 0x0806 and len(body) >= 28:
            op = struct.unpack('!H', body[6:8])[0]
            if op == 1 and body[24:28] == HOST_IP:
                self.log('arp_request', sender=body[14:18].hex(), target=body[24:28].hex())
                reply = body[:6] + struct.pack('!H', 2) + HOST_MAC + HOST_IP + body[8:14] + body[14:18]
                return [self.frame(src, 0x0806, reply)]
            return []
        if ethertype != 0x0800 or len(body) < 20 or body[0] >> 4 != 4:
            self.log('frame_ignored', ethertype=ethertype, length=len(frame))
            return []
        ihl = (body[0] & 15) * 4
        total = struct.unpack('!H', body[2:4])[0]
        proto, sip, dip = body[9], body[12:16], body[16:20]
        data = body[ihl:total]
        if proto == 1 and dip == HOST_IP and data[:1] == b'\x08':
            self.log('icmp_echo', source=sip.hex(), length=len(data))
            reply = b'\0\0' + b'\0\0' + data[4:]
            reply = reply[:2] + struct.pack('!H', checksum(reply)) + reply[4:]
            return [self.frame(src, 0x0800, ipv4(HOST_IP, sip, 1, reply))]
        if proto == 17 and len(data) >= 8 and struct.unpack('!H', data[2:4])[0] == 67:
            return self.dhcp(src, data[8:])
        self.log('ip_ignored', protocol=proto, destination=dip.hex(), length=len(data))
        return []

    def dhcp(self, src, boot):
        if len(boot) < 240 or boot[0] != 1 or boot[236:240] != b'\x63\x82\x53\x63':
            return []
        options, i, kind = boot[240:], 0, None
        while i < len(options) and options[i] != 255:
            if options[i] == 0:
                i += 1
                continue
            if i + 1 >= len(options):
                break
            if options[i] == 53 and options[i + 1] == 1:
                kind = options[i + 2]
            i += 2 + options[i + 1]
        answer = {1: 2, 3: 5}.get(kind)                 # DISCOVER -> OFFER, REQUEST -> ACK
        if answer is None:
            return []
        self.log('dhcp', request=kind, answer=answer, client=boot[28:34].hex())
        if answer == 5:
            self.lease = GUEST_IP
        reply = bytearray(236)
        reply[0:4] = b'\x02\x01\x06\x00'
        reply[4:8] = boot[4:8]                          # xid
        reply[10:12] = boot[10:12]                      # flags
        reply[16:20] = GUEST_IP
        reply[20:24] = HOST_IP
        reply[28:44] = boot[28:44]                      # chaddr
        reply += b'\x63\x82\x53\x63' + bytes((53, 1, answer, 54, 4)) + HOST_IP + \
            bytes((51, 4)) + struct.pack('!I', 86400) + bytes((1, 4, 255, 255, 255, 0, 3, 4)) + HOST_IP + b'\xff'
        udp = struct.pack('!HHHH', 67, 68, 8 + len(reply), 0) + bytes(reply)
        return [self.frame(b'\xff' * 6, 0x0800, ipv4(HOST_IP, b'\xff' * 4, 17, udp))]


def device_descriptor(pid, speed):
    return struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x0210 if speed == 'high' else 0x0200, 0, 0, 0, 64,
                       0x0bda, pid, 0x3000, 1, 2, 0, 1)


def config_descriptor(speed, kind=2):
    bulk = 512 if speed == 'high' else 64
    body = bytes((9, 4, 0, 0, 3, 0xff, 0xff, 0, 0)) + \
        bytes((7, 5, EP_IN, 2)) + struct.pack('<HB', bulk, 0) + \
        bytes((7, 5, EP_OUT, 2)) + struct.pack('<HB', bulk, 0) + \
        bytes((7, 5, EP_INT, 3)) + struct.pack('<HB', 2, 8 if speed == 'high' else 16)
    return bytes((9, kind)) + struct.pack('<HBBBBB', 9 + len(body), 1, 1, 0, 0xa0, 50) + body


def usb_string(text):
    raw = text.encode('utf-16le')
    return bytes((len(raw) + 2, 3)) + raw


class Model:
    def __init__(self, connection, trace, chip='8153', speed='high', now=time.monotonic,
                 link_after=1.0, flap_after=None, flap_for=2.0):
        self.connection, self.trace, self.now = connection, trace, now
        self.chip_name, self.speed = chip, speed
        self.chip = Chip(chip)
        self.net = Network(self.log)
        self.device = device_descriptor(self.chip.pid, speed)
        self.config_desc = config_descriptor(speed)
        self.other = config_descriptor('full' if speed == 'high' else 'high', 7)
        self.strings = {0: b'\x04\x03\x09\x04', 1: usb_string('usb-wdm-free SIMULATED'),
                        2: usb_string('RTL%s Ethernet - SIMULATED' % chip.upper())}
        self.bulk_max = 512 if speed == 'high' else 64
        self.link_after, self.flap_after, self.flap_for = link_after, flap_after, flap_for
        self.peer_caps = 0
        self.config = 0
        self.interrupt = False
        self.pending = collections.OrderedDict()   # bulk-IN request id -> length
        self.rx = collections.deque()              # frames waiting for the guest
        self.link = False
        self.link_at = self.flap_at = self.restore_at = None
        self.flapped = False
        self.autoneg_seen = 0
        self.irq = collections.deque()
        self.totals = collections.Counter()

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
            for slot, kind, interval, size in ((16 + (EP_IN & 15), 2, 0, self.bulk_max),
                                               (EP_OUT, 2, 0, self.bulk_max),
                                               (16 + (EP_INT & 15), 3, 8 if self.speed == 'high' else 16, 2)):
                types[slot], intervals[slot], packets[slot] = kind, interval, size
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << CAP_EP_MAX):
            data += struct.pack('<32H', *packets)
        self.send(5, data=data)
        numbers, classes, subclasses, protocols = (bytearray(32) for _ in range(4))
        classes[0], subclasses[0] = 0xff, 0xff
        self.send(4, data=struct.pack('<I', 1 if self.config else 0) + numbers + classes + subclasses + protocols)

    def reset_device(self):
        for ident in list(self.pending):
            self.send(101, ident, self.bulk_header(EP_IN, 1, 0))
        self.pending.clear()
        self.rx.clear()
        self.config, self.interrupt = 0, False

    # -- link ----------------------------------------------------------------
    def set_link(self, up, why):
        if up == self.link:
            return
        self.link = up
        self.chip.pla[MR_LINK] = (LNK_UP | self.chip.rate_bit | LNK_FULL) if up else 0
        self.log('link', up=up, why=why)
        self.irq.append(bytes((IRQ_LINK, 0)))
        self.flush_irq()

    def flush_irq(self):
        while self.interrupt and self.irq:
            data = self.irq.popleft()
            self.send(103, data=struct.pack('<BBH', EP_INT, 0, len(data)) + data)
            self.totals['interrupts'] += 1

    def pump(self):
        now = self.now()
        if self.chip.autoneg != self.autoneg_seen:
            self.autoneg_seen = self.chip.autoneg
            self.set_link(False, 'autoneg_restart')
            self.link_at = now + self.link_after
        if self.link_at is not None and now >= self.link_at:
            self.link_at = None
            if self.restore_at is None:
                self.set_link(True, 'autoneg_done')
                if self.flap_after is not None and not self.flapped:
                    self.flap_at = now + self.flap_after
        if self.flap_at is not None and now >= self.flap_at:
            self.flap_at, self.flapped = None, True
            self.set_link(False, 'cable_out')
            self.restore_at = now + self.flap_for
        if self.restore_at is not None and now >= self.restore_at:
            self.restore_at = None
            self.set_link(True, 'cable_in')
        self.drain()

    # -- data ----------------------------------------------------------------
    def deliver(self, frame):
        """A frame from the network to the adapter's receiver."""
        if not (self.link and self.chip.receiving() and self.chip.accepts(frame)):
            self.totals['rx_filtered'] += 1
            return
        if len(frame) < 60:
            frame += bytes(60 - len(frame))
        if len(self.rx) < 256:
            self.rx.append(frame)
        self.drain()

    def aggregate(self, length):
        """One bulk-IN transfer: record header + frame + FCS, each record 8-byte
        aligned, as many as fit in length."""
        out = bytearray()
        while self.rx:
            frame = self.rx[0]
            record = RX_DESC_LEN + len(frame) + 4
            padded = (record + 7) & ~7
            if len(out) + record > length:
                break
            self.rx.popleft()
            desc = struct.pack('<I', len(frame) + 4) + bytes(RX_DESC_LEN - 4)
            out += desc + frame + struct.pack('<I', zlib.crc32(frame))
            out += b'\xee' * (padded - record)
            self.totals['rx_frames'] += 1
        if len(out) > length:
            out = out[:length]
        return bytes(out)

    def drain(self):
        while self.pending and self.rx:
            ident, length = next(iter(self.pending.items()))
            data = self.aggregate(length)
            if not data:
                break
            del self.pending[ident]
            self.send(101, ident, self.bulk_header(EP_IN, 0, len(data)) + data)
            self.totals['rx_transfers'] += 1
            self.log('bulk_in', length=len(data), sha256=hashlib.sha256(data).hexdigest())

    def transmit(self, data):
        """One bulk-OUT transfer: 8-byte header + frame, each 4-byte aligned."""
        frames, off = [], 0
        while off + TX_DESC_LEN <= len(data):
            opts1 = struct.unpack_from('<I', data, off)[0]
            length = opts1 & 0x3ffff
            if not length or off + TX_DESC_LEN + length > len(data):
                raise ValueError('transmit header length beyond the transfer')
            if opts1 & 0xc0000000 != 0xc0000000:
                raise ValueError('transmit header without the first and last segment bits')
            frames.append(data[off + TX_DESC_LEN:off + TX_DESC_LEN + length])
            off = (off + TX_DESC_LEN + length + 3) & ~3
        if off < len(data) and any(data[off:]):
            raise ValueError('trailing bytes after the last frame')
        if not (self.chip.pla[MR_COMMAND] & CMD_TX_ON):
            self.totals['tx_while_disabled'] += 1
        for frame in frames:
            self.totals['tx_frames'] += 1
            self.log('tx_frame', length=len(frame), destination=frame[:6].hex(),
                     ethertype=frame[12:14].hex() if len(frame) >= 14 else '')
            if self.link:
                for reply in self.net.handle(frame):
                    self.deliver(reply)
        return frames

    # -- control -------------------------------------------------------------
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
                self.strings.get(number) if kind == 3 else \
                (struct.pack('<BBHBBBBBB', 10, 6, 0x200, 0, 0, 0, 64, 1, 0) if kind == 6 and not number else
                 self.other if kind == 7 and not number else None)
            status, data = (0, data) if data is not None else (4, b'')
        elif request == 0 and rt in (0x80, 0x81, 0x82):
            status, data = 0, b'\0\0'
        elif request == 8 and rt == 0x80:
            status, data = 0, bytes((self.config,))
        elif request == 10 and rt == 0x81:
            status, data = 0, b'\0'
        elif request in (1, 3) and rt == 2 and value == 0:
            status = 0
        elif request == 5 and rt == 0xc0 and self.config:
            got = self.chip.read(value, index, length)
            status, data = (0, got) if got is not None else (4, b'')
        elif request == 5 and rt == 0x40 and self.config:
            status = 0 if self.chip.write(value, index, payload) else 4
            if not status:
                self.pump()
        data = data[:length]
        actual = len(data) if rt & 128 else (0 if status else length)
        self.send(100, ident, struct.pack('<BBBBHHH', ep, request, rt, status, value, index, actual) + data)
        if rt & 0x60 != 0x40:
            self.log('control', request=request, request_type=rt, value=value, index=index,
                     requested=length, status=status)
        else:
            self.totals['vendor_requests'] += 1
            if status:
                self.log('vendor_refused', request_type=rt, value=value, index=index, length=length)

    # -- usbredir messages -----------------------------------------------------
    def handle(self, kind, ident, packet):
        if kind == 3:
            if packet:
                raise ValueError('reset length')
            self.reset_device()
            self.log('reset')
        elif kind in (6, 7):
            if len(packet) != (1 if kind == 6 else 0):
                raise ValueError('configuration length')
            status = 0
            if kind == 6:
                status = 0 if packet[0] in (0, 1) else 4
                if not status:
                    self.reset_device()
                    self.config = packet[0]
                    self.info()
                    self.log('configuration', value=self.config)
            self.send(8, ident, bytes((status, self.config)))
        elif kind in (9, 10):
            if len(packet) != (2 if kind == 9 else 1):
                raise ValueError('alternate length')
            ok = self.config and packet[0] == 0 and (kind == 10 or packet[1] == 0)
            self.send(11, ident, bytes((0 if ok else 4, packet[0], 0)))
        elif kind in (15, 16):
            if len(packet) != 1:
                raise ValueError('interrupt header')
            status = 0 if packet[0] == EP_INT and self.config else 4
            self.interrupt = kind == 15 and not status
            self.send(17, ident, bytes((status, packet[0])))
            self.flush_irq()
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
            if stream or not self.config or endpoint not in (EP_IN, EP_OUT):
                self.send(101, ident, self.bulk_header(endpoint, 4, 0))
            elif endpoint == EP_OUT:
                data = packet[size:]
                if len(data) != length:
                    raise ValueError('bulk OUT length mismatch')
                self.send(101, ident, self.bulk_header(EP_OUT, 0, length))
                if data:
                    self.transmit(data)
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
        data = struct.pack('<BBBBHH', 2 if self.speed == 'high' else 1, 0, 0, 0, 0x0bda, self.chip.pid)
        if self.peer_caps & (1 << CAP_VERSION):
            data += struct.pack('<H', 0x3000)
        self.send(1, data=data)
        self.log('connected', model='SIMULATED RTL%s' % self.chip_name, speed=self.speed,
                 mac=self.chip.mac.hex(), config_sha256=hashlib.sha256(self.config_desc).hexdigest())

    def run(self):
        self.send(0, data=b'usb-wdm-free RTL815x SIMULATED'.ljust(64, b'\0') + struct.pack('<I', CAPS))
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
    ap.add_argument('--chip', choices=sorted(CHIPS), default='8153')
    ap.add_argument('--speed', choices=('high', 'full'), default='high')
    ap.add_argument('--link-after', type=float, default=1.0)
    ap.add_argument('--flap-after', type=float, help='seconds after link-up to take the cable out, once')
    ap.add_argument('--flap-for', type=float, default=2.0)
    args = ap.parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'SIMULATED RTL{args.chip} listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            model = Model(connection, trace, args.chip, args.speed, link_after=args.link_after,
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
