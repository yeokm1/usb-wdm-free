/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * The stream-format model the USB Audio 1.0 parser (uac1desc.c) fills;
 * uac1model.c puts it in the shared model (src/audio/common/uacdesc.h)
 * the driver runs on (docs/design/uac2.md section 26).
 * No Windows types, allocation, packed structures or unaligned reads.
 */
#ifndef UACFMT_H
#define UACFMT_H
#ifdef __cplusplus
extern "C" {
#endif

#define UAC_FMT_MAX_ALTS 64
#define UAC_FMT_MAX_RATES 32

/* Sync: the data endpoint's bmAttributes bits 3..2. */
#define UAC_SYNC_NONE 0
#define UAC_SYNC_ASYNC 1
#define UAC_SYNC_ADAPTIVE 2
#define UAC_SYNC_SYNC 3

/* EpControls: UAC 1.0 class-specific endpoint bmAttributes (ADC-1 Table
 * 4-21). UAC 2.0 maps its pitch control to bit 1 and its MaxPacketsOnly
 * to bit 7; it has no endpoint rate control (the rate is the clock's). */
#define UAC_EP_SAMFREQ 0x01
#define UAC_EP_PITCH 0x02
#define UAC_EP_MAXPACKETSONLY 0x80

typedef struct UAC_FMT_ALT {
    unsigned char Interface, Alternate, Terminal, Capture;
    unsigned char Channels, Subframe, Bits, Sync;
    unsigned short FormatTag;           /* UAC 1.0 wFormatTag; UAC 2.0 bmFormats bit + 1 */
    unsigned char DataEndpoint, Attributes, Interval;
    unsigned char SyncEndpoint, SyncInterval, Refresh;
    unsigned short MaxPacket, SyncMaxPacket; /* the encoded wMaxPacketSize */
    unsigned char EpControls, LockUnits;
    unsigned short LockDelay;
    unsigned char RateCount;            /* 0: continuous RateMin..RateMax */
    unsigned long RateMin, RateMax;     /* also the bounds of a discrete set */
    unsigned long Rate[UAC_FMT_MAX_RATES];
} UAC_FMT_ALT;

/* Payload bytes one service interval can carry: Full Speed bits 10..0;
 * High Speed bits 10..0 times (bits 12..11) + 1 transactions. 0 when the
 * encoding is illegal for the speed (USB 2.0 9.6.6, 5.6.3). */
unsigned int UacFmtCapacity(unsigned int maxPacket, int highSpeed);

/* Whether the alternate lists the rate (discrete) or contains it
 * (continuous, inclusive bounds). */
int UacFmtHasRate(const UAC_FMT_ALT *alt, unsigned long rate);

/* The largest packet, in bytes, the stream may need in one service
 * interval at this rate: ceil(rate * period / base) frames, plus one frame
 * for an asynchronous endpoint, whose rate may run above nominal. 0 for an
 * invalid interval. */
unsigned int UacFmtNeed(const UAC_FMT_ALT *alt, unsigned long rate, int highSpeed);

/* The index of the alternate that matches (capture, format tag, channels,
 * subframe, bits, rate) and whose capacity is the smallest that carries
 * UacFmtNeed; -1 when none does. Ties keep descriptor order. */
int UacFmtPick(const UAC_FMT_ALT *alts, unsigned int count, int capture,
               unsigned int formatTag, unsigned int channels, unsigned int subframe,
               unsigned int bits, unsigned long rate, int highSpeed);

#ifdef __cplusplus
}
#endif
#endif
