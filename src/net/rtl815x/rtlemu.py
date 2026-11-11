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
4C00), link in MR_LINK with an interrupt-IN report on each change (its
IRQ_LINK bit is the link state: set up, clear down), receive framing (record header, frame length with FCS, alignment,
several frames per bulk-IN transfer) and transmit framing. Register
semantics follow rtltest.c's register-file model. A write into the PHY
window is one PHY write per word, whichever byte lanes carry it: Realtek's
XP driver 5.23 asks for the PHY patch with the low byte of PR_PATCH_CMD
alone (wIndex 0x0101) and, with no PATCH_GRANTED, reset the device in a
loop (docs/testing/2026-11-06-reference-drivers.md).

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

Chips (--chip): 8152 (id 4C10, 0BDA:8152, 100 Mb/s; 8152-4c00 and
8152-4800 the other versions), 8153 (id 5C10, 0BDA:8153, 1000 Mb/s, default;
8153-5c00, 8153-5c20, 8153-5c30), 8153b (id 6010; 8153b-6000), 8153c (id
6400), 8153d (id 7420, 0BDA:8053, 1000 Mb/s; 8153d-8153 under 0BDA:8153),
8156 (id 7030; 8156-7020 id 7020; 0BDA:8156, 2500 Mb/s), 8156b (id 7410;
8156b-7400 id 7400; 0BDA:8156, 2500 Mb/s), 8157 (id 1030, 0BDA:8157,
5000 Mb/s), 8159 (id 2020, 0BDA:815A, 10000 Mb/s). Each has the bcdDevice
Realtek's INFs choose its chip by (CHIPS). --vid, --pid and --bcd-device
(hex) put a profile behind another id (an OEM's id, a dock's).
The 8157 answers as a real RTL8157 did (REAL8157): its descriptors, its
registers' and PHY's first reads, the PHY OCP window at USB D630/D634 that
Realtek's Windows driver uses, the bits that clear themselves, and an
interrupt report every bInterval (--irq-on-change: only on a change).
--speed super|high|full (super: the newer chips' default);
--partner the link partner's fastest speed in Mb/s (default 10000);
--link-after seconds after the PHY's autonegotiation restart (default
1.0); --flap-after/--flap-for take the cable out once; --ext-init and
--power-cut start the PHY in its external-init state or after a power cut;
--phy-patched lets the 8159 link above 5 Gb/s, as with its PHY patch.
--serial gives the device a serial number string (index 3, 7 on the 8157 as
on a real one; 'mac' its form there; by default none) and --serial-stall N
STALLs the first N reads of it in this run,
after the first K answered with --serial-skip K (a re-enumeration's reads
failing once the first ones passed), so a host's instance id rules can be
exercised (xHCI98 task 45.40).
--udp LOCAL:REMOTE replaces the simulated host by a bridge: every frame the
guest sends goes as one UDP datagram to 127.0.0.1:REMOTE, every datagram
arriving on 127.0.0.1:LOCAL is a frame from the network (the framing of
QEMU's "-netdev dgram"/"socket,udp=" backend, so a QEMU user-mode network
behind a hub gives real TCP, for sustained-traffic legs); frames are then
not logged one by one.
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
UR_OCP_ADDR, UR_OCP_CMD, OCP_GO, OCP_WRITE = 0xd630, 0xd634, 0x0001, 0x0002   # the RTL8157's PHY OCP window
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
# Offloads of the RTL8157/8159 (the 16-byte descriptors), as Linux r8152.c
# names the bits (tx_desc, rx_desc_v2) and as a real RTL8157 under
# Realtek's Windows driver 10.68.20 showed them (usbmon, 2026-11-07):
# transmit dword 0 GTSENDV4 (bit 28) with the TCP header offset in bits
# 18-24 (Realtek's driver left it 0, Linux sets it), dword 1 UDP_CS (31),
# TCP_CS (30), IPV4_CS (29) and bits 17-27 the MSS of a large send or the
# TCP/UDP header offset; receive dword 2 IPF (26), UDPF (25), TCPF (24),
# IPV4 (14), UDP (11), TCP (10). The real chip set, for a good TCP frame,
# IPV4|TCP with UDPF; a UDP one IPV4|UDP with TCPF; ICMP IPV4 with TCPF and
# UDPF; ARP IPF|UDPF|TCPF. A failed checksum setting its own F bit, the
# IP ID stepping by one per segment of a large send, and FIN/PSH on its
# last segment only are inferred (Linux/Windows expectations), not seen.
TX_GTSENDV4, TX_UDP_CS, TX_TCP_CS, TX_IPV4_CS = 1 << 28, 1 << 31, 1 << 30, 1 << 29
RX2_IPF, RX2_UDPF, RX2_TCPF, RX2_IPV4, RX2_UDP, RX2_TCP = 1 << 26, 1 << 25, 1 << 24, 1 << 14, 1 << 11, 1 << 10
RMS_DEFAULT = 1522

