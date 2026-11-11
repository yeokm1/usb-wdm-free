/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original implementation from the USB-IF "Device Class Definition for
 * Audio Devices" 1.0 (ADC-1) Tables 4-1 to 4-22 and "... for Audio Data
 * Formats" 1.0 (FMT-1) Table 2-1. No DDK sample or third-party code used.
 */
#include "uac1desc.h"
#include "uacconv.h"

static unsigned int Word(const unsigned char *p)
{
    return p[0] | ((unsigned int)p[1] << 8);
}

static unsigned long Tri(const unsigned char *p)
{
    return p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16);
}

static void Clear(void *p, unsigned int n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

/* wTotalLength when every descriptor is framed; 0 otherwise. */
static unsigned int Framed(const unsigned char *b, unsigned int size)
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
        if (b[off + 1] == 5 && n < 7) return 0;
    }
    return end;
}

int Uac1View(const unsigned char *b, unsigned int size, unsigned int number)
{
    unsigned int off, end = Framed(b, size);
    if (!end || number > 255) return UAC1_VIEW_NONE;
    for (off = 9; off < end; off += b[off]) {
        const unsigned char *p = b + off;
        if (p[1] != 4 || p[2] != number || p[3] != 0) continue;
        if (p[5] != 1 || p[7] != 0) return UAC1_VIEW_NONE;
        if (p[6] == 1) return UAC1_VIEW_CONTROL;
        if (p[6] == 2) return UAC1_VIEW_STREAMING;
        if (p[6] == 3) return UAC1_VIEW_MIDI;
        return UAC1_VIEW_NONE;
    }
    return UAC1_VIEW_NONE;
}

int Uac1WholeFirst(const unsigned char *b, unsigned int size, unsigned int *first)
{
    unsigned int off, end = Framed(b, size);
    if (!first) return UAC1_BAD_DESCRIPTOR;
    *first = 256;
    if (!end) return UAC1_BAD_DESCRIPTOR;
    for (off = 9; off < end; off += b[off])
        if (b[off + 1] == 11 && b[off + 4] == 1 && b[off + 6] == 0 &&
            Uac1View(b, size, b[off + 2]) == UAC1_VIEW_CONTROL) {
            *first = b[off + 2];
            return UAC1_OK;
        }
    for (off = 9; off < end; off += b[off])
        if (b[off + 1] == 4 && !b[off + 3] && b[off + 5] == 1 &&
            b[off + 6] == 1 && b[off + 7] == 0) {
            *first = b[off + 2];
            return UAC1_OK;
        }
    return UAC1_NO_AUDIO;
}

/* Entity-specific minimum lengths (ADC-1 Tables 4-3 to 4-15); 0 if the
 * descriptor at p (bLength n) is too short to read its fixed fields. */
static int EntityShape(const unsigned char *p, unsigned int n)
{
    unsigned int pins, size;
    switch (p[2]) {
    case UAC1_INPUT_TERMINAL: return n >= 12;
    case UAC1_OUTPUT_TERMINAL: return n >= 9;
    case UAC1_MIXER_UNIT:
        if (n < 5) return 0;
        pins = p[4];
        return pins && n >= 10 + pins;
    case UAC1_SELECTOR_UNIT:
        if (n < 5) return 0;
        pins = p[4];
        return pins && n >= 6 + pins;
    case UAC1_FEATURE_UNIT:
        if (n < 7) return 0;
        size = p[5];
        return size && size <= 4 && n >= 7 + size && !((n - 7) % size);
    case UAC1_PROCESSING_UNIT:
    case UAC1_EXTENSION_UNIT:
        if (n < 7) return 0;
        pins = p[6];
        if (n < 13 + pins) return 0;
        size = p[11 + pins]; /* bControlSize after iChannelNames */
        return n >= 13 + pins + size;
    default:
        return 1; /* unknown subtypes are recorded as opaque entities */
    }
}

static int InCollection(const UAC1_DESC *d, unsigned int iface)
{
    unsigned int i;
    for (i = 0; i < d->CollectionCount; i++)
        if (d->Collection[i] == iface) return 1;
    return 0;
}

