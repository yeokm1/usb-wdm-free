/* SPDX-License-Identifier: GPL-2.0-or-later
 * ASIX AX8817X based USB 2.0 Ethernet Devices
 * Copyright (C) 2003-2006 David Hollis <dhollis@davehollis.com>
 * Copyright (C) 2005 Phil Chang <pchang23@sbcglobal.net>
 * Copyright (C) 2006 James Painter <jamie.painter@iname.com>
 * Copyright (c) 2002-2003 TiVo Inc.
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * AX772F chip core: the ASIX AX88772B and AX88772C (ethernet.md section
 * 6.8), behind the netchip.h interface. Request codes, register bits and
 * the bring-up adapted from Linux drivers/net/usb/asix.h, asix_common.c
 * and asix_devices.c at v7.3 (GPL-2.0-or-later). The CoreChips SR9800
 * (section 6.9.1) uses the same command set; its own values below are
 * facts from Linux sr9800.c and sr9800.h (GPL version 2 only: facts, no
 * code).
 */
#ifndef AX7CHIP_H
#define AX7CHIP_H

#include "netchip.h"

/* Vendor requests (bRequest), as asix.h names them. */
#define AX_CMD_SET_SW_MII          0x06
#define AX_CMD_READ_MII_REG        0x07
#define AX_CMD_WRITE_MII_REG       0x08
#define AX_CMD_STATMNGSTS_REG      0x09
#define AX_CMD_SET_HW_MII          0x0a
#define AX_CMD_READ_EEPROM         0x0b
#define AX_CMD_READ_RX_CTL         0x0f
#define AX_CMD_WRITE_RX_CTL        0x10
#define AX_CMD_WRITE_IPG0          0x12
#define AX_CMD_READ_NODE_ID        0x13
#define AX_CMD_WRITE_NODE_ID       0x14
#define AX_CMD_WRITE_MULTI_FILTER  0x16
#define AX_CMD_READ_PHY_ID         0x19
#define AX_CMD_READ_MEDIUM_STATUS  0x1a
#define AX_CMD_WRITE_MEDIUM_MODE   0x1b
#define AX_CMD_WRITE_GPIOS         0x1f
#define AX_CMD_SW_RESET            0x20
#define AX_CMD_SW_PHY_SELECT       0x22
#define AX_QCTCTRL                 0x2a

#define AX_CHIPCODE_MASK           0x70
#define AX_AX88772_CHIPCODE        0x00
#define AX_AX88772A_CHIPCODE       0x10
#define AX_AX88772B_CHIPCODE       0x20
#define AX_HOST_EN                 0x01
#define AX_HOST_EN_RETRIES         30

#define AX_PHYSEL_SSEN             0x10
#define AX_EMBD_PHY_ADDR           0x10
#define AX_GPIO_RSE                0x80

#define AX_SWRESET_CLEAR           0x00
#define AX_SWRESET_IPRL            0x20
#define AX_SWRESET_IPPD            0x40

#define AX88772_IPG0_DEFAULT       0x15
#define AX88772_IPG1_DEFAULT       0x0c
#define AX88772_IPG2_DEFAULT       0x12

#define AX_MEDIUM_TFC              0x0020
#define AX_MEDIUM_RFC              0x0010
#define AX_MEDIUM_AC               0x0004
#define AX_MEDIUM_FD               0x0002
#define AX_MEDIUM_PS               0x0200
#define AX_MEDIUM_RE               0x0100
#define AX88772_MEDIUM_DEFAULT     (AX_MEDIUM_FD | AX_MEDIUM_PS | AX_MEDIUM_AC | AX_MEDIUM_RE)

#define AX_RX_CTL_SO               0x0080
#define AX_RX_CTL_AM               0x0010
#define AX_RX_CTL_AB               0x0008
#define AX_RX_CTL_AMALL            0x0002
#define AX_RX_CTL_PRO              0x0001
#define AX_DEFAULT_RX_CTL          (AX_RX_CTL_SO | AX_RX_CTL_AB)
#define AX_MAX_MCAST               64

/* IEEE 802.3 clause 22 registers and bits. */
#define MII_BMCR                   0x00
#define   BMCR_RESET               0x8000
#define   BMCR_SPEED100            0x2000
#define   BMCR_FULLDPLX            0x0100
#define   BMCR_ANENABLE            0x1000
#define   BMCR_PDOWN               0x0800
#define   BMCR_ANRESTART           0x0200
#define MII_BMSR                   0x01
#define   BMSR_ANEGCOMPLETE        0x0020
#define   BMSR_LSTATUS             0x0004
#define MII_ADVERTISE              0x04
#define MII_LPA                    0x05
#define   ADVERTISE_CSMA           0x0001
#define   ADVERTISE_10HALF         0x0020
#define   ADVERTISE_10FULL         0x0040
#define   ADVERTISE_100HALF        0x0080
#define   ADVERTISE_100FULL        0x0100
#define   ADVERTISE_PAUSE_CAP      0x0400
#define   ADVERTISE_PAUSE_ASYM     0x0800

#define A7_RX_TRANSFER   2048UL   /* rx_urb_size: the QCTCTRL 2 KiB burst */

