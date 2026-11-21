/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * uac2's (and uac1's) WaveCyclic/USB streaming: render and capture in
 * the formats and rates the device's descriptors and clocks offer.
 * Original code from DDK headers and published PortCls documentation.
 * Built as uac2wdm.sys (USB Audio 2.0) and, with UAC_V1 defined, as
 * uac1wdm.sys (USB Audio 1.0, docs/design/uac2.md section 26).
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
#include "uaccap.h"
/* Design 28.15: the playback level engine. The default build uses
 * uaclevel.c; UAC_LEVEL_RETAINED (defined on the compiler command line)
 * builds the shipped implementation kept below instead. uaclevel.obj is
 * linked in both builds; preprocessing selects one Wave path, so exactly
 * one is referenced, and a retained image's linker (/Gy, /OPT:REF)
 * removes the unreferenced engine functions. */
#ifndef UAC_LEVEL_RETAINED
#include "uaclevel.h"
#endif
#ifdef UAC_V1
#include "uac1desc.h"
#include "uacconv.h"
#endif
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
#ifndef PNP_DEVICE_FAILED
#define PNP_DEVICE_FAILED 0x00000004   /* wdm.h's PNP_DEVICE_STATE flags */
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
static PDEVICE_OBJECT PdoOf(PDEVICE_OBJECT fdo);
static void RecordStreams(PDEVICE_OBJECT fdo, ULONG attempts, ULONG created, ULONG closed, ULONG active);
static void RecordWaits(PDEVICE_OBJECT fdo, ULONG waits, ULONG timeouts, ULONG ms);
static void PutValue(PDEVICE_OBJECT fdo, PWSTR name, const void *data, ULONG size);
static BOOLEAN GetValue(PDEVICE_OBJECT fdo, PWSTR name, void *data, ULONG size);
static ULONG GetDwordRaw(PDEVICE_OBJECT fdo, PWSTR value);
static ULONG GetQueueRaw(PDEVICE_OBJECT fdo);
static ULONG GetRealtimeRaw(PDEVICE_OBJECT fdo);
static UAC_GATE *GateOf(PDEVICE_OBJECT fdo);
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
/* docs/design/uac2.md 30: a bounded mitigation for 98 SE and ME, where
 * the remove comes with no surprise removal before it while clients still
 * hold the device's pins and filters. Observed with the trace build's
 * markers (x4full removals while recording): after the forward to PortCls a
 * client thread stopped and closed the pin and its filter; where a final
 * Wave release was logged before that close, the close did not return and
 * the KS fault screen came up, or the client's state request returned
 * pending and nothing followed. The mitigation: at PortCls's first call
 * back into a miniport during that remove (a release, on the PnP thread, at
 * PASSIVE, once), wait while a stream of the device is not yet destroyed,
 * a create or close is executing, or one ended less than
 * UAC_REMOVE_QUIET_MS ago; never past UAC_REMOVE_HOLD_MS
 * (UacPlanRemoveHold). It is a timing rule, not a barrier: a client that
 * starts late or pauses longer than the quiet time inside its teardown can
 * still meet the old order, and at the cap the remove goes on regardless.
 * A remove after a surprise removal (2000 and later) waits for nothing.
 * The hold is not taken (UacPlanRemoveHoldTake) when nothing of the device
 * is left to tear down (no stream, no handle, no create or close
 * executing), or when PortCls's last device power state for it was not
 * D0. With a uac2 and a uac1 device removed together, the uac1 device had
 * been set to D3 during the other's remove, and a client's close that came
 * in its remove returned pending and the remove did not return; asked back
 * to D0 before its remove (UacPlanRemoveWake, PcRequestNewPowerState), its
 * closes in the hold returned (section 30.6).
 *
 * Each device's entry is allocated at AddDevice (no allocation, no device:
 * AddDevice fails before PcAddAdapterDevice), kept in a list outside the
 * device extension, and freed when PortCls's remove has returned; a close
 * or destroy that comes later finds no entry and touches nothing. The list
 * holds no reference on the device object: an entry is found by address. */
struct UAC_FILES {
    LIST_ENTRY Link;
    PDEVICE_OBJECT Fdo;
    LONG Opens, Streams;
    LONG Busy;                  /* creates and closes executing in PortCls's dispatch */
    ULONGLONG Last;             /* interrupt time of the last create or close end, or stream destroy */
    PVOID Remover;              /* the thread in PortCls's remove (98 SE, ME), or NULL */
    LONG Held;                  /* that remove's hold has been taken */
    LONG Down;                  /* PortCls's last device power state for it was not D0 */
};
static KSPIN_LOCK FilesLock;
static LIST_ENTRY FilesList;
static UAC_FILES *FilesFind(PDEVICE_OBJECT fdo)
{
    for (PLIST_ENTRY e = FilesList.Flink; e != &FilesList; e = e->Flink) {
        UAC_FILES *f = CONTAINING_RECORD(e, UAC_FILES, Link);
        if (f->Fdo == fdo) return f;
    }
    return NULL;
}
/* AddDevice: the entry, allocated before the device exists (NULL: fail). */
static UAC_FILES *FilesAlloc(void)
{
    UAC_FILES *f = (UAC_FILES *)ExAllocatePoolWithTag(NonPagedPool, sizeof(UAC_FILES), 'f2aU');
    if (f) RtlZeroMemory(f, sizeof(*f));
    return f;
}
static void FilesLink(UAC_FILES *f, PDEVICE_OBJECT fdo)
{
    KIRQL old;
    f->Fdo = fdo;
    KeAcquireSpinLock(&FilesLock, &old);
    InsertTailList(&FilesList, &f->Link);
    KeReleaseSpinLock(&FilesLock, old);
}
/* opens, streams, busy: the changes; a change of busy or streams ending
 * (busy -1, streams -1) is stamped. */
static void FilesNote(PDEVICE_OBJECT fdo, LONG opens, LONG streams, LONG busy)
{
    KIRQL old;
    KeAcquireSpinLock(&FilesLock, &old);
    UAC_FILES *f = FilesFind(fdo);
    if (f) {
        f->Opens += opens; f->Streams += streams; f->Busy += busy;
        f->Last = KeQueryInterruptTime();
    }
    KeReleaseSpinLock(&FilesLock, old);
}
/* PortCls's device power state for the device: down when not D0. */
static void FilesPower(PDEVICE_OBJECT fdo, BOOLEAN down)
{
    KIRQL old;
    KeAcquireSpinLock(&FilesLock, &old);
    UAC_FILES *f = FilesFind(fdo);
    if (f) f->Down = down;
    KeReleaseSpinLock(&FilesLock, old);
}
static BOOLEAN FilesDown(PDEVICE_OBJECT fdo)
{
    KIRQL old;
    BOOLEAN down = FALSE;
    KeAcquireSpinLock(&FilesLock, &old);
    UAC_FILES *f = FilesFind(fdo);
    if (f) down = f->Down != 0;
    KeReleaseSpinLock(&FilesLock, old);
    return down;
}
/* The remove's forward: thread marks the hold's thread; forget unlinks and
 * frees the entry once PortCls's remove has returned. */
static void FilesRemover(PDEVICE_OBJECT fdo, PVOID thread, BOOLEAN forget)
{
    KIRQL old;
    UAC_FILES *gone = NULL;
    KeAcquireSpinLock(&FilesLock, &old);
    UAC_FILES *f = FilesFind(fdo);
    if (f) {
        if (forget) { RemoveEntryList(&f->Link); gone = f; }
        else { f->Remover = thread; f->Held = 0; }
    }
    KeReleaseSpinLock(&FilesLock, old);
    if (gone) ExFreePool(gone);
}
static void RemoveHold(PDEVICE_OBJECT fdo)
{
    KIRQL old;
    BOOLEAN hold = FALSE;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    KeAcquireSpinLock(&FilesLock, &old);
    UAC_FILES *f = FilesFind(fdo);
    LONG streams0 = 0, opens0 = 0, busy0 = 0, down = 0;
    if (f && f->Remover && f->Remover == (PVOID)PsGetCurrentThread() && !f->Held) {
        f->Held = 1; hold = TRUE;
        streams0 = f->Streams; opens0 = f->Opens; busy0 = f->Busy; down = f->Down;
    }
    KeReleaseSpinLock(&FilesLock, old);
    if (!hold) return;
    /* 30: no hold for a device powered down, or with nothing left to tear down */
    if (!UacPlanRemoveHoldTake(!down, streams0 > 0 ? (ULONG)streams0 : 0, opens0 > 0 ? (ULONG)opens0 : 0,
                               busy0 > 0 ? (ULONG)busy0 : 0)) {
        UAC_TRACE(("rmskip", (ULONG)streams0 << 16 | (ULONG)(busy0 & 0xFFFF), (ULONG)opens0, (ULONG)down));
        return;
    }
    ULONGLONG start = KeQueryInterruptTime();
    for (;;) {
        LONG streams = 0, opens = 0, busy = 0;
        ULONGLONG last = start, now;
        KeAcquireSpinLock(&FilesLock, &old);
        f = FilesFind(fdo);
        if (f) { streams = f->Streams; opens = f->Opens; busy = f->Busy; if (f->Last > last) last = f->Last; }
        KeReleaseSpinLock(&FilesLock, old);
        now = KeQueryInterruptTime();
        /* 100 ns units to milliseconds with no 64-bit divide (VC6). */
        ULONG quiet = (ULONG)((now - last) >> 4) / 625, held = (ULONG)((now - start) >> 4) / 625;
        if (!UacPlanRemoveHold(streams > 0 ? (ULONG)streams : 0, busy > 0 ? (ULONG)busy : 0, quiet, held)) {
            UAC_TRACE(("rmhold", (ULONG)streams << 16 | (ULONG)(busy & 0xFFFF), (ULONG)opens, held));
            break;
        }
        LARGE_INTEGER t;
        t.QuadPart = -100000;   /* 10 ms */
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
}
/* 30: a stream counts from Create's publication to the end of its
 * destroy (uacstream.h); a failed creation never counts. */
#define UAC_STREAM_BORN(device) FilesNote((device), 0, 1, 0)
#define UAC_STREAM_GONE(device) FilesNote((device), 0, -1, 0)
#include "uacstream.h"

#define UAC_PAIRS 4
/* 27: how long an open waits for the previous stream's queued destroy. */
#define UAC_CLOSE_WAIT_MS 2000

class Wave;
/* One direction of the wave filter: its formats, its one-stream gate and
 * the transport stream of the open pin instance. */
struct UAC_DIR {
    Wave *Owner;
    BOOLEAN Capture;
    LONG Active;
    ULONG Attempts, Created, Closed;
    ULONG Waits, WaitTimeouts, WaitMs; /* 27: opens that met a closing stream */
    UAC_USB *Usb;
    UacStream *Stream;          /* the stream Usb serves */
    ULONG Channels, Subslot;    /* its device format */
    ULONG QueueRead, QueueMs;   /* Uac2QueueMs as the open pin read it (19.3) */
    UAC_DIR_FORMATS Formats;
#ifdef UAC_V1
    /* 26.12: the open stream's wire format (UAC_TAG_PCM: none converted),
     * and a frame's bytes on the wire and in the ring (Subslot above is
     * the ring's, 2, for a converted format). */
    ULONG Tag, WireSub, WireFrame, RingFrame;
#endif
};

#ifdef UAC_LEVEL_RETAINED
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
    BOOLEAN AboveZero;          /* Uac2VolAboveZero: the whole range offered (section 20) */
    BOOLEAN Lowered;            /* a control was above the top offered at start: set to it */
    unsigned int RangeRead, StartKnown;  /* bit n: slot n's RANGE read, CUR read at start */
    int StartActual[UAC_VOL_SLOTS];      /* volume CUR at start, where StartKnown */
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
    /* Build 25 (section 20), after the fields above: the top the device's
     * range reaches (KsMax is the top offered), Uac2VolAboveZero in force,
     * and whether the start lowered a control. */
    LONG KsTop;
    ULONG AboveZero, Lowered;
};
#else
/* The engine's state with the driver's part: the spin lock its Lock
 * callback takes, and the work item that applies SETs made at raised
 * IRQL, as in the retained UAC_LEVEL. */
