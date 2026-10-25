/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ucif.h - the direct-call interface between USBCOMV.VXD (the VCOMM port
 * driver) and USBCOM.SYS (the WDM driver) on Windows 98 SE and ME
 * (docs/design/serial.md 5.1.2). Both are ring 0 in one flat address
 * space; the VxD gets this table once per open with one
 * _NtKernDeviceIoControl and calls it directly from then on.
 *
 * Calling rules (the VxD keeps them; the SYS relies on them):
 *  - The VxD calls Attach, Detach and Control only from PortOpen,
 *    PortClose or its own global-event callback, never at interrupt time.
 *    Kick is called from the same places. None of them blocks except
 *    Attach, which may wait up to two seconds for a previous open's I/O.
 *  - The SYS calls RxData, TxPull, TxDone and SerialState from its DPC or
 *    URB completion routines (DISPATCH_LEVEL), and Gone from its PnP path
 *    (PASSIVE_LEVEL). It calls none of them after Detach has returned.
 *  - After the SYS has called Gone, the VxD never calls the table again
 *    (not even Detach): the SYS frees the port once Gone returns and its
 *    URBs have drained, and the driver image may be unloaded after that.
 *
 * This header compiles both under the NT DDK headers and under the 98 DDK
 * VxD headers, so it uses only plain C types.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#ifndef UCIF_H
#define UCIF_H

#define UCIF_VERSION 1

/* CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS) */
#define IOCTL_USBCOM_GET_INTERFACE 0x00222400UL

/* The input buffer of IOCTL_USBCOM_GET_INTERFACE. */
#define UCIF_MAGIC 0x4D4F4355UL          /* 'UCOM' */
typedef struct UCIF_REQUEST {
    unsigned long Magic;
    unsigned long Version;
} UCIF_REQUEST;

/* Kick flags */
#define UCK_TX        0x01   /* the transmit queue has data: pull it */
#define UCK_RX        0x02   /* the receive queue has room again */
#define UCK_PURGE_TX  0x04   /* PurgeComm(TX): abort OUT transfers in flight */
#define UCK_PURGE_RX  0x08   /* PurgeComm(RX): drop data held by the SYS */

/* Control operations, sent in call order and never coalesced (7.2) */
#define UCOP_LINE_CODING  1  /* Data[0..6] = SET_LINE_CODING payload */
#define UCOP_CONTROL      2  /* Value bit0 = DTR, bit1 = RTS */
#define UCOP_BREAK        3  /* Value = SEND_BREAK wValue (0xFFFF on, 0 off) */

typedef struct UCIF_OP {
    unsigned long Op;
    unsigned long Value;
    unsigned char Data[8];
} UCIF_OP;

typedef struct UCIF_CALLBACKS {
    unsigned long Size;
    void *Context;
    /* Received bytes; returns how many were taken. The rest stays with
       the SYS until UCK_RX. */
    unsigned long (__stdcall *RxData)(void *ctx, const unsigned char *data,
                                      unsigned long len);
    /* Fills buf with up to max bytes to send; returns the count. */
    unsigned long (__stdcall *TxPull)(void *ctx, unsigned char *buf,
                                      unsigned long max);
    /* `sent` bytes reached the device (0 on error); `idle` is 1 when no
       OUT transfer is in flight any more. */
    void (__stdcall *TxDone)(void *ctx, unsigned long sent, unsigned long idle);
    /* A SERIAL_STATE notification (PSTN 1.2 6.5.4). */
    void (__stdcall *SerialState)(void *ctx, unsigned short state);
    /* The device is gone: see the rules above. */
    void (__stdcall *Gone)(void *ctx);
} UCIF_CALLBACKS;

/* UCIF_TABLE.Caps */
#define UCIF_CAP_BREAK  0x0001   /* ACM bmCapabilities D2 */
#define UCIF_CAP_LINE   0x0002   /* ACM bmCapabilities D1 */

typedef struct UCIF_TABLE {
    unsigned long Version;
    unsigned long Size;
    void *Port;
    unsigned long Caps;
    unsigned long PortNumber;           /* n of COMn chosen by the SYS */
    /* Returns 0 on success, nonzero if the port is gone or busy. */
    long (__stdcall *Attach)(void *port, const UCIF_CALLBACKS *cb);
    void (__stdcall *Detach)(void *port);
    void (__stdcall *Kick)(void *port, unsigned long flags);
    /* Queues one control operation; returns 0, or nonzero if the queue is
       full (the VxD keeps the operation and tries again later). */
    long (__stdcall *Control)(void *port, const UCIF_OP *op);
} UCIF_TABLE;

/* The output buffer of IOCTL_USBCOM_GET_INTERFACE. */
typedef struct UCIF_REPLY {
    unsigned long Magic;
    UCIF_TABLE *Table;
} UCIF_REPLY;

/* Device name of a port's PDO: \Device\UsbCom<PortName>, e.g. ...COM3 */
#define UCIF_DEVICE_PREFIX "\\Device\\UsbCom"

#endif /* UCIF_H */
