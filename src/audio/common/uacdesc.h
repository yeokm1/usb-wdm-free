/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original code from USB Audio 2.0 sections 4.7, 4.9 and 4.10.
 * No Windows types, allocation, packed structures or unaligned reads.
 */
#ifndef UACDESC_H
#define UACDESC_H

#define UAC_MAX_ALTS 128
#define UAC_OK 0
#define UAC_BAD_DESCRIPTOR 1
#define UAC_TOO_MANY_ALTS 2
#define UAC_NO_AUDIO 3
#define UAC_CLOCK_INVALID 4
#define UAC_CLOCK_SELECTION 5

typedef unsigned char UAC_BYTE;
typedef unsigned short UAC_WORD;

typedef struct UAC_ALT {
    UAC_BYTE Interface, Alternate, Terminal;
    UAC_BYTE Channels, Subslot, Bits;
    UAC_BYTE DataEndpoint, FeedbackEndpoint;
    UAC_BYTE Attributes, Interval, FeedbackInterval;
    UAC_WORD MaxPacket, FeedbackMaxPacket;
    unsigned int ChannelConfig; /* AS_GENERAL bmChannelConfig (UAC2 4.1) */
#ifdef UAC_V1
    /* USB Audio 1.0, uac1 only (docs/design/uac2.md section 26): the
     * alternate's own rates from its Type I format descriptor, discrete
     * (RateCount 1..UAC1_RATES) or continuous (RateCount 0, RateMin to
     * RateMax), and its class-specific endpoint bmAttributes (D0 Sampling
     * Frequency, D7 MaxPacketsOnly). ChannelConfig holds the stream's
     * cluster wChannelConfig. Entity offsets point at UAC 1.0 layouts. */
#define UAC1_RATES 32   /* uacfmt.h UAC_FMT_MAX_RATES: every rate the parser keeps */
    unsigned int RateCount, RateMin, RateMax, Rate[UAC1_RATES];
    UAC_BYTE EpControls;
#endif
} UAC_ALT;

typedef struct UAC_DESC {
    const UAC_BYTE *Bytes; /* caller owns the immutable configuration */
    unsigned int Length;
    UAC_BYTE Control, First, Count;
    UAC_WORD Entity[256]; /* descriptor offsets; zero means absent */
    unsigned int AltCount, Skipped;
    UAC_ALT Alt[UAC_MAX_ALTS];
#ifdef UAC_V1
    /* The bus speed uac1 parsed the function for: 1 Full, 2 High Speed. */
    unsigned int Speed;
#endif
} UAC_DESC;

/* Only the caller's function is parsed. Count must include Control.
 * On failure the output must not be consumed. Length may include trailing
 * transport storage; wTotalLength is the parsing boundary.
 */
int UacParse(const UAC_BYTE *bytes, unsigned int length,
             unsigned int control, unsigned int first, unsigned int count,
             UAC_DESC *result);

/* Find the function anchored by the PDO's MI number. Supports a full
 * configuration with an IAD and a parent-filtered/no-IAD configuration.
 * The latter must start at that AC interface and have consecutive AS
 * interfaces; a second AC or foreign interface ends the inferred scope.
 */
int UacFunction(const UAC_BYTE *bytes, unsigned int length,
                unsigned int first, unsigned int *control, unsigned int *count);

/* A whole-device PDO has no MI number: 98 SE under NUSB's EHCI, whose hub
 * does not split a class EF/02/01 (IAD) device, binds the device itself.
 * Its audio function starts at the first UAC2 audio IAD's bFirstInterface,
 * else at the first UAC2 AudioControl interface (alternate 0).
 */
int UacWholeFirst(const UAC_BYTE *bytes, unsigned int length, unsigned int *first);

/* Every interface of the PDO's configuration at alternate 0, in descriptor
 * order, with its endpoint count: what SELECT_CONFIGURATION lists, for a
 * function view and for the whole device alike (USBD may refuse a subset).
 * Fails on bad framing, a repeated alternate 0, more than max interfaces,
 * or a count other than bNumInterfaces.
 */
int UacAltZero(const UAC_BYTE *bytes, unsigned int length, UAC_BYTE *numbers,
               UAC_BYTE *pipes, unsigned int max, unsigned int *count);

/* selections[id] is a one-based CUR response for a multi-input selector.
 * A one-input selector needs no control request. Multipliers are rejected
 * until their ratio is implemented; never silently assume a 1:1 ratio.
 */
int UacClock(const UAC_DESC *desc, UAC_BYTE terminal,
             const UAC_BYTE *selections, UAC_BYTE *source);

/* The clock entities from a terminal to its clock source, in order: each
 * selector with the one-based input taken, each multiplier (UAC2 4.7.2.3;
 * the caller reads its numerator/denominator, only 1/1 is used). UacClock
 * is this walk without the path.
 */
#define UAC_CLOCK_HOPS 16
typedef struct UAC_CLOCK_PATH {
    UAC_BYTE Source, Count;
    UAC_BYTE Id[UAC_CLOCK_HOPS], Kind[UAC_CLOCK_HOPS], Pin[UAC_CLOCK_HOPS];
} UAC_CLOCK_PATH;
int UacClockPath(const UAC_DESC *desc, UAC_BYTE terminal,
                 const UAC_BYTE *selections, UAC_CLOCK_PATH *path);

/* The device's name (docs/design/uac2.md section 21). UacNameLanguage:
 * the language of string descriptor 0's list (USB 2.0 9.6.7) to read the
 * product string in: 0x0409 when listed, else the first; 0 when the
 * answer is not a valid list. UacName: a product string descriptor
 * (bLength, type 3, UTF-16LE) as a display name of at most max characters
 * (1..UAC_NAME_MAX) in out (room for max + 1, NUL-terminated): control
 * characters (below U+0020, U+007F to U+009F) read as spaces, surrogates,
 * U+FEFF, U+FFFE and U+FFFF are left out, runs of spaces become one and
 * the ends are trimmed; for pair > 0, " <pair + 1>" is appended, the name
 * cut before it to fit (trailing spaces trimmed again). Returns the
 * characters written, 0 (out empty) when the descriptor is malformed or
 * nothing printable is left: the INF's name stands. */
#define UAC_NAME_MAX 31
unsigned int UacNameLanguage(const UAC_BYTE *d, unsigned int length);
unsigned int UacName(const UAC_BYTE *d, unsigned int length, unsigned int pair,
                     UAC_WORD *out, unsigned int max);
#endif
