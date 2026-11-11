/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * uac1 only (docs/design/uac2.md 26.12). Original code from the USB Audio
 * 1.0 Formats document 2.2.1, 2.2.5 and 2.2.6 (PCM8 unsigned; IEEE-754
 * single precision; A-law and mu-law by ITU-T G.711, the octets as G.711
 * transmits them; 8-bit samples left-justified in their subframe), USB
 * Audio 1.0 3.7.2.2 and 4.6.1.2. Integer only: no floating point.
 */
#include "uacconv.h"

/* v >> n rounding toward minus infinity, for negative v too. */
static int Floor(int v, unsigned int n)
{
    return v >= 0 ? v >> n : -((-v - 1) >> n) - 1;
}

static int Get16(const unsigned char *p)
{
    int v = (int)((unsigned int)p[0] | ((unsigned int)p[1] << 8));
    return v >= 32768 ? v - 65536 : v;
}

static void Put16(unsigned char *p, int v)
{
    p[0] = (unsigned char)((unsigned int)v & 0xff);
    p[1] = (unsigned char)(((unsigned int)v >> 8) & 0xff);
}

int UacConvKnown(unsigned int tag, unsigned int subframe, unsigned int bits)
{
    switch (tag) {
    case UAC_TAG_PCM: return subframe >= 2 && subframe <= 4 && bits && bits <= subframe * 8;
    case UAC_TAG_PCM8: case UAC_TAG_ALAW: case UAC_TAG_MULAW: return subframe >= 1 && subframe <= 4 && bits == 8;
    case UAC_TAG_FLOAT: return subframe == 4 && bits == 32;
    default: return 0;
    }
}

unsigned int UacConvRank(unsigned int tag)
{
    switch (tag) {
    case UAC_TAG_PCM: return 0;
    case UAC_TAG_FLOAT: return 1;
    case UAC_TAG_ALAW: case UAC_TAG_MULAW: return 2;
    case UAC_TAG_PCM8: return 3;
    default: return 255;
    }
}

/* G.711 A-law: a 13-bit sample (sign and 12-bit magnitude) in segments
 * 0..7, segment 0 and 1 in steps of 2, each later one twice the step of
 * the one before; the code is sign (1 positive), segment, 4-bit step,
 * with its even bits inverted (XOR 55h). */
unsigned int UacConvAlaw(int s)
{
    int x = Floor(s, 3);                     /* -4096..4095 */
    unsigned int sign = 0x80, m, seg = 0, code;
    if (x < 0) { sign = 0; x = -x - 1; }     /* magnitude 0..4095 */
    m = (unsigned int)x;
    while (seg < 7 && m >= (32U << seg)) seg++;
    code = seg << 4 | ((seg < 2 ? m >> 1 : m >> seg) & 15);
    return (sign | code) ^ 0x55;
}

int UacConvAlawDecode(unsigned int a)
{
    unsigned int seg, t;
    a = (a & 0xff) ^ 0x55;
    seg = (a >> 4) & 7;
    t = (a & 15) << 4;
    /* The middle of the step, in 16-bit units (13-bit << 3). */
    if (!seg) t += 8;
    else t = (t + 0x108) << (seg - 1);
    return (a & 0x80) ? (int)t : -(int)t;
}

/* G.711 mu-law: a 14-bit sample, biased by 33, in segments 0..7 of 16
 * steps each; the code is sign (0 negative), segment, step, every bit
 * inverted. */
unsigned int UacConvMulaw(int s)
{
    int x = Floor(s, 2);                     /* -8192..8191 */
    unsigned int mask = 0xff, m, seg = 0, code;
    if (x < 0) { mask = 0x7f; x = -x; }
    m = (unsigned int)x;
    m += 33;                                 /* 33..8225 */
    if (m > 8191) m = 8191;                  /* the top step */
    while (seg < 7 && m >= (64U << seg)) seg++;
    code = seg << 4 | ((m >> (seg + 1)) & 15);
    return (code ^ mask) & 0xff;
}

