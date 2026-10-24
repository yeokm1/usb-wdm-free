/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * CDCNET descriptor probe: a pure parser (ethernet.md sections 3.3, 5.1,
 * 5.2, 6.1-6.3). Original code written from USB 2.0 chapter 9 and the
 * USB-IF CDC 1.2 / ECM 1.2 / NCM 1.0 descriptor layouts; the RNDIS class
 * triples are those ethernet.md section 2.2 lists. Every read is bounded
 * by the caller's size; a descriptor that does not fit fails the whole
 * configuration.
 */
#include "cdcproto.h"

#define MAX_ALTS 64

typedef struct ALT {
    cdc_u32 Start, End;        /* this interface descriptor .. next one */
    cdc_u8 Number, Alt, Class, Sub, Proto, Endpoints;
} ALT;

static cdc_u16 Le16(const cdc_u8 *p) { return (cdc_u16)(p[0] | (p[1] << 8)); }

static int ControlProtocol(const ALT *a)
{
    if (a->Class == 2 && a->Sub == 0x0d && a->Proto == 0) return CDC_PROTO_NCM;
    if (a->Class == 2 && a->Sub == 0x06 && a->Proto == 0) return CDC_PROTO_ECM;
    if ((a->Class == 2 && a->Sub == 2 && a->Proto == 0xff) ||
        (a->Class == 0xe0 && a->Sub == 1 && a->Proto == 3) ||
        (a->Class == 0xef && a->Sub == 1 && a->Proto == 1) ||
        (a->Class == 0xef && a->Sub == 4 && a->Proto == 1)) return CDC_PROTO_RNDIS;
    return CDC_PROTO_NONE;
}

/* Inspect the control interface's own descriptors. */
static long Control(const cdc_u8 *b, const ALT *a, int protocol, CDC_FUNCTION *f, cdc_u32 *slave)
{
    cdc_u32 off, n, header = 0, unions = 0, ether = 0, ncm = 0, interrupts = 0;
    for (off = a->Start + b[a->Start]; off < a->End; off += n) {
        n = b[off];
        if (b[off + 1] == 0x24 && n >= 3) {
            switch (b[off + 2]) {
            case 0x00: header++; break;
            case 0x06:
                if (n < 5 || unions++) return CDC_E_CONFIG;
                if (b[off + 3] != a->Number) return CDC_E_CONFIG;
                *slave = b[off + 4]; f->HasUnion = 1;
                break;
            case 0x0f:
                if (n < 13 || ether++) return CDC_E_CONFIG;
                f->MacIndex = b[off + 3];
                f->MaxSegment = Le16(b + off + 8);
                f->McFilters = Le16(b + off + 10);
                break;
            case 0x1a:
                if (n < 6 || ncm++) return CDC_E_CONFIG;
                f->NcmCaps = b[off + 5];
                break;
            default: break;
            }
        } else if (b[off + 1] == 5) {
            cdc_u16 max = (cdc_u16)(Le16(b + off + 4) & 0x7ff);
            if ((b[off + 3] & 3) != 3 || !(b[off + 2] & 0x80) || !max || interrupts++)
                return CDC_E_CONFIG;
            f->Interrupt = b[off + 2]; f->IntMax = max; f->IntInterval = b[off + 6];
        }
    }
    if (protocol != CDC_PROTO_RNDIS) {
        /* ECM/NCM: Header, Union and Ethernet descriptors are mandatory, NCM
         * also needs its own; a frame of 1514 bytes must fit. */
        if (!header || !unions || !ether || !f->MacIndex || f->MaxSegment < CDC_ETH_MAX)
            return CDC_E_CONFIG;
        if (protocol == CDC_PROTO_NCM && !ncm) return CDC_E_CONFIG;
    } else if (!unions) {
        *slave = (cdc_u32)a->Number + 1; /* RNDIS without a Union: next interface */
    }
    return CDC_OK;
}

