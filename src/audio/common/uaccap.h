/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * The capture graph, read only (docs/design/uac2.md sections 28.3 to 28.5
 * and 28.12): every route from a capture endpoint's USB streaming Output
 * Terminal back to an Input Terminal, each entity classified, the chooser
 * and its sources, which Feature Unit controls are eligible to carry a
 * source's level, the adapter-wide claims table built once in its fixed
 * order, and the route diagnostics of Uac2VolMapC. Nothing here writes a
 * device: the graph is computed from the descriptors once and is not
 * changed after; positions are the caller's (the route table's). The
 * driver calls UacCapStart only (design 28.18, stage 1: discovery and the
 * Uac2VolMapC record); nothing else here is called by a driver file yet.
 * No Windows types, allocation or floating
 * point; compiled with UAC_V1 for USB Audio 1.0 layouts. For the driver
 * (design 28.7, "Integration contracts"): UacCapBuild and UacCapClaims
 * run at PASSIVE (PnP start); a UAC_CAP_GRAPH (7424 bytes) and the claims
 * table are allocated from nonpaged pool beforehand, never kernel-stack
 * locals.
 */
#ifndef UACCAP_H
#define UACCAP_H
#include "uacvol.h"
#ifdef __cplusplus
extern "C" {
#endif

#define UAC_CAP_DEPTH 32        /* entities per route */
#define UAC_CAP_ROUTES 64       /* routes per endpoint */
#define UAC_CAP_SOURCES 8       /* B3: more and there is no chooser */
#define UAC_CAP_SELECTORS 8     /* (selector, input) pairs per source; selectors listed */

/* Entity classes (28.3's table). */
#define UAC_CAP_PASS 1          /* Feature, SRC, Effect, a one-input Selector */
#define UAC_CAP_OPAQUE 2        /* a one-input Processing or Extension Unit */
#define UAC_CAP_JOIN 3          /* a Mixer Unit */
#define UAC_CAP_OPAQUE_JOIN 4   /* a Processing or Extension Unit with two or more inputs */
#define UAC_CAP_CHOICE 5        /* a Selector Unit with two or more inputs */
#define UAC_CAP_END 6           /* an Input Terminal */
#define UAC_CAP_OUTPUT 7        /* the route's own Output Terminal (Id[0]) */

/* GraphFlags (Uac2VolMapC offset 252). */
#define UAC_CAP_MANY_ROUTES 0x01u
#define UAC_CAP_TOO_DEEP 0x02u
#define UAC_CAP_MALFORMED 0x04u
#define UAC_CAP_UNLISTED 0x08u
#define UAC_CAP_MANY_SOURCES 0x10u
#define UAC_CAP_CHOOSER_HELD 0x20u
#define UAC_CAP_LOOP 0x40u
#define UAC_CAP_NOT_BUILT 0x80u /* no graph: its storage could not be allocated (28.18) */
#define UAC_CAP_INCOMPLETE (UAC_CAP_MANY_ROUTES | UAC_CAP_TOO_DEEP | UAC_CAP_MALFORMED | \
                            UAC_CAP_UNLISTED | UAC_CAP_LOOP | UAC_CAP_NOT_BUILT)

/* One route: Id[0] the Output Terminal, Id[Length - 1] the End (or where
 * the route was cut); Pin[i] the one-based input of Id[i] the route takes
 * to reach Id[i + 1] (0 on the last). Cut: 0 when it reaches an End, else
 * the GraphFlags bit that cut it. */
typedef struct UAC_CAP_ROUTE {
    unsigned int Length, Cut;
    UAC_BYTE Id[UAC_CAP_DEPTH], Pin[UAC_CAP_DEPTH], Class[UAC_CAP_DEPTH];
} UAC_CAP_ROUTE;

/* A source of the chooser: the (selector, input) pairs met before any
 * join, the chooser first; the first complete route's Input Terminal and
 * its wTerminalType. */
typedef struct UAC_CAP_SOURCE {
    unsigned int Count;
    UAC_BYTE Selector[UAC_CAP_SELECTORS], Input[UAC_CAP_SELECTORS];
    UAC_BYTE End;
    unsigned int Type;
} UAC_CAP_SOURCE;

