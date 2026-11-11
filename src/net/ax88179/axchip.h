/* SPDX-License-Identifier: GPL-2.0-or-later
 * ASIX AX88179/178A USB 3.0/2.0 to Gigabit Ethernet Devices
 * Copyright (C) 2011-2013 ASIX
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * AX179F chip core: the AX88179/AX88178A register sequences adapted from
 * Linux drivers/net/usb/ax88179_178a.c at f47d223627f80cb7a1eb14c585fe6cb35de5e984
 * (ethernet.md sections 6.6 and 9), behind the netchip.h interface.
 */
#ifndef AXCHIP_H
#define AXCHIP_H

#include "netchip.h"

/* Vendor requests (bRequest) and registers, as named in the Linux driver. */
#define AX_ACCESS_MAC            0x01
#define AX_ACCESS_PHY            0x02
#define AX_ACCESS_EEPROM         0x04
#define AX_FIFO_STATUS_CMD       0x81    /* link_reset's TX FIFO poll, value 0x8c */
#define AX88179_PHY_ID           0x03

#define AX_PHYSICAL_LINK_STATUS  0x02
#define   AX_USB_SS              0x04
#define   AX_USB_HS              0x02
#define AX_RX_CTL                0x0b
#define   AX_RX_CTL_DROPCRCERR   0x0100
#define   AX_RX_CTL_IPE          0x0200
#define   AX_RX_CTL_START        0x0080
#define   AX_RX_CTL_AP           0x0020
#define   AX_RX_CTL_AM           0x0010
#define   AX_RX_CTL_AB           0x0008
#define   AX_RX_CTL_AMALL        0x0002
#define   AX_RX_CTL_PRO          0x0001
#define AX_NODE_ID               0x10
#define AX_MULFLTARY             0x16
#define AX_MEDIUM_STATUS_MODE    0x22
#define   AX_MEDIUM_GIGAMODE     0x01
#define   AX_MEDIUM_FULL_DUPLEX  0x02
#define   AX_MEDIUM_EN_125MHZ    0x08
#define   AX_MEDIUM_RXFLOW_CTRLEN 0x10
#define   AX_MEDIUM_TXFLOW_CTRLEN 0x20
#define   AX_MEDIUM_RECEIVE_EN   0x100
#define   AX_MEDIUM_PS           0x200
#define AX_MONITOR_MOD           0x24
#define   AX_MONITOR_MODE_RWMP   0x04
#define   AX_MONITOR_MODE_PMEPOL 0x20
#define   AX_MONITOR_MODE_PMETYPE 0x40
#define AX_PHYPWR_RSTCTL         0x26
#define   AX_PHYPWR_RSTCTL_BZ    0x0010
#define   AX_PHYPWR_RSTCTL_IPRL  0x0020
#define   AX_PHYPWR_RSTCTL_AT    0x1000
#define AX_RX_BULKIN_QCTRL       0x2e
#define AX_CLK_SELECT            0x33
#define   AX_CLK_SELECT_BCS      0x01
#define   AX_CLK_SELECT_ACS      0x02
#define   AX_CLK_SELECT_ULR      0x08
#define AX_RXCOE_CTL             0x34
#define AX_TXCOE_CTL             0x35
#define AX_PAUSE_WATERLVL_HIGH   0x54
#define AX_PAUSE_WATERLVL_LOW    0x55

