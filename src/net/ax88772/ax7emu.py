#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# ASIX AX8817X based USB 2.0 Ethernet Devices
# Copyright (C) 2003-2006 David Hollis <dhollis@davehollis.com>
# Copyright (C) 2005 Phil Chang <pchang23@sbcglobal.net>
# Copyright (C) 2006 James Painter <jamie.painter@iname.com>
# Copyright (c) 2002-2003 TiVo Inc.
# Copyright (C) 2026 Yeo Kheng Meng
"""SIMULATED ASIX AX88772B / AX88772C USB Fast Ethernet adapter over
localhost usbredir (test aid only).

No AX88772B or AX88772C is on this host, and QEMU has no model of one,
so this is the vehicle for ax88772's legs. It models what the driver's
chip core (ax7chip.c) reads and writes, not the chip's firmware, and it
is written from the same Linux files as the driver: a misreading the two
share passes here, so the real chips stay owed to the bench. Where each
fact comes from (Linux drivers/net/usb at v7.3, GPL-2.0-or-later):

- Identity (asix_devices.c products[]): 0B95:772B by default; --pid and
  --vid for the other AX88772B ids (0B95:7E2B, 17EF:7203, 2001:1A02).
  bcdDevice 0001 for --chip 772b and 0002 for --chip 772c (ASIX's own
  Windows 7 INF lists those two revisions of 0B95:772B). INFERRED, this
  model's choice: the rest of the descriptors (device class FF, one
  configuration, interface 0 class FF/FF/00 with interrupt IN 0x81 of 8
  bytes, bulk IN 0x82 and bulk OUT 0x03, 512 bytes at High Speed and 64
  at Full Speed), the strings and the serial number.
- The vendor requests and registers (asix.h, asix_common.c,
  asix_devices.c): SET_SW_MII 06, READ/WRITE_MII_REG 07/08 (wValue the
  PHY address, wIndex the register), STATMNGSTS 09 (the chip code 0x20
  in bits 4-6; bit 0 host enable, set here once software MII access was
  asked for), SET_HW_MII 0A, READ_EEPROM 0B (the MAC in words 4-6),
  READ/WRITE_RX_CTL 0F/10, WRITE_IPG0 12, READ/WRITE_NODE_ID 13/14,
  WRITE_MULTI_FILTER 16, READ_PHY_ID 19 (byte 1 the embedded PHY, 0x10),
  READ_MEDIUM_STATUS 1A, WRITE_MEDIUM_MODE 1B, WRITE_GPIOS 1F, SW_RESET
  20, SW_PHY_SELECT 22, QCTCTRL 2A. INFERRED: requests outside that set,
  and wrong lengths, STALL. Vendor requests are answered whether or not
  the device is configured (INFERRED): ASIX's XP driver 5.4.3.0 sets
  configuration 1, then 0, sends its bring-up on endpoint 0, and sets
  configuration 1 again (docs/testing/2026-11-07-ax88772-model.md; why a
  2026-11-06 run ended at configuration 0 is not settled).
  READ_IPG012 11 (asix.h; three bytes, IPG0, IPG1, IPG2 as WRITE_IPG0 last
  set them, Linux's defaults 15 0C 12 at power-up, INFERRED) is answered:
  ASIX's 7 driver 5.12.12.0 reads it right after writing them and stops
  when it STALLs. SW_PHY_STATUS 21 (asix.h, one byte) answers the last
  SW_PHY_SELECT value (INFERRED: asix.h gives the request, not its answer).
- What ASIX's own drivers (XP 5.4.3.0, 7 5.12.12.0) need beyond Linux,
  each INFERRED from their traffic against this model (no specification
  or open driver gives it; docs/testing/2026-11-07-ax88772-model.md):
  - Requests Linux does not name: writes 26 with no data and 24 with four
    bytes, and a four-byte read 2D, accepted with no modelled effect (2D
    reads zeros). Writes 2C and 2E (no data) carry in wValue bit sets that
    match the INFs' checksum-offload defaults (2C 0x0319, 2E 0x0307: the
    IPv4 header, TCP and UDP over IPv4 and IPv6); this model keeps them as
    the receive and transmit offload settings (RXCOE, TXCOE).
  - RX_CTL 0x0398: bits 8 and 9 are sr9800.h's RH1M and RH2M (receive
    header modes); sr9800 sets RH1M alone and reads the 4-byte header. With
    bit 9 set the header carries 2 more bytes, after the length word and
    before the frame; this model sends them as zeros (no receive checksum
    result). Without them ASIX's drivers take no frame.
  - QCTCTRL: ASIX's drivers write wValue 0x0000 and 0x008F (Linux 0x8000);
    with wValue bit 15 clear this model sends one record per bulk-IN
    transfer, never cut (ASIX's drivers lose a frame cut across transfers).
    Bit 15 set keeps Linux's bursts.
  - The PHY's BMCR reads, once autonegotiation is done, with its speed
    (bit 13) and duplex (bit 8) bits as the mode resolved: both drivers
    read only BMSR and BMCR after a link change and program the medium mode
    from them (0x0336 or 0x03B6 at 100 full; 0x0134 or 0x0104 without).
  - Transmit: the header's complement is checked on the 11 length bits
    (Linux complements all 16; ASIX's 7 driver only the 11) and bits 11-15
    of the length half are flags; several records may share one bulk-OUT
    transfer, each on an even offset. Unless flag bit 15 is set, this model
    inserts what TXCOE asks for: the IPv4 header checksum (bit 0) and the
    TCP (bit 1) or UDP (bit 2) checksum of a whole datagram. As sent:
    every bit-15 record had correct sums (XP: ARP, ICMP, UDP; 7: ARP, IPv6);
    bit-14 records a wrong TCP or UDP checksum (XP: TCP, its IPv4 header
    right; 7: UDP with the IPv4 header right, TCP with both wrong); 7's
    records with no flag (ICMP, IGMP) a wrong IPv4 header checksum. Not
    modelled: insertion for IPv6 (TXCOE bits 8 and 9). The trace counts
    each record's state as sent (tx_as_sent_*).
  - Not modelled: ASIX's 7 driver logs "Non-authentic ASIX product." about
    30 s after it starts against this model and stops (Code 43). What it
    checks is not known and is not imitated.
- The PHY (IEEE 802.3 clause 22; drivers/net/phy/ax88796b.c): id
  003B:1861 (AX88772B) or 003B:1881 (AX88772C); BMCR reset self-clears;
  autonegotiation completes --link-after seconds after a restart; the
  partner advertises 10/100 half and full with pause (--mbps, --half,
  --no-partner-pause change it). INFERRED: the timing. Not modelled:
  ax88796b.c's "won't reset unless reset bit toggles" (the driver writes
  BMCR 0 first, and this model resets on any write of the reset bit).
- The status report (interrupt IN): INFERRED, 8 bytes of asix_status's
  struct ax88172_int_data, byte 0 0xA1, byte 2 bit 0 the link.
- Receive (asix_rx_fixup_internal): frames as records, a 4-byte header
  (length, its complement in the high half) and the frame, padded to
  even. The records form one stream; each bulk-IN transfer carries at
  most the burst QCTCTRL set (2048 bytes for 0x8000) and at most what
  the host asked for, so records cross transfers, as Linux handles
  (ASIX's settings: above).
  --cut random cuts at random even offsets instead. INFERRED: turning
  receive off (RX_CTL without SO) empties the buffer. No FCS. Frames reach
  the guest only while the link is up, RX_CTL has SO and the medium RE,
  and RX_CTL's filter takes them (directed by NODE_ID, AB broadcast, AM
  with the hash, AMALL, PRO).
- Transmit (asix_tx_fixup): one record per bulk-OUT transfer, and the
  pad word 0xffff0000 after it exactly when header and frame end on a
  packet boundary. Anything else is a fatal error in the trace. The chip
  sees packets, and a transfer ends with a short packet (a zero-length one
  included): a host that sends a transfer one packet at a time (UHCI at
  Full Speed, as QEMU passes it) is joined up first, at most
  TX_JOIN_MAX bytes. A full last packet also ends it when the bytes so
  far end a record: Linux's form once its header's length is in (sr9800's
  records need no pad on a packet boundary), ASIX's form at a record's
  end (a whole High Speed transfer of 512 bytes with no ZLP). A joined
  Linux-form transfer longer than its header allows is refused at once.
  A part-joined transfer is dropped by a USB reset, a configuration or
  an accepted SET_INTERFACE.

--chip sr9800: the CoreChips SR9800 (0FE6:9800 unless --vid/--pid),
from Linux sr9800.c and sr9800.h (GPL version 2 only: facts, no code):
the MAC read with READ_NODE_ID, request 0x70 (LED mux) accepted,
request 0x2A sets the burst (0x8100 4 KiB, 0x8000 2 KiB), and the
transmit pad word expected exactly where sr_tx_fixup puts it (header and
frame a multiple of maxpacket - 1) and no terminator otherwise.
INFERRED: the PHY id (this model answers 0283:BC30; Linux needs only a
value other than 0 and FFFF) and bcdDevice 0001.

Behind the adapter sits the SIMULATED network of axemu.py (one host,
192.168.77.1: ARP, ICMP echo, DHCP offering 192.168.77.2), with TCP from
tcphost.py (echo 7, discard 9, a byte source 19, HTTP 80). No physical
USB device is accessed.
From Supereal's SR9800 1.1.7.0 driver run against this model (7 and XP,
docs/testing/2026-11-07-model-fixes-ref4.md): its first request is
SET_FEATURE(DEVICE_REMOTE_WAKEUP), taken as the configuration advertises
remote wakeup; WRITE_MONITOR_MODE 0x1D and READ_MONITOR_MODE 0x1C keep a
value and do nothing else; its records carry no flag bits and take TXCOE
(0x2E, written 0307) as above. Its c0 B8 0120 3000 poll has no name in
any source and is refused (the driver carries on).
"""
import argparse
import collections
import hashlib
import json
import random
import select
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'ax88179'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from axemu import (CAPS, CAP_VERSION, CAP_EP_MAX, CAP_ID64, CAP_BULK32, HOST_IP, HOST_MAC,  # noqa: E402
                   checksum, ether_crc, ipv4, ipv4_fragments, usb_string)
