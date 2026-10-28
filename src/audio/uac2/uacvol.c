/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Playback volume and mute (uacvol.h). Original code from USB Audio 2.0
 * sections 4.7.2 (entity descriptors), 5.2.1-5.2.3 (CUR/RANGE layouts)
 * and 5.2.5.7.1-2 (mute, volume), and the KS audio property units
 * (KSPROPERTY_AUDIO_VOLUMELEVEL, published kit documentation).
 */
#include "uacvol.h"

#define WALK_DEPTH 32

static int Word(const unsigned char *p)
{
    unsigned int v = (unsigned int)p[0] | ((unsigned int)p[1] << 8);
    return v & 0x8000u ? (int)v - 65536 : (int)v;
}

/* dB 0..96 as Q16 linear: round(65536 * 10^(-k/20)). */
static const unsigned long Table[97] = {
    65536UL, 58409UL, 52057UL, 46396UL, 41350UL, 36854UL, 32846UL, 29274UL,
    26090UL, 23253UL, 20724UL, 18471UL, 16462UL, 14672UL, 13076UL, 11654UL,
    10387UL, 9257UL, 8250UL, 7353UL, 6554UL, 5841UL, 5206UL, 4640UL,
    4135UL, 3685UL, 3285UL, 2927UL, 2609UL, 2325UL, 2072UL, 1847UL,
    1646UL, 1467UL, 1308UL, 1165UL, 1039UL, 926UL, 825UL, 735UL,
    655UL, 584UL, 521UL, 464UL, 414UL, 369UL, 328UL, 293UL,
    261UL, 233UL, 207UL, 185UL, 165UL, 147UL, 131UL, 117UL,
    104UL, 93UL, 83UL, 74UL, 66UL, 58UL, 52UL, 46UL,
    41UL, 37UL, 33UL, 29UL, 26UL, 23UL, 21UL, 18UL,
    16UL, 15UL, 13UL, 12UL, 10UL, 9UL, 8UL, 7UL,
    7UL, 6UL, 5UL, 5UL, 4UL, 4UL, 3UL, 3UL,
    3UL, 2UL, 2UL, 2UL, 2UL, 1UL, 1UL, 1UL,
    1UL
};

/* An entity's descriptor, bounds-checked against the configuration. */
static const UAC_BYTE *Entity(const UAC_DESC *d, unsigned int id)
{
    unsigned int off;
    if (!id || id > 255) return 0;
    off = d->Entity[id];
    if (!off || off + 4 > d->Length || off + d->Bytes[off] > d->Length) return 0;
    return d->Bytes + off;
}

/* The source ids of a unit or Output Terminal: count and the offset of
 * the first id within the descriptor. 0 sources for an Input Terminal or
 * an entity this walk does not cross. */
static unsigned int Sources(const UAC_BYTE *p, unsigned int *first)
{
    unsigned int n = p[0], count;
    switch (p[2]) {
    case 0x03: *first = 7; count = n >= 12 ? 1 : 0; break;     /* Output Terminal */
    case 0x06: case 0x0d: *first = 4; count = n >= 5 ? 1 : 0; break; /* Feature, SRC */
    case 0x07: *first = 6; count = n >= 7 ? 1 : 0; break;      /* Effect Unit */
    case 0x04: case 0x05:                                      /* Mixer, Selector */
        *first = 5; count = n >= 5 ? p[4] : 0; break;
    case 0x08: case 0x09:                                      /* Processing, Extension */
        *first = 7; count = n >= 7 ? p[6] : 0; break;
    default: return 0;
    }
    if (*first + count > n) return 0;
    return count;
}

/* Depth-first from `id` back to `terminal`; path[] collects the ids from
 * `id` on (path[0] is the Output Terminal). */
