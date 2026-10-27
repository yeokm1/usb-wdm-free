#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""SIMULATED Realtek RTL815x USB Ethernet adapter over localhost usbredir
(test aid only).

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
change, receive framing (record header, frame length with FCS, alignment,
several frames per bulk-IN transfer) and transmit framing. Register
semantics follow rtltest.c's register-file model.

The newer chips (RTL8153D, RTL8156, RTL8156B, RTL8157, RTL8159) add, from
the facts in docs/testing/2026-10-27-rtl815x-sim-newchips.md: the PHY SRAM
windows (address and data pairs, the address stepping by 2 per data
write), the indirect "ADV" window of the 8157/8159 (BMU and IP spaces),
the PHY state (external init, power-down, LAN on), the PHY flash and
power-cut bits the bring-up polls, link speed and duplex from the PHY's
advertisement against a link partner (--partner), up to 2.5, 5 and 10
Gb/s, the 16-byte receive and transmit descriptors of the 8157/8159 and
the receive early size in each chip's units. No firmware patch is
modelled: the chip runs as with none loaded, so the 8159 links at 5 Gb/s
at most unless --phy-patched.

Behind the adapter sits a SIMULATED network of one host, 192.168.77.1
(02:00:5E:77:00:01): it answers ARP, ICMP echo and DHCP (offering
192.168.77.2/24, router 192.168.77.1), and logs every frame. Frames reach
the guest only while the receiver is on (MR_COMMAND CMD_RX_ON) and the receive
configuration (MR_RX_ACCEPT, MR_HASH) accepts them.

Chips (--chip): 8152 (id 4C10, 0BDA:8152, 100 Mb/s), 8153 (id 5C10,
0BDA:8153, 1000 Mb/s, default), 8153b (id 6010, 0BDA:8153), 8153d (id 7420,
0BDA:8053, 1000 Mb/s), 8156 (id 7030; 8156-7020 id 7020; 0BDA:8156,
2500 Mb/s), 8156b (id 7410; 8156b-7400 id 7400; 0BDA:8156, 2500 Mb/s),
8157 (id 1030, 0BDA:8157, 5000 Mb/s), 8159 (id 2020, 0BDA:815A,
10000 Mb/s). --speed super|high|full (super: the newer chips' default);
--partner the link partner's fastest speed in Mb/s (default 10000);
--link-after seconds after the PHY's autonegotiation restart (default
1.0); --flap-after/--flap-for take the cable out once; --ext-init and
--power-cut start the PHY in its external-init state or after a power cut;
--phy-patched lets the 8159 link above 5 Gb/s, as with its PHY patch.
No physical USB device is accessed.
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