from tcphost import Host  # noqa: E402

SPEED_CODE = {'full': 1, 'high': 2}
EP_INT, EP_IN, EP_OUT = 0x81, 0x82, 0x03
SET_SW_MII, READ_MII, WRITE_MII, STATMNGSTS, SET_HW_MII = 0x06, 0x07, 0x08, 0x09, 0x0a
READ_EEPROM, READ_RX_CTL, WRITE_RX_CTL, WRITE_IPG0 = 0x0b, 0x0f, 0x10, 0x12
READ_IPG012, SW_PHY_STATUS = 0x11, 0x21
# Requests ASIX's own drivers send that Linux does not name (their traffic on XP SP3 and 7 against this
# model, docs/testing/2026-11-06-reference-drivers.md): accepted, with no modelled effect.
ASIX_WRITES_NO_DATA, ASIX_WRITE_24, ASIX_READ_2D = (0x26,), 0x24, 0x2d
# Checksum offload (INFERRED from ASIX's drivers' traffic, see the header): 2C and 2E carry, in wValue,
# bit sets that match the INFs' offload defaults (receive 0x0319, transmit 0x0307: IPv4 header, TCP and
# UDP over IPv4, TCP and UDP over IPv6), and 0x0009 / 0x0003 when the driver turns offload down.
WRITE_RXCOE, WRITE_TXCOE = 0x2c, 0x2e
READ_MONITOR_MODE, WRITE_MONITOR_MODE = 0x1c, 0x1d                # sr9800.h SR_CMD_WRITE_MONITOR_MODE, asix.h the same number
TXCOE_IP, TXCOE_TCP, TXCOE_UDP = 0x0001, 0x0002, 0x0004
TX_NO_COE = 0x8000                 # transmit header bit 15: the record's checksums are left as sent
TX_JOIN_MAX = 65536                # a bulk-OUT transfer joined from packets: at most this many bytes
READ_NODE_ID, WRITE_NODE_ID, WRITE_MULTI, READ_PHY_ID = 0x13, 0x14, 0x16, 0x19
READ_MEDIUM, WRITE_MEDIUM, WRITE_GPIOS, SW_RESET, SW_PHY_SELECT, QCTCTRL = 0x1a, 0x1b, 0x1f, 0x20, 0x22, 0x2a
RX_SO, RX_AM, RX_AB, RX_AMALL, RX_PRO = 0x0080, 0x0010, 0x0008, 0x0002, 0x0001
MEDIUM_RE = 0x0100
RX_EXT_HEADER = 0x0200             # INFERRED: RX_CTL bit 9, sr9800.h's RH2M (ASIX's drivers write 0x0398)
QCT_AGGREGATE = 0x8000             # INFERRED: QCTCTRL wValue bit 15 (Linux 0x8000; ASIX's 0x0000, 0x008F)
BMCR, BMSR, PHYID1, PHYID2, ADVERTISE, LPA = 0, 1, 2, 3, 4, 5
BMCR_RESET, BMCR_ANENABLE, BMCR_PDOWN, BMCR_ANRESTART = 0x8000, 0x1000, 0x0800, 0x0200
BMCR_SPEED100, BMCR_FULLDPLX = 0x2000, 0x0100
BMSR_LINK, BMSR_ANDONE = 0x0004, 0x0020
CHIPCODE_772B, HOST_EN = 0x20, 0x01
EMBD_PHY = 0x10
MODEL_MAC = bytes.fromhex('000ec6772b01')
BURST = {0x8000: 2048, 0x8100: 4096, 0x8200: 6144, 0x8300: 8192, 0x8400: 16384}
LEDS_REQ = 0x70
CHIPS = {'772b': (0x003b1861, 0x0001, 0x0b95, 0x772b, 'AX88772B'),
         '772c': (0x003b1881, 0x0002, 0x0b95, 0x772b, 'AX88772C'),
         'sr9800': (0x0283bc30, 0x0001, 0x0fe6, 0x9800, 'SR9800')}


