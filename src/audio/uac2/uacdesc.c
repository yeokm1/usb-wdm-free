/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original implementation. Sources and limitations: docs/testing/
 * 2026-10-21-wave1.md. No DDK samples or third-party code used.
 */
#include "uacdesc.h"

static unsigned int Word(const UAC_BYTE *p)
{
    return p[0] | ((unsigned int)p[1] << 8);
}

static void Clear(void *p, unsigned int n)
{
    UAC_BYTE *b = (UAC_BYTE *)p;
    while (n--) *b++ = 0;
}

int UacFunction(const UAC_BYTE *b, unsigned int size, unsigned int first,
                unsigned int *control, unsigned int *count)
{
    unsigned int off, n, end, iad = 0, found = 0;
    if (!b || !control || !count || first > 255 || size < 9 ||
        b[0] != 9 || b[1] != 2) return UAC_BAD_DESCRIPTOR;
    *control = 256; *count = 0;
    end = Word(b + 2);
    if (end < 9 || end > size) return UAC_BAD_DESCRIPTOR;
    /* First validate framing and see whether the parent retained the IAD. */
    for (off = 9; off < end; off += n) {
        if (end - off < 2) return UAC_BAD_DESCRIPTOR;
        n = b[off];
        if (n < 2 || n > end - off) return UAC_BAD_DESCRIPTOR;
        if (b[off + 1] == 11) {
            if (n < 8) return UAC_BAD_DESCRIPTOR;
            if (b[off + 2] == first) {
                if (iad || !b[off + 3] || b[off + 3] > 256 - first ||
                    b[off + 4] != 1 || b[off + 5] != 0 || b[off + 6] != 0x20)
                    return UAC_BAD_DESCRIPTOR;
                iad = b[off + 3];
            } else if (b[off + 2] < first &&
                       (unsigned int)b[off + 2] + b[off + 3] > first)
                return UAC_BAD_DESCRIPTOR;
        }
    }
    for (off = 9; off < end; off += b[off]) {
        const UAC_BYTE *p = b + off;
        if (p[1] != 4) continue;
        if (p[0] < 9) return UAC_BAD_DESCRIPTOR;
        if (p[3] != 0) continue;
        if (!found) {
            if (p[2] != first) continue;
            if (p[5] != 1 || p[6] != 1 || p[7] != 0x20) return UAC_NO_AUDIO;
            *control = first; *count = 1; found = 1;
        } else {
            if (p[2] != first + *count || p[5] != 1 || p[6] != 2 || p[7] != 0x20)
                break;
            (*count)++;
        }
        if (iad && *count == iad) break;
    }
    if (!found || (iad && *count != iad)) return UAC_NO_AUDIO;
    return UAC_OK;
}

/* Candidate storage is local until an entire alternate has been checked. */
typedef struct CANDIDATE {
    UAC_ALT Alt;
    unsigned int Active, General, Format, Type, Pcm;
    unsigned int Expected, Endpoints, Data, Feedback, Bad;
} CANDIDATE;

static int Finish(UAC_DESC *d, const CANDIDATE *c)
{
    unsigned int i;
    if (!c->Active) return UAC_OK;
    /* Unsupported formats disable this alternate, never the function. */
    if (!c->General || !c->Format || c->Type != 1 || !c->Pcm ||
        c->Bad || c->Endpoints != c->Expected || c->Data != 1 ||
        !c->Alt.Channels || !c->Alt.Subslot || c->Alt.Subslot > 4 ||
        !c->Alt.Bits || c->Alt.Bits > c->Alt.Subslot * 8 ||
        c->Feedback > 1 ||
        ((c->Alt.DataEndpoint & 0x80) && c->Feedback)) {
        d->Skipped++;
        return UAC_OK;
    }
    if (d->AltCount == UAC_MAX_ALTS) return UAC_TOO_MANY_ALTS;
    for (i = 0; i < d->AltCount; i++)
        if (d->Alt[i].Interface == c->Alt.Interface &&
            d->Alt[i].Alternate == c->Alt.Alternate)
            return UAC_BAD_DESCRIPTOR;
    d->Alt[d->AltCount++] = c->Alt;
    return UAC_OK;
}