struct UAC_LEVEL_HOSTED : public UAC_LEVEL {
    KSPIN_LOCK Lock;
    PIO_WORKITEM Item;          /* preallocated: applies SETs made at raised IRQL */
    LONG Pending, Queued, Work; /* a SET to apply; Item queued; work items not finished */
    LONG Closed;                /* removal: no more work items */
};
#endif

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
#ifdef UAC_LEVEL_RETAINED
    UAC_LEVEL level;
#else
    UAC_LEVEL_HOSTED level;
#endif
    /* Endpoint pair `index` (docs/design/uac2.md section 15.3): pair 0
     * owns the function (descriptor model, transport); a later pair holds
     * a reference to pair 0 and borrows them. Pair 0 names the others
     * (not referenced) for its power changes, under its operation mutex. */
    ULONG index;
    Wave *primary;
    Wave *peers[UAC_PAIRS - 1];
    ULONG peerCount;
    WCHAR levelName[16];        /* Uac2Level (pair 0), Uac2LevelPxx */
    const GUID *jack[2];        /* the bridge category of each direction */
    PCPIN_DESCRIPTOR pins[4];
    PCNODE_DESCRIPTOR nodes[2];
    GUID categories[3];
    PCFILTER_DESCRIPTOR filter;

    /* The USB callbacks' context is the direction (Run). */
#ifdef UAC_V1
    /* 26.12: a converted format passes through a buffer of whole frames
     * on the worker's stack: 16-bit frames from the ring, the software
     * volume on them, the wire format out (every wire byte written, so
     * silence is the format's own); and the other way for capture. */
    enum { ConvSamples = 128 };
    static void Fill(PVOID context, PUCHAR bytes, ULONG count) {
        UAC_DIR *d = (UAC_DIR *)context;
        if (d->Tag == UAC_TAG_PCM) {
            d->Stream->FillBytes(bytes, count);
            if (!d->Capture) d->Owner->Soften(d, bytes, count);
            return;
        }
        UCHAR ring[2 * ConvSamples];
        ULONG frames = count / d->WireFrame, step = ConvSamples / d->Channels;
        while (frames) {
            ULONG n = frames < step ? frames : step;
            d->Stream->FillBytes(ring, n * d->RingFrame);
            d->Owner->Soften(d, ring, n * d->RingFrame);
            UacConvOut(d->Tag, d->WireSub, ring, n * d->Channels, bytes);
            bytes += n * d->WireFrame; frames -= n;
        }
    }
    static void Captured(PVOID context, const UCHAR *bytes, ULONG count) {
        UAC_DIR *d = (UAC_DIR *)context;
        if (d->Tag == UAC_TAG_PCM) { d->Stream->WriteBytes(bytes, count); return; }
        UCHAR ring[2 * ConvSamples];
        ULONG frames = count / d->WireFrame, step = ConvSamples / d->Channels;
        while (frames) {
            ULONG n = frames < step ? frames : step;
            UacConvIn(d->Tag, d->WireSub, bytes, n * d->Channels, ring);
            d->Stream->WriteBytes(ring, n * d->RingFrame);
            bytes += n * d->WireFrame; frames -= n;
        }
    }
    static void Progress(PVOID context, ULONG bytes, ULONG errors) {
        UAC_DIR *d = (UAC_DIR *)context;
        if (d->Tag != UAC_TAG_PCM) bytes = bytes / d->WireFrame * d->RingFrame;
        d->Stream->CompleteBytes(bytes, errors);
    }
#else
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
#endif
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
#ifdef UAC_V1
        /* USB Audio 1.0 (ADC-1 5.2.2.4): SET_CUR 0x01, GET_CUR 0x81 and,
         * as `attribute`, GET_MIN 0x82, GET_MAX 0x83, GET_RES 0x84;
         * wValue selector << 8 | channel, wIndex unit << 8 | interface. */
        UCHAR setup[8];
        if (selector > 255 || channel > 255 || size > 0xffff) return STATUS_INVALID_PARAMETER;
        setup[0] = read ? 0xA1 : 0x21;
        setup[1] = (UCHAR)(!read ? 0x01 : attribute == UAC_CTL_CUR ? 0x81 : attribute);
        setup[2] = (UCHAR)channel; setup[3] = (UCHAR)selector;
        setup[4] = descriptor->Control; setup[5] = unit;
        setup[6] = (UCHAR)size; setup[7] = (UCHAR)(size >> 8);
#else
        UAC_CTL_TARGET t;
        UCHAR setup[8];
        t.Interface = descriptor->Control; t.Entity = unit; t.Endpoint = 0;
        t.Channel = channel; t.MaxChannel = 255;
        if (!UacCtlSetup(UAC_CTL_FEATURE, selector, &t, read ? 1 : 3, attribute, read ? 1 : 0,
                         size, 0, setup)) return STATUS_INVALID_PARAMETER;
#endif
        ULONG length = size;
        NTSTATUS s = UacUsbClassRequest(dev, setup, bytes, &length);
        if (got) *got = length;
        else if (NT_SUCCESS(s) && length != size) s = STATUS_DEVICE_DATA_ERROR;
        return s;
    }
#ifdef UAC_LEVEL_RETAINED
    /* Room for UAC_VOL_RANGES subranges in one request; a device that
     * refuses it is asked for one subrange, the wLength Linux uses
     * ([linux] sound/usb/mixer.c), and its first subrange is the range. */
    BOOLEAN ReadRange(UAC_BYTE unit, ULONG channel, UAC_VOL_RANGE *range) {
        if (!ReadRangeOnce(unit, channel, range)) return FALSE;
        if (channel < UAC_VOL_SLOTS) level.RangeRead |= 1u << channel;
        return TRUE;
    }
#ifdef UAC_V1
    /* USB Audio 1.0: GET_MIN, GET_MAX and GET_RES (2 bytes each), held as
     * the one subrange of a RANGE answer, under UacVolRange's rules. */
    BOOLEAN ReadRangeOnce(UAC_BYTE unit, ULONG channel, UAC_VOL_RANGE *range) {
        UCHAR b[8];
        ULONG i;
        b[0] = 1; b[1] = 0;
        for (i = 0; i < 3; i++)
            if (!NT_SUCCESS(Feature(unit, 2, channel, TRUE, 0x82 + i, b + 2 + 2 * i, 2))) return FALSE;
        return UacVolRange(b, sizeof(b), range) ? TRUE : FALSE;
    }
