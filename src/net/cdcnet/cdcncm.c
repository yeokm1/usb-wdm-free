/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * CDCNET CDC-NCM NTB16 framing: a pure parser and builder (ethernet.md
 * section 6.2). Original code written from the USB-IF NCM 1.0 NTB16
 * layout (NTH16, NDP16, GET_NTB_PARAMETERS); parameter names and the
 * power-of-two/minimum rules were cross-checked against Linux
 * include/uapi/linux/usb/cdc.h and drivers/net/usb/cdc_ncm.c at
 * f47d2236, whose code is not copied. Every offset is checked against
 * the block before use, by subtraction, so no sum can wrap.
 */
#include "cdcproto.h"

static cdc_u16 Le16(const cdc_u8 *p) { return (cdc_u16)(p[0] | (p[1] << 8)); }
static cdc_u32 Le32(const cdc_u8 *p)
{
    return (cdc_u32)p[0] | ((cdc_u32)p[1] << 8) | ((cdc_u32)p[2] << 16) | ((cdc_u32)p[3] << 24);
}
static void Put16(cdc_u8 *p, cdc_u32 v) { p[0] = (cdc_u8)v; p[1] = (cdc_u8)(v >> 8); }
static void Put32(cdc_u8 *p, cdc_u32 v) { Put16(p, v); Put16(p + 2, v >> 16); }
static int Pow2(cdc_u32 v) { return v && !(v & (v - 1)); }

#define NCM_MIN_NTB 2048 /* NCM 1.0: dwNtbInMaxSize/dwNtbOutMaxSize minimum */

long CdcNcmParams(const cdc_u8 *b, cdc_u32 size, cdc_u32 inLimit, cdc_u32 outLimit, NCM_PARAMS *p)
{
    if (!b || size < 28 || Le16(b) < 28) return CDC_E_MALFORMED;
    p->Formats = Le16(b + 2);
    p->InMaxSize = Le32(b + 4);
    p->InDivisor = Le16(b + 8);
    p->InRemainder = Le16(b + 10);
    p->InAlignment = Le16(b + 12);
    p->OutMaxSize = Le32(b + 16);
    p->OutDivisor = Le16(b + 20);
    p->OutRemainder = Le16(b + 22);
    p->OutAlignment = Le16(b + 24);
    p->OutMaxDatagrams = Le16(b + 26);
    if (!(p->Formats & 1)) return CDC_E_UNSUPPORTED;            /* NTB16 required */
    if (p->InMaxSize < NCM_MIN_NTB || p->OutMaxSize < NCM_MIN_NTB) return CDC_E_CONFIG;
    if (inLimit < NCM_MIN_NTB || outLimit < NCM_MIN_NTB) return CDC_E_SPACE;
    if (p->InMaxSize > inLimit) p->InMaxSize = inLimit;
    if (p->OutMaxSize > outLimit) p->OutMaxSize = outLimit;
    if (p->InMaxSize > 0xffff) p->InMaxSize = 0xffff;   /* NTB16 block length */
    if (p->OutMaxSize > 0xffff) p->OutMaxSize = 0xffff;
    /* Device-relative "Out" constrains our transmit layout. A divisor or
     * alignment that is not a power of two, or is absurd, falls back to
     * 4, the NCM minimum; the remainder must be below its divisor. */
    if (!Pow2(p->OutDivisor) || p->OutDivisor < 4 || p->OutDivisor > 512) p->OutDivisor = 4;
    if (p->OutRemainder >= p->OutDivisor) p->OutRemainder = 0;
    if (!Pow2(p->OutAlignment) || p->OutAlignment < 4 || p->OutAlignment > 512) p->OutAlignment = 4;
    if (!p->OutMaxDatagrams || p->OutMaxDatagrams > NCM_TX_DATAGRAMS) p->OutMaxDatagrams = NCM_TX_DATAGRAMS;
    return CDC_OK;
}