int UacConvMulawDecode(unsigned int u)
{
    unsigned int t;
    u = ~u & 0xff;
    t = (((u & 15) << 3) + 0x84) << ((u >> 4) & 7);
    return (u & 0x80) ? (int)0x84 - (int)t : (int)t - 0x84;
}

/* s / 32768 as IEEE-754 single precision, exact. */
unsigned long UacConvFloat(int s)
{
    unsigned long sign = 0, m, p = 0;
    if (!s) return 0;
    if (s < 0) { sign = 0x80000000UL; m = (unsigned long)(-(long)s); }
    else m = (unsigned long)s;
    while ((m >> (p + 1)) != 0) p++;         /* the top bit, 0..15 */
    return sign | ((127UL + p - 15UL) << 23) | ((m << (23 - p)) & 0x7fffffUL);
}

/* An IEEE-754 single as 16-bit PCM: times 32768, rounded to nearest
 * (half away from zero), saturated; NaN and denormals as 0. */
int UacConvFloatDecode(unsigned long bits)
{
    unsigned long e = (bits >> 23) & 0xff, m = (bits & 0x7fffffUL) | 0x800000UL, r;
    int negative = (bits & 0x80000000UL) != 0;
    if (e == 0xff && (bits & 0x7fffffUL)) return 0;      /* NaN */
    if (!e) return 0;                                    /* zero, denormal */
    /* value * 32768 = m * 2^(e - 150 + 15) = m >> (135 - e) */
    if (e >= 135) r = 0x10000UL;                         /* |x| >= 2^9: saturate */
    else if (135 - e > 25) r = 0;                        /* below 2^-17 */
    else r = (m + (1UL << (134 - e))) >> (135 - e);
    if (negative) return r >= 32768 ? -32768 : -(int)r;
    return r > 32767 ? 32767 : (int)r;
}

void UacConvOut(unsigned int tag, unsigned int subframe, const unsigned char *pcm, unsigned int n,
                unsigned char *wire)
{
    unsigned int i, k;
    for (i = 0; i < n; i++, wire += subframe) {
        int s = Get16(pcm + 2 * i);
        unsigned int b;
        switch (tag) {
        case UAC_TAG_FLOAT: {
            unsigned long f = UacConvFloat(s);
            wire[0] = (unsigned char)(f & 0xff);
            wire[1] = (unsigned char)((f >> 8) & 0xff);
            wire[2] = (unsigned char)((f >> 16) & 0xff);
            wire[3] = (unsigned char)((f >> 24) & 0xff);
            continue;
        }
        case UAC_TAG_PCM8:
            /* (s + 128) >> 8, saturated, offset by 128: floor((s + 32896) / 256). */
            b = (unsigned int)(s + 32896) >> 8;
            if (b > 255) b = 255;
            break;
        case UAC_TAG_ALAW: b = UacConvAlaw(s); break;
        case UAC_TAG_MULAW: b = UacConvMulaw(s); break;
        default: b = 0; break;
        }
        /* The 8 significant bits left-justified: the last byte. */
        for (k = 0; k + 1 < subframe; k++) wire[k] = 0;
        wire[subframe - 1] = (unsigned char)b;
    }
}

void UacConvIn(unsigned int tag, unsigned int subframe, const unsigned char *wire, unsigned int n,
               unsigned char *pcm)
{
    unsigned int i;
    for (i = 0; i < n; i++, wire += subframe) {
        unsigned int b = wire[subframe - 1];
        int s;
        switch (tag) {
        case UAC_TAG_PCM8: s = ((int)b - 128) * 256; break;
        case UAC_TAG_FLOAT:
            s = UacConvFloatDecode((unsigned long)wire[0] | ((unsigned long)wire[1] << 8) |
                                   ((unsigned long)wire[2] << 16) | ((unsigned long)wire[3] << 24));
            break;
        case UAC_TAG_ALAW: s = UacConvAlawDecode(b); break;
        case UAC_TAG_MULAW: s = UacConvMulawDecode(b); break;
        default: s = 0; break;
        }
        Put16(pcm + 2 * i, s);
    }
}

