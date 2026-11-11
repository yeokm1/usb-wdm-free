#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""SIMULATED CoreChips SR9700 USB Ethernet adapter over localhost usbredir
(test aid only).

No SR9700 is on this host, and QEMU has no model of one, so this is the
vehicle for sr9700's legs. It models what the driver's chip core
(srchip.c) reads and writes, not the chip's firmware, and is written from
the same Linux files as the driver (drivers/net/usb/sr9700.c and sr9700.h
at v7.3, GPL version 2 only: facts only, no code). A misreading the two
share passes here, so the real chip stays owed to the bench.

- Identity: 0FE6:9700 (sr9700.c products[]). INFERRED, this model's
  choice: the descriptors (Full Speed, bcdUSB 0110, device class FF, one
  configuration, interface 0 class FF/FF/00 with bulk IN 0x81 and bulk
  OUT 0x02 of 64 bytes and interrupt IN 0x83 of 8 bytes), the strings,
  the serial number, the power-up address 00:0e:c6:97:00:01.
- Requests (sr9700.h): 0x00 reads registers (wIndex the first), 0x01
  writes registers, 0x03 writes one (wValue the byte). Registers: network
  control 0x00 (bit 0 reset), network status 0x01 (bit 6 link), receive
  control 0x05 (bit 0 on, 1 promiscuous, 3 all multicast), the address
  0x10-0x15, PHY reset 0x1F (bit 0). INFERRED: other registers read as
  written; wrong requests STALL.
- Link: 10 Mb/s half duplex only (sr9700_get_link_ksettings); up
  --link-after seconds after power-on and after the PHY leaves reset. The status report:
  8 bytes, the first the network status (sr9700_status).
- Receive (sr9700_rx_fixup): packets of status 0x40, the length with
  the FCS (little endian), the frame and its FCS, back to back, as many
  whole packets as fit the host's request (Linux never splits one).
  At most 3072 bytes a transfer (INFERRED, see RX_TRANSFER_MAX).
  Only while the link is up and receive is on; the filter: directed by
  the address, broadcast, all multicast when bit 3, everything when bit 1.
- Transmit (sr9700_tx_fixup, usbnet): a 2-byte length, then the frame;
  when header and frame end on a packet boundary the length is one more
  and a zero byte follows. A transfer ends with a packet shorter than 64
  bytes, so one that comes a packet at a time (UHCI) is joined up first.
  A header that does not give the transfer's length, or a transfer that
  ends on a packet boundary, is a fatal error in the trace.
- From Linux 6.12's sr9700 (Debian 13) run against this model
  (docs/testing/2026-11-07-linux-reference.md): that version still reads
  PHY registers through EPCR/EPAR/EPDR (0x0B-0x0E), which v7.3 no longer
  does; this model has no PHY behind them, so such a read returns the
  last word written to EPDR (Linux 6.12 then reports 100 Mb/s full
  duplex from its own advertisement). Not modelled: no fact gives the
  chip's answer.

