/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original implementation; see uacplan.h for the sources. No DDK samples
 * or third-party code used.
 */
#include "uacplan.h"

unsigned int UacPlanChannelMask(unsigned int config, unsigned int channels)
{
    unsigned int ones = 0, m = config & 0x7ffffffU;
    /* D31 (raw data): no positions. */
    if (config & 0x80000000U) return 0;
    while (m) { ones += m & 1; m >>= 1; }
    /* More named positions than channels: malformed, none kept. */
    if (ones > channels) return 0;
    /* Channels follow the named positions in bit order, then the unnamed
     * ones; positions 18..26 have no Windows bit and come after 0..17, so
     * the 0..17 prefix keeps its place and the rest stay unassigned. */
    return config & 0x3ffff;
}

unsigned int UacPlanCapacity(unsigned int maxPacket)
{
    return (maxPacket & 0x7ff) * (1 + ((maxPacket >> 11) & 3));
}

static int Runnable(const UAC_ALT *a, int highSpeed)
{
    unsigned int sync = UacPlanSync(a);
    if (highSpeed ? (a->Interval < 1 || a->Interval > 4) : a->Interval != 1)
        return 0;
    if (!(a->DataEndpoint & 0x80) && sync == 1) {
        /* Explicit feedback: 4 bytes (16.16) at High Speed, 3 (10.14) at
         * Full Speed, an interval the feedback slots can poll. */
        if (!a->FeedbackEndpoint) return 0;
        if (UacPlanCapacity(a->FeedbackMaxPacket) < (highSpeed ? 4U : 3U)) return 0;
        if (highSpeed ? (a->FeedbackInterval < 1 || a->FeedbackInterval > 4) :
                        a->FeedbackInterval < 1 || a->FeedbackInterval > 4) return 0;
    }
    if (UacPlanCapacity(a->MaxPacket) > (highSpeed ? 3072U : 1023U)) return 0;
    return 1;
}

unsigned int UacPlanFormats(const UAC_DESC *desc, int capture, int highSpeed,
                            UAC_FORMAT *out, unsigned int max)
{
    unsigned int i, j, n = 0;
    if (!desc || !out) return 0;
    for (i = 0; i < desc->AltCount && n < max; i++) {
        const UAC_ALT *a = &desc->Alt[i];
        if (((a->DataEndpoint & 0x80) != 0) != (capture != 0)) continue;
        if (!Runnable(a, highSpeed)) continue;
        for (j = 0; j < n; j++)
            if (out[j].Channels == a->Channels && out[j].Subslot == a->Subslot &&
                out[j].Bits == a->Bits && out[j].Terminal == a->Terminal &&
                out[j].ChannelConfig == a->ChannelConfig) break;
        if (j < n) continue;
        out[n].Channels = a->Channels; out[n].Subslot = a->Subslot;
        out[n].ChannelConfig = a->ChannelConfig;
        out[n].Bits = a->Bits; out[n].Terminal = a->Terminal;
        n++;
    }
    return n;
}

unsigned int UacPlanPacketBytes(unsigned int rate, int highSpeed,
                                unsigned int interval, unsigned int frameBytes,
                                int headroom)
{
    unsigned int base = highSpeed ? 8000U : 1000U, period, frames;
    if (rate < 8000 || rate > 384000 || !frameBytes || frameBytes > 64 ||
        !interval || interval > (highSpeed ? 4U : 1U)) return 0;
    period = 1U << (interval - 1);
    /* rate * period <= 384000 * 8: no overflow. */
    frames = (rate * period + base - 1) / base + (headroom ? 1 : 0);
    return frames * frameBytes;
}

unsigned int UacPlanTerminal(const UAC_DESC *desc, int capture, int highSpeed)
{
    unsigned int i;
    if (!desc) return 0;
    for (i = 0; i < desc->AltCount; i++) {
        const UAC_ALT *a = &desc->Alt[i];
        if (((a->DataEndpoint & 0x80) != 0) == (capture != 0) && Runnable(a, highSpeed))
            return a->Terminal;
    }
    return 0;
}

int UacPlanPick(const UAC_DESC *desc, int capture, unsigned int terminal,
                unsigned int channels, unsigned int subslot, unsigned int bits,
                unsigned int config, unsigned int rate, int highSpeed, unsigned int *index)
{
    unsigned int i, need, best = 0, bestCap = 0;
    int found = 0;
    if (!desc || !index) return UAC_NO_AUDIO;
    for (i = 0; i < desc->AltCount; i++) {
        const UAC_ALT *a = &desc->Alt[i];
        unsigned int cap;
        if (((a->DataEndpoint & 0x80) != 0) != (capture != 0)) continue;
        if (a->Channels != channels || a->Subslot != subslot || a->Bits != bits) continue;
        if (config != UAC_PLAN_ANY_CONFIG && a->ChannelConfig != config) continue;
        if (terminal && a->Terminal != terminal) continue;
        if (!Runnable(a, highSpeed)) continue;
        need = UacPlanPacketBytes(rate, highSpeed, a->Interval,
                                  (unsigned int)a->Channels * a->Subslot, UacPlanSync(a) == 1);
        cap = UacPlanCapacity(a->MaxPacket);
        if (!need || cap < need) continue;
        if (!found || cap < bestCap) { best = i; bestCap = cap; found = 1; }
    }
    if (!found) return UAC_NO_AUDIO;
    *index = best;
    return UAC_OK;
}
