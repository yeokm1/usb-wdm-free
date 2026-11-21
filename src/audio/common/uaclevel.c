/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * The playback level engine, moved out of uacwdm.cpp's Wave (uaclevel.h;
 * docs/design/uac2.md 28.15): every function below is the Wave method
 * of the same role, statement for statement, with the driver's calls
 * (Feature, DeviceReady, Running, the spin lock, the registry, the delay)
 * made through UAC_LEVEL_OPS. Behaviour is unchanged; the comments are the
 * shipped ones.
 */
#include "uaclevel.h"

#define UAC_LEVEL_ASSERT(e, n) typedef char n[(e) ? 1 : -1]
UAC_LEVEL_ASSERT(sizeof(UAC_LEVEL_SAVE) == 48, uacLevelSaveSize);
UAC_LEVEL_ASSERT(sizeof(UAC_LEVEL_DIAG) == 232, uacLevelDiagSize);

static void Zero(void *p, unsigned long n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

/* One Feature Unit request (the driver builds and checks the setup). */
static long Feature(UAC_LEVEL *v, UAC_BYTE unit, unsigned long selector, unsigned long channel, int read,
                    unsigned long attribute, void *bytes, unsigned long size, unsigned long *got)
{
    return v->Ops->Feature(v->Ops->Context, unit, selector, channel, read, attribute, bytes, size, got);
}

#ifdef UAC_V1
/* USB Audio 1.0: GET_MIN, GET_MAX and GET_RES (2 bytes each), held as
 * the one subrange of a RANGE answer, under UacVolRange's rules. */
static int ReadRangeOnce(UAC_LEVEL *v, UAC_BYTE unit, unsigned long channel, UAC_VOL_RANGE *range)
{
    unsigned char b[8];
    unsigned long i;
    b[0] = 1; b[1] = 0;
    for (i = 0; i < 3; i++)
        if (Feature(v, unit, 2, channel, 1, 0x82 + i, b + 2 + 2 * i, 2, 0) < 0) return 0;
    return UacVolRange(b, sizeof(b), range) ? 1 : 0;
}
#else
/* Room for UAC_VOL_RANGES subranges in one request; a device that
 * refuses it is asked for one subrange, the wLength Linux uses
 * ([linux] sound/usb/mixer.c), and its first subrange is the range. */
static int ReadRangeOnce(UAC_LEVEL *v, UAC_BYTE unit, unsigned long channel, UAC_VOL_RANGE *range)
{
    unsigned char b[2 + 6 * UAC_VOL_RANGES];
    unsigned long got = 0, n;
    Zero(b, sizeof(b));
    if (Feature(v, unit, 2, channel, 1, UAC_CTL_RANGE_ATTRIBUTE, b, sizeof(b), &got) < 0 &&
        Feature(v, unit, 2, channel, 1, UAC_CTL_RANGE_ATTRIBUTE, b, 8, &got) < 0)
        return 0;
    if (got < 8) return 0;
    n = (unsigned long)b[0] | ((unsigned long)b[1] << 8);
    if (n > 1 && got < 2 + 6 * n) { n = 1; b[0] = 1; b[1] = 0; }
    if (!n || n > UAC_VOL_RANGES) return 0;
    return UacVolRange(b, 2 + 6 * n, range) ? 1 : 0;
}
#endif

static int ReadRange(UAC_LEVEL *v, UAC_BYTE unit, unsigned long channel, UAC_VOL_RANGE *range)
{
    if (!ReadRangeOnce(v, unit, channel, range)) return 0;
    if (channel < UAC_VOL_SLOTS) v->RangeRead |= 1u << channel;
    return 1;
}

/* CUR, raw: volume in 1/256 dB (UAC_VOL_SILENCE included), mute 0/1. */
static int ReadCur(UAC_LEVEL *v, UAC_BYTE unit, unsigned long selector, unsigned long channel, int *value)
{
    unsigned char b[2];
    unsigned long size = selector == 2 ? 2 : 1;
    if (Feature(v, unit, selector, channel, 1, UAC_CTL_CUR, b, size, 0) < 0 ||
        !UacCtlValue(UAC_CTL_FEATURE, selector, b, size, 0)) return 0;
    if (selector == 2) {
        int x = (int)((unsigned int)b[0] | ((unsigned int)b[1] << 8));
        *value = x >= 32768 ? x - 65536 : x;
    } else *value = b[0];
    return 1;
}

static int WriteCur(UAC_LEVEL *v, UAC_BYTE unit, unsigned long selector, unsigned long channel, int value)
{
    unsigned char b[2];
    long s;
    b[0] = (unsigned char)value; b[1] = (unsigned char)((unsigned int)value >> 8);
    s = Feature(v, unit, selector, channel, 0, UAC_CTL_CUR, b, selector == 2 ? 2 : 1, 0);
    v->Writes++;
    if (s < 0) { v->Failures++; v->LastStatus = s; }
    return s >= 0 ? 1 : 0;
}

/* The KS range of node channel c. */
const UAC_VOL_RANGE *UacLevelKsRange(const UAC_LEVEL *v, unsigned long c)
{
    return v->Map.VolumeMode == UAC_VOL_CHANNEL ? &v->Range[c + 1] : &v->Range[0];
}

/* A device value as a node level: silence is the bottom of the range. */
static long Level(const UAC_LEVEL *v, int value, unsigned long c)
{
    return value == UAC_VOL_SILENCE ? UacLevelKsRange(v, c)->KsMin : UacVolToKs(value);
}

/* Slot n (0 master, n logical channel n) carries the volume (mute)
 * control the driver writes: the main one, or an extra to normalize. */
static int VolumeSlot(const UAC_LEVEL *v, unsigned long n, int *extra)
{
    const UAC_VOL_MAP *m = &v->Map;
    *extra = 0;
    if (m->VolumeMode == UAC_VOL_MASTER) {
        if (!n) return 1;
        *extra = (m->VolumeExtra & (1u << (n - 1))) != 0;
        return *extra;
    }
    return m->VolumeMode == UAC_VOL_CHANNEL && n >= 1 && n <= m->VolumeChannels;
}

static int MuteSlot(const UAC_LEVEL *v, unsigned long n, int *extra)
{
    const UAC_VOL_MAP *m = &v->Map;
    *extra = 0;
    if (m->MuteMode == UAC_VOL_MASTER) {
        if (!n) return 1;
        *extra = (m->MuteExtra & (1u << (n - 1))) != 0;
        return *extra;
    }
    return m->MuteMode == UAC_VOL_CHANNEL && n >= 1 && n <= m->MuteChannels;
}

/* Reads every controlled slot not yet known (start, after D3, after
 * a write that failed: the device may or may not have taken it). */
static void ReadState(UAC_LEVEL *v)
{
    int extra;
    int value;
    unsigned long n;
    for (n = 0; n < UAC_VOL_SLOTS; n++) {
        if (VolumeSlot(v, n, &extra) && !v->Known[n] && ReadCur(v, v->Map.VolumeUnit, 2, n, &value)) {
            v->Actual[n] = value; v->Known[n] = 1;
        }
        if (MuteSlot(v, n, &extra) && !v->MuteKnown[n] && ReadCur(v, v->Map.MuteUnit, 1, n, &value)) {
            v->ActualMute[n] = value; v->MuteKnown[n] = 1;
        }
    }
}

/* What an unknown volume is taken as: the top the device's range
 * reaches (KsTop, above the 0 dB the driver offers, section 20); an
 * extra channel volume without a RANGE, the most a CUR can say (+128
 * dB). Gains also silences that channel while the volume is owned. */
static int Assumed(const UAC_LEVEL *v, unsigned long n)
{
    if (n && (v->NoRange & (1u << (n - 1)))) return 0x7FFF;
    return (int)(v->Range[n].KsTop / 256);
}

/* The gains for the device's state: an unknown volume as Assumed, an
 * unknown mute as off. With next (the values about to
 * be written, mute included), the lower of the two for each channel. */
static void Gains(UAC_LEVEL *v, const long *levels, long mute, const int *next, unsigned long *gain)
{
    int actual[UAC_VOL_SLOTS], muted[UAC_VOL_SLOTS];
    unsigned long after[UAC_VOL_CHANNELS];
    int extra;
    unsigned long n, k;
    UAC_VOL_MAP map = v->Map;
    long flat[UAC_VOL_CHANNELS];
    for (n = 0; n < UAC_VOL_SLOTS; n++) {
        actual[n] = v->Known[n] ? v->Actual[n] : Assumed(v, n);
        muted[n] = v->MuteKnown[n] && v->ActualMute[n];
    }
    if (!v->VolumeOwned) {
        /* Until a volume is set the device's own setting stands: no
         * software volume (an owned mute still applies). */
        map.VolumeMode = UAC_VOL_NONE; map.VolumeExtra = 0;
        for (k = 0; k < UAC_VOL_CHANNELS; k++) flat[k] = 0;
        levels = flat;
    }
    UacVolGains(levels, v->Channels, mute, &map, actual, muted, gain);
    /* An owned master whose extra channel volume has no RANGE and an
     * unknown CUR: that channel is silent until the CUR is read. */
    if (map.VolumeMode == UAC_VOL_MASTER)
        for (k = 0; k < UAC_VOL_CHANNELS; k++)
            if ((v->NoRange & (1u << k)) && !v->Known[k + 1]) gain[k] = 0;
    if (!next) return;
    for (n = 0; n < UAC_VOL_SLOTS; n++) {
        if (VolumeSlot(v, n, &extra)) actual[n] = next[n];
        if (MuteSlot(v, n, &extra)) muted[n] = extra ? 0 : (int)mute;
    }
    UacVolGains(levels, v->Channels, mute, &map, actual, muted, after);
    for (k = 0; k < UAC_VOL_CHANNELS; k++) if (after[k] < gain[k]) gain[k] = after[k];
}

static void Publish(UAC_LEVEL *v, const unsigned long *gain)
{
    v->Ops->Publish(v->Ops->Context, v->Gain, gain);
}

static int Ready(const UAC_LEVEL *v) { return v->Ops->Ready(v->Ops->Context); }

/* PnP start, after Learn: the render line's controls and levels. */
void UacLevelSetup(UAC_LEVEL *v, const UAC_LEVEL_OPS *ops, int pair, const UAC_DESC *desc,
                   unsigned long formats, unsigned long most, unsigned int terminal)
{
    UAC_VOL_MAP *m = &v->Map;
    UAC_LEVEL_SAVE saved;
    int extra;
    unsigned long c, n;
    Zero(v, sizeof(*v));
    v->Ops = ops; v->Pair = pair;
    for (c = 0; c < UAC_VOL_CHANNELS; c++) v->Gain[c] = (long)UAC_VOL_UNITY;
    if (!formats) return;
    if (most < 1) most = 1;
    /* Mono or a stereo pair: a stream with more channels takes channel
     * c % 2 (uacvol.h), as the system's mixer shows a pair. */
    v->Channels = most >= 2 ? 2 : 1;
    UacVolFind(desc, terminal, v->Channels, m);
    /* A Feature Unit an earlier pair drives stays that pair's: here the
     * level is software only, so no pair changes another's loudness
     * behind its back (section 15.3 item 6). */
    if (pair && m->VolumeUnit && ops->Taken(ops->Context, m->VolumeUnit)) {
        m->VolumeMode = UAC_VOL_NONE; m->VolumeUnit = 0;
        m->VolumeChannels = 0; m->VolumeExtra = 0;
    }
    if (pair && m->MuteUnit && ops->Taken(ops->Context, m->MuteUnit)) {
        m->MuteMode = UAC_VOL_NONE; m->MuteUnit = 0;
        m->MuteChannels = 0; m->MuteExtra = 0;
    }
    /* Every controlled channel's own RANGE; a control without a usable
     * one is left to software (an extra one is then not written). */
    if (m->VolumeMode == UAC_VOL_MASTER) {
        if (!ReadRange(v, m->VolumeUnit, 0, &v->Range[0])) m->VolumeMode = UAC_VOL_NONE;
        for (n = 1; n <= UAC_VOL_CHANNELS; n++)
            if ((m->VolumeExtra & (1u << (n - 1))) && !ReadRange(v, m->VolumeUnit, n, &v->Range[n]))
                v->NoRange |= 1u << (n - 1);
    } else if (m->VolumeMode == UAC_VOL_CHANNEL) {
        for (n = 1; n <= m->VolumeChannels; n++)
            if (!ReadRange(v, m->VolumeUnit, n, &v->Range[n])) m->VolumeMode = UAC_VOL_NONE;
    }
    if (m->VolumeMode == UAC_VOL_NONE) {
        m->VolumeUnit = 0; m->VolumeChannels = 0; m->VolumeExtra = 0;
        UacVolSoftRange(&v->Range[0]);
    }
    /* Uac2VolAboveZero, REG_DWORD 1: the device's whole range is
     * offered, gain above 0 dB included (build 24 and before); else
     * the top offered is the highest step at or below 0 dB (section 20). */
    v->AboveZero = ops->GetDword(ops->Context, "Uac2VolAboveZero") == 1;
    if (v->AboveZero)
        for (n = 0; n < UAC_VOL_SLOTS; n++) v->Range[n].KsMax = v->Range[n].KsTop;
    ReadState(v);
    for (n = 0; n < UAC_VOL_SLOTS; n++)
        if (v->Known[n]) { v->StartKnown |= 1u << n; v->StartActual[n] = v->Actual[n]; }
    /* A written control whose CUR, read now, is above the top offered
     * is set to that top, that control alone: a lowering only, nothing
     * taken over, no other control (a master-mode extra's attenuation
     * included) touched. A CUR not read is left as it is (section 20). */
    if (m->VolumeMode != UAC_VOL_NONE && Ready(v))
        for (n = 0; n < UAC_VOL_SLOTS; n++)
            if (VolumeSlot(v, n, &extra) && !(extra && (v->NoRange & (1u << (n - 1)))) && v->Known[n] &&
                v->Actual[n] != UAC_VOL_SILENCE && UacVolToKs(v->Actual[n]) > v->Range[n].KsMax) {
                int top = (int)(v->Range[n].KsMax / 256);
                if (WriteCur(v, m->VolumeUnit, 2, n, top)) { v->Actual[n] = top; v->Lowered = 1; }
                else v->Known[n] = 0;   /* may or may not have taken it: Assumed */
            }
    /* The levels: restored, else the device's own (0 dB in software). */
    for (c = 0; c < v->Channels; c++) {
        n = m->VolumeMode == UAC_VOL_MASTER ? 0 : c + 1;
        v->Level[c] = m->VolumeMode != UAC_VOL_NONE && v->Known[n] ? Level(v, v->Actual[n], c) : 0;
    }
    /* Muted when every controlled channel is known muted. */
    v->Mute = m->MuteMode != UAC_VOL_NONE;
    for (n = m->MuteMode == UAC_VOL_MASTER ? 0 : 1; n < UAC_VOL_SLOTS && v->Mute; n++)
        if (MuteSlot(v, n, &extra) && !extra && !(v->MuteKnown[n] && v->ActualMute[n])) v->Mute = 0;
    if (ops->GetValue(ops->Context, pair, &saved, sizeof(saved)) && saved.Version == 2 &&
        saved.Channels == v->Channels) {
        if (saved.Owned & UAC_LEVEL_VOLUME) {
            for (c = 0; c < v->Channels; c++) v->Level[c] = saved.Level[c];
            v->VolumeOwned = v->VolumeDirty = 1;
        }
        if (saved.Owned & UAC_LEVEL_MUTE) {
            v->Mute = saved.Mute ? 1 : 0;
            v->MuteOwned = v->MuteDirty = 1;
        }
    }
    for (c = 0; c < v->Channels; c++)
        v->Level[c] = UacVolToKs(UacVolFromKs(v->Level[c], UacLevelKsRange(v, c)));
    v->Present = 1;
}

/* An owned control with a write to retry, a state to read again, or
 * an extra control still to set (Run applies it before streaming). */
int UacLevelStale(UAC_LEVEL *v)
{
    int extra, stale = 0, volume, mute;
    unsigned long n;
    if (!v->Present) return 0;
    v->Ops->Lock(v->Ops->Context);
    volume = v->VolumeOwned && v->Map.VolumeMode != UAC_VOL_NONE;
    mute = v->MuteOwned && v->Map.MuteMode != UAC_VOL_NONE;
    if ((volume && v->VolumeDirty) || (mute && v->MuteDirty)) stale = 1;
    v->Ops->Unlock(v->Ops->Context);
    if ((volume && v->Map.VolumeExtra && !v->VolumeNormal) || (mute && v->Map.MuteExtra && !v->MuteNormal))
        stale = 1;
    for (n = 0; n < UAC_VOL_SLOTS; n++) {
        if (volume && VolumeSlot(v, n, &extra) && !v->Known[n]) stale = 1;
        if (mute && MuteSlot(v, n, &extra) && !v->MuteKnown[n]) stale = 1;
    }
    return stale;
}

static void RecordLevel(UAC_LEVEL *v, const long *levels, long mute)
{
    UAC_LEVEL_DIAG g;
    unsigned long c;
    Zero(&g, sizeof(g));
    g.Channels = v->Channels; g.Output = v->Map.Output;
    g.VolumeUnit = v->Map.VolumeUnit; g.VolumeMode = v->Map.VolumeMode;
    g.VolumeChannels = v->Map.VolumeChannels; g.VolumeExtra = v->Map.VolumeExtra;
    g.MuteUnit = v->Map.MuteUnit; g.MuteMode = v->Map.MuteMode;
    g.MuteChannels = v->Map.MuteChannels; g.MuteExtra = v->Map.MuteExtra;
    g.Owned = (v->VolumeOwned ? UAC_LEVEL_VOLUME : 0) | (v->MuteOwned ? UAC_LEVEL_MUTE : 0);
    g.KsMin = UacLevelKsRange(v, 0)->KsMin; g.KsMax = UacLevelKsRange(v, 0)->KsMax;
    g.KsStep = UacLevelKsRange(v, 0)->KsStep;
    g.KsTop = UacLevelKsRange(v, 0)->KsTop; g.AboveZero = v->AboveZero; g.Lowered = v->Lowered;
    g.Change = v->Change; g.Writes = v->Writes; g.Failures = v->Failures; g.LastStatus = v->LastStatus;
    g.Mute = mute;
    for (c = 0; c < v->Channels; c++) g.Level[c] = levels[c];
    for (c = 0; c < UAC_VOL_CHANNELS; c++) g.Gain[c] = v->Gain[c];
    for (c = 0; c < UAC_VOL_SLOTS; c++) {
        g.Actual[c] = v->Actual[c]; g.ActualMute[c] = v->ActualMute[c];
        if (v->Known[c]) g.Known |= 1u << c;
        if (v->MuteKnown[c]) g.MuteKnown |= 1u << c;
    }
    v->Ops->PutDiag(v->Ops->Context, &g, sizeof(g));
}

/* PASSIVE, operation held. Reads what is unknown; publishes gains safe
 * for both the device's state and the values about to be written
 * (waiting out the USB queue, at most 32 ms, before a channel gets
 * louder in hardware); writes the owned controls that differ; gains
 * again for what the device then has; the registry copy. A failed
 * write leaves that control unknown (read again next time) and dirty. */
void UacLevelApply(UAC_LEVEL *v)
{
    UAC_VOL_MAP *m = &v->Map;
    long levels[UAC_VOL_CHANNELS], mute;
    int target[UAC_VOL_SLOTS], next[UAC_VOL_SLOTS];
    unsigned long gain[UAC_VOL_CHANNELS];
    int volumeOwned, muteOwned, extra;
    int writeVolume = 0, writeMute = 0, louder = 0;
    int volumeFailed = 0, muteFailed = 0;
    unsigned long c, n, change;
    if (!v->Present) return;
    v->Ops->Lock(v->Ops->Context);
    for (c = 0; c < v->Channels; c++) levels[c] = v->Level[c];
    mute = v->Mute; change = v->Change;
    volumeOwned = v->VolumeOwned; muteOwned = v->MuteOwned;
    v->VolumeDirty = v->MuteDirty = 0;
    v->Ops->Unlock(v->Ops->Context);
    if (Ready(v)) ReadState(v);
    UacVolTargets(levels, v->Channels, m, v->Range, target);
    /* What each written slot will hold: main controls their targets,
     * extras their value nearest 0 dB (unmuted). */
    for (n = 0; n < UAC_VOL_SLOTS; n++) {
        next[n] = v->Known[n] ? v->Actual[n] : Assumed(v, n);
        if (VolumeSlot(v, n, &extra) && !(extra && (v->NoRange & (1u << (n - 1)))))
            next[n] = extra ? UacVolFromKs(0, &v->Range[n]) : target[n];
    }
    /* An owned control is written wherever the device differs from it
     * or is not known (the dirty flags: a SET or restore since). */
    if (Ready(v) && volumeOwned && m->VolumeMode != UAC_VOL_NONE)
        for (n = 0; n < UAC_VOL_SLOTS; n++)
            if (VolumeSlot(v, n, &extra) && !(extra && (v->NoRange & (1u << (n - 1)))) &&
                (!v->Known[n] || next[n] != v->Actual[n])) {
                writeVolume = 1;
                if (!v->Known[n] || next[n] > v->Actual[n]) louder = 1;
            }
    if (Ready(v) && muteOwned && m->MuteMode != UAC_VOL_NONE)
        for (n = 0; n < UAC_VOL_SLOTS; n++)
            if (MuteSlot(v, n, &extra) && (!v->MuteKnown[n] || v->ActualMute[n] != (extra ? 0 : mute))) {
                writeMute = 1;
                if (extra || !mute) louder = 1;
            }
    Gains(v, levels, mute, writeVolume || writeMute ? next : 0, gain);
    Publish(v, gain);
    if (louder && v->Ops->Running(v->Ops->Context)) v->Ops->Delay40(v->Ops->Context);
    if (writeVolume || writeMute) {
        /* The volume lowerings (an unknown volume counts as Assumed),
         * then the mutes turned on; then the volume raisings, then the
         * mutes turned off: no state on the way is louder than both
         * ends. A failed write holds every later pass (left dirty), as
         * the device may not have taken it. */
        int volumeNormal = 1, muteNormal = 1;
        unsigned long pass;
        for (pass = 0; pass < 4; pass++) {
            if (pass >= 2 && (volumeFailed || muteFailed)) {
                if (writeVolume) { volumeFailed = 1; volumeNormal = 0; }
                if (writeMute) { muteFailed = 1; muteNormal = 0; }
                break;
            }
            for (n = 0; n < UAC_VOL_SLOTS; n++) {
                if (pass == 0 || pass == 2) {
                    if (!writeVolume || !VolumeSlot(v, n, &extra) || (extra && (v->NoRange & (1u << (n - 1)))) ||
                        (v->Known[n] && next[n] == v->Actual[n])) continue;
                    if ((pass == 0) != (next[n] < (v->Known[n] ? v->Actual[n] : Assumed(v, n)))) continue;
                    if (WriteCur(v, m->VolumeUnit, 2, n, next[n])) { v->Actual[n] = next[n]; v->Known[n] = 1; }
                    else { v->Known[n] = 0; volumeFailed = 1; if (extra) volumeNormal = 0; }
                } else {
                    int want;
                    if (!writeMute || !MuteSlot(v, n, &extra)) continue;
                    want = extra ? 0 : (int)mute;
                    if ((v->MuteKnown[n] && v->ActualMute[n] == want) || (pass == 1) != (want != 0)) continue;
                    if (WriteCur(v, m->MuteUnit, 1, n, want)) { v->ActualMute[n] = want; v->MuteKnown[n] = 1; }
                    else { v->MuteKnown[n] = 0; muteFailed = 1; if (extra) muteNormal = 0; }
                }
            }
        }
        if (writeVolume) v->VolumeNormal = volumeNormal;
        if (writeMute) v->MuteNormal = muteNormal;
    }
    if (volumeFailed || muteFailed) {
        v->Ops->Lock(v->Ops->Context);
        if (volumeFailed) v->VolumeDirty = 1;
        if (muteFailed) v->MuteDirty = 1;
        v->Ops->Unlock(v->Ops->Context);
    }
    Gains(v, levels, mute, 0, gain);
    Publish(v, gain);
    if (!v->Ops->Removed(v->Ops->Context)) {
        if (change != v->Saved) {
            UAC_LEVEL_SAVE save;
            Zero(&save, sizeof(save));
            save.Version = 2; save.Channels = v->Channels; save.Mute = mute;
            save.Owned = (volumeOwned ? UAC_LEVEL_VOLUME : 0) | (muteOwned ? UAC_LEVEL_MUTE : 0);
            for (c = 0; c < v->Channels; c++) save.Level[c] = levels[c];
            v->Ops->PutValue(v->Ops->Context, v->Pair, &save, sizeof(save));
            v->Saved = change;
        }
        RecordLevel(v, levels, mute);
    }
}

/* The device may come back with its own levels: read them again. */
void UacLevelPowerDown(UAC_LEVEL *v)
{
    unsigned long n;
    for (n = 0; n < UAC_VOL_SLOTS; n++) v->Known[n] = v->MuteKnown[n] = 0;
    v->VolumeNormal = v->MuteNormal = 0;
}

/* The render line's volume as found at start, in the stats words
 * after build 24's (uacusb.h, design 20.5). */
void UacLevelStats(const UAC_LEVEL *v, UAC_LEVEL_STATS *s)
{
    unsigned long n;
    s->VolLayout = 25;
    s->VolUnit = v->Map.VolumeUnit; s->VolMode = v->Map.VolumeMode;
    s->VolChannels = v->Map.VolumeChannels; s->VolAboveZero = v->AboveZero;
    s->VolLowered = v->Lowered; s->VolRangeRead = v->RangeRead; s->VolStartKnown = v->StartKnown;
    for (n = 0; n < 3; n++) {
        const UAC_VOL_RANGE *r = &v->Range[n];
        int read = (v->RangeRead & (1u << n)) != 0;
        s->VolCount[n] = read ? r->Count : 0;
        s->VolMin[n] = read ? r->Min[0] : 0;
        s->VolMax[n] = read && r->Count ? r->Max[r->Count - 1] : 0;
        s->VolRes[n] = read ? (long)(r->KsStep / 256) : 0;
        s->VolTop[n] = read ? (long)(r->KsTop / 256) : 0;
        s->VolOffered[n] = read ? (long)(r->KsMax / 256) : 0;
        s->VolStartCur[n] = (v->StartKnown & (1u << n)) ? v->StartActual[n] : 0;
    }
}

/* KSPROPERTY_AUDIO_VOLUMELEVEL (volume) or KSPROPERTY_AUDIO_MUTE GET:
 * channel -1 answers the loudest channel. */
long UacLevelGet(UAC_LEVEL *v, int volume, long channel)
{
    long value;
    unsigned long c;
    v->Ops->Lock(v->Ops->Context);
    if (!volume) value = v->Mute;
    else if (channel >= 0) value = v->Level[channel];
    else for (value = v->Level[0], c = 1; c < v->Channels; c++)
        if (v->Level[c] > value) value = v->Level[c];
    v->Ops->Unlock(v->Ops->Context);
    return value;
}

/* A SET takes the control over (the driver writes it from then on and
 * restores it), even at the value it already has. */
void UacLevelSet(UAC_LEVEL *v, int volume, long channel, long value)
{
    long snapped[UAC_VOL_CHANNELS];
    unsigned long c;
    for (c = 0; c < v->Channels; c++)
        snapped[c] = UacVolToKs(UacVolFromKs(value, UacLevelKsRange(v, c)));
    v->Ops->Lock(v->Ops->Context);
    if (!volume) {
        v->Mute = value ? 1 : 0;
        v->MuteOwned = v->MuteDirty = 1;
    } else {
        for (c = 0; c < v->Channels; c++)
            if (channel < 0 || (unsigned long)channel == c) v->Level[c] = snapped[c];
        v->VolumeOwned = v->VolumeDirty = 1;
    }
    v->Change++;
    v->Ops->Unlock(v->Ops->Context);
}

/* Volume may always take software (balance, a channel's limits, channels
 * past the unit's); mute only without a device mute. */
int UacLevelNotHostCpu(const UAC_LEVEL *v, int volume)
{
    return !volume && v->Map.MuteMode != UAC_VOL_NONE;
}
