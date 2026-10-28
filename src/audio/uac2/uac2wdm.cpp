/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Experimental UAC 2.0 WaveCyclic/USB streaming: render and capture in
 * the formats and rates the device's descriptors and clocks offer.
 * Original code from DDK headers and published PortCls documentation.
 */
#define PC_NEW_NAMES
#include <initguid.h>
#include <portcls.h>
extern "C" {
#pragma warning(push)
#pragma warning(disable:4200) /* flexible arrays in the kit's USB headers */
#if defined(_AMD64_)
#include <usb.h>
#else
#include <usbdi.h>
#endif
#include <usbioctl.h>
#pragma warning(pop)
#include "uacdesc.h"
#include "uacusb.h"
#include "uacplan.h"
#include "uactopo.h"
#include "uacvol.h"
}
#ifndef KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL
#define KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL 0x00000002 /* XP ks.h */
#endif
#ifndef VT_I4
#define VT_I4 3
#endif
#ifndef VT_BOOL
#define VT_BOOL 11
#endif

static KSDATARANGE analog = {
    sizeof(KSDATARANGE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE bridgeRanges[] = { &analog };
/* uactopo.c holds the graphs (host-tested); UAC_LINK has
 * PCCONNECTION_DESCRIPTOR's layout. */
#define UAC_ASSERT(e, n) typedef char n[(e) ? 1 : -1]
UAC_ASSERT(sizeof(UAC_LINK) == sizeof(PCCONNECTION_DESCRIPTOR), uacLinkSize);
UAC_ASSERT(UAC_FILTER_NODE == PCFILTER_NODE, uacFilterNode);
/* The topology filter carries a SUM node between each pair of its pins:
 * Windows 98 SE's SysAudio took a page fault on the earlier pin-to-pin
 * topology (uactopo.h). A SUM node adds no mixer control (2000 DDK,
 * "WDMAud Topology Parsing"), so no control is offered that the driver
 * cannot carry out. */
#define UAC_RANGES 32
#define UAC_FORMATS UAC_PLAN_FORMATS

/* One direction's formats and the rates each runs at, from the device's
 * descriptors and its clock's RANGE answers (PnP start). */
struct UAC_DIR_FORMATS {
    ULONG Count;
    UAC_FORMAT Format[UAC_FORMATS];
    ULONG RateCount[UAC_FORMATS];
    ULONG Rate[UAC_FORMATS][UAC_USB_RATES];
    ULONG Ranges;
    KSDATARANGE_AUDIO Range[UAC_RANGES];
    PKSDATARANGE RangeList[UAC_RANGES];
    ULONG RangeFormat[UAC_RANGES];  /* index into Format[] */
};

/* Pool construction avoids the C++ runtime, exceptions and global ctors. */
inline void *operator new(size_t, void *memory) { return memory; }
static void Record(PDEVICE_OBJECT fdo, ULONG stage, NTSTATUS status);
static void RecordStats(PDEVICE_OBJECT fdo, UAC_USB_STATS *stats, BOOLEAN capture);
static void RecordStreams(PDEVICE_OBJECT fdo, ULONG attempts, ULONG created, ULONG closed, ULONG active);
static void PutValue(PDEVICE_OBJECT fdo, PWSTR name, const void *data, ULONG size);
static BOOLEAN GetValue(PDEVICE_OBJECT fdo, PWSTR name, void *data, ULONG size);
#ifndef _WIN64
/* Windows 98 SE's NTKERN exports no IoAllocateWorkItem/IoQueueWorkItem/
 * IoFreeWorkItem (link probe against the 98 DDK wdm.lib, 2026-10-22). The
 * x86 build therefore runs the stream cleanup as an executive work item
 * (ExQueueWorkItem, in every WDM system) behind those names. The stream
 * keeps its own reference on the device object until DestroyPassive; unlike
 * IoQueueWorkItem, nothing else holds the image while Cleanup returns. */
struct UAC_WORK_ITEM {
    WORK_QUEUE_ITEM Item;
    PIO_WORKITEM_ROUTINE Routine;
    PVOID Context;
    PDEVICE_OBJECT Device;
};
static VOID UacExItem(PVOID p)
{
    UAC_WORK_ITEM *j = (UAC_WORK_ITEM *)p;
    j->Routine(j->Device, j->Context); /* frees j: no access after */
}
static PIO_WORKITEM UacAllocItem(PDEVICE_OBJECT d)
{
    UAC_WORK_ITEM *j = (UAC_WORK_ITEM *)ExAllocatePoolWithTag(NonPagedPool, sizeof(*j), 'w2aU');
    if (j) { RtlZeroMemory(j, sizeof(*j)); j->Device = d; }
    return (PIO_WORKITEM)j;
}
static VOID UacFreeItem(PIO_WORKITEM i) { ExFreePool(i); }
static VOID UacQueueItem(PIO_WORKITEM i, PIO_WORKITEM_ROUTINE r, WORK_QUEUE_TYPE q, PVOID c)
{
    UAC_WORK_ITEM *j = (UAC_WORK_ITEM *)i;
    j->Routine = r; j->Context = c;
    ExInitializeWorkItem(&j->Item, UacExItem, j);
    ExQueueWorkItem(&j->Item, q);
}
#define IoAllocateWorkItem UacAllocItem
#define IoFreeWorkItem UacFreeItem
#define IoQueueWorkItem UacQueueItem
#endif
#include "uacstream.h"

class Wave;
/* One direction of the wave filter: its formats, its one-stream gate and
 * the transport stream of the open pin instance. */
struct UAC_DIR {
    Wave *Owner;
    BOOLEAN Capture;
    LONG Active;
    ULONG Attempts, Created, Closed;
    UAC_USB *Usb;
    UacStream *Stream;          /* the stream Usb serves */
    ULONG Channels, Subslot;    /* its device format */
    UAC_DIR_FORMATS Formats;
};

/* Playback volume and mute: the topology filter's VOLUME and MUTE nodes
 * (uacvol.h). Each node channel has a level in KS units (1/65536 dB);
 * the device's Feature Unit does what it can and the rest is a software
 * gain on the samples on their way to USB (Wave::Fill), computed from
 * what the device is known to do, so a pending, refused or unconfirmed
 * write never makes a channel louder than asked. Until a control is set,
 * or restored from the registry, the device keeps its own setting of it. */
struct UAC_LEVEL {
    BOOLEAN Present;            /* a render line */
    ULONG Channels;
    UAC_VOL_MAP Map;
    UAC_VOL_RANGE Range[UAC_VOL_SLOTS]; /* [0] master or software, [n] logical channel n */
    KSPIN_LOCK Lock;            /* Level[], Mute, the Owned and Dirty flags, Change */
    LONG Level[UAC_VOL_CHANNELS];
    LONG Mute;
    BOOLEAN VolumeOwned, MuteOwned;   /* set or restored: the driver writes it */
    BOOLEAN VolumeDirty, MuteDirty;   /* to write */
    ULONG Change, Saved;        /* SETs taken; the last one saved */
    /* The device, per slot (UAC_VOL_SLOTS), under the operation mutex: */
    int Actual[UAC_VOL_SLOTS];  /* volume CUR, where Known */
    LONG ActualMute[UAC_VOL_SLOTS];
    BOOLEAN Known[UAC_VOL_SLOTS], MuteKnown[UAC_VOL_SLOTS];
    BOOLEAN VolumeNormal, MuteNormal; /* the extra channel controls set (VolumeExtra, MuteExtra) */
    unsigned int NoRange;       /* extras without a usable RANGE: read and counted, not written */
    LONG Gain[UAC_VOL_CHANNELS];  /* Q16 per stream channel, read by the USB worker */
    PIO_WORKITEM Item;          /* preallocated: applies SETs made at raised IRQL */
    LONG Pending, Queued, Work; /* a SET to apply; Item queued; work items not finished */
    LONG Closed;                /* removal: no more work items */
    ULONG Writes, Failures;
    NTSTATUS LastStatus;
};
/* The registry copy of the levels (the device's driver key, Uac2Level). */
#define UAC_LEVEL_VOLUME 1
#define UAC_LEVEL_MUTE 2
struct UAC_LEVEL_SAVE {
    ULONG Version, Channels, Owned;  /* Version 2; Owned: UAC_LEVEL_VOLUME | UAC_LEVEL_MUTE */
    LONG Mute;
    LONG Level[UAC_VOL_CHANNELS];
};
/* Diagnostics (Uac2VolMap): what the driver found and does. */
struct UAC_LEVEL_DIAG {
    ULONG Channels, Output, VolumeUnit, VolumeMode, VolumeChannels, VolumeExtra;
    ULONG MuteUnit, MuteMode, MuteChannels, MuteExtra, Owned, Known, MuteKnown;
    LONG KsMin, KsMax, KsStep;
    ULONG Change, Writes, Failures;
    NTSTATUS LastStatus;
    LONG Mute, Level[UAC_VOL_CHANNELS], Gain[UAC_VOL_CHANNELS];
    LONG Actual[UAC_VOL_SLOTS], ActualMute[UAC_VOL_SLOTS];
};

class Wave : public IMiniportWaveCyclic, public IAdapterPowerManagement {
    static void operator delete(PVOID memory) { ExFreePool(memory); }
    LONG refs;
    PDEVICE_OBJECT device;
    PDEVICE_OBJECT lower;
    UAC_DESC *descriptor;
    UAC_DEV *dev;
    const UAC_GRAPH *graph;
    PPORTWAVECYCLIC port;
    KMUTEX operation;
    BOOLEAN removed, pnpBlocked, powerBlocked;
    UAC_DIR dir[2];             /* 0 render, 1 capture */
    UAC_LEVEL level;
    PCPIN_DESCRIPTOR pins[4];
    PCNODE_DESCRIPTOR nodes[2];
    GUID categories[3];
    PCFILTER_DESCRIPTOR filter;

    /* The USB callbacks' context is the direction (Run). */
    static void Fill(PVOID context, PUCHAR bytes, ULONG count) {
        UAC_DIR *d = (UAC_DIR *)context;
        d->Stream->FillBytes(bytes, count);
        if (!d->Capture) d->Owner->Soften(d, bytes, count);
    }
    static void Captured(PVOID context, const UCHAR *bytes, ULONG count) {
        ((UAC_DIR *)context)->Stream->WriteBytes(bytes, count);
    }
    static void Progress(PVOID context, ULONG bytes, ULONG errors) {
        ((UAC_DIR *)context)->Stream->CompleteBytes(bytes, errors);
    }
    /* The software part of the levels, on the USB worker. */
    void Soften(UAC_DIR *d, PUCHAR bytes, ULONG count) {
        unsigned long gain[UAC_VOL_CHANNELS];
        BOOLEAN unity = TRUE;
        if (!level.Present) return;
        for (ULONG c = 0; c < UAC_VOL_CHANNELS; c++) {
            gain[c] = (unsigned long)level.Gain[c];
            if (gain[c] < UAC_VOL_UNITY) unity = FALSE;
        }
        if (!unity) UacVolScale(bytes, count, d->Subslot, d->Channels, gain, UAC_VOL_CHANNELS);
    }
    /* One Feature Unit request (uacctl.c builds and checks the setup). */
    /* got: the bytes a GET returned (may be short); NULL: exactly size. */
    NTSTATUS Feature(UAC_BYTE unit, ULONG selector, ULONG channel, BOOLEAN read,
                     ULONG attribute, PVOID bytes, ULONG size, ULONG *got = NULL) {
        UAC_CTL_TARGET t;
        UCHAR setup[8];
        t.Interface = descriptor->Control; t.Entity = unit; t.Endpoint = 0;
        t.Channel = channel; t.MaxChannel = 255;
        if (!UacCtlSetup(UAC_CTL_FEATURE, selector, &t, read ? 1 : 3, attribute, read ? 1 : 0,
                         size, 0, setup)) return STATUS_INVALID_PARAMETER;
        ULONG length = size;
        NTSTATUS s = UacUsbClassRequest(dev, setup, bytes, &length);
        if (got) *got = length;
        else if (NT_SUCCESS(s) && length != size) s = STATUS_DEVICE_DATA_ERROR;
        return s;
    }
    /* Room for UAC_VOL_RANGES subranges in one request; a device that
     * refuses it is asked for one subrange, the wLength Linux uses
     * ([linux] sound/usb/mixer.c), and its first subrange is the range. */
    BOOLEAN ReadRange(UAC_BYTE unit, ULONG channel, UAC_VOL_RANGE *range) {
        UCHAR b[2 + 6 * UAC_VOL_RANGES];
        ULONG got = 0, n;
        RtlZeroMemory(b, sizeof(b));
        if (!NT_SUCCESS(Feature(unit, 2, channel, TRUE, UAC_CTL_RANGE_ATTRIBUTE, b, sizeof(b), &got)) &&
            !NT_SUCCESS(Feature(unit, 2, channel, TRUE, UAC_CTL_RANGE_ATTRIBUTE, b, 8, &got)))
            return FALSE;
        if (got < 8) return FALSE;
        n = (ULONG)b[0] | ((ULONG)b[1] << 8);
        if (n > 1 && got < 2 + 6 * n) { n = 1; b[0] = 1; b[1] = 0; }
        if (!n || n > UAC_VOL_RANGES) return FALSE;
        return UacVolRange(b, 2 + 6 * n, range) ? TRUE : FALSE;
    }
    /* CUR, raw: volume in 1/256 dB (UAC_VOL_SILENCE included), mute 0/1. */
    BOOLEAN ReadCur(UAC_BYTE unit, ULONG selector, ULONG channel, int *value) {
        UCHAR b[2];
        ULONG size = selector == 2 ? 2 : 1;
        if (!NT_SUCCESS(Feature(unit, selector, channel, TRUE, UAC_CTL_CUR, b, size)) ||
            !UacCtlValue(UAC_CTL_FEATURE, selector, b, size, 0)) return FALSE;
        if (selector == 2) {
            int v = (int)((unsigned int)b[0] | ((unsigned int)b[1] << 8));
            *value = v >= 32768 ? v - 65536 : v;
        } else *value = b[0];
        return TRUE;
    }
    BOOLEAN WriteCur(UAC_BYTE unit, ULONG selector, ULONG channel, int value) {
        UCHAR b[2];
        b[0] = (UCHAR)value; b[1] = (UCHAR)((unsigned int)value >> 8);
        NTSTATUS s = Feature(unit, selector, channel, FALSE, UAC_CTL_CUR, b, selector == 2 ? 2 : 1);
        level.Writes++;
        if (!NT_SUCCESS(s)) { level.Failures++; level.LastStatus = s; }
        return NT_SUCCESS(s) ? TRUE : FALSE;
    }
    /* The KS range of node channel c. */
    const UAC_VOL_RANGE *KsRange(ULONG c) const {
        return level.Map.VolumeMode == UAC_VOL_CHANNEL ? &level.Range[c + 1] : &level.Range[0];
    }
    /* A device value as a node level: silence is the bottom of the range. */
    LONG Level(int value, ULONG c) const {
        return value == UAC_VOL_SILENCE ? KsRange(c)->KsMin : UacVolToKs(value);
    }
    /* Slot n (0 master, n logical channel n) carries the volume (mute)
     * control the driver writes: the main one, or an extra to normalize. */
    BOOLEAN VolumeSlot(ULONG n, BOOLEAN *extra) const {
        const UAC_VOL_MAP *m = &level.Map;
        *extra = FALSE;
        if (m->VolumeMode == UAC_VOL_MASTER) {
            if (!n) return TRUE;
            *extra = (m->VolumeExtra & (1u << (n - 1))) != 0;
            return *extra;
        }
        return m->VolumeMode == UAC_VOL_CHANNEL && n >= 1 && n <= m->VolumeChannels;
    }
    BOOLEAN MuteSlot(ULONG n, BOOLEAN *extra) const {
        const UAC_VOL_MAP *m = &level.Map;
        *extra = FALSE;
        if (m->MuteMode == UAC_VOL_MASTER) {
            if (!n) return TRUE;
            *extra = (m->MuteExtra & (1u << (n - 1))) != 0;
            return *extra;
        }
        return m->MuteMode == UAC_VOL_CHANNEL && n >= 1 && n <= m->MuteChannels;
    }
    /* Reads every controlled slot not yet known (start, after D3, after
     * a write that failed: the device may or may not have taken it). */
    void ReadState() {
        BOOLEAN extra;
        int value;
        for (ULONG n = 0; n < UAC_VOL_SLOTS; n++) {
            if (VolumeSlot(n, &extra) && !level.Known[n] && ReadCur(level.Map.VolumeUnit, 2, n, &value)) {
                level.Actual[n] = value; level.Known[n] = TRUE;
            }
            if (MuteSlot(n, &extra) && !level.MuteKnown[n] && ReadCur(level.Map.MuteUnit, 1, n, &value)) {
                level.ActualMute[n] = value; level.MuteKnown[n] = TRUE;
            }
        }
    }
    /* What an unknown volume is taken as: the top of its range; an extra
     * channel volume without a RANGE, the most a CUR can say (+128 dB).
     * Gains also silences that channel while the volume is owned. */
    int Assumed(ULONG n) const {
        if (n && (level.NoRange & (1u << (n - 1)))) return 0x7FFF;
        return (int)(level.Range[n].KsMax / 256);
    }
    /* The gains for the device's state: an unknown volume as Assumed, an
     * unknown mute as off. With next (the values about to
     * be written, mute included), the lower of the two for each channel. */
    void Gains(const LONG *levels, LONG mute, const int *next, unsigned long *gain) {
        int actual[UAC_VOL_SLOTS], muted[UAC_VOL_SLOTS];
        unsigned long after[UAC_VOL_CHANNELS];
        BOOLEAN extra;
        ULONG n, k;
        UAC_VOL_MAP map = level.Map;
        LONG flat[UAC_VOL_CHANNELS];
        for (n = 0; n < UAC_VOL_SLOTS; n++) {
            actual[n] = level.Known[n] ? level.Actual[n] : Assumed(n);
            muted[n] = level.MuteKnown[n] && level.ActualMute[n];
        }
        if (!level.VolumeOwned) {
            /* Until a volume is set the device's own setting stands: no
             * software volume (an owned mute still applies). */
            map.VolumeMode = UAC_VOL_NONE; map.VolumeExtra = 0;
            for (k = 0; k < UAC_VOL_CHANNELS; k++) flat[k] = 0;
            levels = flat;
        }
        UacVolGains(levels, level.Channels, mute, &map, actual, muted, gain);
        /* An owned master whose extra channel volume has no RANGE and an
         * unknown CUR: that channel is silent until the CUR is read. */
        if (map.VolumeMode == UAC_VOL_MASTER)
            for (k = 0; k < UAC_VOL_CHANNELS; k++)
                if ((level.NoRange & (1u << k)) && !level.Known[k + 1]) gain[k] = 0;
        if (!next) return;
        for (n = 0; n < UAC_VOL_SLOTS; n++) {
            if (VolumeSlot(n, &extra)) actual[n] = next[n];
            if (MuteSlot(n, &extra)) muted[n] = extra ? 0 : mute;
        }
        UacVolGains(levels, level.Channels, mute, &map, actual, muted, after);
        for (k = 0; k < UAC_VOL_CHANNELS; k++) if (after[k] < gain[k]) gain[k] = after[k];
    }
    void Publish(const unsigned long *gain) {
        for (ULONG k = 0; k < UAC_VOL_CHANNELS; k++) InterlockedExchange(&level.Gain[k], (LONG)gain[k]);
    }
    /* PnP start, after Learn: the render line's controls and levels. */
    void LevelSetup() {
        UAC_LEVEL *v = &level;
        UAC_VOL_MAP *m = &v->Map;
        UAC_LEVEL_SAVE saved;
        BOOLEAN extra;
        ULONG c, n, most = 1;
        RtlZeroMemory(v, sizeof(*v));
        KeInitializeSpinLock(&v->Lock);
        for (c = 0; c < UAC_VOL_CHANNELS; c++) v->Gain[c] = (LONG)UAC_VOL_UNITY;
        if (!dir[0].Formats.Count) return;
        for (c = 0; c < dir[0].Formats.Count; c++)
            if (dir[0].Formats.Format[c].Channels > most) most = dir[0].Formats.Format[c].Channels;
        /* Mono or a stereo pair: a stream with more channels takes channel
         * c % 2 (uacvol.h), as the system's mixer shows a pair. */
        v->Channels = most >= 2 ? 2 : 1;
        UacVolFind(descriptor, dir[0].Formats.Format[0].Terminal, v->Channels, m);
        /* Every controlled channel's own RANGE; a control without a usable
         * one is left to software (an extra one is then not written). */
        if (m->VolumeMode == UAC_VOL_MASTER) {
            if (!ReadRange(m->VolumeUnit, 0, &v->Range[0])) m->VolumeMode = UAC_VOL_NONE;
            for (n = 1; n <= UAC_VOL_CHANNELS; n++)
                if ((m->VolumeExtra & (1u << (n - 1))) && !ReadRange(m->VolumeUnit, n, &v->Range[n]))
                    v->NoRange |= 1u << (n - 1);
        } else if (m->VolumeMode == UAC_VOL_CHANNEL) {
            for (n = 1; n <= m->VolumeChannels; n++)
                if (!ReadRange(m->VolumeUnit, n, &v->Range[n])) m->VolumeMode = UAC_VOL_NONE;
        }
        if (m->VolumeMode == UAC_VOL_NONE) {
            m->VolumeUnit = 0; m->VolumeChannels = 0; m->VolumeExtra = 0;
            UacVolSoftRange(&v->Range[0]);
        }
        ReadState();
        /* The levels: restored, else the device's own (0 dB in software). */
        for (c = 0; c < v->Channels; c++) {
            n = m->VolumeMode == UAC_VOL_MASTER ? 0 : c + 1;
            v->Level[c] = m->VolumeMode != UAC_VOL_NONE && v->Known[n] ? Level(v->Actual[n], c) : 0;
        }
        /* Muted when every controlled channel is known muted. */
        v->Mute = m->MuteMode != UAC_VOL_NONE;
        for (n = m->MuteMode == UAC_VOL_MASTER ? 0 : 1; n < UAC_VOL_SLOTS && v->Mute; n++)
            if (MuteSlot(n, &extra) && !extra && !(v->MuteKnown[n] && v->ActualMute[n])) v->Mute = 0;
        if (GetValue(device, L"Uac2Level", &saved, sizeof(saved)) && saved.Version == 2 &&
            saved.Channels == v->Channels) {
            if (saved.Owned & UAC_LEVEL_VOLUME) {
                for (c = 0; c < v->Channels; c++) v->Level[c] = saved.Level[c];
                v->VolumeOwned = v->VolumeDirty = TRUE;
            }
            if (saved.Owned & UAC_LEVEL_MUTE) {
                v->Mute = saved.Mute ? 1 : 0;
                v->MuteOwned = v->MuteDirty = TRUE;
            }
        }
        for (c = 0; c < v->Channels; c++)
            v->Level[c] = UacVolToKs(UacVolFromKs(v->Level[c], KsRange(c)));
        v->Present = TRUE;
    }
    BOOLEAN DeviceReady() const { return dev && !removed && !pnpBlocked && !powerBlocked; }
    BOOLEAN Running() const { return dir[0].Usb != NULL; }
    /* An owned control with a write to retry, a state to read again, or
     * an extra control still to set (Run applies it before streaming). */
    BOOLEAN LevelStale() {
        UAC_LEVEL *v = &level;
        BOOLEAN extra, stale = FALSE;
        KIRQL irql;
        if (!v->Present) return FALSE;
        KeAcquireSpinLock(&v->Lock, &irql);
        BOOLEAN volume = v->VolumeOwned && v->Map.VolumeMode != UAC_VOL_NONE;
        BOOLEAN mute = v->MuteOwned && v->Map.MuteMode != UAC_VOL_NONE;
        if ((volume && v->VolumeDirty) || (mute && v->MuteDirty)) stale = TRUE;
        KeReleaseSpinLock(&v->Lock, irql);
        if ((volume && v->Map.VolumeExtra && !v->VolumeNormal) || (mute && v->Map.MuteExtra && !v->MuteNormal))
            stale = TRUE;
        for (ULONG n = 0; n < UAC_VOL_SLOTS; n++) {
            if (volume && VolumeSlot(n, &extra) && !v->Known[n]) stale = TRUE;
            if (mute && MuteSlot(n, &extra) && !v->MuteKnown[n]) stale = TRUE;
        }
        return stale;
    }
    /* PASSIVE, operation held. Reads what is unknown; publishes gains safe
     * for both the device's state and the values about to be written
     * (waiting out the USB queue, at most 32 ms, before a channel gets
     * louder in hardware); writes the owned controls that differ; gains
     * again for what the device then has; the registry copy. A failed
     * write leaves that control unknown (read again next time) and dirty. */
    void ApplyLocked() {
        UAC_LEVEL *v = &level;
        UAC_VOL_MAP *m = &v->Map;
        LONG levels[UAC_VOL_CHANNELS], mute;
        int target[UAC_VOL_SLOTS], next[UAC_VOL_SLOTS];
        unsigned long gain[UAC_VOL_CHANNELS];
        BOOLEAN volumeOwned, muteOwned, volumeDirty, muteDirty, extra;
        BOOLEAN writeVolume = FALSE, writeMute = FALSE, louder = FALSE;
        BOOLEAN volumeFailed = FALSE, muteFailed = FALSE;
        ULONG c, n, change;
        KIRQL irql;
        if (!v->Present) return;
        KeAcquireSpinLock(&v->Lock, &irql);
        for (c = 0; c < v->Channels; c++) levels[c] = v->Level[c];
        mute = v->Mute; change = v->Change;
        volumeOwned = v->VolumeOwned; muteOwned = v->MuteOwned;
        volumeDirty = v->VolumeDirty; muteDirty = v->MuteDirty;
        v->VolumeDirty = v->MuteDirty = FALSE;
        KeReleaseSpinLock(&v->Lock, irql);
        if (DeviceReady()) ReadState();
        UacVolTargets(levels, v->Channels, m, v->Range, target);
        /* What each written slot will hold: main controls their targets,
         * extras their value nearest 0 dB (unmuted). */
        for (n = 0; n < UAC_VOL_SLOTS; n++) {
            next[n] = v->Known[n] ? v->Actual[n] : Assumed(n);
            if (VolumeSlot(n, &extra) && !(extra && (v->NoRange & (1u << (n - 1)))))
                next[n] = extra ? UacVolFromKs(0, &v->Range[n]) : target[n];
        }
        /* An owned control is written wherever the device differs from it
         * or is not known (volumeDirty/muteDirty: a SET or restore since). */
        UNREFERENCED_PARAMETER(volumeDirty); UNREFERENCED_PARAMETER(muteDirty);
        if (DeviceReady() && volumeOwned && m->VolumeMode != UAC_VOL_NONE)
            for (n = 0; n < UAC_VOL_SLOTS; n++)
                if (VolumeSlot(n, &extra) && !(extra && (v->NoRange & (1u << (n - 1)))) &&
                    (!v->Known[n] || next[n] != v->Actual[n])) {
                    writeVolume = TRUE;
                    if (!v->Known[n] || next[n] > v->Actual[n]) louder = TRUE;
                }
        if (DeviceReady() && muteOwned && m->MuteMode != UAC_VOL_NONE)
            for (n = 0; n < UAC_VOL_SLOTS; n++)
                if (MuteSlot(n, &extra) && (!v->MuteKnown[n] || v->ActualMute[n] != (extra ? 0 : mute))) {
                    writeMute = TRUE;
                    if (extra || !mute) louder = TRUE;
                }
        Gains(levels, mute, writeVolume || writeMute ? next : NULL, gain);
        Publish(gain);
        if (louder && Running()) {
            LARGE_INTEGER t;
            t.QuadPart = -400000;   /* 40 ms: the queued USB audio plays out */
            KeDelayExecutionThread(KernelMode, FALSE, &t);
        }
        if (writeVolume || writeMute) {
            /* The volume lowerings (an unknown volume counts as Assumed),
             * then the mutes turned on; then the volume raisings, then the
             * mutes turned off: no state on the way is louder than both
             * ends. A failed write holds every later pass (left dirty), as
             * the device may not have taken it. */
            BOOLEAN volumeNormal = TRUE, muteNormal = TRUE;
            for (ULONG pass = 0; pass < 4; pass++) {
                if (pass >= 2 && (volumeFailed || muteFailed)) {
                    if (writeVolume) { volumeFailed = TRUE; volumeNormal = FALSE; }
                    if (writeMute) { muteFailed = TRUE; muteNormal = FALSE; }
                    break;
                }
                for (n = 0; n < UAC_VOL_SLOTS; n++) {
                    if (pass == 0 || pass == 2) {
                        if (!writeVolume || !VolumeSlot(n, &extra) || (extra && (v->NoRange & (1u << (n - 1)))) ||
                            (v->Known[n] && next[n] == v->Actual[n])) continue;
                        if ((pass == 0) != (next[n] < (v->Known[n] ? v->Actual[n] : Assumed(n)))) continue;
                        if (WriteCur(m->VolumeUnit, 2, n, next[n])) { v->Actual[n] = next[n]; v->Known[n] = TRUE; }
                        else { v->Known[n] = FALSE; volumeFailed = TRUE; if (extra) volumeNormal = FALSE; }
                    } else {
                        int want;
                        if (!writeMute || !MuteSlot(n, &extra)) continue;
                        want = extra ? 0 : mute;
                        if ((v->MuteKnown[n] && v->ActualMute[n] == want) || (pass == 1) != (want != 0)) continue;
                        if (WriteCur(m->MuteUnit, 1, n, want)) { v->ActualMute[n] = want; v->MuteKnown[n] = TRUE; }
                        else { v->MuteKnown[n] = FALSE; muteFailed = TRUE; if (extra) muteNormal = FALSE; }
                    }
                }
            }
            if (writeVolume) v->VolumeNormal = volumeNormal;
            if (writeMute) v->MuteNormal = muteNormal;
        }
        if (volumeFailed || muteFailed) {
            KeAcquireSpinLock(&v->Lock, &irql);
            if (volumeFailed) v->VolumeDirty = TRUE;
            if (muteFailed) v->MuteDirty = TRUE;
            KeReleaseSpinLock(&v->Lock, irql);
        }
        Gains(levels, mute, NULL, gain);
        Publish(gain);
        if (!removed) {
            if (change != v->Saved) {
                UAC_LEVEL_SAVE save;
                RtlZeroMemory(&save, sizeof(save));
                save.Version = 2; save.Channels = v->Channels; save.Mute = mute;
                save.Owned = (volumeOwned ? UAC_LEVEL_VOLUME : 0) | (muteOwned ? UAC_LEVEL_MUTE : 0);
                for (c = 0; c < v->Channels; c++) save.Level[c] = levels[c];
                PutValue(device, L"Uac2Level", &save, sizeof(save));
                v->Saved = change;
            }
            RecordLevel(levels, mute);
        }
    }
    void RecordLevel(const LONG *levels, LONG mute) {
        UAC_LEVEL *v = &level;
        UAC_LEVEL_DIAG g;
        ULONG c;
        RtlZeroMemory(&g, sizeof(g));
        g.Channels = v->Channels; g.Output = v->Map.Output;
        g.VolumeUnit = v->Map.VolumeUnit; g.VolumeMode = v->Map.VolumeMode;
        g.VolumeChannels = v->Map.VolumeChannels; g.VolumeExtra = v->Map.VolumeExtra;
        g.MuteUnit = v->Map.MuteUnit; g.MuteMode = v->Map.MuteMode;
        g.MuteChannels = v->Map.MuteChannels; g.MuteExtra = v->Map.MuteExtra;
        g.Owned = (v->VolumeOwned ? UAC_LEVEL_VOLUME : 0) | (v->MuteOwned ? UAC_LEVEL_MUTE : 0);
        g.KsMin = KsRange(0)->KsMin; g.KsMax = KsRange(0)->KsMax; g.KsStep = KsRange(0)->KsStep;
        g.Change = v->Change; g.Writes = v->Writes; g.Failures = v->Failures; g.LastStatus = v->LastStatus;
        g.Mute = mute;
        for (c = 0; c < v->Channels; c++) g.Level[c] = levels[c];
        for (c = 0; c < UAC_VOL_CHANNELS; c++) g.Gain[c] = v->Gain[c];
        for (c = 0; c < UAC_VOL_SLOTS; c++) {
            g.Actual[c] = v->Actual[c]; g.ActualMute[c] = v->ActualMute[c];
            if (v->Known[c]) g.Known |= 1u << c;
            if (v->MuteKnown[c]) g.MuteKnown |= 1u << c;
        }
        PutValue(device, L"Uac2VolMap", &g, sizeof(g));
    }
    void Apply() {
        KeWaitForSingleObject(&operation, Executive, KernelMode, FALSE, NULL);
        ApplyLocked();
        KeReleaseMutex(&operation, FALSE);
    }
    /* The work item: one at a time, applying until no SET is pending. The
     * Wave and the device object stay referenced until it ends (the x86
     * executive work item holds neither), and Shutdown waits for it (Work). */
    static VOID LevelWork(PDEVICE_OBJECT, PVOID context) {
        Wave *w = (Wave *)context;
        PDEVICE_OBJECT fdo = w->device;
        for (;;) {
            while (!w->level.Closed && InterlockedExchange(&w->level.Pending, 0)) w->Apply();
            InterlockedExchange(&w->level.Queued, 0);
            /* A SET that came while Queued was still set: take it here,
             * unless a new item already has. */
            if (w->level.Closed || !w->level.Pending || InterlockedExchange(&w->level.Queued, 1)) break;
        }
        InterlockedDecrement(&w->level.Work);
        w->Release();
        ObDereferenceObject(fdo);
    }
    /* A property set at raised IRQL: apply on the work item, at PASSIVE. */
    void Changed() {
        if (KeGetCurrentIrql() == PASSIVE_LEVEL) { Apply(); return; }
        /* Work first, then Closed: Shutdown sets Closed, then waits for Work. */
        InterlockedIncrement(&level.Work);
        if (level.Closed) { InterlockedDecrement(&level.Work); return; }
        InterlockedExchange(&level.Pending, 1);
        if (InterlockedExchange(&level.Queued, 1)) {
            InterlockedDecrement(&level.Work);
            return;
        }
        AddRef();
        ObReferenceObject(device);
        IoQueueWorkItem(level.Item, LevelWork, DelayedWorkQueue, this);
    }
    NTSTATUS LevelBasic(PPCPROPERTY_REQUEST r, BOOLEAN volume) {
        ULONG full = sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER) +
                     level.Channels * sizeof(KSPROPERTY_STEPPING_LONG);
        ULONG access = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET;
        if (!r->ValueSize) { r->ValueSize = full; return STATUS_BUFFER_OVERFLOW; }
        if (!r->Value) return STATUS_INVALID_PARAMETER;
        if (r->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
            PKSPROPERTY_DESCRIPTION d = (PKSPROPERTY_DESCRIPTION)r->Value;
            d->AccessFlags = access;
            d->DescriptionSize = full;
            d->PropTypeSet.Set = KSPROPTYPESETID_General;
            d->PropTypeSet.Id = volume ? VT_I4 : VT_BOOL;
            d->PropTypeSet.Flags = 0;
            d->MembersListCount = 1;
            d->Reserved = 0;
            if (r->ValueSize >= full) {
                PKSPROPERTY_MEMBERSHEADER m = (PKSPROPERTY_MEMBERSHEADER)(d + 1);
                PKSPROPERTY_STEPPING_LONG step = (PKSPROPERTY_STEPPING_LONG)(m + 1);
                m->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
                m->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
                m->MembersCount = level.Channels;
                m->Flags = KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL;
                for (ULONG c = 0; c < level.Channels; c++) {
                    step[c].SteppingDelta = volume ? (ULONG)KsRange(c)->KsStep : 1;
                    step[c].Reserved = 0;
                    step[c].Bounds.SignedMinimum = volume ? KsRange(c)->KsMin : 0;
                    step[c].Bounds.SignedMaximum = volume ? KsRange(c)->KsMax : 1;
                }
                r->ValueSize = full;
            } else r->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            return STATUS_SUCCESS;
        }
        if (r->ValueSize >= sizeof(ULONG)) {
            *(PULONG)r->Value = access; r->ValueSize = sizeof(ULONG); return STATUS_SUCCESS;
        }
        return STATUS_BUFFER_TOO_SMALL;
    }
public:
    /* KSPROPERTY_AUDIO_VOLUMELEVEL (volume) or KSPROPERTY_AUDIO_MUTE on
     * the topology filter's node; the channel is the KSNODEPROPERTY_AUDIO_
     * CHANNEL's (0..Channels-1, or -1 for all). A SET takes the control
     * over (the driver writes it from then on and restores it), even at
     * the value it already has. */
    NTSTATUS LevelProperty(PPCPROPERTY_REQUEST r, BOOLEAN volume) {
        UAC_LEVEL *v = &level;
        KIRQL irql;
        ULONG c;
        if (!v->Present) return STATUS_INVALID_DEVICE_REQUEST;
        if (r->Verb & KSPROPERTY_TYPE_BASICSUPPORT) return LevelBasic(r, volume);
        if (r->InstanceSize < sizeof(LONG) || !r->Instance) return STATUS_INVALID_PARAMETER;
        LONG channel = *(PLONG)r->Instance;
        if (channel != -1 && (channel < 0 || (ULONG)channel >= v->Channels)) return STATUS_INVALID_PARAMETER;
        if (!r->ValueSize) { r->ValueSize = sizeof(LONG); return STATUS_BUFFER_OVERFLOW; }
        if (r->ValueSize < sizeof(LONG) || !r->Value) return STATUS_BUFFER_TOO_SMALL;
        if (r->Verb & KSPROPERTY_TYPE_GET) {
            LONG value;
            KeAcquireSpinLock(&v->Lock, &irql);
            if (!volume) value = v->Mute;
            else if (channel >= 0) value = v->Level[channel];
            else for (value = v->Level[0], c = 1; c < v->Channels; c++)
                if (v->Level[c] > value) value = v->Level[c];
            KeReleaseSpinLock(&v->Lock, irql);
            *(PLONG)r->Value = value;
            r->ValueSize = sizeof(LONG);
            return STATUS_SUCCESS;
        }
        if (r->Verb & KSPROPERTY_TYPE_SET) {
            LONG value = *(PLONG)r->Value, snapped[UAC_VOL_CHANNELS];
            if (v->Closed) return STATUS_DEVICE_NOT_READY;
            for (c = 0; c < v->Channels; c++)
                snapped[c] = UacVolToKs(UacVolFromKs(value, KsRange(c)));
            KeAcquireSpinLock(&v->Lock, &irql);
            if (!volume) {
                v->Mute = value ? 1 : 0;
                v->MuteOwned = v->MuteDirty = TRUE;
            } else {
                for (c = 0; c < v->Channels; c++)
                    if (channel < 0 || (ULONG)channel == c) v->Level[c] = snapped[c];
                v->VolumeOwned = v->VolumeDirty = TRUE;
            }
            v->Change++;
            KeReleaseSpinLock(&v->Lock, irql);
            Changed();
            return STATUS_SUCCESS;
        }
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    /* KSPROPERTY_AUDIO_CPU_RESOURCES: volume may always take software
     * (balance, a channel's limits, channels past the unit's); mute only
     * without a device mute. */
    ULONG LevelCpu(BOOLEAN volume) const {
        return !volume && level.Map.MuteMode != UAC_VOL_NONE ?
               KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU : KSAUDIO_CPU_RESOURCES_HOST_CPU;
    }
private:
    /* The device format (index) a stream format maps to, or -1. The
     * container must be the subslot; valid bits may differ (a 24-bit
     * device takes 32-bit samples in a 32-bit slot and ignores the low
     * byte, as its subslot says). */
    /* Exact valid bits first; otherwise playback takes any device format
     * of the container, capture one whose bits the stream declares room
     * for. Accept and Run use this one rule. Declared positions must be
     * the device format's (no remapping); a plain format, an unassigned
     * mask (0) or a device that names none takes the device's order. */
    static LONG Lookup(const UAC_DIR_FORMATS *f, BOOLEAN capture, ULONG channels, ULONG bits,
                       ULONG container, ULONG rate, ULONG mask) {
        for (ULONG pass = 0; pass < 2; pass++)
            for (ULONG i = 0; i < f->Count; i++) {
                const UAC_FORMAT *x = &f->Format[i];
                ULONG device = UacPlanChannelMask(x->ChannelConfig, x->Channels);
                if (x->Channels != channels || (ULONG)x->Subslot * 8 != container) continue;
                if (mask != UAC_PCM_NO_MASK && mask && device && mask != device) continue;
                if (!pass ? x->Bits != bits : (capture && x->Bits > bits)) continue;
                for (ULONG k = 0; k < f->RateCount[i]; k++)
                    if (f->Rate[i][k] == rate) return (LONG)i;
            }
        return -1;
    }
    /* Playback may declare fewer valid bits than the device takes (the
     * device ignores the low ones its subslot does not use); capture must
     * declare at least the device's bits, which are delivered unchanged. */
    static BOOLEAN Accept(PVOID context, BOOLEAN capture, ULONG channels,
                          ULONG bits, ULONG container, ULONG rate, ULONG mask) {
        UAC_DIR *d = (UAC_DIR *)context;
        if (d->Capture != capture) return FALSE;
        return Lookup(&d->Formats, capture, channels, bits, container, rate, mask) >= 0;
    }
    static NTSTATUS Run(PVOID context, UacStream *stream) {
        UAC_DIR *d = (UAC_DIR *)context;
        Wave *w = d->Owner;
        KeWaitForSingleObject(&w->operation, Executive, KernelMode, FALSE, NULL);
        NTSTATUS status = STATUS_DEVICE_NOT_READY;
        if (!w->removed && !w->pnpBlocked && !w->powerBlocked && w->dev) {
            const UAC_PCM *pcm = stream->Format();
            LONG i = Lookup(&d->Formats, d->Capture, pcm->Channels, pcm->Bits, pcm->Container,
                            pcm->Rate, pcm->Mask);
            status = STATUS_NOT_SUPPORTED;
            if (d->Usb) status = STATUS_SUCCESS;
            else if (i >= 0) {
                d->Stream = stream;
                d->Channels = pcm->Channels;
                d->Subslot = d->Formats.Format[i].Subslot;
                status = UacUsbCreate(w->dev, d->Capture, d->Formats.Format[i].Terminal,
                    pcm->Channels, d->Formats.Format[i].Subslot, d->Formats.Format[i].Bits,
                    d->Formats.Format[i].ChannelConfig, pcm->Rate, Fill, Captured, Progress, d, &d->Usb);
            }
            /* The device may have lost its levels in D3 or a stop. */
            if (NT_SUCCESS(status) && !d->Capture && w->LevelStale()) w->ApplyLocked();
            if (NT_SUCCESS(status)) status = UacUsbStart(d->Usb);
            Record(w->device, 31, status);
        }
        KeReleaseMutex(&w->operation, FALSE);
        return status;
    }
    void Drain(UAC_DIR *d, BOOLEAN destroy) {
        if (d->Usb) {
            UacUsbStop(d->Usb);
            UAC_USB_STATS stats;
            UacUsbGetStats(d->Usb, &stats);
            if (!removed) RecordStats(device, &stats, d->Capture);
            if (destroy) { UacUsbDestroy(d->Usb); d->Usb = NULL; d->Stream = NULL; }
        }
    }
    static void Halt(PVOID context, UacStream *) {
        UAC_DIR *d = (UAC_DIR *)context;
        Wave *w = d->Owner;
        KeWaitForSingleObject(&w->operation, Executive, KernelMode, FALSE, NULL);
        w->Drain(d, FALSE);
        KeReleaseMutex(&w->operation, FALSE);
    }
    static void Close(PVOID context, UacStream *) {
        UAC_DIR *d = (UAC_DIR *)context;
        Wave *w = d->Owner;
        /* UacStream's device-associated cleanup guarantees PASSIVE even
         * if its final COM Release arrives at DISPATCH. Never free a
         * callback target before cancel-and-drain has returned. */
        PAGED_CODE();
        KeWaitForSingleObject(&w->operation, Executive, KernelMode, FALSE, NULL);
        w->Drain(d, TRUE);
        d->Closed++;
        if (!w->removed) RecordStreams(w->device, d->Attempts, d->Created, d->Closed, d->Active);
        KeReleaseMutex(&w->operation, FALSE);
    }
    static void Notify(PVOID context, PSERVICEGROUP group) {
        ((UAC_DIR *)context)->Owner->port->Notify(group);
    }
    /* One KSDATARANGE_AUDIO per (format, rate): a device clock offers
     * discrete rates, which a min/max range cannot say. */
    /* The order a format's rates are declared in. Playback: 48 kHz, then
     * 44.1 kHz, then the rest ascending; 98 SE's KMixer opens the device
     * at the first range it can (a 48 kHz client played at 44.1 kHz).
     * Capture stays ascending: with 48 kHz first, a capture-only mono
     * device faulted 98 SE's WDMAud at arrival (wdmaud(07)+2F3C,
     * 2026-10-24, 2 of 2), so capture may open at its lowest rate and
     * KMixer converts. */
    static ULONG RateRank(BOOLEAN capture, ULONG rate) {
        if (capture) return 0;
        return rate == 48000 ? 0 : rate == 44100 ? 1 : 2;
    }
    static void BuildRanges(UAC_DIR_FORMATS *f, BOOLEAN capture) {
        f->Ranges = 0;
        for (ULONG i = 0; i < f->Count; i++)
            for (ULONG pass = 0; pass < 3; pass++)
            for (ULONG k = 0; k < f->RateCount[i] && f->Ranges < UAC_RANGES; k++) {
                if (RateRank(capture, f->Rate[i][k]) != pass) continue;
                KSDATARANGE_AUDIO *r = &f->Range[f->Ranges];
                RtlZeroMemory(r, sizeof(*r));
                r->DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
                r->DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
                r->DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
                r->DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
                r->MaximumChannels = f->Format[i].Channels;
                r->MinimumBitsPerSample = r->MaximumBitsPerSample = (ULONG)f->Format[i].Subslot * 8;
                r->MinimumSampleFrequency = r->MaximumSampleFrequency = f->Rate[i][k];
                f->RangeList[f->Ranges] = (PKSDATARANGE)r;
                f->RangeFormat[f->Ranges] = i;
                f->Ranges++;
            }
    }
    /* Formats and rates of one direction; rates a planned alternate
     * cannot carry are dropped. */
    void Learn(UAC_DIR *d) {
        UAC_DIR_FORMATS *f = &d->Formats;
        UAC_FORMAT all[UAC_FORMATS];
        ULONG n = UacPlanFormats(descriptor, d->Capture, UacUsbHighSpeed(dev), all, UAC_FORMATS);
        /* One wave pin per direction: the direction's first terminal (an
         * X4's analog output, not its S/PDIF one). */
        UAC_BYTE terminal = (UAC_BYTE)UacPlanTerminal(descriptor, d->Capture, UacUsbHighSpeed(dev));
        f->Count = 0;
        for (ULONG i = 0; i < n; i++) {
            ULONG rates[UAC_USB_RATES], count = 0, kept = 0;
            if (all[i].Subslot < 2) continue; /* 8-bit: see UacStream::Parse */
            if (all[i].Terminal != terminal) continue;
            if (!NT_SUCCESS(UacUsbRates(dev, all[i].Terminal, rates, UAC_USB_RATES, &count))) continue;
            for (ULONG k = 0; k < count; k++) {
                unsigned int index;
                if (UacPlanPick(descriptor, d->Capture, terminal, all[i].Channels, all[i].Subslot,
                                all[i].Bits, all[i].ChannelConfig, rates[k],
                                UacUsbHighSpeed(dev), &index) == UAC_OK)
                    f->Rate[f->Count][kept++] = rates[k];
            }
            if (!kept) continue;
            f->Format[f->Count] = all[i];
            f->RateCount[f->Count] = kept;
            f->Count++;
        }
        BuildRanges(f, d->Capture);
    }
    static void Pin(PCPIN_DESCRIPTOR *p, ULONG instances, ULONG ranges, PKSDATARANGE *list,
                    KSPIN_DATAFLOW flow, KSPIN_COMMUNICATION comm, const GUID *category) {
        RtlZeroMemory(p, sizeof(*p));
        p->MaxGlobalInstanceCount = p->MaxFilterInstanceCount = instances;
        p->KsPinDescriptor.DataRangesCount = ranges;
        p->KsPinDescriptor.DataRanges = list;
        p->KsPinDescriptor.DataFlow = flow;
        p->KsPinDescriptor.Communication = comm;
        p->KsPinDescriptor.Category = category;
    }
public:
    Wave(PDEVICE_OBJECT fdo, PDEVICE_OBJECT target, UAC_DESC *model)
        : refs(1), device(fdo), lower(target), descriptor(model), dev(NULL), graph(NULL),
          port(NULL), removed(FALSE), pnpBlocked(FALSE), powerBlocked(FALSE) {
        KeInitializeMutex(&operation, 0);
        RtlZeroMemory(dir, sizeof(dir));
        RtlZeroMemory(&level, sizeof(level));
        dir[0].Owner = dir[1].Owner = this; dir[1].Capture = TRUE;
    }
    ~Wave() {
        /* Close and PnP Shutdown own all USB destruction at PASSIVE. */
        ASSERT(dir[0].Usb == NULL && dir[1].Usb == NULL);
        if (level.Item) IoFreeWorkItem(level.Item);
        if (dev) UacUsbClose(dev);
        if (descriptor) {
            ExFreePool((PVOID)descriptor->Bytes);
            ExFreePool(descriptor);
        }
    }
    /* PnP start: open the transport, learn both directions and build the
     * wave filter for what the device has. */
    NTSTATUS Prepare() {
        NTSTATUS s = UacUsbOpen(lower, descriptor, &dev);
        if (!NT_SUCCESS(s)) return s;
        Learn(&dir[0]); Learn(&dir[1]);
        graph = UacGraphFor(dir[0].Formats.Count != 0, dir[1].Formats.Count != 0);
        if (!graph) return STATUS_NOT_SUPPORTED;
        LevelSetup();
        if (level.Present && !(level.Item = IoAllocateWorkItem(device))) return STATUS_INSUFFICIENT_RESOURCES;
        ApplyLocked();
        ULONG c = 0;
        categories[c++] = KSCATEGORY_AUDIO;
        BOOLEAN render = dir[0].Formats.Count != 0;
        if (render) categories[c++] = KSCATEGORY_RENDER;
        if (graph->CapturePin >= 0) categories[c++] = KSCATEGORY_CAPTURE;
        if (graph->RenderPin >= 0) {
            /* Capture-only devices keep a stub render pin: no instances,
             * no PCM range (uactopo.c, the 98 SE WDMAud fault). */
            if (render)
                Pin(&pins[graph->RenderPin], 1, dir[0].Formats.Ranges, dir[0].Formats.RangeList,
                    KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO);
            else
                Pin(&pins[graph->RenderPin], 0, 1, bridgeRanges,
                    KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO);
            Pin(&pins[graph->RenderBridge], 0, 1, bridgeRanges,
                KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO);
            RtlZeroMemory(&nodes[graph->Dac], sizeof(nodes[0]));
            nodes[graph->Dac].Type = &KSNODETYPE_DAC;
        }
        if (graph->CapturePin >= 0) {
            Pin(&pins[graph->CapturePin], 1, dir[1].Formats.Ranges, dir[1].Formats.RangeList,
                KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_SINK, &PINNAME_CAPTURE);
            Pin(&pins[graph->CaptureBridge], 0, 1, bridgeRanges,
                KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO);
            RtlZeroMemory(&nodes[graph->Adc], sizeof(nodes[0]));
            nodes[graph->Adc].Type = &KSNODETYPE_ADC;
        }
        RtlZeroMemory(&filter, sizeof(filter));
        filter.PinSize = sizeof(PCPIN_DESCRIPTOR);
        filter.PinCount = graph->WavePins; filter.Pins = pins;
        filter.NodeSize = sizeof(PCNODE_DESCRIPTOR);
        filter.NodeCount = graph->WaveNodes; filter.Nodes = nodes;
        filter.ConnectionCount = graph->WaveLinkCount;
        filter.Connections = (const PCCONNECTION_DESCRIPTOR *)graph->WaveLinks;
        filter.CategoryCount = c; filter.Categories = categories;
        return STATUS_SUCCESS;
    }
    const UAC_GRAPH *Graph() const { return graph; }
    void Shutdown(BOOLEAN permanent) {
        PAGED_CODE();
        /* A level work item still to run takes the operation mutex: wait
         * for it outside the mutex (Changed queues no more once Closed).
         * A stopped Wave is not started again (Start makes a new one), so
         * a stop closes it too. */
        InterlockedExchange(&level.Closed, 1);
        while (level.Work) {
            LARGE_INTEGER t;
            t.QuadPart = -100000;   /* 10 ms */
            KeDelayExecutionThread(KernelMode, FALSE, &t);
        }
        KeWaitForSingleObject(&operation, Executive, KernelMode, FALSE, NULL);
        pnpBlocked = TRUE;
        Drain(&dir[0], TRUE); Drain(&dir[1], TRUE);
        if (permanent) removed = TRUE;
        KeReleaseMutex(&operation, FALSE);
    }
    STDMETHODIMP_(void) PowerChangeState(POWER_STATE state);
    STDMETHODIMP QueryPowerChangeState(POWER_STATE state);
    STDMETHODIMP QueryDeviceCapabilities(PDEVICE_CAPABILITIES caps);
    STDMETHODIMP QueryInterface(REFIID id, PVOID *out) {
        *out = NULL;
        if (IsEqualGUID(id, IID_IAdapterPowerManagment)) {
            *out = (IAdapterPowerManagement *)this; AddRef(); return STATUS_SUCCESS;
        }
        if (!IsEqualGUID(id, IID_IUnknown) && !IsEqualGUID(id, IID_IMiniport) &&
            !IsEqualGUID(id, IID_IMiniportWaveCyclic)) return STATUS_INVALID_PARAMETER;
        *out = (PMINIPORTWAVECYCLIC)this; AddRef(); return STATUS_SUCCESS;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() {
        LONG n = InterlockedDecrement(&refs);
        if (!n) { this->~Wave(); ExFreePool(this); }
        return n;
    }
    STDMETHODIMP GetDescription(PPCFILTER_DESCRIPTOR *out) {
        *out = &filter; return STATUS_SUCCESS;
    }
    /* PortCls's default handler makes only mono/stereo WAVEFORMATEX
     * (DDK, "Default Data Intersection Handlers"): this one answers every
     * declared range (one format at one rate), with WAVEFORMATEXTENSIBLE
     * beyond two channels or 16-bit containers. The client range must
     * admit it: channels, container bits and rate inside its bounds, or a
     * plain/wildcard range. */
    STDMETHODIMP DataRangeIntersection(ULONG pin, PKSDATARANGE range,
        PKSDATARANGE match, ULONG size, PVOID result, PULONG needed) {
        UAC_DIR *d = NULL;
        ULONG k, index = UAC_RANGES;
        *needed = 0;
        if (!graph || !range || !match) return STATUS_NO_MATCH;
        if ((LONG)pin == graph->RenderPin) d = &dir[0];
        else if ((LONG)pin == graph->CapturePin) d = &dir[1];
        else return STATUS_NOT_IMPLEMENTED;
        for (k = 0; k < d->Formats.Ranges; k++)
            if (d->Formats.RangeList[k] == match) { index = k; break; }
        if (index == UAC_RANGES) return STATUS_NO_MATCH;
        const KSDATARANGE_AUDIO *mine = &d->Formats.Range[index];
        const UAC_FORMAT *f = &d->Formats.Format[d->Formats.RangeFormat[index]];
        ULONG channels = f->Channels, container = (ULONG)f->Subslot * 8, rate = mine->MaximumSampleFrequency;
        if (!IsEqualGUID(range->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) &&
            !IsEqualGUID(range->MajorFormat, KSDATAFORMAT_TYPE_WILDCARD)) return STATUS_NO_MATCH;
        if (!IsEqualGUID(range->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
            !IsEqualGUID(range->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD)) return STATUS_NO_MATCH;
        if (IsEqualGUID(range->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) &&
            range->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
            const KSDATARANGE_AUDIO *c = (const KSDATARANGE_AUDIO *)range;
            if (c->MaximumChannels != (ULONG)-1 && c->MaximumChannels < channels) return STATUS_NO_MATCH;
            if (container < c->MinimumBitsPerSample || container > c->MaximumBitsPerSample) return STATUS_NO_MATCH;
            if (rate < c->MinimumSampleFrequency || rate > c->MaximumSampleFrequency) return STATUS_NO_MATCH;
        } else if (!IsEqualGUID(range->Specifier, KSDATAFORMAT_SPECIFIER_WILDCARD) &&
                   !IsEqualGUID(range->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
            return STATUS_NO_MATCH;
        }
        /* The device's own positions (bmChannelConfig); unassigned (0)
         * when it names none. KMixer mixes into an unassigned layout as
         * silence (98 SE, 8 channels, 2026-10-23). Plain PCM only where it
         * says the same: mono or front pair, 16 valid bits in 16. */
        ULONG positions = UacPlanChannelMask(f->ChannelConfig, channels);
        BOOLEAN extensible = channels > 2 || container != 16 || f->Bits != container ||
                             (positions && positions != (channels == 1 ? 0x4UL : 0x3UL));
        ULONG length = extensible ? sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE)
                                  : sizeof(KSDATAFORMAT_WAVEFORMATEX);
        *needed = length;
        if (!size) return STATUS_BUFFER_OVERFLOW;
        if (size < length || !result) return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(result, length);
        PKSDATAFORMAT out = (PKSDATAFORMAT)result;
        out->FormatSize = length;
        out->SampleSize = channels * (container / 8);
        out->MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
        out->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        out->Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
        PWAVEFORMATEX w = (PWAVEFORMATEX)(out + 1);
        w->wFormatTag = extensible ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_PCM;
        w->nChannels = (USHORT)channels;
        w->nSamplesPerSec = rate;
        w->wBitsPerSample = (USHORT)container;
        w->nBlockAlign = (USHORT)(channels * (container / 8));
        w->nAvgBytesPerSec = rate * w->nBlockAlign;
        if (extensible) {
            PWAVEFORMATEXTENSIBLE e = (PWAVEFORMATEXTENSIBLE)w;
            w->cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            e->Samples.wValidBitsPerSample = f->Bits;
            e->dwChannelMask = positions;
            e->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        }
        return STATUS_SUCCESS;
    }
    STDMETHODIMP Init(PUNKNOWN adapter, PRESOURCELIST resources, PPORTWAVECYCLIC port) {
        UNREFERENCED_PARAMETER(adapter); UNREFERENCED_PARAMETER(resources);
        this->port = port; return STATUS_SUCCESS;
    }
    STDMETHODIMP NewStream(PMINIPORTWAVECYCLICSTREAM *stream, PUNKNOWN outer,
        POOL_TYPE pool, ULONG pin, BOOLEAN capture, PKSDATAFORMAT format,
        PDMACHANNEL *dma, PSERVICEGROUP *service) {
        UNREFERENCED_PARAMETER(pool);
        *stream = NULL; *dma = NULL; *service = NULL;
        if (outer || !graph) return STATUS_INVALID_PARAMETER;
        if (capture ? (LONG)pin != graph->CapturePin : (LONG)pin != graph->RenderPin)
            return STATUS_INVALID_PARAMETER;
        if (!dir[capture ? 1 : 0].Formats.Count) return STATUS_INVALID_PARAMETER;
        UAC_DIR *d = &dir[capture ? 1 : 0];
        KeWaitForSingleObject(&operation, Executive, KernelMode, FALSE, NULL);
        UAC_STREAM_HOOKS hooks = { d, Run, Halt, Close, Notify, Accept };
        d->Attempts++;
        NTSTATUS status = removed || pnpBlocked || powerBlocked ? STATUS_DEVICE_NOT_READY :
            UacStream::Create((PMINIPORTWAVECYCLIC)this, &d->Active, format,
                stream, dma, service, &hooks, device, capture);
        if (NT_SUCCESS(status)) d->Created++;
        if (!removed) {
            Record(device, 30, status);
            RecordStreams(device, d->Attempts, d->Created, d->Closed, d->Active);
        }
        KeReleaseMutex(&operation, FALSE);
        return status;
    }
};

#pragma code_seg("PAGE")
STDMETHODIMP_(void) Wave::PowerChangeState(POWER_STATE state) {
    PAGED_CODE();
    KeWaitForSingleObject(&operation, Executive, KernelMode, FALSE, NULL);
    powerBlocked = state.DeviceState != PowerDeviceD0;
    if (powerBlocked) {
        Drain(&dir[0], FALSE); Drain(&dir[1], FALSE);
        /* The device may come back with its own levels: read them again. */
        for (ULONG n = 0; n < UAC_VOL_SLOTS; n++) level.Known[n] = level.MuteKnown[n] = FALSE;
        level.VolumeNormal = level.MuteNormal = FALSE;
    }
    KeReleaseMutex(&operation, FALSE);
}
STDMETHODIMP Wave::QueryPowerChangeState(POWER_STATE state) {
    PAGED_CODE(); UNREFERENCED_PARAMETER(state); return STATUS_SUCCESS;
}
STDMETHODIMP Wave::QueryDeviceCapabilities(PDEVICE_CAPABILITIES caps) {
    PAGED_CODE(); UNREFERENCED_PARAMETER(caps); return STATUS_SUCCESS;
}
#pragma code_seg()

static NTSTATUS LevelHandler(PPCPROPERTY_REQUEST r);
static const PCPROPERTY_ITEM volumeItems[] = {
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_VOLUMELEVEL,
      PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT, LevelHandler },
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_CPU_RESOURCES, PCPROPERTY_ITEM_FLAG_GET, LevelHandler }
};
static const PCPROPERTY_ITEM muteItems[] = {
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_MUTE,
      PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT, LevelHandler },
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_CPU_RESOURCES, PCPROPERTY_ITEM_FLAG_GET, LevelHandler }
};
DEFINE_PCAUTOMATION_TABLE_PROP(volumeTable, volumeItems);
DEFINE_PCAUTOMATION_TABLE_PROP(muteTable, muteItems);

