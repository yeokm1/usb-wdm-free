/* SPDX-License-Identifier: GPL-2.0-or-later
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
#ifdef UAC_V1
#include "uacconv.h"
#endif

/* At most UAC_QUEUE_MAX_URBS data URBs (docs/design/uac2.md 19.4); a
 * stream uses DataSlots of them. */
#define DATA_SLOTS UAC_QUEUE_MAX_URBS
#define FEEDBACK_SLOTS 2
#define SLOTS (DATA_SLOTS + FEEDBACK_SLOTS)
#define FEEDBACK_PACKETS 8
#define CLAIMS 16
#define WALK_VISITS 64
#define TAG 'b2aU'

/* The trace flavour (build.cmd trace): a stream trace to QEMU's debug
 * console port 0xE9, test-only and never released. Events go to a small
 * ring as they happen and are written out at an anomaly (a failure, a
 * worker wait that saw nothing for 100 ms, a stop), so the output does
 * not slow the stream it watches; the first events of every start are
 * written at once. A free or checked build compiles none of it. */
#if defined(UAC_TRACE_E9)
#define TR_RING 4096
#define TR_HEAD 48
typedef struct TR_EVENT { ULONG T; char K; UCHAR Slot; USHORT Packets; ULONG Seq, A, B, C, D; } TR_EVENT;
static TR_EVENT TrRing[TR_RING];
static ULONG TrNext, TrFrom, TrHead;
static KSPIN_LOCK TrLock;
static LONG TrInit;

static VOID TrByte(char c)
{
#if defined(_M_AMD64)
    __outbyte(0xe9, (unsigned char)c);
#else
    __asm { mov dx, 0xe9
            mov al, c
            out dx, al }
#endif
}

static VOID TrHex(ULONG v)
{
    int i;
    for (i = 28; i >= 0; i -= 4) TrByte("0123456789abcdef"[(v >> i) & 15]);
}

static VOID TrDec(ULONG v)
{
    char b[12];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) TrByte(b[--n]);
}

static VOID TrStr(const char *s) { while (*s) TrByte(*s++); }

/* Milliseconds from the performance counter (the interrupt time is the
 * stream's own clock; watchchk.ps1 counts its uses). */
static ULONG TrNow(void)
{
    LARGE_INTEGER hz, now = KeQueryPerformanceCounter(&hz);
    return hz.QuadPart >= 1000 ? (ULONG)(now.QuadPart / (hz.QuadPart / 1000)) : 0;
}

static VOID TrLine(const char *tag, ULONG a, ULONG b, ULONG c)
{
    TrStr("u2: "); TrDec(TrNow()); TrByte(' '); TrStr(tag);
    TrByte(' '); TrHex(a); TrByte(' '); TrHex(b); TrByte(' '); TrHex(c); TrByte('\n');
}

static VOID TrPrint(const TR_EVENT *e)
{
    TrStr("u2: "); TrDec(e->T); TrByte(' '); TrByte(e->K);
    TrStr(" s="); TrDec(e->Slot); TrStr(" q="); TrDec(e->Seq); TrStr(" n="); TrDec(e->Packets);
    TrByte(' '); TrHex(e->A); TrByte(' '); TrHex(e->B); TrByte(' '); TrHex(e->C); TrByte(' '); TrHex(e->D);
    TrByte('\n');
}

static VOID TrSetup(void)
{
    if (!InterlockedExchange(&TrInit, 1)) KeInitializeSpinLock(&TrLock);
}

/* K: 'S' submit (A bytes, B current frame, C flags), 'C' completion (A
 * IRP status, B URB status, C start frame, D current frame), 'P' after a
 * completion with errors (A the first bad packet's status, B its length,
 * C bad packets << 16 | first bad index, D the URB's ErrorCount). */
static VOID TrEvent(char k, ULONG slot, ULONG seq, ULONG packets, ULONG a, ULONG b, ULONG c, ULONG d)
{
    KIRQL old;
    TR_EVENT e;
    BOOLEAN now;
    e.T = TrNow(); e.K = k; e.Slot = (UCHAR)slot; e.Packets = (USHORT)packets;
    e.Seq = seq; e.A = a; e.B = b; e.C = c; e.D = d;
    KeAcquireSpinLock(&TrLock, &old);
    now = TrHead < TR_HEAD;
    if (now) TrHead++;
    else {
        TrRing[TrNext % TR_RING] = e;
        TrNext++;
        if (TrNext - TrFrom > TR_RING) TrFrom = TrNext - TR_RING;
    }
    KeReleaseSpinLock(&TrLock, old);
    if (now) TrPrint(&e);
}

/* Writes what the ring holds since the last dump, then the reason. */
static VOID TrDump(const char *why, ULONG a, ULONG b, ULONG c)
{
    KIRQL old;
    TR_EVENT e;
    for (;;) {
        KeAcquireSpinLock(&TrLock, &old);
        if (TrFrom == TrNext) { KeReleaseSpinLock(&TrLock, old); break; }
        e = TrRing[TrFrom % TR_RING];
        TrFrom++;
        KeReleaseSpinLock(&TrLock, old);
        TrPrint(&e);
    }
    TrLine(why, a, b, c);
}

static VOID TrRestart(void)
{
    KIRQL old;
    KeAcquireSpinLock(&TrLock, &old);
    TrHead = 0; TrFrom = TrNext;
    KeReleaseSpinLock(&TrLock, old);
}
void UacTrace(const char *tag, ULONG a, ULONG b, ULONG c)
{
    TrDump(tag, a, b, c);
}
#define TR_EVENT_(x) TrEvent x
#define TR_DUMP(x) TrDump x
#define TR_LINE(x) TrLine x
#else
#define TR_EVENT_(x)
#define TR_DUMP(x)
#define TR_LINE(x)
#endif

typedef struct USB_SLOT {
    struct UAC_USB *Owner;
    PIRP Irp;
    struct _URB_ISOCH_TRANSFER *Urb;
    PUCHAR Buffer;
    ULONG Packets, Bytes, BufferBytes, Sequence;
    LONG Phase; /* 0 idle,1 lower-owned,2 completed and caller-owned */
    ULONG DoneStamp; /* section 25: the stream's completion count when it came back */
    ULONG SubmitFrame; /* 26.13: the bus frame at submission, while SubmitFrameValid */
    BOOLEAN Feedback, CancelIssued, Used, SubmitFrameValid;
    NTSTATUS Status;
#ifdef UAC_V1
    ULONG Pcm;      /* render: the audio bytes of Bytes (26.12, MaxPacketsOnly) */
#endif
} USB_SLOT;

/* A clock entity a running stream depends on: a source at a rate, or a
 * selector at an input. Streams share an entity only at the same value. */
typedef struct CLAIM { UAC_BYTE Id; ULONG Value, Users; } CLAIM;

/* 26.13: the worker's one frame request, sent and not waited for. Its URB
 * is the stream's, kept until the request is back (Phase 2); the worker
 * never exits while one is with the stack. */
typedef struct FRAME_REQ {
    PIRP Irp;
    struct _URB_GET_CURRENT_FRAME_NUMBER Urb;
    LONG Phase;             /* 0 none, 1 with the stack, 2 back and not yet taken */
    NTSTATUS Status;
    BOOLEAN Cancelled, ForResync;
    ULONGLONG SentAt;       /* interrupt time */
    ULONG AgeMs;            /* sent to taken, set by FrameTake */
} FRAME_REQ;

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
    ULONG DataSlots, TransferUs, QueueRead, QueueMs; /* 19.4, fixed at Create */
    ULONG RealtimeRead, Realtime;   /* 19.6, set before Start */
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
    ULONG LostRun, LostLimit; /* section 25: lost data URBs in a row, and the most ridden out */
    ULONG FbRestartRun;       /* 25.7: feedback URBs refused for their start frame, in a row */
    BOOLEAN FeedbackOff;      /* 25.7: feedback polling stopped for the rest of the stream */
    UAC_RECOVERY Rec;         /* 26.13: Resync (slots held until all are back), ResyncRun, LagSuspect */
    ULONGLONG ResyncSince;    /* 26.13: interrupt time the hold began (0: none) */
    FRAME_REQ Frame;          /* 26.13 */
    UAC_WATCH_TICK LastFeedbackPacket, LastFeedbackValueAt;
    ULONG FeedbackUnusableUs, FeedbackQuietUs, FeedbackPacketUs; /* section 25: the deadline in bus time */
    ULONG DoneCount, QuietFrom, FeedbackStamp; /* section 25: completions, the last usable and the newest feedback's stamps */
    UAC_RATE Rate_;
    UAC_USB_STATS Stats;
