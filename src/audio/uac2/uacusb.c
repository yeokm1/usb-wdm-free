/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original bounded UAC 2.0 isochronous transport from USB Audio 2.0
 * control facts (sections 4.7.2, 5.2), USB 2.0 chapters 5 and 9 and the
 * published WDM/USB headers. No kit sample, vendor implementation or
 * binary code was used.
 */
#include <wdm.h>
#pragma warning(push)
#pragma warning(disable:4200)
#if defined(_AMD64_)
#include <usb.h>
#else
#include <usbdi.h>
#endif
#include <usbioctl.h>
#pragma warning(pop)
#include "uacusb.h"
#include "uacrate.h"
#include "uacwatch.h"

#define DATA_SLOTS 4
#define FEEDBACK_SLOTS 2
#define SLOTS (DATA_SLOTS + FEEDBACK_SLOTS)
#define FEEDBACK_PACKETS 8
#define CLAIMS 16
#define WALK_VISITS 64
#define TAG 'b2aU'

typedef struct USB_SLOT {
    struct UAC_USB *Owner;
    PIRP Irp;
    struct _URB_ISOCH_TRANSFER *Urb;
    PUCHAR Buffer;
    ULONG Packets, Bytes, Sequence;
    LONG Phase; /* 0 idle,1 lower-owned,2 completed and caller-owned */
    BOOLEAN Feedback, CancelIssued, Used;
    NTSTATUS Status;
} USB_SLOT;

/* A clock entity a running stream depends on: a source at a rate, or a
 * selector at an input. Streams share an entity only at the same value. */
typedef struct CLAIM { UAC_BYTE Id; ULONG Value, Users; } CLAIM;

struct UAC_DEV {
    PDEVICE_OBJECT Lower;
    PUCHAR Config;
    UAC_DESC Desc;          /* Bytes points at Config */
    UAC_BYTE IfNumber[32], IfPipes[32]; /* alternate 0 of every interface */
    ULONG IfCount, Streams;
    USBD_CONFIGURATION_HANDLE Configuration;
    KMUTEX Lock;            /* control requests, clock and configuration state */
    BOOLEAN HighSpeed;
    CLAIM Claim[CLAIMS];
};

struct UAC_USB {
    UAC_DEV *Dev;
    UAC_ALT Alt;
    ULONG Capture, Rate, FrameBytes, Capacity, FeedbackBytes, SlotBytes, DataPackets;
    UAC_CLOCK_PATH Path;    /* the claimed clock path while ClockHeld */
    BOOLEAN Feedback, ClockHeld;
    USBD_PIPE_HANDLE DataPipe, FeedbackPipe;
    UAC_USB_FILL Fill;
    UAC_USB_CAPTURE Captured;
    UAC_USB_PROGRESS Progress;
    PVOID CallbackContext, Thread;
    KSPIN_LOCK Lock;
    KMUTEX Operation;
    KEVENT Wake, Ready, Exited; /* Exited: the worker's last act */
    BOOLEAN Stop, ActiveAlt, WaitThread; /* WaitThread: WDM 1.10+ */
    ULONG NextSequence, CompletedSequence;
    UAC_WATCH_TICK LastFeedbackPacket, LastFeedbackValueAt;
    UAC_RATE Rate_;
    UAC_USB_STATS Stats;
    USB_SLOT Slot[SLOTS];
};