class Topology : public IMiniportTopology {
    static void operator delete(PVOID memory) { ExFreePool(memory); }
    LONG refs;
    Wave *wave;                 /* referenced: the levels live there */
    const UAC_GRAPH *graph;
    PCPIN_DESCRIPTOR pins[4];
    PCNODE_DESCRIPTOR nodes[4];
    GUID categories[2];
    PCFILTER_DESCRIPTOR filter;
    static void Bridge(PCPIN_DESCRIPTOR *p, KSPIN_DATAFLOW flow, const GUID *category) {
        RtlZeroMemory(p, sizeof(*p));
        p->KsPinDescriptor.DataRangesCount = 1;
        p->KsPinDescriptor.DataRanges = bridgeRanges;
        p->KsPinDescriptor.DataFlow = flow;
        p->KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
        p->KsPinDescriptor.Category = category;
    }
public:
    Topology(const UAC_GRAPH *g, Wave *w) : refs(1), wave(w), graph(g) {
        wave->AddRef();
        RtlZeroMemory(pins, sizeof(pins)); RtlZeroMemory(nodes, sizeof(nodes));
        if (g->TopoRenderIn >= 0) {
            Bridge(&pins[g->TopoRenderIn], KSPIN_DATAFLOW_IN, &KSCATEGORY_AUDIO);
            Bridge(&pins[g->TopoSpeaker], KSPIN_DATAFLOW_OUT, &KSNODETYPE_SPEAKER);
        }
        if (g->TopoMic >= 0) {
            Bridge(&pins[g->TopoMic], KSPIN_DATAFLOW_IN, &KSNODETYPE_MICROPHONE);
            Bridge(&pins[g->TopoCaptureOut], KSPIN_DATAFLOW_OUT, &KSCATEGORY_AUDIO);
        }
        for (ULONG i = 0; i < g->TopoNodes; i++) nodes[i].Type = &KSNODETYPE_SUM;
        if (g->Volume >= 0) {
            nodes[g->Volume].Type = &KSNODETYPE_VOLUME;
            nodes[g->Volume].Name = &KSAUDFNAME_MASTER_VOLUME;
            nodes[g->Volume].AutomationTable = &volumeTable;
        }
        if (g->Mute >= 0) {
            nodes[g->Mute].Type = &KSNODETYPE_MUTE;
            nodes[g->Mute].Name = &KSAUDFNAME_MASTER_MUTE;
            nodes[g->Mute].AutomationTable = &muteTable;
        }
        categories[0] = KSCATEGORY_AUDIO; categories[1] = KSCATEGORY_TOPOLOGY;
        RtlZeroMemory(&filter, sizeof(filter));
        filter.PinSize = sizeof(PCPIN_DESCRIPTOR);
        filter.PinCount = g->TopoPins; filter.Pins = pins;
        filter.NodeSize = sizeof(PCNODE_DESCRIPTOR);
        filter.NodeCount = g->TopoNodes; filter.Nodes = nodes;
        filter.ConnectionCount = g->TopoLinkCount;
        filter.Connections = (const PCCONNECTION_DESCRIPTOR *)g->TopoLinks;
        filter.CategoryCount = 2; filter.Categories = categories;
    }
    STDMETHODIMP QueryInterface(REFIID id, PVOID *out) {
        *out = NULL;
        if (!IsEqualGUID(id, IID_IUnknown) && !IsEqualGUID(id, IID_IMiniport) &&
            !IsEqualGUID(id, IID_IMiniportTopology)) return STATUS_INVALID_PARAMETER;
        *out = (PMINIPORTTOPOLOGY)this; AddRef(); return STATUS_SUCCESS;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() {
        LONG n = InterlockedDecrement(&refs);
        if (!n) { this->~Topology(); ExFreePool(this); }
        return n;
    }
    ~Topology() { wave->Release(); }
    /* The node's property, for LevelHandler. */
    NTSTATUS Level(PPCPROPERTY_REQUEST r) {
        BOOLEAN volume = r->PropertyItem->Id == KSPROPERTY_AUDIO_VOLUMELEVEL;
        LONG node = r->PropertyItem->Id == KSPROPERTY_AUDIO_CPU_RESOURCES ? -2 :
                    volume ? graph->Volume : graph->Mute;
        if (node == -2) {
            if ((LONG)r->Node != graph->Volume && (LONG)r->Node != graph->Mute)
                return STATUS_INVALID_DEVICE_REQUEST;
            if (!(r->Verb & KSPROPERTY_TYPE_GET)) return STATUS_INVALID_DEVICE_REQUEST;
            if (!r->ValueSize) { r->ValueSize = sizeof(ULONG); return STATUS_BUFFER_OVERFLOW; }
            if (r->ValueSize < sizeof(ULONG) || !r->Value) return STATUS_BUFFER_TOO_SMALL;
            *(PULONG)r->Value = wave->LevelCpu((LONG)r->Node == graph->Volume);
            r->ValueSize = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        if (node < 0 || (LONG)r->Node != node) return STATUS_INVALID_DEVICE_REQUEST;
        return wave->LevelProperty(r, volume);
    }
    STDMETHODIMP GetDescription(PPCFILTER_DESCRIPTOR *out) {
        *out = &filter; return STATUS_SUCCESS;
    }
    STDMETHODIMP DataRangeIntersection(ULONG pin, PKSDATARANGE range,
        PKSDATARANGE match, ULONG size, PVOID result, PULONG needed) {
        UNREFERENCED_PARAMETER(pin); UNREFERENCED_PARAMETER(range);
        UNREFERENCED_PARAMETER(match); UNREFERENCED_PARAMETER(size);
        UNREFERENCED_PARAMETER(result); *needed = 0; return STATUS_NOT_IMPLEMENTED;
    }
    STDMETHODIMP Init(PUNKNOWN adapter, PRESOURCELIST resources, PPORTTOPOLOGY port) {
        UNREFERENCED_PARAMETER(adapter); UNREFERENCED_PARAMETER(resources);
        UNREFERENCED_PARAMETER(port); return STATUS_SUCCESS;
    }
};

/* PortCls passes the miniport it was given as MajorTarget. */
static NTSTATUS LevelHandler(PPCPROPERTY_REQUEST r)
{
    if (!r || !r->MajorTarget || !r->PropertyItem) return STATUS_INVALID_PARAMETER;
    return ((Topology *)(PMINIPORTTOPOLOGY)r->MajorTarget)->Level(r);
}

struct EXT { PDEVICE_OBJECT Pdo, Lower; Wave *Miniport; };
static PDRIVER_DISPATCH PortClsPnp;
static EXT *Extension(PDEVICE_OBJECT fdo) {
    return (EXT *)((PUCHAR)fdo->DeviceExtension + PORT_CLASS_DEVICE_EXTENSION_SIZE);
}

/* Per-instance M0 diagnostics survive a failed START (Code 10), without
 * relying on a kernel debugger. Best effort: diagnostics never change the
 * result of the operation being tested. No device identifiers are recorded.
 */
static void Record(PDEVICE_OBJECT fdo, ULONG stage, NTSTATUS status)
{
    HANDLE key;
    UNICODE_STRING stageName = { 22, 24, L"Uac2M0Stage" };
    UNICODE_STRING statusName = { 24, 26, L"Uac2M0Status" };
    UNICODE_STRING buildName = { 18, 20, L"Uac2Build" };
    ULONG build = 17;
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension(fdo)->Pdo,
            PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key))) {
        ZwSetValueKey(key, &stageName, 0, REG_DWORD, &stage, sizeof(stage));
        ZwSetValueKey(key, &statusName, 0, REG_DWORD, &status, sizeof(status));
        ZwSetValueKey(key, &buildName, 0, REG_DWORD, &build, sizeof(build));
        ZwClose(key);
    }
}