#ifdef UAC_V1
    /* docs/design/uac2.md 26.12: an adaptive source's synch OUT endpoint
     * fed Ff (the feedback slots, sending), MaxPacketsOnly either way, and
     * the nominal cadence a MaxPacketsOnly IN packet is counted against. */
    BOOLEAN Feedforward, MpoOut, MpoIn, FfDelivered; /* FfDelivered: 26.13 */
    ULONG FfValue, FfRun;
    UAC_CONV_CADENCE Cadence;
#endif
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
    TR_LINE(("busif", (ULONG)status, (ULONG)bus.Size << 16 | bus.Version, bus.IsHighSpeed ? 1 : 0));
    if (!NT_SUCCESS(status)) return status;
    /* Answered, but not with an interface this module can use: refused. */
    status = STATUS_DEVICE_CONFIGURATION_ERROR;
    if (bus.Size >= sizeof(bus) && bus.Version >= 1 && bus.IsHighSpeed && bus.Dereference) {
        *high = bus.IsHighSpeed(bus.BusContext) ? TRUE : FALSE;
        status = STATUS_SUCCESS;
    } else if (bus.Version == 0 && bus.Size >= FIELD_OFFSET(USB_SPEED_INTERFACE, IsHighSpeed) &&
               bus.Dereference) {
        /* A version 0 interface (no IsHighSpeed, which version 1 added
         * with USB 2.0): ME's own hub answers so (32 bytes, 2026-10-30).
         * Reported as no answer, for UacUsbOpen's 9x rule. */
        status = STATUS_NOT_SUPPORTED;
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

#ifdef UAC_V1
/* USB Audio 1.0: the rates the terminal's alternates list, no request: a
 * discrete list as given, a continuous range as the common rates in it. */
NTSTATUS UacUsbRates(UAC_DEV *d, UAC_BYTE terminal, ULONG *rates, ULONG max, ULONG *count)
{
    ULONG i, k;
    if (!d || !rates || !count) return STATUS_INVALID_PARAMETER;
    *count = 0;
    for (i = 0; i < d->Desc.AltCount; i++) {
        const UAC_ALT *a = &d->Desc.Alt[i];
        if (a->Terminal != terminal) continue;
        if (a->RateCount)
            for (k = 0; k < a->RateCount && k < UAC1_RATES; k++) AddRate(rates, max, count, a->Rate[k]);
        else
            for (k = 0; k < sizeof(CommonRates) / sizeof(CommonRates[0]); k++)
                if (CommonRates[k] >= a->RateMin && CommonRates[k] <= a->RateMax)
                    AddRate(rates, max, count, CommonRates[k]);
    }
    return *count ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}
#else
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
#endif

int UacUsbHighSpeed(UAC_DEV *d) { return d && d->HighSpeed; }

#ifdef UAC_V1
NTSTATUS UacUsbBusSpeed(PDEVICE_OBJECT lower, BOOLEAN *high)
{
    if (!lower || !high || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_PARAMETER;
    return BusSpeed(lower, high);
}
#endif

NTSTATUS UacUsbDescriptor(PDEVICE_OBJECT lower, UCHAR type, UCHAR index, USHORT language,
                          PVOID buffer, USHORT size, ULONG *got)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST urb;
    NTSTATUS status;
    *got = 0;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    urb.TransferBufferLength = size;
    urb.TransferBuffer = buffer;
    urb.DescriptorType = type;
    urb.Index = index;
    urb.LanguageId = language;
    status = UsbRequest(lower, (PURB)&urb);
    if (NT_SUCCESS(status)) *got = urb.TransferBufferLength > size ? size : urb.TransferBufferLength;
    TR_LINE(("desc", (ULONG)type << 16 | index, (ULONG)status, *got));
    return status;
}

NTSTATUS UacUsbClassRequest(UAC_DEV *d, const UCHAR *setup, PVOID bytes, ULONG *length)
{
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST urb;
    NTSTATUS status;
    if (!d || !setup || !length || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        (setup[0] & 0x7f) != 0x21 || *length != ((ULONG)setup[6] | ((ULONG)setup[7] << 8)) ||
        (*length && !bytes))
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_CLASS_INTERFACE;
    urb.TransferFlags = setup[0] & 0x80 ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : 0;
    urb.TransferBuffer = bytes; urb.TransferBufferLength = *length;
    urb.Request = setup[1];
    urb.Value = (USHORT)(setup[2] | (setup[3] << 8));
    urb.Index = (USHORT)(setup[4] | (setup[5] << 8));
    KeWaitForSingleObject(&d->Lock, Executive, KernelMode, FALSE, NULL);
    status = UsbRequest(d->Lower, (PURB)&urb);
    KeReleaseMutex(&d->Lock, FALSE);
    *length = urb.TransferBufferLength;
    return status;
}

/* Optional read-only audibility diagnostics: the first feature unit's
 * master mute/volume. No mute or volume write. */
static VOID ReadFeature(UAC_USB *s)
{
    UAC_DEV *d = s->Dev;
    UCHAR bytes[2], controls = 0, id = 0;
    ULONG size, i;
    NTSTATUS status;
    s->Stats.FeatureStatus = STATUS_NOT_SUPPORTED;
#ifdef UAC_V1
    /* UAC 2.0 Feature Unit layouts and requests: not read for UAC 1.0. */
    return;
#endif
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

#ifdef UAC_V1
/* USB Audio 1.0 has no clock entity: the rate is the data endpoint's
 * Sampling Frequency control, set after the alternate is selected
 * (EndpointRate). */
static NTSTATUS SetClock(UAC_USB *s)
{
    UNREFERENCED_PARAMETER(s);
    return STATUS_SUCCESS;
}

/* ADC-1 5.2.3.2.3.1: SET_CUR (0x01) of SAMPLING_FREQ_CONTROL (0x01) to the
 * data endpoint, 3 bytes, where its class-specific endpoint descriptor
 * sets D0 (an endpoint without it runs its one rate). Then GET_CUR (0x81),
 * advisory: many devices stall it; a different answer is recorded in
 * ClockHz and the stream runs at the asked rate, as the specification has
 * a discrete endpoint round only a rate it does not list. */
static NTSTATUS EndpointRate(UAC_USB *s)
{
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST urb;
    UCHAR b[3];
    NTSTATUS status;
    s->Stats.ClockHz = s->Rate;
    if (!(s->Alt.EpControls & 1)) return STATUS_SUCCESS;
    b[0] = (UCHAR)s->Rate; b[1] = (UCHAR)(s->Rate >> 8); b[2] = (UCHAR)(s->Rate >> 16);
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_CLASS_ENDPOINT;
    urb.TransferFlags = 0;
    urb.TransferBuffer = b; urb.TransferBufferLength = 3;
    urb.Request = 0x01;
    urb.Value = 0x0100;
    urb.Index = s->Alt.DataEndpoint;
    KeWaitForSingleObject(&s->Dev->Lock, Executive, KernelMode, FALSE, NULL);
    status = UsbRequest(s->Dev->Lower, (PURB)&urb);
    if (NT_SUCCESS(status)) {
        RtlZeroMemory(&urb, sizeof(urb));
        RtlZeroMemory(b, sizeof(b));
        urb.Hdr.Length = sizeof(urb);
        urb.Hdr.Function = URB_FUNCTION_CLASS_ENDPOINT;
        urb.TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
        urb.TransferBuffer = b; urb.TransferBufferLength = 3;
        urb.Request = 0x81;
        urb.Value = 0x0100;
        urb.Index = s->Alt.DataEndpoint;
        if (NT_SUCCESS(UsbRequest(s->Dev->Lower, (PURB)&urb)) && urb.TransferBufferLength == 3)
            s->Stats.ClockHz = (ULONG)b[0] | ((ULONG)b[1] << 8) | ((ULONG)b[2] << 16);
    }
    KeReleaseMutex(&s->Dev->Lock, FALSE);
    return status;
}
#else
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
#endif

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

#if defined(UAC_TRACE_E9)
/* The bus's current frame, PASSIVE only; 0xffffffff when the stack fails it. */
static ULONG TrFrame(UAC_USB *s)
{
    struct _URB_GET_CURRENT_FRAME_NUMBER frame;
    RtlZeroMemory(&frame, sizeof(frame));
    frame.Hdr.Length = sizeof(frame);
    frame.Hdr.Function = URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !NT_SUCCESS(UsbRequest(s->Dev->Lower, (PURB)&frame)))
        return 0xffffffff;
    return frame.FrameNumber;
}

/* One line: every slot's phase (0 idle, 1 with the stack, 2 completed),
 * '.' for an unused slot. */
static VOID TrPhases(UAC_USB *s, const char *why, ULONG a)
{
    ULONG i;
    TrStr("u2: "); TrDec(TrNow()); TrByte(' '); TrStr(why); TrByte(' '); TrHex(a);
    TrStr(" cs="); TrDec(s->CompletedSequence); TrStr(" ns="); TrDec(s->NextSequence);
    TrStr(" stop="); TrDec(s->Stop); TrStr(" ph=");
    for (i = 0; i < SLOTS; i++) TrByte((char)(s->Slot[i].Used ? '0' + s->Slot[i].Phase : '.'));
    TrStr(" seq=");
    for (i = 0; i < SLOTS; i++)
        if (s->Slot[i].Used && !s->Slot[i].Feedback) { TrDec(s->Slot[i].Sequence); TrByte(','); }
    TrByte('\n');
}
#endif

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
    TR_LINE(("alt", (ULONG)s->Alt.Interface << 16 | urb->Interface.AlternateSetting, (ULONG)status,
             (ULONG)urb->Hdr.Status));
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
#ifdef UAC_V1
        if (active && s->Feedforward && !s->FeedbackPipe) status = STATUS_DEVICE_CONFIGURATION_ERROR;
#endif
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
    slot->DoneStamp = ++s->DoneCount;
    /* The sound driver's wait increment: on NT the woken worker runs
     * boosted (at most 15, decaying) ahead of normal-priority threads
     * (docs/design/uac2.md 19.6). */
    KeSetEvent(&s->Wake, IO_SOUND_INCREMENT, FALSE);
    KeReleaseSpinLock(&s->Lock, old);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* asap FALSE: at the explicit frame start (26.13's resync); else ASAP.
 * frameValid: frame is the bus frame the worker read before this
 * submission (26.13's lag test). */
static NTSTATUS SubmitSlot(UAC_USB *s, USB_SLOT *slot, BOOLEAN asap, ULONG start,
                           BOOLEAN frameValid, ULONG frame)
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
#ifdef UAC_V1
    /* 26.12: an adaptive source's synch slots send. */
    urb->TransferFlags = USBD_START_ISO_TRANSFER_ASAP |
        ((slot->Feedback ? !s->Feedforward : s->Capture) ?
         USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : 0);
#else
    urb->TransferFlags = USBD_START_ISO_TRANSFER_ASAP |
        (slot->Feedback || s->Capture ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : 0);
#endif
    if (!asap) {
        urb->TransferFlags &= ~USBD_START_ISO_TRANSFER_ASAP;
        urb->StartFrame = start;
    }
    urb->NumberOfPackets = slot->Packets;
    urb->TransferBuffer = slot->Buffer;
    for (i = 0; i < slot->Packets; i++) {
        if (slot->Feedback) n = s->FeedbackBytes;
        else if (s->Capture) n = s->Capacity;
        else n = UacRateNextBytes(&s->Rate_);
        /* A zero-length OUT packet is legal: a cadence below one frame per
         * service interval (8 kHz at High Speed) has empty intervals. */
        if (n > (slot->Feedback ? s->FeedbackBytes : s->Capacity) || size > slot->BufferBytes - n)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        urb->IsoPacket[i].Offset = size;
#ifdef UAC_V1
        /* 26.12, ADC-1 4.6.1.2: a MaxPacketsOnly OUT packet that is not
         * null is wMaxPacketSize long, its audio first; the audio length
         * waits in Length (unused for OUT) until the fill below. */
        if (s->MpoOut && !slot->Feedback && n) {
            if (size > slot->BufferBytes - s->Capacity) return STATUS_DEVICE_CONFIGURATION_ERROR;
            urb->IsoPacket[i].Length = n;
            n = s->Capacity;
        }
#endif
        size += n;
    }
    urb->TransferBufferLength = size;
    slot->Bytes = size;
    RtlZeroMemory(slot->Buffer, size); /* playback: defensive underrun silence */
#ifdef UAC_V1
    slot->Pcm = size;
    if (slot->Feedback && s->Feedforward) {
        /* Ff in every packet: 3 bytes 10.14, or 4 bytes 16.16 (26.12). */
        for (i = 0; i < slot->Packets; i++) {
            ULONG k, at = urb->IsoPacket[i].Offset;
            for (k = 0; k < s->FeedbackBytes; k++) slot->Buffer[at + k] = (UCHAR)(s->FfValue >> (8 * k));
        }
    } else if (!slot->Feedback && !s->Capture && s->MpoOut) {
        slot->Pcm = 0;
        for (i = 0; i < slot->Packets; i++) {
            n = urb->IsoPacket[i].Length;
            urb->IsoPacket[i].Length = 0;
            if (!n) continue;
            s->Fill(s->CallbackContext, slot->Buffer + urb->IsoPacket[i].Offset, n);
            slot->Pcm += n;
        }
        for (i = 0; i < size; i++) if (slot->Buffer[i]) nonzero++;
    } else
#endif
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
    /* 26.13: the frame read before this submission, so the completion can
     * tell a URB the stack placed behind the bus from one it held or the
     * controller skipped. Read earlier than the call, it can only make a
     * URB look less behind. */
    slot->SubmitFrameValid = !slot->Feedback && asap && frameValid;
    slot->SubmitFrame = frame;
    TR_EVENT_(('S', (ULONG)(slot - s->Slot), slot->Feedback ? 0xffffffff : s->NextSequence, slot->Packets,
               size, TrFrame(s), urb->TransferFlags, (ULONG)urb->StartFrame));
    /* Published as the stack's only here, a Stop seen last, and passed on
     * at once: nothing waits between. */
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
    TR_DUMP(("fail", (ULONG)status, s->CompletedSequence, s->NextSequence));
}

/* The packets that report a failure of their own. */
static ULONG FailedPackets(USB_SLOT *slot)
{
    ULONG i, failed = 0;
    for (i = 0; i < slot->Packets; i++)
        if (!USBD_SUCCESS(slot->Urb->IsoPacket[i].Status)) failed++;
    return failed;
}

static ULONG PacketErrors(USB_SLOT *slot)
{
    ULONG errors = FailedPackets(slot);
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
        else if (packet->Length == 4 || (!s->Dev->HighSpeed && packet->Length == 3))
            s->Stats.FbFourBytes++; /* a well-formed length; the name is older */
        else if (!packet->Length) s->Stats.FbZeroLength++;
        else FbCount(s, 0x40000000 | packet->Length);
    }
}

static NTSTATUS FrameDone(PDEVICE_OBJECT device, PIRP irp, PVOID context)
{
    UAC_USB *s = context;
    KIRQL old;
    UNREFERENCED_PARAMETER(device);
    KeAcquireSpinLock(&s->Lock, &old);
    s->Frame.Status = irp->IoStatus.Status;
    s->Frame.Phase = 2;
    KeSetEvent(&s->Wake, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&s->Lock, old);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static LONG FramePhase(UAC_USB *s)
{
    KIRQL old;
    LONG phase;
    KeAcquireSpinLock(&s->Lock, &old); phase = s->Frame.Phase; KeReleaseSpinLock(&s->Lock, old);
    return phase;
}

/* 26.13, the worker only: sends a frame request and returns without
 * waiting for it (FrameDone wakes the worker). FALSE when one is still
 * out or back and not taken (its storage is not reused before FrameTake),
 * or no IRP could be had. */
static BOOLEAN FrameSend(UAC_USB *s, BOOLEAN forResync)
{
    PIO_STACK_LOCATION stack;
    KIRQL old;
    PIRP irp;
    if (FramePhase(s)) return FALSE;
    irp = IoAllocateIrp(s->Dev->Lower->StackSize, FALSE);
    if (!irp) return FALSE;
    RtlZeroMemory(&s->Frame.Urb, sizeof(s->Frame.Urb));
    s->Frame.Urb.Hdr.Length = sizeof(s->Frame.Urb);
    s->Frame.Urb.Hdr.Function = URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    stack = IoGetNextIrpStackLocation(irp);
    RtlZeroMemory(stack, sizeof(*stack));
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = &s->Frame.Urb;
    IoSetCompletionRoutine(irp, FrameDone, s, TRUE, TRUE, TRUE);
    s->Frame.Irp = irp;
    s->Frame.Cancelled = FALSE;
    s->Frame.ForResync = forResync;
    s->Frame.SentAt = KeQueryInterruptTime();
    KeAcquireSpinLock(&s->Lock, &old); s->Frame.Phase = 1; KeReleaseSpinLock(&s->Lock, old);
    (void)IoCallDriver(s->Dev->Lower, irp);
    return TRUE;
}

/* The worker only: a request that is back is taken (its IRP freed): TRUE,
 * with *ok, the frame and what it was sent for. FALSE when none is back. */
static BOOLEAN FrameTake(UAC_USB *s, BOOLEAN *ok, ULONG *frame, BOOLEAN *forResync)
{
    KIRQL old;
    NTSTATUS status;
    KeAcquireSpinLock(&s->Lock, &old);
    if (s->Frame.Phase != 2) { KeReleaseSpinLock(&s->Lock, old); return FALSE; }
    status = s->Frame.Status;
    KeReleaseSpinLock(&s->Lock, old);
    IoFreeIrp(s->Frame.Irp); s->Frame.Irp = NULL;
    *ok = NT_SUCCESS(status) && USBD_SUCCESS(s->Frame.Urb.Hdr.Status) ? TRUE : FALSE;
    *frame = s->Frame.Urb.FrameNumber;
    *forResync = s->Frame.ForResync;
    s->Frame.AgeMs = (ULONG)((KeQueryInterruptTime() - s->Frame.SentAt) / 10000);
    KeAcquireSpinLock(&s->Lock, &old); s->Frame.Phase = 0; KeReleaseSpinLock(&s->Lock, old);
    TR_LINE(("frameq", (ULONG)status, *frame, s->Frame.AgeMs << 2 | (ULONG)*forResync << 1 | s->Frame.Cancelled));
    return TRUE;
}

static VOID Completed(UAC_USB *s, USB_SLOT *slot)
{
    KIRQL old;
    ULONG i, j, raw, errors = PacketErrors(slot);
    BOOLEAN stop, valid = FALSE, good, lost, restart = FALSE, fbOff = FALSE;
    unsigned int refused = UAC_FB_NONE;
    NTSTATUS status = slot->Status;
    USBD_STATUS usbStatus = slot->Urb->Hdr.Status;
#if defined(UAC_TRACE_E9)
    {
        ULONG k, first = 0xffff;
        for (k = 0; k < slot->Packets; k++)
            if (!USBD_SUCCESS(slot->Urb->IsoPacket[k].Status)) { first = k; break; }
        TrEvent('C', (ULONG)(slot - s->Slot), slot->Feedback ? 0xffffffff : slot->Sequence, slot->Packets,
                (ULONG)status, (ULONG)usbStatus, (ULONG)slot->Urb->StartFrame, TrFrame(s));
        if (first != 0xffff || errors)
            TrEvent('P', (ULONG)(slot - s->Slot), first, slot->Packets,
                    first != 0xffff ? (ULONG)slot->Urb->IsoPacket[first].Status : 0,
                    first != 0xffff ? slot->Urb->IsoPacket[first].Length : 0,
                    errors << 16 | first, (ULONG)slot->Urb->ErrorCount);
    }
#endif
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
    /* Section 25: a URB the stack failed whole (every packet not accessed
     * or late, as a host controller that fell behind its schedule leaves
     * them) is a lost service interval, as a failed packet is: counted,
     * its time passed, the stream going on. Only a run of them covering
     * UAC_LOST_LIMIT_MS of data stops it. */
    lost = !stop && status != STATUS_CANCELLED &&
        (UacPlanIsoLost((ULONG)usbStatus, slot->Packets, FailedPackets(slot)) ||
         /* 25.8: a data URB refused whole for its start frame, likewise */
         (!slot->Feedback && UacPlanDataRefused((ULONG)usbStatus, slot->Packets)));
#ifdef UAC_V1
    if (slot->Feedback && s->Feedforward) {
        /* 26.12: a synch URB to an adaptive source. A failure is counted
         * and the slot sent again; 8 in a row end the stream, as a lost
         * feedback does (26.10, U4): a source waiting for Ff would leave
         * a recording that never advances. */
        BOOLEAN end = FALSE;
        ULONG failed = FailedPackets(slot);
        /* 26.13: a synch URB with some packets delivered ends the run. Once
         * Ff has reached the source it runs at the last Ff it was sent, so a
         * failed synch URB is a lost interval: counted, never ending the
         * stream (the data's own rules still do). Before the first delivery
         * the run of 8 stands: a source waiting for Ff never starts. A
         * refused start frame marks the resync either way. */
        KeAcquireSpinLock(&s->Lock, &old);
        s->Stats.FeedbackUrbs++;
        if (!stop && status != STATUS_CANCELLED) {
            if (good && failed < slot->Packets) {
                s->FfRun = 0; s->FfDelivered = TRUE; s->Stats.GoodFeedback++;
                if (failed) FbCount(s, 0x10000000UL);   /* some packets failed */
            } else {
                s->Stats.BadFeedback++;
                FbCount(s, (ULONG)usbStatus | (s->FfDelivered ? 0x10000000UL : 0));
                if (!NT_SUCCESS(status)) FbCount(s, (ULONG)status);
                if (!s->FfDelivered) end = ++s->FfRun >= UAC_FB_RESTART_LIMIT;
                if (usbStatus == (USBD_STATUS)UAC_ISO_BAD_START_STATUS) s->Rec.Resync = 1;
            }
        }
        KeReleaseSpinLock(&s->Lock, old);
        slot->Phase = 0;
        if (end) {
            TR_LINE(("fflost", s->FfRun, (ULONG)usbStatus, (ULONG)status));
            Fail(s, NT_SUCCESS(status) ? STATUS_DEVICE_DATA_ERROR : status);
        }
        return;
    }
#endif
    /* Design 25.7: a feedback URB the stack refused for its start frame
     * is passed over a few times in a row; data keeps the rules above. */
    if (slot->Feedback && !stop && status != STATUS_CANCELLED) {
        refused = UacPlanFbRefused((ULONG)usbStatus, s->FbRestartRun);
        restart = refused == UAC_FB_RESUBMIT;
        fbOff = refused == UAC_FB_OFF;
        if (refused != UAC_FB_NONE) s->FbRestartRun++;
        else if (good) s->FbRestartRun = 0;
    }
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
                if (!USBD_SUCCESS(packet->Status)) { s->Rate_.BadFeedback++; continue; }
                /* The value is decoded by the length that came back (Full
                 * Speed: 3 bytes 10.14 or 4 bytes 16.16), within the
                 * packet's stride and the slot's allocation. */
                switch (UacRateFeedbackPacket(&s->Rate_, slot->Buffer,
                        slot->Bytes <= slot->BufferBytes ? slot->Bytes : slot->BufferBytes,
                        packet->Offset, packet->Length, s->FeedbackBytes)) {
                case UAC_FEEDBACK_KEEPALIVE:
                    valid = TRUE;
                    KeAcquireSpinLock(&s->Lock, &old);
                    s->Stats.FeedbackKeepalives++;
                    KeReleaseSpinLock(&s->Lock, old);
                    break;
                case UAC_FEEDBACK_VALUE:
                    valid = TRUE;
                    s->LastFeedbackValueAt = KeQueryInterruptTime();
                    break;
                case UAC_FEEDBACK_EMPTY:
                    /* Section 25.6: before the first value, an empty answer
                     * shows the endpoint serviced; the stream goes on at
                     * the nominal cadence, as it does until a value. */
                    valid = TRUE;
                    break;
                default:
                    /* 'F': a rejected feedback packet (A its first 4 bytes, B
                     * its length, C its offset, D the stride). */
                    TR_EVENT_(('F', (ULONG)(slot - s->Slot), i, slot->Packets,
                               packet->Length && packet->Offset + 4 <= slot->BufferBytes ?
                               *(ULONG UNALIGNED *)(slot->Buffer + packet->Offset) : 0,
                               packet->Length, packet->Offset, s->FeedbackBytes));
                    break;
                }
            }
        }
        if (lost) s->Rate_.BadFeedback += slot->Packets;
        /* Section 25: the deadline counts the service time of the URBs
         * that came back with nothing usable, not the time since the last
         * usable packet: a time in which the stack held every URB and none
         * came back is not the device's. The feedback slots are retired by
         * index, so one that came back before the newest already retired
         * one leaves the accounting alone. */
        if (UacFeedbackAfter(slot->DoneStamp, s->FeedbackStamp)) {
            s->FeedbackStamp = slot->DoneStamp;
            if (valid) {
                s->LastFeedbackPacket = KeQueryInterruptTime();
                s->FeedbackUnusableUs = s->FeedbackQuietUs = 0;
                s->QuietFrom = slot->DoneStamp;
            } else if (!stop && status != STATUS_CANCELLED)
                s->FeedbackUnusableUs = UacFeedbackAdd(s->FeedbackUnusableUs, slot->Packets, s->FeedbackPacketUs);
        }
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
#ifdef UAC_V1
                /* 26.12: every interval's nominal frames are owed; a
                 * failed or null packet starts the balance afresh. */
                if (s->MpoIn) {
                    (void)UacConvNominal(&s->Cadence);
                    if (!USBD_SUCCESS(packet->Status) || !length ||
                        length > s->Capacity || packet->Offset > slot->Bytes - length)
                        UacConvLost(&s->Cadence);
                }
#endif
                if (!USBD_SUCCESS(packet->Status) || !length) continue;
                if (length > s->Capacity || packet->Offset > slot->Bytes - length) continue;
                length -= length % s->FrameBytes;
#ifdef UAC_V1
                /* ADC-1 4.6.1.2: the padding after a MaxPacketsOnly
                 * source's audio is discarded (the counting rule of 26.12;
                 * PCM and IEEE_FLOAT only, where zero bytes are silence). */
                if (s->MpoIn)
                    length = UacConvMpoFrames(&s->Cadence, slot->Buffer + packet->Offset, length,
                                              s->FrameBytes, NULL) * s->FrameBytes;
#endif
                if (length) s->Captured(s->CallbackContext, slot->Buffer + packet->Offset, length);
                delivered += length;
            }
            KeAcquireSpinLock(&s->Lock, &old);
            s->Stats.CapturedBytes += delivered; s->Stats.CompletedBytes += delivered;
            KeReleaseSpinLock(&s->Lock, old);
            s->Progress(s->CallbackContext, 0, errors);
        } else if (lost && s->Capture) {
#ifdef UAC_V1
            /* 26.12: the transfer's intervals pass all the same. */
            if (s->MpoIn) UacConvLostPackets(&s->Cadence, slot->Packets);
#endif
            s->Progress(s->CallbackContext, 0, errors);
        } else if (good || lost) {
            /* Advance in submission order, including failed/lost scheduled
             * packets. Never retry time-specific PCM or synthesize a timer. */
#ifdef UAC_V1
            s->Progress(s->CallbackContext, slot->Pcm, errors);  /* audio bytes (26.12) */
#else
            s->Progress(s->CallbackContext, slot->Bytes, errors);
#endif
        }
        if (lost) s->LostRun++;
        else if (good) s->LostRun = 0;
        {
            /* 26.13: a refusal wants a resync at explicit frames. A URB
             * lost whole, or a recording's URB back successful and empty,
             * that the stack had placed on frames already past when it was
             * submitted: its ASAP chain lags the bus, and only a resync ends
             * that sooner than its refusal a frame list later; the first
             * such URB arms the submission frames. Only a URB that carried
             * audio (render: any good one) clears the resync limit's run and
             * the lag test: 98 SE's own UHCI returns a recording's URBs
             * placed behind the bus successful and empty between its
             * refusals. */
            unsigned int kind = UAC_DATA_OTHER, behind = 0;
            if (lost && UacPlanDataRefused((ULONG)usbStatus, slot->Packets)) kind = UAC_DATA_REFUSED;
            else if (lost) kind = UAC_DATA_LOST;
            else if (good) kind = !s->Capture || delivered ? UAC_DATA_AUDIO : UAC_DATA_EMPTY;
            if ((kind == UAC_DATA_LOST || kind == UAC_DATA_EMPTY) && slot->SubmitFrameValid)
                behind = UacPlanBehind(slot->SubmitFrame, slot->Urb->StartFrame,
                                       UacPlanSpanFrames(s->TransferUs), UAC_RESYNC_LEAD);
            if (UacPlanDataDone(&s->Rec, kind, behind)) {
                TR_LINE(("behind", slot->SubmitFrame, (ULONG)slot->Urb->StartFrame, slot->Sequence));
                KeAcquireSpinLock(&s->Lock, &old); s->Stats.ResyncBehind++; KeReleaseSpinLock(&s->Lock, old);
            }
        }
        /* Section 25: bus time since the last usable feedback, also while
         * feedback URBs are held or come back unusable. */
        if (s->Feedback && (good || lost) && UacFeedbackAfter(slot->DoneStamp, s->QuietFrom))
            s->FeedbackQuietUs = UacFeedbackAdd(s->FeedbackQuietUs, 1, s->TransferUs);
        s->CompletedSequence++;
    }
    slot->Phase = 0;
    if (lost) {
        if (!slot->Feedback && s->LostRun >= s->LostLimit)
            Fail(s, NT_SUCCESS(status) ? STATUS_DEVICE_DATA_ERROR : status);
    } else if (restart) {
        /* passed over: the slot is resubmitted at the resync (26.13) */
        s->Rec.Resync = 1;
    } else if (fbOff) {
        /* 25.7: no more feedback polls in this stream; the cadence keeps
         * its last good value (or the nominal one) and the deadline is off. */
        KeAcquireSpinLock(&s->Lock, &old);
        s->FeedbackOff = TRUE; s->Stats.FeedbackOff = 1;
        KeReleaseSpinLock(&s->Lock, old);
        TR_LINE(("fboff", s->FbRestartRun, (ULONG)usbStatus, 0));
    } else if (!stop && !NT_SUCCESS(status)) Fail(s, status);
    else if (!stop && !USBD_SUCCESS(usbStatus)) Fail(s, STATUS_DEVICE_DATA_ERROR);
}