/* The one validation walk; emit is called only on the second walk. */
static long Walk(const cdc_u8 *b, cdc_u32 size, CDC_EMIT emit, void *context)
{
    cdc_u32 limit, ndp, ndps[NCM_MAX_NDPS], ndpLen[NCM_MAX_NDPS], count = 0, datagrams = 0, i;
    if (!b || size < 12 || Le32(b) != NCM_NTH16_SIG || Le16(b + 4) != 12) return CDC_E_MALFORMED;
    limit = Le16(b + 8);
    if (limit < 12 + 16 || limit > size) return CDC_E_MALFORMED;
    ndp = Le16(b + 10);
    while (ndp) {
        cdc_u32 len, entries, e;
        if (count == NCM_MAX_NDPS || ndp < 12 || (ndp & 3) || ndp > limit - 16) return CDC_E_MALFORMED;
        for (i = 0; i < count; i++) if (ndps[i] == ndp) return CDC_E_MALFORMED; /* cycle */
        if (Le32(b + ndp) != NCM_NDP16_SIG) return CDC_E_MALFORMED;            /* incl. CRC */
        len = Le16(b + ndp + 4);
        if (len < 16 || (len & 3) || len > limit - ndp) return CDC_E_MALFORMED;
        /* No two tables overlap. */
        for (i = 0; i < count; i++)
            if (ndp < ndps[i] + ndpLen[i] && ndps[i] < ndp + len) return CDC_E_MALFORMED;
        ndps[count] = ndp; ndpLen[count] = len; count++;
        entries = (len - 8) / 4;
        for (e = 0; e < entries; e++) {
            cdc_u32 at = Le16(b + ndp + 8 + 4 * e), n = Le16(b + ndp + 10 + 4 * e);
            if (!at || !n) break;                     /* the terminating entry */
            if (at < 12 || at > limit || n > limit - at) return CDC_E_MALFORMED;
            if (n < CDC_ETH_MIN || n > CDC_ETH_MAX) return CDC_E_MALFORMED;
            if (++datagrams > NCM_MAX_DATAGRAMS) return CDC_E_MALFORMED;
            if (emit) emit(context, b + at, n);
        }
        if (e == entries) return CDC_E_MALFORMED;     /* no terminator */
        ndp = Le16(b + ndp + 6);
    }
    if (!count) return CDC_E_MALFORMED;
    /* Payload never overlaps the tables: checked in a second pass over the
     * recorded tables so a later table cannot hide under an earlier frame. */
    if (!emit) {
        cdc_u32 t;
        for (t = 0; t < count; t++) {
            cdc_u32 e, entries = (ndpLen[t] - 8) / 4;
            for (e = 0; e < entries; e++) {
                cdc_u32 at = Le16(b + ndps[t] + 8 + 4 * e), n = Le16(b + ndps[t] + 10 + 4 * e);
                if (!at || !n) break;
                for (i = 0; i < count; i++)
                    if (at < ndps[i] + ndpLen[i] && ndps[i] < at + n) return CDC_E_MALFORMED;
            }
        }
    }
    return (long)datagrams;
}

long CdcNcmRxParse(const cdc_u8 *b, cdc_u32 size, CDC_EMIT emit, void *context)
{
    long status = Walk(b, size, 0, 0);
    if (status < 0 || !emit) return status;
    return Walk(b, size, emit, context);
}

static cdc_u32 AlignUp(cdc_u32 v, cdc_u32 a) { return (v + a - 1) & ~(a - 1); }

cdc_u32 CdcNcmTxPlan(const NCM_PARAMS *p, const cdc_u32 *lengths, cdc_u32 count,
                     cdc_u32 capacity, cdc_u32 *offsets, cdc_u32 *ndp, cdc_u32 *total)
{
    cdc_u32 max = count, i, at, used = 0;
    *total = 0;
    if (capacity > p->OutMaxSize) capacity = p->OutMaxSize;
    if (max > p->OutMaxDatagrams) max = p->OutMaxDatagrams;
    if (!max) return 0;
    *ndp = AlignUp(12, p->OutAlignment);
    at = *ndp + 8 + 4 * (max + 1);
    for (i = 0; i < max; i++) {
        cdc_u32 start = at - (at % p->OutDivisor) + p->OutRemainder;
        if (start < at) start += p->OutDivisor;
        if (lengths[i] < CDC_ETH_MIN || lengths[i] > CDC_ETH_MAX) break;
        if (start > capacity || lengths[i] > capacity - start) break;
        offsets[i] = start; at = start + lengths[i]; used++;
    }
    if (used) *total = at;
    return used;
}

void CdcNcmTxFinish(cdc_u8 *b, cdc_u16 sequence, const cdc_u32 *lengths,
                    const cdc_u32 *offsets, cdc_u32 count, cdc_u32 ndp, cdc_u32 total)
{
    cdc_u32 i, at, len = 8 + 4 * (count + 1);
    Put32(b, NCM_NTH16_SIG); Put16(b + 4, 12); Put16(b + 6, sequence);
    Put16(b + 8, total); Put16(b + 10, ndp);
    for (at = 12; at < ndp; at++) b[at] = 0;
    Put32(b + ndp, NCM_NDP16_SIG); Put16(b + ndp + 4, len); Put16(b + ndp + 6, 0);
    for (i = 0; i < count; i++) {
        Put16(b + ndp + 8 + 4 * i, offsets[i]);
        Put16(b + ndp + 10 + 4 * i, lengths[i]);
    }
    Put32(b + ndp + 8 + 4 * count, 0);
    at = ndp + len;
    for (i = 0; i < count; i++) {
        while (at < offsets[i]) b[at++] = 0;
        at = offsets[i] + lengths[i];
    }
}
