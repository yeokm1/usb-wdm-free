/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original code from USB 2.0 9.6.6 and 5.6.3 and the UAC 1.0 / 2.0
 * format descriptors (docs/design/uac2.md section 26).
 */
#include "uacfmt.h"

unsigned int UacFmtCapacity(unsigned int maxPacket, int highSpeed)
{
    unsigned int size = maxPacket & 0x7ff, extra = (maxPacket >> 11) & 3;
    if (maxPacket & 0xe000) return 0;
    if (!highSpeed) return (extra || size > 1023) ? 0 : size;
    /* USB 2.0 Table 9-14: two transactions need 513..1024 bytes each,
     * three 683..1024. */
    if (extra == 3 || size > 1024 || (extra == 1 && size < 513) || (extra == 2 && size < 683))
        return 0;
    return size * (extra + 1);
}

int UacFmtHasRate(const UAC_FMT_ALT *a, unsigned long rate)
{
    unsigned int i;
    if (!a || !rate) return 0;
    if (!a->RateCount) return rate >= a->RateMin && rate <= a->RateMax;
    for (i = 0; i < a->RateCount && i < UAC_FMT_MAX_RATES; i++)
        if (a->Rate[i] == rate) return 1;
    return 0;
}

unsigned int UacFmtNeed(const UAC_FMT_ALT *a, unsigned long rate, int highSpeed)
{
    unsigned long base = highSpeed ? 8000UL : 1000UL, period, frames, bytes;
    if (!a || !rate || rate > 768000UL || !a->Interval ||
        a->Interval > (highSpeed ? 16 : 255)) return 0;
    /* Full Speed isochronous bInterval is 1 (ADC-1 Table 4-20); High
     * Speed counts 2^(bInterval-1) microframes (USB 2.0 9.6.6). */
    if (highSpeed) {
        if (a->Interval > 4) return 0;
        period = 1UL << (a->Interval - 1);
    } else {
        if (a->Interval != 1) return 0;
        period = 1;
    }
    frames = (rate * period + base - 1) / base;
    /* Only an asynchronous endpoint runs on a clock other than the host's
     * cadence: its feedback (sink) or its own clock (source) may need one
     * frame more than nominal. Adaptive and synchronous sinks get the
     * nominal cadence (docs/design/uac2.md 26.3). */
    if (a->Sync == UAC_SYNC_ASYNC) frames++;
    bytes = frames * a->Channels * a->Subframe;
    return bytes > 0xffffUL ? 0 : (unsigned int)bytes;
}

int UacFmtPick(const UAC_FMT_ALT *alts, unsigned int count, int capture,
               unsigned int formatTag, unsigned int channels, unsigned int subframe,
               unsigned int bits, unsigned long rate, int highSpeed)
{
    unsigned int i, need, cap, best = 0;
    int found = -1;
    if (!alts || count > UAC_FMT_MAX_ALTS) return -1;
    for (i = 0; i < count; i++) {
        const UAC_FMT_ALT *a = &alts[i];
        if ((a->Capture != 0) != (capture != 0) || a->FormatTag != formatTag || a->Channels != channels ||
            a->Subframe != subframe || a->Bits != bits || !UacFmtHasRate(a, rate))
            continue;
        need = UacFmtNeed(a, rate, highSpeed);
        cap = UacFmtCapacity(a->MaxPacket, highSpeed);
        if (!need || cap < need) continue;
        /* The transport's planner limits (uacrate.c UacRateInitialize), so
         * every format offered or accepted can also start. */
        if (rate < 8000 || rate > 384000 || a->Channels * a->Subframe > 64 || cap > 3072) continue;
        if (found < 0 || cap < best) { found = (int)i; best = cap; }
    }
    return found;
}