static NTSTATUS SyncDone(PDEVICE_OBJECT device, PIRP irp, PVOID context)
{
    UNREFERENCED_PARAMETER(device); UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* The timeout requests cancellation. Only observed completion permits free.
 * A broken lower stack may make this wait unbounded; never free its IRP,
 * URB, buffer, event or device owner just to return after a deadline. */
static NTSTATUS CallAndDrain(PDEVICE_OBJECT lower, PIRP irp, PKEVENT event)
{
    LARGE_INTEGER timeout;
    NTSTATUS wait, status;
    IoSetCompletionRoutine(irp, SyncDone, event, TRUE, TRUE, TRUE);
    (void)IoCallDriver(lower, irp);
    timeout.QuadPart = -50000000;
    wait = KeWaitForSingleObject(event, Executive, KernelMode, FALSE, &timeout);
    if (wait == STATUS_TIMEOUT) {
        (void)IoCancelIrp(irp);
        KeWaitForSingleObject(event, Executive, KernelMode, FALSE, NULL);
        status = irp->IoStatus.Status;
        if (status == STATUS_CANCELLED) status = STATUS_IO_TIMEOUT;
    } else status = irp->IoStatus.Status;
    IoFreeIrp(irp);
    return status;
}

static NTSTATUS UsbRequest(PDEVICE_OBJECT lower, PURB urb)
{
    PIRP irp = IoAllocateIrp(lower->StackSize, FALSE);
    PIO_STACK_LOCATION stack;
    KEVENT event;
    NTSTATUS status;
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    stack = IoGetNextIrpStackLocation(irp);
    RtlZeroMemory(stack, sizeof(*stack));
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = urb;
    status = CallAndDrain(lower, irp, &event);
    if (NT_SUCCESS(status) && !USBD_SUCCESS(urb->UrbHeader.Status))
        status = STATUS_DEVICE_DATA_ERROR;
    return status;
}

/* ABI field layout/GUID facts: WDK7.1 usbbusif.h USB_BUS_INTERFACE_USBDI_V1.
 * Four preceding callbacks are opaque because this module never calls them.
 * The 2000 DDK has no V1 declaration. This is runtime-probed; neither
 * bcdUSB nor endpoint packet size proves actual connection speed. */
typedef struct USB_SPEED_INTERFACE {
    USHORT Size, Version;
    PVOID BusContext;
    PINTERFACE_REFERENCE Reference;
    PINTERFACE_DEREFERENCE Dereference;
    PVOID OpaqueCallbacks[4];
    BOOLEAN (__stdcall *IsHighSpeed)(PVOID);
} USB_SPEED_INTERFACE;

/* STATUS_SUCCESS with *high set, or the failure of the query. */
static NTSTATUS BusSpeed(PDEVICE_OBJECT lower, BOOLEAN *high)
{
    static const GUID id = { 0xb1a96a13,0x3de0,0x4574,{0x9b,1,0xc0,0x8f,0xea,0xb3,0x18,0xd6} };
    USB_SPEED_INTERFACE bus;
    PIRP irp;
    PIO_STACK_LOCATION stack;
    KEVENT event;
    NTSTATUS status;
    RtlZeroMemory(&bus, sizeof(bus));
    irp = IoAllocateIrp(lower->StackSize, FALSE);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    stack = IoGetNextIrpStackLocation(irp);
    RtlZeroMemory(stack, sizeof(*stack));
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    stack->Parameters.QueryInterface.InterfaceType = (LPGUID)&id;
    stack->Parameters.QueryInterface.Size = sizeof(bus);
    stack->Parameters.QueryInterface.Version = 1;
    stack->Parameters.QueryInterface.Interface = (PINTERFACE)&bus;
    stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;
    status = CallAndDrain(lower, irp, &event);
    if (!NT_SUCCESS(status)) return status;
    status = STATUS_NOT_SUPPORTED;
    if (bus.Size >= sizeof(bus) && bus.Version >= 1 && bus.IsHighSpeed && bus.Dereference) {
        *high = bus.IsHighSpeed(bus.BusContext) ? TRUE : FALSE;
        status = STATUS_SUCCESS;
    }
    if (bus.Dereference) bus.Dereference(bus.BusContext);
    return status;
}

static NTSTATUS EntityRequest(UAC_DEV *d, UCHAR entity, int input, UCHAR request,
                             UCHAR selector, UCHAR channel, PVOID bytes, ULONG *length)
{
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST urb;
    NTSTATUS status;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_CLASS_INTERFACE;
    urb.TransferFlags = input ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : 0;
    urb.TransferBuffer = bytes; urb.TransferBufferLength = *length;
    urb.Request = request;
    urb.Value = ((USHORT)selector << 8) | channel;
    urb.Index = ((USHORT)entity << 8) | d->Desc.Control;
    status = UsbRequest(d->Lower, (PURB)&urb);
    *length = urb.TransferBufferLength;
    return status;
}

static ULONG Word32(const UCHAR *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static const UCHAR *Entity(UAC_DEV *d, ULONG id)
{
    ULONG off = id < 256 ? d->Desc.Entity[id] : 0;
    return off ? d->Desc.Bytes + off : NULL;
}

/* Up to 32 frequency subranges (UAC2 5.2.1, layout 3) of a clock source. */
#define RANGES 32
typedef struct RANGE3 { ULONG Min, Max, Res; } RANGE3;
static NTSTATUS SourceRanges(UAC_DEV *d, UCHAR id, RANGE3 *r, ULONG *count)
{
    UCHAR countBytes[2], range[2 + RANGES * 12];
    ULONG size, n, i, previousMaximum = 0;
    NTSTATUS status;
    *count = 0;
    size = sizeof(countBytes);
    status = EntityRequest(d, id, 1, 2, 1, 0, countBytes, &size); /* RANGE frequency */
    if (!NT_SUCCESS(status) || size != sizeof(countBytes)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    n = countBytes[0] | ((ULONG)countBytes[1] << 8);
    if (!n || n > RANGES) return STATUS_NOT_SUPPORTED;
    size = 2 + 12 * n;
    status = EntityRequest(d, id, 1, 2, 1, 0, range, &size);
    if (!NT_SUCCESS(status) || size != 2 + 12 * n ||
        range[0] != countBytes[0] || range[1] != countBytes[1])
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (i = 0; i < n; i++) {
        r[i].Min = Word32(range + 2 + i * 12);
        r[i].Max = Word32(range + 6 + i * 12);
        r[i].Res = Word32(range + 10 + i * 12);
        if (!r[i].Min || r[i].Max < r[i].Min ||
            (r[i].Min == r[i].Max ? r[i].Res != 0 : r[i].Res == 0) ||
            (i && r[i].Min <= previousMaximum)) return STATUS_DEVICE_CONFIGURATION_ERROR;
        previousMaximum = r[i].Max;
    }
    *count = n;
    return STATUS_SUCCESS;
}

static BOOLEAN InRanges(const RANGE3 *r, ULONG n, ULONG rate)
{
    ULONG i;
    for (i = 0; i < n; i++)
        if (rate >= r[i].Min && rate <= r[i].Max &&
            (r[i].Min == r[i].Max || !((rate - r[i].Min) % r[i].Res))) return TRUE;
    return FALSE;
}

static const ULONG CommonRates[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000,
                                     88200, 96000, 176400, 192000 };

static VOID AddRate(ULONG *rates, ULONG max, ULONG *count, ULONG rate)
{
    ULONG i, j;
    if (rate < 8000 || rate > 384000) return;
    for (i = 0; i < *count && rates[i] < rate; i++) ;
    if (i < *count && rates[i] == rate) return;
    if (*count == max) return;
    for (j = *count; j > i; j--) rates[j] = rates[j - 1];
    rates[i] = rate; (*count)++;
}

/* A multiplier counts only at 1/1 (CM_NUMERATOR/DENOMINATOR, UAC2 5.2.5.3). */
static BOOLEAN UnityMultiplier(UAC_DEV *d, UCHAR id)
{
    UCHAR a[2], b[2];
    ULONG size = 2, size2 = 2;
    if (!NT_SUCCESS(EntityRequest(d, id, 1, 1, 1, 0, a, &size)) || size != 2) return FALSE;
    if (!NT_SUCCESS(EntityRequest(d, id, 1, 1, 2, 0, b, &size2)) || size2 != 2) return FALSE;
    return (a[0] | (a[1] << 8)) == (b[0] | (b[1] << 8)) && (a[0] | a[1]);
}

/* Walk from a clock entity towards its sources. With rate 0, collect every
 * reachable source's rates; with a rate, find the first path that carries
 * it and record the selector inputs taken. Bounded by depth. */
/* Pool-allocated: the recursive frames hold no arrays (the x86 kernel
 * stack is three pages). */
typedef struct WALK {
    ULONG Rate;
    ULONG Phase;                /* rate 0: 0 = exact and common rates, 1 = stepped extras */
    ULONG Stepped;              /* pass 0 met a range of more than one rate */
    ULONG *Rates, Max, *Count;
    UAC_CLOCK_PATH Path;
    ULONG Visits;               /* total entities entered: bounds the work */
    UAC_BYTE OnPath[256];       /* cycle guard */
    RANGE3 Scratch[RANGES];     /* one source's ranges, used at a leaf only */
} WALK;

static BOOLEAN WalkEntity(UAC_DEV *d, ULONG id, WALK *w, ULONG depth);
static CLAIM *FindClaim(UAC_DEV *d, UAC_BYTE id);
static BOOLEAN PathFree(UAC_DEV *d, const UAC_CLOCK_PATH *path, ULONG count, UAC_BYTE source, ULONG rate);

/* A clock selector's bmControls, after baCSourceID[] (0 if absent). */
static ULONG SelectorControls(const UCHAR *p)
{
    return p[0] > 5U + p[4] ? p[5 + p[4]] : 0;
}

static BOOLEAN Walk(UAC_DEV *d, ULONG id, WALK *w, ULONG depth)
{
    BOOLEAN found;
    if (id >= 256 || depth >= UAC_CLOCK_HOPS || w->OnPath[id] || ++w->Visits > WALK_VISITS)
        return FALSE;
    w->OnPath[id] = 1;
    found = WalkEntity(d, id, w, depth);
    w->OnPath[id] = 0;
    return found;
}

static BOOLEAN WalkEntity(UAC_DEV *d, ULONG id, WALK *w, ULONG depth)
{
    const UCHAR *p = Entity(d, id);
    ULONG i, n;
    RANGE3 *r = w->Scratch;
    if (!p) return FALSE;
    if (p[2] == 10) {
        if (!NT_SUCCESS(SourceRanges(d, (UCHAR)id, r, &n))) return FALSE;
        if (!w->Rate) {
            for (i = 0; i < n; i++) {
                ULONG k, lo, hi, steps;
                if (!w->Phase) {
                    /* Pass 0, every source: exact rates and the common
                     * rates a range holds, so no stepped extra displaces
                     * them from the bounded list. */
                    if (r[i].Min == r[i].Max) { AddRate(w->Rates, w->Max, w->Count, r[i].Min); continue; }
                    w->Stepped = 1;
                    for (k = 0; k < sizeof(CommonRates) / sizeof(CommonRates[0]); k++)
                        if (InRanges(&r[i], 1, CommonRates[k])) AddRate(w->Rates, w->Max, w->Count, CommonRates[k]);
                    continue;
                }
                /* Pass 1: a range's step-aligned rates inside 8..384 kHz,
                 * all of them when 16 or fewer, else the two ends. */
                if (r[i].Min == r[i].Max || r[i].Max < 8000 || r[i].Min > 384000) continue;
                lo = r[i].Min < 8000 ? r[i].Min + (8000 - r[i].Min + r[i].Res - 1) / r[i].Res * r[i].Res : r[i].Min;
                hi = r[i].Max > 384000 ? 384000 : r[i].Max;
                if (lo > hi) continue;
                hi = lo + (hi - lo) / r[i].Res * r[i].Res;
                steps = (hi - lo) / r[i].Res + 1;
                if (steps <= 16)
                    for (k = 0; k < steps; k++) AddRate(w->Rates, w->Max, w->Count, lo + k * r[i].Res);
                else {
                    AddRate(w->Rates, w->Max, w->Count, lo);
                    AddRate(w->Rates, w->Max, w->Count, hi);
                }
            }
            return FALSE; /* keep walking every input */
        }
        if (!InRanges(r, n, w->Rate)) return FALSE;
        /* A source whose frequency is not writable must already run the
         * rate; otherwise another path is tried. */
        if ((p[5] & 3) != 3) {
            UCHAR cur[4];
            ULONG size = sizeof(cur);
            if (!NT_SUCCESS(EntityRequest(d, (UCHAR)id, 1, 1, 1, 0, cur, &size)) || size != sizeof(cur) ||
                Word32(cur) != w->Rate) return FALSE;
        }
        /* A source Start will not set (external, or frequency not
         * writable) must already be valid where it says (D3..2): an
         * unlocked one leaves the selector's other inputs to try. */
        if (((p[5] >> 2) & 3) && (!(p[4] & 3) || (p[5] & 3) != 3)) {
            UCHAR valid = 0;
            ULONG size = 1;
            if (!NT_SUCCESS(EntityRequest(d, (UCHAR)id, 1, 1, 2, 0, &valid, &size)) || size != 1 ||
                valid != 1) return FALSE;
        }
        /* A path another running stream holds at other values is not
         * usable: keep looking for an independent one. */
        if (!PathFree(d, &w->Path, depth, (UAC_BYTE)id, w->Rate)) return FALSE;
        w->Path.Source = (UAC_BYTE)id;
        w->Path.Count = (UAC_BYTE)depth;
        return TRUE;
    }
    if (p[2] == 12) {
        if (!UnityMultiplier(d, (UCHAR)id)) return FALSE;
        w->Path.Id[depth] = (UAC_BYTE)id; w->Path.Kind[depth] = 12; w->Path.Pin[depth] = 1;
        return Walk(d, p[4], w, depth + 1);
    }
    if (p[2] == 11) {
        /* bmControls follows baCSourceID[]; a selector whose control is not
         * writable (D1..0 != 3) runs only on its current input. A writable
         * one tries its current input first. */
        UCHAR current = 0, pin;
        ULONG size = 1, controls = SelectorControls(p);
        /* A one-input selector without a readable control is on input 1;
         * no request is made (it may stall). */
        if (p[4] == 1 && !(controls & 3)) current = 1;
        else if (!NT_SUCCESS(EntityRequest(d, (UCHAR)id, 1, 1, 1, 0, &current, &size)) || size != 1 ||
                 current < 1 || current > p[4])
            current = 0;
        /* Step 0: the current input; then, if writable, every other one. */
        for (i = 0; i <= p[4]; i++) {
            if (!i) { if (!current) continue; pin = current; }
            else { if ((controls & 3) != 3 || i == current) continue; pin = (UCHAR)i; }
            w->Path.Id[depth] = (UAC_BYTE)id; w->Path.Kind[depth] = 11; w->Path.Pin[depth] = pin;
            if (Walk(d, p[4 + pin], w, depth + 1)) return TRUE;
        }
    }
    return FALSE;
}

static ULONG TerminalClock(UAC_DEV *d, UAC_BYTE terminal)
{
    const UCHAR *p = Entity(d, terminal);
    if (!p) return 0;
    if (p[2] == 2) return p[7];
    if (p[2] == 3) return p[8];
    return 0;
}

NTSTATUS UacUsbRates(UAC_DEV *d, UAC_BYTE terminal, ULONG *rates, ULONG max, ULONG *count)
{
    WALK *w;
    ULONG clock;
    if (!d || !rates || !count || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_PARAMETER;
    *count = 0;
    clock = TerminalClock(d, terminal);
    if (!clock) return STATUS_DEVICE_CONFIGURATION_ERROR;
    w = ExAllocatePoolWithTag(NonPagedPool, sizeof(*w), TAG);
    if (!w) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(w, sizeof(*w));
    w->Rates = rates; w->Max = max; w->Count = count;
    KeWaitForSingleObject(&d->Lock, Executive, KernelMode, FALSE, NULL);
    (void)Walk(d, clock, w, 0);
    /* The second pass only for stepped ranges: a device of discrete rates
     * sees one walk's requests, as before the two passes. With both
     * passes, a capture-only device's hot arrival on 98 SE under xHCI98
     * faulted WDMAud (wdmaud(07)+2F3C, 2026-10-24); cause not read. */
    if (w->Stepped) {
        w->Phase = 1; w->Visits = 0;
        (void)Walk(d, clock, w, 0);
    }
    KeReleaseMutex(&d->Lock, FALSE);
    ExFreePool(w);
    return *count ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

int UacUsbHighSpeed(UAC_DEV *d) { return d && d->HighSpeed; }

/* Optional read-only audibility diagnostics: the first feature unit's
 * master mute/volume. No mute or volume write. */
static VOID ReadFeature(UAC_USB *s)
{
    UAC_DEV *d = s->Dev;
    UCHAR bytes[2], controls = 0, id = 0;
    ULONG size, i;
    NTSTATUS status;
    s->Stats.FeatureStatus = STATUS_NOT_SUPPORTED;
    for (i = 1; i < 256 && !id; i++) {
        const UCHAR *p = Entity(d, i);
        if (p && p[2] == 6 && p[0] >= 10) { id = (UCHAR)i; controls = p[5]; }
    }
    if (!id) return;
    if ((controls & 3) == 1 || (controls & 3) == 3) {
        size = 1;
        status = EntityRequest(d, id, 1, 1, 1, 0, bytes, &size);
        if (NT_SUCCESS(status) && (size != 1 || bytes[0] > 1)) status = STATUS_DEVICE_DATA_ERROR;
        if (NT_SUCCESS(status)) { s->Stats.MuteKnown = 1; s->Stats.Mute = bytes[0]; }
        s->Stats.FeatureStatus = status;
    }
    if (((controls >> 2) & 3) == 1 || ((controls >> 2) & 3) == 3) {
        size = 2;
        status = EntityRequest(d, id, 1, 1, 2, 0, bytes, &size);
        if (NT_SUCCESS(status) && size != 2) status = STATUS_DEVICE_DATA_ERROR;
        if (NT_SUCCESS(status)) {
            s->Stats.VolumeKnown = 1;
            s->Stats.Volume256Db = (SHORT)((USHORT)bytes[0] | ((USHORT)bytes[1] << 8));
        }
        if (!NT_SUCCESS(status) || s->Stats.FeatureStatus == STATUS_NOT_SUPPORTED)
            s->Stats.FeatureStatus = status;
    }
}

/* Device lock held. The entities of a path with their values: each
 * selector at its input, the source at the rate. */
static ULONG PathClaims(const UAC_CLOCK_PATH *path, ULONG rate, UAC_BYTE *ids, ULONG *values)
{
    ULONG i, n = 0;
    for (i = 0; i < path->Count; i++)
        if (path->Kind[i] == 11) { ids[n] = path->Id[i]; values[n++] = path->Pin[i]; }
    ids[n] = path->Source; values[n++] = rate;
    return n;
}

static CLAIM *FindClaim(UAC_DEV *d, UAC_BYTE id)
{
    ULONG i;
    for (i = 0; i < CLAIMS; i++) if (d->Claim[i].Users && d->Claim[i].Id == id) return &d->Claim[i];
    return NULL;
}

/* Device lock held. The first count hops of path and the source at rate
 * agree with every claim. */
static BOOLEAN PathFree(UAC_DEV *d, const UAC_CLOCK_PATH *path, ULONG count, UAC_BYTE source, ULONG rate)
{
    ULONG i;
    CLAIM *c;
    for (i = 0; i < count; i++) {
        if (path->Kind[i] != 11) continue;
        c = FindClaim(d, path->Id[i]);
        if (c && c->Value != path->Pin[i]) return FALSE;
    }
    c = FindClaim(d, source);
    return !c || c->Value == rate;
}

/* Device lock held. Every entity is checked before any is taken: a stream
 * never moves a selector or a rate another running stream depends on. */
static NTSTATUS ClaimPath(UAC_DEV *d, const UAC_CLOCK_PATH *path, ULONG rate)
{
    UAC_BYTE ids[UAC_CLOCK_HOPS + 1];
    ULONG values[UAC_CLOCK_HOPS + 1], n = PathClaims(path, rate, ids, values), i, j, free = 0;
    for (i = 0; i < n; i++) {
        CLAIM *c = FindClaim(d, ids[i]);
        if (c && c->Value != values[i]) return STATUS_DEVICE_BUSY;
        if (!c) free++;
    }
    for (j = 0; j < CLAIMS; j++) if (!d->Claim[j].Users) { if (free) free--; }
    if (free) return STATUS_INSUFFICIENT_RESOURCES;
    for (i = 0; i < n; i++) {
        CLAIM *c = FindClaim(d, ids[i]);
        if (!c) {
            for (j = 0; j < CLAIMS && d->Claim[j].Users; j++) ;
            c = &d->Claim[j];
            c->Id = ids[i]; c->Value = values[i]; c->Users = 0;
        }
        c->Users++;
    }
    return STATUS_SUCCESS;
}

static VOID ReleasePath(UAC_DEV *d, const UAC_CLOCK_PATH *path, ULONG rate)
{
    UAC_BYTE ids[UAC_CLOCK_HOPS + 1];
    ULONG values[UAC_CLOCK_HOPS + 1], n, i;
    KeWaitForSingleObject(&d->Lock, Executive, KernelMode, FALSE, NULL);
    n = PathClaims(path, rate, ids, values);
    for (i = 0; i < n; i++) {
        CLAIM *c = FindClaim(d, ids[i]);
        if (c) c->Users--;
    }
    KeReleaseMutex(&d->Lock, FALSE);
}

/* Set the stream's clock path: selector inputs, then the source frequency
 * (written only if it differs and the control is writable), then read back
 * the frequency and, if readable, the validity. */
static NTSTATUS SetClock(UAC_USB *s)
{
    UAC_DEV *d = s->Dev;
    WALK *w;
    UCHAR current[4], valid, pin;
    const UCHAR *src;
    ULONG size, clock, i;
    NTSTATUS status;
    clock = TerminalClock(d, s->Alt.Terminal);
    if (!clock) return STATUS_DEVICE_CONFIGURATION_ERROR;
    w = ExAllocatePoolWithTag(NonPagedPool, sizeof(*w), TAG);
    if (!w) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(w, sizeof(*w));
    w->Rate = s->Rate;
    KeWaitForSingleObject(&d->Lock, Executive, KernelMode, FALSE, NULL);
    if (!Walk(d, clock, w, 0)) { status = STATUS_NOT_SUPPORTED; goto done; }
    status = ClaimPath(d, &w->Path, s->Rate);
    if (!NT_SUCCESS(status)) goto done;
    s->Path = w->Path; s->ClockHeld = TRUE;
    s->Stats.ClockId = w->Path.Source;
    for (i = 0; i < w->Path.Count; i++) {
        const UCHAR *sel;
        if (w->Path.Kind[i] != 11) continue;
        sel = Entity(d, w->Path.Id[i]);
        /* The walk already established a one-input or read-only selector
         * is on this input: only a writable one is read and set. */
        if (!sel || (SelectorControls(sel) & 3) != 3) continue;
        size = 1; pin = 0;
        status = EntityRequest(d, w->Path.Id[i], 1, 1, 1, 0, &pin, &size);
        if (!NT_SUCCESS(status) || size != 1) { status = STATUS_DEVICE_CONFIGURATION_ERROR; goto done; }
        if (pin != w->Path.Pin[i]) {
            pin = w->Path.Pin[i]; size = 1;
            status = EntityRequest(d, w->Path.Id[i], 0, 1, 1, 0, &pin, &size);
            if (!NT_SUCCESS(status)) goto done;
        }
    }
    src = Entity(d, w->Path.Source);
    size = sizeof(current);
    status = EntityRequest(d, w->Path.Source, 1, 1, 1, 0, current, &size);
    if (!NT_SUCCESS(status) || size != sizeof(current)) { status = STATUS_DEVICE_CONFIGURATION_ERROR; goto done; }
    if (Word32(current) != s->Rate) {
        if ((src[5] & 3) != 3) { status = STATUS_NOT_SUPPORTED; goto done; } /* frequency read only */
        current[0] = (UCHAR)s->Rate; current[1] = (UCHAR)(s->Rate >> 8);
        current[2] = (UCHAR)(s->Rate >> 16); current[3] = (UCHAR)(s->Rate >> 24);
        size = sizeof(current);
        status = EntityRequest(d, w->Path.Source, 0, 1, 1, 0, current, &size);
        if (!NT_SUCCESS(status)) goto done;
        size = sizeof(current);
        status = EntityRequest(d, w->Path.Source, 1, 1, 1, 0, current, &size);
        if (!NT_SUCCESS(status) || size != sizeof(current) || Word32(current) != s->Rate) {
            status = STATUS_DEVICE_CONFIGURATION_ERROR; goto done;
        }
    }
    if ((src[5] >> 2) & 3) {
        size = 1; valid = 0;
        status = EntityRequest(d, w->Path.Source, 1, 1, 2, 0, &valid, &size);
        if (!NT_SUCCESS(status) || size != 1 || valid != 1) { status = STATUS_DEVICE_CONFIGURATION_ERROR; goto done; }
    }
    s->Stats.ClockHz = s->Rate;
    status = STATUS_SUCCESS;
done:
    KeReleaseMutex(&d->Lock, FALSE);
    ExFreePool(w);
    if (!NT_SUCCESS(status) && s->ClockHeld) { ReleasePath(d, &s->Path, s->Rate); s->ClockHeld = FALSE; }
    return status;
}

static NTSTATUS Configure(UAC_DEV *d)
{
    struct _URB_SELECT_CONFIGURATION *urb;
    PUSBD_INTERFACE_INFORMATION info;
    ULONG size, entry, i, j;
    NTSTATUS status;
    /* Every interface the PDO's descriptor holds, at alternate 0
     * (UacAltZero): the function's interfaces under a composite parent, or
     * all of them when the whole device is bound (98 SE under NUSB). Pipes
     * outside the streaming interfaces are opened and never used. */
    entry = FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes);
    size = FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface);
    for (i = 0; i < d->IfCount; i++)
        size += entry + d->IfPipes[i] * sizeof(USBD_PIPE_INFORMATION);
    urb = ExAllocatePoolWithTag(NonPagedPool, size, TAG);
    if (!urb) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(urb, size);
    urb->Hdr.Length = (USHORT)size;
    urb->Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    urb->ConfigurationDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR)d->Config;
    info = &urb->Interface;
    for (i = 0; i < d->IfCount; i++) {
        info->Length = (USHORT)(entry + d->IfPipes[i] * sizeof(USBD_PIPE_INFORMATION));
        info->InterfaceNumber = d->IfNumber[i];
        info->AlternateSetting = 0;
        info->NumberOfPipes = d->IfPipes[i];
        for (j = 0; j < d->IfPipes[i]; j++) info->Pipes[j].MaximumTransferSize = 4096;
        info = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)info + info->Length);
    }
    status = UsbRequest(d->Lower, (PURB)urb);
    if (NT_SUCCESS(status)) {
        d->Configuration = urb->ConfigurationHandle;
        if (!d->Configuration) status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    ExFreePool(urb);
    return status;
}

static ULONG AltZeroPipes(UAC_DEV *d, ULONG iface)
{
    ULONG i;
    for (i = 0; i < d->IfCount; i++) if (d->IfNumber[i] == iface) return d->IfPipes[i];
    return 0;
}

static NTSTATUS SelectAlt(UAC_USB *s, BOOLEAN active)
{
    UAC_DEV *d = s->Dev;
    struct _URB_SELECT_INTERFACE *urb;
    ULONG pipes = active ? 1 + (s->Alt.FeedbackEndpoint ? 1 : 0) : AltZeroPipes(d, s->Alt.Interface);
    ULONG size = FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface) +
        FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) + pipes * sizeof(USBD_PIPE_INFORMATION);
    ULONG i;
    NTSTATUS status;
    urb = ExAllocatePoolWithTag(NonPagedPool, size, TAG);
    if (!urb) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(urb, size);
    urb->Hdr.Length = (USHORT)size; urb->Hdr.Function = URB_FUNCTION_SELECT_INTERFACE;
    urb->ConfigurationHandle = d->Configuration;
    urb->Interface.Length = (USHORT)(size - FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface));
    urb->Interface.InterfaceNumber = s->Alt.Interface;
    urb->Interface.AlternateSetting = active ? s->Alt.Alternate : 0;
    urb->Interface.NumberOfPipes = pipes;
    for (i = 0; i < pipes; i++) urb->Interface.Pipes[i].MaximumTransferSize = s->SlotBytes;
    KeWaitForSingleObject(&d->Lock, Executive, KernelMode, FALSE, NULL);
    status = UsbRequest(d->Lower, (PURB)urb);
    KeReleaseMutex(&d->Lock, FALSE);
    if (NT_SUCCESS(status)) {
        s->DataPipe = s->FeedbackPipe = NULL;
        s->ActiveAlt = active;
        for (i = 0; active && i < pipes; i++) {
            PUSBD_PIPE_INFORMATION pipe = &urb->Interface.Pipes[i];
            if (pipe->PipeType != UsbdPipeTypeIsochronous) { status = STATUS_DEVICE_CONFIGURATION_ERROR; break; }
            if (pipe->EndpointAddress == s->Alt.DataEndpoint) s->DataPipe = pipe->PipeHandle;
            if (s->Alt.FeedbackEndpoint && pipe->EndpointAddress == s->Alt.FeedbackEndpoint)
                s->FeedbackPipe = pipe->PipeHandle;
        }
        if (active && (!s->DataPipe || (s->Feedback && !s->FeedbackPipe)))
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    ExFreePool(urb);
    return status;
}