/* SR9800 (ethernet.md section 6.9.1): its own requests and values. */
#define A7_KIND_AX772B   0
#define A7_KIND_SR9800   1
#define S98_LEDS_REQ       0x70     /* LED sources: wValue 0x4080, wIndex 0x4001 */
#define S98_INBURST_REQ    0x2a     /* bulk-IN burst: 4 KiB 0x8100/0x8147, 2 KiB 0x8000/0x8001 */
#define S98_GPIO_AT_RESET  0xb0     /* EEPROM reload, GPIO 2 driven high */
#define S98_RESET_EXTPHY   0x04     /* SW_RESET value with an external PHY */
#define S98_RESET_PHYLINE  0x08     /* SW_RESET: the PHY reset line */
#define S98_RX_MODE        0x0188   /* RX_CTL: receive on, broadcast, bit 8 */
#define S98_MEDIUM_MODE    0x0336   /* full duplex, both pause, 100 Mb/s, AC, receive on */
#define S98_RX_TRANSFER_HS 4096UL   /* bulk-IN transfer at High Speed */
#define A7_FRAME_MAX     1518U    /* mtu + ETH_HLEN + VLAN_HLEN */
#define A7_PHY_RESET_POLLS 50     /* 10 ms apart: clause 22's 500 ms */
#define A7_REALIGN_AFTER 4        /* malformed transfers in a row before the stream is restarted */

/* What a record stream carries across bulk-IN transfers
 * (asix_rx_fixup_info). */
typedef struct A7_RX {
    nc_u32 Header;               /* the header word; low half only while Split */
    nc_u16 Size;                 /* the frame's length from its header */
    nc_u16 Remaining;            /* bytes of it still to come */
    nc_u8 Split;                 /* a header's first two bytes ended the last transfer */
    nc_u8 Keep;                  /* the frame is delivered (14..1514 bytes) */
} A7_RX;

typedef struct A7_STATE {
    nc_u8 Kind;                  /* A7_KIND_* from the device's id */
    nc_u8 Mac[6];
    nc_u8 PhyAddr;
    nc_u8 Embedded;              /* the PHY is the chip's own (address 0x10) */
    nc_u8 ChipCode;              /* STATMNGSTS & 0x70 */
    nc_u8 LinkUp;                /* what Link last applied */
    nc_u8 DownPending;           /* the link-down work (medium 0, PHY reset) is not done yet */
    nc_u8 LinkPending;           /* a Link has not finished: Poll asks for it again */
    nc_u8 MacPending;            /* SetMac failed: Link writes WantMac */
    nc_u8 MiiReadOk;             /* the last MII read's own transfer succeeded */
    nc_u8 WantMac[6];            /* the address SetMac was asked for */
    nc_u8 FilterPending;         /* the last SetFilter failed: Link's caller applies it again */
    nc_u8 Mbps100, Full, Pause;  /* the link Link last applied */
    /* The Advanced tab (ethernet.md 4.10), from the probe at each start:
     * *SpeedDuplex (0 auto, 1-4 forced 10/100) and, on the AX88772B/C,
     * *FlowControl (bit 0 transmit, bit 1 receive pause). */
    nc_u8 Speed, Flow;
    nc_u16 RxCtl, RxCtlRead, MediumRead;
    nc_u16 RxBase;               /* RX_CTL before the filter bits (AX_DEFAULT_RX_CTL or S98_RX_MODE) */
    nc_u32 RxTransfer;           /* bulk-IN transfer length: the chip's burst */
    nc_u16 Medium;               /* the medium mode last written */
    nc_u16 PhyId1, PhyId2;
    nc_u32 Resyncs, Malformed;   /* receive stream: synchronisation lost; transfers refused */
    nc_u32 BadRun;               /* malformed transfers in a row (RxParse; read by Poll) */
    nc_u32 Realigns;             /* receive restarts by Link for a stream out of step */
    A7_RX Rx;                    /* the stream's position (the emitting walk's) */
    nc_u8 Frame[A7_FRAME_MAX];   /* the frame being collected */
} A7_STATE;

/* The chip table is NetPackageChip (netchip.h); its Settings, in order: */
enum { S_SPEED, S_FLOW, S_COUNT };

long A7Probe(const nc_u8 *device, nc_u32 deviceSize, const nc_u8 *const *config,
             const nc_u32 *size, nc_u32 count, NET_PROBE *probe);
long A7Start(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params);
long A7SetMac(void *chip, NET_IO *io, const nc_u8 mac[6]);
long A7Link(void *chip, NET_IO *io, NET_LINK *link);
int A7Poll(void *chip, NET_IO *io);
long A7SetFilter(void *chip, NET_IO *io, nc_u32 filter, const nc_u8 *multicast, nc_u32 count);
long A7Stop(void *chip, NET_IO *io);
long A7Suspend(void *chip, NET_IO *io);
long A7Resume(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params);
int A7Status(void *chip, const nc_u8 *data, nc_u32 length);
long A7RxParse(void *chip, const nc_u8 *data, nc_u32 length, NET_EMIT emit, void *context,
               NET_RX_STATS *stats);
void A7TxHeader(void *chip, nc_u8 *hdr, nc_u32 length, nc_u32 maxPacket, int single);
void A7RxReset(void *chip);
int A7TxEnd(void *chip, nc_u8 *buffer, nc_u32 *total, nc_u32 capacity, nc_u32 maxPacket);

#endif