/* Find the data interface's alternate carrying exactly one bulk pair. */
static long DataAlt(const cdc_u8 *b, const ALT *alts, cdc_u32 count, cdc_u32 number, CDC_FUNCTION *f)
{
    cdc_u32 i, off, n;
    int found = 0, zero = 0;
    for (i = 0; i < count; i++) {
        const ALT *a = &alts[i];
        cdc_u8 in = 0, out = 0, other = 0;
        cdc_u16 inMax = 0, outMax = 0;
        if (a->Number != number) continue;
        if (a->Class != 0x0a) return CDC_E_CONFIG;
        if (a->Alt == 0 && a->Endpoints == 0) zero = 1;
        for (off = a->Start + b[a->Start]; off < a->End; off += n) {
            n = b[off];
            if (b[off + 1] != 5) continue;
            if ((b[off + 3] & 3) != 2) { other = 1; continue; }
            if (b[off + 2] & 0x80) {
                if (in) other = 1;
                in = b[off + 2]; inMax = (cdc_u16)(Le16(b + off + 4) & 0x7ff);
            } else {
                if (out) other = 1;
                out = b[off + 2]; outMax = (cdc_u16)(Le16(b + off + 4) & 0x7ff);
            }
        }
        if (!found && in && out && !other && inMax && outMax) {
            found = 1;
            f->Data = a->Number; f->DataAlt = a->Alt;
            f->In = in; f->Out = out; f->InMax = inMax; f->OutMax = outMax;
        }
    }
    if (!found) return CDC_E_CONFIG;
    f->DataAltZero = (cdc_u8)zero;
    /* ECM/NCM: alternate 0 has no endpoints (ECM 1.2 section 3.3);
     * the bulk pair lives in a later alternate. */
    if (f->Protocol != CDC_PROTO_RNDIS && (!zero || f->DataAlt == 0)) return CDC_E_CONFIG;
    return CDC_OK;
}

long CdcProbeConfig(const cdc_u8 *b, cdc_u32 size, unsigned allowMask, CDC_FUNCTION *out)
{
    ALT alts[MAX_ALTS];
    cdc_u32 count = 0, off, n, i;
    long result = CDC_E_UNSUPPORTED;
    int best = CDC_PROTO_NONE;
    if (!b || size < 9 || b[0] < 9 || b[1] != 2 || Le16(b + 2) != size)
        return CDC_E_MALFORMED;
    for (off = b[0]; off < size; off += n) {
        if (size - off < 2) return CDC_E_MALFORMED;
        n = b[off];
        if (n < 2 || n > size - off) return CDC_E_MALFORMED;
        switch (b[off + 1]) {
        case 4:
            if (n < 9) return CDC_E_MALFORMED;
            if (count) alts[count - 1].End = off;
            if (count == MAX_ALTS) return CDC_E_UNSUPPORTED;
            alts[count].Start = off; alts[count].End = size;
            alts[count].Number = b[off + 2]; alts[count].Alt = b[off + 3];
            alts[count].Endpoints = b[off + 4]; alts[count].Class = b[off + 5];
            alts[count].Sub = b[off + 6]; alts[count].Proto = b[off + 7];
            for (i = 0; i < count; i++)
                if (alts[i].Number == alts[count].Number && alts[i].Alt == alts[count].Alt)
                    return CDC_E_MALFORMED;
            count++;
            break;
        case 5:
            if (n < 7 || !count) return CDC_E_MALFORMED;
            break;
        case 0x0b: /* an IAD ends the previous interface's descriptors */
            if (n < 8) return CDC_E_MALFORMED;
            if (count) alts[count - 1].End = off;
            break;
        default: break;
        }
    }
    for (i = 0; i < count; i++) {
        CDC_FUNCTION f;
        cdc_u32 slave = 256, k;
        long status;
        int protocol = ControlProtocol(&alts[i]);
        if (protocol == CDC_PROTO_NONE || alts[i].Alt != 0) continue;
        if (!(allowMask & CDC_ALLOW(protocol)) || protocol <= best) continue;
        for (k = 0; k < sizeof(f); k++) ((cdc_u8 *)&f)[k] = 0;
        f.Protocol = (cdc_u8)protocol; f.ConfigValue = b[5];
        f.Control = alts[i].Number; f.NumInterfaces = b[4];
        status = Control(b, &alts[i], protocol, &f, &slave);
        if (status == CDC_OK && (slave > 255 || slave == f.Control)) status = CDC_E_CONFIG;
        /* The notification endpoint is optional for every protocol: link
         * state then stays as last known, RNDIS responses are polled. */
        if (status == CDC_OK) status = DataAlt(b, alts, count, slave, &f);
        if (status == CDC_OK) {
            *out = f; best = protocol; result = CDC_OK;
        } else if (result != CDC_OK) {
            result = status;
        }
    }
    return result;
}