static NTSTATUS SlotDone(PDEVICE_OBJECT device, PIRP irp, PVOID context)
{
    USB_SLOT *slot = context;
    UAC_USB *s = slot->Owner;
    KIRQL old;
    UNREFERENCED_PARAMETER(device);
    KeAcquireSpinLock(&s->Lock, &old);
    slot->Status = irp->IoStatus.Status;
    slot->Phase = 2;
    KeSetEvent(&s->Wake, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&s->Lock, old);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS SubmitSlot(UAC_USB *s, USB_SLOT *slot)
{
    ULONG i, size = 0, n, nonzero = 0;
    PIO_STACK_LOCATION stack;
    KIRQL old;
    struct _URB_ISOCH_TRANSFER *urb = slot->Urb;
    RtlZeroMemory(urb, FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, IsoPacket) +
                    slot->Packets * sizeof(USBD_ISO_PACKET_DESCRIPTOR));
    urb->Hdr.Length = (USHORT)(FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, IsoPacket) +
                    slot->Packets * sizeof(USBD_ISO_PACKET_DESCRIPTOR));
    urb->Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    urb->PipeHandle = slot->Feedback ? s->FeedbackPipe : s->DataPipe;
    urb->TransferFlags = USBD_START_ISO_TRANSFER_ASAP |
        (slot->Feedback || s->Capture ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : 0);
    urb->NumberOfPackets = slot->Packets;
    urb->TransferBuffer = slot->Buffer;
    for (i = 0; i < slot->Packets; i++) {
        if (slot->Feedback) n = s->FeedbackBytes;
        else if (s->Capture) n = s->Capacity;
        else n = UacRateNextBytes(&s->Rate_);
        /* A zero-length OUT packet is legal: a cadence below one frame per
         * service interval (8 kHz at High Speed) has empty intervals. */
        if (n > (slot->Feedback ? s->FeedbackBytes : s->Capacity) || size > s->SlotBytes - n)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        urb->IsoPacket[i].Offset = size;
        size += n;
    }
    urb->TransferBufferLength = size;
    slot->Bytes = size;
    RtlZeroMemory(slot->Buffer, size); /* playback: defensive underrun silence */
    if (!slot->Feedback && !s->Capture) {
        s->Fill(s->CallbackContext, slot->Buffer, size);
        for (i = 0; i < size; i++) if (slot->Buffer[i]) nonzero++;
    }
    slot->Irp = IoAllocateIrp(s->Dev->Lower->StackSize, FALSE);
    if (!slot->Irp) return STATUS_INSUFFICIENT_RESOURCES;
    slot->Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    stack = IoGetNextIrpStackLocation(slot->Irp);
    RtlZeroMemory(stack, sizeof(*stack));
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = urb;
    IoSetCompletionRoutine(slot->Irp, SlotDone, slot, TRUE, TRUE, TRUE);
    KeAcquireSpinLock(&s->Lock, &old);
    if (s->Stop) {
        KeReleaseSpinLock(&s->Lock, old);
        IoFreeIrp(slot->Irp); slot->Irp = NULL;
        return STATUS_DELETE_PENDING;
    }
    slot->CancelIssued = FALSE; slot->Phase = 1;
    if (!slot->Feedback) {
        slot->Sequence = s->NextSequence++;
        if (!s->Capture) {
            s->Stats.SubmittedBytes += size;
            s->Stats.SubmittedNonzeroBytes += nonzero;
        }
    }
    KeReleaseSpinLock(&s->Lock, old);
    (void)IoCallDriver(s->Dev->Lower, slot->Irp);
    return STATUS_SUCCESS;
}