static void RecordDescriptors(PDEVICE_OBJECT fdo, PUCHAR bytes, ULONG length,
                              ULONG first, ULONG count, ULONG control, ULONG result)
{
    HANDLE key;
    ULONG parse[4] = { first, count, control, result };
    UNICODE_STRING configName = { 24, 26, L"Uac2M0Config" };
    UNICODE_STRING parseName = { 22, 24, L"Uac2M0Parse" };
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension(fdo)->Pdo,
            PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key))) {
        ZwSetValueKey(key, &configName, 0, REG_BINARY, bytes, length);
        ZwSetValueKey(key, &parseName, 0, REG_BINARY, parse, sizeof(parse));
        ZwClose(key);
    }
}

static void RecordStats(PDEVICE_OBJECT fdo, UAC_USB_STATS *stats, BOOLEAN capture)
{
    HANDLE key;
    UNICODE_STRING render = { 24, 26, L"Uac2UsbStats" };
    UNICODE_STRING input = { 28, 30, L"Uac2UsbStatsIn" };
    UNICODE_STRING name = capture ? input : render;
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension(fdo)->Pdo,
            PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key))) {
        ZwSetValueKey(key, &name, 0, REG_BINARY, stats, sizeof(*stats));
        ZwClose(key);
    }
}