/* Design 26.13: the worker, once no slot is with the stack after a URB was
 * refused for its start frame, has read the bus's frame (its own request,
 * back: ok and frame) with no Stop seen since, and submits every slot of
 * each pipe at explicit frames past it (UacPlanResyncFrame); ASAP after
 * that. UAC_RESYNC_LIMIT resyncs with no data URB that carried audio between
 * them, or a failed frame request or submission, end the stream. A Stop
 * wins (SubmitSlot sees it last, under the lock). */
static VOID Resync(UAC_USB *s, BOOLEAN ok, ULONG frame)
{
    ULONG i, k = 0, kf = 0, start;
    NTSTATUS status;
    KIRQL old;
    if (!ok) {
        TR_LINE(("rsyframe", 0, 0, 0));
        Fail(s, STATUS_DEVICE_DATA_ERROR);
        return;
    }
    if (!UacPlanResyncStart(&s->Rec)) {
        TR_LINE(("rsylimit", s->Rec.ResyncRun, s->CompletedSequence, 0));
        Fail(s, STATUS_DEVICE_DATA_ERROR);
        return;
    }
    s->ResyncSince = 0; /* the next hold has its own deadline */
    KeAcquireSpinLock(&s->Lock, &old);
    s->Stats.Resyncs++; s->Stats.ResyncFrame = frame;
    KeReleaseSpinLock(&s->Lock, old);
    TR_LINE(("resync", frame, s->Rec.ResyncRun, s->CompletedSequence));
    for (i = 0; i < SLOTS; i++) {
        USB_SLOT *slot = &s->Slot[i];
        if (!slot->Used || (slot->Feedback && s->FeedbackOff)) continue;
        start = slot->Feedback ?
            UacPlanResyncFrame(frame, kf++, UacPlanSpanFrames(slot->Packets * s->FeedbackPacketUs)) :
            UacPlanResyncFrame(frame, k++, UacPlanSpanFrames(s->TransferUs));
        status = SubmitSlot(s, slot, FALSE, start, FALSE, 0);
        if (status == STATUS_DELETE_PENDING) return; /* SubmitSlot saw Stop */
        if (!NT_SUCCESS(status)) {
            TR_LINE(("rsysub", i, (ULONG)status, 0));
            Fail(s, status);
            return;
        }
    }
}