typedef struct UAC_CAP_GRAPH {
    UAC_BYTE Output;                /* the USB streaming Output Terminal */
    unsigned int Flags;             /* GraphFlags */
    unsigned int RouteCount;        /* routes kept (at most UAC_CAP_ROUTES) */
    UAC_CAP_ROUTE Route[UAC_CAP_ROUTES];
    UAC_BYTE Chooser;               /* 0: none (no MUX) */
    unsigned int SourceCount;       /* 0 or 2..UAC_CAP_SOURCES */
    UAC_CAP_SOURCE Source[UAC_CAP_SOURCES];
    unsigned int SelectorCount;     /* every Choice on any route, first 8 in route order */
    UAC_BYTE Selector[UAC_CAP_SELECTORS];
    /* Each entity's output channel count as the walk established it, to
     * check the layouts that depend on it (a Mixer's bmMixerControls, a
     * Feature or Effect Unit's per-channel array): 0 not read, 1 to 255
     * the count, 0x100 the walk cuts a route at or above it (a loop, a
     * missing, unlisted or short entity), 0x200 none defined (a Selector
     * whose inputs differ, a count of 0). */
    UAC_WORD Width[256];
} UAC_CAP_GRAPH;

/* Builds the graph of the USB streaming Output Terminal `output`
 * (wTerminalType 0x0101). Returns 1 when the graph is complete, 0 when it
 * is incomplete (Flags says why; a malformed or absent Output Terminal
 * included). Every route is kept for the diagnostics either way. */
int UacCapBuild(const UAC_DESC *desc, unsigned int output, UAC_CAP_GRAPH *graph);

/* Whether route r belongs to source s (1..SourceCount): its pairs met
 * before any join are the source's. With no chooser, every complete route
 * belongs to "source" 1. */
int UacCapInSource(const UAC_CAP_GRAPH *graph, unsigned int r, unsigned int s);

/* A hardware control (28.5): an entity's control selector on one channel
 * (0 the master; a selector's control is selector 1, channel 0). */
typedef struct UAC_CAP_CONTROL {
    UAC_BYTE Entity, Selector, Channel;
} UAC_CAP_CONTROL;

/* The claims table (28.5): one holder per hardware control. Holders are
 * endpoint numbers 0..; Kind says what for. Sealed after the build: no
 * later claim. */
#define UAC_CLAIM_LEVEL 1       /* a level control (playback or capture) */
#define UAC_CLAIM_SELECTOR 2    /* a chooser or inner selector */
#define UAC_CLAIM_NORMAL 3      /* a playback normalisation target (28.9) */
#define UAC_CLAIM_RESERVED 4    /* a first-arrival pass reservation (28.6) */
#define UAC_CLAIM_MAX 512
typedef struct UAC_CLAIMS {
    unsigned int Count, Sealed, Refused;
    UAC_CAP_CONTROL Control[UAC_CLAIM_MAX];
    UAC_BYTE Holder[UAC_CLAIM_MAX], Kind[UAC_CLAIM_MAX];
} UAC_CLAIMS;
void UacClaimInit(UAC_CLAIMS *t);
/* 1 when this call made `holder` the control's holder; 0 when it is held
 * already (by anyone), the table is sealed or full (Refused counted). */
int UacClaimTake(UAC_CLAIMS *t, const UAC_CAP_CONTROL *c, unsigned int holder, unsigned int kind);
/* The holder, or -1 when nobody holds it; *kind (may be NULL) its kind. */
int UacClaimHolder(const UAC_CLAIMS *t, const UAC_CAP_CONTROL *c, unsigned int *kind);
/* 1 when `holder` may write the control: nobody else holds it. */
int UacClaimMay(const UAC_CLAIMS *t, const UAC_CAP_CONTROL *c, unsigned int holder);

/* A capture source's level controls (28.4): Mode UAC_VOL_MASTER (channel
 * 0), UAC_VOL_CHANNEL (channels 1..Channels) or UAC_VOL_NONE (software). */
typedef struct UAC_CAP_LEVEL {
    UAC_BYTE VolumeUnit, MuteUnit;
    unsigned int VolumeMode, MuteMode, VolumeChannels, MuteChannels;
} UAC_CAP_LEVEL;
/* Picks source s's level controls among the eligible ones (whole-signal
 * coverage, no join that is opaque downstream, per-channel only across
 * one-input Pass entities, where the cluster correspondence is proved:
 * never across a multi-input Selector; held by nobody but `holder` in t,
 * which may be NULL): volume and mute each on the eligible unit nearest the
 * Output Terminal with a read/write master, else with the control
 * read/write on every logical channel (at least `channels`, at most
 * UAC_VOL_CHANNELS). An incomplete graph picks nothing. */
