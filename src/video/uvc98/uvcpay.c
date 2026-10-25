/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original implementation from UVC1.1 payload-header and frame-boundary facts.
 */
#include "uvcpay.h"

static unsigned long Read32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
        ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

int UvcPayHeader(const unsigned char *packet, unsigned int bytes,
    UVC_PAY_HEADER *header)
{
    UVC_PAY_HEADER h = { 0 };
    unsigned int needed = 2, offset = 2;
    if (!header) return 0;
    *header = h;
    if (!packet || bytes < 2 || packet[0] < 2 || packet[0] > bytes) return 0;
    h.Length = packet[0]; h.Flags = packet[1];
    if (h.Flags & 4) needed += 4;
    if (h.Flags & 8) needed += 6;
    if (h.Length < needed) return 0;
    if (h.Flags & 4) { h.Pts = Read32(packet + offset); offset += 4; }
    if (h.Flags & 8) {
        h.ScrClock = Read32(packet + offset);
        h.ScrSof = (packet[offset + 4] | ((unsigned int)packet[offset + 5] << 8)) & 0x7ff;
    }
    h.Payload = packet + h.Length; h.PayloadBytes = bytes - h.Length;
    *header = h;
    return 1;
}

static void Count(unsigned long *value)
{
    if (*value != (unsigned long)-1) ++*value;
}

int UvcPayInit(UVC_PAY *s, unsigned char *buffer, unsigned int capacity,
    unsigned int expected, UVC_PAY_FRAME deliver, void *context)
{
    UVC_PAY empty = { 0 };
    if (!s) return 0;
    *s = empty;
    if (!buffer || !capacity || expected > capacity || !deliver) return 0;
    s->Buffer = buffer; s->Capacity = capacity; s->Expected = expected;
    s->Deliver = deliver; s->Context = context;
    return 1;
}

static void Finish(UVC_PAY *s)
{
    if (!s->Active) return;
    s->Active = 0;
    if (!s->Bad && s->Used && (!s->Expected || s->Used == s->Expected)) {
        Count(&s->Frames);
        s->Deliver(s->Context, s->Buffer, s->Used, s->Still);
    } else {
        Count(&s->Dropped);
        if (s->Bad) Count(&s->DropBad); else Count(&s->DropSize);
        s->LastDropUsed = s->Used;
    }
}

void UvcPayLost(UVC_PAY *s)
{
    if (!s || !s->Buffer) return;
    if (s->Active) s->Bad = 1;
    s->Unsynced = 1;
}

void UvcPayFeed(UVC_PAY *s, const unsigned char *packet, unsigned int bytes)
{
    UVC_PAY_HEADER h;
    unsigned int fid, i;
    if (!s || !s->Buffer || !bytes) return;
    if (!UvcPayHeader(packet, bytes, &h)) {
        Count(&s->InvalidPackets); UvcPayLost(s); return;
    }
    fid = h.Flags & 1;
    if (h.Flags & 0x40) Count(&s->ErrorPackets);
    if (!s->HaveFid || s->Unsynced) {
        /* We may have attached in the middle of a frame. Never publish it. */
        Finish(s);
        s->HaveFid = 1; s->LastFid = fid; s->Unsynced = 0; return;
    }
    if (fid != s->LastFid) {
        Count(&s->FidFlips);
        Finish(s);
        s->LastFid = fid; s->Active = 1;
        s->Used = s->Bad = s->Still = 0;
    }
    if (!s->Active) return; /* EOF consumed; wait for the next FID. */
    if (h.Flags & 0x40) s->Bad = 1;
    if (h.Flags & 0x20) s->Still = 1;
    if (!s->Bad) {
        if (h.PayloadBytes > s->Capacity - s->Used) {
            Count(&s->Overflows); s->Bad = 1;
        } else {
            for (i = 0; i < h.PayloadBytes; i++)
                s->Buffer[s->Used + i] = h.Payload[i];
            s->Used += h.PayloadBytes;
        }
    }
    if (h.Flags & 2) { Count(&s->Eofs); Finish(s); }
}