int UacParse(const UAC_BYTE *b, unsigned int size,
             unsigned int control, unsigned int first, unsigned int count,
             UAC_DESC *d)
{
    unsigned int off, end, n, type, iface = 256, subclass = 0;
    unsigned int alt = 0, inScope = 0, header = 0, rc;
    unsigned int acTotal = 0, acBytes = 0;
    UAC_BYTE seen[256];
    CANDIDATE c;
    if (!d) return UAC_BAD_DESCRIPTOR;
    Clear(d, sizeof(*d));
    Clear(&c, sizeof(c));
    Clear(seen, sizeof(seen));
    if (!b || size < 9 || b[0] != 9 || b[1] != 2 ||
        !count || first > 255 || count > 256 - first ||
        control < first || control >= first + count)
        return UAC_BAD_DESCRIPTOR;
    end = Word(b + 2);
    if (end < 9 || end > size) return UAC_BAD_DESCRIPTOR;
    d->Bytes = b;
    d->Length = end;
    d->Control = (UAC_BYTE)control;
    d->First = (UAC_BYTE)first;
    /* A USB IAD count is one byte, never 256. */
    if (count > 255) return UAC_BAD_DESCRIPTOR;
    d->Count = (UAC_BYTE)count;
    for (off = 9; off < end; off += n) {
        if (end - off < 2) return UAC_BAD_DESCRIPTOR;
        n = b[off]; type = b[off + 1];
        if (n < 2 || n > end - off) return UAC_BAD_DESCRIPTOR;
        if (type == 11) {
            if (n < 8) return UAC_BAD_DESCRIPTOR;
            /* An overlapping IAD must describe this exact UAC2 function. */
            if (b[off + 2] < first + count &&
                (unsigned int)b[off + 2] + b[off + 3] > first) {
                if (b[off + 2] != first || b[off + 3] != count ||
                    b[off + 4] != 1 || b[off + 5] != 0 ||
                    b[off + 6] != 0x20) return UAC_BAD_DESCRIPTOR;
            }
        } else if (type == 4) {
            rc = Finish(d, &c);
            if (rc) return rc;
            Clear(&c, sizeof(c));
            if (n < 9) return UAC_BAD_DESCRIPTOR;
            iface = b[off + 2]; alt = b[off + 3];
            inScope = iface >= first && iface < first + count;
            subclass = b[off + 6];
            if (!inScope) continue;
            if (b[off + 5] != 1 || b[off + 7] != 0x20)
                return UAC_BAD_DESCRIPTOR;
            if (iface == control && (subclass != 1 || alt != 0))
                return UAC_BAD_DESCRIPTOR;
            if (subclass == 1 && iface != control)
                return UAC_BAD_DESCRIPTOR;
            /* Each interface starts with alternate zero. */
            if (!alt) {
                if (seen[iface]) return UAC_BAD_DESCRIPTOR;
                seen[iface] = 1;
            } else if (!seen[iface]) return UAC_BAD_DESCRIPTOR;
            c.Active = subclass == 2 && alt != 0;
            c.Expected = b[off + 4];
            c.Alt.Interface = (UAC_BYTE)iface;
            c.Alt.Alternate = (UAC_BYTE)alt;
        } else if (type == 0x24 && inScope) {
            unsigned int sub;
            if (n < 3) return UAC_BAD_DESCRIPTOR;
            sub = b[off + 2];
            if (subclass == 1) {
                unsigned int id;
                acBytes += n;
                if (sub == 1) {
                    if (n < 9 || Word(b + off + 3) != 0x0200 || header++)
                        return UAC_BAD_DESCRIPTOR;
                    /* UAC2 Table 4-5: this subtotal includes the header
                     * and AC entities, not the standard interrupt EP. */
                    acTotal = Word(b + off + 6);
                    if (acTotal < n || acTotal > end - off)
                        return UAC_BAD_DESCRIPTOR;
                    continue;
                }
                if (!header) return UAC_BAD_DESCRIPTOR;
                if (n < 4) return UAC_BAD_DESCRIPTOR;
                id = b[off + 3];
                if (!id || d->Entity[id]) return UAC_BAD_DESCRIPTOR;
                /* Entity-specific minimums before any field can be read. */
                if ((sub == 2 && n < 17) || (sub == 3 && n < 12) ||
                    (sub == 10 && n < 8) || (sub == 12 && n < 7))
                    return UAC_BAD_DESCRIPTOR;
                if (sub == 11 && (n < 7 || !b[off + 4] ||
                    n < 7U + b[off + 4])) return UAC_BAD_DESCRIPTOR;
                /* Table 4-13: master plus channel control bitmaps are
                 * four bytes each, followed by the string index. */
                if (sub == 6 && (n < 10 || (n - 6) % 4))
                    return UAC_BAD_DESCRIPTOR;
                d->Entity[id] = (UAC_WORD)off;
            } else if (c.Active && sub == 1) {
                if (n < 16 || c.General++) return UAC_BAD_DESCRIPTOR;
                c.Alt.Terminal = b[off + 3];
                c.Type = b[off + 5];
                c.Pcm = b[off + 6] & 1;
                c.Alt.Channels = b[off + 10];
                c.Alt.ChannelConfig = b[off + 11] | ((unsigned int)b[off + 12] << 8) |
                    ((unsigned int)b[off + 13] << 16) | ((unsigned int)b[off + 14] << 24);
            } else if (c.Active && sub == 2) {
                if (n < 4 || c.Format++) return UAC_BAD_DESCRIPTOR;
                if (b[off + 3] == 1) {
                    if (n < 6) return UAC_BAD_DESCRIPTOR;
                    c.Alt.Subslot = b[off + 4];
                    c.Alt.Bits = b[off + 5];
                }
                /* Type in AS_GENERAL and FORMAT_TYPE must agree. */
                if (!c.General || b[off + 3] != c.Type) c.Bad = 1;
            }
        } else if (type == 5 && c.Active) {
            unsigned int addr, attr, packet, usage;
            if (n < 7) return UAC_BAD_DESCRIPTOR;
            c.Endpoints++;
            addr = b[off + 2]; attr = b[off + 3];
            packet = Word(b + off + 4); usage = (attr >> 4) & 3;
            /* Speed-dependent packet limits are checked by the USB layer. */
            if (!(addr & 15) || (addr & 0x70) || (attr & 3) != 1 ||
                !(packet & 0x7ff) || (packet & 0xe000) ||
                ((packet >> 11) & 3) == 3 ||
                !b[off + 6] || b[off + 6] > 16) c.Bad = 1;
            if (usage == 1) {
                c.Feedback++;
                if (!(addr & 0x80)) c.Bad = 1;
                c.Alt.FeedbackEndpoint = (UAC_BYTE)addr;
                c.Alt.FeedbackMaxPacket = (UAC_WORD)packet;
                c.Alt.FeedbackInterval = b[off + 6];
            } else if (usage == 0 || (usage == 2 && (addr & 0x80))) {
                c.Data++;
                c.Alt.DataEndpoint = (UAC_BYTE)addr;
                c.Alt.Attributes = (UAC_BYTE)attr;
                c.Alt.MaxPacket = (UAC_WORD)packet;
                c.Alt.Interval = b[off + 6];
            } else c.Bad = 1;
        }
    }
    rc = Finish(d, &c);
    if (rc) return rc;
    if (header && acBytes != acTotal) return UAC_BAD_DESCRIPTOR;
    if (!header || !seen[control] || !d->AltCount) return UAC_NO_AUDIO;
    return UAC_OK;
}