# Chips. id: MR_TX_CONFIG's high word; pid: the USB product id; family: 'a'
# the RTL8152/8153/8153B, 'b' the RTL8156/8156B/8153D, 'c' the
# RTL8157/8159 (16-byte descriptors, ADV window); top: the fastest link in
# Mb/s; usb3: bcdUSB at SuperSpeed (0: a USB 2.0 chip); unit: bytes per count
# of UR_RX_EARLY_LEN (checked for families 'b' and 'c'); rxbuf: the bulk-IN
# buffer Linux uses at SuperSpeed (the early size is set against it); bcd: the
# device's bcdDevice. Realtek's Windows INFs pick the install section for the
# chip by its high byte (0x20 the RTL8152B; 0x30 the RTL8153, 8156, 8157 and
# 8159; 0x31 the RTL8153B and 8156B; 0x32 the RTL8153C, inferred from the
# order; 0x33 the RTL8153D): a wrong one (every chip here gave 0x3000 before)
# put Realtek's driver 10.68.20 on its RTL8156 section for an RTL8156B, and it
# failed with Code 10 (docs/testing/2026-11-07-realtek-diagnostic.md). Only
# the RTL8157's 0x3000 is read from a device; the others follow the INFs.
Kind = collections.namedtuple('Kind', 'id pid family top usb3 unit rxbuf name bcd')
CHIPS = {
    '8152': Kind(0x4c10, 0x8152, 'a', 100, 0, 0, 16384, 'RTL8152', 0x2000),
    # Linux's other RTL8152 versions (r8152.c RTL_VER_01, RTL_VER_07): the
    # 4C00 keeps its address in MR_NODE_ID, not MR_NODE_ID_SAVED (rtlchip.c
    # K_4C00); the 4800 runs as the 4C10.
    '8152-4c00': Kind(0x4c00, 0x8152, 'a', 100, 0, 0, 16384, 'RTL8152', 0x2000),
    '8152-4800': Kind(0x4800, 0x8152, 'a', 100, 0, 0, 16384, 'RTL8152', 0x2000),
    '8153': Kind(0x5c10, 0x8153, 'a', 1000, 0x0300, 4, 32768, 'RTL8153', 0x3000),
    # The other RTL8153 (A) versions (RTL_VER_03, 05, 06) and the first
    # RTL8153B (RTL_VER_08), so each of rtlchip.c's per-version paths has a
    # profile; the early-size unit is the 5C10's.
    '8153-5c00': Kind(0x5c00, 0x8153, 'a', 1000, 0x0300, 4, 32768, 'RTL8153', 0x3000),
    '8153-5c20': Kind(0x5c20, 0x8153, 'a', 1000, 0x0300, 4, 32768, 'RTL8153', 0x3000),
    '8153-5c30': Kind(0x5c30, 0x8153, 'a', 1000, 0x0300, 4, 32768, 'RTL8153', 0x3000),
    '8153b': Kind(0x6010, 0x8153, 'a', 1000, 0x0300, 8, 32768, 'RTL8153B', 0x3100),
    '8153b-6000': Kind(0x6000, 0x8153, 'a', 1000, 0x0300, 8, 32768, 'RTL8153B', 0x3100),
    # RTL8153C (Linux RTL_VER_14): the RTL8153B's framing; bcdDevice 0x32xx is
    # inferred from the INFs' order (they do not name the chip).
    '8153c': Kind(0x6400, 0x8153, 'a', 1000, 0x0300, 8, 32768, 'RTL8153C', 0x3200),
    '8153d': Kind(0x7420, 0x8053, 'b', 1000, 0x0300, 8, 49152, 'RTL8153D', 0x3300),
    # The RTL8153D under the RTL8153's product id, as Realtek's INFs also list
    # it (revision 0x33xx).
    '8153d-8153': Kind(0x7420, 0x8153, 'b', 1000, 0x0300, 8, 49152, 'RTL8153D', 0x3300),
    '8156': Kind(0x7030, 0x8156, 'b', 2500, 0x0300, 8, 49152, 'RTL8156', 0x3000),
    '8156-7020': Kind(0x7020, 0x8156, 'b', 2500, 0x0300, 8, 49152, 'RTL8156', 0x3000),
    '8156b': Kind(0x7410, 0x8156, 'b', 2500, 0x0300, 8, 49152, 'RTL8156B', 0x3100),
    '8156b-7400': Kind(0x7400, 0x8156, 'b', 2500, 0x0300, 8, 49152, 'RTL8156B', 0x3100),
    # 0BDA:8056: not in Linux's table; Realtek's own Windows 7 INF lists it in its
    # RTL8156B sections (the id only; owner, 2026-11-06). That the chip behind it
    # reports 7410 is this model's inference: no such device has been seen.
    '8156b-8056': Kind(0x7410, 0x8056, 'b', 2500, 0x0300, 8, 49152, 'RTL8156B', 0x3100),
    '8157': Kind(0x1030, 0x8157, 'c', 5000, 0x0320, 16, 32768, 'RTL8157', 0x3000),
    '8159': Kind(0x2020, 0x815a, 'c', 10000, 0x0320, 16, 49152, 'RTL8159', 0x3000),
}