static void RecordStreams(PDEVICE_OBJECT fdo, ULONG attempts, ULONG created, ULONG closed, ULONG active)
{
    HANDLE key;
    ULONG counts[4] = { attempts, created, closed, active };
    UNICODE_STRING name = { 22, 24, L"Uac2Streams" };
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension(fdo)->Pdo,
            PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key))) {
        ZwSetValueKey(key, &name, 0, REG_BINARY, counts, sizeof(counts));
        ZwClose(key);
    }
}

static void Name(UNICODE_STRING *u, PWSTR name)
{
    USHORT n = 0;
    while (name[n]) n++;
    u->Buffer = name; u->Length = (USHORT)(n * sizeof(WCHAR)); u->MaximumLength = u->Length + sizeof(WCHAR);
}

static void PutValue(PDEVICE_OBJECT fdo, PWSTR name, const void *data, ULONG size)
{
    HANDLE key;
    UNICODE_STRING u;
    Name(&u, name);
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension(fdo)->Pdo,
            PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key))) {
        ZwSetValueKey(key, &u, 0, REG_BINARY, (PVOID)data, size);
        ZwClose(key);
    }
}

/* A REG_BINARY value of exactly size bytes. */
static BOOLEAN GetValue(PDEVICE_OBJECT fdo, PWSTR name, void *data, ULONG size)
{
    HANDLE key;
    UNICODE_STRING u;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 128];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    ULONG got = 0;
    BOOLEAN ok = FALSE;
    if (size > 128) return FALSE;
    Name(&u, name);
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension(fdo)->Pdo,
            PLUGPLAY_REGKEY_DRIVER, KEY_QUERY_VALUE, &key))) {
        if (NT_SUCCESS(ZwQueryValueKey(key, &u, KeyValuePartialInformation, info, sizeof(buffer), &got)) &&
            info->Type == REG_BINARY && info->DataLength == size) {
            RtlCopyMemory(data, info->Data, size);
            ok = TRUE;
        }
        ZwClose(key);
    }
    return ok;
}

