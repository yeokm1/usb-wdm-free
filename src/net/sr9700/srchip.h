/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * SR9700F chip core: the CoreChips SR9700 USB Ethernet (ethernet.md section
 * 6.9.2), behind the netchip.h interface. Written for this project from
 * facts in Linux drivers/net/usb/sr9700.c and sr9700.h at v7.3 (GPL
 * version 2 only, so facts only: request codes, register addresses and
 * bits, the bring-up sequence and the frame formats; no code, names,
 * comments or structure of them).
 */
#ifndef SRCHIP_H
#define SRCHIP_H

#include "netchip.h"

/* Vendor requests (device recipient). */
#define SR7_REQ_READ        0x00  /* wValue 0, wIndex the first register, data in */
#define SR7_REQ_WRITE       0x01  /* wValue 0, wIndex the first register, data out */
#define SR7_REQ_WRITE1      0x03  /* wValue the byte, wIndex the register, no data */

/* Registers and bits. */
#define SR7_NET_CTRL        0x00
#define   SR7_NET_CTRL_RESET   0x01
#define SR7_NET_STATUS      0x01
#define   SR7_NET_STATUS_LINK  0x40
#define SR7_RX_CTRL         0x05
#define   SR7_RX_ON            0x01
#define   SR7_RX_PROMISC       0x02
#define   SR7_RX_ALL_MULTI     0x08
#define   SR7_RX_DROP_CRC      0x10
#define   SR7_RX_DROP_LONG     0x20
#define SR7_RX_DEFAULT      (SR7_RX_ON | SR7_RX_DROP_CRC | SR7_RX_DROP_LONG)
#define SR7_ADDRESS         0x10  /* 6 bytes */
#define SR7_PHY_RESET       0x1f
#define   SR7_PHY_RESET_ON     0x01

#define SR7_RX_GOOD         0x40  /* the status byte before each received frame */
#define SR7_RX_OVERHEAD     7     /* 3 bytes before the frame, its 4-byte FCS after */
#define SR7_RX_TRANSFER     3072UL

typedef struct SR7_STATE {
    nc_u8 Mac[6];
    nc_u8 LinkUp;                /* what Link last applied */
    nc_u8 RxCtrl;                /* the receive control last written */
    nc_u8 RxCtrlRead;
    nc_u8 Status;                /* the network status last read */
    nc_u8 FilterPending;         /* the last SetFilter failed: Poll asks for Link, then the filter */
    nc_u8 MacPending;            /* the last SetMac failed: Link writes WantMac first */
    nc_u8 WantMac[6];
    nc_u32 Malformed, Tails;     /* refused transfers; transfers with a short tail */
} SR7_STATE;

long Sr7Probe(const nc_u8 *device, nc_u32 deviceSize, const nc_u8 *const *config,
              const nc_u32 *size, nc_u32 count, NET_PROBE *probe);
long Sr7Start(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params);
long Sr7SetMac(void *chip, NET_IO *io, const nc_u8 mac[6]);
long Sr7Link(void *chip, NET_IO *io, NET_LINK *link);
int Sr7Poll(void *chip, NET_IO *io);
long Sr7SetFilter(void *chip, NET_IO *io, nc_u32 filter, const nc_u8 *multicast, nc_u32 count);
long Sr7Stop(void *chip, NET_IO *io);
long Sr7Suspend(void *chip, NET_IO *io);
long Sr7Resume(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params);
int Sr7Status(void *chip, const nc_u8 *data, nc_u32 length);
long Sr7RxParse(void *chip, const nc_u8 *data, nc_u32 length, NET_EMIT emit, void *context,
                NET_RX_STATS *stats);
void Sr7TxHeader(void *chip, nc_u8 *hdr, nc_u32 length, nc_u32 maxPacket, int single);
int Sr7TxEnd(void *chip, nc_u8 *buffer, nc_u32 *total, nc_u32 capacity, nc_u32 maxPacket);

#endif
