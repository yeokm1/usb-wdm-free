/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * usbmtp.c - USBMTP.SYS, the WDM transport for MTP and PTP devices on
 * Windows 98 SE and ME (docs/design/mtp.md 5). It moves bytes; the
 * protocol is the library's (mtplib). Interface 1.1: usbmtp.h.
 *
 * The request slots are slot.c's (5.4): every state change is a Slot* call
 * under the device's one spin lock, and what calls down or completes an
 * IRP is an action performed here with the lock released. The rules M0
 * produced (2026-11-16, docs/testing/2026-11-16-mtp-m0.md) and MTPPRB.SYS
 * established as the pattern:
 * - a request's result travels with it: what a completion tail needs is in
 *   its SA_COMPLETE action or in the caller's IRP the tail owns; no tail
 *   reads slot storage;
 * - admission is gated by the started state under the same hold of the
 *   lock as SlotAdmit; QUERY_STOP is refused while a request is out; STOP
 *   stays pending until every slot is idle;
 * - a request's own fields are written only after its admission;
 * - decisions by the IRP status; a transfer succeeds only when the IRP
 *   succeeded and the URB status is USBD_STATUS_SUCCESS;
 * - power: DO_POWER_PAGABLE; a power IRP is forwarded at PASSIVE_LEVEL
 *   under its admission (a work item above it, the IRP carrying its
 *   admission there); a query below working (or another component's
 *   below D0) closes the power gate, stops every slot and waits, bounded,
 *   for quiet: passed down at quiet with a completion routine that takes
 *   the lower answer under the query's own admission, refused if not; a
 *   set into sleep closes the gate, stops every slot, waits for quiet up
 *   to its bound and goes down once, what is still out quarantined; the
 *   gate opens when its reasons have ended and nothing stopped is out
 *   (mtp.md 5.4, "Standby");
 * - the deleter waits only for the dispatch pins, which cover no call;
 *   everything that calls out (dispatches without a handle, the power
 *   worker, the slots' calls, tails, timers, cancel routines) is drained
 *   by REMOVE before its detach, and no device is deleted before its
 *   detach, so nothing the deleter waits for can be in its own call chain
 *   (mtp.md 5.4, "What is drained");
 * - the device object outlives its handles (a zombie); one exclusive
 *   deleter claims IoDeleteDevice under the lock, after the detach, when
 *   slot.c's FreeReady is set; IoDeleteDevice is the last act (98 SE
 *   calls DriverUnload inside it);
 * - each handle carries its own context (FsContext) with its device's
 *   number; an IRP on another device's handle is refused.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include <wdm.h>
#include <usbdi.h>
#include <usbdlib.h>
#include "slot.h"
#include "cfgwalk.h"
#include "usbmtp.h"

#define DRIVER_VERSION 0x00010000UL     /* 0.1.0.0 */
#define POOL_TAG 'ptMU'

/* The interface's layouts (5.3), checked at compile time. */
typedef char InfoIs52[(sizeof(USBMTP_INFO) == USBMTP_INFO_SIZE && FIELD_OFFSET(USBMTP_INFO, Vid) == 20 &&
                       FIELD_OFFSET(USBMTP_INFO, MaxPacketIn) == 30 && FIELD_OFFSET(USBMTP_INFO, Speed) == 36 &&
                       FIELD_OFFSET(USBMTP_INFO, EndpointIn) == USBMTP_INFO_SIZE_1_0 &&
                       FIELD_OFFSET(USBMTP_INFO, PowerGate) == USBMTP_INFO_SIZE_1_0X &&
                       FIELD_OFFSET(USBMTP_INFO, Vetoes) == 48) ? 1 : -1];
/* The gate's reasons and GET_INFO's power bits are one numbering. */
typedef char PowerBits[(USBMTP_POWER_SYS == SLOT_PD_SYS && USBMTP_POWER_DEV == SLOT_PD_DEV &&
                        USBMTP_POWER_QUERY == SLOT_PD_QUERY && USBMTP_POWER_DRAIN == SLOT_PD_DRAIN &&
                        USBMTP_POWER_DQUERY == SLOT_PD_DQUERY && USBMTP_HELD_TAILS == SLOT_HELD_TAILS) ? 1 : -1];
typedef char StringInIs4[(sizeof(USBMTP_STRING_IN) == 4) ? 1 : -1];
typedef char ClaimIs16[(sizeof(USBMTP_CLAIM_INFO) == 16) ? 1 : -1];
typedef char ClassInIs12[(sizeof(USBMTP_CLASS_IN) == 12) ? 1 : -1];
typedef char WaitInIs8[(sizeof(USBMTP_WAIT_IN) == 8) ? 1 : -1];
typedef char TimeoutsIs8[(sizeof(USBMTP_TIMEOUTS) == 8) ? 1 : -1];
typedef char EventIs64[(sizeof(USBMTP_EVENT) == 64) ? 1 : -1];
typedef char ActionsFit[(SLOT_MAX_ACTIONS >= 3 * SLOT_COUNT + 2) ? 1 : -1];

#ifdef USBMTP_TRACE
int __cdecl _outp(unsigned short port, int value);
#pragma intrinsic(_outp)
static KSPIN_LOCK g_TraceLock;
static void Trace(const char *What, ULONG A, ULONG B)
{
    static const char hex[] = "0123456789ABCDEF";
    KIRQL irql;
    int i;
    KeAcquireSpinLock(&g_TraceLock, &irql);
    for (i = 0; "mtp "[i]; i++) _outp(0xE9, "mtp "[i]);
    for (i = 0; What[i]; i++) _outp(0xE9, What[i]);
    _outp(0xE9, ' ');
    for (i = 28; i >= 0; i -= 4) _outp(0xE9, hex[(A >> i) & 15]);
    _outp(0xE9, ' ');
    for (i = 28; i >= 0; i -= 4) _outp(0xE9, hex[(B >> i) & 15]);
    _outp(0xE9, '\n');
    KeReleaseSpinLock(&g_TraceLock, irql);
}
#define TRACE(w, a, b) Trace(w, (ULONG)(a), (ULONG)(b))
#else
#define TRACE(w, a, b) ((void)0)
#endif

/* A request's kind on its slot, set at admission. */
#define K_READ      1   /* SLOT_BIN */
#define K_WRITE     2   /* SLOT_BOUT */
#define K_ZLP       3   /* SLOT_BOUT */
#define K_CLASS     4   /* SLOT_CTL */
#define K_RESET     5   /* SLOT_CTL */
#define K_STRING    6   /* SLOT_CTL */
#define K_POLL      7   /* SLOT_INT */
#define K_WAIT      8   /* SLOT_WAIT */

/* The standby's bounds (mtp.md 5.4, "Standby"): choices, not measurements.
   The standby test build (build.ps1 -Flavour standbytest, a trace build,
   never released) takes both waits at 0 ms: the emulator retires every
   cancel at once, so only a zero bound lets its legs reach a refusal and a
   set passed down with requests out. */
#ifdef USBMTP_STANDBY_TEST
#ifndef USBMTP_TRACE
#error the standby test build is a trace build
#endif
#define QUIET_QUERY_MS 0
#define QUIET_SET_MS   0
#else
#define QUIET_QUERY_MS 3000     /* a query's wait for quiet */
#define QUIET_SET_MS   3000     /* a set's wait for quiet */
#endif
#define QUERY_HOLD_MS  30000    /* the lapse of an accepted query (a local recovery heuristic) */

#define CTL_BUF     512
#define INT_BUF     3072    /* one interrupt interval: up to 3 x 1,024 (high-bandwidth high speed) */
#define EVENT_QUEUE 16

/* Handle rights (5.1). */
#define R_INFO      0
#define R_OWNER     1
#define R_BOUND     2

struct EXT;

typedef struct HCTX {           /* a handle's own context: FileObject->FsContext */
    ULONG Instance;             /* its device's number */
    int Rights;
    ULONG Epoch;                /* the claim it holds or is bound to */
    int CleanedUp;
} HCTX;

typedef struct WCTX { struct EXT *E; int Slot; } WCTX;

typedef struct XSLOT {
    PIRP Irp;                   /* lower IRP: IoAllocateIrp(n, FALSE), rebuilt per submission */
    PURB Urb;
    PUCHAR Buf;                 /* the slot's own transfer buffer */
    ULONG BufLen;
    KTIMER Timer;
    KDPC Dpc;
    WCTX W;
    /* the request's fields, written at admission */
    int Kind;
    ULONG Len;                  /* the whole request's length */
    ULONG Seg;                  /* the length of the segment in flight */
    ULONG Param;                /* class: request number; reset: target pipe; string: index | lang << 16 */
    ULONG Param2;               /* class: transaction id; string: the languages still to read */
    PUCHAR Caller;              /* the caller's buffer (system address), for reads, writes, controls */
} XSLOT;

typedef struct EXT {
    PDEVICE_OBJECT Self, Lower, Pdo;
    KSPIN_LOCK Lock;
    SLOT_DEVICE D;
    SLOT_OPS Ops;
    XSLOT X[SLOT_COUNT];
    USBD_PIPE_HANDLE PipeIn, PipeOut, PipeInt;
    USBMTP_INFO Info;
    ULONG Instance;             /* the handles' tag */
    ULONG N;                    /* \\.\USBMTP<n> */
    int Started, Configured, StopQueried;
    LIST_ENTRY PowerQ;          /* admitted power IRPs that came above PASSIVE_LEVEL (slot.c's PowerQ counts them) */
    WORK_QUEUE_ITEM PowerItem;  /* preallocated: no allocation on the power path */
    ULONG BulkInMs, BulkOutMs;
    ULONG IntLen;               /* the interrupt poll's transfer length (Start) */
    ULONG ArmMs;                /* the time-out SlotAdmit arms (OpArm), set just before it under the lock */
    ULONG TokenLow, TokenHigh;  /* the active claim's */
    PFILE_OBJECT Owner;         /* the owner handle's file object */
    USBMTP_EVENT Queue[EVENT_QUEUE];
    int QHead, QCount, QLost;
    WCHAR LinkBuf[32];
    UNICODE_STRING Link;
} EXT;

static LONG g_Running;          /* power worker routines not yet at their last act: DriverUnload waits */
static LONG g_Foreign;          /* IRPs refused for coming on another device's handle */
static LONG g_Dropped;          /* actions Run could not queue: never, by slot.h's sizing */
static ULONG g_Generation, g_Instance, g_Token;
static ULONG g_NUsed;           /* bit n: \\.\USBMTP<n> is taken */
static KSPIN_LOCK g_Global;
static PDEVICE_OBJECT g_Deleted;
/* The late hand-off (mtp.md 5.4): power IRPs refused above PASSIVE_LEVEL,
   completed at PASSIVE_LEVEL by one driver-global work item. Under g_Global:
   the list and g_LateQueued. The deleter never waits for it; DriverUnload
   waits for g_LateRunning unless it runs on the worker's own thread (inside
   a completion the worker made). */
static LIST_ENTRY g_LateQ;
static WORK_QUEUE_ITEM g_LateItem;
static int g_LateQueued;            /* the late worker is queued or running */
static LONG g_LateRunning;          /* the late worker not yet at its last act */
static PKTHREAD volatile g_LateThread;  /* the thread the late worker runs on, while it takes IRPs */

static ULONG Ms(void)
{
    LARGE_INTEGER t;
    KeQueryTickCount(&t);
    return (ULONG)((t.QuadPart * KeQueryTimeIncrement()) / 10000);
}

/* ------------------------------------------------------------------ waits */

static void Sleep10(void)
{
    LARGE_INTEGER step;
    step.QuadPart = -100000;    /* 10 ms */
    KeDelayExecutionThread(KernelMode, FALSE, &step);
}

/* ------------------------------------------------------------------ ops for slot.c */

static VOID CallerCancel(PDEVICE_OBJECT Dev, PIRP Irp);

static void OpArm(void *C, int Slot)
{
    EXT *E = (EXT *)C;
    LARGE_INTEGER due;
    /* Only SlotAdmit arms, so the staged value is the admitted request's;
       the slot's own fields are written only after the admission. */
    due.QuadPart = -(LONGLONG)E->ArmMs * 10000;
    KeSetTimer(&E->X[Slot].Timer, due, &E->X[Slot].Dpc);   /* one-shot */
}
static int OpCancelTimer(void *C, int Slot) { return KeCancelTimer(&((EXT *)C)->X[Slot].Timer) ? 1 : 0; }
static void OpSetCancel(void *C, int Slot) { IoSetCancelRoutine((PIRP)((EXT *)C)->D.S[Slot].Irp, CallerCancel); }
static int OpIrpCancelled(void *C, int Slot) { return ((PIRP)((EXT *)C)->D.S[Slot].Irp)->Cancel; }
static int OpClearCancel(void *C, int Slot) { return IoSetCancelRoutine((PIRP)((EXT *)C)->D.S[Slot].Irp, NULL) != NULL; }

