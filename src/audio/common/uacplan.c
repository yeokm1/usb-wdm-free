/* SPDX-License-Identifier: GPL-2.0-or-later
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

unsigned int UacPlanRealtime(unsigned long raw)
{
    if (raw == UAC_QUEUE_NONE) return UAC_REALTIME_DEFAULT;
    return raw != 0 ? 1U : 0U;
}

unsigned int UacPlanDuty(unsigned __int64 elapsed, unsigned __int64 busy,
                         unsigned __int64 window, unsigned int demoted,
                         unsigned int *closed)
{
    *closed = 0;
    if (elapsed < window) {
        return demoted;
    }
    *closed = 1;
    if (demoted) {
        return 0;   /* one window demoted at most, then real time again */
    }
    if (busy > elapsed) {
        busy = elapsed;
    }
    return busy * 100U >= elapsed * UAC_DUTY_PERCENT ? 1U : 0U;
}

unsigned __int64 UacPlanCounts(unsigned __int64 hz, unsigned long n,
                               unsigned long perSecond)
{
    unsigned __int64 c = hz * n / perSecond;
    return c != 0 ? c : 1;
}

unsigned int UacPlanQueueMs(unsigned long raw)
{
    if (raw == UAC_QUEUE_NONE) return UAC_QUEUE_DEFAULT_MS;
    if (raw < UAC_QUEUE_MIN_MS) return UAC_QUEUE_MIN_MS;
    if (raw > UAC_QUEUE_MAX_MS) return UAC_QUEUE_MAX_MS;
    return (unsigned int)raw;
}

unsigned int UacPlanIsoLost(unsigned long urbStatus, unsigned int packets, unsigned int failed)
{
    return urbStatus == UAC_ISO_LOST_STATUS && packets && failed == packets;
}

unsigned int UacPlanDataRefused(unsigned long urbStatus, unsigned int packets)
{
    return urbStatus == UAC_ISO_BAD_START_STATUS && packets;
}

unsigned int UacPlanFbRefused(unsigned long urbStatus, unsigned int run)
{
    if (urbStatus != UAC_ISO_BAD_START_STATUS) return UAC_FB_NONE;
    return run < UAC_FB_RESTART_LIMIT - 1 ? UAC_FB_RESUBMIT : UAC_FB_OFF;
}

unsigned int UacPlanLostLimit(unsigned long spanUs)
{
    unsigned long n;
    if (!spanUs) spanUs = 1000UL;
    n = (UAC_LOST_LIMIT_MS * 1000UL + spanUs - 1) / spanUs;
    return n < 2 ? 2U : (unsigned int)n;
}

unsigned int UacPlanUsb11Hub(const unsigned short *name, unsigned int count)
{
    static const char want[] = "\\driver\\usbhub";
    unsigned int i;
    if (!name || count != sizeof(want) - 1) return 0;
    for (i = 0; i < count; i++) {
        unsigned short c = name[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned short)(c - 'A' + 'a');
        if (c != (unsigned char)want[i]) return 0;
    }
    return 1;
}

unsigned int UacPlanQueue(int highSpeed, unsigned int interval, unsigned int queueMs,
                          unsigned long bytesPerSecond, unsigned int frameBytes,
                          unsigned int capacity, unsigned long ringBytes,
                          unsigned int *packets, unsigned long *spanUs)
{
    unsigned int n, shift, ms;
    unsigned long span, urb, limit;
    shift = interval ? interval - 1 : 0;
    if (shift > 15) shift = 15;
    if (highSpeed) {
        n = shift < 6 ? 32U >> shift : 0;
        if (n < 8) n = 8;
        span = (125UL * n) << shift;
    } else {
        n = 4;
        span = 1000UL * n << shift;
    }
    if (packets) *packets = n;
    if (spanUs) *spanUs = span;
    queueMs = UacPlanQueueMs(queueMs);
    limit = ((unsigned long)queueMs * 1000UL + span - 1) / span;
    if (ringBytes) {
        /* span at most 32768 ms (bInterval 16): no product below wraps. */
        ms = (unsigned int)((span + 999) / 1000);
        urb = bytesPerSecond / 1000 * ms + (bytesPerSecond % 1000 * ms + 999) / 1000;
        urb += (urb + 7) / 8 + (unsigned long)frameBytes * n;
        if (capacity && urb > (unsigned long)capacity * n) urb = (unsigned long)capacity * n;
        if (!urb || urb >= ringBytes) limit = 0;  /* a ring under two URBs */
        else if ((ringBytes - urb) / urb < limit) limit = (ringBytes - urb) / urb;
    }
    if (limit < 2) limit = 2;
    if (limit > UAC_QUEUE_MAX_URBS) limit = UAC_QUEUE_MAX_URBS;
    return (unsigned int)limit;
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
    return UacPlanFormatsFor(desc, capture, highSpeed, 0, out, max);
}