static int Walk(const UAC_DESC *d, unsigned int id, unsigned int terminal,
                UAC_BYTE *visited, UAC_BYTE *path, unsigned int depth, unsigned int *length)
{
    const UAC_BYTE *p;
    unsigned int first, count, i;
    if (depth >= WALK_DEPTH || !id || id > 255 || visited[id]) return 0;
    p = Entity(d, id);
    if (!p) return 0;
    path[depth] = (UAC_BYTE)id;
    if (id == terminal) {
        if (p[2] != 0x02) return 0;
        *length = depth + 1;
        return 1;
    }
    visited[id] = 1;
    count = Sources(p, &first);
    for (i = 0; i < count; i++)
        if (Walk(d, p[first + i], terminal, visited, path, depth + 1, length)) return 1;
    return 0;
}

/* A Feature Unit's logical channels (not counting the master), or -1. */
static int UnitChannels(const UAC_BYTE *p)
{
    if (!p || p[2] != 0x06 || p[0] < 10 || (p[0] - 6) % 4) return -1;
    return (p[0] - 6) / 4 - 1;
}

/* The control's access on logical channel `channel` of a Feature Unit. */
static int Access(const UAC_BYTE *fu, unsigned int channel, unsigned int selector)
{
    return UacCtlAccess(fu + 5 + 4 * channel, 4, selector);
}

static void Pick(const UAC_DESC *d, const UAC_BYTE *path, unsigned int length,
                 unsigned int channels, unsigned int selector, UAC_BYTE *unit,
                 unsigned int *mode, unsigned int *unitChannels, unsigned int *extra)
{
    unsigned int i, c;
    int n;
    *unit = 0; *mode = UAC_VOL_NONE; *unitChannels = 0; *extra = 0;
    for (i = 1; i + 1 < length; i++) {
        const UAC_BYTE *p = Entity(d, path[i]);
        if ((n = UnitChannels(p)) < 0 || Access(p, 0, selector) != 3) continue;
        *unit = path[i]; *mode = UAC_VOL_MASTER;
        for (c = 1; c <= (unsigned int)n && c <= UAC_VOL_CHANNELS; c++)
            if (Access(p, c, selector) == 3) *extra |= 1u << (c - 1);
        return;
    }
    for (i = 1; i + 1 < length; i++) {
        const UAC_BYTE *p = Entity(d, path[i]);
        int all = 1;
        if ((n = UnitChannels(p)) < 0) continue;
        if ((unsigned int)n < channels || n > UAC_VOL_CHANNELS) continue;
        for (c = 1; c <= (unsigned int)n; c++) if (Access(p, c, selector) != 3) all = 0;
        if (all) { *unit = path[i]; *mode = UAC_VOL_CHANNEL; *unitChannels = (unsigned int)n; return; }
    }
}

int UacVolFind(const UAC_DESC *desc, unsigned int terminal, unsigned int channels,
               UAC_VOL_MAP *map)
{
    UAC_BYTE visited[256], path[WALK_DEPTH];
    unsigned int id, i, length = 0;
    UAC_VOL_MAP m;
    if (!map) return 0;
    if (channels < 1) channels = 1;
    if (channels > UAC_VOL_CHANNELS) channels = UAC_VOL_CHANNELS;
    m.Channels = channels; m.Output = 0;
    m.VolumeUnit = m.MuteUnit = 0;
    m.VolumeMode = m.MuteMode = UAC_VOL_NONE;
    m.VolumeChannels = m.MuteChannels = 0;
    m.VolumeExtra = m.MuteExtra = 0;
    *map = m;
    if (!desc || !desc->Bytes || !terminal || terminal > 255) return 0;
    for (id = 1; id < 256; id++) {
        const UAC_BYTE *p = Entity(desc, id);
        if (!p || p[2] != 0x03 || p[0] < 12) continue;
        if (((unsigned int)p[4] | ((unsigned int)p[5] << 8)) == 0x0101) continue;
        for (i = 0; i < 256; i++) visited[i] = 0;
        if (!Walk(desc, id, terminal, visited, path, 0, &length)) continue;
        m.Output = (UAC_BYTE)id;
        Pick(desc, path, length, channels, 2, &m.VolumeUnit, &m.VolumeMode, &m.VolumeChannels,
             &m.VolumeExtra);
        Pick(desc, path, length, channels, 1, &m.MuteUnit, &m.MuteMode, &m.MuteChannels,
             &m.MuteExtra);
        *map = m;
        return 1;
    }
    return 0;
}