typedef struct EP {
    unsigned char Length, Address, Attributes, Interval, Refresh, Synch;
    unsigned short MaxPacket;
    unsigned char Controls, LockUnits;
    unsigned short LockDelay;
    unsigned char HasCs;
} EP;

/* Candidate storage is local until an entire alternate has been checked. */
typedef struct CANDIDATE {
    UAC_FMT_ALT Alt;
    unsigned int Active, General, Format, Type, Bad, Endpoints, Expected;
    unsigned int Delay;
    EP Ep[2];
} CANDIDATE;

static int LinkOk(const UAC1_DESC *d, unsigned int link, int capture)
{
    const unsigned char *t = Uac1Entity(d, link);
    if (!t) return 0;
    if (capture) return t[2] == UAC1_OUTPUT_TERMINAL && Word(t + 4) == UAC1_TERMINAL_STREAMING;
    return t[2] == UAC1_INPUT_TERMINAL && Word(t + 4) == UAC1_TERMINAL_STREAMING;
}

static int Finish(UAC1_DESC *d, CANDIDATE *c, int highSpeed)
{
    unsigned int i, data = 2, sync = 2;
    UAC_FMT_ALT *a = &c->Alt;
    EP *e;
    if (!c->Active) return UAC1_OK;
    if (c->Endpoints == 1) data = 0;
    else if (c->Endpoints == 2) {
        /* The data endpoint's bSynchAddress names the synch endpoint
         * (ADC-1 3.7.2.2); else the USB 2.0 usage type 01b (feedback). */
        for (i = 0; i < 2; i++)
            if (c->Ep[i].Synch && c->Ep[i].Synch == c->Ep[1 - i].Address) { data = i; sync = 1 - i; }
        if (data == 2)
            for (i = 0; i < 2; i++)
                if (((c->Ep[1 - i].Attributes >> 4) & 3) == 1) { data = i; sync = 1 - i; }
    }
    if (!c->General || !c->Format || c->Type != 1 || c->Bad || data == 2 ||
        c->Endpoints != c->Expected ||
        !a->Channels || a->Channels > 8 || !a->Subframe || a->Subframe > 4 ||
        !a->Bits || a->Bits > a->Subframe * 8 || !a->RateMin || a->RateMax < a->RateMin)
        goto skip;
    /* FMT-1 2.2.6: PCM in 2 to 4-byte subframes; PCM8, ALAW and MULAW 8
     * bits in 1 byte; IEEE_FLOAT 32 bits in 4 (docs/design/uac2.md 26.12). */
    if (!UacConvKnown(a->FormatTag, a->Subframe, a->Bits)) goto skip;
    e = &c->Ep[data];
    /* ADC-1 Table 4-20: a 9-byte isochronous endpoint, synchronization type
     * asynchronous, adaptive or synchronous, bInterval 1 at Full Speed;
     * Table 4-21: its class-specific endpoint descriptor follows. */
    if (e->Length < 9 || (e->Attributes & 3) != 1 || !((e->Attributes >> 2) & 3) ||
        !UacFmtCapacity(e->MaxPacket, highSpeed) ||
        !e->Interval || e->Interval > (highSpeed ? 4 : 1) || !e->HasCs || e->Refresh ||
        (sync == 2 && e->Synch))
        goto skip;
    a->Capture = (unsigned char)((e->Address & 0x80) ? 1 : 0);
    if (!LinkOk(d, a->Terminal, a->Capture)) goto skip;
    a->DataEndpoint = e->Address;
    a->Attributes = e->Attributes;
    a->Interval = e->Interval;
    a->MaxPacket = e->MaxPacket;
    a->Sync = (unsigned char)((e->Attributes >> 2) & 3);
    a->EpControls = e->Controls;
    a->LockUnits = e->LockUnits;
    a->LockDelay = e->LockDelay;
    /* A MaxPacketsOnly endpoint is carried (ADC-1 4.6.1.2): OUT padded;
     * IN from a synchronous or adaptive source, whose rate the host knows,
     * in PCM or IEEE_FLOAT, where a zero frame is silence, counted against
     * the rate (docs/design/uac2.md 26.12); not from an asynchronous
     * source, nor in PCM8, ALAW or MULAW. An adaptive source's synch OUT
     * endpoint (ADC-1 3.7.2.2) is kept and fed Ff. */
    if (a->Capture && (a->EpControls & UAC_EP_MAXPACKETSONLY) &&
        (a->Sync == UAC_SYNC_ASYNC || (a->FormatTag != UAC_TAG_PCM && a->FormatTag != UAC_TAG_FLOAT)))
        goto skip;
    /* An asynchronous sink has its synch IN endpoint (ADC-1 3.7.2.2);
     * implicit feedback is not supported in v1. */
    if (!a->Capture && a->Sync == UAC_SYNC_ASYNC && sync == 2) goto skip;
    if (sync < 2) {
        EP *s = &c->Ep[sync];
        /* Table 4-22: 9 bytes, isochronous, synchronization type none,
         * bInterval 1 (Full Speed), bRefresh 1..9, bSynchAddress 0; it runs
         * against the data direction: an asynchronous sink's feedback IN
         * endpoint, or an adaptive source's Ff OUT endpoint (kept only so
         * the alternate's pipes are all selected). */
        if (s->Length < 9 || (s->Attributes & 3) != 1 || ((s->Attributes >> 2) & 3) ||
            ((s->Address ^ e->Address) & 0x80) == 0 || s->Synch ||
            !UacFmtCapacity(s->MaxPacket, highSpeed) || (s->MaxPacket & 0x7ff) < 3 ||
            !s->Refresh || s->Refresh > 9 || !s->Interval || s->Interval > (highSpeed ? 4 : 1) ||
            (a->Capture ? a->Sync != UAC_SYNC_ADAPTIVE : a->Sync != UAC_SYNC_ASYNC) ||
            e->Synch != s->Address)
            goto skip;
        a->SyncEndpoint = s->Address;
        a->SyncMaxPacket = s->MaxPacket;
        a->SyncInterval = s->Interval;
        a->Refresh = s->Refresh;
    }
    if (d->AltCount == UAC_FMT_MAX_ALTS) return UAC1_TOO_MANY_ALTS;
    for (i = 0; i < d->AltCount; i++)
        if (d->Alt[i].Interface == a->Interface && d->Alt[i].Alternate == a->Alternate)
            return UAC1_BAD_DESCRIPTOR;
    d->Alt[d->AltCount++] = *a;
    return UAC1_OK;
skip:
    d->Skipped++;
    return UAC1_OK;
}

