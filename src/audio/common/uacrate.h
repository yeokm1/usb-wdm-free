/* SPDX-License-Identifier: GPL-2.0-or-later
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

/* HS: 4-byte little-endian16.16 samples/microframe. FS: by the length
 * received, 3-byte10.14 (USB2 section5.12.4.2) or 4-byte16.16, both
 * samples/frame (Windows' own UAC2 driver takes only the latter at FS).
 * Accept only within +/-12.5% of nominal and endpoint capacity. Return
 * 1accepted/0rejected; rejection preserves last valid cadence. No other
 * feedback-format quirks or implicit pairing.
 */
int UacRateFeedback(UAC_RATE *state, const unsigned char *bytes, unsigned int length);

/* Bytes to request per feedback packet: 4 at HS; at FS 4 when the
 * endpoint's capacity (bytes, not the encoded wMaxPacketSize field) holds
 * 4, else 3. 0 when the endpoint cannot carry the format.
 */
unsigned int UacRateFeedbackStride(int highSpeed, unsigned int capacity);

#define UAC_FEEDBACK_BAD 0
#define UAC_FEEDBACK_VALUE 1
#define UAC_FEEDBACK_KEEPALIVE 2
#define UAC_FEEDBACK_EMPTY 3
/* One successful feedback packet of a URB whose packets are `stride`
 * bytes apart in a `bufferBytes` buffer. A packet whose stride does not
 * lie in the buffer, or whose length is above the stride, is rejected
 * before the buffer is read, a zero-length one included. A zero-length
 * packet is a keepalive only once a valid rate is active; before that it
 * is UAC_FEEDBACK_EMPTY: the endpoint was serviced and answered nothing,
 * the nominal cadence goes on, and it is not counted as bad
 * (docs/design/uac2.md 25.6). Counts BadFeedback on rejection, as
 * UacRateFeedback does.
 */
int UacRateFeedbackPacket(UAC_RATE *state, const unsigned char *buffer,
    unsigned int bufferBytes, unsigned int offset, unsigned int length,
    unsigned int stride);

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