# A real RTL8157 (chip id 1030, 0BDA:8157 rev 3000) read on development host A
# on 2026-11-07 under Realtek's Windows 10 driver 10.68.20 and its diagnostic
# (docs/testing/2026-11-07-realtek-diagnostic.md). Where the --chip 8157
# profile answers from these, it answers as that device did. The register
# values are the first read of each register before any write to it: the
# state the device was in after earlier drivers had run it, not a cold reset
# (no power cycle was possible), and they are kept only where they read the
# same under two of Realtek's driver runs. The address, the serial number and
# the counters are not taken from it. Bytes are as on the wire (little-endian).
REAL8157 = {
    # MR_TX_CONFIG's high word: the id 1030 under Linux's mask 0x7cf0, with
    # bits 0x0300 set as well.
    'tcr': 0x1330,
    # (bytes as on the wire, little-endian; '..' a byte that was not read
    # with its lane enabled, or read differently in the two runs: the
    # model's own value stays)
    'pla': {
        0xc010: '0e480004', 0xc0b4: '6d001e00', 0xc0c0: '....0000', 0xd284: '....0000', 0xd388: '00000000',
        0xd38c: '0000....', 0xd398: '01......', 0xd3ac: '....0400', 0xd3c8: '0000....', 0xdc48: '0400....',
        0xdc68: '....0000', 0xe000: 'ef3f0100', 0xe004: '92c0....', 0xe040: 'c3ff....', 0xe080: 'c05f0064',
        0xe0c4: '00001000', 0xe0c8: '000003c0', 0xe0cc: '3701f959', 0xe61c: '....0000', 0xe810: '....000c',
        0xe81c: '1100cf0c', 0xe820: '20000000', 0xe84c: '00000032', 0xe854: '0100....', 0xe858: '00000010'},
    'usb': {
        0xb854: '....0000', 0xb858: '0000....', 0xbd00: '0000....', 0xbdf8: '0000....', 0xc264: '....0000',
        0xcd04: '....0008', 0xcd54: '0000....', 0xcf04: '0000....', 0xcfcc: 'a94901..', 0xcfd4: '....0000',
        0xcfd8: '00..c800', 0xcfec: '....2440', 0xcffc: '....0800', 0xd268: '....0006', 0xd284: '0000....',
        0xd288: '....00e0', 0xd328: '....5f5a', 0xd334: '0030....', 0xd378: '0000....', 0xd3c8: '....0000',
        0xd404: '....4000', 0xd434: '....0000', 0xd4ac: '....0000', 0xd4e8: '0000....', 0xd620: '0000....',
        0xd808: '....8000', 0xd818: '....0000', 0xd81c: '00000000', 0xd848: '0006100c', 0xe434: '0000....'},
    # PHY OCP words (the low three bits of the state register 0xa420 and the
    # SRAM data ports are the model's own).
    'phy': {
        0xa420: 0x0020, 0xa400: 0x1040, 0xa408: 0x1de1, 0xa40c: 0x006f, 0xa412: 0x0200, 0xa430: 0xa19f, 0xa442: 0x0c3c,
        0xa4a2: 0x0101, 0xa5d0: 0x0006, 0xa5d4: 0x0181, 0xa5ea: 0x6298, 0xa654: 0x8bb0, 0xa6d4: 0x0003,
        0xac0e: 0x0140, 0xad94: 0x0032, 0xad9c: 0x00e0, 0xada0: 0x01a2, 0xade8: 0x1400, 0xae06: 0x7c00,
        0xb54c: 0x3719, 0xb648: 0x7254, 0xb800: 0x00e0},
    # PHY SRAM words (through 0xa436/0xa438 and 0xb87c/0xb87e) and the ADV
    # window's BMU and IP dwords, first reads as above.
    'sram': {0x8011: 0xd337, 0x8018: 0x7700, 0x8183: 0x5965, 0x8fa8: 0x2900},
    'sram2': {
        0x8036: 0x305a, 0x8078: 0x305a, 0x807c: 0x5057, 0x8082: 0x5d8a, 0x809d: 0x5057, 0x80ef: 0x0c32,
        0x810f: 0x0104, 0x8111: 0x0103, 0x8118: 0x0709, 0x811a: 0x0712, 0x811c: 0x0501, 0x811d: 0x0103,
        0x8122: 0x0c72, 0x8142: 0x0124, 0x8144: 0x010c, 0x814b: 0x110a, 0x814d: 0x1114, 0x814f: 0x0b01,
        0x8150: 0x011d, 0x862c: 0x1216, 0x8647: 0xe680, 0x864b: 0x9dcd, 0x89e9: 0x000b, 0x8ffd: 0x0102,
        0x8ffe: 0x0204, 0x8fff: 0x0400},
    'adv_bmu': {0x0054: 0x00000010, 0x2300: 0x00000005, 0x2360: 0x00000001, 0x2460: 0x00000001,
                0x2700: 0x00000005},
    'adv_ip': {0xc2c0: 0x01021103, 0xc700: 0x00c8080d, 0xc70c: 0x00807485},
    # BMU dwords whose bits clear at once when a write sets bit 0 (0x2350 and
    # 0x2450 read 0 after 3, 0x0050 after 1).
    'adv_self_clear': {0x0050: 0x1, 0x2350: 0x3, 0x2450: 0x3},
    # Descriptors at SuperSpeed: configuration attributes (bus powered, no
    # remote wakeup) and bMaxPower, the bulk bursts, the interrupt endpoint
    # (16-byte packets, bInterval 11), the BOS (USB 2.0 extension 0x06, LTM,
    # bFunctionalitySupport 2) and the serial number's string index.
    'attributes': 0x80, 'power': 0x44, 'burst': 7, 'int_size': 16, 'int_interval': 11,
    'usb2_ext': 0x06, 'ss_attributes': 0x02, 'ss_function': 2, 'serial_index': 7,
    # Bits that read back clear at once after a write sets them (bytes, by
    # space): Realtek's driver sets each and waits for it to clear (MAC
    # 0xdc6a bit 4, PLA_POL_GPIO_CTRL in Linux, it polled for 1 s and then
    # reset the device when the model kept it).
    'self_clear': (('pla', 0xdc6a, 0x10), ('pla', 0xe800, 0x01), ('usb', 0xd4e8, 0x04),
                   ('usb', 0xe434, 0x10), ('usb', 0xd437, 0x03)),
    # The interrupt endpoint answers every poll with two bytes, the link bit
    # (0x04) set or clear: these were the device's words.
    'irq_up': bytes((0xec, 0x12)), 'irq_down': bytes((0xea, 0x12)),
    'link_bits': 0x0080, 'link_high': 0x0009,
    # Its other two configurations at SuperSpeed, as xHCI98 read them on XP:
    # 2 CDC NCM, 3 CDC ECM (each with iMACAddress 3). The model describes them
    # but runs only the vendor configuration 1 (SET_CONFIGURATION 2 or 3
    # STALLs); string 3 is the model's address in hex, as that field asks.
    'configs': (
        '09026800020200a0440904000001020d0005052400100105240600010d240f031f503100ee0500800006241a00012b07058303'
        '10000a06300000080009040100000a00010009040101020a00010407058102000400063007000000070502020004000630'
        '07000000',
        '09026200020300a044090400000102060005052400100105240600010d240f031f503100ee050080000705830310000b0630'
        '0000080009040100000a00000009040101020a0000040705810200040006300700000007050202000400063007000000'),
}
REAL = {'8157': REAL8157}
# The bits the real RTL8157 cleared at once that Realtek's driver also sets
# and waits for on the RTL8156B (10.68.20 reset the device in a loop while
# MAC 0xdc6a bit 4 stayed set, and never took the link while USB 0xd4e8
# bit 2 did): inferred for the other newer chips from the RTL8157's.
NEWER_SELF_CLEAR = (('pla', 0xdc6a, 0x10), ('usb', 0xd4e8, 0x04))
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

    def __init__(self, chip='8153', ext_init=False, power_cut=False, phy_patch=False, pid=None):
        self.kind = k = CHIPS[chip]
        self.real = REAL.get(chip)
        self.tcr0, self.pid = k.id, pid if pid is not None else k.pid
        self.tcr_word = self.real['tcr'] if self.real else k.id
        # The PHY OCP window at USB D630 (address, data) and D634 (command):
        # Realtek's Windows driver reaches the RTL8157's PHY through it.
        self.ocp_window = k.family == 'c'
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
        if self.real:
            for space, table in ((self.pla, self.real['pla']), (self.usb, self.real['usb'])):
                for addr, raw in table.items():
                    for i in range(4):
                        if raw[2 * i:2 * i + 2] != '..':
                            space[addr + i] = int(raw[2 * i:2 * i + 2], 16)
            for addr, value in self.real['phy'].items():
                self.phy[addr // 2] = value
            # MR_LINK: bit 7 read set in every read of a real one, up or down
            struct.pack_into('<HH', self.pla, MR_LINK, self.real['link_bits'], self.real['link_high'])
            self.sram.update(self.real['sram'])
            self.sram2.update(self.real['sram2'])
            self.adv['bmu'].update(self.real['adv_bmu'])
            self.adv['ip'].update(self.real['adv_ip'])
            # The bits a driver must clear on these chips stay set at power-up,
            # as on the other profiles, so a driver that leaves one shows
            # (the device read them clear only after earlier drivers had run).
            # Not the 16-byte receive descriptor bit: Realtek's driver never
            # writes it and the device worked, so this profile keeps it clear
            # as read.
            self.usb[UR_TX_SIG_CHECK] |= TXSIG_CHECK
            self.adv['ip'][IR_U2P3] |= U2P3_V2_ON
            self.adv['bmu'][BR_RX_ZLP] |= BMU_RX_ZLP
            if ext_init:
                self.phy[PR_INIT_GATE // 2] = INIT_GATE_BITS
            if power_cut:
                self.usb[UR_GPHY] |= GPHY_RESTORED
                self.usb[UR_MISC0] |= PCUT_SEEN
        self.autoneg = 0                             # restarts seen
        self.writes = self.reads = self.ocp_ops = 0

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
            # A restart, a reset, or a change of the forced speed and duplex
            # (autonegotiation off: the Advanced tab's *SpeedDuplex 1 to 4)
            # takes the link down and brings it up as the PHY now says.
            forced = MIICTL_AN_ENABLE | MIICTL_SPEED100 | MIICTL_FULL | MIICTL_SPEED1000
            if value & MIICTL_AN_RESTART or \
                    (not value & MIICTL_AN_ENABLE and (value & MIICTL_RESET or
                                                      (value ^ self.phy[addr // 2]) & forced)) or \
                    (self.newer and (value & MIICTL_RESET or not value & MIICTL_AN_ENABLE or
                                     (value ^ self.phy[addr // 2]) & MIICTL_POWERDOWN)):
                self.autoneg += 1
            # The reset and the autonegotiation restart both clear themselves
            # (IEEE 802.3 22.2.4.1); a real RTL8157 read BMCR back without the
            # restart bit, and Realtek's driver waited for that.
            value &= ~(MIICTL_RESET | MIICTL_AN_RESTART)
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
            out[2], out[3] = self.tcr_word & 255, self.tcr_word >> 8
        return bytes(out)

    def write(self, value, index, data):
        length, byen = len(data), index & 0xff
        if value & 3 or not length or length & 3 or value + length > 0x10000 or index & ~0x1ff or \
                length > 512 or (byen != RTL_LANES_ALL and (length != 4 or not byen)):
            return False
        self.writes += 1
        space = self.pla if index & RTL_SPACE_MAC else self.usb
        words = {}                                   # PHY word -> its new value, in write order
        for k in range(length):
            a = value + k
            if byen != RTL_LANES_ALL and not byen & (1 << (k & 3)):
                continue
            if index & RTL_SPACE_MAC and 0xb000 <= a < 0xc000:
                pa = self.phy_address(a & ~1)
                w = words.get(pa, self.phy[pa // 2])
                words[pa] = (w & 0x00ff) | data[k] << 8 if a & 1 else (w & 0xff00) | data[k]
                continue
            space[a] = data[k]
        # One PHY write per word touched, whichever of its bytes the lanes carry:
        # Linux writes whole words; Realtek's XP driver also writes a low byte
        # alone (PATCH_ASK into PR_PATCH_CMD with wIndex 0x0101).
        for pa, w in words.items():
            self.phy_written(pa, w)
        self.pla[MR_COMMAND] &= ~CMD_RESET & 0xff
        clears = self.real['self_clear'] if self.real else NEWER_SELF_CLEAR if self.newer else ()
        if clears:
            for name, addr, mask in clears:
                if (name == 'pla') == bool(index & RTL_SPACE_MAC) and value <= addr < value + length:
                    space[addr] &= ~mask & 0xff
        if self.wide and not index & RTL_SPACE_MAC and value <= UR_ADV_CMD < value + length:
            self.adv_command()
        if self.ocp_window and not index & RTL_SPACE_MAC and value <= UR_OCP_CMD < value + length:
            self.ocp_command()
        return True

    def ocp_command(self):
        """The PHY OCP window, as a real RTL8157 answered Realtek's driver:
        D630 holds the PHY address (low word) and the data (high word); a
        command word at D634 with bit 0 starts it, bit 1 a write. It is done
        at once: D634 reads 0 again, a read's data is in D632, and D630's low
        word then reads 0x0001, as it did on the device."""
        cmd = self.usb_word(UR_OCP_CMD)
        if not cmd & OCP_GO:
            return
        addr, data = struct.unpack_from('<HH', self.usb, UR_OCP_ADDR)
        if cmd & OCP_WRITE:
            self.phy_written(addr & 0xfffe, data)
        else:
            struct.pack_into('<HH', self.usb, UR_OCP_ADDR, 0x0001, self.phy_read(addr & 0xfffe))
        struct.pack_into('<H', self.usb, UR_OCP_CMD, 0)
        self.ocp_ops += 1

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
            clear = self.real['adv_self_clear'].get(addr, 0) if self.real and not cmd & ADV_IP else 0
            if space[addr] & 1 and clear:
                space[addr] &= ~clear
        else:
            struct.pack_into('<I', self.usb, UR_ADV_DATA, space.get(addr, 0))
        # The real RTL8157 reads the whole command word back as 0.
        struct.pack_into('<H', self.usb, UR_ADV_CMD, 0 if self.real else cmd & ~ADV_BUSY)

    def dword(self, a):
        return struct.unpack_from('<I', self.pla, a)[0]

    def usb_word(self, a):
        return struct.unpack_from('<H', self.usb, a)[0]

    def pla_word(self, a):
        return struct.unpack_from('<H', self.pla, a)[0]

    @property
    def mac(self):
        return bytes(self.pla[MR_NODE_ID:MR_NODE_ID + 6])

    def settings(self):
        """What the Advanced tab's keywords set (ethernet.md 4.10), as the
        registers hold it at link-up, for the legs' logs."""
        return dict(bmcr=self.phy_word(PR_MII), an_adv=self.phy_word(PR_MII + 8),
                    gig_ctl=self.phy_word(PR_MII + 18), xg_ctl=self.phy_word(PR_XG_CTRL),
                    eee_mac=self.pla_word(0xe040) & 3, eee_adv=self.phy_word(0xa5d0),
                    eee_adv2=self.phy_word(0xa6d4), eee_cfg=self.phy_word(0xa432),
                    eee_a1=self.phy_word(0x2080), eee_plus=self.pla_word(0xe080) & 2,
                    u2p3=self.usb_word(0xb460) & 1, green=self.sram.get(0x8011, 0),
                    bias_10=self.sram.get(0x8045, 0))

    # -- link ----------------------------------------------------------------
    def negotiate(self, partner):
        """(Mb/s, full duplex) of the link with a partner whose fastest speed
        is partner, from the PHY's control and advertisement; None if none."""
        bmcr, top = self.phy_word(PR_MII), min(self.top, partner)
        if self.phy_state() != PHYST_LAN_ON:
            return None                                # powered down or still in external init
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
            peer = (ANADV_10H | ANADV_10F | (ANADV_100H | ANADV_100F if partner >= 100 else 0) |
                    ANADV_PAUSE | 1) if autoneg else 0
            peer_gig = GIGST_PEER_1000_FULL if autoneg and partner >= 1000 else 0
            peer_xg = (XGST_PEER_2500 if partner >= 2500 else 0) | (XGST_PEER_5000 if partner >= 5000 else 0) | \
                (XGST_PEER_10000 if partner >= 10000 else 0) if autoneg else 0
        if self.real:
            # The real RTL8157 read MR_LINK as 0x10e3 (5 Gb/s, flow control,
            # up, full; bit 7 too) with 0x0009 above it, and on a link down
            # only the up bit went (0x10e1).
            word = (word | self.real['link_bits']) if got else self.pla_word(MR_LINK) & ~LNK_UP
            struct.pack_into('<H', self.pla, MR_LINK + 2, self.real['link_high'])
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


def device_descriptor(kind, speed, serial=0, vid=0x0bda, pid=None, bcd_device=None, configs=1):
    """serial: the serial number's string index (0: none)."""
    bcd = kind.usb3 if speed == 'super' else 0x0210 if speed == 'high' else 0x0200
    return struct.pack('<BBHBBBBHHHBBBB', 18, 1, bcd, 0, 0, 0, 9 if speed == 'super' else 64,
                       vid, kind.pid if pid is None else pid, kind.bcd if bcd_device is None else bcd_device,
                       1, 2, serial, configs)


def bulk_size(speed):
    return {'super': 1024, 'high': 512, 'full': 64}[speed]


def int_interval(speed):
    return 16 if speed == 'full' else 8


def config_descriptor(speed, kind=2, real=None):
    """The vendor configuration: one interface, class FF, bulk IN 1, bulk OUT
    2, interrupt IN 3 (what Linux checks); at SuperSpeed each endpoint has
    its companion (bMaxBurst 3 for bulk is this model's choice). real: a real
    device's SuperSpeed values (REAL8157)."""
    bulk, ss = bulk_size(speed), speed == 'super'
    burst, isize, ival, attrs, power = 3, 2, int_interval(speed), 0xa0, 50
    if real and ss:
        burst, isize, ival = real['burst'], real['int_size'], real['int_interval']
    if real:
        attrs, power = real['attributes'], real['power']
    body = bytes((9, 4, 0, 0, 3, 0xff, 0xff, 0, 0))
    for ep, attr, size, interval, bmax in ((EP_IN, 2, bulk, 0, burst), (EP_OUT, 2, bulk, 0, burst),
                                           (EP_INT, 3, isize, ival, 0)):
        body += bytes((7, 5, ep, attr)) + struct.pack('<HB', size, interval)
        if ss:
            body += bytes((6, 0x30, bmax, 0)) + struct.pack('<H', isize if attr == 3 else 0)
    return bytes((9, kind)) + struct.pack('<HBBBBB', 9 + len(body), 1, 1, 0, attrs, power) + body


def bos_descriptor(kind, speed, real=None):
    """BOS: the USB 2.0 extension (LPM); at SuperSpeed also the SuperSpeed
    capability, and for a bcdUSB 0x0320 chip a SuperSpeedPlus capability of
    one symmetric 10 Gb/s sublink (Gen 2x1). None at Full Speed (bcdUSB
    0x0200). real: a real device's values (REAL8157)."""
    if speed == 'full':
        return None
    caps = [bytes((7, 16, 2)) + struct.pack('<I', real['usb2_ext'] if real else 0x02)]
    if speed == 'super':
        caps.append(bytes((10, 16, 3, real['ss_attributes'] if real else 0)) +
                    struct.pack('<HBBH', 0x000e, real['ss_function'] if real else 1, 10, 0x07ff))
    if speed == 'super' and kind.usb3 >= 0x0320:
        caps.append(bytes((20, 16, 0x0a, 0)) + struct.pack('<IHH', 1, 0x1100, 0) +
                    struct.pack('<II', 0x000a4030, 0x000a40b0))
    body = b''.join(caps)
    return bytes((5, 15)) + struct.pack('<HB', 5 + len(body), len(caps)) + body


def usb_string(text):
    raw = text.encode('utf-16le')
    return bytes((len(raw) + 2, 3)) + raw


def ones(data):
    """The Internet checksum (RFC 1071) of data."""
    if len(data) & 1:
        data += b'\0'
    total = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return ~total & 0xffff


def ip_header_sum(ip):
    head = bytearray(ip)
    head[10:12] = b'\0\0'
    return ones(bytes(head))


def l4_sum(frame, transport, proto):
    """The TCP or UDP checksum of an IPv4 frame, its header at transport."""
    ip = frame[14:]
    seg = bytearray(frame[transport:])
    seg[16 if proto == 6 else 6:18 if proto == 6 else 8] = b'\0\0'
    pseudo = ip[12:20] + struct.pack('!BBH', 0, proto, len(seg))
    s = ones(pseudo + bytes(seg))
    return 0xffff if proto == 17 and s == 0 else s


def ipv4_parse(frame):
    """(header length, total length, protocol) of an IPv4 frame, or None."""
    if len(frame) < 34 or frame[12:14] != b'\x08\x00' or frame[14] >> 4 != 4:
        return None
    ihl = (frame[14] & 15) * 4
    if ihl < 20 or 14 + ihl > len(frame):
        return None
    return ihl, struct.unpack_from('!H', frame, 16)[0], frame[23]


def tx_checksum(frame, opts2):
    """A frame whose descriptor asks for the IPv4 and TCP/UDP checksums."""
    info = ipv4_parse(frame)
    if not info:
        raise ValueError('checksum offload on a frame that is not IPv4')
    ihl, total, proto = info
    if len(frame) > 14 + total and opts2 & (TX_TCP_CS | TX_UDP_CS):
        # A real RTL8157 sent a TCP ACK padded to 60 bytes with a TCP
        # checksum request out with the checksum wrong (the peer dropped
        # the ACKs; 2026-11-07, seen as stalled receives, the wire not
        # captured); Linux and Realtek's driver send such frames unpadded.
        raise ValueError('checksum offload on a frame padded past its IP length')
    out = bytearray(frame)
    if opts2 & (TX_TCP_CS | TX_UDP_CS):
        transport = opts2 >> 17 & 0x7ff
        want = 6 if opts2 & TX_TCP_CS else 17
        if transport != 14 + ihl or proto != want:
            raise ValueError('checksum offload: header offset %d or protocol %d wrong' % (transport, proto))
        at = transport + (16 if want == 6 else 6)
        struct.pack_into('!H', out, at, l4_sum(bytes(out[:14 + total]), transport, want))
    if opts2 & TX_IPV4_CS or opts2 & (TX_TCP_CS | TX_UDP_CS):
        struct.pack_into('!H', out, 24, ip_header_sum(bytes(out[14:14 + ihl])))
    return bytes(out)


def tx_large_send(frame, opts1, opts2):
    """Segment a large TCP send: MSS bytes of payload each, headers copied,
    IP total length, ID and checksum and TCP sequence and checksum
    rewritten; FIN and PSH on the last segment only."""
    info = ipv4_parse(frame)
    if not info or info[2] != 6:
        raise ValueError('large send that is not TCP over IPv4')
    ihl, _, _ = info
    transport = 14 + ihl
    given = opts1 >> 18 & 0x7f
    if given and given != transport:
        raise ValueError('large send: TCP header offset %d, not %d' % (given, transport))
    mss = opts2 >> 17 & 0x7ff
    thl = (frame[transport + 12] >> 4) * 4
    if not mss or thl < 20 or transport + thl > len(frame):
        raise ValueError('large send without an MSS or a TCP header')
    head, payload = frame[:transport + thl], frame[transport + thl:]
    ident, = struct.unpack_from('!H', frame, 18)
    seq, = struct.unpack_from('!I', frame, transport + 4)
    flags = frame[transport + 13]
    out = []
    for k, at in enumerate(range(0, max(len(payload), 1), mss)):
        part = payload[at:at + mss]
        last = at + mss >= len(payload)
        f = bytearray(head + part)
        struct.pack_into('!H', f, 16, ihl + thl + len(part))
        struct.pack_into('!H', f, 18, (ident + k) & 0xffff)
        struct.pack_into('!I', f, transport + 4, (seq + at) & 0xffffffff)
        f[transport + 13] = flags if last else flags & ~0x09
        struct.pack_into('!H', f, 24, ip_header_sum(bytes(f[14:14 + ihl])))
        struct.pack_into('!H', f, transport + 16, l4_sum(bytes(f), transport, 6))
        if len(f) > 1514:
            raise ValueError('large send segment longer than 1514 bytes (MSS %d)' % mss)
        out.append(bytes(f))
    return out


def rx_checksum_bits(frame):
    """The receive descriptor's third dword for a frame (16-byte format)."""
    info = ipv4_parse(frame)
    if not info:
        return RX2_IPF | RX2_UDPF | RX2_TCPF
    ihl, total, proto = info
    bits = RX2_IPV4
    if ip_header_sum(frame[14:14 + ihl]) != struct.unpack_from('!H', frame, 24)[0]:
        return bits | RX2_IPF | RX2_UDPF | RX2_TCPF
    transport = 14 + ihl
    body = frame[:14 + total]
    if proto == 6 and len(body) >= transport + 20:
        bits |= RX2_TCP | RX2_UDPF
        if l4_sum(body, transport, 6) != struct.unpack_from('!H', body, transport + 16)[0]:
            bits |= RX2_TCPF
    elif proto == 17 and len(body) >= transport + 8:
        bits |= RX2_UDP | RX2_TCPF
        given = struct.unpack_from('!H', body, transport + 6)[0]
        if given and l4_sum(body, transport, 17) != given:
            bits |= RX2_UDPF
    else:
        bits |= RX2_TCPF | RX2_UDPF
    return bits


class Model:
    def __init__(self, connection, trace, chip='8153', speed='high', now=time.monotonic,
                 link_after=1.0, flap_after=None, flap_for=2.0, partner=10000, ext_init=False,
                 power_cut=False, phy_patch=False, serial=None, serial_stall=0,
                 serial_skip=0, vid=None, pid=None, bcd_device=None, irq_on_change=False,
                 bridge=None):
        self.connection, self.trace, self.now = connection, trace, now
        self.bridge = bridge                       # (socket, remote) of --udp, or None
        self.chip_name, self.speed = chip, speed or default_speed(chip)
        self.chip = Chip(chip, ext_init, power_cut, phy_patch, pid)
        kind = self.chip.kind
        if self.speed == 'super' and not kind.usb3:
            raise ValueError('%s is a USB 2.0 chip' % kind.name)
        self.vid = 0x0bda if vid is None else vid
        self.bcd_device = kind.bcd if bcd_device is None else bcd_device
        self.partner = partner
        self.net = Network(self.log)
        real = self.chip.real
        self.serial_index = (real['serial_index'] if real else 3) if serial is not None else 0
        if serial == 'mac':                          # as the real RTL8157's: 0003 and the address
            serial = '0003' + self.chip.mac.hex().upper()
        # The real RTL8157's CDC configurations, at SuperSpeed only (only
        # those were read from it).
        self.more_configs = [bytes.fromhex(x) for x in real['configs']] if real and self.speed == 'super' else []
        self.device = device_descriptor(kind, self.speed, self.serial_index, self.vid, self.chip.pid,
                                        self.bcd_device, 1 + len(self.more_configs))
        self.config_desc = config_descriptor(self.speed, real=real)
        # The interrupt endpoint as the descriptor gives it: QEMU ends an
        # interrupt transfer on a packet shorter than this, so a 2-byte report
        # on the real RTL8157's 16-byte endpoint completes the transfer alone.
        self.int_size, self.int_interval = (real['int_size'], real['int_interval']) \
            if real and self.speed == 'super' else (2, int_interval(self.speed))
        self.other = None if self.speed == 'super' else \
            config_descriptor('full' if self.speed == 'high' else 'high', 7, real)
        self.bos = bos_descriptor(kind, self.speed, real)
        self.strings = {0: b'\x04\x03\x09\x04', 1: usb_string('usb-wdm-free SIMULATED'),
                        2: usb_string('%s Ethernet - SIMULATED' % kind.name)}
        if self.more_configs:
            self.strings[3] = usb_string(self.chip.mac.hex().upper())   # their iMACAddress
        if serial is not None:
            self.strings[self.serial_index] = usb_string(serial)
        self.serial_stall = serial_stall           # serial string reads still to STALL
        self.serial_skip = serial_skip             # reads answered before the STALLs
        self.bulk_max = bulk_size(self.speed)
        self.link_after, self.flap_after, self.flap_for = link_after, flap_after, flap_for
        self.peer_caps = 0
        self.config = 0
        self.u1u2 = 0                              # GET_STATUS bits 2 and 3 (U1/U2 enabled)
        self.interrupt = False
        # The real RTL8157 answers every interrupt poll: here one report per
        # bInterval at SuperSpeed (11: 128 ms), each showing the link as the
        # one before did (the device's report changed a poll after MR_LINK;
        # Realtek's driver takes the link up only on that order). The other
        # profiles, and this one with irq_on_change, report only a change.
        self.irq_period = (2 ** (real['int_interval'] - 1)) * 125e-6 \
            if real and self.speed == 'super' and not irq_on_change else None
        self.irq_next = 0.0
        self.irq_shown = False
        self.pending = collections.OrderedDict()   # bulk-IN request id -> length
        self.out_buf, self.out_pieces = bytearray(), 0   # a bulk-OUT transfer arriving in pieces
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
                                               (16 + (EP_INT & 15), 3, self.int_interval, self.int_size)):
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
        self.out_buf, self.out_pieces = bytearray(), 0
        self.config, self.interrupt = 0, False
        self.u1u2 = 0                              # a reset clears the U1/U2 enables

    # -- link ----------------------------------------------------------------
    def set_link(self, up, why):
        if up == self.link:
            return
        if self.chip.newer:
            got = self.chip.negotiate(self.partner) if up else None
            if up and got is None:
                why = 'no_common_speed' if self.chip.phy_state() == PHYST_LAN_ON else 'phy_not_on'
                self.log(why, partner=self.partner, phy_state=self.chip.phy_state())
                self.totals[why] += 1
                return
            self.chip.show_link(got, self.partner)
            self.link_mbps, self.link_full = got if got else (0, False)
            if got and got[0] > 1000 and self.speed != 'super':   # off Linux's default advertisement
                self.totals['multigig_below_superspeed'] += 1
        else:
            # The RTL8152 and 8153: the speed and duplex the PHY's control
            # and advertisement leave (the chip's fastest, as before, with
            # the driver's default advertisement).
            got = self.chip.negotiate(self.partner) if up else None
            if up and got is None:
                self.log('no_common_speed', partner=self.partner)
                self.totals['no_common_speed'] += 1
                return
            self.chip.pla[MR_LINK] = (LNK_UP | LNK_SPEED[got[0]] | (LNK_FULL if got[1] else 0)) if up else 0
            self.link_mbps, self.link_full = got if up else (0, False)
        self.link = up
        self.log('link', up=up, why=why, mbps=self.link_mbps, full=self.link_full,
                 **(self.chip.settings() if up else {}))
        if not self.irq_period:
            self.irq.append(self.irq_word(up))                # the bit is the link state
            self.flush_irq()

    def irq_word(self, up):
        real = self.chip.real
        if real:
            return real['irq_up'] if up else real['irq_down']
        return bytes((IRQ_LINK if up else 0, 0))

    def flush_irq(self):
        while self.interrupt and self.irq:
            data = self.irq.popleft()
            self.send(103, data=struct.pack('<BBH', EP_INT, 0, len(data)) + data)
            self.totals['interrupts'] += 1

    def pump(self):
        now = self.now()
        if self.irq_period and self.interrupt and now >= self.irq_next:
            self.irq_next = now + self.irq_period
            self.irq.append(self.irq_word(self.irq_shown))
            self.irq_shown = self.link
            self.flush_irq()
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
        off; on the newer chips, none more once the early size is reached).
        The RTL8157/8159's 16-byte header carries the length in bits
        17-31 of its first dword; the others' 24-byte header in bits 0-14."""
        out = bytearray()
        desc_len, align = self.chip.rx_desc
        single = self.chip.usb_word(UR_CTRL) & CTRL_RX_AGG_OFF
        early = self.chip.early_bytes()
        while self.rx:
            frame = self.rx[0]
            record = desc_len + len(frame) + 4
            padded = (record + align - 1) & ~(align - 1)
            if len(out) + record > length:
                break
            self.rx.popleft()
            size = len(frame) + 4
            desc = struct.pack('<I', size << 17 if self.chip.wide else size) + bytes(desc_len - 4)
            if self.chip.wide:
                desc = desc[:8] + struct.pack('<I', rx_checksum_bits(frame)) + desc[12:]
            out += desc + frame + struct.pack('<I', zlib.crc32(frame))
            out += b'\xee' * (padded - record)
            self.totals['rx_frames'] += 1
            if single or (early and len(out) >= early):
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
            if not self.bridge:
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

    def bulk_out(self, data):
        """One usb-redir bulk-OUT packet. A host controller model may hand
        over one transfer in pieces: QEMU's UHCI sends each 64-byte packet
        alone, where its EHCI and xHCI send the whole transfer. The pieces
        are joined until the transfer ends: a short packet, a zero-length
        one, or (full-size pieces) records that end with the data."""
        if data:
            self.out_pieces += 1
        self.out_buf += data
        if not self.out_buf:
            return
        if len(data) == self.bulk_max and not self.tx_complete(self.out_buf):
            if len(self.out_buf) > 1 << 17:
                raise ValueError('bulk OUT transfer never ends')
            return
        whole, pieces = bytes(self.out_buf), self.out_pieces
        self.out_buf, self.out_pieces = bytearray(), 0
        if pieces > 1:
            if not self.totals['tx_joined']:
                self.log('tx_joined', pieces=pieces, length=len(whole))
            self.totals['tx_joined'] += 1
        self.transmit(whole)

    def tx_complete(self, data):
        """True when data holds whole transmit records up to its end (zero
        padding after the last one allowed); the checks are transmit()'s."""
        hlen, align = self.chip.tx_desc
        off = 0
        while off + hlen <= len(data):
            length = struct.unpack_from('<I', data, off)[0] & 0x3ffff
            if not length:
                return not any(data[off:])
            if off + hlen + length > len(data):
                return False
            off = (off + hlen + length + align - 1) & ~(align - 1)
        return not any(data[off:])

    def transmit(self, data):
        """One bulk-OUT transfer: header + frame, each record aligned. The
        8-byte header has the length in bits 0-17 of its first dword; the
        RTL8157/8159's 16-byte one in bits 4-21 of its third, with the
        signature 0x15 in bits 27-31 of its fourth, and the same length in
        bits 0-17 of its first, as Linux writes it."""
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
                if opts1 & 0x3ffff != length:
                    raise ValueError('transmit header lengths disagree')
            if not length or off + hlen + length > len(data):
                raise ValueError('transmit header length beyond the transfer')
            if opts1 & TX_FIRST_LAST != TX_FIRST_LAST:
                raise ValueError('transmit header without the first and last segment bits')
            frame = data[off + hlen:off + hlen + length]
            opts2 = struct.unpack_from('<I', data, off + 4)[0]
            if (opts1 & TX_GTSENDV4 or opts2 & 0xf0000000) and not self.chip.wide:
                raise ValueError('transmit offload bits on a chip modelled without offload')
            if opts1 & TX_GTSENDV4:
                segs = tx_large_send(frame, opts1, opts2)
                self.totals['tx_large_sends'] += 1
                self.totals['tx_large_send_segments'] += len(segs)
                frames.extend(segs)
            elif opts2 & (TX_IPV4_CS | TX_TCP_CS | TX_UDP_CS):
                frames.append(tx_checksum(frame, opts2))
                self.totals['tx_checksummed'] += 1
            else:
                if len(frame) > 1514:
                    raise ValueError('transmit frame longer than 1514 bytes without a large send')
                frames.append(frame)
            off = (off + hlen + length + align - 1) & ~(align - 1)
        if off < len(data) and any(data[off:]):
            raise ValueError('trailing bytes after the last frame')
        if not (self.chip.pla[MR_COMMAND] & CMD_TX_ON):
            self.totals['tx_while_disabled'] += 1
        if self.chip.wide and self.chip.usb[UR_TX_SIG_CHECK] & TXSIG_CHECK:
            self.totals['tx_sig_check_left_on'] += 1
        for frame in frames:
            self.totals['tx_frames'] += 1
            if self.bridge:
                if self.link:
                    self.bridge[0].sendto(frame, self.bridge[1])
                continue
            self.log('tx_frame', length=len(frame), destination=frame[:6].hex(),
                     ethertype=frame[12:14].hex() if len(frame) >= 14 else '')
            if self.link:
                for reply in self.net.handle(frame):
                    self.deliver(reply)
        return frames

    # -- control -------------------------------------------------------------
    def descriptor(self, kind, number):
        if kind == 3:
            serial = self.serial_index and number == self.serial_index and number in self.strings
            if serial and self.serial_skip > 0:
                self.serial_skip -= 1
            elif serial and self.serial_stall > 0:
                self.serial_stall -= 1
                self.log('serial_stalled', left=self.serial_stall)
                return None
            return self.strings.get(number)
        if kind == 2 and 0 < number <= len(self.more_configs):
            return self.more_configs[number - 1]
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
        elif request == 0 and rt == 0x80:
            status, data = 0, struct.pack('<H', self.u1u2)
        elif request == 0 and rt in (0x81, 0x82):
            status, data = 0, b'\0\0'
        elif request == 8 and rt == 0x80:
            status, data = 0, bytes((self.config,))
        elif request == 10 and rt == 0x81:
            status, data = 0, b'\0'
        elif request in (1, 3) and rt == 2 and value == 0:
            status = 0
        elif request in (1, 3) and rt == 0 and self.speed == 'super':
            status = 0                                  # U1/U2 enable features
            bit = {48: 4, 49: 8}.get(value, 0)
            self.u1u2 = (self.u1u2 | bit) if request == 3 else (self.u1u2 & ~bit)
        elif request in (48, 49) and rt == 0 and self.speed == 'super':
            status = 0                                  # SET_SEL, SET_ISOCH_DELAY
        elif request == 5 and rt == 0xc0:               # also unconfigured: Linux reads the id first
            got = self.chip.read(value, index, length)
            status, data = (0, got) if got is not None else (4, b'')
        elif request == 5 and rt == 0x40:
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
                self.bulk_out(data)
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
        data = struct.pack('<BBBBHH', SPEED_CODE[self.speed], 0, 0, 0, self.vid, self.chip.pid)
        if self.peer_caps & (1 << CAP_VERSION):
            data += struct.pack('<H', self.bcd_device)
        self.send(1, data=data)
        self.log('connected', model='SIMULATED %s' % self.chip.kind.name, chip=self.chip_name,
                 chip_id='%04x' % self.chip.tcr0, vid='%04x' % self.vid, pid='%04x' % self.chip.pid,
                 bcd_device='%04x' % self.bcd_device, speed=self.speed, partner=self.partner,
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
    ap.add_argument('--serial', help='a serial number string (iSerialNumber 3; 7 on the 8157, as the real '
                    "device's); 'mac' gives the real RTL8157's form, 0003 and the address; none by default")
    ap.add_argument('--vid', type=lambda x: int(x, 16), help="USB vendor id in hex (default 0BDA), e.g. a dock "
                    "maker's id, with the --chip profile behind it")
    ap.add_argument('--pid', type=lambda x: int(x, 16), help="USB product id in hex (default: the profile's)")
    ap.add_argument('--bcd-device', type=lambda x: int(x, 16), help="bcdDevice in hex (default: the profile's)")
    ap.add_argument('--irq-on-change', action='store_true',
                    help='8157: an interrupt report only on a link change, not one every bInterval')
    ap.add_argument('--serial-stall', type=int, default=0,
                    help='STALL the first N reads of the serial number string of this run')
    ap.add_argument('--serial-skip', type=int, default=0,
                    help='answer the first K reads of the serial string before those STALLs')
    ap.add_argument('--udp', help='LOCAL:REMOTE UDP ports on 127.0.0.1: bridge frames there (QEMU dgram framing)')
    args = ap.parse_args()
    bridge = None
    if args.udp:
        local, remote = (int(x) for x in args.udp.split(':'))
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        udp.bind(('127.0.0.1', local))
        udp.setblocking(False)
        bridge = (udp, ('127.0.0.1', remote))
    if args.serial is not None and not (0 < len(args.serial) <= 126 and args.serial.isascii()):
        ap.error('--serial needs 1 to 126 ASCII characters (a string descriptor holds 126)')
    if args.serial_stall < 0 or args.serial_skip < 0:
        ap.error('--serial-stall and --serial-skip need 0 or more')
    if args.serial_skip and not args.serial_stall:
        ap.error('--serial-skip needs --serial-stall')
    if args.serial_stall and args.serial is None:
        ap.error('--serial-stall needs --serial')
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
                          ext_init=args.ext_init, power_cut=args.power_cut, phy_patch=args.phy_patched,
                          serial=args.serial, serial_stall=args.serial_stall,
                          serial_skip=args.serial_skip, vid=args.vid, pid=args.pid,
                          bcd_device=args.bcd_device, irq_on_change=args.irq_on_change,
                          bridge=bridge)
            try:
                model.run()
            except (EOFError, ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
                model.log('disconnected', **model.totals)
            except Exception as error:
                model.log('fatal', error=repr(error), **model.totals)
                raise


if __name__ == '__main__':
    main()