/* Framing as UacFunction checks it; returns the wTotalLength bound or 0. */
static unsigned int Framed(const UAC_BYTE *b, unsigned int size)
{
    unsigned int off, n, end;
    if (!b || size < 9 || b[0] != 9 || b[1] != 2) return 0;
    end = Word(b + 2);
    if (end < 9 || end > size) return 0;
    for (off = 9; off < end; off += n) {
        if (end - off < 2) return 0;
        n = b[off];
        if (n < 2 || n > end - off) return 0;
        if (b[off + 1] == 4 && n < 9) return 0;
        if (b[off + 1] == 11 && n < 8) return 0;
    }
    return end;
}

int UacWholeFirst(const UAC_BYTE *b, unsigned int size, unsigned int *first)
{
    unsigned int off, end = Framed(b, size);
    if (!first) return UAC_BAD_DESCRIPTOR;
    *first = 256;
    if (!end) return UAC_BAD_DESCRIPTOR;
    for (off = 9; off < end; off += b[off])
        if (b[off + 1] == 11 && b[off + 4] == 1 && b[off + 6] == 0x20) {
            *first = b[off + 2]; return UAC_OK;
        }
    for (off = 9; off < end; off += b[off])
        if (b[off + 1] == 4 && !b[off + 3] && b[off + 5] == 1 &&
            b[off + 6] == 1 && b[off + 7] == 0x20) {
            *first = b[off + 2]; return UAC_OK;
        }
    return UAC_NO_AUDIO;
}