static NTSTATUS Outcome(int O, NTSTATUS Lower)
{
    switch (O) {
    case SO_SUCCESS: return STATUS_SUCCESS;
    case SO_ERROR: return NT_SUCCESS(Lower) ? STATUS_UNSUCCESSFUL : Lower;
    case SO_CANCELLED: return STATUS_CANCELLED;
    case SO_TIMEOUT: return STATUS_IO_TIMEOUT;
    case SO_REMOVED: return STATUS_DELETE_PENDING;
    case SO_BUSY: return STATUS_DEVICE_BUSY;
    case SO_DENIED: return STATUS_ACCESS_DENIED;
    case SO_ASLEEP: return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS Done(PIRP Irp, NTSTATUS St, ULONG Info)
{
    Irp->IoStatus.Status = St;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return St;
}

/* ------------------------------------------------------------------ actions */

static NTSTATUS LowerDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx);

static void BuildControl(EXT *E, XSLOT *x, UCHAR Request, ULONG In, PVOID Data, ULONG Length)
{
    UsbBuildVendorRequest(x->Urb, URB_FUNCTION_CLASS_INTERFACE, sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST),
                          (In ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : USBD_TRANSFER_DIRECTION_OUT), 0,
                          Request, 0, E->Info.Interface, Data, NULL, Length, NULL);
}

/* Rebuild the slot's lower IRP for its next segment: only for SA_SUBMIT,
   when slot.c has the IRP back. The request's fields were set at its
   admission; the slot owns the caller's buffer until Finish. */
static void Build(EXT *E, int s)
{
    XSLOT *x = &E->X[s];
    SLOT *S = &E->D.S[s];
    PIO_STACK_LOCATION st;
    CCHAR n = E->Lower->StackSize;
    ULONG left;
    IoInitializeIrp(x->Irp, IoSizeOfIrp(n), n);     /* 98/ME reuse (Microsoft, "Reusing IRPs") */
    switch (x->Kind) {
    case K_READ:
        left = x->Len - S->Bytes;
        x->Seg = left < USBMTP_SEGMENT ? left : USBMTP_SEGMENT;
        UsbBuildInterruptOrBulkTransferRequest(x->Urb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER), E->PipeIn,
                                               x->Buf, NULL, x->Seg,
                                               USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, NULL);
        break;
    case K_WRITE:
        left = x->Len - S->Bytes;
        x->Seg = left < USBMTP_SEGMENT ? left : USBMTP_SEGMENT;
        RtlCopyMemory(x->Buf, x->Caller + S->Bytes, x->Seg);
        UsbBuildInterruptOrBulkTransferRequest(x->Urb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER), E->PipeOut,
                                               x->Buf, NULL, x->Seg, USBD_TRANSFER_DIRECTION_OUT, NULL);
        break;
    case K_ZLP:
        x->Seg = 0;
        UsbBuildInterruptOrBulkTransferRequest(x->Urb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER), E->PipeOut,
                                               x->Buf, NULL, 0, USBD_TRANSFER_DIRECTION_OUT, NULL);
        break;
    case K_POLL:
        /* one interval's worth: the packet size times the transactions per
           interval (high-bandwidth high speed), so no event can overrun it */
        x->Seg = E->IntLen;
        UsbBuildInterruptOrBulkTransferRequest(x->Urb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER), E->PipeInt,
                                               x->Buf, NULL, x->Seg,
                                               USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, NULL);
        break;
    case K_RESET:
        RtlZeroMemory(x->Urb, sizeof(struct _URB_PIPE_REQUEST));
        x->Urb->UrbPipeRequest.Hdr.Length = sizeof(struct _URB_PIPE_REQUEST);
        x->Urb->UrbPipeRequest.Hdr.Function = URB_FUNCTION_RESET_PIPE;
        x->Urb->UrbPipeRequest.PipeHandle = x->Param == USBMTP_PIPE_BULK_IN ? E->PipeIn :
                                            x->Param == USBMTP_PIPE_BULK_OUT ? E->PipeOut : E->PipeInt;
        break;
    case K_STRING:
        /* Param2 set: language id 0 was asked for, so the first segment
           reads string descriptor 0 (the device's languages). */
        x->Seg = 255;
        UsbBuildGetDescriptorRequest(x->Urb, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST), USB_STRING_DESCRIPTOR_TYPE,
                                     (UCHAR)(x->Param2 ? 0 : x->Param & 0xFF), (USHORT)(x->Param2 ? 0 : x->Param >> 16),
                                     x->Buf, NULL, 255, NULL);
        break;
    case K_CLASS:
        if (x->Param == USBMTP_REQ_CANCEL) {
            /* Cancel Request data: the Cancellation Code 4001h, then the transaction id */
            x->Buf[0] = 0x01; x->Buf[1] = 0x40;
            x->Buf[2] = (UCHAR)x->Param2; x->Buf[3] = (UCHAR)(x->Param2 >> 8);
            x->Buf[4] = (UCHAR)(x->Param2 >> 16); x->Buf[5] = (UCHAR)(x->Param2 >> 24);
            x->Seg = 6;
            BuildControl(E, x, 0x64, 0, x->Buf, 6);
        } else if (x->Param == USBMTP_REQ_RESET) {
            x->Seg = 0;
            BuildControl(E, x, 0x66, 0, NULL, 0);
        } else {
            x->Seg = x->Len;
            BuildControl(E, x, (UCHAR)(x->Param == USBMTP_REQ_EXT_EVENT ? 0x65 : 0x67), 1, x->Buf, x->Len);
        }
        break;
    }
    st = IoGetNextIrpStackLocation(x->Irp);
    st->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    st->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    st->Parameters.Others.Argument1 = x->Urb;
    IoSetCompletionRoutine(x->Irp, LowerDone, &x->W, TRUE, TRUE, TRUE);
}

/* Perform slot.c's actions; the actions they lead to are performed by the
   same loop (no recursion). */
static void Run(EXT *E, SLOT_ACTIONS *First)
{
    SLOT_ACTIONS q, more;
    int i, go;
    KIRQL irql;
    PIRP irp;
    q = *First;
    while (q.Count) {
        SLOT_ACTION a = q.A[0];
        for (i = 1; i < q.Count; i++) q.A[i - 1] = q.A[i];
        q.Count--;
        more.Count = 0;
        switch (a.Kind) {
        case SA_SUBMIT:
            /* The submission (5.4): commit, rebuild, hand off. A stop before
               the commit, or during the rebuild (its cancel deferred), keeps
               the IRP from ever going down, and no IoCancelIrp overlaps the
               rebuild; a stop after the hand-off cancels the whole IRP. */
            KeAcquireSpinLock(&E->Lock, &irql);
            go = SlotSubmitCommit(&E->D, &E->Ops, a.Slot, &more);
            KeReleaseSpinLock(&E->Lock, irql);
            if (go) {
                Build(E, a.Slot);
                KeAcquireSpinLock(&E->Lock, &irql);
                go = SlotSubmitBuilt(&E->D, &E->Ops, a.Slot, &more);
                KeReleaseSpinLock(&E->Lock, irql);
            }
            if (go) {
                IoCallDriver(E->Lower, E->X[a.Slot].Irp);
                KeAcquireSpinLock(&E->Lock, &irql);
                SlotSubmitReturned(&E->D, &E->Ops, a.Slot, &more);
                KeReleaseSpinLock(&E->Lock, irql);
            }
            break;
        case SA_CANCEL_LOWER:
            IoCancelIrp(E->X[a.Slot].Irp);
            KeAcquireSpinLock(&E->Lock, &irql);
            SlotPinReturned(&E->D, &E->Ops, a.Slot, &more);
            KeReleaseSpinLock(&E->Lock, irql);
            break;
        case SA_COMPLETE:
            /* The tail: everything from the action and the IRP it owns.
               Information was set in the IRP at the lower completion for
               the kinds that report something else than the byte count. */
            irp = (PIRP)a.Irp;
            Done(irp, Outcome(a.Outcome, irp->IoStatus.Status),
                 a.Outcome == SO_SUCCESS ? (irp->IoStatus.Information ? irp->IoStatus.Information : a.Bytes) : 0);
            KeAcquireSpinLock(&E->Lock, &irql);
            SlotTailDone(&E->D, &more);
            KeReleaseSpinLock(&E->Lock, irql);
            break;
        case SA_REMOVE_READY:
        case SA_FREE_READY:
            /* Only noted: REMOVE and the last close read RemoveReady and
               FreeReady under the lock (one exclusive deleter, 5.4). */
            break;
        }
        for (i = 0; i < more.Count; i++) {
            if (q.Count < SLOT_MAX_ACTIONS) {
                q.A[q.Count++] = more.A[i];
            } else {
                InterlockedIncrement(&g_Dropped);
                TRACE("dropped", more.A[i].Kind, more.A[i].Slot);
            }
        }
    }
}

/* An interrupt transfer into the event queue (under the lock): the oldest
   is dropped when the queue is full, and the loss is flagged. */
static void QueueEvent(EXT *E, PUCHAR Data, ULONG Len)
{
    USBMTP_EVENT *ev;
    int at;
    if (Len > sizeof(ev->Data)) Len = sizeof(ev->Data);
    if (E->QCount == EVENT_QUEUE) {
        E->QHead = (E->QHead + 1) % EVENT_QUEUE;
        E->QCount--;
        E->QLost = 1;
    }
    at = (E->QHead + E->QCount) % EVENT_QUEUE;
    ev = &E->Queue[at];
    RtlZeroMemory(ev, sizeof(*ev));
    ev->Length = Len;
    RtlCopyMemory(ev->Data, Data, Len);
    E->QCount++;
}

/* Take the oldest queued event into Out (under the lock); FALSE if none. */
static BOOLEAN TakeEvent(EXT *E, USBMTP_EVENT *Out)
{
    if (!E->QCount) return FALSE;
    *Out = E->Queue[E->QHead];
    Out->Flags = E->QLost ? USBMTP_EVENT_LOST : 0;
    E->QLost = 0;
    E->QHead = (E->QHead + 1) % EVENT_QUEUE;
    E->QCount--;
    return TRUE;
}