Behind the adapter sits the SIMULATED network of axemu.py (one host,
192.168.77.1: ARP, ICMP echo, DHCP offering 192.168.77.2).
"""
import argparse
import hashlib
import socket
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'ax88772'))
import ax7emu  # noqa: E402
from ax7emu import CAPS, CAP_VERSION, HOST_IP, HOST_MAC, checksum, ipv4, ipv4_fragments, usb_string  # noqa: E402,F401

EP_IN, EP_OUT, EP_INT = 0x81, 0x02, 0x83
READ, WRITE, WRITE1 = 0x00, 0x01, 0x03
REG_NETCTL, REG_NETSTAT, REG_RXCTL, REG_ADDR, REG_PHYRST = 0x00, 0x01, 0x05, 0x10, 0x1f
NETSTAT_LINK = 0x40
RX_ON, RX_PROMISC, RX_ALLMULTI = 0x01, 0x02, 0x08
MODEL_MAC = bytes.fromhex('000ec6970001')
# INFERRED: one receive transfer holds whole packets up to 3072 bytes, the
# receive URB Linux's sr9700_bind sets (rx_urb_size) and sr9700f uses; more
# never comes without a short packet. CoreChips' driver dropped a 6733-byte
# five-packet transfer and took a 2610-byte one.
RX_TRANSFER_MAX = 3072
VENDOR, PRODUCT, RELEASE = 0x0fe6, 0x9700, 0x0001
PRODUCT_9702 = 0x9702                               # the SR9700 with a driver disc (Linux sr9700.c products[])


class Chip:
    def __init__(self):
        self.regs = bytearray(256)
        self.regs[REG_ADDR:REG_ADDR + 6] = MODEL_MAC
        self.link = False
        self.mbps, self.half = 10, True
        # The PHY links at power-on: CoreChips' own driver never resets it
        # (it writes no 0x1F) and waits for the link on the interrupt
        # endpoint (docs/testing/2026-11-07-model-fixes-ref4.md). Linux's and
        # sr9700f's reset through 0x1F takes the link down and back up.
        self.restarts = 1
        self.phy_reset = False
        self.sw_mii = False

    @property
    def medium(self):                    # ax7emu.Model reads it to decide whether frames flow
        return 0x0100 if self.receiving() else 0

    def receiving(self):
        return self.link and bool(self.regs[REG_RXCTL] & RX_ON)

    def powered(self):
        return not self.phy_reset

    def accepts(self, frame):
        rx, dest = self.regs[REG_RXCTL], frame[:6]
        if rx & RX_PROMISC:
            return True
        if dest == b'\xff' * 6:
            return True
        if dest[0] & 1:
            return bool(rx & RX_ALLMULTI)
        return dest == bytes(self.regs[REG_ADDR:REG_ADDR + 6])

    def read(self, request, value, index, length):
        if request != READ or value or index + length > 256:
            return None
        out = bytearray(self.regs[index:index + length])
        if index <= REG_NETSTAT < index + length:
            out[REG_NETSTAT - index] = NETSTAT_LINK if self.link else 0
        return bytes(out)

    def write(self, request, value, index, data):
        if request == WRITE and not value and index + len(data) <= 256:
            self.regs[index:index + len(data)] = data
            return True
        if request == WRITE1 and not data and value < 256 and index < 256:
            if index == REG_NETCTL and value & 1:
                self.regs[REG_RXCTL] = 0                       # INFERRED: a reset turns receive off
                value &= ~1
            if index == REG_PHYRST:
                was = self.phy_reset
                self.phy_reset = bool(value & 1)
                if was and not self.phy_reset:
                    self.restarts += 1                  # the PHY leaves reset and links
            self.regs[index] = value
            return True
        return False


def config_descriptor(kind=2, composite=False):
    """The SR9700's configuration; composite: the 0FE6:9702's, whose
    interface 0 is the driver disc's, a mass-storage CD-ROM interface
    (class 08/06/50; INFERRED: modelled with no endpoints, so no storage
    driver can run it; Linux sr9700.c's products[] says only that the
    Ethernet is interface 1) and interface 1 the Ethernet."""
    eth = 1 if composite else 0
    body = bytes((9, 4, 0, 0, 0, 0x08, 0x06, 0x50, 0)) if composite else b''
    body += bytes((9, 4, eth, 0, 3, 0xff, 0xff, 0, 0))
    body += bytes((7, 5, EP_IN, 2)) + struct.pack('<HB', 64, 0)
    body += bytes((7, 5, EP_OUT, 2)) + struct.pack('<HB', 64, 0)
    body += bytes((7, 5, EP_INT, 3)) + struct.pack('<HB', 8, 1)
    return bytes((9, kind)) + struct.pack('<HBBBBB', 9 + len(body), 1 + eth, 1, 0, 0x80, 0x32) + body


class Model(ax7emu.Model):
    """ax7emu's usbredir plumbing and simulated network, with the SR9700's
    registers, descriptors and framing."""

    def __init__(self, connection, trace, now=None, link_after=1.0, flap_after=None, flap_for=2.0,
                 serial='SIM9700', composite=False):
        kw = {} if now is None else {'now': now}
        super().__init__(connection, trace, 'full', link_after=link_after, flap_after=flap_after,
                         flap_for=flap_for, serial=serial, **kw)
        self.chip = Chip()
        self.composite = composite
        self.eth_interface = 1 if composite else 0
        self.vid, self.pid, self.release = VENDOR, PRODUCT_9702 if composite else PRODUCT, RELEASE
        self.model_name = 'SR9700'
        # The composite device: class 0 at the device, so the host's composite parent splits it.
        self.device = struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x0110, 0 if composite else 0xff,
                                  0 if composite else 0xff, 0, 64,
                                  VENDOR, self.pid, RELEASE, 1, 2, 3 if serial else 0, 1)
        self.config_desc = config_descriptor(composite=composite)
        self.other = None
        self.strings[2] = usb_string('SR9700 Ethernet - SIMULATED')
        self.bulk_max = 64
        self.ep_in, self.ep_out, self.ep_int = EP_IN, EP_OUT, EP_INT
        self.tx_buf = bytearray()

    def descriptor(self, kind, number):
        if kind == 6:
            return None                                   # a Full Speed device: no qualifier
        return super().descriptor(kind, number)

    def info(self):
        types, intervals, interfaces = bytearray([255] * 32), bytearray(32), bytearray(32)
        packets = [0] * 32
        types[0] = types[16] = 0
        packets[0] = packets[16] = 64
        if self.config:
            for slot, kind, interval, size in ((16 + (EP_IN & 15), 2, 0, 64), (EP_OUT, 2, 0, 64),
                                               (16 + (EP_INT & 15), 3, 1, 8)):
                types[slot], intervals[slot], packets[slot] = kind, interval, size
                interfaces[slot] = self.eth_interface    # the endpoints' interface (the 9702: 1)
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << ax7emu.CAP_EP_MAX):
            data += struct.pack('<32H', *packets)
        self.send(5, data=data)
        numbers, classes, subclasses, protocols = (bytearray(32) for _ in range(4))
        classes[0], subclasses[0] = 0xff, 0xff
        count = 1
        if self.composite:
            classes[0], subclasses[0], protocols[0] = 0x08, 0x06, 0x50
            numbers[1], classes[1], subclasses[1] = 1, 0xff, 0xff
            count = 2
        self.send(4, data=struct.pack('<I', count if self.config else 0) + numbers + classes + subclasses + protocols)

    def set_link(self, up, why):
        if up == self.chip.link:
            return
        self.chip.link = up
        self.log('link', up=up, why=why, mbps=10 if up else 0, half=True)
        if not up:
            self.stream.clear()
        self.irq.append(bytes((NETSTAT_LINK if up else 0, 0, 0, 0, 0, 0, 0, 0)))
        self.flush_irq()

    def flush_irq(self):
        while self.interrupt and self.irq:
            data = self.irq.popleft()
            self.send(103, data=struct.pack('<BBH', EP_INT, 0, len(data)) + data)
            self.totals['interrupts'] += 1

    def deliver(self, frame):
        if not (self.chip.receiving() and self.chip.accepts(frame)):
            self.totals['rx_filtered'] += 1
            return
        if len(frame) < 60:
            frame += bytes(60 - len(frame))
        if len(self.rx_packets) >= 256:                # a full receive FIFO drops, as ax7emu's
            self.totals['rx_overflow'] += 1
            return
        packet = bytes((0x40,)) + struct.pack('<H', len(frame) + 4) + frame + struct.pack('<I', zlib.crc32(frame))
        self.rx_packets.append(packet)
        self.totals['rx_frames'] += 1
        self.drain()

    @property
    def rx_packets(self):
        if not hasattr(self, '_packets'):
            self._packets = []
        return self._packets

    def drain(self):
        while self.pending and self.rx_packets:
            ident, length = next(iter(self.pending.items()))
            data = b''
            while self.rx_packets and len(data) + len(self.rx_packets[0]) <= min(length, RX_TRANSFER_MAX):
                data += self.rx_packets.pop(0)
            if not data:
                if not self.totals['rx_request_too_small']:
                    self.log('rx_request_too_small', length=length)
                self.totals['rx_request_too_small'] += 1
                break
            del self.pending[ident]
            self.send(101, ident, self.bulk_header(EP_IN, 0, len(data)) + data)
            self.totals['rx_transfers'] += 1
            if not self.bridge:
                self.log('bulk_in', length=len(data), sha256=hashlib.sha256(data).hexdigest())

    def reset_device(self):
        super().reset_device()
        self.rx_packets.clear()
        self.tx_buf = bytearray()

    def receive_out(self, data):
        """One bulk-OUT packet or transfer from the host. The chip sees
        packets: a transfer ends with a packet shorter than 64 bytes (a
        zero-length one included), so a host that sends one packet at a
        time (UHCI) is joined up here before the frame is taken."""
        if len(self.tx_buf) + len(data) > 2 + 1514 + 1:
            raise ValueError('bulk-OUT transfer longer than a header, a frame and the pad byte')
        self.tx_buf += data
        if data and len(data) % self.bulk_max == 0:
            if len(self.tx_buf) >= 2:
                length = struct.unpack_from('<H', self.tx_buf)[0]
                if len(self.tx_buf) == length + 2:
                    raise ValueError('a transfer on a packet boundary without the length bump and pad byte')
                if len(self.tx_buf) > length + 2:
                    raise ValueError('transmit header %d, transfer %d or more' % (length, len(self.tx_buf)))
            return
        whole, self.tx_buf = bytes(self.tx_buf), bytearray()
        if whole:
            self.transmit(whole)

    def transmit(self, data):
        """One frame after its 2-byte length. A transfer that would end on a
        packet boundary must have been made one byte longer (the length
        counts that byte); the chip cannot tell that byte from the frame's
        own, and neither can this model."""
        if len(data) < 2 + 14:
            raise ValueError('transmit transfer shorter than a header and a frame')
        length = struct.unpack_from('<H', data)[0]
        if length != len(data) - 2:
            raise ValueError('transmit header %d, transfer %d' % (length, len(data)))
        if len(data) % self.bulk_max == 0:
            raise ValueError('a transfer on a packet boundary without the length bump and pad byte')
        padded = (len(data) - 1) % self.bulk_max == 0 and data[-1] == 0
        frame = data[2:]
        self.totals['tx_frames'] += 1
        if self.bridge:
            if self.chip.link:
                self.bridge[0].sendto(frame, self.bridge[1])
            return frame
        if padded:
            self.totals['tx_padded'] += 1
        self.log('tx_frame', length=len(frame), destination=frame[:6].hex(), ethertype=frame[12:14].hex(),
                 padded=padded)
        if self.chip.link:
            for reply in self.net.handle(frame):
                self.deliver(reply)
        return frame

    def handle(self, kind, ident, packet):
        if kind in (9, 10) and self.composite:
            # The 9702's two interfaces, each with alternate 0 only; selecting
            # the Ethernet's drops a transfer cut off, the CD-ROM's touches it not.
            if len(packet) != (2 if kind == 9 else 1):
                raise ValueError('alternate length')
            ok = self.config and packet[0] in (0, 1) and (kind == 10 or packet[1] == 0)
            if ok and kind == 9 and packet[0] == self.eth_interface:
                self.tx_buf = bytearray()
            self.send(11, ident, bytes((0 if ok else 4, packet[0], 0)))
            return
        if kind == 9 and self.config and packet == b'\0\0':
            self.tx_buf = bytearray()                 # SET_INTERFACE(0, 0): a transfer cut off is dropped
        if kind in (15, 16):
            if len(packet) != 1:
                raise ValueError('interrupt header')
            status = 0 if packet[0] == EP_INT and self.config else 4
            self.interrupt = kind == 15 and not status
            self.send(17, ident, bytes((status, packet[0])))
            self.flush_irq()
            return
        if kind == 101:
            size = 10 if self.len32 else 8
            endpoint = packet[0] if packet else 0
            if endpoint in (EP_IN, EP_OUT):
                # ax7emu checks its own endpoint numbers; map onto them.
                mapped = ax7emu.EP_IN if endpoint == EP_IN else ax7emu.EP_OUT
                packet = bytes((mapped,)) + packet[1:]
                self._real_ep = endpoint
            if len(packet) >= size and packet[0] == ax7emu.EP_OUT:
                data = packet[size:]
                length = struct.unpack('<H', packet[2:4])[0] | ((struct.unpack('<H', packet[8:10])[0] << 16) if self.len32 else 0)
                if len(data) != length:
                    raise ValueError('bulk OUT length mismatch')
                self.send(101, ident, self.bulk_header(EP_OUT, 0, length))
                self.receive_out(data)
                self.drain()
                return
        super().handle(kind, ident, packet)

    def bulk_header(self, endpoint, status, length):
        if endpoint == ax7emu.EP_IN:
            endpoint = EP_IN
        return super().bulk_header(endpoint, status, length)

    def connect(self):
        self.info()
        data = struct.pack('<BBBBHH', 1, 0 if self.composite else 0xff, 0 if self.composite else 0xff, 0,
                           VENDOR, self.pid)
        if self.peer_caps & (1 << CAP_VERSION):
            data += struct.pack('<H', RELEASE)
        self.send(1, data=data)
        self.log('connected', model='SIMULATED SR9700', speed='full', mac=MODEL_MAC.hex(),
                 config_sha256=hashlib.sha256(self.config_desc).hexdigest())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--log', type=Path, required=True, help='new JSONL trace (must not exist)')
    ap.add_argument('--link-after', type=float, default=1.0)
    ap.add_argument('--flap-after', type=float)
    ap.add_argument('--flap-for', type=float, default=2.0)
    ap.add_argument('--serial', default='SIM9700')
    ap.add_argument('--composite', action='store_true',
                    help='the 0FE6:9702: a CD-ROM interface 0 (modelled endpoint-less), the Ethernet on 1')
    ap.add_argument('--udp', help='LOCAL:REMOTE UDP ports on 127.0.0.1: bridge frames there (QEMU dgram framing)')
    args = ap.parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'SIMULATED SR9700 listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            model = Model(connection, trace, link_after=args.link_after, flap_after=args.flap_after,
                          flap_for=args.flap_for, serial=args.serial, composite=args.composite)
            if args.udp:
                model.bridge = ax7emu.open_bridge(args.udp)
            try:
                model.run()
            except (EOFError, ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
                model.log('disconnected', **model.totals)
            except Exception as error:
                model.log('fatal', error=repr(error), **model.totals)
                raise


if __name__ == '__main__':
    main()
