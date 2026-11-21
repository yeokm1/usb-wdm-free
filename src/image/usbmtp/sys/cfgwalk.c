/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cfgwalk.c - the bounded walks of a device descriptor and a configuration
 * descriptor (cfgwalk.h, docs/design/mtp.md 3.3). Every byte is read
 * through AT(), and every record's bLength is checked against what remains
 * before any of its fields is read. Every count is an int, checked against
 * its limit before it is raised. cfgtest.c compiles this file with an AT()
 * that fails the test on any index outside the buffer.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include "cfgwalk.h"

#ifndef CFG_AT
#define CFG_AT(p, len, i) ((p)[i])
#endif
#define AT(i) CFG_AT(Cfg, Len, (i))

#define MAX_IFACES 64       /* interface descriptors (alternates included) in a configuration */
#define MAX_IFACE_EPS 30    /* endpoints of one interface setting: 15 IN and 15 OUT (USB 2.0 9.6.6) */
#define MAX_CFG_EPS 512     /* endpoint records in a whole configuration, every setting counted */

static int FullBulk(unsigned short Mps)
{
    return Mps == 8 || Mps == 16 || Mps == 32 || Mps == 64;
}

int DevCheck(const unsigned char *Cfg, unsigned long Len, int *Super)
{
    unsigned char mps0;
    unsigned short bcd;
    if (Super) *Super = 0;
    if (!Cfg || Len != 18) return CW_SHORT;
    if (AT(0) != 18 || AT(1) != 1) return CW_HEADER;
    mps0 = AT(7);
    bcd = (unsigned short)(AT(2) | (AT(3) << 8));
    if (!AT(17)) return CW_DEVICE;          /* bNumConfigurations */
    /* USB 3.2 9.6.1: at SuperSpeed bMaxPacketSize0 is 09h (2^9 = 512) and
       bcdUSB 0300h or more; at the other speeds it is 8, 16, 32 or 64. The
       pair is what the device states about the speed it runs at; nothing
       is inferred from packet sizes. */
    if (mps0 == 9 && bcd >= 0x0300) {
        if (Super) *Super = 1;
        return CW_OK;
    }
    if (mps0 != 8 && mps0 != 16 && mps0 != 32 && mps0 != 64) return CW_DEVICE;
    return CW_OK;
}

/* An endpoint's size rule at the device's speed (USB 2.0 5.7.3, 5.8.3 and
   table 9-14; USB 3.2 9.6.6, 9.6.7). Bulk: 1,024 at SuperSpeed, 512 at
   high speed, 8 to 64 at full speed. Interrupt: 1 to 1,024 at SuperSpeed
   (1,024 when the companion's bMaxBurst is more than 0); at high speed 1 to
   1,024, with one or two additional transactions only for 513 to 1,024 or
   683 to 1,024 bytes; 1 to 64 at full speed. Reserved bits refused. */
static int SizeOk(int Kind, unsigned short Raw, int Speed, unsigned char Burst, unsigned short Bpi)
{
    unsigned short size = (unsigned short)(Raw & 0x7FF), mult = (unsigned short)((Raw >> 11) & 3);
    if (Raw & 0xE000) return 0;
    if (Speed == CS_SUPER) {
        if (mult) return 0;
        if (Kind == 2) return size == 1024 && Burst <= 15;     /* wBytesPerInterval: reserved for bulk */
        /* interrupt: wBytesPerInterval 1 to the packet size times (bMaxBurst + 1) */
        return size >= 1 && size <= 1024 && Burst <= 2 && (!Burst || size == 1024) &&
               Bpi >= 1 && Bpi <= (unsigned long)size * (Burst + 1);
    }
    if (Kind == 2) {
        if (mult) return 0;
        return Speed == CS_HIGH ? size == 512 : FullBulk(size);
    }
    if (Speed == CS_FULL) return !mult && size >= 1 && size <= 64;
    if (mult == 3) return 0;
    if (mult == 1) return size >= 513 && size <= 1024;
    if (mult == 2) return size >= 683 && size <= 1024;
    return size >= 1 && size <= 1024;
}

/* bInterval of the interrupt endpoint (USB 2.0 9.6.6, USB 3.2 9.6.6):
   1 to 255 frames at full speed, 1 to 16 (2^(n-1) microframes or bus
   intervals) at high speed and SuperSpeed. */
static int IntervalOk(unsigned char Interval, int Speed)
{
    return Interval >= 1 && (Speed == CS_FULL || Interval <= 16);
}