static NTSTATUS Descriptor(PDEVICE_OBJECT lower, PVOID buffer, USHORT size)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST urb;
    KEVENT event;
    IO_STATUS_BLOCK ios;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    urb.TransferBufferLength = size;
    urb.TransferBuffer = buffer;
    urb.DescriptorType = USB_CONFIGURATION_DESCRIPTOR_TYPE;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    PIRP irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB,
        lower, NULL, 0, NULL, 0, TRUE, &event, &ios);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = &urb;
    NTSTATUS s = IoCallDriver(lower, irp);
    if (s == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        s = ios.Status;
    }
    if (NT_SUCCESS(s) && (!USBD_SUCCESS(urb.Hdr.Status) || urb.TransferBufferLength != size))
        s = STATUS_DEVICE_DATA_ERROR;
    return s;
}

static int Hex(WCHAR c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return -1;
}

static NTSTATUS ParseDevice(PDEVICE_OBJECT fdo, UAC_DESC **model)
{
    *model = NULL;
    EXT *ext = Extension(fdo);
    WCHAR ids[256]; ULONG used;
    NTSTATUS s = IoGetDeviceProperty(ext->Pdo, DevicePropertyHardwareID,
                                     sizeof(ids), ids, &used);
    Record(fdo, 1, s);
    if (!NT_SUCCESS(s)) return s;
    int first = -1;
    for (ULONG i = 0; i + 6 < used / sizeof(WCHAR); i++) {
        if (ids[i] == L'&' && ids[i+1] == L'M' && ids[i+2] == L'I' && ids[i+3] == L'_' &&
            Hex(ids[i+4]) >= 0 && Hex(ids[i+5]) >= 0) {
            first = Hex(ids[i+4]) * 16 + Hex(ids[i+5]); break;
        }
    }
    /* No MI number: the whole device is bound (98 SE under NUSB's EHCI,
     * whose hub does not split an IAD device); the function is found in
     * the configuration below (UacWholeFirst). */
    USB_CONFIGURATION_DESCRIPTOR header;
    s = Descriptor(ext->Lower, &header, sizeof(header));
    Record(fdo, 2, s);
    if (!NT_SUCCESS(s)) return s;
    if (header.bLength != sizeof(header) || header.bDescriptorType != 2 ||
        header.wTotalLength < sizeof(header)) return STATUS_DEVICE_DATA_ERROR;
    PUCHAR bytes = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, header.wTotalLength, 'd2aU');
    UAC_DESC *parsed = (UAC_DESC *)ExAllocatePoolWithTag(NonPagedPool, sizeof(UAC_DESC), 'p2aU');
    if (!bytes || !parsed) { s = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    s = Descriptor(ext->Lower, bytes, header.wTotalLength);
    Record(fdo, 3, s);
    if (NT_SUCCESS(s)) {
        unsigned int count = 0, control = 256, whole = 256;
        int rc = UAC_OK;
        if (first < 0) {
            rc = UacWholeFirst(bytes, header.wTotalLength, &whole);
            first = (int)whole;
        }
        if (!rc) rc = UacFunction(bytes, header.wTotalLength, first, &control, &count);
        if (!rc) rc = UacParse(bytes, header.wTotalLength, control, first, count, parsed);
        else RtlZeroMemory(parsed, sizeof(*parsed));
        RecordDescriptors(fdo, bytes, header.wTotalLength, first, count, control, rc);
        DbgPrint("uac2wdm: descriptor result %d, PCM alternates %u, skipped %u\n",
                 rc, parsed->AltCount, parsed->Skipped);
        if (rc) s = STATUS_DEVICE_CONFIGURATION_ERROR;
        Record(fdo, 4, s);
    }
done:
    if (NT_SUCCESS(s)) { *model = parsed; parsed = NULL; bytes = NULL; }
    if (parsed) ExFreePool(parsed);
    if (bytes) ExFreePool(bytes);
    return s;
}