int UacConvCadence(UAC_CONV_CADENCE *c, unsigned long rate, int highSpeed, unsigned int interval,
                   unsigned long ff)
{
    unsigned long low, high;
    if (!c || rate < 8000 || rate > 384000 || !interval || interval > (highSpeed ? 4U : 1U)) return 0;
    if (ff) {
        /* Ff samples a frame in 10.14, or a microframe in 16.16: the rate
         * an adaptive source follows (UAC 1.0 3.7.2.2). */
        c->Base = highSpeed ? 65536UL : 16384UL;
        c->Step = ff << (interval - 1);      /* below 2^23 << 3: no overflow */
    } else {
        c->Base = highSpeed ? 8000UL : 1000UL;
        c->Step = rate << (interval - 1);    /* 384000 * 8: no overflow */
    }
    c->Acc = 0;
    c->Owed = 0;
    low = c->Step / c->Base;
    high = (c->Step + c->Base - 1) / c->Base;
    /* Formats 1.0 2.2.1: a whole nav allows nav - 1 to nav + 1. */
    if (low == high) { if (low) low--; high++; }
    c->Low = (unsigned int)low; c->High = (unsigned int)high;
    return 1;
}

/* The frames owed stay near zero for a source within 2.2.1's 1.5
 * samples; held to two packets' worth against a source that is not (a
 * debt beyond that is dropped, not repaid by later packets). */
static void Bound(UAC_CONV_CADENCE *c)
{
    long top = 2L * (long)c->High + 2;
    if (c->Owed > top) c->Owed = top;
    if (c->Owed < -top) c->Owed = -top;
}

unsigned int UacConvNominal(UAC_CONV_CADENCE *c)
{
    unsigned long n;
    c->Acc += c->Step;
    n = c->Acc / c->Base;
    c->Acc %= c->Base;
    c->Owed += (long)n;
    Bound(c);
    return (unsigned int)n;
}

void UacConvLost(UAC_CONV_CADENCE *c)
{
    c->Owed = 0;
}

void UacConvLostPackets(UAC_CONV_CADENCE *c, unsigned int packets)
{
    while (packets--) {
        (void)UacConvNominal(c);
        UacConvLost(c);
    }
}

unsigned int UacConvMpoFrames(UAC_CONV_CADENCE *c, const unsigned char *packet,
                              unsigned int bytes, unsigned int frameBytes, unsigned int *last)
{
    unsigned int frames, nonzero = 0, f, k, low, high, n;
    if (last) *last = 0;
    if (!c || !packet || !frameBytes) return 0;
    frames = bytes / frameBytes;
    for (f = frames; f && !nonzero; f--)
        for (k = 0; k < frameBytes; k++)
            if (packet[(f - 1) * frameBytes + k]) { nonzero = f; break; }
    low = nonzero > c->Low ? nonzero : c->Low;
    high = frames < c->High ? frames : c->High;
    if (low > high) n = high;                /* past the bounds, or a short packet */
    else if (c->Owed <= (long)low) n = low;
    else if (c->Owed >= (long)high) n = high;
    else n = (unsigned int)c->Owed;
    c->Owed -= (long)n;
    Bound(c);
    if (last) *last = nonzero;
    return n;
}

unsigned long UacConvFf(unsigned long rate, int highSpeed)
{
    if (rate < 8000 || rate > 384000) return 0;
    /* rate * 16384 / 1000 = rate * 2048 / 125; rate * 65536 / 8000 =
     * rate * 1024 / 125; rate * 2048 <= 786432000: no overflow. */
    return highSpeed ? (rate * 1024UL + 62UL) / 125UL : (rate * 2048UL + 62UL) / 125UL;
}
