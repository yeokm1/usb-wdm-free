#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# ASIX AX88179/178A USB 3.0/2.0 to Gigabit Ethernet Devices
# Copyright (C) 2011-2013 ASIX
# Copyright (C) 2026 Yeo Kheng Meng
"""SIMULATED ASIX AX88179 USB Ethernet adapter over localhost usbredir
(test aid only).

The owner's AX88179 is not on this host, so this is the vehicle for
ax88179's legs without it. It models what ax88179's own chip core
(axchip.c, axframe.c) reads and writes, not the chip's firmware. Where
each fact comes from:

- Identity and descriptors: the physical capture in testdata/ax179/
  (0B95:1790, bcdDevice 0100, bcdUSB 0300, one configuration, interface
  0 class FF/FF/00, interrupt IN 0x81 of 8 bytes with interval 0x0b,
  bulk IN 0x82 and bulk OUT 0x03, 1024 bytes with their SuperSpeed
  companions). At SuperSpeed the device and configuration descriptors
  are those bytes. The High and Full Speed forms (bcdUSB 0210 and 0200,
  bulk 512 and 64 bytes, no companions) and the BOS are this model's
  choice: no USB 2.0 capture exists (testdata/ax2k is QEMU passing the
  SuperSpeed set through to EHCI).
- The vendor requests, as axchip.h names them from Linux v6.12
  drivers/net/usb/ax88179_178a.c (pinned adc218676eef; facts only):
  ACCESS_MAC 0x01 (wValue the register, wIndex its width, the data the
  bytes), ACCESS_PHY 0x02 (wValue the PHY address 3, wIndex the PHY
  register, two bytes), ACCESS_EEPROM 0x04 (wValue the word, wIndex 1,
  two bytes) and 0x81 (wValue 0x8c, four bytes: the TX FIFO status that
  link_reset polls, bit 30 full). CONTROL.md's table gives the setup
  bytes of three of them as Linux issues them.
- The registers and bits: axchip.h (PHYSICAL_LINK_STATUS 0x02 with its
  SuperSpeed and High Speed bits, RX_CTL 0x0b, NODE_ID 0x10, MULFLTARY
  0x16, MEDIUM_STATUS_MODE 0x22, PHYPWR_RSTCTL 0x26, RX_BULKIN_QCTRL
  0x2e, CLK_SELECT 0x33), and the PHY's GMII_PHY_PHYSR 0x11 (link 0400,
  full duplex 2000, speed C000) and MII BMCR (autonegotiation enable and
  restart).
- The status (interrupt IN) report: eight bytes, the first little-endian
  dword's bit 16 the link (AX_INT_LINK, axchip.h; Linux's
  AX_INT_PPLS_LINK).
- Receive framing (axframe.c, from Linux's ax88179_rx_fixup): each frame
  is stored after two padding bytes, padded to 8; after the frames, one
  dword per frame with its length (frame + 2) in bits 16-28 and its error
  bits (0x80000000 drop, 0x20000000 CRC) clear; the transfer ends in a
  dword holding the count in bits 0-15 and the headers' offset in bits
  16-31. No FCS by default (--fcs forwards one in every record, as the
  owner's adapter did in its first session after a plug:
  docs/testing/2026-10-22-ax88179-fcs.md).
- Transmit framing (axframe.c, Linux's ax88179_tx_fixup): eight bytes,
  the frame length and then the flags dword (0x80008000 when header and
  frame end on a packet boundary), then the frame; one frame per bulk-OUT
  transfer.
- Power-up values the facts do not give are this model's: the MAC
  00:0e:c6:77:00:02 in NODE_ID, the PHY's BMCR 1140 (autonegotiation on),
  EEPROM word 0x43 0x0000 (no auto-detach), the FIFO never full.

Link: the PHY is up when PHYPWR_RSTCTL holds IPRL and CLK_SELECT both
clocks; it links (1000 Mb/s full duplex, or --mbps) --link-after seconds
after it is powered or its autonegotiation restarts, with a status report
on each change; --flap-after/--flap-for take the cable out once. Frames
reach the guest only while the receiver is on (RX_CTL START, MEDIUM
RECEIVE_EN) and RX_CTL's filter accepts them (directed always, as the
driver's comment says; broadcast AB; multicast AMALL or AM with the
MULFLTARY hash; PRO everything).

Behind the adapter sits a SIMULATED network of one host, 192.168.77.1
(02:00:5E:77:00:01): it answers ARP, ICMP echo and DHCP (offering
192.168.77.2/24, router 192.168.77.1), and logs every frame (the same
host as rtlemu.py's). No physical USB device is accessed.
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
SPEED_CODE = {'full': 1, 'high': 2, 'super': 3}  # usbredir speeds
HOST_MAC, HOST_IP = bytes.fromhex('02005e770001'), bytes((192, 168, 77, 1))
GUEST_IP = bytes((192, 168, 77, 2))
TESTDATA = Path(__file__).resolve().parent / 'testdata' / 'ax179'
VENDOR, PRODUCT, RELEASE = 0x0b95, 0x1790, 0x0100
EP_INT, EP_IN, EP_OUT = 0x81, 0x82, 0x03
ACCESS_MAC, ACCESS_PHY, ACCESS_EEPROM, FIFO_STATUS = 0x01, 0x02, 0x04, 0x81
PHY_ID = 0x03
R_LINK_STATUS, R_RX_CTL, R_NODE_ID, R_MULTI = 0x02, 0x0b, 0x10, 0x16
R_MEDIUM, R_PHYPWR, R_QCTRL, R_CLK = 0x22, 0x26, 0x2e, 0x33
USB_SS, USB_HS = 0x04, 0x02
RX_START, RX_AP, RX_AM, RX_AB, RX_AMALL, RX_PRO = 0x0080, 0x0020, 0x0010, 0x0008, 0x0002, 0x0001
MEDIUM_RECEIVE = 0x0100
PHYPWR_IPRL = 0x0020
CLK_BCS, CLK_ACS = 0x01, 0x02
BMCR, BMSR, PHYSR, PAGE_SELECT = 0x00, 0x01, 0x11, 0x1f
BMCR_ANENABLE, BMCR_ANRESTART, BMCR_RESET = 0x1000, 0x0200, 0x8000
BMSR_LINK, BMSR_ANDONE = 0x0004, 0x0020
PHYSR_LINK, PHYSR_FULL = 0x0400, 0x2000
PHYSR_SPEED = {1000: 0x8000, 100: 0x4000, 10: 0x0000}
INT_LINK = 0x00010000
FIFO_FULL = 0x40000000
TX_PAD = 0x80008000
MODEL_MAC = bytes.fromhex('000ec6770002')
EEPROM = {0x43: 0x0000}


class Chip:
    """The MAC register file, the PHY and the EEPROM words the core reads."""

    def __init__(self, mbps=1000):
        self.mac_regs = bytearray(256)
        self.mac_regs[R_NODE_ID:R_NODE_ID + 6] = MODEL_MAC
        self.pages = {0: [0] * 32}
        self.pages[0][BMCR] = 0x1140
        self.pages[0][BMSR] = 0x7949
        self.mmd = {}
        self.mmd_ctrl = 0
        self.mbps = mbps
        self.link = False
        self.restarts = 0                    # PHY power-ups and autonegotiation restarts
        self.speed_bits = 0
        self.reads = self.writes = 0

    def word(self, reg):
        return self.mac_regs[reg] | self.mac_regs[reg + 1] << 8

    def page(self):
        return self.pages[0][PAGE_SELECT] & 0x1f

    def powered(self):
        return bool(self.word(R_PHYPWR) & PHYPWR_IPRL) and \
            self.mac_regs[R_CLK] & (CLK_ACS | CLK_BCS) == CLK_ACS | CLK_BCS

    def receiving(self):
        return bool(self.word(R_RX_CTL) & RX_START) and bool(self.word(R_MEDIUM) & MEDIUM_RECEIVE)

    def accepts(self, frame):
        rx, dest = self.word(R_RX_CTL), frame[:6]
        if rx & RX_PRO:
            return True
        if dest == b'\xff' * 6:
            return bool(rx & RX_AB)
        if dest[0] & 1:
            if rx & RX_AMALL:
                return True
            if not rx & RX_AM:
                return False
            bit = ether_crc(dest) >> 26
            return bool(self.mac_regs[R_MULTI + (bit >> 3)] >> (bit & 7) & 1)
        return dest == bytes(self.mac_regs[R_NODE_ID:R_NODE_ID + 6])

    # -- PHY -----------------------------------------------------------------
    def phy_read(self, reg):
        if reg == PHYSR and self.page() == 0:
            return (PHYSR_LINK | PHYSR_FULL | PHYSR_SPEED[self.mbps]) if self.link else 0
        if reg == BMSR and self.page() == 0:
            v = self.pages[0][BMSR] & ~(BMSR_LINK | BMSR_ANDONE)
            return v | (BMSR_LINK | BMSR_ANDONE if self.link else 0)
        if reg == 0x0e and self.page() == 0:
            return self.mmd.get(self.mmd_ctrl, 0)
        return self.pages.setdefault(self.page() if reg != PAGE_SELECT else 0, [0] * 32)[reg]

    def phy_write(self, reg, value):
        page = 0 if reg == PAGE_SELECT else self.page()
        regs = self.pages.setdefault(page, [0] * 32)
        if page == 0 and reg == BMCR:
            if value & (BMCR_ANRESTART | BMCR_RESET):
                self.restarts += 1
            value &= ~(BMCR_ANRESTART | BMCR_RESET)
        if page == 0 and reg == 0x0d:
            self.mmd_ctrl = value
        if page == 0 and reg == 0x0e:
            self.mmd[self.mmd_ctrl] = value      # the device/register pair's data, as written
        regs[reg] = value & 0xffff

    # -- vendor requests -------------------------------------------------------
    def read(self, request, value, index, length, speed):
        """bytes, or None for a STALL."""
        if request == ACCESS_MAC:
            if not 1 <= index <= 8 or length != index or value + length > 256:
                return None
            self.reads += 1
            out = bytearray(self.mac_regs[value:value + length])
            if value <= R_LINK_STATUS < value + length:
                out[R_LINK_STATUS - value] = {'super': USB_SS, 'high': USB_HS}.get(speed, 0)
            return bytes(out)
        if request == ACCESS_PHY:
            if value != PHY_ID or index > 31 or length != 2:
                return None
            self.reads += 1
            return struct.pack('<H', self.phy_read(index))
        if request == ACCESS_EEPROM:
            if index != 1 or length != 2 or value not in EEPROM:
                return None
            self.reads += 1
            return struct.pack('<H', EEPROM[value])
        if request == FIFO_STATUS:
            if value != 0x8c or index or length != 4:
                return None
            self.reads += 1
            return struct.pack('<I', 0)            # never full: FIFO_FULL stays clear
        return None

    def write(self, request, value, index, data):
        if request == ACCESS_MAC:
            if not 1 <= index <= 8 or len(data) != index or value + len(data) > 256 or \
                    value <= R_LINK_STATUS < value + len(data):
                return False
            self.writes += 1
            was = self.powered()
            self.mac_regs[value:value + len(data)] = data
            if self.powered() and not was:
                self.restarts += 1                 # the PHY comes out of reset and negotiates
            return True
        if request == ACCESS_PHY:
            if value != PHY_ID or index > 31 or len(data) != 2:
                return False
            self.writes += 1
            self.phy_write(index, struct.unpack('<H', data)[0])
            return True
        return False


# The simulated network and helpers: as rtlemu.py (the same SIMULATED host), except
# that a request's IPv4 header and ICMP/UDP checksums are checked before it is answered.
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
        self.refused = 0

    def refuse(self, why, length):
        self.refused += 1
        self.log(why, length=length)

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
        # A real host drops a datagram whose header, length or checksums are
        # wrong; so does this one, so that a corrupted transmit is never
        # answered (the driver offloads no checksum: AxStart).
        if ihl < 20 or total < ihl or total > len(body) or checksum(body[:ihl]):
            self.refuse('ip_header_bad', len(frame))
            return []
        proto, sip, dip = body[9], body[12:16], body[16:20]
        data = body[ihl:total]
        if proto == 1 and checksum(data):
            self.refuse('icmp_checksum_bad', len(frame))
            return []
        if proto == 17:
            ulen = struct.unpack('!H', data[4:6])[0] if len(data) >= 8 else 0
            if not 8 <= ulen <= len(data):
                self.refuse('udp_length_bad', len(frame))
                return []
            data = data[:ulen]
            if struct.unpack('!H', data[6:8])[0] and \
                    checksum(sip + dip + struct.pack('!BBH', 0, 17, ulen) + data):
                self.refuse('udp_checksum_bad', len(frame))
                return []
        if proto == 1 and dip == HOST_IP and data[:1] == b'\x08':
            self.log('icmp_echo', source=sip.hex(), length=len(data))
            reply = b'\0\0' + b'\0\0' + data[4:]
            reply = reply[:2] + struct.pack('!H', checksum(reply)) + reply[4:]
            return [self.frame(src, 0x0800, ipv4(HOST_IP, sip, 1, reply))]
        if proto == 17 and struct.unpack('!H', data[2:4])[0] == 67:
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
            if i + 1 >= len(options) or i + 2 + options[i + 1] > len(options):
                self.refuse('dhcp_option_truncated', len(boot))
                return []
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


def usb_string(text):
    raw = text.encode('utf-16le')
    return bytes((len(raw) + 2, 3)) + raw


def device_descriptor(speed, serial):
    if speed == 'super':
        return (TESTDATA / 'device.bin').read_bytes()                      # the physical capture
    return struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x0210 if speed == 'high' else 0x0200, 0xff, 0xff, 0,
                       64, VENDOR, PRODUCT, RELEASE, 1, 2, 3 if serial else 0, 1)


def bulk_size(speed):
    return {'super': 1024, 'high': 512, 'full': 64}[speed]


def config_descriptor(speed, kind=2):
    if speed == 'super' and kind == 2:
        return (TESTDATA / 'config0.bin').read_bytes()                     # the physical capture
    bulk = bulk_size(speed)
    body = bytes((9, 4, 0, 0, 3, 0xff, 0xff, 0, 4))
    body += bytes((7, 5, EP_INT, 3)) + struct.pack('<HB', 8, 0x0b)
    body += bytes((7, 5, EP_IN, 2)) + struct.pack('<HB', bulk, 0)
    body += bytes((7, 5, EP_OUT, 2)) + struct.pack('<HB', bulk, 0)
    return bytes((9, kind)) + struct.pack('<HBBBBB', 9 + len(body), 1, 1, 0, 0xa0, 0x3e) + body


def bos_descriptor(speed):
    """The model's BOS (none was captured): the USB 2.0 extension, and at
    SuperSpeed the SuperSpeed capability. None at Full Speed."""
    if speed == 'full':
        return None
    caps = [bytes((7, 16, 2)) + struct.pack('<I', 0x02)]
    if speed == 'super':
        caps.append(bytes((10, 16, 3, 0)) + struct.pack('<HBBH', 0x000e, 1, 10, 0x07ff))
    body = b''.join(caps)
    return bytes((5, 15)) + struct.pack('<HB', 5 + len(body), len(caps)) + body


class Model:
    def __init__(self, connection, trace, speed='super', now=time.monotonic, link_after=1.0,
                 flap_after=None, flap_for=2.0, mbps=1000, fcs=False, serial='SIM179'):
        self.connection, self.trace, self.now = connection, trace, now
        self.speed = speed
        self.chip = Chip(mbps)
        self.fcs = fcs
        self.net = Network(self.log)
        self.device = device_descriptor(speed, serial is not None)
        self.config_desc = config_descriptor(speed)
        self.other = None if speed == 'super' else config_descriptor('full' if speed == 'high' else 'high', 7)
        self.bos = bos_descriptor(speed)
        self.strings = {0: b'\x04\x03\x09\x04', 1: usb_string('usb-wdm-free SIMULATED'),
                        2: usb_string('AX88179 Ethernet - SIMULATED'), 4: usb_string('SIMULATED')}
        if serial is not None:
            self.strings[3] = usb_string(serial)
        self.bulk_max = bulk_size(speed)
        self.link_after, self.flap_after, self.flap_for = link_after, flap_after, flap_for
        self.peer_caps = 0
        self.config = 0
        self.interrupt = False
        self.pending = collections.OrderedDict()   # bulk-IN request id -> length
        self.rx = collections.deque()
        self.link_at = self.flap_at = self.restore_at = None
        self.flapped = False
        self.restarts_seen = 0
        self.irq = collections.deque()
        self.totals = collections.Counter()

    # -- plumbing (as rtlemu.py) ---------------------------------------------
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
        packets[0] = packets[16] = 512 if self.speed == 'super' else 64
        if self.config:
            for slot, kind, interval, size in ((16 + (EP_IN & 15), 2, 0, self.bulk_max),
                                               (EP_OUT, 2, 0, self.bulk_max),
                                               (16 + (EP_INT & 15), 3, 0x0b, 8)):
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
        if up == self.chip.link:
            return
        self.chip.link = up
        self.log('link', up=up, why=why, mbps=self.chip.mbps if up else 0)
        self.irq.append(struct.pack('<II', INT_LINK if up else 0, 0))
        self.flush_irq()

    def flush_irq(self):
        while self.interrupt and self.irq:
            data = self.irq.popleft()
            self.send(103, data=struct.pack('<BBH', EP_INT, 0, len(data)) + data)
            self.totals['interrupts'] += 1

    def pump(self):
        now = self.now()
        if not self.chip.powered():
            self.link_at = None
            self.set_link(False, 'phy_off')
        elif self.chip.restarts != self.restarts_seen:
            self.restarts_seen = self.chip.restarts
            self.set_link(False, 'autoneg_restart')
            self.link_at = now + self.link_after
        if self.link_at is not None and now >= self.link_at and self.chip.powered():
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
            if self.chip.powered():
                self.set_link(True, 'cable_in')
        self.drain()

    # -- data ----------------------------------------------------------------
    def deliver(self, frame):
        if not (self.chip.link and self.chip.receiving() and self.chip.accepts(frame)):
            self.totals['rx_filtered'] += 1
            return
        if len(frame) < 60:
            frame += bytes(60 - len(frame))
        if len(self.rx) < 256:
            self.rx.append(frame)
        self.drain()

    def aggregate(self, length):
        """One bulk-IN transfer: records (2 padding bytes + frame [+ FCS],
        padded to 8), one header dword per record, the trailer dword."""
        records, headers = bytearray(), []
        while self.rx:
            frame = self.rx[0]
            body = b'\0\0' + frame + (struct.pack('<I', zlib.crc32(frame)) if self.fcs else b'')
            padded = (len(body) + 7) & ~7
            if len(records) + padded + 4 * (len(headers) + 1) + 4 > length:
                break
            self.rx.popleft()
            records += body + bytes(padded - len(body))
            headers.append(len(body) << 16)
            self.totals['rx_frames'] += 1
        if not headers:
            return b''
        return bytes(records) + struct.pack('<%dI' % len(headers), *headers) + \
            struct.pack('<I', len(headers) | len(records) << 16)

    def drain(self):
        while self.pending and self.rx:
            ident, length = next(iter(self.pending.items()))
            data = self.aggregate(length)
            if not data:
                if not self.totals['rx_request_too_small']:
                    self.log('rx_request_too_small', length=length)
                self.totals['rx_request_too_small'] += 1
                break
            del self.pending[ident]
            self.send(101, ident, self.bulk_header(EP_IN, 0, len(data)) + data)
            self.totals['rx_transfers'] += 1
            self.log('bulk_in', length=len(data), sha256=hashlib.sha256(data).hexdigest())

    def transmit(self, data):
        """One bulk-OUT transfer: the 8-byte header and one frame."""
        if len(data) < 8 + 14:
            raise ValueError('transmit transfer shorter than a header and a frame')
        length, flags = struct.unpack_from('<II', data)
        if length != len(data) - 8:
            raise ValueError('transmit header length %d, transfer %d' % (length, len(data)))
        want = TX_PAD if (length + 8) % self.bulk_max == 0 else 0
        if flags != want:
            raise ValueError('transmit flags %08x, expected %08x' % (flags, want))
        frame = data[8:]
        self.totals['tx_frames'] += 1
        self.log('tx_frame', length=len(frame), destination=frame[:6].hex(), ethertype=frame[12:14].hex())
        if self.chip.link:
            for reply in self.net.handle(frame):
                self.deliver(reply)
        return frame

    # -- control -------------------------------------------------------------
    def descriptor(self, kind, number):
        if kind == 3:
            return self.strings.get(number)
        if number:
            return None
        if kind == 6 and self.speed != 'super':
            return struct.pack('<BBHBBBBBB', 10, 6, 0x200, 0xff, 0xff, 0, 64, 1, 0)
        return {1: self.device, 2: self.config_desc, 7: self.other, 15: self.bos}.get(kind)

    def control(self, ident, packet):
        if len(packet) < 10:
            raise ValueError('short control header')
        ep, request, rt, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        payload = packet[10:]
        if len(payload) != (0 if rt & 128 else length):
            raise ValueError('control length mismatch')
        status, data = 4, b''
        if rt == 0x80 and request == 6:
            data = self.descriptor(value >> 8, value & 255)
            status, data = (0, data) if data is not None else (4, b'')
        elif request == 0 and rt in (0x80, 0x81, 0x82):
            status, data = 0, b'\0\0'
        elif request == 8 and rt == 0x80:
            status, data = 0, bytes((self.config,))
        elif request == 10 and rt == 0x81:
            status, data = 0, b'\0'
        elif request in (1, 3) and rt == 2 and value == 0:
            status = 0
        elif request in (1, 3) and rt == 0 and self.speed == 'super':
            status = 0                                  # U1/U2 enable features
        elif request in (48, 49) and rt == 0 and self.speed == 'super':
            status = 0                                  # SET_SEL, SET_ISOCH_DELAY
        elif rt == 0xc0 and self.config:
            got = self.chip.read(request, value, index, length, self.speed)
            status, data = (0, got) if got is not None else (4, b'')
        elif rt == 0x40 and self.config:
            status = 0 if self.chip.write(request, value, index, payload) else 4
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
                self.totals['vendor_refused'] += 1
                self.log('vendor_refused', request=request, request_type=rt, value=value, index=index,
                         length=length, configured=self.config)

    # -- usbredir messages (as rtlemu.py) ---------------------------------------
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
        data = struct.pack('<BBBBHH', SPEED_CODE[self.speed], 0xff, 0xff, 0, VENDOR, PRODUCT)
        if self.peer_caps & (1 << CAP_VERSION):
            data += struct.pack('<H', RELEASE)
        self.send(1, data=data)
        self.log('connected', model='SIMULATED AX88179', speed=self.speed, mbps=self.chip.mbps,
                 fcs=self.fcs, mac=MODEL_MAC.hex(), config_sha256=hashlib.sha256(self.config_desc).hexdigest())

    def run(self):
        self.send(0, data=b'usb-wdm-free AX88179 SIMULATED'.ljust(64, b'\0') + struct.pack('<I', CAPS))
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
    ap.add_argument('--speed', choices=('super', 'high', 'full'), default='super')
    ap.add_argument('--mbps', type=int, choices=(10, 100, 1000), default=1000, help='the Ethernet link speed')
    ap.add_argument('--link-after', type=float, default=1.0)
    ap.add_argument('--flap-after', type=float, help='seconds after link-up to take the cable out, once')
    ap.add_argument('--flap-for', type=float, default=2.0)
    ap.add_argument('--fcs', action='store_true', help='forward the FCS in every receive record')
    ap.add_argument('--serial', default='SIM179', help='the serial number string (iSerialNumber 3)')
    args = ap.parse_args()
    if not (0 < len(args.serial) <= 126 and args.serial.isascii()):
        ap.error('--serial needs 1 to 126 ASCII characters')
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'SIMULATED AX88179 listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            model = Model(connection, trace, args.speed, link_after=args.link_after,
                          flap_after=args.flap_after, flap_for=args.flap_for, mbps=args.mbps,
                          fcs=args.fcs, serial=args.serial)
            try:
                model.run()
            except (EOFError, ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
                model.log('disconnected', **model.totals)
            except Exception as error:
                model.log('fatal', error=repr(error), **model.totals)
                raise


if __name__ == '__main__':
    main()