static NTSTATUS LowerDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx)
{
    WCTX *w = (WCTX *)Ctx;
    EXT *E = w->E;
    int s = w->Slot;
    XSLOT *x = &E->X[s];
    SLOT *S;
    SLOT_ACTIONS out;
    KIRQL irql;
    NTSTATUS st = Irp->IoStatus.Status;
    USBD_STATUS us = x->Urb->UrbHeader.Status;
    ULONG raw = 0, got = 0;
    int ok, shrt = 0;
    PIRP caller;
    (void)Dev;
    /* The length by the request's own kind, never by the URB's Function
       field, which a stack may rewrite while it holds the URB. */
    if (x->Kind == K_READ || x->Kind == K_WRITE || x->Kind == K_ZLP || x->Kind == K_POLL)
        raw = x->Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    else if (x->Kind == K_CLASS)
        raw = x->Urb->UrbControlVendorClassRequest.TransferBufferLength;
    else if (x->Kind == K_STRING)
        raw = x->Urb->UrbControlDescriptorRequest.TransferBufferLength;
    TRACE("len", ((ULONG)s << 24) | ((ULONG)x->Kind << 16) | x->Urb->UrbHeader.Function, raw);
    /* Success needs both the IRP and the URB (5.4: decisions by the IRP
       status; a URB status counts only when it is USBD_STATUS_SUCCESS). */
    ok = NT_SUCCESS(st) && us == USBD_STATUS_SUCCESS;
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    S = &E->D.S[s];
    caller = (PIRP)S->Irp;
    if (ok && raw > x->Seg) ok = 0;                     /* more than asked: not data */
    if (ok) {
        got = raw;
        switch (x->Kind) {
        case K_READ:
            if (caller && raw) RtlCopyMemory(x->Caller + S->Bytes, x->Buf, raw);
            shrt = raw < x->Seg;
            break;
        case K_WRITE:
            shrt = raw < x->Seg;    /* a write that moved less ends the request */
            break;
        case K_CLASS:
            if (x->Param == USBMTP_REQ_EXT_EVENT || x->Param == USBMTP_REQ_STATUS) {
                if (caller && raw) RtlCopyMemory(x->Caller, x->Buf, raw);
            } else {
                got = 0;    /* Cancel and Reset have no output: nothing goes back */
            }
            shrt = 1;
            break;
        case K_STRING: {
            /* the descriptor's own bLength, even, at least 2, no more than arrived */
            ULONG bl = raw >= 2 ? x->Buf[0] : 0;
            if (bl < 2 || (bl & 1) || bl > raw || x->Buf[1] != USB_STRING_DESCRIPTOR_TYPE) {
                ok = 0;
            } else if (x->Param2) {
                /* the languages: the first one listed, then the string itself */
                if (bl < 4) ok = 0;
                else {
                    x->Param = (x->Param & 0xFF) | ((ULONG)x->Buf[2] << 16) | ((ULONG)x->Buf[3] << 24);
                    x->Param2 = 0;
                    got = 0;
                }
            } else {
                if (caller) RtlCopyMemory(x->Caller, x->Buf, bl);
                got = bl;
                shrt = 1;
            }
            break;
        }
        case K_POLL:
            /* with the gate closed the data is neither queued nor delivered;
               SlotPollDone flags it lost, taken below (5.4) */
            if (!SlotPollAccept(&E->D)) break;
            QueueEvent(E, x->Buf, raw);
            /* An event for a pending WAIT_EVENT travels with its own IRP. */
            if (E->D.S[SLOT_WAIT].State == SS_ACTIVE && E->D.S[SLOT_WAIT].Irp) {
                PIRP wirp = (PIRP)E->D.S[SLOT_WAIT].Irp;
                if (TakeEvent(E, (USBMTP_EVENT *)wirp->AssociatedIrp.SystemBuffer))
                    wirp->IoStatus.Information = sizeof(USBMTP_EVENT);
            }
            break;
        default:
            shrt = 1;
            break;
        }
    }
    if (!ok && caller) caller->IoStatus.Status = NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
    if (s == SLOT_INT) {
        SlotPollDone(&E->D, &E->Ops, ok, &out);
        if (SlotTakeLost(&E->D)) E->QLost = 1;  /* the library refreshes the device (7.4) */
    } else {
        SlotLowerDone(&E->D, &E->Ops, s, ok, shrt, got, &out);
    }
    KeReleaseSpinLock(&E->Lock, irql);
    TRACE("irp", s, st);
    TRACE("urb", s, us);   /* every URB status, a failure's included */
    Run(E, &out);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static VOID TimerDpc(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    WCTX *w = (WCTX *)Ctx;
    EXT *E = w->E;
    SLOT_ACTIONS out;
    (void)Dpc; (void)A1; (void)A2;
    out.Count = 0;
    KeAcquireSpinLockAtDpcLevel(&E->Lock);
    SlotTimerBegin(&E->D, &E->Ops, w->Slot, &out);
    KeReleaseSpinLockFromDpcLevel(&E->Lock);
    Run(E, &out);
    out.Count = 0;
    KeAcquireSpinLockAtDpcLevel(&E->Lock);
    SlotTimerEnd(&E->D, &E->Ops, w->Slot, &out);
    KeReleaseSpinLockFromDpcLevel(&E->Lock);
    Run(E, &out);
}

static VOID CallerCancel(PDEVICE_OBJECT Dev, PIRP Irp)
{
    EXT *E = (EXT *)Dev->DeviceExtension;
    SLOT_ACTIONS out;
    KIRQL irql;
    int s, found = -1;
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    for (s = 0; s < SLOT_COUNT; s++) if (E->D.S[s].Irp == (void *)Irp) found = s;
    if (found >= 0) SlotCancelBegin(&E->D, &E->Ops, found, &out);
    KeReleaseSpinLock(&E->Lock, irql);
    Run(E, &out);
    if (found >= 0) {
        out.Count = 0;
        KeAcquireSpinLock(&E->Lock, &irql);
        SlotCancelEnd(&E->D, &E->Ops, found, &out);
        KeReleaseSpinLock(&E->Lock, irql);
        Run(E, &out);
    }
}

/* ------------------------------------------------------------------ admission */

/* Admit a caller IRP on slot s: the started gate, SlotAdmit, the request's
   own fields and SlotAdmitCheck, all under one hold of the lock. */
static NTSTATUS Admit(EXT *E, HCTX *H, PIRP Irp, int s, int Kind, ULONG Len, ULONG TimeoutMs, ULONG P1, ULONG P2,
                      PUCHAR Caller, int Reserve, int NeedClaim)
{
    SLOT_ACTIONS out;
    KIRQL irql;
    unsigned long r = 0;
    int started, removing, segs = 1;
    if (Kind == K_READ || Kind == K_WRITE) segs = (int)((Len + USBMTP_SEGMENT - 1) / USBMTP_SEGMENT);
    if (Kind == K_STRING && P2) segs = 2;      /* the languages first */
    out.Count = 0;
    IoMarkIrpPending(Irp);
    KeAcquireSpinLock(&E->Lock, &irql);
    SlotQueryExpire(&E->D, Ms(), QUERY_HOLD_MS);    /* the lapse, in the admission's own hold (5.4) */
    removing = E->D.Removing;
    started = E->Started;
    E->ArmMs = TimeoutMs;
    if (started && !removing) r = SlotAdmit(&E->D, &E->Ops, s, Irp, segs, TimeoutMs != 0, Reserve, NeedClaim,
                               H ? H->Epoch : 0, &out);
    if (r) {
        XSLOT *x = &E->X[s];
        x->Kind = Kind;
        x->Len = Len;
        x->Seg = 0;
        x->Param = P1;
        x->Param2 = P2;
        x->Caller = Caller;
        Irp->IoStatus.Status = STATUS_SUCCESS;     /* the lower status lands here on failure */
        Irp->IoStatus.Information = 0;
        SlotAdmitCheck(&E->D, &E->Ops, s, &out);
        /* An event queued before the wait was admitted ends it at once; the
           poll delivers only to an admitted wait. */
        if (Kind == K_WAIT && E->D.S[SLOT_WAIT].State == SS_ACTIVE &&
            TakeEvent(E, (USBMTP_EVENT *)Caller)) {
            Irp->IoStatus.Information = sizeof(USBMTP_EVENT);
            SlotStop(&E->D, &E->Ops, SLOT_WAIT, SO_SUCCESS, &out);
        }
    }
    KeReleaseSpinLock(&E->Lock, irql);
    if (removing || !started) {
        Done(Irp, removing ? STATUS_DELETE_PENDING : STATUS_DEVICE_NOT_READY, 0);
        return STATUS_PENDING;
    }
    if (!r) {
        Done(Irp, Outcome(out.A[0].Outcome, STATUS_SUCCESS), 0);
        return STATUS_PENDING;
    }
    Run(E, &out);
    return STATUS_PENDING;
}

/* Stop slot s's request: the driver's own stop (standby). */
static void StopSlot(EXT *E, int s)
{
    SLOT_ACTIONS out;
    KIRQL irql;
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    SlotStop(&E->D, &E->Ops, s, SO_CANCELLED, &out);
    KeReleaseSpinLock(&E->Lock, irql);
    Run(E, &out);
}

/* At PASSIVE_LEVEL: up to WaitMs milliseconds in all, one deadline, for
   every slot in Mask (bit s for slot s) to become idle. Returns the bits
   of those still not idle. */
static ULONG WaitSlotsIdle(EXT *E, ULONG Mask, ULONG WaitMs)
{
    KIRQL irql;
    ULONG t0 = Ms(), left;
    int s;
    for (;;) {
        left = 0;
        KeAcquireSpinLock(&E->Lock, &irql);
        for (s = 0; s < SLOT_COUNT; s++) {
            SLOT *S = &E->D.S[s];
            if ((Mask & (1UL << s)) && !(S->State == SS_IDLE && !S->Lower && !S->Pins && !S->Timer && !S->Cancel))
                left |= 1UL << s;
        }
        KeReleaseSpinLock(&E->Lock, irql);
        if (!left || Ms() - t0 >= WaitMs) return left;
        Sleep10();
    }
}


/* ------------------------------------------------------------------ power */

static VOID PowerWorker(PVOID Ctx);
static NTSTATUS SyncDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ev);
static int AllIdle(EXT *E);

/* The end of a dispatch without a handle (Late admitted it): its last
   access to the device. REMOVE waits for every admission before it
   forwards REMOVE and detaches, so the device lives at least until here. */
static void ActiveDone(EXT *E)
{
    KIRQL lk;
    KeAcquireSpinLock(&E->Lock, &lk);
    SlotActiveLeave(&E->D);
    KeReleaseSpinLock(&E->Lock, lk);
}

/* A power IRP's kind (5.4, "Power" and "Standby"). */
#define PK_OTHER     0
#define PK_SYS_DOWN  1  /* SET_POWER: a system state below working */
#define PK_SYS_UP    2  /* SET_POWER: S0 */
#define PK_DEV_DOWN  3  /* SET_POWER: a device state below D0 */
#define PK_DEV_UP    4  /* SET_POWER: D0 */
#define PK_SYS_QUERY 5  /* QUERY_POWER: a system state below working */
#define PK_DEV_QUERY 6  /* QUERY_POWER: a device state below D0 */

static int PowerKind(PIO_STACK_LOCATION s)
{
    if (s->MinorFunction == IRP_MN_QUERY_POWER) {
        if (s->Parameters.Power.Type == SystemPowerState &&
            s->Parameters.Power.State.SystemState > PowerSystemWorking)
            return PK_SYS_QUERY;
        if (s->Parameters.Power.Type == DevicePowerState && s->Parameters.Power.State.DeviceState > PowerDeviceD0)
            return PK_DEV_QUERY;
        return PK_OTHER;    /* a query for S0 or D0: passed down untouched */
    }
    if (s->MinorFunction != IRP_MN_SET_POWER) return PK_OTHER;
    if (s->Parameters.Power.Type == SystemPowerState)
        return s->Parameters.Power.State.SystemState > PowerSystemWorking ? PK_SYS_DOWN :
               s->Parameters.Power.State.SystemState == PowerSystemWorking ? PK_SYS_UP : PK_OTHER;
    if (s->Parameters.Power.Type == DevicePowerState)
        return s->Parameters.Power.State.DeviceState > PowerDeviceD0 ? PK_DEV_DOWN :
               s->Parameters.Power.State.DeviceState == PowerDeviceD0 ? PK_DEV_UP : PK_OTHER;
    return PK_OTHER;
}

/* At PASSIVE_LEVEL: wait until every slot is idle and every tail done,
   logging What once a second while it waits. No time limit: what ends the
   wait is the driver's own cancels (5.4). It waits for itself if the
   calling thread holds a tail or a pin (a completion or an outside call of
   a slot that reached this dispatch synchronously): not expected, an
   assumption of 5.4, and the log then shows the hang. */
static void QuietWait(EXT *E, const char *What)
{
    ULONG t0 = Ms(), said = 0;
    while (!AllIdle(E)) {
        if (Ms() - t0 >= said + 1000) {
            said += 1000;
            TRACE(What, said, 0);
        }
        Sleep10();
    }
}

/* At PASSIVE_LEVEL: wait until quiet (every slot idle, every tail done) or
   until Bound ms after T0, the tick read before the hold that closed the
   gate, logging What once a second with what is still out (SlotInfoHeld,
   read under the lock and traced after it). 1 at quiet, 0 at the bound.
   The bound bounds only this wait (5.4, "Standby"). */
static int QuietBounded(EXT *E, const char *What, ULONG T0, ULONG Bound)
{
    ULONG said = 0;
    KIRQL lk;
    int held;
    while (!AllIdle(E)) {
        if (Ms() - T0 >= Bound) return 0;
        if (Ms() - T0 >= said + 1000) {
            said += 1000;
            KeAcquireSpinLock(&E->Lock, &lk);
            held = SlotInfoHeld(&E->D);
            KeReleaseSpinLock(&E->Lock, lk);
            TRACE(What, said, held);
        }
        Sleep10();
    }
    return 1;
}

/* At PASSIVE_LEVEL, before a set goes down: until no submitter is between
   its commit and its IoCallDriver's return (SlotCalling 0). This waits on
   the submitters' own lower dispatch returning, not on any request's
   completion (5.4, "The submission"); it logs power-down-calling once a
   second while it lasts. */
static void CallsWait(EXT *E)
{
    ULONG t0 = Ms(), said = 0;
    KIRQL lk;
    int c;
    for (;;) {
        KeAcquireSpinLock(&E->Lock, &lk);
        c = SlotCalling(&E->D);
        KeReleaseSpinLock(&E->Lock, lk);
        if (!c) return;
        if (Ms() - t0 >= said + 1000) {
            said += 1000;
            TRACE("power-down-calling", said, c);
        }
        Sleep10();
    }
}

/* The completion routine of an accepted query (system or device), at any
   IRQL up to DISPATCH_LEVEL, inside the PoCallDriver or later: the lower
   drivers' final answer taken in one hold (SlotQueryEnd), which also gives
   up the query's own admission, the one SlotQueryDecide took. That hold is
   its last device access; the dispatch or the power worker that forwarded
   the query keeps its own admission to its own last access (5.4, "Two
   lifetimes at an accepted query"). Nothing waits for this routine (a
   power dispatch routine must not wait for an event that code handling the
   same IRP sets). The IRP then completes on up with the lower status; this
   routine completes nothing. */
