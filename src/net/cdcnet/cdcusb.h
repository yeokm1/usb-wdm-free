/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * The private interface between CDCNET's NDIS upper edge (cdcnet.c,
 * ndis.h) and its WDM/USB lower edge (cdcusb.c, wdm.h): the two halves
 * the 2000 DDK network guide section 12.5 asks to keep in separate source
 * files. Only C types and void pointers cross it.
 *
 * Execution contract (ethernet.md sections 3.3, 4.7 and 4.7.1):
 * - CdcUsbStart and CdcUsbStop run at PASSIVE_LEVEL, from
 *   MiniportInitialize and MiniportHalt.
 * - CdcUsbKick and CdcUsbRequest may run at IRQL <= DISPATCH_LEVEL.
 * - Receive, TxTake, TxCopy, TxDone and Link are called at
 *   IRQL <= DISPATCH_LEVEL, from USB completion routines or the pump,
 *   never with a lower-edge lock held.
 * - TaskDone is called once per accepted request, at PASSIVE_LEVEL from
 *   the adapter's worker thread, or from CdcUsbStop for a request it
 *   abandons.
 */
#ifndef CDCUSB_H
#define CDCUSB_H

#include "cdcproto.h"

#define CDC_TX_BATCH 32           /* frames per transfer, at most */

/* Requests run on the adapter's passive worker. */
#define CDC_REQ_FILTER     1      /* Value: NDIS packet filter bits */
#define CDC_REQ_MULTICAST  2      /* Data: Count * 6 bytes */
#define CDC_REQ_RESET      3
#define CDC_REQ_POWER      4      /* Value: 1 = D0, otherwise sleep */

typedef struct CDC_CALLBACKS {
    void *Context;
    void (*Receive)(void *context, const cdc_u8 *frame, cdc_u32 length);
    /* Dequeue up to max queued NDIS packets; returns the number taken. */
    cdc_u32 (*TxTake)(void *context, void **packets, cdc_u32 max);
    /* Copy one packet's frame; returns its length, 0 if unusable. */
    cdc_u32 (*TxCopy)(void *context, void *packet, cdc_u8 *buffer, cdc_u32 capacity);
    void (*TxDone)(void *context, void **packets, cdc_u32 count, int success);
    void (*Link)(void *context, int connected, cdc_u32 speed100bps);
    void (*TaskDone)(void *context, int request, long status);
} CDC_CALLBACKS;

typedef struct CDC_START {
    unsigned AllowMask;           /* CDC_ALLOW(CDC_PROTO_*) */
    cdc_u8 Mac[6];                /* out: the address used for traffic */
    cdc_u8 PermanentMac[6];       /* out */
    cdc_u8 Protocol;              /* out: CDC_PROTO_* */
    cdc_u8 HighSpeed;             /* out: bulk wMaxPacketSize >= 512 */
    cdc_u32 LinkSpeed;            /* out: 100 bps units, initial */
    int Connected;                /* out: initial media state */
    cdc_u32 Failure;              /* out: CDC_FAIL_* stage, for the log */
    long Status;                  /* out: status of the failing stage */
} CDC_START;

#define CDC_FAIL_NONE      0
#define CDC_FAIL_DEVICE    1      /* device descriptor */
#define CDC_FAIL_PROBE     2      /* no usable configuration */
#define CDC_FAIL_SELECT    3      /* SELECT_CONFIGURATION / INTERFACE */
#define CDC_FAIL_MAC       4
#define CDC_FAIL_PROTOCOL  5      /* NCM parameters / RNDIS initialize */
#define CDC_FAIL_MEMORY    6
#define CDC_FAIL_THREAD    7

long CdcUsbStart(void *lowerDevice, const CDC_CALLBACKS *callbacks, CDC_START *start, void **device);
void CdcUsbStop(void *device);
void CdcUsbKick(void *device);
/* Returns nonzero when queued; TaskDone then follows exactly once. */
int CdcUsbRequest(void *device, int request, cdc_u32 value, const cdc_u8 *data, cdc_u32 length);
/* NDIS 5.1 surprise removal: close the gate, issue no new device request. */
void CdcUsbSurpriseRemoved(void *device);

void CdcTrace(const char *format, ...);

#endif