static void Format(CANDIDATE *c, const unsigned char *p, unsigned int n)
{
    unsigned int t, i;
    UAC_FMT_ALT *a = &c->Alt;
    if (n < 4 || c->Format++) { c->Bad = 1; return; }
    c->Type = p[3];
    if (c->Type != 1) return;
    if (n < 8) { c->Bad = 1; return; }
    a->Channels = p[4]; a->Subframe = p[5]; a->Bits = p[6];
    t = p[7];
    if (!t) {
        if (n < 14) { c->Bad = 1; return; }
        a->RateCount = 0;
        a->RateMin = Tri(p + 8); a->RateMax = Tri(p + 11);
        return;
    }
    if (n < 8 + 3 * t || t > UAC_FMT_MAX_RATES) { c->Bad = 1; return; }
    a->RateCount = (unsigned char)t;
    a->RateMin = 0xffffffffUL; a->RateMax = 0;
    for (i = 0; i < t; i++) {
        unsigned long r = Tri(p + 8 + 3 * i);
        if (!r) { c->Bad = 1; return; }
        a->Rate[i] = r;
        if (r < a->RateMin) a->RateMin = r;
        if (r > a->RateMax) a->RateMax = r;
    }
}

/* Logical channels an entity outputs (ADC-1 3.7.2.3): Input Terminal,
 * Mixer, Processing and Extension Units state it; Feature and Selector
 * Units pass their input's through. 0 for none or a cycle. */