void UacCapPick(const UAC_DESC *desc, const UAC_CAP_GRAPH *graph, unsigned int s,
                unsigned int channels, const UAC_CLAIMS *t, unsigned int holder, UAC_CAP_LEVEL *out);

/* One endpoint's part in the claims build (28.5): a playback endpoint's
 * level and normalised controls, or a capture endpoint's graph. */
typedef struct UAC_CAP_ENDPOINT {
    int Capture;
    unsigned int Channels;                  /* the node's channels */
    unsigned int LevelCount, NormalCount;   /* playback */
    const UAC_CAP_CONTROL *Level, *Normal;
    const UAC_CAP_GRAPH *Graph;             /* capture */
} UAC_CAP_ENDPOINT;
/* Builds the table for every endpoint, holder = index in eps, in 28.5's
 * order: each playback endpoint's level controls; each capture endpoint's
 * chooser and inner selectors, then its sources' level controls (picked
 * against the table so far); each playback endpoint's normalised
 * controls; each capture endpoint's first-arrival pass targets (every
 * read/write mute and volume, master and channels, of every Feature Unit
 * on each source's routes) still unheld, reserved. An incomplete graph
 * claims nothing. Then sealed. */
void UacCapClaims(const UAC_DESC *desc, const UAC_CAP_ENDPOINT *eps, unsigned int count, UAC_CLAIMS *t);

/* A Feature Unit's logical channels (not the master), -1 when the entity
 * is not a well-formed Feature Unit; the access (3 read/write, 1 read
 * only, 0 none) of control `selector` on `channel` (0 the master). */
int UacCapUnitChannels(const UAC_DESC *desc, unsigned int unit);
int UacCapAccess(const UAC_DESC *desc, unsigned int unit, unsigned int channel, unsigned int selector);

/* The source the positions select (positions[id], 0 unknown): 1..N when
 * every (selector, input) of exactly that source reads so, else 0. */
unsigned int UacCapConfirmed(const UAC_CAP_GRAPH *graph, const UAC_BYTE *positions);

/* Uac2VolMapC's route fields (28.12), little-endian into a 412-byte
 * record: Sources (232), Confirmed (236), RouteState (244: 3 incomplete,
 * 1 unknown, else 0), GraphFlags (252), Selector[8] at 256 + 12i (Id,
 * Position, Status) from positions[] and status[] (either may be NULL:
 * 0). Other fields are left as they are. */
#define UAC_CAP_DIAG_SIZE 412
void UacCapDiag(const UAC_CAP_GRAPH *graph, const UAC_BYTE *positions, const long *status,
                unsigned char *record);

/* Stage 1 (design 28.18): a capture endpoint's discovery at PnP start.
 * Fills the whole UAC_CAP_DIAG_SIZE-byte Uac2VolMapC record from nothing:
 * Channels (offset 0, 1 or 2 from `channels`), Output (4, `output`),
 * Gain[8] (116 to 144) at unity, since the driver does not scale captured
 * samples, then the graph's route fields as UacCapDiag gives them with no
 * position read (no request is made: Position 0, Status 0, Confirmed 0;
 * RouteState 1 for a chooser, 0 without one, 3 for an incomplete graph);
 * every other field 0. `graph` NULL (its storage not allocated):
 * RouteState 3 and GraphFlags UAC_CAP_NOT_BUILT. Returns UacCapBuild's
 * answer (1 complete), 0 when there is no graph. Reads the descriptors
 * only. */
int UacCapStart(const UAC_DESC *desc, unsigned int output, unsigned int channels,
                UAC_CAP_GRAPH *graph, unsigned char *record);
/* The record's registry value name for endpoint pair `pair` (28.12):
 * "Uac2VolMapC" for pair 0, "Uac2VolMapCnn" (two upper-case hex digits)
 * for pair nn; UTF-16 code units, zero-terminated, at most 14 with the
 * terminator. */
#define UAC_CAP_NAME_UNITS 14
void UacCapDiagName(unsigned int pair, unsigned short *name);

#ifdef __cplusplus
}
#endif
#endif