static VOID Fail(UAC_USB *s, NTSTATUS status)
{
    KIRQL old;
    KeAcquireSpinLock(&s->Lock, &old);
    if (NT_SUCCESS(s->Stats.LastStatus)) s->Stats.LastStatus = status;
    s->Stop = TRUE;
    KeSetEvent(&s->Wake, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&s->Lock, old);
}

static ULONG PacketErrors(USB_SLOT *slot)
{
    ULONG i, errors = 0;
    for (i = 0; i < slot->Packets; i++)
        if (!USBD_SUCCESS(slot->Urb->IsoPacket[i].Status)) errors++;
    if (!NT_SUCCESS(slot->Status) || !USBD_SUCCESS(slot->Urb->Hdr.Status))
        if (!errors) errors = slot->Packets;
    return errors;
}

/* Caller holds s->Lock. Counts what each feedback packet returned. */
static VOID FbCount(UAC_USB *s, ULONG code)
{
    ULONG k;
    for (k = 0; k < 4; k++) {
        if (s->Stats.FbCodeCount[k] && s->Stats.FbCode[k] == code) { s->Stats.FbCodeCount[k]++; return; }
        if (!s->Stats.FbCodeCount[k]) { s->Stats.FbCode[k] = code; s->Stats.FbCodeCount[k] = 1; return; }
    }
}

