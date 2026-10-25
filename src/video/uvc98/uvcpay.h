/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original bounded UVC payload core from USB-IF UVC1.1 section2.4.3.3.
 * No OS/USB I/O, allocation, image conversion or clock recovery.
 */
#ifndef UVCPAY_H
#define UVCPAY_H

typedef struct UVC_PAY_HEADER {
    unsigned int Length, Flags, PayloadBytes;
    unsigned long Pts, ScrClock;
    unsigned int ScrSof;
    const unsigned char *Payload;
} UVC_PAY_HEADER;

/* A successful parse preserves flags; Pts/Scr fields are meaningful only
 * when their corresponding flag is set. Unknown extension bytes are skipped.
 */
int UvcPayHeader(const unsigned char *packet, unsigned int bytes,
    UVC_PAY_HEADER *header);

typedef void (*UVC_PAY_FRAME)(void *context, const unsigned char *frame,
    unsigned int bytes, unsigned int still);

typedef struct UVC_PAY {
    unsigned char *Buffer;
    unsigned int Capacity, Expected, Used;
    unsigned int HaveFid, LastFid, Active, Bad, Still, Unsynced;
    unsigned long Frames, Dropped, InvalidPackets, ErrorPackets, Overflows;
    UVC_PAY_FRAME Deliver;
    void *Context;
    /* Diagnostics: frame boundaries seen, dropped frames by cause (bad =
     * error/loss/overflow; size = clean but Used != Expected), and the
     * assembled size of the newest dropped frame. */
    unsigned long Eofs, FidFlips, DropBad, DropSize;
    unsigned int LastDropUsed;
} UVC_PAY;

/* Caller owns buffer and serialization. Expected=0 permits variable-size
 * compressed frames; otherwise only exactly Expected bytes are delivered.
 * Callback consumes/copies synchronously: buffer is reused after it returns.
 * Callback must not reenter/reset this state. Initial partial frame is
 * discarded until a FID boundary, and EOF never delivers a frame twice.
 */
int UvcPayInit(UVC_PAY *state, unsigned char *buffer, unsigned int capacity,
    unsigned int expected, UVC_PAY_FRAME deliver, void *context);

/* Feed one complete UVC payload, not one Windows URB or bulk fragment.
 * Empty successful USB transactions carry no header and are ignored.
 * After a failed/missing USB packet call Lost before feeding more data.
 * Loss discards the first surviving FID's frame: its start may be missing.
 * No no-FID camera quirks are implemented.
 */
void UvcPayFeed(UVC_PAY *state, const unsigned char *packet, unsigned int bytes);
void UvcPayLost(UVC_PAY *state);

#endif