static unsigned int Channels(const UAC1_DESC *d, unsigned int id)
{
    unsigned char visited[256];
    const unsigned char *p;
    Clear(visited, sizeof(visited));
    /* Feature and Selector Units pass one source's cluster through: follow
     * that single chain iteratively; a revisited id is a cycle (no
     * recursion on the kernel stack). */
    for (;;) {
        if (!id || id > 255 || !d->Entity[id] || visited[id]) return 0;
        visited[id] = 1;
        p = d->Bytes + d->Entity[id];
        switch (p[2]) {
        case UAC1_INPUT_TERMINAL: return p[7];
        case UAC1_MIXER_UNIT: return p[5 + p[4]];
        case UAC1_PROCESSING_UNIT:
        case UAC1_EXTENSION_UNIT: return p[7 + p[6]];
        case UAC1_FEATURE_UNIT: id = p[4]; break;
        case UAC1_SELECTOR_UNIT: id = p[5]; break;
        default: return 0;
        }
    }
}

/* Lengths that depend on the channel clusters: a Mixer Unit's bmControls
 * covers every input channel times every output channel (Table 4-5); the
 * Up/Down-mix and Dolby Prologic Processing Units carry bNrModes and
 * waModes after the common part (Tables 4-9, 4-10). */
static int Graph(const UAC1_DESC *d)
{
    unsigned int id, i, pins, in, bits, size, modes, base;
    for (id = 1; id < 256; id++) {
        const unsigned char *p;
        unsigned int n;
        if (!d->Entity[id]) continue;
        p = d->Bytes + d->Entity[id];
        n = p[0];
        if (p[2] == UAC1_MIXER_UNIT) {
            pins = p[4];
            for (i = 0, in = 0; i < pins; i++) {
                unsigned int c = Channels(d, p[5 + i]);
                if (!c) return 0;
                in += c;
            }
            if (!p[5 + pins]) return 0;
            bits = in * p[5 + pins];
            if (n < 10 + pins + (bits + 7) / 8) return 0;
        } else if (p[2] == UAC1_PROCESSING_UNIT) {
            unsigned int type = Word(p + 4);
            pins = p[6];
            size = p[11 + pins];
            base = 13 + pins + size;
            if (type == 1 || type == 2) {
                if (n < base + 1) return 0;
                modes = p[base];
                if (n < base + 1 + 2 * modes) return 0;
            }
        }
    }
    return 1;
}

/* An IAD that covers the AC interface bounds the function: it must name
 * it first, be class 01 protocol 00, and contain every collected
 * interface (USB IAD ECN; ADC-1 4.3.2). */
/* The IAD (by its offset) covering interface `n`, 0 if none. */
static unsigned int IadOf(const unsigned char *b, unsigned int end, unsigned int n)
{
    unsigned int off;
    for (off = 9; off < end; off += b[off])
        if (b[off + 1] == 11 && n >= b[off + 2] && n < (unsigned int)b[off + 2] + b[off + 3])
            return off;
    return 0;
}

/* The whole IAD map. IADs may not be empty or
 * overlap; the AC interface's IAD (if any) must start at it, be class 01
 * protocol 00; every collected interface must have the same IAD as the AC
 * interface (none, or that one), so the collection never crosses into
 * another function. */
static int IadScope(const unsigned char *b, unsigned int end, unsigned int control,
                    const UAC1_DESC *d)
{
    unsigned int off, other, i, mine;
    for (off = 9; off < end; off += b[off]) {
        if (b[off + 1] != 11) continue;
        if (!b[off + 3] || (unsigned int)b[off + 2] + b[off + 3] > 256) return 0;
        for (other = off + b[off]; other < end; other += b[other])
            if (b[other + 1] == 11 && b[other + 2] < b[off + 2] + b[off + 3] &&
                b[off + 2] < b[other + 2] + b[other + 3]) return 0;
    }
    mine = IadOf(b, end, control);
    if (mine && (b[mine + 2] != control || b[mine + 4] != 1 || b[mine + 6] != 0)) return 0;
    for (i = 0; i < d->CollectionCount; i++)
        if (IadOf(b, end, d->Collection[i]) != mine) return 0;
    return 1;
}

