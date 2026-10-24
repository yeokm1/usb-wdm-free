/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * CDCNET RNDIS message codec: a pure parser and builder (ethernet.md
 * section 6.3). Original code written from Microsoft's published
 * [MS-RNDIS] message layouts and the Microsoft Learn pages on
 * REMOTE_NDIS_PACKET_MSG and KEEPALIVE that ethernet.md cites. The
 * device controls every length and offset here: each one is checked
 * against the message and the transfer, by subtraction, before use.
 * Offsets in QUERY/SET/INDICATE/PACKET messages count from byte 8.
 */
#include "cdcproto.h"

static cdc_u32 Le32(const cdc_u8 *p)
{
    return (cdc_u32)p[0] | ((cdc_u32)p[1] << 8) | ((cdc_u32)p[2] << 16) | ((cdc_u32)p[3] << 24);
}
static void Put32(cdc_u8 *p, cdc_u32 v)
{
    p[0] = (cdc_u8)v; p[1] = (cdc_u8)(v >> 8); p[2] = (cdc_u8)(v >> 16); p[3] = (cdc_u8)(v >> 24);
}
static void Zero(cdc_u8 *p, cdc_u32 n) { while (n--) *p++ = 0; }

cdc_u32 CdcRndisInit(cdc_u8 *b, cdc_u32 id, cdc_u32 maxTransfer)
{
    Put32(b, RNDIS_MSG_INIT); Put32(b + 4, 24); Put32(b + 8, id);
    Put32(b + 12, 1); Put32(b + 16, 0); Put32(b + 20, maxTransfer);
    return 24;
}

cdc_u32 CdcRndisQuery(cdc_u8 *b, cdc_u32 capacity, cdc_u32 id, cdc_u32 oid,
                      const void *data, cdc_u32 length, int set)
{
    cdc_u32 i;
    if (capacity < 28 || length > capacity - 28) return 0;
    Put32(b, set ? RNDIS_MSG_SET : RNDIS_MSG_QUERY); Put32(b + 4, 28 + length);
    Put32(b + 8, id); Put32(b + 12, oid); Put32(b + 16, length);
    Put32(b + 20, 20); Put32(b + 24, 0);
    for (i = 0; i < length; i++) b[28 + i] = ((const cdc_u8 *)data)[i];
    return 28 + length;
}

cdc_u32 CdcRndisSimple(cdc_u8 *b, cdc_u32 type, cdc_u32 id)
{
    Put32(b, type); Put32(b + 4, 12); Put32(b + 8, type == RNDIS_MSG_RESET ? 0 : id);
    return 12;
}

cdc_u32 CdcRndisKeepaliveReply(cdc_u8 *b, cdc_u32 id)
{
    Put32(b, RNDIS_MSG_KEEPALIVE | RNDIS_COMPLETION); Put32(b + 4, 16);
    Put32(b + 8, id); Put32(b + 12, RNDIS_STATUS_SUCCESS);
    return 16;
}

/* An optional byte-8-relative buffer inside a message of length len. */
static long Buffer(const cdc_u8 *b, cdc_u32 len, cdc_u32 fixed, cdc_u32 offset, cdc_u32 length,
                   const cdc_u8 **out)
{
    *out = 0;
    if (!length) return CDC_OK;
    if (offset > len - 8 || offset + 8 < fixed || length > len - 8 - offset) return CDC_E_MALFORMED;
    *out = b + 8 + offset;
    return CDC_OK;
}

