/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original integer implementation from USB2 feedback format facts.
 */
#include "uacrate.h"

int UacRateInitialize(UAC_RATE *s, unsigned int rate, int highSpeed,
    unsigned int interval, unsigned int frameBytes, unsigned int maxPacket)
{
    unsigned int base, period, numerator, maxFrames;
    UAC_RATE empty = { 0 };
    if (!s) return 0;
    *s = empty;
    if (rate < 8000 || rate > 384000 || (highSpeed != 0 && highSpeed != 1) ||
        !frameBytes || frameBytes > 64 || !maxPacket || maxPacket > 3072 ||
        !interval || interval > (highSpeed ? 4u : 1u)) return 0;
    base = highSpeed ? 8000 : 1000;
    period = 1u << (interval - 1);
    numerator = rate * period;
    maxFrames = maxPacket / frameBytes;
    if ((numerator + base - 1) / base > maxFrames) return 0;
    s->Base = base;
    s->Period = period;
    s->FrameBytes = frameBytes;
    s->MaxFrames = maxFrames;
    s->Numerator = numerator;
    /* Split before shifting, avoiding a 32-bit rate<<16 overflow. */
    s->Nominal = ((rate / base) << 16) + ((rate % base) << 16) / base;
    s->HighSpeed = highSpeed;
    s->Ready = 1;
    return 1;
}

int UacRateFeedback(UAC_RATE *s, const unsigned char *bytes, unsigned int length)
{
    unsigned int value, step, tolerance;
    if (!s || !s->Ready) return 0;
    if (!bytes || length != (s->HighSpeed ? 4u : 3u)) goto bad;
    value = bytes[0] | ((unsigned int)bytes[1] << 8) | ((unsigned int)bytes[2] << 16);
    if (s->HighSpeed) value |= (unsigned int)bytes[3] << 24;
    else value <<= 2; /*10.14 ->16.16*/
    tolerance = s->Nominal / 8;
    if (value < s->Nominal - tolerance || value > s->Nominal + tolerance) goto bad;
    /* Validated rates and periods keep this product well below32 bits. */
    step = value * s->Period;
    if ((step >> 16) + ((step & 65535u) != 0) > s->MaxFrames) goto bad;
    if (!s->FeedbackActive) s->Phase = (s->Remainder << 16) / s->Base;
    s->FeedbackStep = step;
    s->FeedbackActive = 1;
    s->GoodFeedback++;
    return 1;
bad:
    s->BadFeedback++;
    return 0;
}

unsigned int UacRateNextBytes(UAC_RATE *s)
{
    unsigned int frames;
    if (!s || !s->Ready) return 0;
    if (!s->FeedbackActive) {
        s->Remainder += s->Numerator;
        frames = s->Remainder / s->Base;
        s->Remainder %= s->Base;
    } else {
        s->Phase += s->FeedbackStep;
        frames = s->Phase >> 16;
        s->Phase &= 65535u;
    }
    return frames * s->FrameBytes;
}