int Uac1Parse(const unsigned char *b, unsigned int size, unsigned int control,
              int highSpeed, UAC1_DESC *d)
{
    unsigned int off, end, n, type, iface = 256, alt = 0, inAc = 0, inAs = 0;
    unsigned int header = 0, acTotal = 0, acBytes = 0, rc, i, found = 0;
    unsigned char seen[256], busy[256];
    CANDIDATE c;
    if (!d) return UAC1_BAD_DESCRIPTOR;
    Clear(d, sizeof(*d));
    Clear(&c, sizeof(c));
    Clear(seen, sizeof(seen));
    Clear(busy, sizeof(busy));
    end = Framed(b, size);
    if (!end || control > 255) return UAC1_BAD_DESCRIPTOR;
    if (Uac1View(b, size, control) != UAC1_VIEW_CONTROL) return UAC1_NO_AUDIO;
    d->Bytes = b; d->Length = end; d->Control = (unsigned char)control;
    /* Pass 1: the AC interface, its header and entities. */
    for (off = 9; off < end; off += n) {
        const unsigned char *p = b + off;
        n = p[0]; type = p[1];
        if (type == 4) {
            inAc = p[2] == control;
            if (inAc && p[3]) return UAC1_BAD_DESCRIPTOR; /* AC has alternate 0 only */
            if (inAc) found = 1;
            continue;
        }
        if (!inAc) continue;
        if (type == 5) {
            if ((p[3] & 3) == 3 && (p[2] & 0x80) && !d->Interrupt) d->Interrupt = p[2];
            continue;
        }
        if (type != 0x24) continue;
        if (n < 3) return UAC1_BAD_DESCRIPTOR;
        acBytes += n;
        if (p[2] == UAC1_HEADER) {
            if (header++ || n < 8 || Word(p + 3) != 0x0100) return UAC1_BAD_DESCRIPTOR;
            acTotal = Word(p + 5);
            d->CollectionCount = p[7];
            if (n < 8U + p[7] || p[7] > UAC1_MAX_COLLECTION || acTotal < n || acTotal > end - off)
                return UAC1_BAD_DESCRIPTOR;
            for (i = 0; i < p[7]; i++) {
                unsigned int k, nr = p[8 + i];
                if (nr == control) return UAC1_BAD_DESCRIPTOR;
                for (k = 0; k < i; k++) if (d->Collection[k] == nr) return UAC1_BAD_DESCRIPTOR;
                d->Collection[i] = (unsigned char)nr;
            }
            continue;
        }
        if (!header || n < 4) return UAC1_BAD_DESCRIPTOR;
        if (!p[3] || d->Entity[p[3]] || !EntityShape(p, n)) return UAC1_BAD_DESCRIPTOR;
        d->Entity[p[3]] = (unsigned short)off;
    }
    if (!found || !header || acBytes != acTotal) return UAC1_BAD_DESCRIPTOR;
    if (!Graph(d)) return UAC1_BAD_DESCRIPTOR;
    if (!IadScope(b, end, control, d)) return UAC1_BAD_DESCRIPTOR;
    /* Pass 2: every alternate of the collection's interfaces. */
    for (off = 9; off < end; off += n) {
        const unsigned char *p = b + off;
        n = p[0]; type = p[1];
        if (type == 4) {
            rc = Finish(d, &c, highSpeed);
            if (rc) return rc;
            Clear(&c, sizeof(c));
            iface = p[2]; alt = p[3];
            inAs = InCollection(d, iface);
            if (!inAs) continue;
            if (p[5] != 1 || p[7] != 0 || (p[6] != 2 && p[6] != 3)) return UAC1_BAD_DESCRIPTOR;
            if (!alt) {
                if (seen[iface]) return UAC1_BAD_DESCRIPTOR;
                seen[iface] = 1;
                if (p[6] == 3) d->MidiCount++;
                /* A streaming interface whose alternate 0 has endpoints is not
                 * carried: stopping a stream selects alternate 0 with no pipes
                 * (ADC-1 3.7.2: alternate 0 is the zero-bandwidth setting). */
                if (p[6] == 2 && p[4]) busy[iface] = 1;
            } else if (!seen[iface]) return UAC1_BAD_DESCRIPTOR;
            c.Active = p[6] == 2 && alt != 0;
            if (c.Active && busy[iface]) { c.Active = 0; d->Skipped++; }
            c.Expected = p[4];
            c.Alt.Interface = (unsigned char)iface;
            c.Alt.Alternate = (unsigned char)alt;
            continue;
        }
        if (!inAs || !c.Active) continue;
        if (type == 0x24) {
            if (n < 3) { c.Bad = 1; continue; }
            if (p[2] == 1) {
                if (n < 7 || c.General++) { c.Bad = 1; continue; }
                c.Alt.Terminal = p[3];
                c.Delay = p[4];
                c.Alt.FormatTag = (unsigned short)Word(p + 5);
            } else if (p[2] == 2) Format(&c, p, n);
        } else if (type == 5) {
            EP *e;
            if (c.Endpoints >= 2) { if (c.Endpoints < 255) c.Endpoints++; c.Bad = 1; continue; }
            e = &c.Ep[c.Endpoints++];
            e->Length = (unsigned char)n;
            e->Address = p[2]; e->Attributes = p[3];
            e->MaxPacket = (unsigned short)Word(p + 4); e->Interval = p[6];
            if (n >= 9) { e->Refresh = p[7]; e->Synch = p[8]; }
            if ((e->Address & 0x70) || !(e->Address & 15)) c.Bad = 1;
        } else if (type == 0x25) {
            /* Only after a stored endpoint (at most two, Ep[]). */
            if (!c.Endpoints || c.Endpoints > 2 || n < 7 || p[2] != 1 ||
                c.Ep[c.Endpoints - 1].HasCs) { c.Bad = 1; continue; }
            c.Ep[c.Endpoints - 1].HasCs = 1;
            c.Ep[c.Endpoints - 1].Controls = p[3];
            c.Ep[c.Endpoints - 1].LockUnits = p[4];
            c.Ep[c.Endpoints - 1].LockDelay = (unsigned short)Word(p + 5);
        }
    }
    rc = Finish(d, &c, highSpeed);
    if (rc) return rc;
    for (i = 0; i < d->CollectionCount; i++)
        if (!seen[d->Collection[i]]) return UAC1_BAD_DESCRIPTOR;
    if (!d->AltCount && !d->MidiCount) return UAC1_NO_AUDIO;
    return UAC1_OK;
}