class Chip:
    def __init__(self, phy_id, mbps=100, half=False, partner_pause=True):
        self.eeprom = [0] * 64
        for i in range(3):
            self.eeprom[4 + i] = MODEL_MAC[2 * i] | MODEL_MAC[2 * i + 1] << 8
        self.node_id = bytearray(MODEL_MAC)  # INFERRED: loaded from the EEPROM at power-up and by RSE
        self.rx_ctl = self.medium = 0
        self.monitor = 0                     # WRITE_MONITOR_MODE 0x1D; INFERRED: 0 at power-up
        self.multi = bytearray(8)
        self.sw_mii = False
        self.qct = None
        self.rxcoe = self.txcoe = 0                 # INFERRED power-up: no offload
        self.ipg = bytearray((0x15, 0x0c, 0x12))  # INFERRED power-up: Linux's AX88772_IPG0/1/2_DEFAULT
        self.phy_select = 0x01                       # INFERRED power-up: the embedded PHY selected
        self.phy = [0] * 32
        self.phy[BMCR] = BMCR_ANENABLE
        self.phy[BMSR] = 0x7809
        self.phy[PHYID1], self.phy[PHYID2] = phy_id >> 16, phy_id & 0xffff
        self.phy[ADVERTISE] = 0x01e1
        self.mbps, self.half, self.partner_pause = mbps, half, partner_pause
        self.link = False
        self.restarts = 0
        self.reads = self.writes = 0

    def lpa(self):
        if not self.link:
            return 0
        if self.mbps == 100:
            v = 0x0080 if self.half else 0x0180
        else:
            v = 0x0020 if self.half else 0x0060
        return v | 0x0001 | (0x0400 if self.partner_pause else 0) | 0x4000

    def rx_ext(self):
        """The receive header's 2-byte extension (INFERRED, see the header)."""
        return bool(self.rx_ctl & RX_EXT_HEADER)

    def aggregates(self):
        """Records packed into bursts that may end inside one (Linux's QCTCTRL
        0x8000 and the other BURST values), or one record per transfer when
        QCTCTRL's wValue has bit 15 clear (INFERRED, see the header)."""
        return self.qct is None or bool(self.qct[0] & QCT_AGGREGATE)

    def receiving(self):
        return self.link and bool(self.rx_ctl & RX_SO) and bool(self.medium & MEDIUM_RE)

    def accepts(self, frame):
        rx, dest = self.rx_ctl, frame[:6]
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
            return bool(self.multi[bit >> 3] >> (bit & 7) & 1)
        return dest == bytes(self.node_id)

    def phy_read(self, reg):
        if reg == BMSR:
            v = self.phy[BMSR] & ~(BMSR_LINK | BMSR_ANDONE)
            return v | (BMSR_LINK | BMSR_ANDONE if self.link else 0)
        if reg == LPA:
            return self.lpa() if self.phy[BMCR] & BMCR_ANENABLE else 0    # none with a forced speed
        if reg == BMCR and self.link and self.phy[BMCR] & BMCR_ANENABLE:
            # INFERRED (see the header): after autonegotiation BMCR's speed and duplex bits
            # read as the mode it resolved, which ASIX's drivers program the medium from.
            v = self.phy[BMCR] & ~(BMCR_SPEED100 | BMCR_FULLDPLX)
            return v | (BMCR_SPEED100 if self.mbps == 100 else 0) | (0 if self.half else BMCR_FULLDPLX)
        return self.phy[reg]

    def phy_write(self, reg, value):
        if reg == BMCR:
            if value & BMCR_RESET:
                self.phy[BMCR] = BMCR_ANENABLE           # the reset is done at once
                self.phy[ADVERTISE] = 0x01e1
                self.restarts += 1
                return
            if value & BMCR_ANRESTART or (value & BMCR_PDOWN) != (self.phy[BMCR] & BMCR_PDOWN):
                self.restarts += 1
            self.phy[BMCR] = value & ~BMCR_ANRESTART
            return
        if reg in (BMSR, PHYID1, PHYID2, LPA):
            return
        self.phy[reg] = value

    def powered(self):
        return not self.phy[BMCR] & BMCR_PDOWN

    def read(self, request, value, index, length):
        """bytes, or None for a STALL."""
        self.reads += 1
        if request == STATMNGSTS and length == 1:
            return bytes((CHIPCODE_772B | (HOST_EN if self.sw_mii else 0),))
        if request == READ_MONITOR_MODE and length == 1:
            return bytes((self.monitor & 255,))
        if request == READ_PHY_ID and length == 2:
            return bytes((0xe0, EMBD_PHY))
        if request == READ_EEPROM and length == 2 and value < 64:
            return struct.pack('<H', self.eeprom[value])
        if request == READ_NODE_ID and length == 6:
            return bytes(self.node_id)
        if request == READ_MII and length == 2 and self.sw_mii and value == EMBD_PHY and index < 32:
            return struct.pack('<H', self.phy_read(index))
        if request == READ_RX_CTL and length == 2:
            return struct.pack('<H', self.rx_ctl)
        if request == READ_MEDIUM and length == 2:
            return struct.pack('<H', self.medium)
        if request == READ_IPG012 and length == 3:     # ASIX's 7 driver reads it back (see the header)
            return bytes(self.ipg)
        if request == SW_PHY_STATUS and length == 1:
            return bytes((self.phy_select,))
        if request == ASIX_READ_2D and length == 4:      # INFERRED: see the header
            return bytes(4)
        return None

    def write(self, request, value, index, data):
        self.writes += 1
        if request in (SET_SW_MII, SET_HW_MII) and not data:
            self.sw_mii = request == SET_SW_MII
        elif request == WRITE_MII and len(data) == 2 and self.sw_mii and value == EMBD_PHY and index < 32:
            self.phy_write(index, struct.unpack('<H', data)[0])
        elif request == WRITE_RX_CTL and not data:
            self.rx_ctl = value
        elif request == WRITE_MEDIUM and not data:
            self.medium = value
        elif request == WRITE_GPIOS and not data:
            if value & 0x80:                            # RSE: reload the EEPROM's MAC
                self.node_id[:] = MODEL_MAC
        elif request == WRITE_IPG0 and not data:
            self.ipg[:] = bytes((value & 255, value >> 8, index & 255))
        elif request == SW_PHY_SELECT and not data:
            self.phy_select = value & 255
        elif request in (SW_RESET, LEDS_REQ) and not data:
            pass
        elif request == WRITE_RXCOE and not data:
            self.rxcoe = value
        elif request == WRITE_TXCOE and not data:
            self.txcoe = value
        elif request in ASIX_WRITES_NO_DATA and not data or request == ASIX_WRITE_24 and len(data) == 4:
            pass                                        # INFERRED: accepted, no effect (see the header)
        elif request == WRITE_MONITOR_MODE and not data:
            self.monitor = value                        # sr9800.h/asix.h 0x1D; wake-up only, no effect here
        elif request == QCTCTRL and not data:
            self.qct = (value, index)
        elif request == WRITE_NODE_ID and len(data) == 6:
            self.node_id[:] = data
        elif request == WRITE_MULTI and len(data) == 8:
            self.multi[:] = data
        else:
            return False
        return True