/* Caller holds s->Lock. */
static VOID DataCount(UAC_USB *s, ULONG code)
{
    ULONG k;
    for (k = 0; k < 4; k++) {
        if (s->Stats.DataCodeCount[k] && s->Stats.DataCode[k] == code) { s->Stats.DataCodeCount[k]++; return; }
        if (!s->Stats.DataCodeCount[k]) { s->Stats.DataCode[k] = code; s->Stats.DataCodeCount[k] = 1; return; }
    }
}

static VOID DataDiag(UAC_USB *s, USB_SLOT *slot, NTSTATUS status)
{
    ULONG i, next;
    if (!NT_SUCCESS(status) || !USBD_SUCCESS(slot->Urb->Hdr.Status)) {
        s->Stats.DataUrbFailed++;
        DataCount(s, (ULONG)slot->Urb->Hdr.Status);
    }
    for (i = 0; i < slot->Packets; i++) {
        PUSBD_ISO_PACKET_DESCRIPTOR packet = &slot->Urb->IsoPacket[i];
        next = i + 1 < slot->Packets ? slot->Urb->IsoPacket[i + 1].Offset : slot->Bytes;
        if (USBD_SUCCESS(packet->Status)) {
            s->Stats.DataPktOk++;
            if (s->Capture) s->Stats.DataPktOkBytes += packet->Length;
            else if (next > packet->Offset) s->Stats.DataPktOkBytes += next - packet->Offset;
        } else DataCount(s, (ULONG)packet->Status);
    }
}

