/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original MIDIStreaming descriptor parser from USB-IF MIDI 1.0 section 6
 * (Tables 6-1..6-7). Tolerance rules follow the behaviour the design cites
 * from Linux sound/usb/midi.c (skip, not fail); no Linux code is copied.
 */
#include "umcore.h"
#include <string.h>

static unsigned int Word(const unsigned char *p)
{
    return p[0] | ((unsigned int)p[1] << 8);
}

/* Validates the configuration header and every descriptor boundary. */
static int Bounded(const unsigned char *b, unsigned int length)
{
    unsigned int off, n;
    if (!b || length < 9 || length > 65535 || b[0] != 9 || b[1] != 2 ||
        Word(b + 2) != length) return 0;
    for (off = 9; off < length; off += n) {
        if (length - off < 2 || (n = b[off]) < 2 || n > length - off) return 0;
        if (b[off + 1] == 4 && n < 9) return 0;
    }
    return 1;
}

int UmParseInterface(const unsigned char *b, unsigned int length,
                     unsigned int number, unsigned int alternate,
                     UM_INTERFACE *out)
{
    UM_INTERFACE t;
    unsigned char kind[256], type[256], str[256], seen[256];
    unsigned int source[256];
    unsigned int off, n, start = 0, end = 0, found = 0, header = 0;
    unsigned int i, j, k, id;
    int pending = -1;          /* -1 none, 0 skip, 1 candidate */
    UM_ENDPOINT cand;
    const unsigned char *p;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (number > 255 || alternate > 255 || !Bounded(b, length)) return 0;
    memset(&t, 0, sizeof(t));
    for (off = 9; off < length; off += n) {
        n = b[off]; p = b + off;
        if (p[1] == 4 || p[1] == 0x0b) {
            if (found && !end) end = off;
            if (p[1] == 4 && p[2] == number && p[3] == alternate) {
                if (found || p[5] != 1 || p[6] != 3) return 0;
                found = 1; start = off + n;
                t.Number = (unsigned char)number;
                t.Alternate = (unsigned char)alternate;
                t.Declared = p[4];
            }
        }
    }
    if (!found) return 0;
    if (!end) end = length;
    memset(kind, 0, sizeof(kind)); memset(type, 0, sizeof(type));
    memset(str, 0, sizeof(str)); memset(seen, 0, sizeof(seen));
    memset(source, 0, sizeof(source)); memset(&cand, 0, sizeof(cand));
    for (off = start; off < end; off += n) {
        p = b + off; n = p[0];
        if (p[1] == 0x24) {                      /* CS_INTERFACE */
            if (n < 3) return 0;
            if (p[2] == 1) {                     /* MS header, Table 6-2 */
                if (header || n < 7 || Word(p + 3) != 0x0100) return 0;
                header = 1;
            } else if (p[2] == 2 || p[2] == 3) { /* MIDI IN / OUT jack */
                if (n < 6 || !p[4]) return 0;
                id = p[4];
                if (kind[id]) return 0;          /* duplicate ID: ambiguous */
                if (p[2] == 2) {
                    kind[id] = 2; str[id] = p[5];
                } else {
                    if (n < 7u + 2u * p[5]) return 0;
                    kind[id] = 3; source[id] = off; str[id] = p[6 + 2 * p[5]];
                }
                type[id] = p[3];
            } else if (p[2] == 4) {              /* Element: skipped */
                if (n >= 4 && p[3] && !kind[p[3]]) kind[p[3]] = 4;
            }                                    /* other subtypes skipped */
        } else if (p[1] == 5) {                  /* standard endpoint */
            unsigned int mp;
            if (n < 7) return 0;
            pending = 0;
            mp = Word(p + 4) & 0x7ff;
            if ((p[2] & 0x70) || !(p[2] & 15) || seen[p[2]] ||
                ((p[3] & 3) != 2 && (p[3] & 3) != 3) || mp < 4) continue;
            memset(&cand, 0, sizeof(cand));
            cand.Address = p[2]; cand.Type = (unsigned char)(p[3] & 3);
            cand.Interval = p[6]; cand.MaxPacket = (unsigned short)mp;
            pending = 1;
        } else if (p[1] == 0x25) {               /* CS_ENDPOINT, Table 6-7 */
            if (pending == 1 && n >= 4 && p[2] == 1 && p[3] && p[3] <= 16 &&
                n >= 4u + p[3] && t.Count < UM_MAX_ENDPOINTS) {
                cand.Cables = p[3];
                for (i = 0; i < p[3]; i++) cand.Jack[i] = p[4 + i];
                seen[cand.Address] = 1;
                t.Endpoint[t.Count++] = cand;
            }
            pending = -1;
        }                                        /* other types skipped */
    }
    if (!header || !t.Count) return 0;
    /* Names: the embedded jack's iJack, else the External jack wired to it. */
    for (i = 0; i < t.Count; i++) {
        UM_ENDPOINT *e = &t.Endpoint[i];
        int in = (e->Address & 0x80) != 0;
        for (j = 0; j < e->Cables; j++) {
            unsigned char name = 0;
            id = e->Jack[j];
            if (kind[id] == (in ? 3u : 2u)) name = str[id];
            if (!name && !in) {
                for (k = 1; k < 256 && !name; k++) if (kind[k] == 3 && type[k] == 2) {
                    const unsigned char *q = b + source[k]; unsigned int s;
                    for (s = 0; s < q[5]; s++) if (q[6 + 2 * s] == id) { name = str[k]; break; }
                }
            } else if (!name && in && kind[id] == 3) {
                const unsigned char *q = b + source[id]; unsigned int s;
                for (s = 0; s < q[5] && !name; s++)
                    if (kind[q[6 + 2 * s]] == 2) name = str[q[6 + 2 * s]];
            }
            e->Name[j] = name;
        }
    }
    *out = t;
    return 1;
}

