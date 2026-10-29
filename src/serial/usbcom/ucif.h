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

#define UCIF_VERSION 2        /* 2: the limits, UCIF_CAP_FLOW and UCOP_FLOW (M5) */

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
#define UCOP_FLOW         4  /* chips (CDC ignores it): Value bit0 = fOutxCtsFlow, bit1 = fRtsFlow */

/* Or-ed into UCIF_OP.Op by PortClose's flush: the SYS takes the operation
   into the places it keeps for it (an open's last settings, such as a
   1200-baud touch, are never lost at close). The VxD keeps at most
   UCIF_VXD_OPS operations, plus a line coding and a flow setting a full
   queue held back, and then sends the DTR drop: UCIF_CLOSE_OPS in all. */
#define UCOP_F_CLOSING    0x80000000UL
#define UCIF_VXD_OPS      16
#define UCIF_CLOSE_OPS    (UCIF_VXD_OPS + 3)

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
#define UCIF_CAP_BREAK  0x0001   /* ACM bmCapabilities D2; a chip that sends break */
#define UCIF_CAP_LINE   0x0002   /* ACM bmCapabilities D1; every chip */
#define UCIF_CAP_FLOW   0x0004   /* a chip: RTS/CTS handshake (UCOP_FLOW), real CTS */
#define UCIF_CAP_CTSFLOW 0x0008  /* the chip holds its transmitter by CTS on its own (CP210x) */
#define UCIF_CAP_CTSNOW 0x0010   /* the chip reports CTS soon after open, unasked (FTDI, CP210x) */
#define UCIF_CAP_PAIRFLOW 0x0020 /* one RTS/CTS mode for both directions (FTDI, PL2303) */

#define UCIF_FLOW_CTS   0x0001   /* UCOP_FLOW: transmit held while CTS is off */
#define UCIF_FLOW_RTS   0x0002   /* UCOP_FLOW: RTS driven by the receive side */

/*
 * What the port takes (design 6.3). A bit set in DataBits for each data
 * bit count (bit n: n bits), in StopBits and Parity for each DCB value
 * (StopBits 0 = 1, 1 = 1.5, 2 = 2; Parity 0 to 4). Max56Baud, if not 0,
 * is the highest rate with 5 or 6 data bits. The VxD copies them at open:
 * the table is not read after Gone.
 */
typedef struct UCIF_LIMITS {
    unsigned long MinBaud;
    unsigned long MaxBaud;
    unsigned long DataBits;
    unsigned long StopBits;
    unsigned long Parity;
    unsigned long Max56Baud;
} UCIF_LIMITS;

typedef struct UCIF_TABLE {
    unsigned long Version;
    unsigned long Size;
    void *Port;
    unsigned long Caps;
    unsigned long PortNumber;           /* n of COMn chosen by the SYS */
    UCIF_LIMITS Limits;                 /* version 2 */
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