/* The highest value a subrange's steps reach. */
static long Top(long lo, long hi, long res)
{
    return res > 0 ? lo + ((hi - lo) / res) * res : lo;
}

int UacVolRange(const unsigned char *answer, unsigned int length, UAC_VOL_RANGE *out)
{
    UAC_CTL_RANGE r;
    UAC_VOL_RANGE v;
    unsigned char bytes[2 + 6 * UAC_VOL_RANGES];
    unsigned int i;
    int step = 0;
    if (!out || !answer || length < 2 || length > sizeof(bytes)) return 0;
    for (i = 0; i < length; i++) bytes[i] = answer[i];
    /* A RES of 0 on a range wider than one value reads as 1, as Linux
     * reads it ([linux] sound/usb/mixer.c, get_min_max_with_quirks). */
    for (i = 2; i + 6 <= length; i += 6)
        if (Word(bytes + i) < Word(bytes + i + 2) && !bytes[i + 4] && !bytes[i + 5]) bytes[i + 4] = 1;
    if (!UacCtlRange(bytes, length, 2, 1, &r) || r.Count > UAC_VOL_RANGES) return 0;
    {
        unsigned char *z = (unsigned char *)&v;
        for (i = 0; i < sizeof(v); i++) z[i] = 0;
    }
    v.Count = r.Count;
    for (i = 0; i < r.Count; i++) {
        const unsigned char *t = bytes + 2 + 6 * i;
        v.Min[i] = Word(t); v.Max[i] = Word(t + 2); v.Res[i] = Word(t + 4);
        if (v.Min[i] == UAC_VOL_SILENCE) {
            /* Silence is CUR's value, not a level of the range. */
            v.Min[i] = -32767;
            if (v.Max[i] < v.Min[i]) return 0;
            if (v.Max[i] == v.Min[i]) v.Res[i] = 0;
        }
        if (v.Res[i] > 0 && (!step || v.Res[i] < step)) step = v.Res[i];
    }
    v.KsMin = UacVolToKs(v.Min[0]);
    v.KsMax = UacVolToKs((int)Top(v.Min[v.Count - 1], v.Max[v.Count - 1], v.Res[v.Count - 1]));
    v.KsStep = (step ? step : 1) * 256L;
    *out = v;
    return 1;
}

void UacVolSoftRange(UAC_VOL_RANGE *out)
{
    unsigned int i;
    if (!out) return;
    for (i = 0; i < UAC_VOL_RANGES; i++) out->Min[i] = out->Max[i] = out->Res[i] = 0;
    out->Count = 1;
    out->Min[0] = -96 * 256; out->Max[0] = 0; out->Res[0] = 128;
    out->KsMin = UAC_VOL_SW_MIN; out->KsMax = UAC_VOL_SW_MAX; out->KsStep = UAC_VOL_SW_STEP;
}

long UacVolToKs(int value)
{
    if (value < -32767) value = -32767;
    if (value > 32767) value = 32767;
    return (long)value * 256L;
}

int UacVolFromKs(long level, const UAC_VOL_RANGE *r)
{
    long best = 0, bestDistance = -1;
    unsigned int i;
    if (!r || !r->Count) return 0;
    if (level < r->KsMin) level = r->KsMin;
    if (level > r->KsMax) level = r->KsMax;
    /* In KS units throughout: |level| < 2^23, no overflow. */
    for (i = 0; i < r->Count; i++) {
        long lo = r->Min[i], res = r->Res[i], top = Top(lo, r->Max[i], res), c, d;
        if (level <= lo * 256L) c = lo;
        else if (level >= top * 256L) c = top;
        else {
            /* res > 0 here (top > lo). The step at or below the level,
             * then the one above if nearer. */
            c = lo + ((level - lo * 256L) / (res * 256L)) * res;
            if (c + res <= top && (c + res) * 256L - level < level - c * 256L) c += res;
        }
        d = c * 256L > level ? c * 256L - level : level - c * 256L;
        if (bestDistance < 0 || d < bestDistance) { best = c; bestDistance = d; }
    }
    return (int)best;
}

