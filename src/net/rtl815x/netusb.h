/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * The private interface between the NDIS upper edge (netcore.c, ndis.h)
 * and the WDM/USB lower edge (netusb.c, wdm.h): the two halves the 2000
 * DDK network guide section 12.5 asks to keep in separate source files.
 * Only C types and void pointers cross it. Derived from cdcnet's
 * cdcusb.h (same author and licence). Identical copy in src/net/ax88179/
 * and src/net/rtl815x/ (see netchip.h).
 *
 * Execution contract (ethernet.md sections 3.3, 4.7 and 4.7.1):
 * - NetUsbStart, NetUsbOpen and NetUsbStop run at PASSIVE_LEVEL, from
 *   MiniportInitialize and MiniportHalt.
 * - NetUsbKick and NetUsbRequest may run at IRQL <= DISPATCH_LEVEL.
 * - Receive, ReceiveEnd, TxTake, TxCopy, TxDone and Link are called at
 *   IRQL <= DISPATCH_LEVEL, from USB completions, the pump or the worker,
 *   never with a lower-edge lock held.
 * - TaskDone is called once per accepted request, at PASSIVE_LEVEL from
 *   the adapter's worker thread, or from NetUsbStop for a request it
 *   abandons.
 */
#ifndef NETUSB_H
#define NETUSB_H

#include "netchip.h"

#define NET_TX_BATCH 64          /* frames per bulk-OUT transfer, at most */
#define NET_RX_BATCH 16          /* received frames per receive indication, at most */

/* One bulk-IN transfer's frames, gathered by the upper edge into receive
 * indications of up to NET_RX_BATCH packets. It lives on the stack of the
 * completion that parses the transfer, so concurrent completions never
 * share one. */
typedef struct NET_RX_GATHER {
    void *Packets[NET_RX_BATCH];
    nc_u32 Count;
} NET_RX_GATHER;

/* Requests run on the adapter's passive worker. */
#define NET_REQ_FILTER     1     /* Value: NDIS packet filter bits */
#define NET_REQ_MULTICAST  2     /* Data: Count * 6 bytes */
#define NET_REQ_RESET      3
#define NET_REQ_POWER      4     /* Value: 1 = D0, otherwise sleep */

typedef struct NET_CALLBACKS {
    void *Context;
    /* One frame of a bulk-IN transfer; gather starts empty for each
     * transfer, and ReceiveEnd follows its last frame. checksum: the
     * NC_RX_*_OK checksums the chip verified (0 if none). */
    void (*Receive)(void *context, NET_RX_GATHER *gather, const nc_u8 *frame, nc_u32 length,
                    nc_u32 checksum);
    void (*ReceiveEnd)(void *context, NET_RX_GATHER *gather);
    /* Dequeue, in order, up to max queued NDIS packets that fit one
     * transfer of room bytes, each at the next multiple of align after
     * the one before, behind a header of header bytes, and padded to
     * NC_ETH_PAD; the first packet always fits. Returns the number taken. */
    nc_u32 (*TxTake)(void *context, void **packets, nc_u32 max, nc_u32 room, nc_u32 header,
                     nc_u32 align);
    /* Copy one packet's frame (at most capacity bytes) and fill info with
     * the offloads it asks for; returns its length, 0 if unusable. */
    nc_u32 (*TxCopy)(void *context, void *packet, nc_u8 *buffer, nc_u32 capacity, NET_TX_INFO *info);
    void (*TxDone)(void *context, void **packets, nc_u32 count, int success);
    /* connected: 1 up, 0 down, -1 the device is gone. speed in 100 bps. */
    void (*Link)(void *context, int connected, nc_u32 speed100bps);
    void (*TaskDone)(void *context, int request, long status);
    /* The device did something its configuration rules out (NET_ANOMALY_*),
     * the first time per adapter; IRQL <= DISPATCH_LEVEL. */
    void (*Anomaly)(void *context, nc_u32 code, nc_u32 value);
} NET_CALLBACKS;

#define NET_ANOMALY_RX_FCS 1     /* received frames carried their FCS (value: in this transfer) */

typedef struct NET_START {
    /* in: the Advanced-tab values, one per NET_CHIP.Settings entry (NULL:
     * the defaults); kept by the caller for the adapter's life. */
    const long *Settings;
    nc_u8 DeviceMac[6];          /* out: the address the chip reported */
    nc_u16 Vendor, Product, Revision;  /* out */
    nc_u32 Version;              /* out: chip revision */
    nc_u8 Speed;                 /* out: NC_SPEED_* */
    nc_u32 Offload;              /* out: NC_OFFLOAD_* (NET_PARAMS.Offload) */
    nc_u32 LsoMax;               /* out: the largest large-send packet, bytes; 0 if none */
    nc_u32 TxRoom;               /* out: one transmit buffer, bytes */
    nc_u32 Failure;              /* out: NET_FAIL_* stage, for the log */
    long Status;                 /* out: status of the failing stage */
} NET_START;

#define NET_FAIL_NONE      0
#define NET_FAIL_DEVICE    1     /* device descriptor */
#define NET_FAIL_PROBE     2     /* no usable configuration */
#define NET_FAIL_SELECT    3     /* SELECT_CONFIGURATION */
#define NET_FAIL_CHIP      4     /* the chip's bring-up */
#define NET_FAIL_MAC       5     /* writing the address */
#define NET_FAIL_MEMORY    6
#define NET_FAIL_THREAD    7

/* Configure the device and run the chip's bring-up. No traffic yet. */
long NetUsbStart(void *lowerDevice, const NET_CALLBACKS *callbacks, NET_START *start, void **device);
/* Program mac (if it differs from the device's), read the link, start
 * the worker and the data path. On failure the device is freed. */
long NetUsbOpen(void *device, const nc_u8 mac[6], NET_START *start, int *connected, nc_u32 *speed100bps);
void NetUsbStop(void *device);
void NetUsbKick(void *device);
/* The device was surprise-removed (NDIS 5.1): no further submissions. */
void NetUsbSurpriseRemoved(void *device);
/* Returns nonzero when queued; TaskDone then follows exactly once. */
int NetUsbRequest(void *device, int request, nc_u32 value, const nc_u8 *data, nc_u32 length);

void NetTrace(const char *format, ...);

#endif
