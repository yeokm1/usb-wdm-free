/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original bounded PCM packet cadence and normative USB audio feedback.
 */
#ifndef UACRATE_H
#define UACRATE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UAC_RATE {
    unsigned int Base, Period, FrameBytes, MaxFrames;
    unsigned int Numerator, Remainder, Nominal;
    unsigned int FeedbackStep, Phase;
    unsigned int GoodFeedback, BadFeedback;
    int FeedbackActive, Ready, HighSpeed;
} UAC_RATE;

/* Return 1 on success, 0 on invalid arguments. Nominal cadence uses an
 * exact rational accumulator. Supported initial limits: 8-384 kHz,
 * 1-64 bytes/PCM frame, payload <=3072 bytes, HS interval1-4 or FS1.
 * MaxPacket is payload capacity, not the encoded wMaxPacketSize field.
 */
int UacRateInitialize(UAC_RATE *state, unsigned int rate, int highSpeed,
    unsigned int interval, unsigned int frameBytes, unsigned int maxPacket);

/* HS: 4-byte little-endian16.16 samples/microframe. FS: 3-byte10.14
 * samples/frame. Accept only within +/-12.5% of nominal and endpoint
 * capacity. Return1accepted/0rejected; rejection preserves last valid
 * cadence. No speculative feedback-format quirks or implicit pairing.
 */
int UacRateFeedback(UAC_RATE *state, const unsigned char *bytes, unsigned int length);

/* One service interval's payload length, always whole PCM frames and
 * <=MaxPacket; returns0 if uninitialized. A fractional cadence below one
 * sample per interval can also produce a legal zero packet. One caller
 * owns all state.
 */
unsigned int UacRateNextBytes(UAC_RATE *state);

#ifdef __cplusplus
}
#endif
#endif