# Register names and values: rtlchip.h, and for the newer chips the facts
# listed in docs/testing/2026-10-27-rtl815x-sim-newchips.md.
RTL_SPACE_MAC = 0x0100
RTL_LANES_ALL = 0xff
MR_NODE_ID, MR_RX_ACCEPT, MR_RX_MAX_LEN, MR_HASH = 0xc000, 0xc010, 0xc016, 0xcd00
MR_NODE_ID_SAVED, MR_GPHY_FLASH = 0xd000, 0xd3ae
MR_BOOT, MR_TX_CONFIG, MR_COMMAND, MR_OOB = 0xe004, 0xe610, 0xe813, 0xe84f
MR_CPLUS, MR_PHY_PAGE, MR_LINK = 0xe854, 0xe86c, 0xe908
UR_GPHY, UR_CTRL, UR_RX_EARLY_LEN, UR_TX_SIG_CHECK = 0xd284, 0xd406, 0xd42e, 0xd4ae
UR_ADV_ADDR, UR_ADV_DATA, UR_ADV_CMD, UR_MISC0 = 0xd5d6, 0xd5d8, 0xd5dc, 0xd81a
PR_MII, PR_STATE, PR_INIT_GATE = 0xa400, 0xa420, 0xa468
PR_SRAM_ADDR, PR_SRAM_DATA, PR_XG_CTRL, PR_XG_STAT = 0xa436, 0xa438, 0xa5d4, 0xa5d6
PR_PATCH_STATE, PR_PATCH_CMD, PR_SRAM2_ADDR, PR_SRAM2_DATA = 0xb800, 0xb820, 0xb87c, 0xb87e
MIICTL_RESET, MIICTL_SPEED100, MIICTL_AN_ENABLE = 0x8000, 0x2000, 0x1000
MIICTL_POWERDOWN, MIICTL_AN_RESTART, MIICTL_FULL, MIICTL_SPEED1000 = 0x0800, 0x0200, 0x0100, 0x0040
MIIST_LINK, MIIST_AN_DONE = 0x0004, 0x0020
ANADV_10H, ANADV_10F, ANADV_100H, ANADV_100F, ANADV_PAUSE = 0x0020, 0x0040, 0x0080, 0x0100, 0x0400
GIGCTL_1000_FULL, GIGST_PEER_1000_FULL = 0x0200, 0x0800
XGCTL_2500, XGCTL_5000, XGCTL_10000 = 0x0080, 0x0100, 0x1000
XGST_PEER_2500, XGST_PEER_5000, XGST_PEER_10000 = 0x0020, 0x0040, 0x0800
PATCH_ASK, PATCH_GRANTED = 0x0010, 0x0040
CMD_RESET, CMD_RX_ON, CMD_TX_ON = 0x10, 0x08, 0x04
OOB_LIST_READY, OOB_FIFOS_EMPTY, OOB_ACTIVE = 0x02, 0x30, 0x80
TXC_EMPTY, BOOT_LOADED = 0x0800, 0x0002
PHYST_EXT_INIT, PHYST_LAN_ON, PHYST_POWER_DOWN = 2, 3, 5
INIT_GATE_BITS = 0x000a                         # PR_INIT_GATE bits 3 and 1
GPHY_HAS_FLASH = 0x02                           # MR_GPHY_FLASH
GPHY_LOADED, GPHY_RESTORED = 0x04, 0x40         # UR_GPHY
PCUT_SEEN = 0x01                                # UR_MISC0
CTRL_RX_AGG_OFF, CTRL_RX_ZERO, CTRL_RX_DESC_LONG, CTRL_8159_B11 = 0x0010, 0x0080, 0x0400, 0x0800
TXSIG_CHECK = 0x02                              # UR_TX_SIG_CHECK
ADV_BUSY, ADV_WRITE, ADV_IP = 0x0001, 0x0002, 0x0004
IR_U2P3, U2P3_V2_ON = 0xc2c0, 1 << 29           # ADV IP space
BR_RX_ZLP, BMU_RX_ZLP = 0x2300, 0x08            # ADV BMU space
ACC_ALL_PHYS, ACC_MY_PHYS, ACC_MCAST, ACC_BCAST = 1, 2, 4, 8
LNK_1000, LNK_100, LNK_10, LNK_UP, LNK_FULL = 0x10, 0x08, 0x04, 0x02, 0x01
LNK_RX_FLOW, LNK_TX_FLOW = 0x20, 0x40
LNK_SPEED = {10: LNK_10, 100: LNK_100, 1000: LNK_1000, 2500: 0x0400, 5000: 0x1000, 10000: 0x4000}
IRQ_LINK = 0x04
RX_DESC_LEN, TX_DESC_LEN = 24, 8                # the RTL8152 to RTL8156B
WIDE_DESC_LEN, WIDE_ALIGN = 16, 16              # the RTL8157 and RTL8159, both ways
TX_FIRST_LAST, TX_SIGNATURE = 0xc0000000, 0x15 << 27
RMS_DEFAULT = 1522