int UacAltZero(const UAC_BYTE *b, unsigned int size, UAC_BYTE *numbers,
               UAC_BYTE *pipes, unsigned int max, unsigned int *count)
{
    UAC_BYTE seen[256];
    unsigned int off, end = Framed(b, size);
    if (!count) return UAC_BAD_DESCRIPTOR;
    *count = 0;
    if (!end || !numbers || !pipes) return UAC_BAD_DESCRIPTOR;
    Clear(seen, sizeof(seen));
    for (off = 9; off < end; off += b[off]) {
        if (b[off + 1] != 4 || b[off + 3]) continue;
        if (seen[b[off + 2]]++ || *count >= max || b[off + 4] > 30)
            return UAC_BAD_DESCRIPTOR;
        numbers[*count] = b[off + 2]; pipes[*count] = b[off + 4]; (*count)++;
    }
    if (!*count || *count != b[4]) return UAC_BAD_DESCRIPTOR;
    return UAC_OK;
}

int UacClock(const UAC_DESC *d, UAC_BYTE terminal,
             const UAC_BYTE *selection, UAC_BYTE *source)
{
    UAC_CLOCK_PATH path;
    int rc;
    if (!source) return UAC_CLOCK_INVALID;
    rc = UacClockPath(d, terminal, selection, &path);
    if (rc == UAC_OK) *source = path.Source;
    return rc;
}

int UacClockPath(const UAC_DESC *d, UAC_BYTE terminal,
                 const UAC_BYTE *selection, UAC_CLOCK_PATH *path)
{
    unsigned int off, id, sub, hops, pin;
    UAC_BYTE visited[256];
    const UAC_BYTE *p;
    if (!d || !d->Bytes || !path) return UAC_CLOCK_INVALID;
    Clear(path, sizeof(*path));
    Clear(visited, sizeof(visited));
    off = d->Entity[terminal];
    if (!off) return UAC_CLOCK_INVALID;
    p = d->Bytes + off;
    if (p[2] == 2) id = p[7];
    else if (p[2] == 3) id = p[8];
    else return UAC_CLOCK_INVALID;
    for (hops = 0; hops < 256; hops++) {
        if (!id || visited[id])
            return UAC_CLOCK_INVALID;
        off = d->Entity[id];
        if (!off) return UAC_CLOCK_INVALID;
        visited[id] = 1;
        p = d->Bytes + off; sub = p[2];
        if (sub == 10) {
            path->Source = (UAC_BYTE)id;
            return UAC_OK;
        }
        if (path->Count == UAC_CLOCK_HOPS) return UAC_CLOCK_INVALID;
        if (sub == 12) {
            /* Clock multiplier: bClockID, bCSourceID (length checked). */
            path->Id[path->Count] = (UAC_BYTE)id; path->Kind[path->Count] = 12;
            path->Pin[path->Count++] = 1;
            id = p[4];
            continue;
        }
        if (sub != 11) return UAC_CLOCK_INVALID;
        pin = p[4] == 1 ? 1 : (selection ? selection[id] : 0);
        if (!pin || pin > p[4]) return UAC_CLOCK_SELECTION;
        path->Id[path->Count] = (UAC_BYTE)id; path->Kind[path->Count] = 11;
        path->Pin[path->Count++] = (UAC_BYTE)pin;
        id = p[4 + pin];
    }
    return UAC_CLOCK_INVALID;
}
