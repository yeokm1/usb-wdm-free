/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * CDCNET pure protocol interface (ethernet.md section 3.3). Nothing here
 * includes an OS header: the descriptor probe, the NCM NTB16 framing and the
 * RNDIS message codec are compiled unchanged into the driver and into the
 * host tests. None of them allocates memory or calls NDIS or USB.
 *
 * Wire facts: USB 2.0 chapter 9; USB-IF CDC 1.2, ECM 1.2 and NCM 1.0;
 * Microsoft [MS-RNDIS]. Field names cross-checked against Linux
 * include/uapi/linux/usb/cdc.h at f47d2236 (ethernet.md section 6.2).
 */
#ifndef CDCPROTO_H
#define CDCPROTO_H

typedef unsigned char cdc_u8;
typedef unsigned short cdc_u16;
typedef unsigned long cdc_u32;

#define CDC_OK              0L
#define CDC_E_MALFORMED   (-1L)  /* a length or offset is out of bounds */
#define CDC_E_UNSUPPORTED (-2L)  /* well formed, but nothing this driver serves */
#define CDC_E_CONFIG      (-3L)  /* a CDC function with missing/contradictory parts */
#define CDC_E_SPACE       (-4L)  /* output buffer too small */

/* Protocols, in the order of preference of ethernet.md section 5.2. */
#define CDC_PROTO_NONE  0
#define CDC_PROTO_RNDIS 1
#define CDC_PROTO_ECM   2
#define CDC_PROTO_NCM   3
#define CDC_ALLOW(p) (1u << (p))

#define CDC_ETH_MIN   14
#define CDC_ETH_MAX   1514

typedef struct CDC_FUNCTION {
    cdc_u8 Protocol;           /* CDC_PROTO_* */
    cdc_u8 ConfigValue;        /* bConfigurationValue */
    cdc_u8 Control, Data;      /* interface numbers */
    cdc_u8 DataAlt;            /* alternate setting carrying the bulk pair */
    cdc_u8 DataAltZero;        /* data interface has an endpoint-less alt 0 */
    cdc_u8 In, Out, Interrupt; /* endpoint addresses; Interrupt 0 if absent */
    cdc_u8 IntInterval;
    cdc_u16 InMax, OutMax, IntMax;
    cdc_u8 MacIndex;           /* ECM/NCM iMACAddress; 0 for RNDIS */
    cdc_u8 NcmCaps;            /* NCM bmNetworkCapabilities */
    cdc_u8 HasUnion;
    cdc_u8 NumInterfaces;      /* bNumInterfaces of the configuration */
    cdc_u16 MaxSegment;        /* ECM/NCM wMaxSegmentSize */
    cdc_u16 McFilters;         /* ECM/NCM wNumberMCFilters */
} CDC_FUNCTION;

/* Find the most preferred function in one complete configuration
 * descriptor (header plus wTotalLength bytes) among the protocols in
 * allowMask. Returns CDC_OK and fills out, CDC_E_UNSUPPORTED when the
 * configuration is well formed but holds no usable function, or a
 * negative error for malformed input. */
long CdcProbeConfig(const cdc_u8 *b, cdc_u32 size, unsigned allowMask, CDC_FUNCTION *out);

/* Decode a USB string descriptor carrying 12 hex digits into a MAC.
 * Rejects zero, multicast and broadcast addresses. */
long CdcParseMacString(const cdc_u8 *s, cdc_u32 size, cdc_u8 mac[6]);

/* ---- CDC-NCM NTB16 ---- */
#define NCM_NTH16_SIG 0x484D434EUL /* "NCMH" */
#define NCM_NDP16_SIG 0x304D434EUL /* "NCM0", no CRC */
#define NCM_NDP16_CRC 0x314D434EUL /* "NCM1", CRC appended: not accepted */
#define NCM_MAX_NDPS 8
#define NCM_MAX_DATAGRAMS 256 /* per received NTB */
#define NCM_TX_DATAGRAMS 32   /* per transmitted NTB */

typedef struct NCM_PARAMS {    /* GET_NTB_PARAMETERS, 28 bytes */
    cdc_u16 Formats;
    cdc_u32 InMaxSize;
    cdc_u16 InDivisor, InRemainder, InAlignment;
    cdc_u32 OutMaxSize;
    cdc_u16 OutDivisor, OutRemainder, OutAlignment, OutMaxDatagrams;
} NCM_PARAMS;

/* Validate and normalise GET_NTB_PARAMETERS against local limits.
 * inLimit/outLimit are the host buffer sizes. */
long CdcNcmParams(const cdc_u8 *b, cdc_u32 size, cdc_u32 inLimit, cdc_u32 outLimit, NCM_PARAMS *p);

typedef void (*CDC_EMIT)(void *context, const cdc_u8 *frame, cdc_u32 length);

/* Validate one NTB16 completely, then emit each datagram. Nothing is
 * emitted from a malformed block. Returns the number of datagrams. */
long CdcNcmRxParse(const cdc_u8 *b, cdc_u32 size, CDC_EMIT emit, void *context);

/* Transmit layout: NTH16 at 0, one NDP16 right after it, datagrams after
 * the NDP at the device's divisor/remainder. Plan how many of count
 * frames fit (at least one if any fit), filling offsets. */
cdc_u32 CdcNcmTxPlan(const NCM_PARAMS *p, const cdc_u32 *lengths, cdc_u32 count,
                     cdc_u32 capacity, cdc_u32 *offsets, cdc_u32 *ndp, cdc_u32 *total);