# Chips. id: MR_TX_CONFIG's high word; pid: the USB product id; family: 'a'
# the RTL8152/8153/8153B, 'b' the RTL8156/8156B/8153D, 'c' the
# RTL8157/8159 (16-byte descriptors, ADV window); top: the fastest link in
# Mb/s; usb3: bcdUSB at SuperSpeed (0: a USB 2.0 chip); unit: bytes per count
# of UR_RX_EARLY_LEN (checked for families 'b' and 'c'); rxbuf: the bulk-IN
# buffer Linux uses at SuperSpeed (the early size is set against it).
Kind = collections.namedtuple('Kind', 'id pid family top usb3 unit rxbuf name')
CHIPS = {
    '8152': Kind(0x4c10, 0x8152, 'a', 100, 0, 0, 16384, 'RTL8152'),
    '8153': Kind(0x5c10, 0x8153, 'a', 1000, 0x0300, 4, 32768, 'RTL8153'),
    '8153b': Kind(0x6010, 0x8153, 'a', 1000, 0x0300, 8, 32768, 'RTL8153B'),
    '8153d': Kind(0x7420, 0x8053, 'b', 1000, 0x0300, 8, 49152, 'RTL8153D'),
    '8156': Kind(0x7030, 0x8156, 'b', 2500, 0x0300, 8, 49152, 'RTL8156'),
    '8156-7020': Kind(0x7020, 0x8156, 'b', 2500, 0x0300, 8, 49152, 'RTL8156'),
    '8156b': Kind(0x7410, 0x8156, 'b', 2500, 0x0300, 8, 49152, 'RTL8156B'),
    '8156b-7400': Kind(0x7400, 0x8156, 'b', 2500, 0x0300, 8, 49152, 'RTL8156B'),
    '8157': Kind(0x1030, 0x8157, 'c', 5000, 0x0320, 16, 32768, 'RTL8157'),
    '8159': Kind(0x2020, 0x815a, 'c', 10000, 0x0320, 16, 49152, 'RTL8159'),
}
NEWER = sorted(k for k, v in CHIPS.items() if v.family != 'a')
PARTNERS = (10, 100, 1000, 2500, 5000, 10000)
BACKUP_MAC = bytes.fromhex('00e04c770002')
HOST_MAC, HOST_IP = bytes.fromhex('02005e770001'), bytes((192, 168, 77, 1))
GUEST_IP = bytes((192, 168, 77, 2))
EP_IN, EP_OUT, EP_INT = 0x81, 0x02, 0x83
SPEED_CODE = {'full': 1, 'high': 2, 'super': 3}  # usbredir speeds


def default_speed(chip):
    return 'super' if CHIPS[chip].family != 'a' else 'high'