int UmFindInterface(const unsigned char *b, unsigned int length, UM_INTERFACE *out)
{
    unsigned char tried[256], alts[256];
    unsigned int off, n, number, alt, off2;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!Bounded(b, length)) return 0;
    memset(tried, 0, sizeof(tried));
    for (off = 9; off < length; off += n) {
        n = b[off];
        if (b[off + 1] != 4 || b[off + 5] != 1 || b[off + 6] != 3) continue;
        number = b[off + 2];
        if (tried[number]) continue;
        tried[number] = 1;
        memset(alts, 0, sizeof(alts));
        for (off2 = 9; off2 < length; off2 += b[off2])
            if (b[off2 + 1] == 4 && b[off2 + 2] == number) alts[b[off2 + 3]] = 1;
        for (alt = 0; alt < 256; alt++)
            if (alts[alt] && UmParseInterface(b, length, number, alt, out)) return 1;
    }
    memset(out, 0, sizeof(*out));
    return 0;
}

unsigned int UmMapPorts(const UM_INTERFACE *iface, UM_PORTMAP *map)
{
    unsigned int i, c;
    if (!map) return 0;
    memset(map, 0, sizeof(*map));
    if (!iface) return 0;
    for (i = 0; i < iface->Count && i < UM_MAX_ENDPOINTS; i++) {
        const UM_ENDPOINT *e = &iface->Endpoint[i];
        int in = (e->Address & 0x80) != 0;
        unsigned int *pipes = in ? &map->InPipes : &map->OutPipes;
        unsigned int *ports = in ? &map->Inputs : &map->Outputs;
        UM_PORT *list = in ? map->In : map->Out;
        if (*pipes >= UM_MAX_PIPES || !e->Cables || e->Cables > 16 ||
            *ports >= UM_MAX_PORTS) continue;
        (in ? map->InPipe : map->OutPipe)[*pipes] = (unsigned char)i;
        for (c = 0; c < e->Cables && *ports < UM_MAX_PORTS; c++) {
            list[*ports].Pipe = (unsigned char)*pipes;
            list[*ports].Cable = (unsigned char)c;
            list[*ports].Name = e->Name[c];
            (*ports)++;
        }
        (*pipes)++;
    }
    return map->Outputs > map->Inputs ? map->Outputs : map->Inputs;
}

static unsigned int Append(unsigned short out[32], unsigned int at,
                           const unsigned short *s, unsigned int n)
{
    unsigned int i;
    for (i = 0; i < n && s[i] && at < 31; i++) out[at++] = s[i];
    return at;
}

unsigned int UmPortName(const unsigned short *product, unsigned int pn,
                        const unsigned short *jack, unsigned int jn,
                        int several, unsigned int index, unsigned short out[32])
{
    static const unsigned short fallback[] = {'U','S','B',' ','M','I','D','I'};
    static const unsigned short midi[] = {' ','M','I','D','I',' '};
    unsigned short digits[4];
    unsigned int at = 0, i, d = 0, v = index + 1;
    if (!product) pn = 0;
    if (!jack) jn = 0;
    for (i = 0; i < pn; i++) if (!product[i]) { pn = i; break; }
    for (i = 0; i < jn; i++) if (!jack[i]) { jn = i; break; }
    while (pn && product[pn - 1] == ' ') pn--;   /* trailing padding */
    while (jn && jack[jn - 1] == ' ') jn--;
    if (!pn) { product = fallback; pn = 8; }
    if (jn) {
        int prefixed = jn >= pn;
        for (i = 0; prefixed && i < pn; i++) if (jack[i] != product[i]) prefixed = 0;
        if (!prefixed) {
            at = Append(out, at, product, pn);
            if (at < 31) out[at++] = ' ';
        }
        at = Append(out, at, jack, jn);
    } else {
        at = Append(out, at, product, pn);
        if (several) {
            at = Append(out, at, midi, 6);
            do { digits[d++] = (unsigned short)('0' + v % 10); v /= 10; } while (v && d < 4);
            while (d && at < 31) out[at++] = digits[--d];
        }
    }
    while (at && out[at - 1] == ' ') at--;
    out[at] = 0;
    return at;
}