#else
    BOOLEAN ReadRangeOnce(UAC_BYTE unit, ULONG channel, UAC_VOL_RANGE *range) {
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
#endif
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
    /* What an unknown volume is taken as: the top the device's range
     * reaches (KsTop, above the 0 dB the driver offers, section 20); an
     * extra channel volume without a RANGE, the most a CUR can say (+128
     * dB). Gains also silences that channel while the volume is owned. */
    int Assumed(ULONG n) const {
        if (n && (level.NoRange & (1u << (n - 1)))) return 0x7FFF;
        return (int)(level.Range[n].KsTop / 256);
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
        /* A Feature Unit an earlier pair drives stays that pair's: here the
         * level is software only, so no pair changes another's loudness
         * behind its back (section 15.3 item 6). */
        if (primary && m->VolumeUnit && primary->UnitTaken(m->VolumeUnit)) {
            m->VolumeMode = UAC_VOL_NONE; m->VolumeUnit = 0;
            m->VolumeChannels = 0; m->VolumeExtra = 0;
        }
        if (primary && m->MuteUnit && primary->UnitTaken(m->MuteUnit)) {
            m->MuteMode = UAC_VOL_NONE; m->MuteUnit = 0;
            m->MuteChannels = 0; m->MuteExtra = 0;
        }
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
        /* Uac2VolAboveZero, REG_DWORD 1: the device's whole range is
         * offered, gain above 0 dB included (build 24 and before); else
         * the top offered is the highest step at or below 0 dB (section 20). */
        v->AboveZero = GetDwordRaw(device, L"Uac2VolAboveZero") == 1;
        if (v->AboveZero)
            for (n = 0; n < UAC_VOL_SLOTS; n++) v->Range[n].KsMax = v->Range[n].KsTop;
        ReadState();
        for (n = 0; n < UAC_VOL_SLOTS; n++)
            if (v->Known[n]) { v->StartKnown |= 1u << n; v->StartActual[n] = v->Actual[n]; }
        /* A written control whose CUR, read now, is above the top offered
         * is set to that top, that control alone: a lowering only, nothing
         * taken over, no other control (a master-mode extra's attenuation
         * included) touched. A CUR not read is left as it is (section 20). */
        if (m->VolumeMode != UAC_VOL_NONE && DeviceReady())
            for (n = 0; n < UAC_VOL_SLOTS; n++)
                if (VolumeSlot(n, &extra) && !(extra && (v->NoRange & (1u << (n - 1)))) && v->Known[n] &&
                    v->Actual[n] != UAC_VOL_SILENCE && UacVolToKs(v->Actual[n]) > v->Range[n].KsMax) {
                    int top = (int)(v->Range[n].KsMax / 256);
                    if (WriteCur(m->VolumeUnit, 2, n, top)) { v->Actual[n] = top; v->Lowered = TRUE; }
                    else v->Known[n] = FALSE;   /* may or may not have taken it: Assumed */
                }
        /* The levels: restored, else the device's own (0 dB in software). */
        for (c = 0; c < v->Channels; c++) {
            n = m->VolumeMode == UAC_VOL_MASTER ? 0 : c + 1;
            v->Level[c] = m->VolumeMode != UAC_VOL_NONE && v->Known[n] ? Level(v->Actual[n], c) : 0;
        }
        /* Muted when every controlled channel is known muted. */
        v->Mute = m->MuteMode != UAC_VOL_NONE;
        for (n = m->MuteMode == UAC_VOL_MASTER ? 0 : 1; n < UAC_VOL_SLOTS && v->Mute; n++)
            if (MuteSlot(n, &extra) && !extra && !(v->MuteKnown[n] && v->ActualMute[n])) v->Mute = 0;
        if (GetValue(device, levelName, &saved, sizeof(saved)) && saved.Version == 2 &&
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
                PutValue(device, levelName, &save, sizeof(save));
                v->Saved = change;
            }
            RecordLevel(levels, mute);
        }
    }
    /* The render line's volume as found at start, in the stats words
     * after build 24's (uacusb.h, design 20.5). */
    void VolumeStats(UAC_USB_STATS *s) const {
        const UAC_LEVEL *v = &level;
        s->VolLayout = 25;
        s->VolUnit = v->Map.VolumeUnit; s->VolMode = v->Map.VolumeMode;
        s->VolChannels = v->Map.VolumeChannels; s->VolAboveZero = v->AboveZero;
        s->VolLowered = v->Lowered; s->VolRangeRead = v->RangeRead; s->VolStartKnown = v->StartKnown;
        for (ULONG n = 0; n < 3; n++) {
            const UAC_VOL_RANGE *r = &v->Range[n];
            BOOLEAN read = (v->RangeRead & (1u << n)) != 0;
            s->VolCount[n] = read ? r->Count : 0;
            s->VolMin[n] = read ? r->Min[0] : 0;
            s->VolMax[n] = read && r->Count ? r->Max[r->Count - 1] : 0;
            s->VolRes[n] = read ? (LONG)(r->KsStep / 256) : 0;
            s->VolTop[n] = read ? (LONG)(r->KsTop / 256) : 0;
            s->VolOffered[n] = read ? (LONG)(r->KsMax / 256) : 0;
            s->VolStartCur[n] = (v->StartKnown & (1u << n)) ? v->StartActual[n] : 0;
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
        g.KsTop = KsRange(0)->KsTop; g.AboveZero = v->AboveZero; g.Lowered = v->Lowered;
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
#else
    /* Design 28.15: the playback level engine is uaclevel.c (UAC_LEVEL,
     * the engine's state, inside UAC_LEVEL_HOSTED with the driver's spin
     * lock and work item); these are its callbacks, each what the
     * retained methods above did themselves. */
    UAC_LEVEL_OPS levelOps;
    KIRQL levelIrql;            /* the level spin lock's IRQL while held */
    static long OpFeature(void *c, UAC_BYTE unit, unsigned long selector, unsigned long channel, int read,
                          unsigned long attribute, void *bytes, unsigned long size, unsigned long *got) {
        return ((Wave *)c)->Feature(unit, selector, channel, read ? TRUE : FALSE, attribute, bytes, size, got);
    }
    static int OpReady(void *c) { return ((Wave *)c)->DeviceReady() ? 1 : 0; }
    static int OpRunning(void *c) { return ((Wave *)c)->Running() ? 1 : 0; }
    static int OpRemoved(void *c) { return ((Wave *)c)->removed ? 1 : 0; }
    static void OpDelay40(void *c) {
        LARGE_INTEGER t;
        UNREFERENCED_PARAMETER(c);
        t.QuadPart = -400000;   /* 40 ms: the queued USB audio plays out */
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    static void OpLock(void *c) { Wave *w = (Wave *)c; KeAcquireSpinLock(&w->level.Lock, &w->levelIrql); }
    static void OpUnlock(void *c) { Wave *w = (Wave *)c; KeReleaseSpinLock(&w->level.Lock, w->levelIrql); }
    static void OpPublish(void *c, long *target, const unsigned long *gain) {
        UNREFERENCED_PARAMETER(c);
        for (ULONG k = 0; k < UAC_VOL_CHANNELS; k++) InterlockedExchange(&target[k], (LONG)gain[k]);
    }
    /* The engine asks one value by name, Uac2VolAboveZero. */
    static unsigned long OpGetDword(void *c, const char *name) {
        static const char want[] = "Uac2VolAboveZero";
        ULONG i;
        for (i = 0; want[i] && name[i] == want[i]; i++) ;
        if (want[i] || name[i]) return 0xFFFFFFFFUL;
        return GetDwordRaw(((Wave *)c)->device, L"Uac2VolAboveZero");
    }
    static int OpGetValue(void *c, int pair, void *data, unsigned long size) {
        Wave *w = (Wave *)c;
        UNREFERENCED_PARAMETER(pair);   /* levelName is this pair's (Uac2Level, Uac2LevelPnn) */
        return GetValue(w->device, w->levelName, data, size) ? 1 : 0;
    }
    static void OpPutValue(void *c, int pair, void *data, unsigned long size) {
        Wave *w = (Wave *)c;
        UNREFERENCED_PARAMETER(pair);
        PutValue(w->device, w->levelName, data, size);
    }
    static void OpPutDiag(void *c, void *data, unsigned long size) {
        PutValue(((Wave *)c)->device, L"Uac2VolMap", data, size);
    }
    static int OpTaken(void *c, UAC_BYTE unit) {
        Wave *w = (Wave *)c;
        return w->primary && w->primary->UnitTaken(unit) ? 1 : 0;
    }
    const UAC_VOL_RANGE *KsRange(ULONG c) const { return UacLevelKsRange(&level, c); }
    /* PnP start, after Learn: the render line's controls and levels. The
     * driver's part (the lock, the work item's counters) is cleared as the
     * retained LevelSetup's RtlZeroMemory of the whole level cleared it. */
    void LevelSetup() {
        ULONG c, most = 1, formats = dir[0].Formats.Count;
        unsigned int terminal = 0;
        level.Item = NULL; level.Pending = level.Queued = level.Work = level.Closed = 0;
        KeInitializeSpinLock(&level.Lock);
        levelOps.Context = this;
        levelOps.Feature = OpFeature; levelOps.Ready = OpReady; levelOps.Running = OpRunning;
        levelOps.Removed = OpRemoved; levelOps.Delay40 = OpDelay40; levelOps.Lock = OpLock;
        levelOps.Unlock = OpUnlock; levelOps.Publish = OpPublish; levelOps.GetDword = OpGetDword;
        levelOps.GetValue = OpGetValue; levelOps.PutValue = OpPutValue; levelOps.PutDiag = OpPutDiag;
        levelOps.Taken = OpTaken;
        for (c = 0; c < formats; c++)
            if (dir[0].Formats.Format[c].Channels > most) most = dir[0].Formats.Format[c].Channels;
        if (formats) terminal = dir[0].Formats.Format[0].Terminal;
        UacLevelSetup(&level, &levelOps, (int)index, descriptor, formats, most, terminal);
    }
    BOOLEAN DeviceReady() const { return dev && !removed && !pnpBlocked && !powerBlocked; }
    BOOLEAN Running() const { return dir[0].Usb != NULL; }
    BOOLEAN LevelStale() { return UacLevelStale(&level) ? TRUE : FALSE; }
    void ApplyLocked() { UacLevelApply(&level); }
    void VolumeStats(UAC_USB_STATS *s) const {
        UAC_LEVEL_STATS v;
        UacLevelStats(&level, &v);
        s->VolLayout = v.VolLayout; s->VolUnit = v.VolUnit; s->VolMode = v.VolMode;
        s->VolChannels = v.VolChannels; s->VolAboveZero = v.VolAboveZero; s->VolLowered = v.VolLowered;
        s->VolRangeRead = v.VolRangeRead; s->VolStartKnown = v.VolStartKnown;
        for (ULONG n = 0; n < 3; n++) {
            s->VolCount[n] = v.VolCount[n]; s->VolMin[n] = v.VolMin[n]; s->VolMax[n] = v.VolMax[n];
            s->VolRes[n] = v.VolRes[n]; s->VolTop[n] = v.VolTop[n]; s->VolOffered[n] = v.VolOffered[n];
            s->VolStartCur[n] = v.VolStartCur[n];
        }
    }
#endif
    /* Design 28.18, stage 1: this pair's capture graph (nonpaged pool,
     * built once in Prepare; NULL when there is no capture line or no
     * memory) and its Uac2VolMapC record. Nothing reads the graph yet.
     * The last members, so every earlier member keeps its offset. */
    UAC_CAP_GRAPH *capGraph;
    UCHAR capDiag[UAC_CAP_DIAG_SIZE];
    void Apply() {
        /* docs/design/uac2.md 29.9: on a failed device a SET stays in the
         * cache and nothing is written. The operation mutex is taken as
         * NewStream and Run take it (29.5.7): a SET that waits behind a
         * request that then expires, and the level work item Shutdown
         * waits for, give up once the device has failed, even while the
         * holder stays blocked in the cancellation's drain. */
        if (!TakeOperation()) return;
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
#ifdef UAC_LEVEL_RETAINED
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
            /* Trace builds: each accepted SET (pair, volume or mute,
             * Change read after it, outside the lock: a trace only; the
             * channel; the value asked). */
            UAC_TRACE(("lvset", index << 24 | (volume ? 1UL : 0UL) << 16 | (v->Change & 0xFFFF), (ULONG)channel,
                       (ULONG)value));
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
#else
    /* KSPROPERTY_AUDIO_VOLUMELEVEL (volume) or KSPROPERTY_AUDIO_MUTE on
     * the topology filter's node, through the engine (uaclevel.c): the
     * same checks, GET of the loudest channel for -1, a SET snapped and
     * owned, applied at once at PASSIVE or by the work item. */
    NTSTATUS LevelProperty(PPCPROPERTY_REQUEST r, BOOLEAN volume) {
        UAC_LEVEL_HOSTED *v = &level;
        if (!v->Present) return STATUS_INVALID_DEVICE_REQUEST;
        if (r->Verb & KSPROPERTY_TYPE_BASICSUPPORT) return LevelBasic(r, volume);
        if (r->InstanceSize < sizeof(LONG) || !r->Instance) return STATUS_INVALID_PARAMETER;
        LONG channel = *(PLONG)r->Instance;
        if (channel != -1 && (channel < 0 || (ULONG)channel >= v->Channels)) return STATUS_INVALID_PARAMETER;
        if (!r->ValueSize) { r->ValueSize = sizeof(LONG); return STATUS_BUFFER_OVERFLOW; }
        if (r->ValueSize < sizeof(LONG) || !r->Value) return STATUS_BUFFER_TOO_SMALL;
        if (r->Verb & KSPROPERTY_TYPE_GET) {
            *(PLONG)r->Value = UacLevelGet(v, volume ? 1 : 0, channel);
            r->ValueSize = sizeof(LONG);
            return STATUS_SUCCESS;
        }
        if (r->Verb & KSPROPERTY_TYPE_SET) {
            if (v->Closed) return STATUS_DEVICE_NOT_READY;
            UacLevelSet(v, volume ? 1 : 0, channel, *(PLONG)r->Value);
            /* Trace builds: as the retained handler's (Change read after
             * the SET, outside the lock: a trace only). */
            UAC_TRACE(("lvset", index << 24 | (volume ? 1UL : 0UL) << 16 | (v->Change & 0xFFFF), (ULONG)channel,
                       (ULONG)*(PLONG)r->Value));
            Changed();
            return STATUS_SUCCESS;
        }
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    ULONG LevelCpu(BOOLEAN volume) const {
        return UacLevelNotHostCpu(&level, volume ? 1 : 0) ?
               KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU : KSAUDIO_CPU_RESOURCES_HOST_CPU;
    }
#endif
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
    /* docs/design/uac2.md 29.5.7: the operation mutex, for an entry that
     * may be refused (NewStream, Run, a level SET). Inactive gate: the
     * untimed wait of before. Active: UacGateAcquire's 100 ms tries that
     * read the gate, and its read after the acquisition; FALSE (nothing
     * held) once failed. */
    static int OperationFailed(void *c) { return UacGateFailed(GateOf(((Wave *)c)->device)) ? 1 : 0; }
    static int OperationTry(void *c) {
        LARGE_INTEGER t;
        t.QuadPart = -1000000;
        return KeWaitForSingleObject(&((Wave *)c)->operation, Executive, KernelMode, FALSE, &t) ==
               STATUS_SUCCESS;
    }
    static void OperationRelease(void *c) { KeReleaseMutex(&((Wave *)c)->operation, FALSE); }
    BOOLEAN TakeOperation() {
        if (!GateOf(device)->Active) {
            KeWaitForSingleObject(&operation, Executive, KernelMode, FALSE, NULL);
            return TRUE;
        }
        return UacGateAcquire(this, OperationFailed, OperationTry, OperationRelease) ? TRUE : FALSE;
    }
    static NTSTATUS Run(PVOID context, UacStream *stream) {
        UAC_DIR *d = (UAC_DIR *)context;
        Wave *w = d->Owner;
        /* 29.5.5: a failed device refuses before any wait. */
        if (UacGateFailed(GateOf(w->device)) || !w->TakeOperation()) return STATUS_DEVICE_NOT_READY;
        NTSTATUS status = STATUS_DEVICE_NOT_READY;
        if (!w->removed && !w->pnpBlocked && !w->powerBlocked && w->dev && !UacGateFailed(GateOf(w->device))) {
            const UAC_PCM *pcm = stream->Format();
            LONG i = Lookup(&d->Formats, d->Capture, pcm->Channels, pcm->Bits, pcm->Container,
                            pcm->Rate, pcm->Mask);
            status = STATUS_NOT_SUPPORTED;
            if (d->Usb) status = STATUS_SUCCESS;
            else if (i >= 0) {
                d->Stream = stream;
                d->Channels = pcm->Channels;
                d->Subslot = d->Formats.Format[i].Subslot;
#ifdef UAC_V1
                /* 26.12: the wire format; the ring's size passed in wire
                 * bytes, so the queue stays below the ring (19.4). */
                const UAC_FORMAT *f = &d->Formats.Format[i];
                d->Tag = f->FormatTag; d->WireSub = f->WireSubslot;
                d->WireFrame = pcm->Channels * f->WireSubslot;
                d->RingFrame = pcm->Channels * f->Subslot;
                status = UacUsbCreate(w->dev, d->Capture, f->Terminal, pcm->Channels, f->WireSubslot,
                    UAC_PLAN_BITS(f->FormatTag, f->WireBits), f->ChannelConfig, pcm->Rate,
                    Fill, Captured, Progress, d, d->QueueRead, d->QueueMs,
                    stream->BufferSize() / d->RingFrame * d->WireFrame, &d->Usb);
#else
                status = UacUsbCreate(w->dev, d->Capture, d->Formats.Format[i].Terminal,
                    pcm->Channels, d->Formats.Format[i].Subslot, d->Formats.Format[i].Bits,
                    d->Formats.Format[i].ChannelConfig, pcm->Rate, Fill, Captured, Progress, d,
                    d->QueueRead, d->QueueMs, stream->BufferSize(), &d->Usb);
#endif
            }
            /* 19.6: at every RUN, a stream kept from a pause or stop
             * (Drain without destroy) included. */
            if (NT_SUCCESS(status) && d->Usb) UacUsbSetRealtime(d->Usb, GetRealtimeRaw(w->device));
            /* The device may have lost its levels in D3 or a stop. */
            if (NT_SUCCESS(status) && !d->Capture && w->LevelStale()) w->ApplyLocked();
            if (NT_SUCCESS(status)) status = UacUsbStart(d->Usb);
            /* 29.5.7: a late success never lets a RUN succeed on a failed
             * device. */
            if (NT_SUCCESS(status) && UacGateFailed(GateOf(w->device))) {
                UacUsbStop(d->Usb);
                status = STATUS_DEVICE_NOT_READY;
            }
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
            VolumeStats(&stats);
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
        /* One wave pin per direction: the direction's first terminal (an
         * X4's analog output, not its S/PDIF one), and only its formats, so
         * an on-board codec's other outputs do not crowd them out. */
        unsigned int ends[UAC_PAIRS];
        ULONG count = UacPlanEndpoints(descriptor, d->Capture, UacUsbHighSpeed(dev), ends, UAC_PAIRS);
        f->Count = 0;
        if (index >= count) { BuildRanges(f, d->Capture); return; }
        UAC_BYTE terminal = (UAC_BYTE)ends[index];
        jack[d->Capture ? 1 : 0] = JackGuid(UacVolJack(descriptor, d->Capture, terminal), d->Capture);
        ULONG n = UacPlanFormatsFor(descriptor, d->Capture, UacUsbHighSpeed(dev), terminal, all,
                                    UAC_FORMATS);
        f->Count = 0;
        for (ULONG i = 0; i < n; i++) {
            ULONG rates[UAC_USB_RATES], count = 0, kept = 0;
            if (all[i].Subslot < 2) continue; /* 8-bit: see UacStream::Parse */
            if (all[i].Terminal != terminal) continue;
            if (!NT_SUCCESS(UacUsbRates(dev, all[i].Terminal, rates, UAC_USB_RATES, &count))) continue;
            for (ULONG k = 0; k < count; k++) {
                unsigned int index;
#ifdef UAC_V1
                if (UacPlanPick(descriptor, d->Capture, terminal, all[i].Channels, all[i].WireSubslot,
                                UAC_PLAN_BITS(all[i].FormatTag, all[i].WireBits), all[i].ChannelConfig,
                                rates[k], UacUsbHighSpeed(dev), &index) == UAC_OK)
#else
                if (UacPlanPick(descriptor, d->Capture, terminal, all[i].Channels, all[i].Subslot,
                                all[i].Bits, all[i].ChannelConfig, rates[k],
                                UacUsbHighSpeed(dev), &index) == UAC_OK)
#endif
                    f->Rate[f->Count][kept++] = rates[k];
            }
            if (!kept) continue;
            f->Format[f->Count] = all[i];
            f->RateCount[f->Count] = kept;
            f->Count++;
        }
#ifdef UAC_V1
        /* 26.12: native PCM first, a converted format only at the rates
         * nothing of its pin view ranked above it carries. */
        f->Count = UacPlanOffer(f->Format, f->RateCount, &f->Rate[0][0], UAC_USB_RATES, f->Count);
#endif
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
    Wave(PDEVICE_OBJECT fdo, PDEVICE_OBJECT target, UAC_DESC *model, ULONG pair = 0,
         Wave *owner = NULL)
        : refs(1), device(fdo), lower(target), descriptor(model), dev(NULL), graph(NULL),
          port(NULL), removed(FALSE), pnpBlocked(FALSE), powerBlocked(FALSE),
          index(pair), primary(owner), peerCount(0), capGraph(NULL) {
        KeInitializeMutex(&operation, 0);
        RtlZeroMemory(capDiag, sizeof(capDiag));
        RtlZeroMemory(dir, sizeof(dir));
        RtlZeroMemory(&level, sizeof(level));
        RtlZeroMemory(peers, sizeof(peers));
        dir[0].Owner = dir[1].Owner = this; dir[1].Capture = TRUE;
        jack[0] = &KSNODETYPE_SPEAKER; jack[1] = &KSNODETYPE_MICROPHONE;
        if (primary) { primary->AddRef(); descriptor = primary->descriptor; }
        LevelName();
    }
    ~Wave() {
        /* Close and PnP Shutdown own all USB destruction at PASSIVE. */
        ASSERT(dir[0].Usb == NULL && dir[1].Usb == NULL);
        if (level.Item) IoFreeWorkItem(level.Item);
        if (capGraph) ExFreePool(capGraph);
        if (primary) { primary->Release(); return; }
        if (dev) UacUsbClose(dev);
        if (descriptor) {
            ExFreePool((PVOID)descriptor->Bytes);
            ExFreePool(descriptor);
        }
    }
    /* The endpoints of pair 0 and the pairs after it (Start). */
    /* Several pairs only when every endpoint has a clock source of its
     * own, read without a selector choice: until a clock reservation at
     * stream open exists (section 15.3 item 5), endpoints that share a
     * clock stay one pair, as before. */
#ifdef UAC_V1
    /* USB Audio 1.0 has no clock entities to tell endpoints apart by:
     * one pair, the first playback and capture terminal (section 26.4). */
    ULONG Pairs() const { return 1; }
#else
    ULONG Pairs() const {
        unsigned int t[2][UAC_PAIRS];
        UAC_BYTE seen[2 * UAC_PAIRS], source;
        ULONG n[2], i, j, count = 0;
        int hs = UacUsbHighSpeed(dev);
        n[0] = UacPlanEndpoints(descriptor, 0, hs, t[0], UAC_PAIRS);
        n[1] = UacPlanEndpoints(descriptor, 1, hs, t[1], UAC_PAIRS);
        if (n[0] < 2 && n[1] < 2) return 1;
        for (i = 0; i < 2; i++)
            for (j = 0; j < n[i]; j++) {
                if (UacClock(descriptor, (UAC_BYTE)t[i][j], NULL, &source) != UAC_OK) return 1;
                for (ULONG k = 0; k < count; k++) if (seen[k] == source) return 1;
                seen[count++] = source;
            }
        /* Nor when two playback paths meet in the Feature Unit either would
         * drive for its volume or mute (section 15.3 item 6). */
        UAC_BYTE units[4 * UAC_PAIRS];
        ULONG used = 0;
        for (j = 0; j < n[0]; j++) {
            /* With each channel count LevelSetup can ask (mono, a pair). */
            for (ULONG ch = 1; ch <= 2; ch++) {
                UAC_VOL_MAP m;
                UacVolFind(descriptor, t[0][j], ch, &m);
                UAC_BYTE u[2] = { m.VolumeUnit, m.MuteUnit };
                for (i = 0; i < 2; i++) {
                    if (!u[i]) continue;
                    for (ULONG k = 0; k < used; k++) if (units[k] == u[i] && (k / 4) != j) return 1;
                }
                units[used++] = u[0]; units[used++] = u[1];
            }
        }
        return n[0] > n[1] ? n[0] : n[1];
    }
#endif
    /* A Feature Unit pair 0 or an earlier pair drives (pair 0 only). */
    BOOLEAN UnitTaken(UAC_BYTE unit) {
        BOOLEAN taken = FALSE;
        const UAC_VOL_MAP *m = &level.Map;
        if (m->VolumeUnit == unit || m->MuteUnit == unit) return TRUE;
        KeWaitForSingleObject(&operation, Executive, KernelMode, FALSE, NULL);
        for (ULONG i = 0; i < peerCount && !taken; i++) {
            const UAC_VOL_MAP *p = &peers[i]->level.Map;
            if (p->VolumeUnit == unit || p->MuteUnit == unit) taken = TRUE;
        }
        KeReleaseMutex(&operation, FALSE);
        return taken;
    }
    void AddPeer(Wave *w) {
        KeWaitForSingleObject(&operation, Executive, KernelMode, FALSE, NULL);
        if (peerCount < UAC_PAIRS - 1) peers[peerCount++] = w;
        KeReleaseMutex(&operation, FALSE);
    }
    /* The registry value of this pair's levels: pair 0 keeps Uac2Level,
     * the name before section 15; a later pair its own. */
    void LevelName() {
        static const WCHAR hex[] = L"0123456789ABCDEF";
        static const WCHAR base[] = L"Uac2Level";
        ULONG i;
        for (i = 0; base[i]; i++) levelName[i] = base[i];
        if (index) {
            levelName[i++] = L'P';
            levelName[i++] = hex[(index >> 4) & 15];
            levelName[i++] = hex[index & 15];
        }
        levelName[i] = 0;
    }
    static const GUID *JackGuid(unsigned int type, BOOLEAN capture) {
        /* A playback bridge is a speaker or headphones only: on XP WDMAud
         * made an S/PDIF output bridge (KSNODETYPE_SPDIF_INTERFACE) a mixer
         * destination of type WAVEIN with no waveOut target, so that
         * output's mixer had no speaker line (the X4 gate, 2026-10-30). */
        if (!capture) return type == 0x0302 ? &KSNODETYPE_HEADPHONES : &KSNODETYPE_SPEAKER;
        switch (type) {
        case 0x0601: return &KSNODETYPE_ANALOG_CONNECTOR;
        case 0x0603: return &KSNODETYPE_LINE_CONNECTOR;
        case 0x0605: return &KSNODETYPE_SPDIF_INTERFACE;
        default: return &KSNODETYPE_MICROPHONE;
        }
    }
    const GUID *Jack(BOOLEAN capture) const { return jack[capture ? 1 : 0]; }
    /* Design 28.18, stage 1: the capture endpoint's graph, from the
     * descriptors alone (no request to the device), at PASSIVE at the end
     * of a successful Prepare, and its route diagnostics written once
     * (Uac2VolMapC, Uac2VolMapCnn). The graph's storage comes from
     * nonpaged pool, never the stack, with no spin lock held. No other
     * state reads it yet: whether the graph is complete, incomplete or not
     * allocated at all, everything else the driver does is as before. */
    void Discover() {
        WCHAR name[UAC_CAP_NAME_UNITS];
        ULONG c, most = 1;
        const UAC_DIR_FORMATS *f = &dir[1].Formats;
        if (!f->Count) return;
        for (c = 0; c < f->Count; c++) if (f->Format[c].Channels > most) most = f->Format[c].Channels;
        capGraph = (UAC_CAP_GRAPH *)ExAllocatePoolWithTag(NonPagedPool, sizeof(UAC_CAP_GRAPH), 'g2aU');
        UacCapStart(descriptor, f->Format[0].Terminal, most, capGraph, capDiag);
        UacCapDiagName(index, (unsigned short *)name);
        PutValue(device, name, capDiag, sizeof(capDiag));
    }
    /* PnP start: open the transport, learn both directions and build the
     * wave filter for what the device has. */
    NTSTATUS Prepare() {
        NTSTATUS s = STATUS_SUCCESS;
        if (primary) dev = primary->dev;
        else s = UacUsbOpen(lower, PdoOf(device), descriptor, GateOf(device), &dev);
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
        Discover();
        return STATUS_SUCCESS;
    }
    const UAC_GRAPH *Graph() const { return graph; }
    PDEVICE_OBJECT Device() const { return device; }
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
        peerCount = 0;
        KeReleaseMutex(&operation, FALSE);
    }
    STDMETHODIMP_(void) PowerChangeState(POWER_STATE state);
    void PowerLocal(POWER_STATE state);
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
        RemoveHold(device);   /* 30: PortCls's remove on 98 SE and ME */
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
        /* 27: the stream of the pin closed just before may still hold the
         * gate until its queued destroy runs Close, which takes the
         * operation mutex: wait for it here, before taking the mutex. */
        /* 29.5.5, 29.5.7: a failed device refuses before the wait for a
         * closing stream, which itself reads the gate, as does the wait for
         * the operation mutex. */
        UAC_GATE *g = GateOf(device);
        if (UacGateFailed(g)) return STATUS_DEVICE_NOT_READY;
        ULONG waitedMs = 0;
        BOOLEAN waited = UacStream::WaitClosing(&d->Active, UAC_CLOSE_WAIT_MS, &waitedMs,
                                                g->Active ? &g->Failed : NULL);
        if (!TakeOperation()) return STATUS_DEVICE_NOT_READY;
        UAC_STREAM_HOOKS hooks = { d, Run, Halt, Close, Notify, Accept };
        d->Attempts++;
        if (waited) {
            d->Waits++; d->WaitMs = waitedMs;
            if (d->Active == UacStream::GateClosing) d->WaitTimeouts++;
            if (!removed) RecordWaits(device, d->Waits, d->WaitTimeouts, d->WaitMs);
        }
        /* One reading sizes this pin's ring and, at RUN, its queue (19.3).
         * The gate admits one stream a direction, so nothing else uses
         * d->Queue* while this one is open. */
        ULONG queueRead = GetQueueRaw(device), queueMs = UacPlanQueueMs(queueRead);
        NTSTATUS status = removed || pnpBlocked || powerBlocked || UacGateFailed(g) ? STATUS_DEVICE_NOT_READY :
            UacStream::Create((PMINIPORTWAVECYCLIC)this, &d->Active, format,
                stream, dma, service, &hooks, device, capture, queueMs);
        if (NT_SUCCESS(status)) { d->QueueRead = queueRead; d->QueueMs = queueMs; }
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
/* PortCls calls pair 0, the one registered for adapter power: it takes the
 * later pairs' streams down (and back) too, before its own. */
STDMETHODIMP_(void) Wave::PowerChangeState(POWER_STATE state) {
    PAGED_CODE();
    FilesPower(device, state.DeviceState != PowerDeviceD0);   /* 30 */
    KeWaitForSingleObject(&operation, Executive, KernelMode, FALSE, NULL);
    for (ULONG i = 0; i < peerCount; i++) peers[i]->PowerLocal(state);
    KeReleaseMutex(&operation, FALSE);
    PowerLocal(state);
}
void Wave::PowerLocal(POWER_STATE state) {
    PAGED_CODE();
    KeWaitForSingleObject(&operation, Executive, KernelMode, FALSE, NULL);
    powerBlocked = state.DeviceState != PowerDeviceD0;
    UAC_TRACE(("power", index, (ULONG)state.DeviceState, 0));
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
            Bridge(&pins[g->TopoSpeaker], KSPIN_DATAFLOW_OUT, w->Jack(FALSE));
        }
        if (g->TopoMic >= 0) {
            Bridge(&pins[g->TopoMic], KSPIN_DATAFLOW_IN, w->Jack(TRUE));
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
        RemoveHold(wave->Device());   /* 30: PortCls's remove on 98 SE and ME */
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

/* Name: the product string descriptor as read at start, NameLength 0
 * when there is none to use; Maker: the manufacturer's (iManufacturer),
 * MakerLength 0 when there is none (docs/design/uac2.md sections 21,
 * 21.7). */
struct EXT { PDEVICE_OBJECT Pdo, Lower; Wave *Miniport; Wave *Pair[UAC_PAIRS - 1];
             ULONG NameLength; UCHAR Name[256]; ULONG MakerLength; UCHAR Maker[256];
             /* docs/design/uac2.md 29.5.5, 29.9: the gate, from AddDevice,
              * and the notifier thread (2000 and later, gate active). */
             UAC_GATE Gate;
             PVOID Notifier;            /* its thread object, referenced */
             KEVENT NotifierReady, NotifierExited;
             LONG NotifierExit;
             LONG Surprised;            /* 30: a surprise removal came (2000 and later) */ };
static PDRIVER_DISPATCH PortClsPnp;
static EXT *Extension(PDEVICE_OBJECT fdo);
static PDEVICE_OBJECT PdoOf(PDEVICE_OBJECT fdo) { return Extension(fdo)->Pdo; }
static EXT *Extension(PDEVICE_OBJECT fdo) {
    return (EXT *)((PUCHAR)fdo->DeviceExtension + PORT_CLASS_DEVICE_EXTENSION_SIZE);
}
static UAC_GATE *GateOf(PDEVICE_OBJECT fdo) { return &Extension(fdo)->Gate; }

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
#ifdef UAC_V1
    ULONG build = UAC1_BUILD;   /* uac1's own build number (uac1wdm.rc) */
#else
    ULONG build = 29;
#endif
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

/* 27: Uac2Waits, three DWORDs of the direction last opened: the opens
 * that met a closing stream, those still closing after the wait (refused
 * busy), and the last wait in milliseconds. Written, never read. */
static void RecordWaits(PDEVICE_OBJECT fdo, ULONG waits, ULONG timeouts, ULONG ms)
{
    HANDLE key;
    ULONG counts[3] = { waits, timeouts, ms };
    UNICODE_STRING name = { 18, 20, L"Uac2Waits" };
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

/* Uac2QueueMs (docs/design/uac2.md 19.3): a REG_DWORD, or the 4-byte
 * little-endian REG_BINARY a 9x INF writes; UAC_QUEUE_NONE otherwise. */
static ULONG GetDwordRaw(PDEVICE_OBJECT fdo, PWSTR value)
{
    HANDLE key;
    UNICODE_STRING u;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 16];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    ULONG got = 0, raw = UAC_QUEUE_NONE;
    Name(&u, value);
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension(fdo)->Pdo,
            PLUGPLAY_REGKEY_DRIVER, KEY_QUERY_VALUE, &key))) {
        if (NT_SUCCESS(ZwQueryValueKey(key, &u, KeyValuePartialInformation, info, sizeof(buffer), &got)) &&
            (info->Type == REG_DWORD || info->Type == REG_BINARY) && info->DataLength == 4)
            raw = info->Data[0] | ((ULONG)info->Data[1] << 8) | ((ULONG)info->Data[2] << 16) |
                  ((ULONG)info->Data[3] << 24);
        ZwClose(key);
    }
    return raw;
}

static ULONG GetQueueRaw(PDEVICE_OBJECT fdo) { return GetDwordRaw(fdo, L"Uac2QueueMs"); }

/* Uac2Realtime (docs/design/uac2.md 19.6): read the same way, at each RUN. */
static ULONG GetRealtimeRaw(PDEVICE_OBJECT fdo) { return GetDwordRaw(fdo, L"Uac2Realtime"); }

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

/* The configuration descriptor at PnP start. With the gate active
 * (docs/design/uac2.md 29.9) through the shared owned-IRP path, with its
 * 5 s deadline and the startup latch; otherwise as before, untimed. */
static NTSTATUS ConfigDescriptor(PDEVICE_OBJECT fdo, PVOID buffer, USHORT size)
{
    EXT *ext = Extension(fdo);
    ULONG got = 0;
    if (!ext->Gate.Active) return Descriptor(ext->Lower, buffer, size);
    NTSTATUS s = UacUsbDescriptor(&ext->Gate, ext->Lower, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
                                  buffer, size, &got);
    if (NT_SUCCESS(s) && got != size) s = STATUS_DEVICE_DATA_ERROR;
    return s;
}

/* docs/design/uac2.md 29.9: the record of the failure in the driver key,
 * written by the notifier (never by the expiry hook). 14 DWORDs: Version,
 * Count, TimeLow, TimeHigh, Kind, Setup0, Setup1, Status, UrbStatus,
 * LateMs, MsSinceAltChange, AltChanges10s, Streams, Build. This step
 * records no wall time and no alternate history: those words read
 * 0xFFFFFFFF ("not recorded"). Status is the lower stack's own completion
 * status (STATUS_CANCELLED when the cancellation took it; the caller was
 * given STATUS_IO_TIMEOUT), UrbStatus the URB's; both 0, and LateMs
 * 0xFFFFFFFF, until the expired request has completed; LateMs saturates
 * at 0xFFFFFFFE. */
static void WriteFailed(PDEVICE_OBJECT fdo, ULONG count)
{
    UAC_GATE *g = &Extension(fdo)->Gate;
    ULONG v[14];
    KIRQL old;
    KeAcquireSpinLock(&g->Lock, &old);
    v[0] = 1; v[1] = count; v[2] = v[3] = 0xFFFFFFFF;
    v[4] = g->Kind; v[5] = g->Setup0; v[6] = g->Setup1;
    v[7] = g->Completed ? (ULONG)g->Status : 0;
    v[8] = g->Completed ? g->UrbStatus : 0;
    v[9] = g->Completed ? g->LateMs : 0xFFFFFFFF;
    KeReleaseSpinLock(&g->Lock, old);
    v[10] = v[11] = v[12] = 0xFFFFFFFF;
#ifdef UAC_V1
    v[13] = UAC1_BUILD;
#else
    v[13] = 29;
#endif
    PutValue(fdo, L"Uac2Failed", v, sizeof(v));
}

/* The notifier (29.9): a system thread of its own, 2000 and later, that
 * sends no USB request and takes no transport, Wave or stream mutex, so
 * it reports a failure even while every other thread is blocked in the
 * expired request. Its call to IoInvalidateDeviceState is admitted under
 * the gate lock while Life reads started; a teardown changes Life first
 * and then joins this thread, so an admitted call finishes before the
 * IRP goes on. It runs from before the start's first request (Start),
 * Life starting: it records a startup expiry and calls nothing for a
 * device that has not started. Each wake is UacNotifyStep's: the exit
 * flag read first, a pending record written before the exit. */
static VOID NotifierMain(PVOID context)
{
    PDEVICE_OBJECT fdo = (PDEVICE_OBJECT)context;
    EXT *ext = Extension(fdo);
    UAC_GATE *g = &ext->Gate;
    UAC_NOTIFY n = { 0, 0, 0 };
    ULONG count = 0;
    ext->Notifier = KeGetCurrentThread();
    ObReferenceObject(ext->Notifier);
    KeSetEvent(&ext->NotifierReady, IO_NO_INCREMENT, FALSE);
    for (;;) {
        KeClearEvent(&g->NotifyWake);
        int exitAsked = InterlockedCompareExchange(&ext->NotifierExit, 0, 0) ? 1 : 0;
        int failed = InterlockedCompareExchange(&g->Failed, 0, 0) ? 1 : 0;
        int completed = InterlockedCompareExchange(&g->Completed, 0, 0) ? 1 : 0;
        unsigned int a = UacNotifyStep(&n, exitAsked, failed, completed);
        if (a & UAC_NOTIFY_WRITE) {
            if (a & UAC_NOTIFY_FIRST) {
                ULONG prev[14];
                count = GetValue(fdo, L"Uac2Failed", prev, sizeof(prev)) && prev[0] == 1 ? prev[1] + 1 : 1;
            }
            WriteFailed(fdo, count);
        }
        if (a & UAC_NOTIFY_ADMIT) {
            KIRQL old;
            KeAcquireSpinLock(&g->Lock, &old);
            int call = UacNotifyLife(&n, (int)g->Life);
            if (call) g->Notifying = 1;   /* the admission point */
            KeReleaseSpinLock(&g->Lock, old);
            if (call) {
                IoInvalidateDeviceState(ext->Pdo);
                KeAcquireSpinLock(&g->Lock, &old);
                g->Notifying = 0;
                KeReleaseSpinLock(&g->Lock, old);
            }
            /* Starting: the start sets the wake event when it has started. */
        }
        if (a & UAC_NOTIFY_EXIT) break;
        KeWaitForSingleObject(&g->NotifyWake, Executive, KernelMode, FALSE, NULL);
    }
    KeSetEvent(&ext->NotifierExited, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Created in the PnP start, in its system thread (no executive work item),
 * the handle closed there; the thread references its own object before it
 * signals ready, and the join waits for that object (2000 and later). */
static NTSTATUS StartNotifier(PDEVICE_OBJECT fdo)
{
    EXT *ext = Extension(fdo);
    HANDLE thread;
    KeClearEvent(&ext->NotifierReady);
    KeClearEvent(&ext->NotifierExited);
    InterlockedExchange(&ext->NotifierExit, 0);
    NTSTATUS s = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, NotifierMain, fdo);
    if (!NT_SUCCESS(s)) return s;
    ZwClose(thread);
    KeWaitForSingleObject(&ext->NotifierReady, Executive, KernelMode, FALSE, NULL);
    return STATUS_SUCCESS;
}

static void StopNotifier(PDEVICE_OBJECT fdo)
{
    EXT *ext = Extension(fdo);
    PVOID thread = ext->Notifier;
    if (!thread) return;
    InterlockedExchange(&ext->NotifierExit, 1);
    KeSetEvent(&ext->Gate.NotifyWake, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(&ext->NotifierExited, Executive, KernelMode, FALSE, NULL);
    KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(thread);
    ext->Notifier = NULL;
}

/* The INF's name less UacName's UAC_NAME_MARK (its Device string is this
 * and the mark; infchk.ps1 checks they agree): what a device without a
 * usable product string is named, "USB Audio 2.0 (wdm-free)", a later
 * pair " N" after it. */
#ifdef UAC_V1
static const char NameFallback[] = "USB Audio 1.0";
#else
static const char NameFallback[] = "USB Audio 2.0";
#endif

/* Windows Vista and later (WDM 6.00) name the audio endpoints after the
 * devnode's FriendlyName, read when an endpoint arrives (section 21.2):
 * the name is written there too, the instance key being the parent of
 * the devnode's Device Parameters key (PLUGPLAY_REGKEY_DEVICE). Earlier
 * systems take the interfaces' FriendlyName and are left alone here.
 * Returns the status recorded as Uac2NameDevnode (STATUS_NOT_SUPPORTED
 * before Vista). */
static NTSTATUS NameDevnode(PDEVICE_OBJECT fdo, const UAC_WORD *name, ULONG n)
{
    EXT *ext = Extension(fdo);
    HANDLE device, instance;
    /* KEY_NAME_INFORMATION (ntddk.h; this build's wdm.h has only the class). */
    struct KeyName { ULONG NameLength; WCHAR Name[1]; };
    UCHAR buffer[sizeof(KeyName) + 512 * sizeof(WCHAR)];
    KeyName *info = (KeyName *)buffer;
    static const WCHAR tail[] = L"\\Device Parameters";
    const ULONG tailChars = sizeof(tail) / sizeof(WCHAR) - 1;
    ULONG got = 0, chars, i;
    NTSTATUS s;
    if (!IoIsWdmVersionAvailable(6, 0)) return STATUS_NOT_SUPPORTED;
    s = IoOpenDeviceRegistryKey(ext->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &device);
    if (!NT_SUCCESS(s)) return s;
    s = ZwQueryKey(device, (KEY_INFORMATION_CLASS)3 /* KeyNameInformation, WDK 7.1 wdm.h */, info, sizeof(buffer) - sizeof(WCHAR), &got);
    ZwClose(device);
    if (!NT_SUCCESS(s)) return s;
    chars = info->NameLength / sizeof(WCHAR);
    if (info->NameLength > sizeof(buffer) - sizeof(KeyName) || chars <= tailChars)
        return STATUS_OBJECT_NAME_INVALID;
    for (i = 0; i < tailChars; i++) {
        WCHAR a = info->Name[chars - tailChars + i], b = tail[i];
        if (a >= L'a' && a <= L'z') a = (WCHAR)(a - L'a' + L'A');
        if (b >= L'a' && b <= L'z') b = (WCHAR)(b - L'a' + L'A');
        if (a != b) return STATUS_OBJECT_NAME_INVALID;
    }
    UNICODE_STRING path, value;
    OBJECT_ATTRIBUTES oa;
    path.Buffer = info->Name;
    path.Length = path.MaximumLength = (USHORT)((chars - tailChars) * sizeof(WCHAR));
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    s = ZwOpenKey(&instance, KEY_SET_VALUE, &oa);
    if (!NT_SUCCESS(s)) return s;
    Name(&value, L"FriendlyName");
    s = ZwSetValueKey(instance, &value, 0, REG_SZ, (PVOID)name, (n + 1) * sizeof(WCHAR));
    ZwClose(instance);
    return s;
}

/* The product string (section 21): the device descriptor's iProduct, in
 * 0x0409 when string descriptor 0 lists it, else its first language (0x0409
 * when that list cannot be read); each read at most 5 s (UacUsbDescriptor).
 * Without a usable one, the INF's name, so a name written at an earlier
 * start does not stay. Uac2NameStatus records the outcome: 0 named, 1 no
 * iProduct, 2 device descriptor unread, 3 string unread, 4 no printable
 * name; Uac2Name the name of pair 0. Nothing here fails a start. */
static void ReadName(PDEVICE_OBJECT fdo)
{
    EXT *ext = Extension(fdo);
    UCHAR device[18], list[64];
    ULONG got = 0, status = 0, language = 0x0409;
    UAC_WORD name[UAC_NAME_MAX + 1];
    ext->NameLength = 0;
    ext->MakerLength = 0;
    if (!NT_SUCCESS(UacUsbDescriptor(&ext->Gate, ext->Lower, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, device, sizeof(device), &got)) ||
        got != sizeof(device) || device[0] != 18 || device[1] != 1) status = 2;
    else if (!device[15]) status = 1;
    else {
        if (NT_SUCCESS(UacUsbDescriptor(&ext->Gate, ext->Lower, USB_STRING_DESCRIPTOR_TYPE, 0, 0, list, sizeof(list), &got))) {
            unsigned int l = UacNameLanguage(list, got);
            if (l) language = l;
        }
        if (!NT_SUCCESS(UacUsbDescriptor(&ext->Gate, ext->Lower, USB_STRING_DESCRIPTOR_TYPE, device[15], (USHORT)language,
                                         ext->Name, sizeof(ext->Name), &got))) status = 3;
        else if (!UacName(ext->Name, got, 0, name, UAC_NAME_MAX)) status = 4;
        else {
            ext->NameLength = got;
            /* The maker's name, which a product string too long for the
             * name loses first (section 21.7); none if it cannot be read. */
            if (device[14] && NT_SUCCESS(UacUsbDescriptor(&ext->Gate, ext->Lower, USB_STRING_DESCRIPTOR_TYPE, device[14], (USHORT)language,
                                                          ext->Maker, sizeof(ext->Maker), &got)))
                ext->MakerLength = got;
        }
    }
    if (!ext->NameLength) {
        /* The INF's name as a string descriptor, for UacName. */
        ULONG i, n = sizeof(NameFallback) - 1;
        ext->Name[0] = (UCHAR)(2 + 2 * n); ext->Name[1] = 3;
        for (i = 0; i < n; i++) { ext->Name[2 + 2 * i] = (UCHAR)NameFallback[i]; ext->Name[3 + 2 * i] = 0; }
        ext->NameLength = 2 + 2 * n;
    }
    HANDLE key;
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(ext->Pdo, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key))) {
        UNICODE_STRING u;
        Name(&u, L"Uac2NameStatus");
        ZwSetValueKey(key, &u, 0, REG_DWORD, &status, sizeof(status));
        Name(&u, L"Uac2Name");
        ULONG n = UacNameFrom(ext->Name, ext->NameLength, ext->Maker, ext->MakerLength, 0, name, UAC_NAME_MAX);
        ZwSetValueKey(key, &u, 0, REG_SZ, name, (n + 1) * sizeof(WCHAR));
        NTSTATUS devnode = n ? NameDevnode(fdo, name, n) : STATUS_UNSUCCESSFUL;
        Name(&u, L"Uac2NameDevnode");
        ZwSetValueKey(key, &u, 0, REG_DWORD, &devnode, sizeof(devnode));
        ZwClose(key);
    }
}

/* Before PcRegisterSubdevice: the pair's KSCATEGORY_AUDIO interfaces
 * (Wave and Topology reference strings, as the INF's AddInterface lines)
 * get the device's name (ReadName's, the INF's when there is none) as
 * FriendlyName, where the INF wrote its own; the registration is the one
 * PortCls makes next (IoRegisterDeviceInterface returns the same link). */
static void NameInterfaces(PDEVICE_OBJECT fdo, ULONG k, PWSTR wave, PWSTR topo)
{
    EXT *ext = Extension(fdo);
    UAC_WORD name[UAC_NAME_MAX + 1];
    ULONG n;
    PWSTR refs[2];
    if (!ext->NameLength) return;
    n = UacNameFrom(ext->Name, ext->NameLength, ext->Maker, ext->MakerLength, k, name, UAC_NAME_MAX);
    if (!n) return;
    refs[0] = wave; refs[1] = topo;
    for (ULONG i = 0; i < 2; i++) {
        UNICODE_STRING ref, link, value;
        HANDLE key;
        Name(&ref, refs[i]);
        link.Buffer = NULL;
        if (!NT_SUCCESS(IoRegisterDeviceInterface(ext->Pdo, &KSCATEGORY_AUDIO, &ref, &link))) continue;
        if (NT_SUCCESS(IoOpenDeviceInterfaceRegistryKey(&link, KEY_SET_VALUE, &key))) {
            Name(&value, L"FriendlyName");
            ZwSetValueKey(key, &value, 0, REG_SZ, name, (n + 1) * sizeof(WCHAR));
            ZwClose(key);
        }
        if (link.Buffer) ExFreePool(link.Buffer);
    }
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
    s = ConfigDescriptor(fdo, &header, sizeof(header));
    Record(fdo, 2, s);
    if (!NT_SUCCESS(s)) return s;
    if (header.bLength != sizeof(header) || header.bDescriptorType != 2 ||
        header.wTotalLength < sizeof(header)) return STATUS_DEVICE_DATA_ERROR;
    PUCHAR bytes = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, header.wTotalLength, 'd2aU');
    UAC_DESC *parsed = (UAC_DESC *)ExAllocatePoolWithTag(NonPagedPool, sizeof(UAC_DESC), 'p2aU');
    if (!bytes || !parsed) { s = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    s = ConfigDescriptor(fdo, bytes, header.wTotalLength);
    Record(fdo, 3, s);
#ifdef UAC_V1
    /* USB Audio 1.0 (section 26): the function the PDO names (its MI_
     * interface, else the whole device's first UAC 1.0 function) parsed for
     * the bus speed (the bus's answer; without one, the speed the
     * descriptors alone fit; else Full Speed, the only one 98 SE's own
     * stack has), then put in the shared model. A function that is not an
     * AudioControl one, or has no alternate this driver runs (MIDI only,
     * say), comes back with no alternates: Start makes it a null function. */
    if (NT_SUCCESS(s)) {
        unsigned int whole = 256, view;
        int rc = UAC1_OK, guessed = 0;
        BOOLEAN high = FALSE;
        UAC1_DESC *p1 = (UAC1_DESC *)ExAllocatePoolWithTag(NonPagedPool, sizeof(UAC1_DESC), 'q1aU');
        if (!p1) { s = STATUS_INSUFFICIENT_RESOURCES; goto done; }
        RtlZeroMemory(parsed, sizeof(*parsed));
        if (first < 0) {
            rc = Uac1WholeFirst(bytes, header.wTotalLength, &whole);
            first = (int)whole;
        }
        view = rc ? UAC1_VIEW_NONE : (unsigned int)Uac1View(bytes, header.wTotalLength, first);
        if (view == UAC1_VIEW_CONTROL) {
            if (NT_SUCCESS(UacUsbBusSpeed(ext->Lower, &high))) guessed = high ? 2 : 1;
            else {
                int hs = 0;
                guessed = Uac1GuessSpeed(bytes, header.wTotalLength, first, p1, &hs) ? (hs ? 2 : 1) : 1;
            }
            rc = Uac1Parse(bytes, header.wTotalLength, first, guessed == 2, p1);
            if (rc == UAC1_OK || rc == UAC1_NO_AUDIO) {
                parsed->Bytes = bytes; parsed->Length = header.wTotalLength;
                parsed->Control = (UAC_BYTE)first;
                if (rc == UAC1_OK) rc = Uac1Model(p1, guessed == 2, parsed);
                parsed->Speed = (unsigned int)guessed;
            }
        }
        RecordDescriptors(fdo, bytes, header.wTotalLength, first, view | ((unsigned int)guessed << 8),
                          parsed->AltCount | (parsed->Skipped << 16), rc);
        DbgPrint("uac1wdm: descriptor result %d, view %u, PCM alternates %u, skipped %u\n",
                 rc, view, parsed->AltCount, parsed->Skipped);
        ExFreePool(p1);
        /* Only a malformed AudioControl function fails the start. */
        if (rc == UAC1_BAD_DESCRIPTOR || rc == UAC1_TOO_MANY_ALTS) s = STATUS_DEVICE_CONFIGURATION_ERROR;
        else if (!parsed->Bytes) { parsed->Bytes = bytes; parsed->Length = header.wTotalLength; parsed->AltCount = 0; }
        Record(fdo, 4, s);
    }
#else
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
#endif
done:
    if (NT_SUCCESS(s)) { *model = parsed; parsed = NULL; bytes = NULL; }
    if (parsed) ExFreePool(parsed);
    if (bytes) ExFreePool(bytes);
    return s;
}

/* Every pair's PnP stop: the later pairs first, pair 0 (the transport's
 * owner) last; permanent also drops the extension's references. */
static void ShutdownPairs(EXT *ext, BOOLEAN permanent, BOOLEAN release)
{
    for (ULONG k = UAC_PAIRS - 1; k-- > 0; ) {
        Wave *v = ext->Pair[k];
        if (!v) continue;
        v->Shutdown(permanent);
        if (release) { ext->Pair[k] = NULL; v->Release(); }
    }
    if (ext->Miniport) {
        ext->Miniport->Shutdown(permanent);
        if (release) { ext->Miniport->Release(); ext->Miniport = NULL; }
    }
}

/* One endpoint pair: its wave and topology ports under the reference
 * strings of section 15.3 item 3, and their physical connections. */
static NTSTATUS StartPair(PDEVICE_OBJECT fdo, PIRP irp, PRESOURCELIST resources, Wave *w,
                          ULONG k)
{
    static const PWSTR waveName[UAC_PAIRS] = { L"Wave", L"Wave2", L"Wave3", L"Wave4" };
    static const PWSTR topoName[UAC_PAIRS] = { L"Topology", L"Topology2", L"Topology3", L"Topology4" };
    PPORT wave = NULL, topo = NULL;
    Topology *t = NULL;
    const UAC_GRAPH *g = w->Graph();
    NTSTATUS s;
    void *tm = ExAllocatePoolWithTag(NonPagedPool, sizeof(Topology), 't2aU');
    if (!tm) return STATUS_INSUFFICIENT_RESOURCES;
    t = new(tm) Topology(g, w);
    s = PcNewPort(&wave, CLSID_PortWaveCyclic); Record(fdo, 5, s);
    if (NT_SUCCESS(s)) { s = wave->Init(fdo, irp, (PMINIPORTWAVECYCLIC)w, NULL, resources); Record(fdo, 6, s); }
    if (NT_SUCCESS(s)) { s = PcNewPort(&topo, CLSID_PortTopology); Record(fdo, 7, s); }
    if (NT_SUCCESS(s)) { s = topo->Init(fdo, irp, t, NULL, resources); Record(fdo, 8, s); }
    if (NT_SUCCESS(s)) NameInterfaces(fdo, k, waveName[k], topoName[k]);
    if (NT_SUCCESS(s)) { s = PcRegisterSubdevice(fdo, waveName[k], wave); Record(fdo, 9, s); }
    if (NT_SUCCESS(s)) { s = PcRegisterSubdevice(fdo, topoName[k], topo); Record(fdo, 10, s); }
    if (NT_SUCCESS(s) && g->RenderBridge >= 0) {
        s = PcRegisterPhysicalConnection(fdo, wave, g->RenderBridge, topo, g->TopoRenderIn); Record(fdo, 11, s);
    }
    if (NT_SUCCESS(s) && g->CaptureBridge >= 0) {
        s = PcRegisterPhysicalConnection(fdo, topo, g->TopoCaptureOut, wave, g->CaptureBridge); Record(fdo, 14, s);
    }
    t->Release();
    if (wave) wave->Release(); if (topo) topo->Release();
    return s;
}

/* docs/design/uac2.md 29.9: a request that expired during the start fails
 * the start (the startup latch), whatever a step that tolerates failures
 * made of it. Never true while the gate is inactive. */
static BOOLEAN StartLatched(EXT *ext) { return UacGateFailed(&ext->Gate) ? TRUE : FALSE; }
/* A step's status as the start takes it (UacStartVerdict): the latch wins
 * over a success, a tolerated failure and any other failure alike. */
static NTSTATUS StartStep(EXT *ext, NTSTATUS s)
{
    return (NTSTATUS)UacStartVerdict((long)s, StartLatched(ext) ? 1 : 0);
}

/* The start's steps, from the configuration descriptor on; every path
 * returns to Start, which ends the start. The latch is tested after each
 * step that may swallow a failure (29.9), before any fallback or
 * success branch is taken. */
static NTSTATUS StartSteps(PDEVICE_OBJECT fdo, PIRP irp, PRESOURCELIST resources)
{
    EXT *ext = Extension(fdo);
    UAC_DESC *model = NULL;
    NTSTATUS s = StartStep(ext, ParseDevice(fdo, &model));
    if (!NT_SUCCESS(s)) {
        if (model) { ExFreePool((PVOID)model->Bytes); ExFreePool(model); }
        return s;
    }
#ifdef UAC_V1
    /* A null function (section 26.6, G3): nothing this driver runs (a
     * MIDI-only function, say, or formats it does not carry). It starts
     * with no audio device, as Windows' driver leaves such a function on
     * 98 SE, rather than failing (Code 10); stage 0x40 records it. */
    if (!model->AltCount) {
        Record(fdo, 0x40, STATUS_NOT_SUPPORTED);
        ExFreePool((PVOID)model->Bytes); ExFreePool(model);
        return STATUS_SUCCESS;
    }
#endif
    void *wm = ExAllocatePoolWithTag(NonPagedPool, sizeof(Wave), 'w2aU');
    if (!wm) {
        ExFreePool((PVOID)model->Bytes); ExFreePool(model);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Wave *w = new(wm) Wave(fdo, ext->Lower, model);
    ext->Miniport = w; /* the adapter/PnP reference: the construction's */
    ReadName(fdo);
    /* 29.9: ReadName never fails a start (its fallback name); a latched
     * expiry still does. Prepare's own tolerated failures (Learn skipping
     * a format whose rates failed, the level setup) and uac1's null
     * function below are decided after the latch, never before it. */
    s = StartStep(ext, STATUS_SUCCESS);
    if (NT_SUCCESS(s)) s = StartStep(ext, w->Prepare());
    Record(fdo, 13, s);
#ifdef UAC_V1
    /* No format either direction can run: a null function too. */
    if (UacStartNullFunction((long)s)) {
        ShutdownPairs(ext, TRUE, TRUE);
        Record(fdo, 0x41, s);
        return STATUS_SUCCESS;
    }
#endif
    if (NT_SUCCESS(s)) s = StartStep(ext, StartPair(fdo, irp, resources, w, 0));
    if (NT_SUCCESS(s)) {
        s = PcRegisterAdapterPowerManagement((PMINIPORTWAVECYCLIC)w, fdo); Record(fdo, 12, s);
        s = StartStep(ext, s);
    }
    /* The later pairs (section 15.3): a pair that cannot be brought up is
     * left out; pair 0 stands on its own. A latched expiry is no pair left
     * out: it ends the start before the next pair is built. */
    ULONG pairs = NT_SUCCESS(s) ? w->Pairs() : 1;
    for (ULONG k = 1; k < pairs && k < UAC_PAIRS && NT_SUCCESS(s); k++) {
        void *pm = ExAllocatePoolWithTag(NonPagedPool, sizeof(Wave), 'w2aU');
        if (!pm) { s = STATUS_INSUFFICIENT_RESOURCES; break; }
        Wave *v = new(pm) Wave(fdo, ext->Lower, NULL, k, w);
        NTSTATUS e = v->Prepare();
        Record(fdo, 0x100 | (k << 4) | 1, e);
        if (StartLatched(ext)) {
            v->Shutdown(TRUE);
            v->Release();
            s = STATUS_IO_TIMEOUT;
            break;
        }
        if (!NT_SUCCESS(e)) {
            /* Nothing of this pair is registered yet (no usable format,
             * say): leave it and the pairs after it out. */
            v->Shutdown(TRUE);
            v->Release();
            break;
        }
        e = StartPair(fdo, irp, resources, v, k);
        Record(fdo, 0x100 | (k << 4) | 2, e);
        e = StartStep(ext, e);
        if (!NT_SUCCESS(e)) {
            /* A pair half registered cannot be taken back on every
             * target: the start fails as a whole. */
            v->Shutdown(TRUE);
            v->Release();
            s = e;
            break;
        }
        ext->Pair[k - 1] = v;
        w->AddPeer(v);
    }
    return s;
}

static NTSTATUS Start(PDEVICE_OBJECT fdo, PIRP irp, PRESOURCELIST resources)
{
    EXT *ext = Extension(fdo);
    ShutdownPairs(ext, TRUE, TRUE);
    StopNotifier(fdo);
    /* 29.9: the gate for this start. Active only on 2000 and later (WDM
     * 1.10 and above) with Uac2FailOnTimeout set to 1; in this step the
     * setting is off unless written (no INF line yet), so a device behaves
     * as before unless a test sets it. 98 SE and ME never read it. */
    UacGateReset(&ext->Gate, IoIsWdmVersionAvailable(1, 0x10) &&
                             GetDwordRaw(fdo, L"Uac2FailOnTimeout") == 1);
    /* 29.9: the notifier before the start's first request (ParseDevice's
     * configuration descriptor), Life starting: an expiry anywhere in the
     * start is recorded by a thread that sends nothing, and no device-state
     * call is made for a device that has not started. */
    NTSTATUS s = ext->Gate.Active ? StartNotifier(fdo) : STATUS_SUCCESS;
    if (NT_SUCCESS(s)) s = StartSteps(fdo, irp, resources);
    /* The last test of the latch, over every outcome; only then is the
     * device started for the notification's admission. Every failed start
     * joins the notifier, which writes a pending record before it leaves. */
    s = StartStep(ext, s);
    if (!NT_SUCCESS(s)) {
        ShutdownPairs(ext, TRUE, TRUE);
        StopNotifier(fdo);
    } else if (ext->Gate.Active) {
        UacGateSetLife(&ext->Gate, UAC_LIFE_STARTED);
        KeSetEvent(&ext->Gate.NotifyWake, IO_NO_INCREMENT, FALSE);   /* a failure in between */
    }
    DbgPrint("uac2wdm: start %08X\n", s);
    return s;
}

static NTSTATUS Add(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo)
{
    /* 30: the device's entry first, so a device is never added without it. */
    UAC_FILES *files = FilesAlloc();
    if (!files) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS s = PcAddAdapterDevice(driver, pdo, Start, 2 * UAC_PAIRS,
                                    PORT_CLASS_DEVICE_EXTENSION_SIZE + sizeof(EXT));
    if (!NT_SUCCESS(s)) { ExFreePool(files); return s; }
    /* Old-target path from the design: no XP-only IoGetLowerDeviceObject.
     * AddDevice runs before PnP starts this stack. */
    PDEVICE_OBJECT lower = pdo, fdo = pdo->AttachedDevice;
    while (fdo && fdo->DriverObject != driver) { lower = fdo; fdo = fdo->AttachedDevice; }
    if (!fdo) { ExFreePool(files); return STATUS_NO_SUCH_DEVICE; }
    Extension(fdo)->Pdo = pdo;
    Extension(fdo)->Lower = lower;
    Extension(fdo)->Miniport = NULL;
    RtlZeroMemory(Extension(fdo)->Pair, sizeof(Extension(fdo)->Pair));
    /* docs/design/uac2.md 29.5.5: the gate and its lock exist from here. */
    UacGateInit(&Extension(fdo)->Gate);
    Extension(fdo)->Notifier = NULL;
    Extension(fdo)->NotifierExit = 0;
    Extension(fdo)->Surprised = 0;
    FilesLink(files, fdo);
    KeInitializeEvent(&Extension(fdo)->NotifierReady, NotificationEvent, FALSE);
    KeInitializeEvent(&Extension(fdo)->NotifierExited, NotificationEvent, FALSE);
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
        if (stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL) ext->Surprised = 1;
        /* 29.9: Life first, so no notification is admitted from here on;
         * the notifier's join then waits for one already admitted. */
        if (ext->Gate.Active)
            UacGateSetLife(&ext->Gate, stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL ?
                                       UAC_LIFE_SURPRISE : UAC_LIFE_TEARDOWN);
        ShutdownPairs(ext, stack->MinorFunction != IRP_MN_STOP_DEVICE,
                      stack->MinorFunction == IRP_MN_REMOVE_DEVICE);
        StopNotifier(fdo);
    } else if (stack->MinorFunction == IRP_MN_QUERY_PNP_DEVICE_STATE &&
               UacGateFailed(&Extension(fdo)->Gate)) {
        /* 29.9: the device has failed: PNP_DEVICE_FAILED added to the flags
         * already there, then the IRP goes on to PortCls as before. */
        irp->IoStatus.Information |= PNP_DEVICE_FAILED;
        irp->IoStatus.Status = STATUS_SUCCESS;
    }
    /* docs/design/uac2.md 30: a remove with no surprise removal before it
     * (98 SE, ME: WDM below 1.10) marks this thread for RemoveHold; the
     * entry goes once PortCls's remove has returned. */
    BOOLEAN remove = stack->MinorFunction == IRP_MN_REMOVE_DEVICE;
    BOOLEAN old = !IoIsWdmVersionAvailable(1, 0x10), surprised = Extension(fdo)->Surprised != 0;
    if (remove && old && !surprised) FilesRemover(fdo, (PVOID)PsGetCurrentThread(), FALSE);
    /* 30.6: a device PortCls left powered down is asked back to D0 first
     * (PowerChangeState then records D0, and the hold's rule applies as for
     * any device in D0); if PortCls does not bring it back, no hold. */
    if (remove && UacPlanRemoveWake(old, surprised, FilesDown(fdo)))
        PcRequestNewPowerState(fdo, PowerDeviceD0);
    NTSTATUS ps = PortClsPnp(fdo, irp);
    if (remove) FilesRemover(fdo, NULL, TRUE);
    return ps;
}

/* docs/design/uac2.md 30: every create and close through PortCls's
 * dispatch, counted as in flight from entry to return and its end stamped
 * (the major is read before the call: the IRP may be completed when it
 * returns). */
static PDRIVER_DISPATCH PortClsDispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];
static NTSTATUS DispatchFile(PDEVICE_OBJECT fdo, PIRP irp)
{
    ULONG major = IoGetCurrentIrpStackLocation(irp)->MajorFunction;
    FilesNote(fdo, 0, 0, 1);            /* in flight from entry */
    NTSTATUS s = PortClsDispatch[major](fdo, irp);
    /* at return, whatever the status: a failed or pending create counts as
     * an open only when it succeeded here; its end is stamped either way */
    LONG opens = major == IRP_MJ_CREATE ? (NT_SUCCESS(s) && s != STATUS_PENDING ? 1 : 0) : -1;
    FilesNote(fdo, opens, 0, -1);
    return s;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path)
{
    KeInitializeSpinLock(&FilesLock);
    InitializeListHead(&FilesList);
    NTSTATUS status = PcInitializeAdapterDriver(driver, path, Add);
    if (NT_SUCCESS(status)) {
        PortClsPnp = driver->MajorFunction[IRP_MJ_PNP];
        driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
        static const UCHAR majors[] = { IRP_MJ_CREATE, IRP_MJ_CLOSE };
        for (ULONG i = 0; i < sizeof(majors); i++) {
            PortClsDispatch[majors[i]] = driver->MajorFunction[majors[i]];
            driver->MajorFunction[majors[i]] = DispatchFile;
        }
    }
    return status;
}