static NTSTATUS QueryDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx)
{
    EXT *E = (EXT *)Ctx;
    int which = IoGetCurrentIrpStackLocation(Irp)->Parameters.Power.Type == DevicePowerState ? SLOT_QD : SLOT_QS;
    NTSTATUS st = Irp->IoStatus.Status;
    KIRQL lk;
    (void)Dev;
    if (Irp->PendingReturned) IoMarkIrpPending(Irp);
    TRACE("power-query-done", which, st);
    KeAcquireSpinLock(&E->Lock, &lk);
    SlotQueryEnd(&E->D, which, NT_SUCCESS(st), Ms());
    KeReleaseSpinLock(&E->Lock, lk);
    return STATUS_SUCCESS;
}

/* A query's preparation (slot.c): the gate closed for it and every slot
   stopped, in one hold; the cancels that hold returned are then sent. */
static void QueryBegin(EXT *E, int Which)
{
    SLOT_ACTIONS out;
    KIRQL lk;
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &lk);
    SlotQueryBegin(&E->D, &E->Ops, Which, &out);
    KeReleaseSpinLock(&E->Lock, lk);
    Run(E, &out);
}

/* The gate (slot.c): close it for Why and stop every slot, in one hold. */
static void GateDown(EXT *E, int Why)
{
    SLOT_ACTIONS out;
    KIRQL lk;
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &lk);
    SlotPowerDown(&E->D, &E->Ops, Why, &out);
    KeReleaseSpinLock(&E->Lock, lk);
    Run(E, &out);
}

static void GateUp(EXT *E, int Why)
{
    KIRQL lk;
    KeAcquireSpinLock(&E->Lock, &lk);
    SlotPowerUp(&E->D, Why);
    KeReleaseSpinLock(&E->Lock, lk);
}

/* A power IRP forwarded and waited for (S0, D0). */
static NTSTATUS PowerWait(EXT *E, PIRP Irp)
{
    KEVENT ev;
    NTSTATUS st;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, SyncDone, &ev, TRUE, TRUE, TRUE);
    st = PoCallDriver(E->Lower, Irp);
    if (st == STATUS_PENDING) KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
    return Irp->IoStatus.Status;
}

/* An admitted power IRP at PASSIVE_LEVEL (5.4, "Power" and "Standby"),
   on the dispatch or the power worker alike, its admission held by the
   caller throughout, who gives it up after the return in every case, an
   accepted query's included (its completion gives up only the query's own
   second admission: "Two lifetimes at an accepted query"). The driver
   requests no device power IRP: on 98 SE and ME it is not the device's
   power policy owner for a system sleep (5.4: under NUSB's EHCI a D3 the
   driver requested for a standby left the stack unable to finish the
   REMOVE it sends after the wake; without one, both stacks remove the
   device as the system goes to sleep). The gate's reasons are kept apart
   (slot.h, "The power gate").
   - A query below working, or another component's device query below D0:
     the gate closed for it and every slot stopped (SlotQueryBegin); the
     wait for quiet up to QUIET_QUERY_MS; one hold decides by the state
     then (SlotQueryDecide). Quiet: passed down with QueryDone as its
     completion routine, the IRP marked pending, no wait here. Not quiet:
     refused STATUS_DEVICE_BUSY, not passed down.
   - A set into sleep (system or device): the gate closed and every slot
     stopped (SlotPowerDown); the wait for quiet up to QUIET_SET_MS, then
     until no committed submit is in its call; passed down once, in any
     case, what is still out counted and quarantined (SlotSetForward).
   - S0, D0: forwarded and waited for. S0 ends the system query's reason
     whatever its result and the system's on success; D0 the same for the
     device query's and the device's. A failed D0 leaves the device's
     reason standing: the gate stays closed until the device is removed.
     DRAIN stays while anything stopped is out.
   - Anything else, queries for S0 or D0 included: passed down untouched.
   No lock is held across PoCallDriver, a wait or a completion. */
static NTSTATUS PowerForward(EXT *E, PIRP Irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    int kind = PowerKind(s), which, quiet, held;
    NTSTATUS st;
    ULONG t0;
    KIRQL lk;
    TRACE("power", ((ULONG)s->MinorFunction << 16) | (ULONG)s->Parameters.Power.Type,
          (ULONG)s->Parameters.Power.State.SystemState);
    PoStartNextPowerIrp(Irp);
    switch (kind) {
    case PK_SYS_QUERY:
    case PK_DEV_QUERY:
        which = kind == PK_DEV_QUERY ? SLOT_QD : SLOT_QS;
        t0 = Ms();
        QueryBegin(E, which);
        QuietBounded(E, "power-query-waiting", t0, QUIET_QUERY_MS);
        KeAcquireSpinLock(&E->Lock, &lk);
        quiet = SlotQueryDecide(&E->D, which);      /* 1: the query's own admission taken */
        held = E->D.HeldAtVeto;
        KeReleaseSpinLock(&E->Lock, lk);
        if (!quiet) {
            TRACE("power-veto", ((ULONG)which << 8) | (ULONG)held, Ms() - t0);
            return Done(Irp, STATUS_DEVICE_BUSY, 0);
        }
        TRACE("power-query", which, Ms() - t0);
        IoMarkIrpPending(Irp);
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp, QueryDone, E, TRUE, TRUE, TRUE);
        PoCallDriver(E->Lower, Irp);
        return STATUS_PENDING;
    case PK_SYS_DOWN:
    case PK_DEV_DOWN:
        t0 = Ms();
        GateDown(E, kind == PK_SYS_DOWN ? SLOT_PD_SYS : SLOT_PD_DEV);
        quiet = QuietBounded(E, "power-down-waiting", t0, QUIET_SET_MS);
        CallsWait(E);
        KeAcquireSpinLock(&E->Lock, &lk);
        held = SlotSetForward(&E->D);
        KeReleaseSpinLock(&E->Lock, lk);
        if (held) TRACE("power-down-held", held, kind);
        TRACE("power-down", kind, quiet ? 0 : 1);
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(E->Lower, Irp);
    case PK_SYS_UP:
    case PK_DEV_UP:
        st = PowerWait(E, Irp);
        if (kind == PK_SYS_UP) GateUp(E, SLOT_PD_QUERY | (NT_SUCCESS(st) ? SLOT_PD_SYS : 0));
        else GateUp(E, SLOT_PD_DQUERY | (NT_SUCCESS(st) ? SLOT_PD_DEV : 0));
        TRACE("power-up", kind, st);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return st;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(E->Lower, Irp);
}

/* An admitted power IRP. At PASSIVE_LEVEL it is handled here and its
   admission given up after, an accepted query's too (the query's own
   second admission is its completion's). One
   that comes above it is queued, by its own list entry, for the device's
   preallocated work item, carrying its admission: nothing on this path
   allocates. */
static NTSTATUS Power(EXT *E, PIRP Irp)
{
    KIRQL lk;
    NTSTATUS st;
    int queue;
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        st = PowerForward(E, Irp);
        ActiveDone(E);
        return st;
    }
    IoMarkIrpPending(Irp);
    KeAcquireSpinLock(&E->Lock, &lk);
    InsertTailList(&E->PowerQ, &Irp->Tail.Overlay.ListEntry);
    queue = SlotPowerQueue(&E->D);
    KeReleaseSpinLock(&E->Lock, lk);
    if (queue) {
        /* The worker is not running, so the IRP just queued keeps its
           admission, and the device, until the worker has taken it. */
        InterlockedIncrement(&g_Running);
        ExInitializeWorkItem(&E->PowerItem, PowerWorker, E);
        ExQueueWorkItem(&E->PowerItem, DelayedWorkQueue);
    }
    return STATUS_PENDING;
}

/* The power worker: each IRP is handled under the admission it carried,
   given up in the hold of the lock that takes the next one (SlotPowerNext),
   each forwarded as at PASSIVE_LEVEL. An accepted query is no exception:
   its completion may already have run and given up the query's own
   admission, so the worker's next hold is covered by the IRP's admission,
   which only that hold gives up (5.4, "Two lifetimes at an accepted
   query"). The hold that finds the list empty is its last device access;
   only the driver-global count DriverUnload waits for is left. */
static VOID PowerWorker(PVOID Ctx)
{
    EXT *E = (EXT *)Ctx;
    KIRQL lk;
    PIRP irp = NULL;
    PLIST_ENTRY le;
    int more;
    KeAcquireSpinLock(&E->Lock, &lk);
    more = SlotPowerNext(&E->D, 0);
    if (more) {
        le = RemoveHeadList(&E->PowerQ);     /* a statement macro in the DDK */
        irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
    }
    KeReleaseSpinLock(&E->Lock, lk);
    while (more) {
        PowerForward(E, irp);
        KeAcquireSpinLock(&E->Lock, &lk);
        more = SlotPowerNext(&E->D, 1);
        if (more) {
            le = RemoveHeadList(&E->PowerQ);
            irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        }
        KeReleaseSpinLock(&E->Lock, lk);
    }
    InterlockedDecrement(&g_Running);           /* the last act */
}

/* REMOVE: close the admission of dispatches without a handle, give up its
   own, and wait until every admitted one is done (power IRPs queued for
   the worker included). After it no power IRP reaches the lower stack.
   An assumption, not a proof: that no admitted dispatch's call chain
   waits for this REMOVE. The PnP manager sends a device one PnP IRP at a
   time, so no other PnP dispatch is in progress; a power IRP's or another
   major's call chain (a lower driver's dispatch, a completion above) is
   not expected to send REMOVE or to wait for it. Should one do so, this
   wait never ends: the removal hangs (logged once a second as
   remove-drain-waiting, with the admissions still out). */
static void ActiveDrain(EXT *E)
{
    KIRQL lk;
    int out;
    ULONG t0 = Ms(), said = 0;
    KeAcquireSpinLock(&E->Lock, &lk);
    SlotActiveClose(&E->D);
    SlotActiveLeave(&E->D);                     /* REMOVE's own admission */
    KeReleaseSpinLock(&E->Lock, lk);
    for (;;) {
        KeAcquireSpinLock(&E->Lock, &lk);
        out = SlotActiveOut(&E->D);
        KeReleaseSpinLock(&E->Lock, lk);
        if (!out) break;
        if (Ms() - t0 >= said + 1000) {
            said += 1000;
            TRACE("remove-drain-waiting", out, Ms() - t0);
        }
        Sleep10();
    }
}

/* ------------------------------------------------------------------ deletion */

static void FreeSlots(EXT *E)
{
    int i;
    for (i = 0; i < SLOT_COUNT; i++) {
        XSLOT *x = &E->X[i];
        if (x->Irp) IoFreeIrp(x->Irp);
        if (x->Urb) ExFreePool(x->Urb);
        if (x->Buf) ExFreePool(x->Buf);
        x->Irp = NULL;
        x->Urb = NULL;
        x->Buf = NULL;
    }
}

/* The one deleter (5.4): the party that claimed Deleting under the lock
   after finding the device detached and FreeReady set. It waits only for
   the dispatch pins, which no call is ever made under; everything that
   calls out was drained by REMOVE before the detach. IoDeleteDevice is
   the last act. */
static void DeleteDev(PDEVICE_OBJECT Dev)
{
    KIRQL irql;
    EXT *E = (EXT *)Dev->DeviceExtension;
    ULONG n = E->N;
    ULONG gen = E->Info.Generation;
    int pinned;
    /* Unpublish the device for IRPs without a handle, then wait for those
       already inside a dispatch routine (slot.h: SlotEnter). The hold that
       finds no pin also publishes the device as deleted (g_Deleted), which
       Late checks in the same lock before it pins: so no dispatch can enter
       between the last check and the deletion, whatever SlotEnter decides
       (mtp.md 5.4). */
    KeAcquireSpinLock(&g_Global, &irql);
    SlotGone(&E->D);
    KeReleaseSpinLock(&g_Global, irql);
    for (;;) {
        KeAcquireSpinLock(&g_Global, &irql);
        pinned = SlotPinned(&E->D);
        if (!pinned) {
            g_NUsed &= ~(1UL << n);     /* n is free only once the object is deleted (5.4) */
            g_Deleted = Dev;
        }
        KeReleaseSpinLock(&g_Global, irql);
        if (!pinned) break;
        Sleep10();
    }
    TRACE("deleting", gen, n);
    IoDeleteDevice(Dev);
}

/* AddDevice's failure: \\.\USBMTP<n> was never linked to a live device. */
static void FreeN(ULONG N)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_Global, &irql);
    g_NUsed &= ~(1UL << N);
    KeReleaseSpinLock(&g_Global, irql);
}

