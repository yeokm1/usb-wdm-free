/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * USB Audio Class 1.0 descriptor parser (ADC-1 1.0 sections 4.3 to 4.6,
 * FMT-1 1.0 section 2.2.5). Original code from the specification tables;
 * docs/design/uac2.md section 26. No Windows types, allocation, packed
 * structures or unaligned reads.
 */
#ifndef UAC1DESC_H
#define UAC1DESC_H
#include "uacfmt.h"
#ifdef __cplusplus
extern "C" {
#endif

#define UAC1_OK 0
#define UAC1_BAD_DESCRIPTOR 1
#define UAC1_TOO_MANY_ALTS 2
#define UAC1_NO_AUDIO 3
#define UAC1_NO_PATH 4

#define UAC1_MAX_COLLECTION 16

/* What the interface a PDO names (its MI_ number) is. */
#define UAC1_VIEW_NONE 0       /* not a UAC 1.0 audio interface */
#define UAC1_VIEW_CONTROL 1    /* an AudioControl interface: the function */
#define UAC1_VIEW_STREAMING 2  /* a lone AudioStreaming interface */
#define UAC1_VIEW_MIDI 3       /* a lone MIDIStreaming interface */

/* AC descriptor subtypes (ADC-1 A.5). */
#define UAC1_HEADER 1
#define UAC1_INPUT_TERMINAL 2
#define UAC1_OUTPUT_TERMINAL 3
#define UAC1_MIXER_UNIT 4
#define UAC1_SELECTOR_UNIT 5
#define UAC1_FEATURE_UNIT 6
#define UAC1_PROCESSING_UNIT 7
#define UAC1_EXTENSION_UNIT 8

#define UAC1_TERMINAL_STREAMING 0x0101

typedef struct UAC1_DESC {
    const unsigned char *Bytes;     /* caller owns the immutable configuration */
    unsigned int Length;            /* wTotalLength */
    unsigned char Control;          /* AC interface number */
    unsigned char CollectionCount;  /* bInCollection */
    unsigned char Collection[UAC1_MAX_COLLECTION];
    unsigned char MidiCount;        /* MIDIStreaming interfaces in the collection */
    unsigned char Interrupt;        /* AC interrupt endpoint address, 0 if none */
    unsigned short Entity[256];     /* descriptor offsets by id; 0 absent */
    unsigned int AltCount, Skipped;
    UAC_FMT_ALT Alt[UAC_FMT_MAX_ALTS];
} UAC1_DESC;

/* Classifies interface `number` (alternate 0) of the configuration. */
int Uac1View(const unsigned char *bytes, unsigned int length, unsigned int number);

/* A whole-device PDO (no MI_ number): the function is the first UAC 1.0
 * IAD's first interface when it is an AC interface, else the first AC
 * interface with protocol 0. */
int Uac1WholeFirst(const unsigned char *bytes, unsigned int length, unsigned int *first);

/* Parses the function headed by AC interface `control`: its header, its
 * entities, and every alternate of the AS interfaces the header lists.
 * Unsupported or malformed AS alternates are skipped and counted; a
 * malformed AC descriptor fails the function. highSpeed selects the
 * endpoint limits. On failure the output must not be consumed. */
int Uac1Parse(const unsigned char *bytes, unsigned int length, unsigned int control,
              int highSpeed, UAC1_DESC *result);

/* With no answer from the bus: which speed the function's descriptors
 * fit. Parses both ways and counts the alternates that can carry at least
 * one of their rates (UacFmtNeed <= UacFmtCapacity) at each speed. Returns
 * 1 and sets *highSpeed when exactly one speed fits; 0 when neither or
 * both fit (ambiguous: the caller decides, and records it). `scratch` is
 * caller storage (the parser's output is large). */
int Uac1GuessSpeed(const unsigned char *bytes, unsigned int length, unsigned int control,
                   UAC1_DESC *scratch, int *highSpeed);

/* The descriptor of entity `id`, or 0. */
const unsigned char *Uac1Entity(const UAC1_DESC *desc, unsigned int id);

/* A Feature Unit's logical channel count (bmaControls entries less the
 * master) and the control bitmap of `channel` (0 master), up to 4 bytes of
 * bControlSize. Returns 0 if `unit` is not a valid Feature Unit or the
 * channel is out of range. */
int Uac1FeatureChannels(const UAC1_DESC *desc, unsigned int unit, unsigned int *channels);
int Uac1FeatureBits(const UAC1_DESC *desc, unsigned int unit, unsigned int channel,
                    unsigned long *bits);

/* Walks upstream from entity `from` (through bSourceID / baSourceID) to
 * entity `to`. Fills path[] from `from` to `to` inclusive. Selector and
 * Mixer Units are searched input by input; a cycle or a missing id fails.
 * Returns UAC1_OK, UAC1_NO_PATH, or UAC1_BAD_DESCRIPTOR. */
int Uac1Path(const UAC1_DESC *desc, unsigned int from, unsigned int to,
             unsigned char *path, unsigned int max, unsigned int *count);

/* The first Feature Unit between a stream's terminal and the device side:
 * render (an OUT alternate): from any Output Terminal whose upstream path
 * reaches the streaming Input Terminal `link`; capture: upstream from the
 * streaming Output Terminal `link`. *unit = 0 when there is none (still
 * UAC1_OK). *terminal is the device-side terminal of the path. */
int Uac1StreamFeature(const UAC1_DESC *desc, unsigned int link, int capture,
                      unsigned int *unit, unsigned int *terminal);

/* uac1model.c (built with UAC_V1): the parsed function as the shared model
 * the driver runs on (src/audio/common/uacdesc.h): the configuration, the
 * AudioControl interface, the entity offsets, and the alternates the
 * driver carries - PCM (wFormatTag 1) with 2- to 4-byte subframes; an OUT
 * alternate with MaxPacketsOnly (D7) is left out in this version, counted
 * in Skipped with the parser's own. Each alternate's ChannelConfig is its
 * stream's cluster wChannelConfig. Returns UAC1_OK, or UAC1_NO_AUDIO when
 * no alternate is left (the output is still filled). */
struct UAC_DESC;
int Uac1Model(const UAC1_DESC *desc, int highSpeed, struct UAC_DESC *out);

#ifdef __cplusplus
}
#endif
#endif