#define GMII_PHY_PHYSR           0x11
#define   GMII_PHY_PHYSR_SMASK   0xc000
#define   GMII_PHY_PHYSR_GIGA    0x8000
#define   GMII_PHY_PHYSR_100     0x4000
#define   GMII_PHY_PHYSR_FULL    0x2000
#define   GMII_PHY_PHYSR_LINK    0x0400
#define GMII_PHY_PAGE_SELECT     0x1f
#define   GMII_PHY_PGSEL_PAGE0   0x0000
#define   GMII_PHY_PGSEL_PAGE3   0x0003
#define MII_BMCR                 0x00
#define   BMCR_FULLDPLX          0x0100
#define   BMCR_ANRESTART         0x0200
#define   BMCR_ANENABLE          0x1000
#define   BMCR_SPEED100          0x2000
#define MII_BMSR                 0x01
#define MII_ADVERTISE            0x04
#define MII_LPA                  0x05
#define   ADVERTISE_10HALF       0x0020
#define   ADVERTISE_10FULL       0x0040
#define   ADVERTISE_100HALF      0x0080
#define   ADVERTISE_100FULL      0x0100
#define   ADVERTISE_PAUSE_CAP    0x0400
#define   ADVERTISE_PAUSE_ASYM   0x0800
#define MII_CTRL1000             0x09
#define   ADVERTISE_1000HALF     0x0100
#define   ADVERTISE_1000FULL     0x0200
#define   GMII_PHY_PGSEL_PAGE5   0x0005
#define MDIO_EEE_100TX           0x0002
#define MDIO_EEE_1000T           0x0004
#define MII_MMD_CTRL             0x0d
#define MII_MMD_DATA             0x0e
#define   MII_MMD_CTRL_NOINCR    0x4000
#define MII_PHYADDR              0x19
#define MDIO_MMD_AN              7
#define MDIO_AN_EEE_ADV          60

#define AX_RX_BUFFER   26624UL   /* 1024 * (0x18 + 2), the largest queue size */
#define AX_INT_LINK    0x00010000UL

typedef struct AX_STATE {
    nc_u16 RxCtl;
    nc_u16 RxCtlRead;            /* RX_CTL read back after AxStart wrote it (log) */
    nc_u8 UsbSpeed;              /* NC_SPEED_* from the endpoints */
    nc_u8 LinkUp;                /* what Link last applied */
    nc_u8 Mac[6];
    nc_u32 RxLength;
    nc_u16 Phy;                  /* last PHYSR read */
    nc_u16 Medium;               /* last medium mode written */
    nc_u8 Queue[5];              /* last bulk-IN queue control written */
    nc_u32 FifoBusy;             /* link_reset FIFO polls that timed out */
    /* The Advanced tab (ethernet.md 4.10), from the probe at each start:
     * *SpeedDuplex (0 auto, 1-4 forced 10/100, 6 1000 full alone),
     * *FlowControl (bit 0 transmit, bit 1 receive pause), *EEE. */
    nc_u16 Speed;
    nc_u8 Flow, Eee;
    nc_u8 Pause;                 /* pause directions last applied (bit 0 transmit, bit 1 receive) */
} AX_STATE;

/* The chip table is NetPackageChip (netchip.h); its Settings, in order: */
enum { S_SPEED, S_FLOW, S_EEE, S_COUNT };

long AxProbe(const nc_u8 *device, nc_u32 deviceSize, const nc_u8 *const *config,
             const nc_u32 *size, nc_u32 count, NET_PROBE *probe);
long AxStart(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params);
long AxSetMac(void *chip, NET_IO *io, const nc_u8 mac[6]);
long AxLink(void *chip, NET_IO *io, NET_LINK *link);
int AxPoll(void *chip, NET_IO *io);
long AxSetFilter(void *chip, NET_IO *io, nc_u32 filter, const nc_u8 *multicast, nc_u32 count);
long AxStop(void *chip, NET_IO *io);
long AxSuspend(void *chip, NET_IO *io);
long AxResume(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params);
int AxStatus(void *chip, const nc_u8 *data, nc_u32 length);
long AxRxParse(void *chip, const nc_u8 *data, nc_u32 length, NET_EMIT emit, void *context,
               NET_RX_STATS *stats);
void AxTxHeader(void *chip, nc_u8 *hdr, nc_u32 length, nc_u32 maxPacket, int single);
int AxTxEnd(void *chip, nc_u8 *buffer, nc_u32 *total, nc_u32 capacity, nc_u32 maxPacket);

#endif