unsigned int UacPlanFormatsFor(const UAC_DESC *desc, int capture, int highSpeed,
                               unsigned int terminal, UAC_FORMAT *out, unsigned int max)
{
    unsigned int i, j, n = 0;
    if (!desc || !out) return 0;
    for (i = 0; i < desc->AltCount && n < max; i++) {
        const UAC_ALT *a = &desc->Alt[i];
        if (((a->DataEndpoint & 0x80) != 0) != (capture != 0)) continue;
        if (terminal && a->Terminal != terminal) continue;
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

unsigned int UacPlanTerminals(const UAC_DESC *desc, int capture, int highSpeed,
                              unsigned int *out, unsigned int max)
{
    unsigned int i, j, n = 0;
    if (!desc || !out) return 0;
    for (i = 0; i < desc->AltCount && n < max; i++) {
        const UAC_ALT *a = &desc->Alt[i];
        if (((a->DataEndpoint & 0x80) != 0) != (capture != 0)) continue;
        if (!Runnable(a, highSpeed)) continue;
        for (j = 0; j < n; j++)
            if (out[j] == a->Terminal) break;
        if (j < n) continue;
        out[n++] = a->Terminal;
    }
    return n;
}

unsigned int UacPlanEndpoints(const UAC_DESC *desc, int capture, int highSpeed,
                              unsigned int *out, unsigned int max)
{
    unsigned int t[8], n, i, j;
    if (!out || !max) return 0;
    n = UacPlanTerminals(desc, capture, highSpeed, t, 8);
    if (n < 2) {
        if (n) out[0] = t[0];
        return n;
    }
    /* Over both directions: an interface whose alternates link two
     * terminals, a playback and a capture one included, would be switched
     * under one pair by another. */
    for (i = 0; i < desc->AltCount; i++) {
        const UAC_ALT *a = &desc->Alt[i];
        if (!Runnable(a, highSpeed)) continue;
        for (j = 0; j < desc->AltCount; j++) {
            const UAC_ALT *b = &desc->Alt[j];
            if (!Runnable(b, highSpeed)) continue;
            /* One interface, two terminals; or one terminal, two interfaces. */
            if ((a->Interface == b->Interface) != (a->Terminal == b->Terminal)) {
                out[0] = t[0];
                return 1;
            }
        }
    }
    for (i = 0; i < n && i < max; i++) out[i] = t[i];
    return i;
}

static const struct {
    unsigned short Vid, Pid;
    unsigned int Quirks;
} quirks[] = {
    { 0x0db0, 0x419c, UAC_QUIRK_ENDPOINTS },  /* MSI MPG X570S Carbon Max WiFi */
    { 0x0db0, 0xa073, UAC_QUIRK_ENDPOINTS },  /* MSI MAG X570S Torpedo Max */
    { 0x0db0, 0xcc78, UAC_QUIRK_ENDPOINTS },  /* MSI MAG B850M Mortar WiFi */
};

unsigned int UacPlanQuirks(unsigned int vid, unsigned int pid)
{
    unsigned int i;
    for (i = 0; i < sizeof(quirks) / sizeof(quirks[0]); i++)
        if (quirks[i].Vid == vid && quirks[i].Pid == pid) return quirks[i].Quirks;
    return 0;
}

#ifdef UAC_V1
int UacPlanHasRate(const UAC_ALT *a, unsigned int rate)
{
    unsigned int i;
    if (!a || !rate) return 0;
    if (!a->RateCount) return rate >= a->RateMin && rate <= a->RateMax;
    for (i = 0; i < a->RateCount && i < UAC1_RATES; i++)
        if (a->Rate[i] == rate) return 1;
    return 0;
}
#endif

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
#ifdef UAC_V1
        if (!UacPlanHasRate(a, rate)) continue;
#endif
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
