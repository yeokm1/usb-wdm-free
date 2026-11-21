/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * usbmtp.h - USBMTP.SYS's interface 1.1 (docs/design/mtp.md 5.1 to 5.3:
 * the controls of 1.0, and GET_INFO's power fields of 1.1), shared by the
 * driver and the protocol library. All controls are
 * FILE_DEVICE_UNKNOWN, METHOD_BUFFERED, FILE_ANY_ACCESS; lengths are exact
 * unless a range is given; structures are packed to 4 bytes and
 * little-endian.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#ifndef USBMTP_H
#define USBMTP_H

#define USBMTP_MAJOR 1
#define USBMTP_MINOR 1

#define USBMTP_CODE(n) CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (n), METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_USBMTP_GET_INFO      USBMTP_CODE(1)   /* any: out 40 to sizeof(USBMTP_INFO) */
#define IOCTL_USBMTP_GET_STRING    USBMTP_CODE(2)   /* any: in USBMTP_STRING_IN, out 256 */
#define IOCTL_USBMTP_CLAIM         USBMTP_CODE(3)   /* info: out USBMTP_CLAIM_INFO */
#define IOCTL_USBMTP_BIND          USBMTP_CODE(4)   /* info: in USBMTP_CLAIM_INFO */
#define IOCTL_USBMTP_CLASS_REQUEST USBMTP_CODE(5)   /* owner, bound: in USBMTP_CLASS_IN, out 0 or data */
#define IOCTL_USBMTP_RESET_PIPE    USBMTP_CODE(6)   /* owner, bound: in ULONG pipe */
#define IOCTL_USBMTP_ABORT         USBMTP_CODE(7)   /* owner, bound: in USBMTP_WAIT_IN, out ULONG */
#define IOCTL_USBMTP_EVENTS        USBMTP_CODE(8)   /* owner, bound: in USBMTP_WAIT_IN, out ULONG */
#define IOCTL_USBMTP_SET_TIMEOUTS  USBMTP_CODE(9)   /* owner: in USBMTP_TIMEOUTS */
#define IOCTL_USBMTP_WRITE_ZLP     USBMTP_CODE(10)  /* owner */
#define IOCTL_USBMTP_WAIT_EVENT    USBMTP_CODE(11)  /* owner, bound: in ULONG ms, out USBMTP_EVENT */

#define USBMTP_REQ_CANCEL          1   /* class request 64h */
#define USBMTP_REQ_EXT_EVENT       2   /* 65h, Get Extended Event Data */
#define USBMTP_REQ_RESET           3   /* 66h, Device Reset */
#define USBMTP_REQ_STATUS          4   /* 67h, Get Device Status */

#define USBMTP_PIPE_BULK_IN        1
#define USBMTP_PIPE_BULK_OUT       2
#define USBMTP_PIPE_INT_IN         3

#define USBMTP_MASK_BULK_IN        1   /* ABORT's pipe mask */
#define USBMTP_MASK_BULK_OUT       2

#define USBMTP_EVENTS_STOP         1
#define USBMTP_EVENTS_START        2

#define USBMTP_FLAG_CLAIMED        1   /* USBMTP_INFO.Flags */
#define USBMTP_FLAG_REMOVING       2
#define USBMTP_EVENT_LOST          1   /* USBMTP_EVENT.Flags: events were dropped since the last wait */

#define USBMTP_MAX_TRANSFER        1048576UL
#define USBMTP_SEGMENT             65536UL
#define USBMTP_MAX_DEVICES         16

#include <pshpack4.h>

/* GET_INFO: the caller passes 40 bytes (the 1.0 base) up to the whole
   structure and gets that much; cbSize is always the driver's own size,
   so a later extension is seen by its size (5.1). */
#define USBMTP_INFO_SIZE_1_0       40
#define USBMTP_INFO_SIZE_1_0X      44   /* 1.0 with the endpoint extension */
#define USBMTP_INFO_SIZE           52

/* GET_INFO's power fields (interface 1.1, mtp.md 5.3 and 5.4, "What user
   mode can read"). */
#define USBMTP_POWER_SYS           1    /* PowerGate: a system state below working is set */
#define USBMTP_POWER_DEV           2    /* a device state below D0 */
#define USBMTP_POWER_QUERY         4    /* a system standby prepared or accepted */
#define USBMTP_POWER_DRAIN         8    /* requests the gate stopped are still out */
#define USBMTP_POWER_DQUERY        16   /* another component's device query prepared or accepted */
#define USBMTP_HELD_TAILS          0x20 /* Held: completions still returning requests to callers */

typedef struct USBMTP_INFO {            /* 52 bytes */
    ULONG cbSize;                       /* 52 */
    USHORT Major, Minor;
    ULONG DriverVersion;                /* 0xMMmmbbrr */
    ULONG Generation;
    ULONG N;                            /* \\.\USBMTP<n> */
    USHORT Vid, Pid, BcdDevice;
    UCHAR Interface, Class, SubClass, Protocol;
    USHORT MaxPacketIn, MaxPacketOut, MaxPacketInt;
    UCHAR Speed;                        /* 1 full, 2 high, 3 SuperSpeed */
    UCHAR Flags;                        /* USBMTP_FLAG_* */
    UCHAR Reserved[2];
    /* the extension past the 1.0 base (offset 40): the endpoint addresses,
       which Get Device Status's parameters are checked against (7.5) */
    UCHAR EndpointIn, EndpointOut, EndpointInt;
    UCHAR Reserved2;
    /* interface 1.1 (offset 44): taken in one hold of the device lock */
    UCHAR PowerGate;                    /* USBMTP_POWER_*: the gate's reasons now */
    UCHAR Held;                         /* 0 unless draining; then bit 0 bulk IN, 1 bulk OUT, 2 control,
                                           3 interrupt, 4 WAIT_EVENT, 5 USBMTP_HELD_TAILS */
    UCHAR HeldAtVeto;                   /* Held at the last refused query */
    UCHAR Reserved3;
    USHORT Vetoes;                      /* refused queries, modulo 65536: compare with your own reading */
    USHORT Forced;                      /* sets passed down with requests out, modulo 65536 */
} USBMTP_INFO;

typedef struct USBMTP_STRING_IN {       /* 4 bytes */
    UCHAR Index;                        /* 1 to 255 */
    UCHAR Reserved;
    USHORT LangId;                      /* 0: the device's first */
} USBMTP_STRING_IN;

typedef struct USBMTP_CLAIM_INFO {      /* 16 bytes */
    ULONG TokenLow, TokenHigh;
    ULONG Generation;
    ULONG Epoch;
} USBMTP_CLAIM_INFO;

typedef struct USBMTP_CLASS_IN {        /* 12 bytes */
    ULONG Request;                      /* USBMTP_REQ_* */
    ULONG TransactionId;                /* Cancel only, else 0 */
    ULONG DataLength;
} USBMTP_CLASS_IN;

typedef struct USBMTP_WAIT_IN {         /* 8 bytes */
    ULONG What;                         /* ABORT: pipe mask; EVENTS: stop or start */
    ULONG WaitMs;                       /* 0 to 5,000 */
} USBMTP_WAIT_IN;

typedef struct USBMTP_TIMEOUTS {        /* 8 bytes */
    ULONG BulkInMs, BulkOutMs;          /* 100 to 600,000 */
} USBMTP_TIMEOUTS;

typedef struct USBMTP_EVENT {           /* 64 bytes */
    ULONG Length;
    ULONG Flags;                        /* USBMTP_EVENT_LOST */
    UCHAR Data[56];
} USBMTP_EVENT;

#include <poppack.h>

#endif