static unsigned int Fits(const UAC1_DESC *d, int highSpeed)
{
    unsigned int i, k, n = 0;
    for (i = 0; i < d->AltCount; i++) {
        const UAC_FMT_ALT *a = &d->Alt[i];
        unsigned int cap = UacFmtCapacity(a->MaxPacket, highSpeed);
        unsigned long r[2];
        if (!a->RateCount) { r[0] = a->RateMin; r[1] = a->RateMin; }
        for (k = 0; k < (a->RateCount ? a->RateCount : 1u); k++) {
            unsigned long rate = a->RateCount ? a->Rate[k] : r[0];
            unsigned int need = UacFmtNeed(a, rate, highSpeed);
            if (need && need <= cap) { n++; break; }
        }
    }
    return n;
}

int Uac1GuessSpeed(const unsigned char *b, unsigned int size, unsigned int control,
                   UAC1_DESC *d, int *highSpeed)
{
    unsigned int fs = 0, hs = 0;
    if (!d || !highSpeed) return 0;
    if (Uac1Parse(b, size, control, 0, d) == UAC1_OK) fs = Fits(d, 0);
    if (Uac1Parse(b, size, control, 1, d) == UAC1_OK) hs = Fits(d, 1);
    if (fs && !hs) { *highSpeed = 0; return 1; }
    if (hs && !fs) { *highSpeed = 1; return 1; }
    return 0;
}

const unsigned char *Uac1Entity(const UAC1_DESC *d, unsigned int id)
{
    if (!d || !d->Bytes || !id || id > 255 || !d->Entity[id]) return 0;
    return d->Bytes + d->Entity[id];
}

int Uac1FeatureChannels(const UAC1_DESC *d, unsigned int unit, unsigned int *channels)
{
    const unsigned char *p = Uac1Entity(d, unit);
    if (!p || p[2] != UAC1_FEATURE_UNIT || !channels) return 0;
    *channels = (p[0] - 7) / p[5] - 1;
    return 1;
}