/* Write NTH16 and NDP16 for a plan whose datagrams are already copied,
 * and zero every gap so no stale byte leaves the host. */
void CdcNcmTxFinish(cdc_u8 *b, cdc_u16 sequence, const cdc_u32 *lengths,
                    const cdc_u32 *offsets, cdc_u32 count, cdc_u32 ndp, cdc_u32 total);

/* ---- CDC notifications (CDC 1.2 section 6.3; ECM 1.2 section 6.3) ---- */
/* One notification: an 8-byte header (bmRequestType 0xA1, bNotification,
 * wValue, wIndex, wLength) and wLength data bytes. */
typedef struct CDC_NOTIFY {
    cdc_u8 Code;               /* bNotification */
    cdc_u16 Value, Index;
    cdc_u16 Length;            /* wLength */
    const cdc_u8 *Data;        /* the wLength bytes after the header */
} CDC_NOTIFY;
/* Take the notification at *offset of one interrupt completion and move
 * *offset past it: one completion may hold several notifications when a
 * full-size packet did not end the transfer. Returns 1 for a
 * notification, 0 when no byte remains, CDC_E_MALFORMED for a truncated
 * tail (fewer than 8 bytes, or wLength past the end) or a header whose
 * bmRequestType is not 0xA1; then *offset is set to size, so the walk
 * ends. Reads only b[0..size). */
long CdcNotifyNext(const cdc_u8 *b, cdc_u32 size, cdc_u32 *offset, CDC_NOTIFY *n);

/* Reassembly across completions. The interrupt transfer is one
 * wMaxPacketSize, so a notification longer than a packet (a 16-byte
 * CONNECTION_SPEED_CHANGE on an 8-byte endpoint) arrives as two full
 * packets. Feed one completion: held bytes from the previous full packet
 * come first; the whole notifications are written to out[] (at most max;
 * their Data points into s->Buf, valid until the next Feed); a truncated
 * tail of a full packet (fullPacket nonzero) shorter than 16 bytes is held
 * for the next completion, any other truncated tail is dropped and
 * counted in s->Truncated. Returns the number written. */
#define CDC_NOTIFY_HELD 16
#define CDC_NOTIFY_BUF (64 + CDC_NOTIFY_HELD)
typedef struct CDC_NOTIFY_STATE {
    cdc_u8 Buf[CDC_NOTIFY_BUF];    /* held bytes, then the completion: out[] points here */
    cdc_u8 Hold[CDC_NOTIFY_HELD];  /* a cut notification's head, apart from Buf */
    cdc_u32 Held;              /* bytes in Hold */
    cdc_u32 Truncated;         /* tails dropped */
} CDC_NOTIFY_STATE;
cdc_u32 CdcNotifyFeed(CDC_NOTIFY_STATE *s, const cdc_u8 *b, cdc_u32 size, int fullPacket,
                      CDC_NOTIFY *out, cdc_u32 max);

/* ---- RNDIS ([MS-RNDIS] section 2.2) ---- */
#define RNDIS_MSG_PACKET     0x00000001UL
#define RNDIS_MSG_INIT       0x00000002UL
#define RNDIS_MSG_HALT       0x00000003UL
#define RNDIS_MSG_QUERY      0x00000004UL
#define RNDIS_MSG_SET        0x00000005UL
#define RNDIS_MSG_RESET      0x00000006UL
#define RNDIS_MSG_INDICATE   0x00000007UL
#define RNDIS_MSG_KEEPALIVE  0x00000008UL
#define RNDIS_COMPLETION     0x80000000UL
#define RNDIS_STATUS_SUCCESS 0x00000000UL
#define RNDIS_STATUS_MEDIA_CONNECT    0x4001000BUL
#define RNDIS_STATUS_MEDIA_DISCONNECT 0x4001000CUL
#define RNDIS_PACKET_HEADER 44
#define RNDIS_CONTROL_MAX 1025

typedef struct RNDIS_REPLY {
    cdc_u32 Type, RequestId, Status;
    const cdc_u8 *Info;        /* query completion payload or status buffer */
    cdc_u32 InfoLength;
    /* INIT completion */
    cdc_u32 Medium, MaxPackets, MaxTransfer, AlignShift;
} RNDIS_REPLY;

cdc_u32 CdcRndisInit(cdc_u8 *b, cdc_u32 id, cdc_u32 maxTransfer);
cdc_u32 CdcRndisQuery(cdc_u8 *b, cdc_u32 capacity, cdc_u32 id, cdc_u32 oid,
                      const void *data, cdc_u32 length, int set);
cdc_u32 CdcRndisSimple(cdc_u8 *b, cdc_u32 type, cdc_u32 id);   /* HALT, RESET, KEEPALIVE */
cdc_u32 CdcRndisKeepaliveReply(cdc_u8 *b, cdc_u32 id);
/* Parse one control message received from GET_ENCAPSULATED_RESPONSE. */
long CdcRndisParseReply(const cdc_u8 *b, cdc_u32 size, RNDIS_REPLY *r);
/* Validate a data transfer of PACKET_MSGs (zero tail allowed), then emit. */
long CdcRndisRxParse(const cdc_u8 *b, cdc_u32 size, CDC_EMIT emit, void *context);
/* Header for one PACKET_MSG whose frame follows at b + 44; returns the
 * message length padded to 1 << alignShift. */
cdc_u32 CdcRndisTxHeader(cdc_u8 *b, cdc_u32 frameLength, cdc_u32 alignShift);

#endif