def checksum_state(frame):
    """For the trace: 'ip-ok'/'ip-bad' and, for UDP or TCP, '-l4-ok'/'-l4-bad'/'-l4-none'
    (UDP 0); 'other' for anything not a whole unfragmented IPv4 frame."""
    if len(frame) < 34 or frame[12:14] != b'\x08\x00' or frame[14] >> 4 != 4:
        return 'other'
    ihl = (frame[14] & 15) * 4
    total = struct.unpack_from('!H', frame, 16)[0]
    if ihl < 20 or ihl > total or 14 + total > len(frame):
        return 'other'
    state = 'ip-ok' if checksum(frame[14:14 + ihl]) == 0 else 'ip-bad'
    proto, l4, end = frame[23], 14 + ihl, 14 + total
    if struct.unpack_from('!H', frame, 20)[0] & 0x3fff or proto not in (6, 17):
        return state
    if proto == 17:
        if end < l4 + 8:
            return state
        udp_len = struct.unpack_from('!H', frame, l4 + 4)[0]
        if not 8 <= udp_len <= total - ihl:
            return state + '-l4-bad'
        if frame[l4 + 6:l4 + 8] == b'\0\0':
            return state + '-l4-none'
        end = l4 + udp_len
    pseudo = frame[26:34] + struct.pack('!BBH', 0, proto, end - l4)
    return state + ('-l4-ok' if checksum(pseudo + frame[l4:end]) == 0 else '-l4-bad')