int Uac1FeatureBits(const UAC1_DESC *d, unsigned int unit, unsigned int channel,
                    unsigned long *bits)
{
    unsigned int channels, size, i;
    unsigned long v = 0;
    const unsigned char *p = Uac1Entity(d, unit);
    if (!bits || !Uac1FeatureChannels(d, unit, &channels) || channel > channels) return 0;
    size = p[5];
    for (i = 0; i < size; i++) v |= (unsigned long)p[6 + channel * size + i] << (8 * i);
    *bits = v;
    return 1;
}

/* Upstream inputs of an entity, or -1 for a malformed one. */
static int Sources(const unsigned char *p, const unsigned char **list)
{
    switch (p[2]) {
    case UAC1_INPUT_TERMINAL: *list = 0; return 0;
    case UAC1_OUTPUT_TERMINAL: *list = p + 7; return 1;
    case UAC1_FEATURE_UNIT: *list = p + 4; return 1;
    case UAC1_MIXER_UNIT:
    case UAC1_SELECTOR_UNIT: *list = p + 5; return p[4];
    case UAC1_PROCESSING_UNIT:
    case UAC1_EXTENSION_UNIT: *list = p + 7; return p[6];
    default: *list = 0; return -1;
    }
}

int Uac1Path(const UAC1_DESC *d, unsigned int from, unsigned int to,
             unsigned char *path, unsigned int max, unsigned int *count)
{
    unsigned char parent[256], visited[256], queue[256];
    unsigned int head = 0, tail = 0, i, id, len;
    if (!count) return UAC1_BAD_DESCRIPTOR;
    *count = 0;
    if (!path || !Uac1Entity(d, from) || !Uac1Entity(d, to)) return UAC1_BAD_DESCRIPTOR;
    Clear(visited, sizeof(visited));
    Clear(parent, sizeof(parent));
    visited[from] = 1; queue[tail++] = (unsigned char)from;
    while (head < tail) {
        const unsigned char *list;
        int n;
        id = queue[head++];
        if (id == to) {
            for (len = 1, i = to; i != from; i = parent[i]) len++;
            if (len > max) return UAC1_BAD_DESCRIPTOR;
            for (i = to, n = (int)len - 1; ; i = parent[i], n--) {
                path[n] = (unsigned char)i;
                if (i == from) break;
            }
            *count = len;
            return UAC1_OK;
        }
        n = Sources(Uac1Entity(d, id), &list);
        if (n < 0) continue; /* opaque entity: no path through it */
        for (i = 0; i < (unsigned int)n; i++) {
            unsigned int s = list[i];
            if (!s || visited[s] || !Uac1Entity(d, s)) continue;
            visited[s] = 1; parent[s] = (unsigned char)id;
            queue[tail++] = (unsigned char)s;
        }
    }
    return UAC1_NO_PATH;
}

int Uac1StreamFeature(const UAC1_DESC *d, unsigned int link, int capture,
                      unsigned int *unit, unsigned int *terminal)
{
    unsigned char path[256];
    unsigned int count, id, i;
    if (!unit || !terminal) return UAC1_BAD_DESCRIPTOR;
    *unit = *terminal = 0;
    if (!LinkOk(d, link, capture)) return UAC1_BAD_DESCRIPTOR;
    for (id = 1; id < 256; id++) {
        const unsigned char *t = Uac1Entity(d, id);
        if (!t || (capture ? t[2] != UAC1_INPUT_TERMINAL : t[2] != UAC1_OUTPUT_TERMINAL)) continue;
        if (Word(t + 4) == UAC1_TERMINAL_STREAMING) continue;
        /* path[] runs from the downstream end to the upstream end. */
        if (capture ? Uac1Path(d, link, id, path, sizeof(path), &count) != UAC1_OK
                    : Uac1Path(d, id, link, path, sizeof(path), &count) != UAC1_OK)
            continue;
        *terminal = id;
        /* The Feature Unit nearest the stream's own terminal. */
        if (capture) {
            for (i = 0; i < count; i++)
                if (Uac1Entity(d, path[i])[2] == UAC1_FEATURE_UNIT) { *unit = path[i]; break; }
        } else {
            for (i = count; i-- > 0; )
                if (Uac1Entity(d, path[i])[2] == UAC1_FEATURE_UNIT) { *unit = path[i]; break; }
        }
        return UAC1_OK;
    }
    return UAC1_NO_PATH;
}