static VOID FeedbackDiag(UAC_USB *s, USB_SLOT *slot, NTSTATUS status)
{
    ULONG i;
    if (!s->Stats.FeedbackUrbs) s->Stats.FbFirstStartFrame = slot->Urb->StartFrame;
    s->Stats.FbLastStartFrame = slot->Urb->StartFrame;
    if (!NT_SUCCESS(status) || !USBD_SUCCESS(slot->Urb->Hdr.Status)) {
        s->Stats.FbUrbFailed++;
        FbCount(s, (ULONG)slot->Urb->Hdr.Status);
    }
    for (i = 0; i < slot->Packets; i++) {
        PUSBD_ISO_PACKET_DESCRIPTOR packet = &slot->Urb->IsoPacket[i];
        if (!USBD_SUCCESS(packet->Status)) FbCount(s, (ULONG)packet->Status);
        else if (packet->Length == s->FeedbackBytes) s->Stats.FbFourBytes++;
        else if (!packet->Length) s->Stats.FbZeroLength++;
        else FbCount(s, 0x40000000 | packet->Length);
    }
}

static VOID Completed(UAC_USB *s, USB_SLOT *slot)
{
    KIRQL old;
    ULONG i, j, raw, errors = PacketErrors(slot);
    BOOLEAN stop, valid = FALSE, good;
    NTSTATUS status = slot->Status;
    USBD_STATUS usbStatus = slot->Urb->Hdr.Status;
    IoFreeIrp(slot->Irp); slot->Irp = NULL;
    KeAcquireSpinLock(&s->Lock, &old);
    stop = s->Stop;
    if (status == STATUS_CANCELLED) s->Stats.Cancelled++;
    /* Expected flush cancellations are not playback packet failures. */
    if (!stop && status != STATUS_CANCELLED) {
        s->Stats.PacketErrors += errors;
        if (!USBD_SUCCESS(usbStatus)) s->Stats.LastUsbStatus = usbStatus;
    }
    KeReleaseSpinLock(&s->Lock, old);
    good = !stop && NT_SUCCESS(status) && USBD_SUCCESS(usbStatus);
    if (slot->Feedback) {
        if (good) {
            for (i = 0; i < slot->Packets; i++) {
                PUSBD_ISO_PACKET_DESCRIPTOR packet = &slot->Urb->IsoPacket[i];
                if (USBD_SUCCESS(packet->Status)) {
                    raw = 0;
                    if (packet->Length && packet->Length <= 4 && packet->Offset <= slot->Bytes - packet->Length)
                        for (j = 0; j < packet->Length; j++)
                            raw |= (ULONG)slot->Buffer[packet->Offset + j] << (8 * j);
                    KeAcquireSpinLock(&s->Lock, &old);
                    if (packet->Length) s->Stats.LastFeedbackValue = raw;
                    s->Stats.LastFeedbackLength = packet->Length;
                    KeReleaseSpinLock(&s->Lock, old);
                }
                /* USB2 section5.12.4.2 permits an unchanged feedback value
                 * as a successful zero-length payload. It proves packet
                 * freshness only after a valid initial rate was received;
                 * it neither resets the planner nor invents a new value. */
                if (USBD_SUCCESS(packet->Status) && !packet->Length && s->Rate_.FeedbackActive) {
                    valid = TRUE;
                    KeAcquireSpinLock(&s->Lock, &old);
                    s->Stats.FeedbackKeepalives++;
                    KeReleaseSpinLock(&s->Lock, old);
                } else if (USBD_SUCCESS(packet->Status) && packet->Length == s->FeedbackBytes &&
                    packet->Offset <= slot->Bytes - s->FeedbackBytes) {
                    if (UacRateFeedback(&s->Rate_, slot->Buffer + packet->Offset, s->FeedbackBytes)) {
                        valid = TRUE;
                        s->LastFeedbackValueAt = KeQueryInterruptTime();
                    }
                } else s->Rate_.BadFeedback++;
            }
        }
        if (valid) s->LastFeedbackPacket = KeQueryInterruptTime();
        KeAcquireSpinLock(&s->Lock, &old);
        if (!stop && status != STATUS_CANCELLED) FeedbackDiag(s, slot, status);
        s->Stats.FeedbackUrbs++;
        s->Stats.GoodFeedback = s->Rate_.GoodFeedback;
        s->Stats.BadFeedback = s->Rate_.BadFeedback;
        KeReleaseSpinLock(&s->Lock, old);
    } else {
        ULONG delivered = 0;
        KeAcquireSpinLock(&s->Lock, &old);
        if (!s->Stats.DataUrbs) s->Stats.DataFirstStartFrame = slot->Urb->StartFrame;
        if (!stop && status != STATUS_CANCELLED) DataDiag(s, slot, status);
        s->Stats.DataUrbs++;
        if (good && !s->Capture) s->Stats.CompletedBytes += slot->Bytes;
        KeReleaseSpinLock(&s->Lock, old);
        if (good && s->Capture) {
            /* Whole PCM frames of each successful packet, in order. */
            for (i = 0; i < slot->Packets; i++) {
                PUSBD_ISO_PACKET_DESCRIPTOR packet = &slot->Urb->IsoPacket[i];
                ULONG length = packet->Length;
                if (!USBD_SUCCESS(packet->Status) || !length) continue;
                if (length > s->Capacity || packet->Offset > slot->Bytes - length) continue;
                length -= length % s->FrameBytes;
                if (length) s->Captured(s->CallbackContext, slot->Buffer + packet->Offset, length);
                delivered += length;
            }
            KeAcquireSpinLock(&s->Lock, &old);
            s->Stats.CapturedBytes += delivered; s->Stats.CompletedBytes += delivered;
            KeReleaseSpinLock(&s->Lock, old);
            s->Progress(s->CallbackContext, 0, errors);
        } else if (good) {
            /* Advance in submission order, including failed/lost scheduled
             * packets. Never retry time-specific PCM or synthesize a timer. */
            s->Progress(s->CallbackContext, slot->Bytes, errors);
        }
        s->CompletedSequence++;
    }
    slot->Phase = 0;
    if (!stop && !NT_SUCCESS(status)) Fail(s, status);
    else if (!stop && !USBD_SUCCESS(usbStatus)) Fail(s, STATUS_DEVICE_DATA_ERROR);
}