def device_descriptor(speed, vid, pid, release, serial):
    return struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x0200, 0xff, 0xff, 0, 64,
                       vid, pid, release, 1, 2, 3 if serial else 0, 1)


def config_descriptor(speed, kind=2):
    bulk = 512 if speed == 'high' else 64
    body = bytes((9, 4, 0, 0, 3, 0xff, 0xff, 0, 0))
    body += bytes((7, 5, EP_INT, 3)) + struct.pack('<HB', 8, 0x0b)
    body += bytes((7, 5, EP_IN, 2)) + struct.pack('<HB', bulk, 0)
    body += bytes((7, 5, EP_OUT, 2)) + struct.pack('<HB', bulk, 0)
    return bytes((9, kind)) + struct.pack('<HBBBBB', 9 + len(body), 1, 1, 0, 0xa0, 0x32) + body


class Model:
    bridge = None                                  # (socket, peer) of --udp, or None

    def __init__(self, connection, trace, speed='high', now=time.monotonic, link_after=1.0,
                 flap_after=None, flap_for=2.0, mbps=100, half=False, partner_pause=True,
                 chip='772b', vid=None, pid=None, cut='burst', seed=1, serial='SIM772'):
        self.connection, self.trace, self.now = connection, trace, now
        self.speed = speed
        phy_id, self.release, default_vid, default_pid, self.model_name = CHIPS[chip]
        vid = default_vid if vid is None else vid
        pid = default_pid if pid is None else pid
        self.chip = Chip(phy_id, mbps, half, partner_pause)
        self.sr9800 = chip == 'sr9800'
        self.remote_wakeup = False
        self.net = Host(self.log, now)
        self.vid, self.pid = vid, pid
        self.chip_name = chip
        self.device = device_descriptor(speed, vid, pid, self.release, serial is not None)
        self.config_desc = config_descriptor(speed)
        self.other = config_descriptor('full' if speed == 'high' else 'high', 7)
        self.strings = {0: b'\x04\x03\x09\x04', 1: usb_string('usb-wdm-free SIMULATED'),
                        2: usb_string('%s Ethernet - SIMULATED' % self.model_name)}
        if serial is not None:
            self.strings[3] = usb_string(serial)
        self.bulk_max = 512 if speed == 'high' else 64
        self.link_after, self.flap_after, self.flap_for = link_after, flap_after, flap_for
        self.cut = cut
        self.random = random.Random(seed)
        self.peer_caps = 0
        self.config = 0
        self.interrupt = False
        self.pending = collections.OrderedDict()   # bulk-IN request id -> length
        self.stream = bytearray()                  # records not yet sent
        self.records = collections.deque()         # the bytes of each record still in the stream
        self.link_at = self.flap_at = self.restore_at = None
        self.flapped = False
        self.restarts_seen = 0
        self.irq = collections.deque()
        self.tx_buf = bytearray()                  # a bulk-OUT transfer still coming a packet at a time
        self.tx_pad_may_follow = False             # ASIX records taken on a full packet: a pad word may follow
        self.totals = collections.Counter()

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
        self.clear_rx()
        self.tx_buf, self.tx_pad_may_follow = bytearray(), False
        self.config, self.interrupt = 0, False

    # -- link ----------------------------------------------------------------
    def set_link(self, up, why):
        if up == self.chip.link:
            return
        self.chip.link = up
        self.log('link', up=up, why=why, mbps=self.chip.mbps if up else 0, half=self.chip.half)
        if not up:
            self.clear_rx()                        # the receive buffer is lost with the link
        self.irq.append(bytes((0xa1, 0, 1 if up else 0, 0, 0, 0, 0, 0)))
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
        for frame in self.net.poll():               # the simulated host's TCP retransmissions
            self.deliver(frame)
        self.drain()

    # -- data ----------------------------------------------------------------
    def deliver(self, frame):
        if not (self.chip.receiving() and self.chip.accepts(frame)):
            self.totals['rx_filtered'] += 1
            return
        if len(frame) < 60:
            frame += bytes(60 - len(frame))
        if len(self.stream) < 32768:
            n = len(frame)
            # the 2-byte extension when RX_CTL asks for it (INFERRED, see the header): zeros
            head = struct.pack('<I', ((n ^ 0xffff) << 16) + n) + (bytes(2) if self.chip.rx_ext() else b'')
            self.records.append(len(head) + n + (n & 1))
            self.stream += head + frame + (b'\0' if n & 1 else b'')
            self.totals['rx_frames'] += 1
        self.drain()

    def clear_rx(self):
        self.stream.clear()
        self.records.clear()

    def burst(self):
        if self.chip.qct is None:
            return 2048                             # INFERRED: before QCTCTRL is written
        return BURST.get(self.chip.qct[0], 2048)

    def drain(self):
        while self.pending and self.stream:
            ident, length = next(iter(self.pending.items()))
            if self.chip.aggregates():
                take = min(length, self.burst(), len(self.stream))
                if self.cut == 'random' and take > 2:
                    take = max(2, self.random.randrange(1, take // 2 + 1) * 2)
            else:
                # INFERRED (see the header): one record per transfer; a host asking for
                # less than the record gets it in pieces, as a stream would
                take = min(self.records[0], length)
            take &= ~1
            if not take:
                break
            data = bytes(self.stream[:take])
            del self.stream[:take]
            left = take
            while left:
                if self.records[0] <= left:
                    left -= self.records.popleft()
                else:
                    self.records[0] -= left
                    left = 0
            del self.pending[ident]
            self.send(101, ident, self.bulk_header(EP_IN, 0, len(data)) + data)
            self.totals['rx_transfers'] += 1
            if not self.bridge:
                self.log('bulk_in', length=len(data), sha256=hashlib.sha256(data).hexdigest())

    def linux_want(self, word):
        """The bytes a Linux-form transfer with this header has (record and
        pad word), or None when the header is not Linux's form."""
        length = word & 0x7ff
        if word >> 27 != 0x1f or word & 0xf800 or (word >> 16) & 0x7ff != length ^ 0x7ff:
            return None
        unit = self.bulk_max - 1 if self.sr9800 else self.bulk_max
        return 4 + length + (4 if (length + 4) % unit == 0 else 0)

    @staticmethod
    def records_whole(data):
        """ASIX's form (see transmit_records) on a packet boundary: True when
        the bytes end with a whole record (or Linux's pad word after one), as
        a chip walking the records would see it; False when one is cut off.
        A header that is not a record also says True: transmit refuses it."""
        offset = 0
        while offset < len(data):
            rest = len(data) - offset
            if rest == 4 and offset and data[offset:] == b'\x00\x00\xff\xff':
                return True
            if rest < 4:
                return False
            word = struct.unpack_from('<I', data, offset)[0]
            length = word & 0x7ff
            if word >> 27 or (word >> 16) & 0x7ff != length ^ 0x7ff:
                return True
            offset += 4 + length + (length & 1)
        return offset == len(data)

    def receive_out(self, data):
        """One bulk-OUT packet or transfer from the host (see the header):
        joined until a short packet ends it, then taken by transmit()."""
        if len(self.tx_buf) + len(data) > TX_JOIN_MAX:
            raise ValueError('bulk-OUT transfer longer than %d bytes' % TX_JOIN_MAX)
        self.tx_buf += data
        if data and len(data) % self.bulk_max == 0:
            want = self.linux_want(struct.unpack_from('<I', self.tx_buf)[0]) if len(self.tx_buf) >= 4 else None
            if want is None:
                if not self.records_whole(self.tx_buf):
                    self.totals['tx_joined_packets'] += 1
                    return
                # taken on its last packet: Linux's pad word may still follow on its own
                whole, self.tx_buf, self.tx_pad_may_follow = bytes(self.tx_buf), bytearray(), True
                self.transmit(whole)
                return
            elif len(self.tx_buf) < want:
                self.totals['tx_joined_packets'] += 1
                return
            elif len(self.tx_buf) > want:
                raise ValueError('transmit transfer %d bytes or more, header gives %d' % (len(self.tx_buf), want))
        whole, self.tx_buf = bytes(self.tx_buf), bytearray()
        follow, self.tx_pad_may_follow = self.tx_pad_may_follow, False
        if follow and whole == b'\x00\x00\xff\xff':
            self.totals['tx_padded'] += 1          # the pad word of the records taken just before
            return
        if whole:
            self.transmit(whole)

    def transmit(self, data):
        """One bulk-OUT transfer: one record, and the pad word when it ends on a packet boundary."""
        if len(data) < 4 + 14:
            raise ValueError('transmit transfer shorter than a header and a frame')
        word = struct.unpack_from('<I', data)[0]
        length = word & 0x7ff
        # Linux complements all 16 bits; ASIX's 7 driver only the 11 length bits, with bits 11-15
        # of the length half set on some frames (06A9:4156, 07B1:804E), all of which the chip takes
        # (see the header): the 11-bit field and its complement are what is checked.
        top, flags = word >> 27, word & 0xf800
        if (word >> 16) & 0x7ff != length ^ 0x7ff or not ((top == 0x1f and flags == 0) or top == 0):
            raise ValueError('transmit header %08x: the complement does not agree' % word)
        if top == 0:
            return self.transmit_records(data)
        unit = self.bulk_max - 1 if self.sr9800 else self.bulk_max
        pad = (length + 4) % unit == 0
        want = 4 + length + (4 if pad else 0)
        if len(data) != want:
            raise ValueError('transmit transfer %d bytes, expected %d (frame %d)' % (len(data), want, length))
        if pad and data[-4:] != b'\x00\x00\xff\xff':
            raise ValueError('transmit pad word %s' % data[-4:].hex())
        return self.transmit_frame(data[4:4 + length], pad)

    def transmit_records(self, data):
        """ASIX's 7 form (see the header): records of a 4-byte header (the
        11-bit length, its 11-bit complement, flags in bits 11-15) and the
        frame, back to back, several in one transfer; a last 4 bytes of
        0xffff0000 are taken as Linux's pad word."""
        offset, frame = 0, b''
        while offset < len(data):
            rest = len(data) - offset
            if rest == 4 and offset and data[offset:] == b'\x00\x00\xff\xff':
                self.totals['tx_padded'] += 1
                break
            if rest < 4 + 14:
                raise ValueError('transmit record at %d: %d bytes left' % (offset, rest))
            word = struct.unpack_from('<I', data, offset)[0]
            length = word & 0x7ff
            if word >> 27 or (word >> 16) & 0x7ff != length ^ 0x7ff or length < 14 or offset + 4 + length > len(data):
                raise ValueError('transmit record at %d: header %08x' % (offset, word))
            self.totals['tx_header_11bit_flags_%04x' % (word & 0xf800)] += 1
            self.totals['tx_records_asix_form'] += 1
            frame = data[offset + 4:offset + 4 + length]
            self.totals['tx_as_sent_%04x_%s' % (word & 0xf800, checksum_state(frame))] += 1
            if not word & TX_NO_COE:
                frame = self.fill_checksums(frame, self.chip.txcoe)
            frame = self.transmit_frame(frame, False)
            offset += 4 + length + (length & 1)          # records start on even offsets
        return frame

    def fill_checksums(self, frame, coe):
        """INFERRED offload (see the header): for an IPv4 frame, the header
        checksum when TXCOE bit 0 is set, and for a whole datagram the TCP
        (bit 1) or UDP (bit 2) checksum, computed here as the chip would insert
        them; anything else passes unchanged."""
        if len(frame) < 34 or frame[12:14] != b'\x08\x00':
            return frame
        f = bytearray(frame)
        ihl = (f[14] & 15) * 4
        total = struct.unpack_from('!H', f, 16)[0]
        if f[14] >> 4 != 4 or ihl < 20 or ihl > total or 14 + total > len(f):
            return frame                                 # not IPv4, or short: left as sent
        if coe & TXCOE_IP:
            f[24:26] = b'\0\0'
            f[24:26] = struct.pack('!H', checksum(bytes(f[14:14 + ihl])))
        proto, l4 = f[23], 14 + ihl
        at = {17: 6, 6: 16}.get(proto) if coe & {17: TXCOE_UDP, 6: TXCOE_TCP}.get(proto, 0) else None
        if struct.unpack_from('!H', f, 20)[0] & 0x3fff:
            at = None                                    # a fragment: no transport checksum
        end = 14 + total
        if proto == 17 and end >= l4 + 8:
            udp_len = struct.unpack_from('!H', f, l4 + 4)[0]
            end = l4 + udp_len if 8 <= udp_len <= total - ihl else 0    # UDP's own length
        if at is not None and end >= l4 + at + 2:
            seg = bytes(f[l4:end])
            seg = seg[:at] + b'\0\0' + seg[at + 2:]
            pseudo = bytes(f[26:34]) + struct.pack('!BBH', 0, proto, len(seg))
            value = checksum(pseudo + seg) or (0xffff if proto == 17 else 0)
            f[l4 + at:l4 + at + 2] = struct.pack('!H', value)
        if bytes(f) != frame:
            self.totals['tx_checksums_filled'] += 1
        return bytes(f)

    def transmit_frame(self, frame, pad):
        self.totals['tx_frames'] += 1
        if self.bridge:
            if self.chip.link and self.chip.medium & MEDIUM_RE:
                self.bridge[0].sendto(frame, self.bridge[1])
            return frame
        if pad:
            self.totals['tx_padded'] += 1
        sums = checksum_state(frame)
        self.totals['tx_sums_' + sums] += 1
        self.log('tx_frame', length=len(frame), destination=frame[:6].hex(), ethertype=frame[12:14].hex(),
                 padded=pad, checksums=sums)
        if self.chip.link and self.chip.medium & MEDIUM_RE:
            for reply in self.net.handle(frame):
                self.deliver(reply)
        return frame

    # -- control -------------------------------------------------------------
    def descriptor(self, kind, number):
        if kind == 3:
            return self.strings.get(number)
        if number:
            return None
        if kind == 6:
            return struct.pack('<BBHBBBBBB', 10, 6, 0x200, 0xff, 0xff, 0, 64, 1, 0)
        return {1: self.device, 2: self.config_desc, 7: self.other}.get(kind)

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
        elif request in (1, 3) and rt == 0 and value == 1:
            # CLEAR/SET_FEATURE(DEVICE_REMOTE_WAKEUP): the configuration
            # advertises remote wakeup (bmAttributes 0xA0), so the feature is
            # taken (USB 2.0 9.4.1, 9.4.9). Supereal's SR9800 driver sends it
            # first and stops on a STALL.
            status = 0
            self.remote_wakeup = request == 3
            self.log('remote_wakeup', enabled=self.remote_wakeup)
        elif rt == 0xc0:                            # in any state (see the header)
            got = self.chip.read(request, value, index, length)
            status, data = (0, got) if got is not None else (4, b'')
        elif rt == 0x40:
            medium = getattr(self.chip, 'medium', None)
            status = 0 if self.chip.write(request, value, index, payload) else 4
            phy = getattr(self.chip, 'phy', None)   # the AX88772's (sremu's chip has none)
            if phy is not None and self.chip.medium != medium:   # the medium the driver set (legs' logs)
                self.log('medium', value=self.chip.medium, bmcr=phy[BMCR], an_adv=phy[ADVERTISE])
            if not status and request == WRITE_RX_CTL and not value & RX_SO:
                self.clear_rx()                     # INFERRED: receive off empties the buffer
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

    # -- usbredir messages (as axemu.py) ---------------------------------------
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
            if ok and kind == 9:
                self.tx_buf, self.tx_pad_may_follow = bytearray(), False   # SET_INTERFACE(0, 0): dropped
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
                self.receive_out(data)
                self.drain()
            else:
                if len(packet) != size or ident in self.pending or len(self.pending) >= 256:
                    raise ValueError('invalid bulk IN request')
                self.pending[ident] = length
                self.totals['bulk_in_asked_%d' % length] += 1
                self.drain()
        else:
            raise ValueError(f'unsupported usbredir message {kind}')

    def connect(self):
        self.info()
        data = struct.pack('<BBBBHH', SPEED_CODE[self.speed], 0xff, 0xff, 0, self.vid, self.pid)
        if self.peer_caps & (1 << CAP_VERSION):
            data += struct.pack('<H', self.release)
        self.send(1, data=data)
        self.log('connected', model='SIMULATED ' + self.model_name, speed=self.speed,
                 mbps=self.chip.mbps, half=self.chip.half, cut=self.cut, vid='%04x' % self.vid,
                 pid='%04x' % self.pid, mac=MODEL_MAC.hex(),
                 config_sha256=hashlib.sha256(self.config_desc).hexdigest())

    def run(self):
        self.send(0, data=b'usb-wdm-free AX88772B SIMULATED'.ljust(64, b'\0') + struct.pack('<I', CAPS))
        buffer, hello = bytearray(), False
        next_report = self.now() + 5
        while True:
            self.pump()
            if self.now() >= next_report:
                next_report = self.now() + 5
                self.log('totals', **self.totals)
            watched = [self.connection] + ([self.bridge[0]] if self.bridge else [])
            readable, _, _ = select.select(watched, [], [], 0.02)
            if self.bridge and self.bridge[0] in readable:
                for _ in range(64):
                    try:
                        frame = self.bridge[0].recv(2048)
                    except (BlockingIOError, ConnectionResetError):
                        break
                    self.totals['bridge_in'] += 1
                    self.deliver(frame)
            if self.connection not in readable:
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


def open_bridge(spec):
    """--udp LOCAL:REMOTE: the bound, non-blocking socket and the peer."""
    local, remote = (int(x) for x in spec.split(':'))
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    udp.bind(('127.0.0.1', local))
    udp.setblocking(False)
    return udp, ('127.0.0.1', remote)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--log', type=Path, required=True, help='new JSONL trace (must not exist)')
    ap.add_argument('--speed', choices=('high', 'full'), default='high')
    ap.add_argument('--chip', choices=tuple(CHIPS), default='772b')
    ap.add_argument('--vid', type=lambda v: int(v, 16), help='hex; default the chip\'s')
    ap.add_argument('--pid', type=lambda v: int(v, 16), help='hex; default the chip\'s')
    ap.add_argument('--mbps', type=int, choices=(10, 100), default=100, help='the Ethernet link speed')
    ap.add_argument('--half', action='store_true', help='the partner offers half duplex only')
    ap.add_argument('--no-partner-pause', action='store_true')
    ap.add_argument('--cut', choices=('burst', 'random'), default='burst',
                    help='bulk-IN transfers end at the burst size (default) or at random even offsets')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--link-after', type=float, default=1.0)
    ap.add_argument('--flap-after', type=float, help='seconds after link-up to take the cable out, once')
    ap.add_argument('--flap-for', type=float, default=2.0)
    ap.add_argument('--serial', default='SIM772', help='the serial number string (iSerialNumber 3)')
    ap.add_argument('--udp', help='LOCAL:REMOTE UDP ports on 127.0.0.1: bridge frames there (QEMU dgram framing)')
    args = ap.parse_args()
    if not (0 < len(args.serial) <= 126 and args.serial.isascii()):
        ap.error('--serial needs 1 to 126 ASCII characters')
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'SIMULATED AX88772 listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            model = Model(connection, trace, args.speed, link_after=args.link_after,
                          flap_after=args.flap_after, flap_for=args.flap_for, mbps=args.mbps,
                          half=args.half, partner_pause=not args.no_partner_pause, chip=args.chip,
                          vid=args.vid, pid=args.pid, cut=args.cut, seed=args.seed, serial=args.serial)
            if args.udp:
                model.bridge = open_bridge(args.udp)
            try:
                model.run()
            except (EOFError, ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
                model.log('disconnected', **model.totals)
            except Exception as error:
                model.log('fatal', error=repr(error), **model.totals)
                raise


if __name__ == '__main__':
    main()