long CdcRndisParseReply(const cdc_u8 *b, cdc_u32 size, RNDIS_REPLY *r)
{
    cdc_u32 len;
    Zero((cdc_u8 *)r, sizeof(*r));
    if (!b || size < 8) return CDC_E_MALFORMED;
    r->Type = Le32(b); len = Le32(b + 4);
    if (len < 8 || len > size) return CDC_E_MALFORMED;
    switch (r->Type) {
    case RNDIS_MSG_INIT | RNDIS_COMPLETION:
        if (len < 52) return CDC_E_MALFORMED;
        r->RequestId = Le32(b + 8); r->Status = Le32(b + 12);
        if (Le32(b + 16) != 1) return CDC_E_UNSUPPORTED;     /* major version */
        r->Medium = Le32(b + 28); r->MaxPackets = Le32(b + 32);
        r->MaxTransfer = Le32(b + 36); r->AlignShift = Le32(b + 40);
        return CDC_OK;
    case RNDIS_MSG_QUERY | RNDIS_COMPLETION:
        if (len < 24) return CDC_E_MALFORMED;
        r->RequestId = Le32(b + 8); r->Status = Le32(b + 12);
        r->InfoLength = Le32(b + 16);
        return Buffer(b, len, 24, Le32(b + 20), r->InfoLength, &r->Info);
    case RNDIS_MSG_SET | RNDIS_COMPLETION:
    case RNDIS_MSG_KEEPALIVE | RNDIS_COMPLETION:
        if (len < 16) return CDC_E_MALFORMED;
        r->RequestId = Le32(b + 8); r->Status = Le32(b + 12);
        return CDC_OK;
    case RNDIS_MSG_RESET | RNDIS_COMPLETION:
        if (len < 16) return CDC_E_MALFORMED;
        r->Status = Le32(b + 8);
        return CDC_OK;
    case RNDIS_MSG_INDICATE:
        if (len < 20) return CDC_E_MALFORMED;
        r->Status = Le32(b + 8); r->InfoLength = Le32(b + 12);
        return Buffer(b, len, 20, Le32(b + 16), r->InfoLength, &r->Info);
    case RNDIS_MSG_KEEPALIVE:
        if (len < 12) return CDC_E_MALFORMED;
        r->RequestId = Le32(b + 8);
        return CDC_OK;
    default:
        return CDC_E_UNSUPPORTED;
    }
}

#define RNDIS_RX_MAX_MESSAGES 256

static long Walk(const cdc_u8 *b, cdc_u32 size, CDC_EMIT emit, void *context)
{
    cdc_u32 off = 0, count = 0;
    if (!b) return CDC_E_MALFORMED;
    while (off < size) {
        cdc_u32 rest = size - off, len, data, dlen, i;
        const cdc_u8 *m = b + off, *unused;
        if (rest < 8 || Le32(m) == 0) {
            /* A zero tail (the one-byte short-packet terminator, or padding
             * to the transfer's end) is accepted whatever its length. */
            for (i = 0; i < rest; i++) if (m[i]) return CDC_E_MALFORMED;
            break;
        }
        if (Le32(m) != RNDIS_MSG_PACKET) return CDC_E_MALFORMED;
        len = Le32(m + 4);
        if (len < RNDIS_PACKET_HEADER || len > rest) return CDC_E_MALFORMED;
        data = Le32(m + 8); dlen = Le32(m + 12);
        if (data + 8 < RNDIS_PACKET_HEADER || data > len - 8 || dlen > len - 8 - data)
            return CDC_E_MALFORMED;
        if (dlen < CDC_ETH_MIN || dlen > CDC_ETH_MAX) return CDC_E_MALFORMED;
        /* Out-of-band data and per-packet information: bounds only. */
        if (Buffer(m, len, RNDIS_PACKET_HEADER, Le32(m + 16), Le32(m + 20), &unused) < 0 ||
            Buffer(m, len, RNDIS_PACKET_HEADER, Le32(m + 28), Le32(m + 32), &unused) < 0)
            return CDC_E_MALFORMED;
        if (++count > RNDIS_RX_MAX_MESSAGES) return CDC_E_MALFORMED;
        if (emit) emit(context, m + 8 + data, dlen);
        off += len;
    }
    return (long)count;
}

long CdcRndisRxParse(const cdc_u8 *b, cdc_u32 size, CDC_EMIT emit, void *context)
{
    long status = Walk(b, size, 0, 0);
    if (status <= 0 || !emit) return status;
    return Walk(b, size, emit, context);
}

cdc_u32 CdcRndisTxHeader(cdc_u8 *b, cdc_u32 frameLength, cdc_u32 alignShift)
{
    cdc_u32 len = RNDIS_PACKET_HEADER + frameLength, padded = len, i;
    if (alignShift > 0 && alignShift <= 7) {
        cdc_u32 a = 1UL << alignShift;
        padded = (len + a - 1) & ~(a - 1);
    }
    Zero(b, RNDIS_PACKET_HEADER);
    Put32(b, RNDIS_MSG_PACKET); Put32(b + 4, padded);
    Put32(b + 8, RNDIS_PACKET_HEADER - 8); Put32(b + 12, frameLength);
    for (i = len; i < padded; i++) b[i] = 0;
    return padded;
}
