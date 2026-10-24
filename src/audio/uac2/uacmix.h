/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2002 by Takashi Iwai <tiwai@suse.de>
 * Upstream mixer.c includes code borrowed from audio.c by
 * Alan Cox (alan@lxorguk.ukuu.org.uk) and
 * Thomas Sailer (sailer@ife.ee.ethz.ch).
 * Copyright (C) 2026 Yeo Kheng Meng
 * Bounded portable API for Linux-v6.12 Mixer Unit bitmap adaptation.
 */
#ifndef UACMIX_H
#define UACMIX_H
#include "uacctl.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct UAC_MIX {
    const unsigned char *Descriptor; /* immutable, caller-owned */
    unsigned int Length,Unit,Pins,Inputs,Outputs,Cells,MatrixOffset,MatrixBytes;
    unsigned int ChannelConfig,ChannelNames,Controls,MixerName;
} UAC_MIX;
/* UAC2 only. Caller resolves every upstream input pin's logical channel
 * count using a cycle-checked topology. pinChannels must match baSourceID
 * order; this parser cannot establish that the external counts are correct.
 * Parses only bLength, permits caller storage containing later descriptors.
 * Rejects malformed lengths, node-count overflow, reserved control lanes
 * and nonzero matrix padding. Output unchanged on failure.
 */
int UacMixParse(const unsigned char *descriptor,unsigned int available,
    const unsigned int *pinChannels,unsigned int pinCount,UAC_MIX *out);
/* Zero-based matrix coordinates. Returns 0 invalid,1 fixed/readable,
 * 3 programmable/readable. MCN is zero-based and includes fixed nodes.
 */
int UacMixAccess(const UAC_MIX *mix,unsigned int input,unsigned int output);
int UacMixControlNumber(const UAC_MIX *mix,unsigned int input,unsigned int output,
    unsigned int *mcn);
/* Standard MU_MIXER_CONTROL (selector1), scalar signed16 CUR/RANGE.
 * Fixed nodes admit GET, programmable nodes also SET_CUR. SET_RANGE needs
 * separate capability evidence. Uses UacCtlSetup; performs no USB I/O.
 */
int UacMixSetup(const UAC_MIX *mix,unsigned int interfaceNumber,
    unsigned int input,unsigned int output,unsigned int attribute,unsigned int read,
    unsigned int length,unsigned int rangeWritable,unsigned char setup[8]);
#ifdef __cplusplus
}
#endif
#endif
