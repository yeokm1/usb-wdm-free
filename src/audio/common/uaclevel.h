/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * The playback level engine (docs/design/uac2.md sections 20 and 28.15):
 * the render line's VOLUME and MUTE state, what the device is known to
 * do, the writes that bring the device to the asked levels, the software
 * gain for the rest, the registry copy (Uac2Level, Uac2LevelPnn) and the
 * diagnostics (Uac2VolMap, the stats words 65 to 93). Moved out of
 * uacwdm.cpp's Wave without a change of behaviour; it talks to the device,
 * the registry, the level lock and the clock only through UAC_LEVEL_OPS,
 * so the host tests (leveltest) drive the same code with a fake device.
 * No Windows types, allocation or floating point. Compiled with UAC_V1
 * for USB Audio 1.0 (uac1), whose ranges are GET_MIN/MAX/RES.
 */
#ifndef UACLEVEL_H
#define UACLEVEL_H
#include "uacvol.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The registry copy (Version 2) and its Owned bits. */
#define UAC_LEVEL_VOLUME 1
#define UAC_LEVEL_MUTE 2
typedef struct UAC_LEVEL_SAVE {
    unsigned long Version, Channels, Owned;
    long Mute;
    long Level[UAC_VOL_CHANNELS];
} UAC_LEVEL_SAVE;

/* Uac2VolMap, build 25's 232 bytes (field for field as shipped). */
typedef struct UAC_LEVEL_DIAG {
    unsigned long Channels, Output, VolumeUnit, VolumeMode, VolumeChannels, VolumeExtra;
    unsigned long MuteUnit, MuteMode, MuteChannels, MuteExtra, Owned, Known, MuteKnown;
    long KsMin, KsMax, KsStep;
    unsigned long Change, Writes, Failures;
    long LastStatus;
    long Mute, Level[UAC_VOL_CHANNELS], Gain[UAC_VOL_CHANNELS];
    long Actual[UAC_VOL_SLOTS], ActualMute[UAC_VOL_SLOTS];
    long KsTop;
    unsigned long AboveZero, Lowered;
} UAC_LEVEL_DIAG;

/* The stats words 65 to 93 of a render record (uacusb.h's Vol* fields,
 * design 20.5), for the driver to copy. */
typedef struct UAC_LEVEL_STATS {
    unsigned long VolLayout, VolUnit, VolMode, VolChannels, VolAboveZero, VolLowered;
    unsigned long VolRangeRead, VolStartKnown;
    unsigned long VolCount[3];
    long VolMin[3], VolMax[3], VolRes[3], VolTop[3], VolOffered[3], VolStartCur[3];
} UAC_LEVEL_STATS;

/* What the engine needs of its host. A status is an NTSTATUS in the
 * driver: 0 or above is success. */
typedef struct UAC_LEVEL_OPS {
    void *Context;
    /* One Feature Unit request as Wave::Feature makes it: read 1 for a GET
     * (attribute: UAC_CTL_CUR, UAC_CTL_RANGE_ATTRIBUTE, or USB Audio 1.0's
     * GET_MIN 0x82 / MAX 0x83 / RES 0x84), 0 for a SET CUR; got NULL:
     * exactly size bytes or a failure. */
    long (*Feature)(void *context, UAC_BYTE unit, unsigned long selector, unsigned long channel,
                    int read, unsigned long attribute, void *bytes, unsigned long size,
                    unsigned long *got);
    int (*Ready)(void *context);            /* DeviceReady */
    int (*Running)(void *context);          /* the render stream has its transport */
    int (*Removed)(void *context);
    void (*Delay40)(void *context);         /* 40 ms: the queued USB audio plays out */
    void (*Lock)(void *context);            /* the level spin lock */
    void (*Unlock)(void *context);
    /* Gain[k] = gain[k] for every k, each store interlocked (the USB worker reads them) */
    void (*Publish)(void *context, long *target, const unsigned long *gain);
    unsigned long (*GetDword)(void *context, const char *name); /* GetDwordRaw */
    int (*GetValue)(void *context, int pair, void *data, unsigned long size); /* the pair's Uac2Level */
    void (*PutValue)(void *context, int pair, void *data, unsigned long size);
    void (*PutDiag)(void *context, void *data, unsigned long size);    /* Uac2VolMap */
    int (*Taken)(void *context, UAC_BYTE unit); /* an earlier pair drives it (pair > 0) */
} UAC_LEVEL_OPS;

/* The render line's levels (Wave's UAC_LEVEL without the driver's lock
 * and work item). Fields as shipped; see uaclevel.c. */
typedef struct UAC_LEVEL {
    const UAC_LEVEL_OPS *Ops;
    int Pair;                   /* 0: pair 0 (Uac2Level); n: Uac2LevelPnn */
    int Present;                /* a render line */
    unsigned long Channels;
    UAC_VOL_MAP Map;
    UAC_VOL_RANGE Range[UAC_VOL_SLOTS];
    long Level[UAC_VOL_CHANNELS];
    long Mute;
    int VolumeOwned, MuteOwned;
    int VolumeDirty, MuteDirty;
    unsigned long Change, Saved;
    int Actual[UAC_VOL_SLOTS];
    long ActualMute[UAC_VOL_SLOTS];
    int Known[UAC_VOL_SLOTS], MuteKnown[UAC_VOL_SLOTS];
    int VolumeNormal, MuteNormal;
    unsigned int NoRange;
    int AboveZero;
    int Lowered;
    unsigned int RangeRead, StartKnown;
    int StartActual[UAC_VOL_SLOTS];
    long Gain[UAC_VOL_CHANNELS];
    unsigned long Writes, Failures;
    long LastStatus;
} UAC_LEVEL;

/* The render formats LevelSetup reads: how many, the most channels of
 * any, and the first's terminal. */
void UacLevelSetup(UAC_LEVEL *v, const UAC_LEVEL_OPS *ops, int pair, const UAC_DESC *desc,
                   unsigned long formats, unsigned long most, unsigned int terminal);
int UacLevelStale(UAC_LEVEL *v);
/* ApplyLocked: PASSIVE, the operation mutex held. The one exception is the
 * exclusive startup: Prepare applies the levels without the operation
 * mutex before the pair is published, when no other caller can reach it. */
void UacLevelApply(UAC_LEVEL *v);
void UacLevelPowerDown(UAC_LEVEL *v);        /* PowerLocal leaving D0 */
void UacLevelStats(const UAC_LEVEL *v, UAC_LEVEL_STATS *s);
const UAC_VOL_RANGE *UacLevelKsRange(const UAC_LEVEL *v, unsigned long c);
/* KS GET (channel -1: the loudest) and SET (snapped, owned, Change + 1;
 * the caller applies). Both take the level lock. */
long UacLevelGet(UAC_LEVEL *v, int volume, long channel);
void UacLevelSet(UAC_LEVEL *v, int volume, long channel, long value);
/* KSPROPERTY_AUDIO_CPU_RESOURCES: 1 not the host CPU, 0 the host CPU. */
int UacLevelNotHostCpu(const UAC_LEVEL *v, int volume);

#ifdef __cplusplus
}
#endif
#endif