static VOID Worker(PVOID context)
{
    UAC_USB *s = context;
    ULONG i, pending;
    KIRQL old;
    BOOLEAN stop, progress;
    LONG phase;
    NTSTATUS status;
    LARGE_INTEGER timeout;
    /* 98 SE: waiting on a thread object faulted there (usbmidi's stop path,
     * 2026-10-22), so the stop waits for Exited; 2000 and later also wait
     * the thread object so the image cannot unload under it. */
    s->Thread = PsGetCurrentThread();
    if (s->WaitThread) ObReferenceObject(s->Thread);
    KeSetEvent(&s->Ready, IO_NO_INCREMENT, FALSE);
    for (;;) {
        KeClearEvent(&s->Wake);
        /* Feedback first so newly prepared data uses the newest valid rate.
         * Data completions are retired in sequence even if callbacks reorder. */
        do {
            progress = FALSE;
            for (i = 0; i < SLOTS; i++) {
                USB_SLOT *slot = &s->Slot[i];
                if (!slot->Used) continue;
                KeAcquireSpinLock(&s->Lock, &old); phase = slot->Phase; KeReleaseSpinLock(&s->Lock, old);
                if (phase == 2 && (slot->Feedback || slot->Sequence == s->CompletedSequence)) {
                    Completed(s, slot); progress = TRUE;
                }
            }
        } while (progress);
        KeAcquireSpinLock(&s->Lock, &old); stop = s->Stop; KeReleaseSpinLock(&s->Lock, old);
        pending = 0;
        for (i = 0; i < SLOTS; i++) {
            USB_SLOT *slot = &s->Slot[i];
            if (!slot->Used) continue;
            KeAcquireSpinLock(&s->Lock, &old); phase = slot->Phase; KeReleaseSpinLock(&s->Lock, old);
            if (stop && phase == 1 && !slot->CancelIssued) {
                slot->CancelIssued = TRUE;
                (void)IoCancelIrp(slot->Irp);
            }
            if (phase) pending++;
            if (!stop && !phase) {
                status = SubmitSlot(s, slot);
                if (!NT_SUCCESS(status)) { Fail(s, status); stop = TRUE; }
                else pending++;
            }
        }
        if (stop && !pending) break;
        /* Adjustable system time is unsuitable for elapsed deadlines: NTP
         * or SetSystemTime must not create a feedback loss or hide one. */
        if (!stop && s->Feedback && UacFeedbackLate(KeQueryInterruptTime(), s->LastFeedbackPacket,
                                                    s->Rate_.FeedbackActive))
            Fail(s, STATUS_IO_TIMEOUT); /* no usable rate for 1 s at start, 200 ms after */
        timeout.QuadPart = -1000000; /* watchdog only; events drive PCM */
        KeWaitForSingleObject(&s->Wake, Executive, KernelMode, FALSE, &timeout);
    }
    KeAcquireSpinLock(&s->Lock, &old); s->Stats.Running = 0; KeReleaseSpinLock(&s->Lock, old);
    KeSetEvent(&s->Exited, IO_NO_INCREMENT, FALSE); /* last access to s */
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS UacUsbOpen(PDEVICE_OBJECT lower, const UAC_DESC *desc, UAC_DEV **out)
{
    UAC_DEV *d;
    UAC_BYTE numbers[32], pipes[32];
    unsigned int ifCount;
    BOOLEAN high = FALSE;
    NTSTATUS status;
    if (!out) return STATUS_INVALID_PARAMETER;
    *out = NULL;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !lower || !desc || !desc->Bytes ||
        desc->Length < 9 || desc->Length > 65535 || desc->AltCount > UAC_MAX_ALTS)
        return STATUS_INVALID_PARAMETER;
    /* The descriptor is the composite parent's function view, or the
     * whole device where no parent splits it (98 SE under NUSB). */
    if (UacAltZero(desc->Bytes, desc->Length, numbers, pipes, 32, &ifCount) != UAC_OK)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    status = BusSpeed(lower, &high);
    if (!NT_SUCCESS(status)) return STATUS_NOT_SUPPORTED; /* speed unknown: not guessed */
    d = ExAllocatePoolWithTag(NonPagedPool, sizeof(*d), TAG);
    if (!d) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(d, sizeof(*d));
    d->Config = ExAllocatePoolWithTag(NonPagedPool, desc->Length, TAG);
    if (!d->Config) { ExFreePool(d); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlCopyMemory(d->Config, desc->Bytes, desc->Length);
    d->Desc = *desc; d->Desc.Bytes = d->Config;
    RtlCopyMemory(d->IfNumber, numbers, ifCount); RtlCopyMemory(d->IfPipes, pipes, ifCount);
    d->IfCount = ifCount;
    d->Lower = lower; d->HighSpeed = high;
    KeInitializeMutex(&d->Lock, 0);
    status = Configure(d);
    if (!NT_SUCCESS(status)) { ExFreePool(d->Config); ExFreePool(d); return status; }
    *out = d;
    return STATUS_SUCCESS;
}

void UacUsbClose(UAC_DEV *d)
{
    if (!d || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    ASSERT(!d->Streams);
    ExFreePool(d->Config); ExFreePool(d);
}

NTSTATUS UacUsbCreate(UAC_DEV *d, int capture, UAC_BYTE terminal, ULONG channels,
    ULONG subslot, ULONG bits, ULONG config, ULONG rate,
    UAC_USB_FILL fill, UAC_USB_CAPTURE captured, UAC_USB_PROGRESS progress,
    PVOID context, UAC_USB **out)
{
    UAC_USB *s;
    unsigned int index;
    const UAC_ALT *a;
    if (!out) return STATUS_INVALID_PARAMETER;
    *out = NULL;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !d || !progress ||
        (capture ? !captured : !fill)) return STATUS_INVALID_PARAMETER;
    if (UacPlanPick(&d->Desc, capture, terminal, channels, subslot, bits, config, rate, d->HighSpeed, &index) != UAC_OK)
        return STATUS_NOT_SUPPORTED;
    a = &d->Desc.Alt[index];
    s = ExAllocatePoolWithTag(NonPagedPool, sizeof(*s), TAG);
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(s, sizeof(*s));
    s->Dev = d; s->Alt = *a; s->Capture = capture ? 1 : 0; s->Rate = rate;
    s->FrameBytes = (ULONG)a->Channels * a->Subslot;
    s->Capacity = UacPlanCapacity(a->MaxPacket);
    s->Feedback = !capture && UacPlanSync(a) == 1;
    s->FeedbackBytes = d->HighSpeed ? 4 : 3;
    /* About 4 ms per data URB at High Speed bInterval 1-3 and at Full
     * Speed (16 ms queued in DATA_SLOTS); 8 ms at bInterval 4 (32 ms), as
     * High Speed packet counts stay a multiple of 8 (one frame) for the
     * older stacks. Inside the cyclic buffer uacstream.h allocates (at
     * least 62 ms); a client shrinking it with SetBufferSize below the
     * queue is not guarded. */
    if (d->HighSpeed) {
        s->DataPackets = 32 >> (a->Interval - 1);
        if (s->DataPackets < 8) s->DataPackets = 8;
    } else s->DataPackets = 4;
    s->SlotBytes = s->DataPackets * s->Capacity;
    s->Fill = fill; s->Captured = captured; s->Progress = progress; s->CallbackContext = context;
    KeInitializeSpinLock(&s->Lock); KeInitializeMutex(&s->Operation, 0);
    KeInitializeEvent(&s->Wake, NotificationEvent, FALSE);
    KeInitializeEvent(&s->Ready, NotificationEvent, FALSE);
    KeInitializeEvent(&s->Exited, NotificationEvent, FALSE);
    s->WaitThread = IoIsWdmVersionAvailable(1, 0x10);
    KeWaitForSingleObject(&d->Lock, Executive, KernelMode, FALSE, NULL);
    d->Streams++;
    KeReleaseMutex(&d->Lock, FALSE);
    *out = s; return STATUS_SUCCESS;
}

static VOID FreeSlots(UAC_USB *s)
{
    ULONG i;
    for (i = 0; i < SLOTS; i++) {
        USB_SLOT *slot = &s->Slot[i];
        /* Only called after thread termination, or before any submission. */
        if (slot->Urb) ExFreePool(slot->Urb);
        if (slot->Buffer) ExFreePool(slot->Buffer);
        RtlZeroMemory(slot, sizeof(*slot));
    }
}

static VOID StopLocked(UAC_USB *s)
{
    KIRQL old;
    if (s->Thread) {
        KeAcquireSpinLock(&s->Lock, &old);
        s->Stop = TRUE;
        KeSetEvent(&s->Wake, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&s->Lock, old);
        KeWaitForSingleObject(&s->Exited, Executive, KernelMode, FALSE, NULL);
        if (s->WaitThread) {
            KeWaitForSingleObject(s->Thread, Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(s->Thread);
        } else {
            LARGE_INTEGER t; t.QuadPart = -100000; /* 10 ms: let it leave our code */
            KeDelayExecutionThread(KernelMode, FALSE, &t);
        }
        s->Thread = NULL;
    }
    FreeSlots(s);
    if (s->ActiveAlt) {
        NTSTATUS status = SelectAlt(s, FALSE);
        if (!NT_SUCCESS(status) && NT_SUCCESS(s->Stats.LastStatus)) s->Stats.LastStatus = status;
        s->ActiveAlt = FALSE;
    }
    if (s->ClockHeld) { ReleasePath(s->Dev, &s->Path, s->Rate); s->ClockHeld = FALSE; }
    s->DataPipe = s->FeedbackPipe = NULL;
    s->Stats.Running = 0;
}

typedef struct CREATE_JOB {
    WORK_QUEUE_ITEM Item;
    UAC_USB *Stream;
    KEVENT Done;
    NTSTATUS Status;
} CREATE_JOB;

/* System process, PASSIVE. The event is the last access to the job. */
static VOID CreateWorker(PVOID context)
{
    CREATE_JOB *job = context;
    HANDLE thread;
    job->Status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, Worker, job->Stream);
    if (NT_SUCCESS(job->Status)) ZwClose(thread);
    KeSetEvent(&job->Done, IO_NO_INCREMENT, FALSE);
}

NTSTATUS UacUsbStart(UAC_USB *s)
{
    NTSTATUS status;
    HANDLE thread;
    ULONG i, size;
    KIRQL old;
    BOOLEAN already;
    if (!s || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    KeWaitForSingleObject(&s->Operation, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&s->Lock, &old);
    already = s->Thread && s->Stats.Running && !s->Stop;
    KeReleaseSpinLock(&s->Lock, old);
    if (already) { KeReleaseMutex(&s->Operation, FALSE); return STATUS_SUCCESS; }
    StopLocked(s);
    RtlZeroMemory(&s->Stats, sizeof(s->Stats));
    s->Stats.HighSpeed = s->Dev->HighSpeed;
    s->Stats.Capture = s->Capture; s->Stats.Channels = s->Alt.Channels;
    s->Stats.Bits = s->Alt.Bits; s->Stats.Subslot = s->Alt.Subslot;
    s->Stats.Interface = s->Alt.Interface; s->Stats.Alternate = s->Alt.Alternate;
    s->Stats.Sync = UacPlanSync(&s->Alt);
    status = SetClock(s);
    if (!NT_SUCCESS(status)) goto done;
    ReadFeature(s);
    status = SelectAlt(s, TRUE);
    if (!NT_SUCCESS(status)) goto done;
    {
        struct _URB_GET_CURRENT_FRAME_NUMBER frame;
        RtlZeroMemory(&frame, sizeof(frame));
        frame.Hdr.Length = sizeof(frame);
        frame.Hdr.Function = URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
        if (NT_SUCCESS(UsbRequest(s->Dev->Lower, (PURB)&frame))) s->Stats.StartCurrentFrame = frame.FrameNumber;
        else s->Stats.StartCurrentFrame = 0xffffffff;
    }
    if (!s->Capture &&
        !UacRateInitialize(&s->Rate_, s->Rate, s->Dev->HighSpeed, s->Alt.Interval,
                           s->FrameBytes, s->Capacity)) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR; goto done;
    }
    for (i = 0; i < SLOTS; i++) {
        USB_SLOT *slot = &s->Slot[i];
        slot->Owner = s;
        /* First slots are feedback, hence processed/submitted first. */
        slot->Feedback = i < FEEDBACK_SLOTS;
        slot->Used = slot->Feedback ? s->Feedback : TRUE;
        if (!slot->Used) continue;
        slot->Packets = slot->Feedback ? FEEDBACK_PACKETS : s->DataPackets;
        size = FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, IsoPacket) +
            slot->Packets * sizeof(USBD_ISO_PACKET_DESCRIPTOR);
        slot->Urb = ExAllocatePoolWithTag(NonPagedPool, size, TAG);
        slot->Buffer = ExAllocatePoolWithTag(NonPagedPool,
            slot->Feedback ? FEEDBACK_PACKETS * s->FeedbackBytes : s->SlotBytes, TAG);
        if (!slot->Urb || !slot->Buffer) { status = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    }
    s->NextSequence = s->CompletedSequence = 0;
    s->Stop = FALSE; s->Stats.Running = 1;
    s->LastFeedbackPacket = KeQueryInterruptTime();
    s->LastFeedbackValueAt = s->LastFeedbackPacket;
    KeClearEvent(&s->Ready); KeClearEvent(&s->Wake); KeClearEvent(&s->Exited);
    /* PsCreateSystemThread wants the system process on 2000 and 98 SE;
     * SetState runs in the client's. An executive work item creates the
     * thread (and closes its handle) there; Start waits for it, so the
     * request, the stream and the image outlive the callback's use. */
    {
        CREATE_JOB job;
        job.Stream = s;
        KeInitializeEvent(&job.Done, NotificationEvent, FALSE);
        ExInitializeWorkItem(&job.Item, CreateWorker, &job);
        ExQueueWorkItem(&job.Item, DelayedWorkQueue);
        KeWaitForSingleObject(&job.Done, Executive, KernelMode, FALSE, NULL);
        status = job.Status;
        UNREFERENCED_PARAMETER(thread);
    }
    if (!NT_SUCCESS(status)) { s->Stats.Running = 0; goto done; }
    KeWaitForSingleObject(&s->Ready, Executive, KernelMode, FALSE, NULL);
done:
    if (!NT_SUCCESS(status)) { StopLocked(s); s->Stats.LastStatus = status; }
    KeReleaseMutex(&s->Operation, FALSE);
    return status;
}

void UacUsbStop(UAC_USB *s)
{
    if (!s || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    KeWaitForSingleObject(&s->Operation, Executive, KernelMode, FALSE, NULL);
    StopLocked(s);
    KeReleaseMutex(&s->Operation, FALSE);
}

void UacUsbDestroy(UAC_USB *s)
{
    UAC_DEV *d;
    if (!s || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    UacUsbStop(s);
    d = s->Dev;
    KeWaitForSingleObject(&d->Lock, Executive, KernelMode, FALSE, NULL);
    d->Streams--;
    KeReleaseMutex(&d->Lock, FALSE);
    ExFreePool(s);
}

void UacUsbGetStats(UAC_USB *s, UAC_USB_STATS *stats)
{
    KIRQL old;
    if (!stats) return;
    RtlZeroMemory(stats, sizeof(*stats));
    if (!s) return;
    KeAcquireSpinLock(&s->Lock, &old);
    *stats = s->Stats;
    KeReleaseSpinLock(&s->Lock, old);
}