/* ------------------------------------------------------------------ dispatch */

/* The pin of an IRP without a handle: its dispatch's last access to E. */
static void Leave(EXT *E)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_Global, &irql);
    SlotLeave(&E->D);
    KeReleaseSpinLock(&g_Global, irql);
}

/* The late worker: completes the refused power IRPs at PASSIVE_LEVEL,
   touching no device. A completion it makes may close a zombie's last
   handle and delete the last device in this thread, which runs
   DriverUnload here too; DriverUnload then does not wait for this worker
   (it is its own caller) and the worker returns through its own code, as
   every deleter does after IoDeleteDevice (mtp.md 5.4). */
static VOID LateWorker(PVOID Ctx)
{
    KIRQL irql;
    PIRP irp;
    PLIST_ENTRY le;
    (void)Ctx;
    g_LateThread = KeGetCurrentThread();
    for (;;) {
        irp = NULL;
        KeAcquireSpinLock(&g_Global, &irql);
        if (IsListEmpty(&g_LateQ)) {
            g_LateQueued = 0;
            g_LateThread = NULL;
        } else {
            le = RemoveHeadList(&g_LateQ);       /* a statement macro in the DDK */
            irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        }
        KeReleaseSpinLock(&g_Global, irql);
        if (!irp) break;
        PoStartNextPowerIrp(irp);
        Done(irp, STATUS_DELETE_PENDING, 0);
    }
    InterlockedDecrement(&g_LateRunning);       /* the last act */
}

/* A refused power IRP that came above PASSIVE_LEVEL, already marked
   pending, onto the late list, under g_Global: in the hold in which the
   dispatch found the device deleted or unpublished, or while it still
   holds its pin. So the late count is raised before the deleter can
   delete and DriverUnload read it (mtp.md 5.4). Returns 1 when the caller
   must queue the worker (LateKick) after releasing the lock. */
static int LateAddLocked(PIRP Irp)
{
    InsertTailList(&g_LateQ, &Irp->Tail.Overlay.ListEntry);
    if (g_LateQueued) return 0;
    g_LateQueued = 1;
    InterlockedIncrement(&g_LateRunning);
    return 1;
}

static void LateKick(int Queue)
{
    TRACE("power-late", Queue, 0);
    if (!Queue) return;
    ExInitializeWorkItem(&g_LateItem, LateWorker, NULL);
    ExQueueWorkItem(&g_LateItem, DelayedWorkQueue);
}

/* An IRP without a handle for a device that is gone, unpublished or
   detached, at PASSIVE_LEVEL if it is a power IRP (one above it went to
   the late list): completed from the driver's globals, touching nothing
   of the device. */
static NTSTATUS Unpublished(PIRP Irp, UCHAR Mj)
{
    /* Completed STATUS_DELETE_PENDING, never passed down: a detached device
       has no lower stack, and one closed to new IRPs is about to lose it.
       Nothing of the device is queued or counted for it, so a close its
       completion makes, which may delete the device, waits on nothing this
       dispatch holds. */
    if (Mj == IRP_MJ_POWER) PoStartNextPowerIrp(Irp);
    return Done(Irp, STATUS_DELETE_PENDING, 0);
}

/* Every dispatch routine's first step: an IRP for a deleted device, a
   handle of another device, or a zombie (removed, kept for its handles).
   Returns 1 with the IRP completed and its status in *St.
   The lifetime rule (mtp.md 5.4): an IRP on an open handle keeps the
   device alive by that handle (only the last close deletes). An IRP
   without a handle (PnP, power, create, any other major) pins the device
   under the driver-global lock (SlotEnter) after checking it is not the
   device already deleted; the deleter unpublishes it (SlotGone) and waits
   for the pins under that lock before IoDeleteDevice. The pin covers only
   Late's own reads and is left before Late returns: no pin, like no lock,
   is ever held across a call down, a completion or anything else that can
   re-enter the driver. What keeps the device for the dispatch itself is
   its admission (slot.c's SlotActiveEnter), taken under the device lock
   while the device is neither detached nor closed to them; REMOVE waits
   for every admission before it forwards REMOVE and detaches, and nothing
   deletes a device before its detach. So a close in an admitted
   dispatch's call chain never claims the deletion, and the deleter never
   waits on an admission. The dispatch gives the admission up
   (ActiveDone) as its last access. */
static int Late(PDEVICE_OBJECT Dev, PIRP Irp, NTSTATUS *St)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    UCHAR mj = s->MajorFunction;
    PFILE_OBJECT fo = s->FileObject;
    int handle = mj == IRP_MJ_READ || mj == IRP_MJ_WRITE || mj == IRP_MJ_DEVICE_CONTROL ||
                 mj == IRP_MJ_CLEANUP || mj == IRP_MJ_CLOSE;
    HCTX *h = handle && fo ? (HCTX *)fo->FsContext : NULL;
    EXT *E;
    SLOT_ACTIONS out;
    KIRQL irql;
    ULONG gen = 0;
    int zombie, del = 0, pinned, admitted, late, queue = 0;
    if (!handle) {
        /* A power IRP above PASSIVE_LEVEL is completed, if refused, by the
           late worker: marked pending now (an admitted one is pending too). */
        late = mj == IRP_MJ_POWER && KeGetCurrentIrql() > PASSIVE_LEVEL;
        if (late) IoMarkIrpPending(Irp);
        KeAcquireSpinLock(&g_Global, &irql);
        pinned = Dev != g_Deleted && SlotEnter(&((EXT *)Dev->DeviceExtension)->D);
        if (!pinned && late) queue = LateAddLocked(Irp);
        KeReleaseSpinLock(&g_Global, irql);
        if (!pinned) {
            if (late) { LateKick(queue); *St = STATUS_PENDING; }
            else *St = Unpublished(Irp, mj);
            return 1;
        }
        E = (EXT *)Dev->DeviceExtension;
        KeAcquireSpinLock(&E->Lock, &irql);
        admitted = SlotActiveEnter(&E->D);  /* the dispatch's ActiveDone gives it up */
        KeReleaseSpinLock(&E->Lock, irql);
        if (!admitted && late) {
            /* onto the late list while the pin is still held, then the pin */
            KeAcquireSpinLock(&g_Global, &irql);
            queue = LateAddLocked(Irp);
            SlotLeave(&E->D);
            KeReleaseSpinLock(&g_Global, irql);
            LateKick(queue);
            *St = STATUS_PENDING;
            return 1;
        }
        Leave(E);                           /* the pin ends here, before any call */
        if (admitted) return 0;
        *St = Unpublished(Irp, mj);
        return 1;
    }
    if (Dev == g_Deleted) {
        if (mj == IRP_MJ_CLOSE && h) { fo->FsContext = NULL; ExFreePool(h); }
        *St = (mj == IRP_MJ_CLEANUP || mj == IRP_MJ_CLOSE) ? STATUS_SUCCESS : STATUS_DELETE_PENDING;
        Done(Irp, *St, 0);
        return 1;
    }
    E = (EXT *)Dev->DeviceExtension;
    if (handle && (!h || h->Instance != E->Instance)) {
        InterlockedIncrement(&g_Foreign);
        TRACE("foreign", mj, h ? h->Instance : 0);
        *St = (mj == IRP_MJ_CLEANUP || mj == IRP_MJ_CLOSE) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
        Done(Irp, *St, 0);
        return 1;
    }
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    zombie = E->D.Detached;
    if (zombie && mj == IRP_MJ_CLOSE) {
        del = SlotClose(&E->D, &out);   /* claims the deletion, or not */
        gen = E->Info.Generation;
    }
    KeReleaseSpinLock(&E->Lock, irql);
    if (!zombie) return 0;
    /* SlotClose's actions are only SA_REMOVE_READY and SA_FREE_READY, which
       need no performing: from here a close that did not claim the deletion
       touches nothing of the device (another close may be deleting it). */
    if (mj == IRP_MJ_CLOSE) {
        fo->FsContext = NULL;
        if (h) ExFreePool(h);
        *St = STATUS_SUCCESS;
        Done(Irp, STATUS_SUCCESS, 0);
        if (del) {
            TRACE("close-deletes", gen, 0);
            DeleteDev(Dev);
        }
        return 1;
    }
    *St = mj == IRP_MJ_CLEANUP ? STATUS_SUCCESS : STATUS_DELETE_PENDING;
    Done(Irp, *St, 0);
    return 1;
}