class Chip:
    """The register file (rtltest.c's MODEL in Python), with the newer chips'
    indirect windows, PHY state and link resolution."""

    def __init__(self, chip='8153', ext_init=False, power_cut=False, phy_patch=False):
        self.kind = k = CHIPS[chip]
        self.tcr0, self.pid = k.id, k.pid
        # The RTL8159's PHY needs its patch for links above 5 Gb/s; with
        # none loaded the model's PHY tops out at 5 Gb/s (phy_patch: as if
        # one had been loaded; no patch itself is modelled).
        self.top = 5000 if k.id == 0x2020 and not phy_patch else k.top
        self.rate_bit = LNK_100 if k.top == 100 else LNK_1000
        self.newer, self.wide = k.family != 'a', k.family == 'c'
        self.ext_init = ext_init
        self.pla, self.usb = bytearray(65536), bytearray(65536)
        self.phy = [0] * 32768
        self.sram, self.sram2 = {}, {}               # PHY SRAM, addressed in bytes
        self.sram_at = self.sram2_at = 0
        self.adv = {'bmu': {}, 'ip': {}}             # the 8157/8159's ADV window
        self.pla[MR_OOB] = OOB_LIST_READY | OOB_FIFOS_EMPTY
        self.pla[MR_TX_CONFIG + 1] = TXC_EMPTY >> 8
        self.pla[MR_BOOT] = BOOT_LOADED
        self.pla[0xe000 + 1] = 0x01                  # MR_POWER_STATE bit 8, polled after ALDPS off
        self.phy[PR_STATE // 2] = PHYST_LAN_ON
        self.pla[MR_NODE_ID_SAVED:MR_NODE_ID_SAVED + 6] = BACKUP_MAC
        self.pla[MR_NODE_ID:MR_NODE_ID + 6] = BACKUP_MAC if self.tcr0 != 0x4c00 else bytes.fromhex('00e04c770003')
        self.pla[MR_RX_ACCEPT] = ACC_ALL_PHYS | ACC_MY_PHYS | ACC_MCAST | ACC_BCAST
        self.usb[UR_CTRL] = CTRL_RX_ZERO | CTRL_RX_AGG_OFF
        self.pla[MR_CPLUS] = 0x40
        if self.newer:
            # Power-up values the facts do not give are chosen so that the
            # bring-up's clears and polls are visible (the record, "Limits").
            self.pla[MR_GPHY_FLASH] = GPHY_HAS_FLASH
            self.usb[UR_GPHY] = GPHY_LOADED | (GPHY_RESTORED if power_cut else 0)
            if power_cut:
                self.usb[UR_MISC0] |= PCUT_SEEN
            if self.wide:
                self.usb[UR_CTRL + 1] = (CTRL_RX_DESC_LONG | (CTRL_8159_B11 if k.id == 0x2020 else 0)) >> 8
                self.usb[UR_TX_SIG_CHECK] = TXSIG_CHECK
                self.adv['ip'][IR_U2P3] = U2P3_V2_ON
                self.adv['bmu'][BR_RX_ZLP] = BMU_RX_ZLP
            xg = (XGCTL_2500 if k.top >= 2500 else 0) | (XGCTL_5000 if k.top >= 5000 else 0) | \
                (XGCTL_10000 if k.top >= 10000 else 0)
            for reg, value in ((PR_MII, MIICTL_AN_ENABLE | MIICTL_SPEED1000), (PR_MII + 2, 0x7949),
                               (PR_MII + 8, ANADV_10H | ANADV_10F | ANADV_100H | ANADV_100F | 1),
                               (PR_MII + 18, GIGCTL_1000_FULL), (PR_XG_CTRL, xg)):
                self.phy[reg // 2] = value
            if ext_init:
                self.phy[PR_INIT_GATE // 2] = INIT_GATE_BITS
        self.autoneg = 0                             # restarts seen
        self.writes = self.reads = 0

    # -- the PHY window ------------------------------------------------------
    def phy_address(self, pla):
        base = self.pla[MR_PHY_PAGE] | self.pla[MR_PHY_PAGE + 1] << 8
        return (base | (pla & 0x0fff)) & 0xffff

    def phy_word(self, addr):
        return self.phy[addr // 2]

    def phy_state(self):
        if self.ext_init and self.phy[PR_INIT_GATE // 2] & INIT_GATE_BITS:
            return PHYST_EXT_INIT
        if self.phy[PR_MII // 2] & MIICTL_POWERDOWN:
            return PHYST_POWER_DOWN
        return PHYST_LAN_ON

    def phy_read(self, addr):
        if addr == PR_SRAM_DATA:
            return self.sram.get(self.sram_at, 0)
        if addr == PR_SRAM2_DATA:
            return self.sram2.get(self.sram2_at, 0)
        if addr == PR_STATE and self.newer:
            return (self.phy[addr // 2] & ~7) | self.phy_state()
        return self.phy[addr // 2]

    def phy_written(self, addr, value):
        if addr == PR_MII:                     # BMCR
            if value & MIICTL_AN_RESTART or \
                    (self.newer and (value & MIICTL_RESET or not value & MIICTL_AN_ENABLE)):
                self.autoneg += 1
            value &= ~MIICTL_RESET                     # the reset completes
        if addr == PR_PATCH_CMD:
            st = self.phy[PR_PATCH_STATE // 2]
            self.phy[PR_PATCH_STATE // 2] = st | PATCH_GRANTED if value & PATCH_ASK else st & ~PATCH_GRANTED
        if addr == PR_SRAM_ADDR:
            self.sram_at = value
        elif addr == PR_SRAM_DATA:
            self.sram[self.sram_at] = value
            self.sram_at = (self.sram_at + 2) & 0xffff
        elif addr == PR_SRAM2_ADDR:
            self.sram2_at = value
        elif addr == PR_SRAM2_DATA:
            self.sram2[self.sram2_at] = value
            self.sram2_at = (self.sram2_at + 2) & 0xffff
        self.phy[addr // 2] = value & 0xffff

    # -- vendor requests -------------------------------------------------------
    def read(self, value, index, length):
        if value & 3 or not length or length & 3 or value + length > 0x10000 or index & ~0x1ff or length > 64:
            return None
        self.reads += 1
        space = self.pla if index & RTL_SPACE_MAC else self.usb
        out = bytearray(length)
        for k in range(length):
            a = value + k
            if index & RTL_SPACE_MAC and 0xb000 <= a < 0xc000:
                w = self.phy_read(self.phy_address(a & ~1))
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
        if self.wide and not index & RTL_SPACE_MAC and value <= UR_ADV_CMD < value + length:
            self.adv_command()
        return True

    def adv_command(self):
        """The 8157/8159's indirect window: UR_ADV_CMD with ADV_BUSY moves one
        dword between UR_ADV_DATA and the BMU or IP space at UR_ADV_ADDR; the
        busy bit clears at once."""
        cmd = self.usb_word(UR_ADV_CMD)
        if not cmd & ADV_BUSY:
            return
        space = self.adv['ip' if cmd & ADV_IP else 'bmu']
        addr = self.usb_word(UR_ADV_ADDR)
        if cmd & ADV_WRITE:
            space[addr] = struct.unpack_from('<I', self.usb, UR_ADV_DATA)[0]
        else:
            struct.pack_into('<I', self.usb, UR_ADV_DATA, space.get(addr, 0))
        struct.pack_into('<H', self.usb, UR_ADV_CMD, cmd & ~ADV_BUSY)

    def dword(self, a):
        return struct.unpack_from('<I', self.pla, a)[0]

    def usb_word(self, a):
        return struct.unpack_from('<H', self.usb, a)[0]

    def pla_word(self, a):
        return struct.unpack_from('<H', self.pla, a)[0]

    @property
    def mac(self):
        return bytes(self.pla[MR_NODE_ID:MR_NODE_ID + 6])

    # -- link ----------------------------------------------------------------
    def negotiate(self, partner):
        """(Mb/s, full duplex) of the link with a partner whose fastest speed
        is partner, from the PHY's control and advertisement; None if none."""
        bmcr, top = self.phy_word(PR_MII), min(self.top, partner)
        if not bmcr & MIICTL_AN_ENABLE:
            mbps = 100 if bmcr & MIICTL_SPEED100 else 1000 if bmcr & MIICTL_SPEED1000 else 10
            full = bool(bmcr & MIICTL_FULL)
            if mbps > min(top, 1000) or (mbps == 1000 and not full):
                return None
            return mbps, full
        xg, gig, adv = self.phy_word(PR_XG_CTRL), self.phy_word(PR_MII + 18), self.phy_word(PR_MII + 8)
        for mbps, bit, full in ((10000, xg & XGCTL_10000, True), (5000, xg & XGCTL_5000, True),
                                (2500, xg & XGCTL_2500, True), (1000, gig & GIGCTL_1000_FULL, True),
                                (100, adv & ANADV_100F, True), (100, adv & ANADV_100H, False),
                                (10, adv & ANADV_10F, True), (10, adv & ANADV_10H, False)):
            if bit and mbps <= top:
                return mbps, full
        return None

    def show_link(self, got, partner):
        """MR_LINK and the PHY's status registers for a link (None: down)."""
        status = self.phy_word(PR_MII + 2) & ~(MIIST_LINK | MIIST_AN_DONE)
        if got is None:
            word, peer, peer_gig, peer_xg = 0, 0, 0, 0
        else:
            mbps, full = got
            autoneg = bool(self.phy_word(PR_MII) & MIICTL_AN_ENABLE)
            word = LNK_UP | LNK_SPEED[mbps] | (LNK_FULL if full else 0)
            if full and autoneg and self.phy_word(PR_MII + 8) & ANADV_PAUSE:
                word |= LNK_RX_FLOW | LNK_TX_FLOW        # the partner offers pause
            status |= MIIST_LINK | (MIIST_AN_DONE if autoneg else 0)
            peer = (ANADV_10H | ANADV_10F | ANADV_100H | ANADV_100F | ANADV_PAUSE | 1) if autoneg else 0
            peer_gig = GIGST_PEER_1000_FULL if autoneg and partner >= 1000 else 0
            peer_xg = (XGST_PEER_2500 if partner >= 2500 else 0) | (XGST_PEER_5000 if partner >= 5000 else 0) | \
                (XGST_PEER_10000 if partner >= 10000 else 0) if autoneg else 0
        struct.pack_into('<H', self.pla, MR_LINK, word)
        self.phy[(PR_MII + 2) // 2], self.phy[(PR_MII + 10) // 2] = status, peer
        self.phy[(PR_MII + 20) // 2], self.phy[PR_XG_STAT // 2] = peer_gig, peer_xg

    # -- receive ---------------------------------------------------------------
    def receiving(self):
        return bool(self.pla[MR_COMMAND] & CMD_RX_ON) and not self.pla[MR_OOB] & OOB_ACTIVE

    def refusal(self, frame):
        """Why the newer chips would not hand this frame to the host, or None."""
        if self.wide and self.usb_word(UR_CTRL) & CTRL_RX_DESC_LONG:
            return 'rx_desc_mode'                  # the driver left the 16-byte format off
        rms = self.pla_word(MR_RX_MAX_LEN)
        if self.newer and rms and len(frame) + 4 > rms:
            return 'rx_too_long'
        return None

    @property
    def rx_desc(self):
        return (WIDE_DESC_LEN, WIDE_ALIGN) if self.wide else (RX_DESC_LEN, 8)

    @property
    def tx_desc(self):
        return (WIDE_DESC_LEN, WIDE_ALIGN) if self.wide else (TX_DESC_LEN, 4)

    def early_bytes(self):
        """The receive early size the driver set, in bytes (newer chips)."""
        return self.usb_word(UR_RX_EARLY_LEN) * self.kind.unit if self.newer else 0

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


def device_descriptor(kind, speed):
    bcd = kind.usb3 if speed == 'super' else 0x0210 if speed == 'high' else 0x0200
    return struct.pack('<BBHBBBBHHHBBBB', 18, 1, bcd, 0, 0, 0, 9 if speed == 'super' else 64,
                       0x0bda, kind.pid, 0x3000, 1, 2, 0, 1)


def bulk_size(speed):
    return {'super': 1024, 'high': 512, 'full': 64}[speed]


def int_interval(speed):
    return 16 if speed == 'full' else 8


def config_descriptor(speed, kind=2):
    """The vendor configuration: one interface, class FF, bulk IN 1, bulk OUT
    2, interrupt IN 3 (what Linux checks); at SuperSpeed each endpoint has
    its companion (bMaxBurst 3 for bulk is this model's choice)."""
    bulk, ss = bulk_size(speed), speed == 'super'
    body = bytes((9, 4, 0, 0, 3, 0xff, 0xff, 0, 0))
    for ep, attr, size, interval, burst in ((EP_IN, 2, bulk, 0, 3), (EP_OUT, 2, bulk, 0, 3),
                                            (EP_INT, 3, 2, int_interval(speed), 0)):
        body += bytes((7, 5, ep, attr)) + struct.pack('<HB', size, interval)
        if ss:
            body += bytes((6, 0x30, burst, 0)) + struct.pack('<H', 2 if attr == 3 else 0)
    return bytes((9, kind)) + struct.pack('<HBBBBB', 9 + len(body), 1, 1, 0, 0xa0, 50) + body


def bos_descriptor(kind):
    """BOS at SuperSpeed: USB 2.0 extension (LPM), SuperSpeed capability,
    and for a bcdUSB 0x0320 chip a SuperSpeedPlus capability of one
    symmetric 10 Gb/s sublink (Gen 2x1)."""
    caps = [bytes((7, 16, 2)) + struct.pack('<I', 0x02),
            bytes((10, 16, 3, 0)) + struct.pack('<HBBH', 0x000e, 1, 10, 0x07ff)]
    if kind.usb3 >= 0x0320:
        caps.append(bytes((20, 16, 0x0a, 0)) + struct.pack('<IHH', 1, 0x1100, 0) +
                    struct.pack('<II', 0x000a4030, 0x000a40b0))
    body = b''.join(caps)
    return bytes((5, 15)) + struct.pack('<HB', 5 + len(body), len(caps)) + body


def usb_string(text):
    raw = text.encode('utf-16le')
    return bytes((len(raw) + 2, 3)) + raw


class Model:
    def __init__(self, connection, trace, chip='8153', speed='high', now=time.monotonic,
                 link_after=1.0, flap_after=None, flap_for=2.0, partner=10000, ext_init=False,
                 power_cut=False, phy_patch=False):
        self.connection, self.trace, self.now = connection, trace, now
        self.chip_name, self.speed = chip, speed or default_speed(chip)
        self.chip = Chip(chip, ext_init, power_cut, phy_patch)
        kind = self.chip.kind
        if self.speed == 'super' and not kind.usb3:
            raise ValueError('%s is a USB 2.0 chip' % kind.name)
        self.partner = partner
        self.net = Network(self.log)
        self.device = device_descriptor(kind, self.speed)
        self.config_desc = config_descriptor(self.speed)
        self.other = None if self.speed == 'super' else \
            config_descriptor('full' if self.speed == 'high' else 'high', 7)
        self.bos = bos_descriptor(kind) if self.speed == 'super' else None
        self.strings = {0: b'\x04\x03\x09\x04', 1: usb_string('usb-wdm-free SIMULATED'),
                        2: usb_string('%s Ethernet - SIMULATED' % kind.name)}
        self.bulk_max = bulk_size(self.speed)
        self.link_after, self.flap_after, self.flap_for = link_after, flap_after, flap_for
        self.peer_caps = 0
        self.config = 0
        self.interrupt = False
        self.pending = collections.OrderedDict()   # bulk-IN request id -> length
        self.rx = collections.deque()              # frames waiting for the guest
        self.link = False
        self.link_mbps, self.link_full = 0, False
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
        packets[0] = packets[16] = 512 if self.speed == 'super' else 64
        if self.config:
            for slot, kind, interval, size in ((16 + (EP_IN & 15), 2, 0, self.bulk_max),
                                               (EP_OUT, 2, 0, self.bulk_max),
                                               (16 + (EP_INT & 15), 3, int_interval(self.speed), 2)):
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
        if self.chip.newer:
            got = self.chip.negotiate(self.partner) if up else None
            if up and got is None:
                self.log('no_common_speed', why=why, partner=self.partner)
                self.totals['no_common_speed'] += 1
                return
            self.chip.show_link(got, self.partner)
            self.link_mbps, self.link_full = got if got else (0, False)
            if got and got[0] > 1000 and self.speed != 'super':
                self.totals['multigig_below_superspeed'] += 1
        else:
            self.chip.pla[MR_LINK] = (LNK_UP | self.chip.rate_bit | LNK_FULL) if up else 0
            self.link_mbps, self.link_full = (self.chip.kind.top, True) if up else (0, False)
        self.link = up
        self.log('link', up=up, why=why, mbps=self.link_mbps, full=self.link_full)
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
        why = self.chip.refusal(frame)
        if why:
            self.totals[why] += 1
            self.log('rx_refused', why=why, length=len(frame))
            return
        if len(self.rx) < 256:
            self.rx.append(frame)
        self.drain()

    def aggregate(self, length):
        """One bulk-IN transfer: record header + frame + FCS, each record
        aligned, as many as fit in length (one if UR_CTRL turns aggregation
        off). The RTL8157/8159's 16-byte header carries the length in bits
        17-31 of its first dword; the others' 24-byte header in bits 0-14."""
        out = bytearray()
        desc_len, align = self.chip.rx_desc
        single = self.chip.usb_word(UR_CTRL) & CTRL_RX_AGG_OFF
        while self.rx:
            frame = self.rx[0]
            record = desc_len + len(frame) + 4
            padded = (record + align - 1) & ~(align - 1)
            if len(out) + record > length:
                break
            self.rx.popleft()
            size = len(frame) + 4
            desc = struct.pack('<I', size << 17 if self.chip.wide else size) + bytes(desc_len - 4)
            out += desc + frame + struct.pack('<I', zlib.crc32(frame))
            out += b'\xee' * (padded - record)
            self.totals['rx_frames'] += 1
            if single:
                break
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

    def check_rx_request(self, length):
        """The newer chips end a bulk-IN transfer once the early size is
        passed, which may add one whole record: a request shorter than the
        early size plus one record can be overrun."""
        early = self.chip.early_bytes()
        if not early:
            return
        desc_len, align = self.chip.rx_desc
        need = early + (self.chip.pla_word(MR_RX_MAX_LEN) or RMS_DEFAULT) + desc_len + align
        if length < need:
            if not self.totals['rx_request_below_early_size']:
                self.log('rx_request_below_early_size', length=length, early=early, need=need)
            self.totals['rx_request_below_early_size'] += 1

    def transmit(self, data):
        """One bulk-OUT transfer: header + frame, each record aligned. The
        8-byte header has the length in bits 0-17 of its first dword; the
        RTL8157/8159's 16-byte one in bits 4-21 of its third, with the
        signature 0x15 in bits 27-31 of its fourth."""
        frames, off = [], 0
        hlen, align = self.chip.tx_desc
        while off + hlen <= len(data):
            opts1 = struct.unpack_from('<I', data, off)[0]
            length = opts1 & 0x3ffff
            if self.chip.wide:
                opts3, opts4 = struct.unpack_from('<II', data, off + 8)
                length = opts3 >> 4 & 0x3ffff
                if opts4 != TX_SIGNATURE:
                    raise ValueError('transmit header without the signature')
                if opts1 & 0x3ffff not in (0, length):
                    raise ValueError('transmit header lengths disagree')
            if not length or off + hlen + length > len(data):
                raise ValueError('transmit header length beyond the transfer')
            if opts1 & TX_FIRST_LAST != TX_FIRST_LAST:
                raise ValueError('transmit header without the first and last segment bits')
            frames.append(data[off + hlen:off + hlen + length])
            off = (off + hlen + length + align - 1) & ~(align - 1)
        if off < len(data) and any(data[off:]):
            raise ValueError('trailing bytes after the last frame')
        if not (self.chip.pla[MR_COMMAND] & CMD_TX_ON):
            self.totals['tx_while_disabled'] += 1
        if self.chip.wide and self.chip.usb[UR_TX_SIG_CHECK] & TXSIG_CHECK:
            self.totals['tx_sig_check_left_on'] += 1
        for frame in frames:
            self.totals['tx_frames'] += 1
            self.log('tx_frame', length=len(frame), destination=frame[:6].hex(),
                     ethertype=frame[12:14].hex() if len(frame) >= 14 else '')
            if self.link:
                for reply in self.net.handle(frame):
                    self.deliver(reply)
        return frames

    # -- control -------------------------------------------------------------
    def descriptor(self, kind, number):
        if kind == 3:
            return self.strings.get(number)
        if number:
            return None
        if kind == 6 and self.speed != 'super':
            return struct.pack('<BBHBBBBBB', 10, 6, 0x200, 0, 0, 0, 64, 1, 0)
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
                self.check_rx_request(length)
                self.pending[ident] = length
                self.drain()
        else:
            raise ValueError(f'unsupported usbredir message {kind}')

    def connect(self):
        self.info()
        data = struct.pack('<BBBBHH', SPEED_CODE[self.speed], 0, 0, 0, 0x0bda, self.chip.pid)
        if self.peer_caps & (1 << CAP_VERSION):
            data += struct.pack('<H', 0x3000)
        self.send(1, data=data)
        self.log('connected', model='SIMULATED %s' % self.chip.kind.name, chip=self.chip_name,
                 chip_id='%04x' % self.chip.tcr0, speed=self.speed, partner=self.partner,
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
    ap.add_argument('--speed', choices=('super', 'high', 'full'),
                    help='USB speed (default: super for the newer chips, high for the others)')
    ap.add_argument('--partner', type=int, choices=PARTNERS, default=10000,
                    help="the link partner's fastest speed in Mb/s (newer chips)")
    ap.add_argument('--link-after', type=float, default=1.0)
    ap.add_argument('--flap-after', type=float, help='seconds after link-up to take the cable out, once')
    ap.add_argument('--flap-for', type=float, default=2.0)
    ap.add_argument('--ext-init', action='store_true', help='the PHY starts in its external-init state')
    ap.add_argument('--power-cut', action='store_true', help='the chip reports a power cut at start')
    ap.add_argument('--phy-patched', action='store_true',
                    help='8159: links above 5 Gb/s as if its PHY patch were loaded (none is modelled)')
    args = ap.parse_args()
    speed = args.speed or default_speed(args.chip)
    if speed == 'super' and not CHIPS[args.chip].usb3:
        ap.error('%s is a USB 2.0 chip' % CHIPS[args.chip].name)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'SIMULATED {CHIPS[args.chip].name} ({args.chip}) listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            model = Model(connection, trace, args.chip, speed, link_after=args.link_after,
                          flap_after=args.flap_after, flap_for=args.flap_for, partner=args.partner,
                          ext_init=args.ext_init, power_cut=args.power_cut, phy_patch=args.phy_patched)
            try:
                model.run()
            except (EOFError, ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
                model.log('disconnected', **model.totals)
            except Exception as error:
                model.log('fatal', error=repr(error), **model.totals)
                raise


if __name__ == '__main__':
    main()