int CfgWalk(const unsigned char *Cfg, unsigned long Len, int Node, int Super, unsigned char Mps0, CFG_PICK *Out)
{
    unsigned long at;
    unsigned char num[MAX_IFACES], alt[MAX_IFACES], hasAlt0[32], anyAlt[32];
    unsigned long addrSeen = 0;     /* the owned interface's endpoint addresses: bit (dir * 16 + number) */
    int nifs = 0, alt0 = 0, found = 0, inOwned = 0, inAny = 0, i, speed;
    int want = 0, ownWant = 0, eps = 0, cfgEps = 0, nin = 0, nout = 0, nint = 0, comps = 0;
    int lastKind = 0, needComp = 0;
    unsigned short rawIn = 0, rawOut = 0, rawInt = 0;
    unsigned char burstIn = 0, burstOut = 0, burstInt = 0, intervalInt = 0;
    unsigned short bpiInt = 0;
    for (i = 0; i < 32; i++) hasAlt0[i] = anyAlt[i] = 0;
    if (!Cfg || !Out || Len < 9) return CW_SHORT;
    if (AT(0) < 9 || AT(0) > Len || AT(1) != 2 || (unsigned long)(AT(2) | (AT(3) << 8)) != Len || !AT(4))
        return CW_HEADER;
    /* USB 2.0 9.6.3: bConfigurationValue is what SET_CONFIGURATION sends,
       and 0 there means "not configured"; bmAttributes has bit 7 set and
       bits 4 to 0 reserved (zero). The selection carries both to USBD. */
    if (!AT(5) || !(AT(7) & 0x80) || (AT(7) & 0x1F)) return CW_CONFIG;
    Out->NumInterfaces = AT(4);
    for (at = AT(0); at < Len; at += AT(at)) {
        unsigned char bl, type;
        if (Len - at < 2) return CW_RECORD;
        bl = AT(at);
        type = AT(at + 1);
        if (bl < 2 || bl > Len - at) return CW_RECORD;
        if (type == 2) return CW_NESTED;
        /* At SuperSpeed every endpoint of the owned interface is followed
           at once by its companion (type 30h, 6 bytes); below SuperSpeed
           the owned interface has none. Other interfaces' companions are
           skipped like any other record. */
        if (inOwned && needComp && type != 0x30) return CW_SPEED;
        if (type == 0x30 && inOwned) {
            unsigned char burst;
            if (!Super || !needComp) return CW_SPEED;
            if (bl != 6) return CW_TOO_SHORT;
            needComp = 0;
            comps++;
            burst = AT(at + 2);
            /* bmAttributes: a bulk endpoint's MaxStreams (bits 4:0) is
               ignored, since no stream is ever opened (98 SE's USBD has no
               streams); an interrupt endpoint's must be 0. */
            if (lastKind == 3 && AT(at + 3)) return CW_SPEED;
            if (lastKind == 1) burstIn = burst;
            else if (lastKind == 2) burstOut = burst;
            else {
                burstInt = burst;
                bpiInt = (unsigned short)(AT(at + 4) | (AT(at + 5) << 8));
            }
            continue;
        }
        if (type == 4) {
            unsigned char n, a;
            if (bl < 9) return CW_TOO_SHORT;
            n = AT(at + 2);
            a = AT(at + 3);
            /* the previous interface's endpoint count, whichever it was */
            if (inAny && eps != want) return CW_ENDPOINTS;
            inAny = 1;
            eps = 0;
            want = AT(at + 4);
            if (want > MAX_IFACE_EPS) return CW_ENDPOINTS;
            inOwned = 0;
            /* each (number, alternate) at most once */
            for (i = 0; i < nifs; i++) if (num[i] == n && alt[i] == a) return CW_COUNT;
            if (nifs == MAX_IFACES) return CW_COUNT;
            num[nifs] = n;
            alt[nifs] = a;
            nifs++;
            anyAlt[n >> 3] |= (unsigned char)(1 << (n & 7));
            if (a) continue;
            hasAlt0[n >> 3] |= (unsigned char)(1 << (n & 7));
            alt0++;
            if (!found && (Node < 0 ? alt0 == 1 : n == (unsigned)Node)) {
                found = 1;
                inOwned = 1;
                ownWant = want;
                Out->IfOffset = at;
                Out->Interface = n;
                Out->Class = AT(at + 5);
                Out->SubClass = AT(at + 6);
                Out->Protocol = AT(at + 7);
            }
        } else if (type == 5) {
            unsigned char addr, kind, attr;
            unsigned short raw;
            unsigned long bit;
            if (bl < 7) return CW_TOO_SHORT;
            if (!inAny) return CW_STRAY;
            /* limits checked before any count is raised */
            if (eps >= MAX_IFACE_EPS || eps >= want || cfgEps >= MAX_CFG_EPS) return CW_ENDPOINTS;
            eps++;
            cfgEps++;
            if (!inOwned) continue;
            if (eps > 3) return CW_ENDPOINTS;       /* the owned interface has exactly three */
            addr = AT(at + 2);
            attr = AT(at + 3);
            kind = (unsigned char)(attr & 3);
            /* bmAttributes' reserved bits, for the two types accepted (they go
               to USBD in the selection): bulk uses bits 1..0 alone (USB 2.0
               table 9-13, USB 3.2 table 9-26); interrupt also bits 5..4 at
               SuperSpeed, the usage type, 00b periodic or 01b notification
               (USB 3.2 table 9-26), and bits 1..0 alone below it. Bits 7..6
               are reserved for both. */
            if ((attr & 0xC0) || (kind == 2 && (attr & 0x3C)) ||
                (kind == 3 && ((attr & 0x0C) || ((attr >> 4) & 3) > (Super ? 1 : 0))))
                return CW_ATTRIBUTES;
            raw = (unsigned short)(AT(at + 4) | (AT(at + 5) << 8));
            if (!(addr & 0x0F) || (addr & 0x70)) return CW_ADDRESS;   /* endpoint 0, reserved bits */
            bit = 1UL << (((addr & 0x80) ? 16 : 0) + (addr & 0x0F));
            if (addrSeen & bit) return CW_ADDRESS;
            addrSeen |= bit;
            if (kind == 2 && (addr & 0x80)) {
                nin++;
                lastKind = 1;
                Out->EpIn = addr;
                rawIn = raw;
            } else if (kind == 2) {
                nout++;
                lastKind = 2;
                Out->EpOut = addr;
                rawOut = raw;
            } else if (kind == 3 && (addr & 0x80)) {
                nint++;
                lastKind = 3;
                Out->EpInt = addr;
                rawInt = raw;
                intervalInt = AT(at + 6);
            } else {
                return CW_ENDPOINTS;
            }
            needComp = Super;
        }
        /* any other descriptor (class-specific and the like): skipped by its bLength */
    }
    if (inOwned && needComp) return CW_SPEED;   /* the last endpoint's companion missing */
    if (inAny && eps != want) return CW_ENDPOINTS;
    if (alt0 != AT(4)) return CW_COUNT;
    for (i = 0; i < 32; i++) if (anyAlt[i] & ~hasAlt0[i]) return CW_COUNT;     /* an interface with no default */
    if (Node < 0 && AT(4) != 1) return CW_WHOLE;
    if (!found) return CW_ABSENT;
    if (!((Out->Class == 6 && Out->SubClass == 1 && Out->Protocol == 1) || Out->Class == 0xFF)) return CW_CLASS;
    if (ownWant != 3 || nin != 1 || nout != 1 || nint != 1) return CW_ENDPOINTS;
    if (Super && comps != 3) return CW_SPEED;
    /* The speed: SuperSpeed as the device descriptor states it; below it,
       high speed exactly when a bulk endpoint is 512 (the only size high
       speed allows, and one full speed does not). */
    speed = Super ? CS_SUPER : (rawIn & 0x7FF) == 512 || (rawOut & 0x7FF) == 512 ? CS_HIGH : CS_FULL;
    if (!SizeOk(2, rawIn, speed, burstIn, 0) || !SizeOk(2, rawOut, speed, burstOut, 0) ||
        !SizeOk(3, rawInt, speed, burstInt, bpiInt))
        return CW_PACKET;
    if (!IntervalOk(intervalInt, speed)) return CW_INTERVAL;
    /* The default endpoint's size from the device descriptor, at the speed
       the walk established (USB 2.0 5.5.3, USB 3.2 9.6.1): 64 at high
       speed, 8 to 64 at full speed, 09h at SuperSpeed. */
    if (speed == CS_HIGH ? Mps0 != 64 : speed == CS_FULL ? (Mps0 != 8 && Mps0 != 16 && Mps0 != 32 && Mps0 != 64)
                                                          : Mps0 != 9)
        return CW_DEVICE;
    Out->Speed = (unsigned char)speed;
    Out->MpsIn = (unsigned short)(rawIn & 0x7FF);
    Out->MpsOut = (unsigned short)(rawOut & 0x7FF);
    Out->MpsInt = (unsigned short)(rawInt & 0x7FF);
    Out->MultInt = (unsigned char)((rawInt >> 11) & 3);
    /* the interrupt endpoint's bytes per interval: at SuperSpeed the
       companion's wBytesPerInterval, else the packet size times its
       transactions per microframe */
    Out->IntBytes = (unsigned short)(speed == CS_SUPER ? bpiInt : Out->MpsInt * (Out->MultInt + 1));
    Out->IntervalInt = intervalInt;
    return CW_OK;
}