static VOID Worker(PVOID context)
{
    UAC_USB *s = context;
    ULONG i, pending, frame = 0;
    KIRQL old;
    BOOLEAN stop, progress, taken, ok = FALSE, forResync = FALSE, frameOut;
    LONG phase;
    NTSTATUS status;
    unsigned int gate;
    LARGE_INTEGER timeout;
    KPRIORITY basePriority = 0;
    LARGE_INTEGER hz;
    ULONGLONG windowStart, busy, runStart, waitStart, now, window, shortWait;
    unsigned int demoted = 0, closed, next;
    BOOLEAN realtime;
    /* 98 SE: waiting on a thread object faulted there (usbmidi's stop path,
     * 2026-10-22), so the stop waits for Exited; 2000 and later also wait
     * the thread object so the image cannot unload under it. */
    s->Thread = PsGetCurrentThread();
    if (s->WaitThread) ObReferenceObject(s->Thread);
    /* 19.6: the refill and resubmit run ahead of every normal-class thread
     * (a game's included, whatever its priority class), as the user report
     * of 2026-11-04 asked: lowering the game's priority cured the pops. The
     * work per wake is bounded (the slots, then a wait), so the worker
     * never holds the processor. LOW_REALTIME_PRIORITY is the lowest of the
     * real-time range; 98 SE's NTKERN exports the name (xHCI98 record 13,
     * row 114), its effect there is read by the legs. */
    /* The worker's own copy: a RUN that changes s->Realtime later applies
     * to the next worker, never to this one's priority or fairness. */
    realtime = s->Realtime ? TRUE : FALSE;
    if (realtime) {
        basePriority = KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
        s->Stats.WorkerPriority = LOW_REALTIME_PRIORITY;
    }
    KeSetEvent(&s->Ready, IO_NO_INCREMENT, FALSE);
    windowStart = runStart = (ULONGLONG)KeQueryPerformanceCounter(&hz).QuadPart;
    window = UacPlanCounts((ULONGLONG)hz.QuadPart, UAC_DUTY_WINDOW_MS, 1000UL);
    shortWait = UacPlanCounts((ULONGLONG)hz.QuadPart, UAC_DUTY_SHORT_WAIT_US, 1000000UL);
    busy = 0;
    for (;;) {
        KeClearEvent(&s->Wake);
        /* Feedback first so newly prepared data uses the newest valid rate,
         * in the order the feedback URBs came back (section 25: the oldest
         * stamp first). Data completions are retired in sequence even if
         * callbacks reorder. */
        do {
            USB_SLOT *oldest = NULL;
            ULONG oldestStamp = 0, stamp;
            progress = FALSE;
            /* One look under the lock: no completion lands between two
             * slots' reads and makes a newer one look the oldest. */
            KeAcquireSpinLock(&s->Lock, &old);
            for (i = 0; i < SLOTS; i++) {
                USB_SLOT *slot = &s->Slot[i];
                if (!slot->Used || !slot->Feedback) continue;
                phase = slot->Phase; stamp = slot->DoneStamp;
                if (phase == 2 && (!oldest || UacFeedbackAfter(oldestStamp, stamp))) {
                    oldest = slot; oldestStamp = stamp;
                }
            }
            KeReleaseSpinLock(&s->Lock, old);
            if (oldest) { Completed(s, oldest); progress = TRUE; continue; }
            for (i = 0; i < SLOTS; i++) {
                USB_SLOT *slot = &s->Slot[i];
                if (!slot->Used || slot->Feedback) continue;
                KeAcquireSpinLock(&s->Lock, &old); phase = slot->Phase; KeReleaseSpinLock(&s->Lock, old);
                if (phase == 2 && slot->Sequence == s->CompletedSequence) {
                    Completed(s, slot); progress = TRUE;
                }
            }
        } while (progress);
        /* 26.13: a frame request back is taken first, so the Stop read
         * below is read after it: a frame is never used past a Stop. What
         * this pass does not use is dropped (a frame is read afresh). */
        taken = FrameTake(s, &ok, &frame, &forResync);
        KeAcquireSpinLock(&s->Lock, &old); stop = s->Stop; KeReleaseSpinLock(&s->Lock, old);
        /* A request with the stack is cancelled at a stop, and a submission's
         * past UAC_FRAME_WAIT_MS; its storage is kept until it is back. */
        if (FramePhase(s) == 1 &&
            UacPlanFrameCancel(stop, s->Frame.Cancelled, s->Frame.ForResync,
                               (ULONG)((KeQueryInterruptTime() - s->Frame.SentAt) / 10000))) {
            s->Frame.Cancelled = TRUE;
            (void)IoCancelIrp(s->Frame.Irp);
        }
        pending = 0;
        for (i = 0; i < SLOTS; i++) {
            USB_SLOT *slot = &s->Slot[i];
            BOOLEAN useFrame = FALSE;
            if (!slot->Used) continue;
            KeAcquireSpinLock(&s->Lock, &old); phase = slot->Phase; KeReleaseSpinLock(&s->Lock, old);
            if (stop && phase == 1 && !slot->CancelIssued) {
                slot->CancelIssued = TRUE;
                (void)IoCancelIrp(slot->Irp);
            }
            if (phase) pending++;
            /* 26.13: while a resync is pending, an idle slot stays idle. */
            if (stop || phase || s->Rec.Resync || (slot->Feedback && s->FeedbackOff)) continue;
            if (!slot->Feedback) {
                /* 26.13: while LagSuspect, a data slot goes with the frame
                 * read before it, the worker going on meanwhile. A request
                 * that came back since the pass began is taken here; one
                 * back and not taken still occupies the storage. */
                if (s->Rec.LagSuspect && !taken) taken = FrameTake(s, &ok, &frame, &forResync);
                gate = UacPlanDataGate(s->Rec.LagSuspect, taken && !forResync, FramePhase(s) != 0,
                                       s->Frame.Cancelled);
                if (gate == UAC_GATE_ASK) {
                    if (!FrameSend(s, FALSE)) gate = UAC_GATE_SUBMIT;
                    else if (FrameTake(s, &ok, &frame, &forResync)) {
                        /* back at once, as most stacks give it; the Stop
                         * is read again after it */
                        taken = TRUE;
                        KeAcquireSpinLock(&s->Lock, &old); stop = s->Stop; KeReleaseSpinLock(&s->Lock, old);
                        if (stop) continue;
                        gate = UAC_GATE_FRAME;
                    } else gate = UAC_GATE_HOLD;
                }
                if (gate == UAC_GATE_HOLD) continue;
                useFrame = gate == UAC_GATE_FRAME && ok;
            }
            status = SubmitSlot(s, slot, TRUE, 0, useFrame, useFrame ? frame : 0);
            if (status == STATUS_DELETE_PENDING) stop = TRUE; /* a Stop: its own status kept */
            else if (!NT_SUCCESS(status)) { TR_LINE(("subfail", i, (ULONG)status, 0)); Fail(s, status); stop = TRUE; }
            else pending++;
        }
        frameOut = FramePhase(s) ? TRUE : FALSE;
        /* 26.13: the hold is bounded. A resync still wanted UAC_RESYNC_WAIT_MS
         * of interrupt time after the hold began (a slot or the resync's
         * frame request the stack kept, or a frame back only after it) ends
         * the stream, as 25.8's run limit did before; the stop's drain
         * cancels what is out. Read again before the resync runs. */
        if (!s->Rec.Resync) s->ResyncSince = 0;
        else if (!s->ResyncSince) s->ResyncSince = KeQueryInterruptTime();
        if (!stop && UacPlanHoldExpired(s->Rec.Resync,
                                        (ULONG)((KeQueryInterruptTime() - s->ResyncSince) / 10000))) {
            TR_LINE(("rsywait", pending, s->CompletedSequence, frameOut));
            Fail(s, STATUS_DEVICE_DATA_ERROR);
            stop = TRUE;
        }
        if (!stop && s->Rec.Resync && !pending) {
            /* the resync's own request, sent once every slot is back; one
             * back since the pass began is taken, the Stop read after it */
            if (!taken && FrameTake(s, &ok, &frame, &forResync)) {
                taken = TRUE;
                KeAcquireSpinLock(&s->Lock, &old); stop = s->Stop; KeReleaseSpinLock(&s->Lock, old);
            }
            gate = stop ? UAC_RSY_WAIT : UacPlanResyncGate(taken && forResync, FramePhase(s) != 0);
            /* a frame out longer than UAC_FRAME_FRESH_MS is read afresh:
             * the slots could not be placed by it inside the stack's range */
            if (gate == UAC_RSY_RUN && ok && !UacPlanResyncAhead(frame, s->Frame.AgeMs, &frame)) {
                TR_LINE(("rsystale", frame, s->Frame.AgeMs, 0));
                gate = UAC_RSY_ASK;
            }
            if (gate == UAC_RSY_ASK) {
                if (!FrameSend(s, TRUE)) {
                    TR_LINE(("rsyirp", 0, 0, 0));
                    Fail(s, STATUS_INSUFFICIENT_RESOURCES);
                } else if (FrameTake(s, &ok, &frame, &forResync)) {
                    KeAcquireSpinLock(&s->Lock, &old); stop = s->Stop; KeReleaseSpinLock(&s->Lock, old);
                    if (!stop) gate = UAC_RSY_RUN;
                    /* stale even so: the next pass asks again (FrameDone
                     * left Wake set), the hold bounding it */
                    if (gate == UAC_RSY_RUN && ok && !UacPlanResyncAhead(frame, s->Frame.AgeMs, &frame))
                        gate = UAC_RSY_WAIT;
                }
            }
            if (gate == UAC_RSY_RUN &&
                UacPlanHoldExpired(s->Rec.Resync, (ULONG)((KeQueryInterruptTime() - s->ResyncSince) / 10000))) {
                TR_LINE(("rsywait", 0, s->CompletedSequence, s->Frame.AgeMs));
                Fail(s, STATUS_DEVICE_DATA_ERROR);
            } else if (gate == UAC_RSY_RUN)
                Resync(s, ok, frame);
            KeAcquireSpinLock(&s->Lock, &old); stop = s->Stop; KeReleaseSpinLock(&s->Lock, old);
            for (pending = 0, i = 0; i < SLOTS; i++) {
                KeAcquireSpinLock(&s->Lock, &old); phase = s->Slot[i].Phase; KeReleaseSpinLock(&s->Lock, old);
                if (s->Slot[i].Used && phase) pending++;
            }
            frameOut = FramePhase(s) ? TRUE : FALSE;
        }
        if (UacPlanWorkerExit(stop, pending, frameOut)) break;
        /* The deadline is in bus time (section 25), never system time:
         * NTP or SetSystemTime must not create a feedback loss or hide one. */
        if (!stop && s->Feedback && !s->FeedbackOff &&
            UacFeedbackStaleBoth(KeQueryInterruptTime(), s->LastFeedbackPacket, s->FeedbackUnusableUs,
                                 s->FeedbackQuietUs, s->Rate_.FeedbackActive)) {
            TR_LINE(("fblate", (ULONG)s->Rate_.FeedbackActive, s->Rate_.GoodFeedback, s->Rate_.BadFeedback));
            Fail(s, STATUS_IO_TIMEOUT);
        } /* nothing usable back for 1 s at start, 200 ms after */
        /* 19.6's fairness rule (build 23): a stack that completes URBs
         * inline over and over (nothing delivered, no Fail) leaves Wake
         * signalled, so the wait below returns at once and a real-time
         * worker would hold the processor. The time run since the last
         * wait is counted; a window busy past UAC_DUTY_PERCENT demotes the
         * worker to its created priority for the next window, where the
         * normal threads - the one that asks Stop among them - share the
         * processor with it. It never sleeps: a working stream keeps
         * refilling. Measured, so no wake racing the wait can hide it. */
        if (realtime) {
            now = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
            busy += now - runStart;
            next = UacPlanDuty(now - windowStart, busy, window, demoted, &closed);
            if (closed) { windowStart = now; busy = 0; }
            if (next != demoted) {
                (VOID)KeSetPriorityThread(KeGetCurrentThread(),
                                          next ? basePriority : LOW_REALTIME_PRIORITY);
                s->Stats.WorkerPriority = next ? (ULONG)basePriority : LOW_REALTIME_PRIORITY;
                if (next) s->Stats.WorkerThrottles++;
                demoted = next;
            }
        }
        timeout.QuadPart = -1000000; /* watchdog only; events drive PCM */
        if (realtime) waitStart = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
#if defined(UAC_TRACE_E9)
        if (KeWaitForSingleObject(&s->Wake, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
            TrDump("idle", pending, stop, 0);
            TrPhases(s, "idle", pending);
        }
#else
        KeWaitForSingleObject(&s->Wake, Executive, KernelMode, FALSE, &timeout);
#endif
        if (realtime) {
            runStart = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
            /* A wait that returned at once is not idle time (Codex, build
             * 23): shorter than UAC_DUTY_SHORT_WAIT_US, it counts as busy. */
            if (runStart - waitStart < shortWait) busy += runStart - waitStart;
        }
    }
    KeAcquireSpinLock(&s->Lock, &old); s->Stats.Running = 0; KeReleaseSpinLock(&s->Lock, old);
    TR_DUMP(("exit", s->CompletedSequence, s->NextSequence, (ULONG)s->Stats.LastStatus));
    KeSetEvent(&s->Exited, IO_NO_INCREMENT, FALSE); /* last access to s */
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS UacUsbOpen(PDEVICE_OBJECT lower, PDEVICE_OBJECT pdo, const UAC_DESC *desc, UAC_DEV **out)
{
    UAC_DEV *d;
    UAC_BYTE numbers[32], pipes[32];
    unsigned int ifCount;
    BOOLEAN high = FALSE, usb11 = FALSE;
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
#ifdef UAC_V1
    /* uac1 decided the speed before parsing (UacUsbBusSpeed and the
     * descriptors, uacwdm.cpp). */
    high = desc->Speed == 2;
    status = desc->Speed == 1 || desc->Speed == 2 ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
#else
    status = BusSpeed(lower, &high);
    /* 2000 (WDM 1.10, below 1.20): the device's PDO is its USB 1.1 hub
     * driver's (usbhub; the USB 2.0 one is usbhub20). */
    if (IoIsWdmVersionAvailable(1, 0x10) && !IoIsWdmVersionAvailable(1, 0x20) &&
        pdo && pdo->DriverObject && pdo->DriverObject->DriverName.Buffer)
        usb11 = UacPlanUsb11Hub(pdo->DriverObject->DriverName.Buffer,
                                pdo->DriverObject->DriverName.Length / sizeof(WCHAR)) ? TRUE : FALSE;
    TR_LINE(("speed", (ULONG)status, high, usb11));
#endif
    /* No speed from the USBDI interface (STATUS_NOT_SUPPORTED: no answer,
     * or a version 0 interface, which predates USB 2.0; any other unusable
     * answer fails otherwise) on a system below WDM 1.10 (98 SE, ME):
     * Windows' own 9x stack, UHCI or OHCI without USB 2.0, where a device
     * runs at Full Speed. NUSB's EHCI stack and xHCI98 give version 1. On
     * 2000 the same holds only under its own USB 1.1 hub driver (UHCI and
     * OHCI; docs/design/uac2.md section 25). Otherwise, on 2000 and later,
     * it is still refused: the speed is not guessed there. */
    if (status == STATUS_NOT_SUPPORTED && (!IoIsWdmVersionAvailable(1, 0x10) || usb11)) {
        high = FALSE;
        status = STATUS_SUCCESS;
    }
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
    TR_LINE(("config", (ULONG)status, d->IfCount, d->HighSpeed));
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
    PVOID context, ULONG queueRead, ULONG queueMs, ULONG ringBytes, UAC_USB **out)
{
    unsigned int packets;
    unsigned long span;
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
    /* Full Speed asks 4 bytes where the endpoint holds them, so a device
     * sending 16.16 is read whole; a 3-byte 10.14 answer is a short
     * packet and is decoded as such. */
    s->FeedbackBytes = UacRateFeedbackStride(d->HighSpeed, UacPlanCapacity(a->FeedbackMaxPacket));
    if (s->Feedback && !s->FeedbackBytes) { ExFreePool(s); return STATUS_NOT_SUPPORTED; }
#ifdef UAC_V1
    /* 26.12: an adaptive source with a synch OUT endpoint is sent Ff (3
     * bytes at Full Speed, 4 at High Speed, as its wMaxPacketSize allows);
     * MaxPacketsOnly OUT is padded, IN counted against the cadence. */
    if (capture && UacPlanSync(a) == 2 && a->FeedbackEndpoint && !(a->FeedbackEndpoint & 0x80) &&
        UacPlanCapacity(a->FeedbackMaxPacket) >= (d->HighSpeed ? 4U : 3U)) {
        s->Feedforward = TRUE;
        s->FeedbackBytes = d->HighSpeed ? 4 : 3;
        s->FfValue = UacConvFf(rate, d->HighSpeed);
    }
    s->MpoOut = !capture && (a->EpControls & UAC_CONV_MPO) ? TRUE : FALSE;
    s->MpoIn = capture && (a->EpControls & UAC_CONV_MPO) ? TRUE : FALSE;
    if (s->MpoIn && (UacPlanSync(a) == 1 ||
                     (a->FormatTag != UAC_TAG_PCM && a->FormatTag != UAC_TAG_FLOAT) ||
                     !UacConvCadence(&s->Cadence, rate, d->HighSpeed, a->Interval,
                                     s->Feedforward ? s->FfValue : 0))) {
        ExFreePool(s); return STATUS_NOT_SUPPORTED;
    }
#endif
    /* About 4 ms per data URB at High Speed bInterval 1-3 and at Full
     * Speed; 8 ms at bInterval 4, as High Speed packet counts stay a
     * multiple of 8 (one frame) for the older stacks. As many URBs as
     * cover Uac2QueueMs, below the client's ring by at least one URB
     * (docs/design/uac2.md 19.4); a ring shrunk after RUN is not
     * followed (17.4 item 5). */
    s->DataSlots = UacPlanQueue(d->HighSpeed, a->Interval, queueMs,
        (unsigned long)rate * s->FrameBytes, (unsigned int)s->FrameBytes,
        (unsigned int)s->Capacity, ringBytes, &packets, &span);
    s->DataPackets = packets; s->TransferUs = span;
    s->QueueRead = queueRead; s->QueueMs = UacPlanQueueMs(queueMs);
    s->SlotBytes = s->DataPackets * s->Capacity;
    s->Fill = fill; s->Captured = captured; s->Progress = progress; s->CallbackContext = context;
    KeInitializeSpinLock(&s->Lock); KeInitializeMutex(&s->Operation, 0);
    KeInitializeEvent(&s->Wake, NotificationEvent, FALSE);
    KeInitializeEvent(&s->Ready, NotificationEvent, FALSE);
    KeInitializeEvent(&s->Exited, NotificationEvent, FALSE);
    s->WaitThread = IoIsWdmVersionAvailable(1, 0x10);
    s->RealtimeRead = UAC_QUEUE_NONE; s->Realtime = UacPlanRealtime(UAC_QUEUE_NONE);
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
    TR_LINE(("stop", s->Thread ? 1 : 0, s->ActiveAlt, (ULONG)s->Stats.LastStatus));
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
        TR_LINE(("stopped", 0, 0, 0));
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

void UacUsbSetRealtime(UAC_USB *s, ULONG raw)
{
    if (!s) return;
    s->RealtimeRead = raw; s->Realtime = UacPlanRealtime(raw);
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
#if defined(UAC_TRACE_E9)
    TrSetup(); TrRestart();
    TrLine("start", (ULONG)s->Capture << 16 | s->Alt.Interface << 8 | s->Alt.Alternate,
           (ULONG)s->Dev->HighSpeed << 24 | s->DataSlots << 16 | s->DataPackets, s->Feedback);
#endif
    RtlZeroMemory(&s->Stats, sizeof(s->Stats));
    s->Stats.HighSpeed = s->Dev->HighSpeed;
    s->Stats.Capture = s->Capture; s->Stats.Channels = s->Alt.Channels;
    s->Stats.Bits = s->Alt.Bits; s->Stats.Subslot = s->Alt.Subslot;
    s->Stats.Interface = s->Alt.Interface; s->Stats.Alternate = s->Alt.Alternate;
    s->Stats.Sync = UacPlanSync(&s->Alt);
    s->Stats.QueueRead = s->QueueRead; s->Stats.QueueMs = s->QueueMs;
    s->Stats.DataTransfers = s->DataSlots; s->Stats.TransferUs = s->TransferUs;
    s->Stats.RealtimeRead = s->RealtimeRead; s->Stats.Realtime = s->Realtime;
    status = SetClock(s);
    if (!NT_SUCCESS(status)) goto done;
    ReadFeature(s);
    status = SelectAlt(s, TRUE);
    if (!NT_SUCCESS(status)) goto done;
#ifdef UAC_V1
    status = EndpointRate(s);
    if (!NT_SUCCESS(status)) goto done;
#endif
    {
        struct _URB_GET_CURRENT_FRAME_NUMBER frame;
        RtlZeroMemory(&frame, sizeof(frame));
        frame.Hdr.Length = sizeof(frame);
        frame.Hdr.Function = URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
        if (NT_SUCCESS(UsbRequest(s->Dev->Lower, (PURB)&frame))) s->Stats.StartCurrentFrame = frame.FrameNumber;
        else s->Stats.StartCurrentFrame = 0xffffffff;
        TR_LINE(("frame", s->Stats.StartCurrentFrame, s->Rate, s->Capacity));
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
#ifdef UAC_V1
        slot->Used = slot->Feedback ? s->Feedback || s->Feedforward : i < FEEDBACK_SLOTS + s->DataSlots;
#else
        slot->Used = slot->Feedback ? s->Feedback : i < FEEDBACK_SLOTS + s->DataSlots;
#endif
        if (!slot->Used) continue;
        slot->Packets = slot->Feedback ? FEEDBACK_PACKETS : s->DataPackets;
        size = FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, IsoPacket) +
            slot->Packets * sizeof(USBD_ISO_PACKET_DESCRIPTOR);
        slot->Urb = ExAllocatePoolWithTag(NonPagedPool, size, TAG);
        slot->BufferBytes = slot->Feedback ? FEEDBACK_PACKETS * s->FeedbackBytes : s->SlotBytes;
        slot->Buffer = ExAllocatePoolWithTag(NonPagedPool, slot->BufferBytes, TAG);
        if (!slot->Urb || !slot->Buffer) { status = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    }
    s->NextSequence = s->CompletedSequence = 0;
    s->LostRun = 0; s->LostLimit = UacPlanLostLimit(s->TransferUs);
    s->FbRestartRun = 0; s->FeedbackOff = FALSE;
    RtlZeroMemory(&s->Rec, sizeof(s->Rec)); s->ResyncSince = 0;
    RtlZeroMemory(&s->Frame, sizeof(s->Frame));
#ifdef UAC_V1
    s->FfRun = 0; s->FfDelivered = FALSE; s->Cadence.Acc = 0; s->Cadence.Owed = 0;
#endif
    s->Stop = FALSE; s->Stats.Running = 1;
    s->LastFeedbackPacket = KeQueryInterruptTime();
    s->LastFeedbackValueAt = s->LastFeedbackPacket;
    s->FeedbackUnusableUs = s->FeedbackQuietUs = 0;
    s->DoneCount = s->QuietFrom = s->FeedbackStamp = 0;
    s->FeedbackPacketUs = UacFeedbackPacketUs(s->Dev->HighSpeed, s->Alt.FeedbackInterval);
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