static int Hex(cdc_u16 c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

long CdcParseMacString(const cdc_u8 *s, cdc_u32 size, cdc_u8 mac[6])
{
    cdc_u32 i;
    int any = 0, all = 1;
    if (!s || size < 26 || s[0] != 26 || s[1] != 3) return CDC_E_MALFORMED;
    for (i = 0; i < 6; i++) {
        int hi = Hex(Le16(s + 2 + 4 * i)), lo = Hex(Le16(s + 4 + 4 * i));
        if (hi < 0 || lo < 0) return CDC_E_MALFORMED;
        mac[i] = (cdc_u8)((hi << 4) | lo);
        if (mac[i]) any = 1;
        if (mac[i] != 0xff) all = 0;
    }
    if (!any || all || (mac[0] & 1)) return CDC_E_CONFIG;
    return CDC_OK;
}

long CdcNotifyNext(const cdc_u8 *b, cdc_u32 size, cdc_u32 *offset, CDC_NOTIFY *n)
{
    cdc_u32 at = *offset, left;
    if (at >= size) { *offset = size; return 0; }
    left = size - at;
    if (left < 8 || b[at] != 0xa1) { *offset = size; return CDC_E_MALFORMED; }
    n->Code = b[at + 1];
    n->Value = (cdc_u16)(b[at + 2] | (b[at + 3] << 8));
    n->Index = (cdc_u16)(b[at + 4] | (b[at + 5] << 8));
    n->Length = (cdc_u16)(b[at + 6] | (b[at + 7] << 8));
    if (n->Length > left - 8) { *offset = size; return CDC_E_MALFORMED; }
    n->Data = b + at + 8;
    *offset = at + 8 + n->Length;
    return 1;
}

cdc_u32 CdcNotifyFeed(CDC_NOTIFY_STATE *s, const cdc_u8 *b, cdc_u32 size, int fullPacket,
                      CDC_NOTIFY *out, cdc_u32 max)
{
    cdc_u32 total, offset = 0, count = 0, i, before;
    long r;
    if (size > CDC_NOTIFY_BUF - s->Held) {      /* longer than any interrupt transfer here */
        s->Held = 0;
        s->Truncated++;
        return 0;
    }
    for (i = 0; i < s->Held; i++) s->Buf[i] = s->Hold[i];
    for (i = 0; i < size; i++) s->Buf[s->Held + i] = b[i];
    total = s->Held + size;
    s->Held = 0;
    for (;;) {
        before = offset;
        if (count == max) {
            if (offset < total) s->Truncated++;   /* more than the caller has room for */
            break;
        }
        r = CdcNotifyNext(s->Buf, total, &offset, &out[count]);
        if (r == 0) break;
        if (r > 0) { count++; continue; }
        /* A truncated tail: keep it when a full packet may continue it.
         * It goes to Hold, so the notifications already in out[] (which
         * point into Buf) stay intact. */
        if (fullPacket && total - before < CDC_NOTIFY_HELD && s->Buf[before] == 0xa1) {
            for (i = 0; i < total - before; i++) s->Hold[i] = s->Buf[before + i];
            s->Held = total - before;
        } else {
            s->Truncated++;
        }
        break;
    }
    return count;
}