unsigned long UacVolGain(long level)
{
    unsigned long att, whole, frac, a, b;
    if (level >= 0) return UAC_VOL_UNITY;
    if (level <= -96L * 65536L) return 0;
    att = (unsigned long)(-level);
    whole = att >> 16; frac = att & 0xFFFFUL;
    a = Table[whole]; b = whole < 96 ? Table[whole + 1] : 0;
    return a - (((a - b) * frac + 32768UL) >> 16);
}

void UacVolTargets(const long *levels, unsigned int channels, const UAC_VOL_MAP *map,
                   const UAC_VOL_RANGE *ranges, int *hardware)
{
    unsigned int c, n;
    long loudest;
    if (!levels || !map || !ranges || !hardware || !channels) return;
    if (channels > UAC_VOL_CHANNELS) channels = UAC_VOL_CHANNELS;
    for (n = 0; n < UAC_VOL_SLOTS; n++) hardware[n] = 0;
    loudest = levels[0];
    for (c = 1; c < channels; c++) if (levels[c] > loudest) loudest = levels[c];
    if (map->VolumeMode == UAC_VOL_MASTER)
        hardware[0] = UacVolFromKs(loudest, &ranges[0]);
    else if (map->VolumeMode == UAC_VOL_CHANNEL)
        for (n = 1; n <= map->VolumeChannels && n <= UAC_VOL_CHANNELS; n++)
            hardware[n] = UacVolFromKs(levels[(n - 1) % channels], &ranges[n]);
}

void UacVolGains(const long *levels, unsigned int channels, int mute, const UAC_VOL_MAP *map,
                 const int *actual, int hwMuted, unsigned long *gain)
{
    unsigned int c;
    if (!levels || !map || !actual || !gain || !channels) return;
    if (channels > UAC_VOL_CHANNELS) channels = UAC_VOL_CHANNELS;
    for (c = 0; c < channels; c++) {
        long done = 0;  /* what the device does to this channel, KS */
        if (map->VolumeMode == UAC_VOL_MASTER) done = UacVolToKs(actual[0]);
        else if (map->VolumeMode == UAC_VOL_CHANNEL) done = UacVolToKs(actual[c + 1]);
        gain[c] = UacVolGain(levels[c] - done);
        if (mute && !(hwMuted && map->MuteMode != UAC_VOL_NONE)) gain[c] = 0;
    }
}

void UacVolScale(unsigned char *bytes, unsigned long count, unsigned int subslot,
                 unsigned int channels, const unsigned long *gain, unsigned int gains)
{
    unsigned long i;
    unsigned int c = 0;
    if (!bytes || !gain || !gains || !channels || subslot < 2 || subslot > 4) return;
    for (i = 0; i + subslot <= count; i += subslot) {
        unsigned long g = gain[(c % channels) % gains];
        unsigned char *p = bytes + i;
        if (g < UAC_VOL_UNITY) {
            long s;
            UAC_CTL_INT x;
            if (subslot == 2) s = (long)(short)(p[0] | (p[1] << 8));
            else if (subslot == 3) s = (long)((unsigned long)p[0] | ((unsigned long)p[1] << 8) |
                                              ((unsigned long)(signed char)p[2] << 16));
            else s = (long)((unsigned long)p[0] | ((unsigned long)p[1] << 8) |
                            ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24));
            /* 32 x 32 to 64 bits, then an arithmetic shift: no helper call. */
            x = (UAC_CTL_INT)s * (long)g;
            s = (long)(x >> 16);
            p[0] = (unsigned char)s; p[1] = (unsigned char)(s >> 8);
            if (subslot >= 3) p[2] = (unsigned char)(s >> 16);
            if (subslot == 4) p[3] = (unsigned char)(s >> 24);
        }
        if (++c == channels) c = 0;
    }
}