static NTSTATUS DispCreate(PDEVICE_OBJECT Dev, PIRP Irp)
{
    EXT *E;
    NTSTATUS st;
    PFILE_OBJECT fo = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    HCTX *h;
    KIRQL irql;
    int ok;
    if (Late(Dev, Irp, &st)) return st;
    E = (EXT *)Dev->DeviceExtension;
    h = fo ? (HCTX *)ExAllocatePoolWithTag(NonPagedPool, sizeof(HCTX), POOL_TAG) : NULL;
    if (!h) {
        ActiveDone(E);
        return Done(Irp, fo ? STATUS_INSUFFICIENT_RESOURCES : STATUS_INVALID_PARAMETER, 0);
    }
    RtlZeroMemory(h, sizeof(*h));
    h->Instance = E->Instance;
    h->Rights = R_INFO;
    KeAcquireSpinLock(&E->Lock, &irql);
    ok = SlotOpen(&E->D);
    KeReleaseSpinLock(&E->Lock, irql);
    ActiveDone(E);                              /* the handle, if opened, keeps the device from here */
    if (!ok) {
        ExFreePool(h);
        return Done(Irp, STATUS_DELETE_PENDING, 0);
    }
    fo->FsContext = h;
    return Done(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS DispCleanupClose(PDEVICE_OBJECT Dev, PIRP Irp)
{
    EXT *E;
    NTSTATUS st;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    HCTX *h;
    SLOT_ACTIONS out;
    KIRQL irql;
    ULONG gen;
    int owner, del;
    if (Late(Dev, Irp, &st)) return st;
    E = (EXT *)Dev->DeviceExtension;
    h = (HCTX *)fo->FsContext;
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    owner = h->Rights == R_OWNER && E->Owner == fo && E->D.Claim == SC_ACTIVE && h->Epoch == E->D.Epoch &&
            (s->MajorFunction == IRP_MJ_CLEANUP || !h->CleanedUp);
    if (owner) {
        /* Release (5.1): revoke at once, stop every slot; FREE when the
           last slot is idle (slot.c). */
        SlotRelease(&E->D, &E->Ops, &out);
        E->Owner = NULL;
        E->TokenLow = E->TokenHigh = 0;
    } else if (E->D.S[SLOT_WAIT].State == SS_ACTIVE && E->D.S[SLOT_WAIT].Irp &&
               IoGetCurrentIrpStackLocation((PIRP)E->D.S[SLOT_WAIT].Irp)->FileObject == fo) {
        SlotStop(&E->D, &E->Ops, SLOT_WAIT, SO_CANCELLED, &out);   /* a bound handle's own wait, nothing else */
    }
    KeReleaseSpinLock(&E->Lock, irql);
    Run(E, &out);
    if (s->MajorFunction == IRP_MJ_CLEANUP) {
        h->CleanedUp = 1;
        return Done(Irp, STATUS_SUCCESS, 0);
    }
    /* close: the handle is gone. REMOVE may have detached since Late
       looked: SlotClose decides the deletion in this hold of the lock. */
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    del = SlotClose(&E->D, &out);
    gen = E->Info.Generation;
    KeReleaseSpinLock(&E->Lock, irql);
    /* As in Late: SlotClose's actions need no performing, and a close that
       did not claim the deletion touches nothing of the device from here
       (REMOVE may be deleting it). */
    fo->FsContext = NULL;
    ExFreePool(h);
    Done(Irp, STATUS_SUCCESS, 0);
    if (del) {
        TRACE("close-deletes", gen, 1);
        DeleteDev(Dev);                         /* the last act */
    }
    return STATUS_SUCCESS;
}

static PUCHAR SysAddr(PIRP Irp)
{
    return Irp->MdlAddress ? (PUCHAR)MmGetSystemAddressForMdl(Irp->MdlAddress) : NULL;
}

/* Under the lock: the handle's claim is the active one; with Owner, it is
   the owner handle itself. */
static int Authority(EXT *E, HCTX *H, PFILE_OBJECT Fo, int Owner)
{
    if (E->D.Claim != SC_ACTIVE || E->D.Removing || H->Epoch != E->D.Epoch || H->Rights == R_INFO) return 0;
    if (Owner || H->Rights == R_OWNER) return H->Rights == R_OWNER && E->Owner == Fo;
    return 1;
}

/* An owner call's check: the handle owns the active claim. */
static int IsOwner(EXT *E, HCTX *H, PFILE_OBJECT Fo)
{
    return H->Rights == R_OWNER && E->Owner == Fo;
}

static NTSTATUS DispReadWrite(PDEVICE_OBJECT Dev, PIRP Irp)
{
    EXT *E;
    NTSTATUS st;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    HCTX *h;
    ULONG len;
    PUCHAR buf;
    if (Late(Dev, Irp, &st)) return st;
    E = (EXT *)Dev->DeviceExtension;
    h = (HCTX *)s->FileObject->FsContext;
    if (!IsOwner(E, h, s->FileObject)) return Done(Irp, STATUS_ACCESS_DENIED, 0);
    if (s->MajorFunction == IRP_MJ_READ) {
        len = s->Parameters.Read.Length;
        if (!E->Info.MaxPacketIn || len < E->Info.MaxPacketIn || len > USBMTP_MAX_TRANSFER || len % E->Info.MaxPacketIn)
            return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        buf = SysAddr(Irp);
        if (!buf) return Done(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        return Admit(E, h, Irp, SLOT_BIN, K_READ, len, E->BulkInMs, 0, 0, buf, -1, 1);
    }
    len = s->Parameters.Write.Length;
    if (!len || len > USBMTP_MAX_TRANSFER) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
    buf = SysAddr(Irp);
    if (!buf) return Done(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    return Admit(E, h, Irp, SLOT_BOUT, K_WRITE, len, E->BulkOutMs, 0, 0, buf, -1, 1);
}

static NTSTATUS DispIoctl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    EXT *E;
    NTSTATUS st;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    HCTX *h;
    ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
    ULONG in = s->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out = s->Parameters.DeviceIoControl.OutputBufferLength;
    PUCHAR buf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    KIRQL irql;
    int rights;
    if (Late(Dev, Irp, &st)) return st;
    E = (EXT *)Dev->DeviceExtension;
    h = (HCTX *)fo->FsContext;
    /* The handle's kind only; whether its claim is still the active one is
       checked again in the hold of the lock that acts (Authority, SlotAdmit,
       SlotStopEpoch, SlotPollStart), never from a value read before. */
    rights = h->Rights;

    switch (code) {
    case IOCTL_USBMTP_GET_INFO: {
        /* 40 bytes (1.0) to the whole structure: the caller's length picks
           how much; cbSize says what the driver has (5.1). */
        ULONG n = out < sizeof(USBMTP_INFO) ? out : sizeof(USBMTP_INFO);
        if (in || out < USBMTP_INFO_SIZE_1_0) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        KeAcquireSpinLock(&E->Lock, &irql);
        SlotQueryExpire(&E->D, Ms(), QUERY_HOLD_MS);    /* the lapse, before what it reports */
        E->Info.Flags = (UCHAR)((E->D.Claim != SC_FREE ? USBMTP_FLAG_CLAIMED : 0) |
                                (E->D.Removing ? USBMTP_FLAG_REMOVING : 0));
        /* interface 1.1: the power fields, all in this hold (5.4, "What
           user mode can read") */
        E->Info.PowerGate = E->D.PowerDown;
        E->Info.Held = (UCHAR)SlotInfoHeld(&E->D);
        E->Info.HeldAtVeto = E->D.HeldAtVeto;
        E->Info.Vetoes = E->D.Vetoes;
        E->Info.Forced = E->D.Forced;
        RtlCopyMemory(buf, &E->Info, n);
        KeReleaseSpinLock(&E->Lock, irql);
        return Done(Irp, STATUS_SUCCESS, n);
    }

    case IOCTL_USBMTP_GET_STRING: {
        USBMTP_STRING_IN si;
        if (in != sizeof(si) || out != 256) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        RtlCopyMemory(&si, buf, sizeof(si));
        if (!si.Index) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        return Admit(E, h, Irp, SLOT_CTL, K_STRING, 255, 2000, (ULONG)si.Index | ((ULONG)si.LangId << 16),
                     si.LangId ? 0 : 1, buf, -1, 0);
    }

    case IOCTL_USBMTP_CLAIM: {
        USBMTP_CLAIM_INFO ci;
        unsigned long ep;
        SLOT_ACTIONS sa;
        if (in || out != sizeof(ci)) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        if (h->Rights != R_INFO) return Done(Irp, STATUS_ACCESS_DENIED, 0);
        sa.Count = 0;
        KeAcquireSpinLock(&E->Lock, &irql);
        SlotQueryExpire(&E->D, Ms(), QUERY_HOLD_MS);    /* the lapse, in this hold */
        if (E->D.Removing) st = STATUS_DELETE_PENDING;
        else if (!E->Started || E->D.PowerDown) st = STATUS_DEVICE_NOT_READY;
        else if (E->D.Claim == SC_ACTIVE) st = STATUS_SHARING_VIOLATION;
        else if (E->D.Claim == SC_RELEASING) st = STATUS_DEVICE_BUSY;
        else {
            ep = SlotClaim(&E->D);
            if (!ep) st = STATUS_DEVICE_BUSY;       /* the epoch space is spent: replug */
            else {
                st = STATUS_SUCCESS;
                h->Rights = R_OWNER;
                h->Epoch = ep;
                E->Owner = fo;
                g_Token++;
                E->TokenLow = g_Token;
                E->TokenHigh = Ms() ^ (E->Info.Generation << 16) ^ 0x4D545000UL;
                ci.TokenLow = E->TokenLow;
                ci.TokenHigh = E->TokenHigh;
                ci.Generation = E->Info.Generation;
                ci.Epoch = ep;
                E->QHead = E->QCount = E->QLost = 0;
                E->X[SLOT_INT].Kind = K_POLL;
                SlotPollStart(&E->D, &E->Ops, ep, &sa);
            }
        }
        KeReleaseSpinLock(&E->Lock, irql);
        Run(E, &sa);
        if (!NT_SUCCESS(st)) return Done(Irp, st, 0);
        RtlCopyMemory(buf, &ci, sizeof(ci));
        return Done(Irp, STATUS_SUCCESS, sizeof(ci));
    }

    case IOCTL_USBMTP_BIND: {
        USBMTP_CLAIM_INFO ci;
        if (in != sizeof(ci) || out) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        RtlCopyMemory(&ci, buf, sizeof(ci));
        KeAcquireSpinLock(&E->Lock, &irql);
        if (h->Rights == R_INFO && E->D.Claim == SC_ACTIVE && ci.TokenLow == E->TokenLow &&
            ci.TokenHigh == E->TokenHigh && ci.Generation == E->Info.Generation && ci.Epoch == E->D.Epoch) {
            h->Rights = R_BOUND;
            h->Epoch = ci.Epoch;
            st = STATUS_SUCCESS;
        } else {
            st = STATUS_ACCESS_DENIED;
        }
        KeReleaseSpinLock(&E->Lock, irql);
        return Done(Irp, st, 0);
    }

    case IOCTL_USBMTP_CLASS_REQUEST: {
        USBMTP_CLASS_IN ci;
        if (rights == R_INFO) return Done(Irp, STATUS_ACCESS_DENIED, 0);
        if (in != sizeof(ci)) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        RtlCopyMemory(&ci, buf, sizeof(ci));
        switch (ci.Request) {
        case USBMTP_REQ_CANCEL:
        case USBMTP_REQ_RESET:
            if (out || ci.DataLength) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
            break;
        case USBMTP_REQ_STATUS:
            if (ci.TransactionId || ci.DataLength < 4 || ci.DataLength > 64 || out != ci.DataLength)
                return Done(Irp, STATUS_INVALID_PARAMETER, 0);
            break;
        case USBMTP_REQ_EXT_EVENT:
            if (ci.TransactionId || ci.DataLength < 8 || ci.DataLength > 512 || out != ci.DataLength)
                return Done(Irp, STATUS_INVALID_PARAMETER, 0);
            break;
        default:
            return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        }
        if (ci.Request != USBMTP_REQ_CANCEL && ci.TransactionId) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        return Admit(E, h, Irp, SLOT_CTL, K_CLASS, ci.DataLength, 2000, ci.Request, ci.TransactionId, buf, -1, 1);
    }

    case IOCTL_USBMTP_RESET_PIPE: {
        ULONG pipe;
        if (rights == R_INFO) return Done(Irp, STATUS_ACCESS_DENIED, 0);
        if (in != sizeof(ULONG) || out) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        pipe = *(ULONG *)buf;
        if (pipe < USBMTP_PIPE_BULK_IN || pipe > USBMTP_PIPE_INT_IN) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        return Admit(E, h, Irp, SLOT_CTL, K_RESET, 0, 2000, pipe, 0, NULL,
                     pipe == USBMTP_PIPE_BULK_IN ? SLOT_BIN : pipe == USBMTP_PIPE_BULK_OUT ? SLOT_BOUT : SLOT_INT, 1);
    }

    case IOCTL_USBMTP_ABORT: {
        USBMTP_WAIT_IN wi;
        ULONG left, mask = 0;
        SLOT_ACTIONS sa;
        int ok = 1;
        if (rights == R_INFO) return Done(Irp, STATUS_ACCESS_DENIED, 0);
        if (in != sizeof(wi) || out != sizeof(ULONG)) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        RtlCopyMemory(&wi, buf, sizeof(wi));
        if (!wi.What || (wi.What & ~(ULONG)(USBMTP_MASK_BULK_IN | USBMTP_MASK_BULK_OUT)) || wi.WaitMs > 5000)
            return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        if (wi.What & USBMTP_MASK_BULK_IN) mask |= 1UL << SLOT_BIN;
        if (wi.What & USBMTP_MASK_BULK_OUT) mask |= 1UL << SLOT_BOUT;
        sa.Count = 0;
        KeAcquireSpinLock(&E->Lock, &irql);     /* the claim's check and both stops: one hold */
        if (mask & (1UL << SLOT_BIN)) ok = SlotStopEpoch(&E->D, &E->Ops, SLOT_BIN, SO_CANCELLED, h->Epoch, &sa);
        if (ok && (mask & (1UL << SLOT_BOUT))) ok = SlotStopEpoch(&E->D, &E->Ops, SLOT_BOUT, SO_CANCELLED, h->Epoch, &sa);
        KeReleaseSpinLock(&E->Lock, irql);
        Run(E, &sa);
        if (!ok) return Done(Irp, STATUS_ACCESS_DENIED, 0);
        left = WaitSlotsIdle(E, mask, wi.WaitMs);   /* one deadline for both */
        *(ULONG *)buf = ((left & (1UL << SLOT_BIN)) ? USBMTP_MASK_BULK_IN : 0) |
                        ((left & (1UL << SLOT_BOUT)) ? USBMTP_MASK_BULK_OUT : 0);
        return Done(Irp, STATUS_SUCCESS, sizeof(ULONG));
    }

    case IOCTL_USBMTP_EVENTS: {
        USBMTP_WAIT_IN wi;
        SLOT_ACTIONS sa;
        if (rights == R_INFO) return Done(Irp, STATUS_ACCESS_DENIED, 0);
        if (in != sizeof(wi) || out != sizeof(ULONG)) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        RtlCopyMemory(&wi, buf, sizeof(wi));
        if (wi.WaitMs > 5000 || (wi.What != USBMTP_EVENTS_STOP && wi.What != USBMTP_EVENTS_START))
            return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        sa.Count = 0;
        KeAcquireSpinLock(&E->Lock, &irql);     /* the claim's check and the change: one hold */
        if (wi.What == USBMTP_EVENTS_START) SlotQueryExpire(&E->D, Ms(), QUERY_HOLD_MS);    /* the lapse */
        if (wi.What == USBMTP_EVENTS_STOP) {
            st = SlotStopEpoch(&E->D, &E->Ops, SLOT_INT, SO_CANCELLED, h->Epoch, &sa) ? STATUS_SUCCESS
                                                                                         : STATUS_ACCESS_DENIED;
        } else if (!E->Started || E->D.PowerDown) {
            st = STATUS_DEVICE_NOT_READY;       /* the started and power gates hold for the poll too */
        } else {
            E->X[SLOT_INT].Kind = K_POLL;
            st = SlotPollStart(&E->D, &E->Ops, h->Epoch, &sa) ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
        }
        KeReleaseSpinLock(&E->Lock, irql);
        Run(E, &sa);
        if (!NT_SUCCESS(st)) return Done(Irp, st, 0);
        *(ULONG *)buf = wi.What == USBMTP_EVENTS_STOP ? (WaitSlotsIdle(E, 1UL << SLOT_INT, wi.WaitMs) ? 0 : 1) : 0;
        return Done(Irp, STATUS_SUCCESS, sizeof(ULONG));
    }

    case IOCTL_USBMTP_SET_TIMEOUTS: {
        USBMTP_TIMEOUTS t;
        if (rights != R_OWNER) return Done(Irp, STATUS_ACCESS_DENIED, 0);
        if (in != sizeof(t) || out) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        RtlCopyMemory(&t, buf, sizeof(t));
        if (t.BulkInMs < 100 || t.BulkInMs > 600000 || t.BulkOutMs < 100 || t.BulkOutMs > 600000)
            return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        KeAcquireSpinLock(&E->Lock, &irql);
        st = Authority(E, h, fo, 1) ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
        if (NT_SUCCESS(st)) {
            E->BulkInMs = t.BulkInMs;
            E->BulkOutMs = t.BulkOutMs;
        }
        KeReleaseSpinLock(&E->Lock, irql);
        return Done(Irp, st, 0);
    }

    case IOCTL_USBMTP_WRITE_ZLP:
        if (rights != R_OWNER) return Done(Irp, STATUS_ACCESS_DENIED, 0);
        if (in || out) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        return Admit(E, h, Irp, SLOT_BOUT, K_ZLP, 0, E->BulkOutMs, 0, 0, NULL, -1, 1);

    case IOCTL_USBMTP_WAIT_EVENT: {
        ULONG ms;
        USBMTP_EVENT ev;
        BOOLEAN got;
        if (rights == R_INFO) return Done(Irp, STATUS_ACCESS_DENIED, 0);
        if (in != sizeof(ULONG) || out != sizeof(USBMTP_EVENT)) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        ms = *(ULONG *)buf;
        if (ms > 60000) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        RtlZeroMemory(buf, sizeof(USBMTP_EVENT));
        if (!ms) {                              /* a look at the queue, no wait */
            KeAcquireSpinLock(&E->Lock, &irql);
            st = Authority(E, h, fo, 0) ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
            got = NT_SUCCESS(st) && TakeEvent(E, &ev);
            KeReleaseSpinLock(&E->Lock, irql);
            if (!NT_SUCCESS(st)) return Done(Irp, st, 0);
            if (!got) return Done(Irp, STATUS_IO_TIMEOUT, 0);
            RtlCopyMemory(buf, &ev, sizeof(ev));
            return Done(Irp, STATUS_SUCCESS, sizeof(ev));
        }
        return Admit(E, h, Irp, SLOT_WAIT, K_WAIT, 0, ms, 0, 0, buf, -1, 1);
    }
    }
    return Done(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

/* ------------------------------------------------------------------ PnP */

static NTSTATUS SyncDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ev)
{
    (void)Dev; (void)Irp;
    KeSetEvent((PKEVENT)Ev, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS Sync(EXT *E, PURB Urb)
{
    KEVENT ev;
    IO_STATUS_BLOCK io;
    PIRP irp;
    NTSTATUS st;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, E->Lower, NULL, 0, NULL, 0, TRUE, &ev, &io);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = Urb;
    st = IoCallDriver(E->Lower, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = io.Status;
    }
    /* Success needs the IRP and the URB (5.4). */
    if (NT_SUCCESS(st) && Urb->UrbHeader.Status != USBD_STATUS_SUCCESS) st = STATUS_UNSUCCESSFUL;
    return st;
}

/* A descriptor read: success only with exactly Want bytes returned. */
static NTSTATUS GetDescriptor(EXT *E, UCHAR Type, PVOID Buf, ULONG Want)
{
    URB u;
    NTSTATUS st;
    RtlZeroMemory(Buf, Want);
    UsbBuildGetDescriptorRequest(&u, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST), Type, 0, 0, Buf, NULL, Want, NULL);
    st = Sync(E, &u);
    if (NT_SUCCESS(st) && u.UrbControlDescriptorRequest.TransferBufferLength != Want) st = STATUS_DEVICE_DATA_ERROR;
    return st;
}

/* The interface the device node stands for (3.3): nn from "&MI_nn" in the
   node's first hardware id, -1 for a whole device; -2 if it cannot be read. */
static int NodeInterface(EXT *E)
{
    WCHAR *b;
    ULONG len = 0, nch, i;
    int nn = -1;
    NTSTATUS st;
    b = (WCHAR *)ExAllocatePoolWithTag(NonPagedPool, 1024, POOL_TAG);
    if (!b) return -2;
    RtlZeroMemory(b, 1024);
    st = IoGetDeviceProperty(E->Pdo, DevicePropertyHardwareID, 1024 - sizeof(WCHAR), b, &len);
    if (!NT_SUCCESS(st) || len < 2 * sizeof(WCHAR)) {
        ExFreePool(b);
        return -2;
    }
    nch = 1024 / sizeof(WCHAR) - 1;     /* the last stays 0 */
    for (i = 0; i + 5 < nch && b[i]; i++) {
        WCHAR h = b[i + 4], l = b[i + 5];
        int hv, lv;
        if (b[i] != L'&' || (b[i + 1] | 0x20) != L'm' || (b[i + 2] | 0x20) != L'i' || b[i + 3] != L'_') continue;
        hv = h >= L'0' && h <= L'9' ? h - L'0' : (h | 0x20) >= L'a' && (h | 0x20) <= L'f' ? (h | 0x20) - L'a' + 10 : -1;
        lv = l >= L'0' && l <= L'9' ? l - L'0' : (l | 0x20) >= L'a' && (l | 0x20) <= L'f' ? (l | 0x20) - L'a' + 10 : -1;
        if (hv >= 0 && lv >= 0) nn = hv * 16 + lv;
        break;
    }
    ExFreePool(b);
    return nn;
}

/* Identification and configuration (3.3), from descriptors only: the
   interface the node stands for (an interface node's nn; a whole device
   only with exactly one interface), of class 06/01/01 or FF, with exactly
   one bulk IN, one bulk OUT and one interrupt IN endpoint. Every reply is
   checked (the IRP, the URB, the length) and the configuration is walked
   whole by CfgWalk (cfgwalk.c) before USBD sees any of it. The selection
   lists the owned interface alone, in a list of bNumInterfaces + 1 zeroed
   entries. Anything else fails start. */
static NTSTATUS Start(EXT *E)
{
    USB_DEVICE_DESCRIPTOR dd;
    PUCHAR cd;
    PUSB_INTERFACE_DESCRIPTOR id;
    PUSBD_INTERFACE_LIST_ENTRY list;
    PUSBD_INTERFACE_INFORMATION ii;
    PURB sel;
    CFG_PICK pk;
    NTSTATUS st;
    ULONG i, n;
    UCHAR head[9];
    USHORT total;
    int nn, w, super;
    E->PipeIn = E->PipeOut = E->PipeInt = NULL;
    nn = NodeInterface(E);
    if (nn == -2) return STATUS_DEVICE_CONFIGURATION_ERROR;
    st = GetDescriptor(E, USB_DEVICE_DESCRIPTOR_TYPE, &dd, sizeof(dd));
    if (!NT_SUCCESS(st)) return st;
    if (DevCheck((const unsigned char *)&dd, sizeof(dd), &super) != CW_OK) return STATUS_DEVICE_DATA_ERROR;
    E->Info.Vid = dd.idVendor;
    E->Info.Pid = dd.idProduct;
    E->Info.BcdDevice = dd.bcdDevice;
    st = GetDescriptor(E, USB_CONFIGURATION_DESCRIPTOR_TYPE, head, 9);
    if (!NT_SUCCESS(st)) return st;
    total = (USHORT)(head[2] | (head[3] << 8));
    if (head[0] < 9 || head[1] != USB_CONFIGURATION_DESCRIPTOR_TYPE || total < 9 || total > 4096 || !head[4])
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    cd = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, total, POOL_TAG);
    if (!cd) return STATUS_INSUFFICIENT_RESOURCES;
    st = GetDescriptor(E, USB_CONFIGURATION_DESCRIPTOR_TYPE, cd, total);
    if (!NT_SUCCESS(st)) {
        ExFreePool(cd);
        return st;
    }
    RtlZeroMemory(&pk, sizeof(pk));
    w = CfgWalk(cd, total, nn, super, dd.bMaxPacketSize0, &pk);
    TRACE("cfgwalk", w, ((ULONG)(nn & 0xFF) << 24) | ((ULONG)pk.NumInterfaces << 16) | total);
    if (w != CW_OK) {
        ExFreePool(cd);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    n = pk.NumInterfaces;
    list = (PUSBD_INTERFACE_LIST_ENTRY)ExAllocatePoolWithTag(NonPagedPool, (n + 1) * sizeof(USBD_INTERFACE_LIST_ENTRY),
                                                             POOL_TAG);
    if (!list) {
        ExFreePool(cd);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(list, (n + 1) * sizeof(USBD_INTERFACE_LIST_ENTRY));
    id = (PUSB_INTERFACE_DESCRIPTOR)(cd + pk.IfOffset);
    list[0].InterfaceDescriptor = id;
    sel = USBD_CreateConfigurationRequestEx((PUSB_CONFIGURATION_DESCRIPTOR)cd, list);
    if (!sel || !list[0].Interface || list[0].Interface->NumberOfPipes != 3) {
        if (sel) ExFreePool(sel);
        ExFreePool(list);
        ExFreePool(cd);
        return sel ? STATUS_DEVICE_CONFIGURATION_ERROR : STATUS_INSUFFICIENT_RESOURCES;
    }
    ii = list[0].Interface;
    for (i = 0; i < ii->NumberOfPipes; i++) ii->Pipes[i].MaximumTransferSize = USBMTP_SEGMENT;
    st = Sync(E, sel);
    if (NT_SUCCESS(st)) {
        E->Info.Interface = pk.Interface;
        E->Info.Class = pk.Class;
        E->Info.SubClass = pk.SubClass;
        E->Info.Protocol = pk.Protocol;
        E->Info.EndpointIn = pk.EpIn;
        E->Info.EndpointOut = pk.EpOut;
        E->Info.EndpointInt = pk.EpInt;
        /* The pipes as USBD gives them, each matched to the walk's endpoint. */
        for (i = 0; i < ii->NumberOfPipes && i < 3; i++) {
            PUSBD_PIPE_INFORMATION p = &ii->Pipes[i];
            if (p->EndpointAddress == pk.EpIn && p->PipeType == UsbdPipeTypeBulk) {
                E->PipeIn = p->PipeHandle;
                E->Info.MaxPacketIn = p->MaximumPacketSize;
            } else if (p->EndpointAddress == pk.EpOut && p->PipeType == UsbdPipeTypeBulk) {
                E->PipeOut = p->PipeHandle;
                E->Info.MaxPacketOut = p->MaximumPacketSize;
            } else if (p->EndpointAddress == pk.EpInt && p->PipeType == UsbdPipeTypeInterrupt) {
                E->PipeInt = p->PipeHandle;
                E->Info.MaxPacketInt = p->MaximumPacketSize;
            }
        }
        /* The speed as the walk decided it: SuperSpeed as the device
           descriptor states it (bcdUSB 3.x, bMaxPacketSize0 9), else high
           speed by its bulk size of 512. The packet sizes are the walk's,
           the size bits only; USBD's must agree with them. */
        E->Info.Speed = pk.Speed;
        if ((E->Info.MaxPacketIn & 0x7FF) != pk.MpsIn || (E->Info.MaxPacketOut & 0x7FF) != pk.MpsOut ||
            (E->Info.MaxPacketInt & 0x7FF) != pk.MpsInt)
            st = STATUS_DEVICE_CONFIGURATION_ERROR;
        E->Info.MaxPacketIn = pk.MpsIn;
        E->Info.MaxPacketOut = pk.MpsOut;
        E->Info.MaxPacketInt = pk.MpsInt;
        E->IntLen = pk.IntBytes;              /* one interval: wBytesPerInterval at SuperSpeed */
        /* 64 KiB segments and MaximumTransferSize hold whole packets at every
           speed (1,024 divides 65,536); a read is a whole number of packets. */
        if (!E->PipeIn || !E->PipeOut || !E->PipeInt || E->IntLen > INT_BUF || USBMTP_SEGMENT % pk.MpsIn)
            st = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    ExFreePool(sel);
    ExFreePool(list);
    ExFreePool(cd);
    return st;
}

static NTSTATUS ForwardWait(EXT *E, PIRP Irp)
{
    KEVENT ev;
    NTSTATUS st;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, SyncDone, &ev, TRUE, TRUE, TRUE);
    st = IoCallDriver(E->Lower, Irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = Irp->IoStatus.Status;
    }
    return st;
}

static int AllIdle(EXT *E)
{
    KIRQL irql;
    int idle;
    KeAcquireSpinLock(&E->Lock, &irql);
    idle = SlotAllIdle(&E->D) && !E->D.Tails;
    KeReleaseSpinLock(&E->Lock, irql);
    return idle;
}

/* PnP under the dispatch's admission; REMOVE gives its own up in
   ActiveDrain (*Left). The PnP manager sends one PnP IRP at a time.
   98 SE and ME deliver an unplug as IRP_MN_REMOVE_DEVICE alone; they do
   not send IRP_MN_SURPRISE_REMOVAL, which is forwarded like any other
   minor if it ever comes, and REMOVE does the whole rundown itself. */
static NTSTATUS PnpBody(PDEVICE_OBJECT Dev, EXT *E, PIRP Irp, int *Left)
{
    UCHAR mn;
    NTSTATUS st;
    SLOT_ACTIONS out;
    KIRQL irql;
    ULONG gen, opens;
    int i, del;
    mn = IoGetCurrentIrpStackLocation(Irp)->MinorFunction;
    switch (mn) {
    case IRP_MN_START_DEVICE:
        st = ForwardWait(E, Irp);
        if (NT_SUCCESS(st)) st = Start(E);
        KeAcquireSpinLock(&E->Lock, &irql);
        E->Configured = NT_SUCCESS(st);
        E->Started = E->Configured;
        E->StopQueried = 0;
        KeReleaseSpinLock(&E->Lock, irql);
        TRACE("start", st, ((ULONG)E->Info.Interface << 24) | ((ULONG)E->Info.Class << 16) |
                           ((ULONG)E->Info.MaxPacketIn));
        return Done(Irp, st, 0);
    case IRP_MN_QUERY_STOP_DEVICE:
        KeAcquireSpinLock(&E->Lock, &irql);
        i = SlotAllIdle(&E->D) && !E->D.Tails && E->D.Claim == SC_FREE;
        if (i) { E->Started = 0; E->StopQueried = 1; }
        KeReleaseSpinLock(&E->Lock, irql);
        if (!i) return Done(Irp, STATUS_DEVICE_BUSY, 0);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(E->Lower, Irp);
    case IRP_MN_CANCEL_STOP_DEVICE:
        ForwardWait(E, Irp);
        KeAcquireSpinLock(&E->Lock, &irql);
        if (E->StopQueried && E->Configured) E->Started = 1;
        E->StopQueried = 0;
        KeReleaseSpinLock(&E->Lock, irql);
        return Done(Irp, STATUS_SUCCESS, 0);
    case IRP_MN_STOP_DEVICE:
        /* Never failed; it waits here, at PASSIVE_LEVEL, until every slot is
           idle. After a successful QUERY_STOP nothing is held (it found every
           slot idle, no tail and no claim, and closed admission in that same
           hold), so the wait ends at once; a STOP without one waits for the
           driver's own cancels (5.4). */
        out.Count = 0;
        KeAcquireSpinLock(&E->Lock, &irql);
        E->Started = 0;
        E->Configured = 0;
        E->StopQueried = 0;
        for (i = 0; i < SLOT_COUNT; i++) SlotStop(&E->D, &E->Ops, i, SO_CANCELLED, &out);
        KeReleaseSpinLock(&E->Lock, irql);
        Run(E, &out);
        QuietWait(E, "stop-waiting");
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(E->Lower, Irp);
    case IRP_MN_REMOVE_DEVICE:
        TRACE("remove", E->Info.Generation, E->D.Opens);
        out.Count = 0;
        KeAcquireSpinLock(&E->Lock, &irql);
        E->Started = 0;
        E->Configured = 0;
        SlotRemove(&E->D, &E->Ops, &out);     /* release and stop everything */
        E->Owner = NULL;
        KeReleaseSpinLock(&E->Lock, irql);
        Run(E, &out);
        ActiveDrain(E);                         /* no admitted dispatch left: no power IRP after REMOVE goes down */
        *Left = 1;
        IoDeleteSymbolicLink(&E->Link);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        st = IoCallDriver(E->Lower, Irp);
        TRACE("remove-lower", st, 0);           /* the stack below returned REMOVE */
        /* The rundown: the driver's own cancels end every request; no limit. */
        {
            ULONG t0 = Ms(), said = 0;
            for (;;) {
                KeAcquireSpinLock(&E->Lock, &irql);
                i = E->D.RemoveReady;
                KeReleaseSpinLock(&E->Lock, irql);
                if (i) break;
                if (Ms() - t0 >= said + 1000) {
                    said += 1000;
                    TRACE("remove-rundown-waiting", said, E->D.Tails);
                }
                Sleep10();
            }
        }
        FreeSlots(E);                           /* every slot idle, every tail done */
        IoDetachDevice(E->Lower);
        gen = E->Info.Generation;
        KeAcquireSpinLock(&E->Lock, &irql);
        opens = E->D.Opens;                     /* read before the detach is published */
        del = SlotDetach(&E->D);                /* detached; claims the deletion if no handle is open */
        KeReleaseSpinLock(&E->Lock, irql);
        /* From here REMOVE touches the device only if it claimed the
           deletion; otherwise the last close may already be deleting it. */
        TRACE(del ? "remove-deletes" : "zombie-kept", gen, opens);
        if (del) DeleteDev(Dev);                /* the last act */
        return st;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(E->Lower, Irp);
}

static NTSTATUS DispPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    NTSTATUS st;
    EXT *E;
    int left = 0;
    if (Late(Dev, Irp, &st)) return st;
    E = (EXT *)Dev->DeviceExtension;
    st = PnpBody(Dev, E, Irp, &left);
    if (!left) ActiveDone(E);                   /* the last access */
    return st;
}

static NTSTATUS DispPower(PDEVICE_OBJECT Dev, PIRP Irp)
{
    NTSTATUS st;
    if (Late(Dev, Irp, &st)) return st;
    return Power((EXT *)Dev->DeviceExtension, Irp);                       /* it gives the admission up, or its worker does */
}

static NTSTATUS DispOther(PDEVICE_OBJECT Dev, PIRP Irp)
{
    NTSTATUS st;
    EXT *E;
    if (Late(Dev, Irp, &st)) return st;
    E = (EXT *)Dev->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    st = IoCallDriver(E->Lower, Irp);
    ActiveDone(E);
    return st;
}

static void Digits(WCHAR *At, ULONG V, int Width)
{
    int i;
    for (i = Width - 1; i >= 0; i--, V /= 10) At[i] = (WCHAR)(L'0' + V % 10);
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Drv, PDEVICE_OBJECT Pdo)
{
    static const WCHAR namePre[] = L"\\Device\\UsbMtp";
    static const WCHAR linkPre[] = L"\\DosDevices\\USBMTP";
    WCHAR nameBuf[32];
    UNICODE_STRING name;
    PDEVICE_OBJECT dev;
    EXT *E;
    NTSTATUS st;
    KIRQL irql;
    ULONG gen, n, inst;
    int i;
    KeAcquireSpinLock(&g_Global, &irql);
    for (n = 0; n < USBMTP_MAX_DEVICES && (g_NUsed & (1UL << n)); n++) ;
    if (n == USBMTP_MAX_DEVICES || g_Generation == 0xFFFFFFFFUL) {
        KeReleaseSpinLock(&g_Global, irql);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    g_NUsed |= 1UL << n;
    gen = ++g_Generation;
    inst = ++g_Instance;
    KeReleaseSpinLock(&g_Global, irql);
    /* The object's name is the generation's (a zombie keeps its own); the
       link is \\.\USBMTP<n>. */
    RtlZeroMemory(nameBuf, sizeof(nameBuf));
    for (i = 0; namePre[i]; i++) nameBuf[i] = namePre[i];
    Digits(nameBuf + i, gen, 10);
    RtlInitUnicodeString(&name, nameBuf);
    st = IoCreateDevice(Drv, sizeof(EXT), &name, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
    if (!NT_SUCCESS(st)) {
        FreeN(n);
        return st;
    }
    if (dev == g_Deleted) g_Deleted = NULL;     /* the memory of a deleted device, reused (M0) */
    E = (EXT *)dev->DeviceExtension;
    RtlZeroMemory(E, sizeof(*E));
    E->Self = dev;
    E->Pdo = Pdo;
    E->Instance = inst;
    E->N = n;
    E->Info.cbSize = sizeof(USBMTP_INFO);
    InitializeListHead(&E->PowerQ);
    E->Info.Major = USBMTP_MAJOR;
    E->Info.Minor = USBMTP_MINOR;
    E->Info.DriverVersion = DRIVER_VERSION;
    E->Info.Generation = gen;
    E->Info.N = n;
    E->BulkInMs = E->BulkOutMs = 10000;
    KeInitializeSpinLock(&E->Lock);
    SlotInit(&E->D);
    E->Ops.Ctx = E;
    E->Ops.ArmTimer = OpArm;
    E->Ops.CancelTimer = OpCancelTimer;
    E->Ops.SetCancel = OpSetCancel;
    E->Ops.IrpCancelled = OpIrpCancelled;
    E->Ops.ClearCancel = OpClearCancel;
    E->Lower = IoAttachDeviceToDeviceStack(dev, Pdo);
    if (!E->Lower) { st = STATUS_NO_SUCH_DEVICE; goto fail; }
    for (i = 0; i < SLOT_COUNT; i++) {
        XSLOT *x = &E->X[i];
        x->W.E = E;
        x->W.Slot = i;
        x->BufLen = i == SLOT_BIN || i == SLOT_BOUT ? USBMTP_SEGMENT : i == SLOT_CTL ? CTL_BUF : INT_BUF;
        KeInitializeTimer(&x->Timer);
        KeInitializeDpc(&x->Dpc, TimerDpc, &x->W);
        if (i == SLOT_WAIT) continue;           /* no lower request */
        x->Irp = IoAllocateIrp(E->Lower->StackSize, FALSE);
        x->Urb = (PURB)ExAllocatePoolWithTag(NonPagedPool, sizeof(URB), POOL_TAG);
        x->Buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, x->BufLen, POOL_TAG);
        if (!x->Irp || !x->Urb || !x->Buf) {
            st = STATUS_INSUFFICIENT_RESOURCES;
            goto fail;
        }
    }
    for (i = 0; linkPre[i]; i++) E->LinkBuf[i] = linkPre[i];
    if (n >= 10) { E->LinkBuf[i++] = L'1'; E->LinkBuf[i++] = (WCHAR)(L'0' + n - 10); }
    else E->LinkBuf[i++] = (WCHAR)(L'0' + n);
    RtlInitUnicodeString(&E->Link, E->LinkBuf);
    st = IoCreateSymbolicLink(&E->Link, &name);
    if (!NT_SUCCESS(st)) goto fail;
    dev->Flags |= DO_DIRECT_IO | DO_POWER_PAGABLE;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    TRACE("adddevice", gen, n);
    return STATUS_SUCCESS;
fail:
    /* No handle can be open yet; IoDeleteDevice is the last act (98 SE may
       call DriverUnload inside it). */
    FreeSlots(E);
    if (E->Lower) IoDetachDevice(E->Lower);
    FreeN(n);
    IoDeleteDevice(dev);
    return st;
}

/* No device object is left when DriverUnload runs (98 SE calls it inside
   the last IoDeleteDevice). A device's power worker may still be between
   its last device access and its return: wait for its count. The deleter
   is never inside that worker's call chain (it calls out only under an
   admission, and no device is deleted while one is out), so this wait
   never waits on its own caller. The late worker's completions can reach
   a deletion (a zombie's last close), so its count is waited for only
   from another thread: on its own thread this is inside one of its
   completions, and it is past its last IRP's IoCompleteRequest only once
   this returns. */
static VOID Unload(PDRIVER_OBJECT Drv)
{
    (void)Drv;
    while (*(volatile LONG *)&g_Running) Sleep10();
    while (*(volatile LONG *)&g_LateRunning && g_LateThread != KeGetCurrentThread()) Sleep10();
    TRACE("unload", g_Foreign, g_Dropped);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING Path)
{
    int i;
    (void)Path;
    KeInitializeSpinLock(&g_Global);
    InitializeListHead(&g_LateQ);
#ifdef USBMTP_TRACE
    KeInitializeSpinLock(&g_TraceLock);
#endif
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) Drv->MajorFunction[i] = DispOther;
    Drv->MajorFunction[IRP_MJ_CREATE] = DispCreate;
    Drv->MajorFunction[IRP_MJ_CLEANUP] = DispCleanupClose;
    Drv->MajorFunction[IRP_MJ_CLOSE] = DispCleanupClose;
    Drv->MajorFunction[IRP_MJ_READ] = DispReadWrite;
    Drv->MajorFunction[IRP_MJ_WRITE] = DispReadWrite;
    Drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispIoctl;
    Drv->MajorFunction[IRP_MJ_PNP] = DispPnp;
    Drv->MajorFunction[IRP_MJ_POWER] = DispPower;
    Drv->DriverExtension->AddDevice = AddDevice;
    Drv->DriverUnload = Unload;
    TRACE("driverentry", DRIVER_VERSION, QUIET_QUERY_MS);   /* 0 names the standby test build */
    return STATUS_SUCCESS;
}