static NTSTATUS Start(PDEVICE_OBJECT fdo, PIRP irp, PRESOURCELIST resources)
{
    EXT *ext = Extension(fdo);
    if (ext->Miniport) {
        ext->Miniport->Shutdown(TRUE);
        ext->Miniport->Release(); ext->Miniport = NULL;
    }
    UAC_DESC *model = NULL;
    NTSTATUS s = ParseDevice(fdo, &model);
    if (!NT_SUCCESS(s)) return s;
    PPORT wave = NULL, topo = NULL;
    void *wm = ExAllocatePoolWithTag(NonPagedPool, sizeof(Wave), 'w2aU');
    void *tm = ExAllocatePoolWithTag(NonPagedPool, sizeof(Topology), 't2aU');
    if (!wm || !tm) {
        if (wm) ExFreePool(wm); if (tm) ExFreePool(tm);
        ExFreePool((PVOID)model->Bytes); ExFreePool(model);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Wave *w = new(wm) Wave(fdo, ext->Lower, model);
    ext->Miniport = w; w->AddRef(); /* explicit adapter/PnP reference */
    Topology *t = NULL;
    s = w->Prepare();
    Record(fdo, 13, s);
    if (NT_SUCCESS(s)) t = new(tm) Topology(w->Graph(), w);
    else ExFreePool(tm);
    const UAC_GRAPH *g = w->Graph();
    if (NT_SUCCESS(s)) { s = PcNewPort(&wave, CLSID_PortWaveCyclic); Record(fdo, 5, s); }
    if (NT_SUCCESS(s)) { s = wave->Init(fdo, irp, (PMINIPORTWAVECYCLIC)w, NULL, resources); Record(fdo, 6, s); }
    if (NT_SUCCESS(s)) { s = PcNewPort(&topo, CLSID_PortTopology); Record(fdo, 7, s); }
    if (NT_SUCCESS(s)) { s = topo->Init(fdo, irp, t, NULL, resources); Record(fdo, 8, s); }
    if (NT_SUCCESS(s)) { s = PcRegisterSubdevice(fdo, L"Wave", wave); Record(fdo, 9, s); }
    if (NT_SUCCESS(s)) { s = PcRegisterSubdevice(fdo, L"Topology", topo); Record(fdo, 10, s); }
    if (NT_SUCCESS(s) && g->RenderBridge >= 0) {
        s = PcRegisterPhysicalConnection(fdo, wave, g->RenderBridge, topo, g->TopoRenderIn); Record(fdo, 11, s);
    }
    if (NT_SUCCESS(s) && g->CaptureBridge >= 0) {
        s = PcRegisterPhysicalConnection(fdo, topo, g->TopoCaptureOut, wave, g->CaptureBridge); Record(fdo, 14, s);
    }
    if (NT_SUCCESS(s)) { s = PcRegisterAdapterPowerManagement((PMINIPORTWAVECYCLIC)w, fdo); Record(fdo, 12, s); }
    if (!NT_SUCCESS(s)) {
        w->Shutdown(TRUE);
        ext->Miniport = NULL; w->Release();
    }
    w->Release(); if (t) t->Release();
    if (wave) wave->Release(); if (topo) topo->Release();
    DbgPrint("uac2wdm: start %08X\n", s);
    return s;
}

static NTSTATUS Add(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo)
{
    NTSTATUS s = PcAddAdapterDevice(driver, pdo, Start, 2,
                                    PORT_CLASS_DEVICE_EXTENSION_SIZE + sizeof(EXT));
    if (!NT_SUCCESS(s)) return s;
    /* Old-target path from the design: no XP-only IoGetLowerDeviceObject.
     * AddDevice runs before PnP starts this stack. */
    PDEVICE_OBJECT lower = pdo, fdo = pdo->AttachedDevice;
    while (fdo && fdo->DriverObject != driver) { lower = fdo; fdo = fdo->AttachedDevice; }
    if (!fdo) return STATUS_NO_SUCH_DEVICE;
    Extension(fdo)->Pdo = pdo;
    Extension(fdo)->Lower = lower;
    Extension(fdo)->Miniport = NULL;
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT fdo, PIRP irp)
{
    /* PnP removal/stop IRPs run at PASSIVE. The extension owns a strong
     * Wave reference until this wrapper clears it before PortCls REMOVE.
     * No extension or device access occurs after forwarding REMOVE. */
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    if (stack->MinorFunction == IRP_MN_STOP_DEVICE ||
        stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL ||
        stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        PAGED_CODE();
        EXT *ext = Extension(fdo);
        Wave *w = ext->Miniport;
        if (w) {
            w->Shutdown(stack->MinorFunction != IRP_MN_STOP_DEVICE);
            if (stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
                ext->Miniport = NULL;
                w->Release();
            }
        }
    }
    return PortClsPnp(fdo, irp);
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path)
{
    NTSTATUS status = PcInitializeAdapterDriver(driver, path, Add);
    if (NT_SUCCESS(status)) {
        PortClsPnp = driver->MajorFunction[IRP_MJ_PNP];
        driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
    }
    return status;
}
