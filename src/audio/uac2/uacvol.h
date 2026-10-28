/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Playback volume and mute: the Feature Unit controls on the path from a
 * USB streaming Input Terminal to an Output Terminal (USB Audio 2.0
 * sections 4.7.2.8 and 5.2.5.7.1-2), the dB conversion between UAC 2.0
 * (signed 1/256 dB, 16 bits) and KS (signed 1/65536 dB, 32 bits;
 * KSPROPERTY_AUDIO_VOLUMELEVEL), and the software gain used where the
 * device has no writable volume or mute. No Windows types, allocation or
 * floating point; the driver and the host test compile it alike.
 */
#ifndef UACVOL_H
#define UACVOL_H
#include "uacdesc.h"
#include "uacctl.h"
#ifdef __cplusplus
extern "C" {
#endif

#define UAC_VOL_CHANNELS 8        /* node channels; unit channels this core writes */
#define UAC_VOL_SLOTS (UAC_VOL_CHANNELS + 1) /* [0] master, [n] logical channel n */
#define UAC_VOL_RANGES 8          /* RANGE subranges kept */
#define UAC_VOL_UNITY 65536UL     /* software gain, Q16 */
#define UAC_VOL_SILENCE (-32768)  /* volume CUR: silence (section 5.2.5.7.2) */
/* The software range (no writable Feature Unit volume): -96 dB to 0 dB
 * in 0.5 dB steps, KS units. At -96 dB the gain is 0. */
#define UAC_VOL_SW_MIN (-96L * 65536L)
#define UAC_VOL_SW_MAX 0L
#define UAC_VOL_SW_STEP 32768L

/* How the device offers one control on the path. */
#define UAC_VOL_NONE 0     /* not writable: software */
#define UAC_VOL_MASTER 1   /* logical channel 0 of the unit */
#define UAC_VOL_CHANNEL 2  /* logical channels 1..n of the unit */

typedef struct UAC_VOL_MAP {
    unsigned int Channels;          /* the node's channels, 1..UAC_VOL_CHANNELS */
    UAC_BYTE Output;                /* the Output Terminal the path ends at; 0 none */
    UAC_BYTE VolumeUnit, MuteUnit;  /* Feature Unit ids; 0 none */
    unsigned int VolumeMode, MuteMode;
    /* Channel mode: the unit's logical channels 1..n are written. */
    unsigned int VolumeChannels, MuteChannels;
    /* Master mode: bit n-1 set when the same unit's logical channel n
     * (n <= UAC_VOL_CHANNELS) has the control read/write too. The driver
     * sets those to 0 dB (unmuted) once it owns the master, so the master
     * alone sets the level. */
    unsigned int VolumeExtra, MuteExtra;
} UAC_VOL_MAP;

/* Walks back from each Output Terminal that is not a USB streaming one
 * (wTerminalType 0x0101) through units (Feature, Mixer, Selector, Effect,
 * Processing, Extension, Sampling Rate Converter) to the Input Terminal
 * `terminal`, and takes the first path found, in entity id order of the
 * Output Terminals and source order of each unit. On that path, the
 * Feature Unit nearest the Output Terminal with a read/write master
 * volume (mute) control is used; failing that, the nearest with the
 * control read/write on every logical channel it has (at least
 * `channels` of them; a unit with more than UAC_VOL_CHANNELS is not
 * used). channels is clamped to 1..UAC_VOL_CHANNELS. Returns 1 when a
 * path was found (the map may still say UAC_VOL_NONE), 0 when none (map:
 * no units, channels kept). The map is written in both cases. */
int UacVolFind(const UAC_DESC *desc, unsigned int terminal, unsigned int channels,
               UAC_VOL_MAP *map);

/* A volume RANGE answer (2-byte signed MIN/MAX/RES triples, section
 * 5.2.5.7.2) in KS units. The silence value -32768 is not a level: a MIN
 * of -32768 reads as its next step (MIN + RES; none without a RES). KsMax
 * is the highest value the steps reach. */
typedef struct UAC_VOL_RANGE {
    unsigned int Count;
    int Min[UAC_VOL_RANGES], Max[UAC_VOL_RANGES], Res[UAC_VOL_RANGES]; /* 1/256 dB */
    long KsMin, KsMax, KsStep;      /* KS: 1/65536 dB */
} UAC_VOL_RANGE;

/* 1 on a valid complete answer (UacCtlRange rules, at most UAC_VOL_RANGES
 * subranges; a RES of 0 on a subrange wider than one value reads as 1),
 * else 0 and out unchanged. */
int UacVolRange(const unsigned char *bytes, unsigned int length, UAC_VOL_RANGE *out);
/* The software range in the same form. */
void UacVolSoftRange(UAC_VOL_RANGE *out);

/* UAC 1/256 dB to KS 1/65536 dB; the silence value reads as -32767. */
long UacVolToKs(int value);
/* A KS level to the value the range allows (1/256 dB) nearest to it:
 * clamped to the range, then the nearest step of the nearest subrange;
 * a tie goes to the lower value. */
int UacVolFromKs(long level, const UAC_VOL_RANGE *range);

/* Q16 linear gain for a KS level: 0 dB and above is UAC_VOL_UNITY, -96 dB
 * and below 0; between, a table at every dB, linear between entries. */
unsigned long UacVolGain(long level);

/* The values to write for the node levels (slots as UAC_VOL_SLOTS):
 * master mode hardware[0], for the loudest channel, from ranges[0];
 * channel mode hardware[n] for logical channel n = 1..map->VolumeChannels,
 * node channel (n - 1) % channels, from ranges[n]. Other slots 0. */
void UacVolTargets(const long *levels, unsigned int channels, const UAC_VOL_MAP *map,
                   const UAC_VOL_RANGE *ranges, int *hardware);

/* The software gain of each stream channel k = 0..UAC_VOL_CHANNELS-1 (the
 * device's logical channel k + 1, node channel k % channels), given what
 * the device does now: actual[] in the slots of UacVolTargets (master
 * mode: actual[0], plus actual[k + 1] where VolumeExtra has channel
 * k + 1; channel mode: actual[k + 1] up to VolumeChannels), and muted[]
 * the mutes confirmed on (master: muted[0], plus muted[k + 1] where
 * MuteExtra has it; channel mode: muted[k + 1] up to MuteChannels). The
 * gain is the level less what the device does, never above unity (software
 * never raises a level); unity where the device is silent; 0 when mute
 * is asked and the device is not silent. A caller that does not know a
 * control's state passes the top of its range: never louder than asked. */
void UacVolGains(const long *levels, unsigned int channels, int mute, const UAC_VOL_MAP *map,
                 const int *actual, const int *muted, unsigned long *gain);

/* Scales whole frames of little-endian signed PCM in place: subslot 2, 3
 * or 4 bytes, channel c of each frame by gain[c % gains] (Q16). A trailing
 * partial frame is scaled sample by sample in the same order. */
void UacVolScale(unsigned char *bytes, unsigned long count, unsigned int subslot,
                 unsigned int channels, const unsigned long *gain, unsigned int gains);

#ifdef __cplusplus
}
#endif
#endif
