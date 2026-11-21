/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mtpprb.c - MTPPRB.SYS, the M0 probe driver of docs/design/mtp.md (5.6).
 * Not shipped; bound by the test INF MTPPRB.INF to a still image interface.
 *
 * It is built on ../sys/slot.c, the driver's accepted slot contract, so it
 * also exercises that contract on the real stacks. It reads, on 98 SE's and
 * ME's NTKERN under each USB stack:
 * - whether IoCancelIrp, or the abort of the pipe, from a second handle
 *   ends a read blocked on the first, and when the transfer retires;
 * - the same for the interrupt pipe, a class control request and a pipe
 *   reset (each cancellable from the second handle);
 * - what status and byte count Win32 sees for each NTSTATUS;
 * - the order of cancel, IRP_MJ_CLEANUP and IRP_MJ_CLOSE with calls
 *   outstanding, and what IRPs (cleanup and close included) still arrive
 *   for handles left open after IoDeleteDevice.
 * Every event is one line "prb <ms> <event> <values>" on QEMU's debug
 * console (port E9h), written under a lock so lines never interleave. The
 * times come from the tick count (10 to 55 ms steps) and every line is
 * written synchronously to the port, so latencies are coarse and include
 * the logging's own cost.
 *
 * Slots: SLOT_BIN (ReadFile, bulk IN), SLOT_BOUT (the interrupt-IN read
 * IOCTL_PRB_INT: the probe puts it on a slot that holds a caller IRP),
 * SLOT_CTL (Get Device Status, RESET_PIPE). If a lower request never comes
 * back at removal, the probe logs it and does not delete the device (a
 * deliberate leak), so no late callback can touch freed memory.
 *
 * The rules here are the pattern for USBMTP.SYS:
 * - A request's result travels with the request: everything a completion
 *   tail needs is in its SA_COMPLETE action or in the caller's IRP that the
 *   tail owns (the lower NTSTATUS is kept in that IRP's IoStatus.Status
 *   under the lock, before the slot can be freed); a tail reads no slot.
 * - Admission is gated by the device's started state, under the same hold
 *   of the lock as SlotAdmit: no transfer before a successful START, after
 *   a failed one, or once QUERY_STOP, STOP or REMOVE has begun. QUERY_STOP
 *   is refused while a request is out; STOP stops every slot and stays
 *   pending until every slot is idle (no limit) before it goes down, so a
 *   later START never changes pipes under a transfer or abort.
 * - Power: DO_POWER_PAGABLE is set before the device is exposed, and power
 *   IRPs are forwarded (or, after IoDeleteDevice, completed) at
 *   PASSIVE_LEVEL, PoStartNextPowerIrp first; one seen above PASSIVE_LEVEL
 *   goes to a work item. Forwarding is admitted under the lock and counted;
 *   REMOVE closes that gate and waits for the count to reach 0 before it
 *   forwards REMOVE, so no power IRP reaches the lower stack after it (one
 *   arriving later is completed STATUS_DELETE_PENDING). Known limit: a
 *   lower driver that waited for REMOVE inside its power dispatch would
 *   deadlock that drain; a hang at removal after "power-drain-waiting-ms"
 *   is that case, and a reading.
 * - Every work item is counted until its routine's last act; the deleter
 *   (below) and DriverUnload wait for none to be left.
 * - The device object is not deleted while a handle is open: at REMOVE the
 *   device runs down and detaches, then stays as a zombie that refuses
 *   everything but cleanup, close (and here the counters) while handles
 *   are open. The open count is slot.c's (SlotOpen, SlotClose). One party
 *   deletes: slot.c's SlotDetach (REMOVE, after its detach) or the
 *   SlotClose of any close claims it, under the lock, when the device is
 *   detached and FreeReady set (the rundown done, no handle open); every
 *   close acts on SlotClose's answer, and after its hold of the lock a
 *   party that did not claim touches nothing of the device. REMOVE never
 *   deletes on its own count; SA_FREE_READY itself is
 *   only noted (it may run at any IRQL). The deleter drains the work
 *   items first; IoDeleteDevice is its last act (98 SE calls DriverUnload
 *   inside it). 98 SE gave a deleted object's address to the next device
 *   (read on 2026-11-16). Each handle carries its device's number in
 *   FsContext, so an IRP that comes on another device's handle is caught
 *   and counted (g_Foreign).
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include <wdm.h>
#include <usbdi.h>
#include <usbdlib.h>
#include "slotm0.h"
#include "mtpprb.h"

int __cdecl _outp(unsigned short port, int value);
#pragma intrinsic(_outp)

#define MAXBUF 65536
#define CTL_DEVSTATUS 1
#define CTL_RESETPIPE 2
#define KIND_BULKOUT 3     /* SLOT_BOUT: a bulk OUT write (else the interrupt read) */

typedef struct XSLOT {
    PIRP Irp;               /* lower IRP: IoAllocateIrp(n, FALSE), rebuilt per submission */
    PURB Urb;
    PUCHAR Buf;
    ULONG Len;              /* the request's length */
    ULONG Got;              /* bytes copied to the caller so far */
    ULONG StopAt;           /* ms at the request's first effective stop, 0 if none */
    ULONG StopReq;          /* the request that stop was for */
    PIRP AbortIrp;
    PURB AbortUrb;
    KEVENT AbortEvent;
    WORK_QUEUE_ITEM Work;
    int Ctl;                /* SLOT_CTL: CTL_DEVSTATUS or CTL_RESETPIPE; SLOT_BOUT: KIND_BULKOUT or 0; set at admission */
    int ResetWhich;         /* RESET_PIPE's target pipe: PRB_BIN or PRB_INT, set at admission */
} XSLOT;

typedef struct EXT {
    PDEVICE_OBJECT Self, Lower;
    KSPIN_LOCK Lock;
    SLOT_DEVICE D;
    SLOT_OPS Ops;
    XSLOT X[SLOT_COUNT];
    USBD_PIPE_HANDLE Pipe[2];       /* PRB_BIN, PRB_INT */
    USBD_PIPE_HANDLE PipeOut;       /* bulk OUT */
    UCHAR Interface;
    int Started;                    /* the admission gate, under Lock */
    int Configured;                 /* START succeeded and no STOP or REMOVE since */
    int StopQueried;                /* QUERY_STOP closed the gate; CANCEL_STOP reopens it */
    LONG PowerOut;                  /* power IRPs being forwarded (inline or by a work item), under Lock */
    int PowerClosed;                /* REMOVE has begun: no power IRP goes down any more, under Lock */
    ULONG Instance;                 /* this device's number (1, 2, ...): its handles' FsContext */
    UNICODE_STRING Link;
} EXT;

typedef struct WCTX { EXT *E; int Slot; } WCTX;

static PDEVICE_OBJECT g_Deleted;
static LONG g_Creates, g_Cleanups, g_Closes, g_Late, g_Dropped;
static LONG g_Removes, g_Deletes, g_Leaks;  /* REMOVE received; IoDeleteDevice done; removal leaked */
static LONG g_Zombies, g_Foreign;           /* removed devices kept for open handles; IRPs on another device's handle */
static LONG g_Instance;
static KSPIN_LOCK g_LogLock;
static WCTX g_Wctx[SLOT_COUNT];     /* one probe device at a time */

/* ------------------------------------------------------------------ log */

static ULONG Ms(void)
{
    LARGE_INTEGER t;
    KeQueryTickCount(&t);
    return (ULONG)((t.QuadPart * KeQueryTimeIncrement()) / 10000);
}

static void Log(const char *What, ULONG A, ULONG B, ULONG C)
{
    static const char hex[] = "0123456789ABCDEF";
    ULONG v[4];
    int i, k;
    KIRQL irql;
    v[0] = Ms(); v[1] = A; v[2] = B; v[3] = C;
    KeAcquireSpinLock(&g_LogLock, &irql);
    for (i = 0; "prb "[i]; i++) _outp(0xE9, "prb "[i]);
    for (k = 0; k < 4; k++) {
        if (k == 1) for (i = 0; What[i]; i++) _outp(0xE9, What[i]);
        _outp(0xE9, ' ');
        for (i = 28; i >= 0; i -= 4) _outp(0xE9, hex[(v[k] >> i) & 15]);
    }
    _outp(0xE9, '\n');
    KeReleaseSpinLock(&g_LogLock, irql);
}

/* Every work item this driver queues is counted until its routine's last
   act, and REMOVE's tail and DriverUnload wait for none to be left, so no
   routine of this image runs after the image can unload. */
static LONG g_WorkOut;

static void WorkQueue(PWORK_QUEUE_ITEM Item, PWORKER_THREAD_ROUTINE Routine, PVOID Ctx)
{
    InterlockedIncrement(&g_WorkOut);
    ExInitializeWorkItem(Item, Routine, Ctx);
    ExQueueWorkItem(Item, DelayedWorkQueue);
}

/* At PASSIVE_LEVEL: wait (no limit, logged every 5 s of elapsed time) until
   no work item of this driver is outstanding. */
static void WorkDrain(const char *What)
{
    LARGE_INTEGER step;
    ULONG t0 = Ms(), last = t0, now;
    step.QuadPart = -100000;        /* 10 ms */
    for (;;) {
        now = Ms();
        if (!*(volatile LONG *)&g_WorkOut) break;
        if (now - last >= 5000) {
            last = now;
            Log("work-drain-waiting-ms", now - t0, (ULONG)g_WorkOut, 0);
        }
        KeDelayExecutionThread(KernelMode, FALSE, &step);
    }
    Log(What, now - t0, 0, 0);
}

/* ------------------------------------------------------------------ ops for slot.c */

static VOID CallerCancel(PDEVICE_OBJECT Dev, PIRP Irp);
static void OpArm(void *C, int Slot) { (void)C; (void)Slot; }          /* the probe arms no timer */
static int OpCancelTimer(void *C, int Slot) { (void)C; (void)Slot; return 0; }
static void OpSetCancel(void *C, int Slot) { IoSetCancelRoutine((PIRP)((EXT *)C)->D.S[Slot].Irp, CallerCancel); }
static int OpIrpCancelled(void *C, int Slot) { return ((PIRP)((EXT *)C)->D.S[Slot].Irp)->Cancel; }
static int OpClearCancel(void *C, int Slot) { return IoSetCancelRoutine((PIRP)((EXT *)C)->D.S[Slot].Irp, NULL) != NULL; }

/* The caller's NTSTATUS: from the outcome and, for SO_ERROR, the lower
   status kept in the caller's own IRP (Lower). */
static NTSTATUS Outcome(int O, NTSTATUS Lower)
{
    switch (O) {
    case SO_SUCCESS: return STATUS_SUCCESS;
    case SO_ERROR: return NT_SUCCESS(Lower) ? STATUS_UNSUCCESSFUL : Lower;
    case SO_CANCELLED: return STATUS_CANCELLED;
    case SO_TIMEOUT: return STATUS_IO_TIMEOUT;
    case SO_REMOVED: return STATUS_DELETE_PENDING;
    case SO_BUSY: return STATUS_DEVICE_BUSY;
    case SO_DENIED: return STATUS_DELETE_PENDING;
    }
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS Done(PIRP Irp, NTSTATUS St, ULONG_PTR Info)
{
    Irp->IoStatus.Status = St;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return St;
}

/* Under the lock: the first effective stop of slot s's request (the slot
   is active: SlotStop, SlotCancelBegin or SlotRemove will change it), timed
   under the lock, so it precedes the retirement LowerDone times under it.
   Returns the request, or 0 if the stop does nothing. */
static ULONG MarkStop(EXT *E, int s)
{
    SLOT *S = &E->D.S[s];
    XSLOT *x = &E->X[s];
    ULONG now;
    if (S->State != SS_ACTIVE) return 0;
    if (!x->StopAt || x->StopReq != S->Request) {
        now = Ms();
        x->StopAt = now ? now : 1;
        x->StopReq = S->Request;
    }
    return S->Request;
}

/* ------------------------------------------------------------------ actions, with the lock released */

static NTSTATUS LowerDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx);
static NTSTATUS AbortDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx);
static VOID AbortWorker(PVOID Ctx);

/* Rebuild the slot's lower IRP: only for SA_SUBMIT, when slot.c has it back.
   Ctl and ResetWhich were set by the admission of the request it is for. */
static void Build(EXT *E, int s)
{
    XSLOT *x = &E->X[s];
    PIO_STACK_LOCATION st;
    CCHAR n = E->Lower->StackSize;
    IoInitializeIrp(x->Irp, IoSizeOfIrp(n), n);     /* 98/ME reuse (Microsoft, "Reusing IRPs") */
    if (s == SLOT_CTL && x->Ctl == CTL_RESETPIPE) {
        RtlZeroMemory(x->Urb, sizeof(struct _URB_PIPE_REQUEST));
        x->Urb->UrbPipeRequest.Hdr.Length = sizeof(struct _URB_PIPE_REQUEST);
        x->Urb->UrbPipeRequest.Hdr.Function = URB_FUNCTION_RESET_PIPE;
        x->Urb->UrbPipeRequest.PipeHandle = E->Pipe[x->ResetWhich];
    } else if (s == SLOT_CTL) {
        UsbBuildVendorRequest(x->Urb, URB_FUNCTION_CLASS_INTERFACE, sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST),
                              USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, 0, 0x67, 0, E->Interface,
                              x->Buf, NULL, x->Len, NULL);
    } else if (s == SLOT_BOUT && x->Ctl == KIND_BULKOUT) {
        UsbBuildInterruptOrBulkTransferRequest(x->Urb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER),
                                               E->PipeOut, x->Buf, NULL, x->Len, USBD_TRANSFER_DIRECTION_OUT, NULL);
    } else {
        UsbBuildInterruptOrBulkTransferRequest(x->Urb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER),
                                               E->Pipe[s == SLOT_BIN ? PRB_BIN : PRB_INT], x->Buf, NULL, x->Len,
                                               USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, NULL);
    }
    st = IoGetNextIrpStackLocation(x->Irp);
    st->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    st->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    st->Parameters.Others.Argument1 = x->Urb;
    IoSetCompletionRoutine(x->Irp, LowerDone, &g_Wctx[s], TRUE, TRUE, TRUE);
}

/* Perform slot.c's actions; the actions they lead to are performed by the
   same loop (no recursion). */
static void Run(EXT *E, SLOT_ACTIONS *First)
{
    SLOT_ACTIONS q, more;
    int i;
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
            Build(E, a.Slot);
            Log("submit", a.Slot, E->X[a.Slot].Len, a.Request);
            IoCallDriver(E->Lower, E->X[a.Slot].Irp);
            KeAcquireSpinLock(&E->Lock, &irql);
            SlotSubmitReturned(&E->D, &E->Ops, a.Slot, &more);
            KeReleaseSpinLock(&E->Lock, irql);
            break;
        case SA_CANCEL_LOWER:
            Log("iocancelirp", a.Slot, a.Request, 0);
            IoCancelIrp(E->X[a.Slot].Irp);
            Log("iocancelirp-returned", a.Slot, a.Request, 0);
            KeAcquireSpinLock(&E->Lock, &irql);
            SlotPinReturned(&E->D, &E->Ops, a.Slot, &more);
            KeReleaseSpinLock(&E->Lock, irql);
            break;
        case SA_QUEUE_ABORT:
            Log("abort-queued", a.Slot, a.Request, 0);
            WorkQueue(&E->X[a.Slot].Work, AbortWorker, &g_Wctx[a.Slot]);
            break;
        case SA_COMPLETE:
            /* The tail: everything from the action and the IRP it owns. */
            irp = (PIRP)a.Irp;
            Log("complete", a.Request, (ULONG)a.Outcome, a.Bytes);
            Done(irp, Outcome(a.Outcome, irp->IoStatus.Status), a.Bytes);
            KeAcquireSpinLock(&E->Lock, &irql);
            SlotTailDone(&E->D, &more);
            KeReleaseSpinLock(&E->Lock, irql);
            break;
        case SA_REMOVE_READY:
            Log("remove-ready", 0, 0, 0);   /* removal's wait reads D.RemoveReady */
            break;
        case SA_FREE_READY:
            /* Only noted: the deletion is claimed under the lock by REMOVE
               after its detach, or by the close that ended the count
               (DeleteDev); this action may run at any IRQL. */
            Log("free-ready", 0, 0, 0);
            break;
        }
        for (i = 0; i < more.Count; i++) {
            if (q.Count < SLOT_MAX_ACTIONS) {
                q.A[q.Count++] = more.A[i];
            } else {
                /* Cannot happen (at most three actions per slot per call);
                   counted and logged so a reading would show it. */
                InterlockedIncrement(&g_Dropped);
                Log("action-dropped", more.A[i].Kind, more.A[i].Slot, more.A[i].Request);
            }
        }
    }
}

static NTSTATUS LowerDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx)
{
    WCTX *w = (WCTX *)Ctx;
    EXT *E = w->E;
    int s = w->Slot;
    XSLOT *x = &E->X[s];
    SLOT_ACTIONS out;
    KIRQL irql;
    NTSTATUS st = Irp->IoStatus.Status;
    USBD_STATUS us = x->Urb->UrbHeader.Status;
    ULONG raw = 0, copied = 0, req, stopAt, now;
    int ok, valid = 1;
    PIRP caller;
    (void)Dev;
    /* What the URB says moved (the URB is the slot's until SlotLowerDone). */
    if (x->Urb->UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER)
        raw = x->Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    else if (x->Urb->UrbHeader.Function == URB_FUNCTION_CLASS_INTERFACE)
        raw = x->Urb->UrbControlVendorClassRequest.TransferBufferLength;
    ok = NT_SUCCESS(st) && USBD_SUCCESS(us);
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    now = Ms();             /* the retirement, timed under the lock (MarkStop) */
    req = E->D.S[s].Request;
    caller = (PIRP)E->D.S[s].Irp;
    stopAt = (x->StopAt && x->StopReq == req) ? x->StopAt : 0;
    if (ok && raw > x->Len - x->Got) {
        valid = 0;          /* more than was asked for: not data */
        ok = 0;
    }
    /* Only a successful URB's count is data. It reaches the caller while
       the slot still owns the caller's IRP. */
    if (ok && caller && raw && s == SLOT_BOUT && x->Ctl == KIND_BULKOUT) {
        x->Got += raw;      /* a write: the bytes the URB says went out */
        copied = raw;
    } else if (ok && caller && raw) {
        RtlCopyMemory((PUCHAR)(caller->MdlAddress ? MmGetSystemAddressForMdl(caller->MdlAddress)
                                                  : caller->AssociatedIrp.SystemBuffer) + x->Got, x->Buf, raw);
        x->Got += raw;
        copied = raw;
    }
    /* The lower status travels with the caller's IRP to its tail. */
    if (!ok && caller) caller->IoStatus.Status = NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
    SlotLowerDone(&E->D, &E->Ops, s, ok, 1, copied, &out);
    KeReleaseSpinLock(&E->Lock, irql);
    Log(s == SLOT_BIN ? "bin-done" : s == SLOT_BOUT ? "int-done" : "ctl-done", req, (ULONG)st, (ULONG)us);
    Log(valid ? "bytes-raw-copied" : "bytes-raw-invalid", req, raw, copied);
    if (stopAt) Log("stop-to-retire-ms", req, now - stopAt, s);
    Run(E, &out);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS AbortDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx)
{
    WCTX *w = (WCTX *)Ctx;
    EXT *E = w->E;
    SLOT_ACTIONS out;
    KIRQL irql;
    (void)Dev;
    Log("abort-irp-done", w->Slot, (ULONG)Irp->IoStatus.Status, 0);
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    SlotAbortIrpDone(&E->D, &E->Ops, w->Slot, &out);
    KeReleaseSpinLock(&E->Lock, irql);
    KeSetEvent(&E->X[w->Slot].AbortEvent, IO_NO_INCREMENT, FALSE);     /* the last access */
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static VOID AbortWorker(PVOID Ctx)
{
    WCTX *w = (WCTX *)Ctx;
    EXT *E = w->E;
    XSLOT *x = &E->X[w->Slot];
    PIO_STACK_LOCATION st;
    SLOT_ACTIONS out;
    KIRQL irql;
    CCHAR n = E->Lower->StackSize;
    IoInitializeIrp(x->AbortIrp, IoSizeOfIrp(n), n);
    RtlZeroMemory(x->AbortUrb, sizeof(struct _URB_PIPE_REQUEST));
    x->AbortUrb->UrbPipeRequest.Hdr.Length = sizeof(struct _URB_PIPE_REQUEST);
    x->AbortUrb->UrbPipeRequest.Hdr.Function = URB_FUNCTION_ABORT_PIPE;
    x->AbortUrb->UrbPipeRequest.PipeHandle = w->Slot == SLOT_BIN ? E->Pipe[PRB_BIN] :
                                             x->Ctl == KIND_BULKOUT ? E->PipeOut : E->Pipe[PRB_INT];
    st = IoGetNextIrpStackLocation(x->AbortIrp);
    st->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    st->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    st->Parameters.Others.Argument1 = x->AbortUrb;
    IoSetCompletionRoutine(x->AbortIrp, AbortDone, w, TRUE, TRUE, TRUE);
    KeClearEvent(&x->AbortEvent);
    Log("abort-pipe", w->Slot, 0, 0);
    IoCallDriver(E->Lower, x->AbortIrp);
    KeWaitForSingleObject(&x->AbortEvent, Executive, KernelMode, FALSE, NULL);
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    SlotAbortWorkerDone(&E->D, &E->Ops, w->Slot, &out);
    KeReleaseSpinLock(&E->Lock, irql);
    Run(E, &out);
    InterlockedDecrement(&g_WorkOut);           /* the last act */
}

static VOID CallerCancel(PDEVICE_OBJECT Dev, PIRP Irp)
{
    EXT *E = (EXT *)Dev->DeviceExtension;
    SLOT_ACTIONS out;
    KIRQL irql;
    int s, found = -1;
    ULONG req = 0;
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    for (s = 0; s < SLOT_COUNT; s++) if (E->D.S[s].Irp == (void *)Irp) found = s;
    if (found >= 0) {
        req = MarkStop(E, found);
        SlotCancelBegin(&E->D, &E->Ops, found, &out);
    }
    KeReleaseSpinLock(&E->Lock, irql);
    Log("caller-cancel-routine", (ULONG)found, req, 0);
    Run(E, &out);
    if (found >= 0) {
        /* The slot completes the IRP once this routine has cleared its
           flag (the handshake of slot.h). */
        out.Count = 0;
        KeAcquireSpinLock(&E->Lock, &irql);
        SlotCancelEnd(&E->D, &E->Ops, found, &out);
        KeReleaseSpinLock(&E->Lock, irql);
        Run(E, &out);
    }
}

/* Admission of a caller IRP on slot s: the started gate, SlotAdmit, the
   request's own fields and SlotAdmitCheck, all under one hold of the lock,
   so a refused request changes nothing of an admitted one. */
static NTSTATUS Admit(EXT *E, PIRP Irp, int s, ULONG Len, int Reserve, int Ctl, int ResetWhich, PVOID Src)
{
    SLOT_ACTIONS out;
    KIRQL irql;
    unsigned long r = 0;
    int started;
    out.Count = 0;
    IoMarkIrpPending(Irp);
    KeAcquireSpinLock(&E->Lock, &irql);
    started = E->Started;
    if (started) r = SlotAdmit(&E->D, &E->Ops, s, Irp, 1, 0, Reserve, 0, 0, &out);
    if (r) {
        XSLOT *x = &E->X[s];
        x->Len = Len;
        x->Got = 0;
        x->StopAt = 0;
        x->StopReq = 0;
        x->Ctl = Ctl;
        x->ResetWhich = ResetWhich;
        if (Src) RtlCopyMemory(x->Buf, Src, Len);   /* a write's data, only once it is admitted */
        E->D.S[s].Fallback = 0;             /* caller cancels and removal: IoCancelIrp */
        Irp->IoStatus.Status = STATUS_SUCCESS;  /* the lower status lands here on failure */
        SlotAdmitCheck(&E->D, &E->Ops, s, &out);
    }
    KeReleaseSpinLock(&E->Lock, irql);
    if (!started) {
        Log("refused-not-started", s, 0, 0);
        Done(Irp, STATUS_DEVICE_NOT_READY, 0);
        return STATUS_PENDING;
    }
    if (!r) {
        Log("refused", s, (ULONG)out.A[0].Outcome, 0);
        Done(Irp, Outcome(out.A[0].Outcome, STATUS_SUCCESS), 0);
        return STATUS_PENDING;
    }
    Log("admitted", s, Len, r);
    Run(E, &out);
    return STATUS_PENDING;
}

/* A stop from a second handle: IoCancelIrp, or the abort (the fallback).
   Returns the request it stopped, or 0 if there was none to stop. */
static ULONG Stop(EXT *E, int s, int ByAbort)
{
    SLOT_ACTIONS out;
    KIRQL irql;
    ULONG req;
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    req = MarkStop(E, s);
    if (req) {
        E->D.S[s].Fallback = (unsigned char)(ByAbort ? 1 : 0);
        SlotStop(&E->D, &E->Ops, s, SO_CANCELLED, &out);
    }
    KeReleaseSpinLock(&E->Lock, irql);
    Log(ByAbort ? "stop-by-abort" : "stop-by-iocancelirp", s, req, (ULONG)out.Count);
    Run(E, &out);
    return req;
}

/* Under the lock: close the gate and stop every slot (STOP and REMOVE). */
static void Gate(EXT *E, SLOT_ACTIONS *Out)
{
    int s;
    E->Started = 0;
    for (s = 0; s < SLOT_COUNT; s++) {
        MarkStop(E, s);
        SlotStop(&E->D, &E->Ops, s, SO_CANCELLED, Out);
    }
}

/* ------------------------------------------------------------------ dispatch */

/* The counters (IOCTL_PRB_COUNTERS), from globals only, so a handle kept
   open after IoDeleteDevice can still read the removal milestones. */
static ULONG Counters(PVOID Buf)
{
    ULONG *v = (ULONG *)Buf;
    v[0] = (ULONG)g_Creates; v[1] = (ULONG)g_Cleanups; v[2] = (ULONG)g_Closes; v[3] = (ULONG)g_Late;
    v[4] = (ULONG)g_Dropped + SlotDropped; v[5] = (ULONG)g_Removes; v[6] = (ULONG)g_Deletes;
    v[7] = (ULONG)g_Leaks; v[8] = (ULONG)g_Zombies; v[9] = (ULONG)g_Foreign;
    return PRB_COUNTERS_BYTES;
}

static VOID PowerWorker(PVOID Ctx);

/* A power IRP, for device E (NULL after IoDeleteDevice). DO_POWER_PAGABLE
   is set, so the power manager sends it at PASSIVE_LEVEL; one seen above it
   is handed to a work item rather than forwarded or completed there.
   Forwarding (PoStartNextPowerIrp, then PoCallDriver to E->Lower) is
   admitted under E->Lock and counted in PowerOut until PoCallDriver has
   returned; once REMOVE has begun (PowerClosed) nothing is forwarded any
   more and the IRP is completed STATUS_DELETE_PENDING instead. REMOVE
   closes that gate and waits for PowerOut to reach 0 before it forwards
   REMOVE, so no power IRP reaches the lower stack after its REMOVE. */
typedef struct PWORK { WORK_QUEUE_ITEM Item; EXT *E; PIRP Irp; } PWORK;

static void PowerDone(EXT *E)
{
    KIRQL lk;
    KeAcquireSpinLock(&E->Lock, &lk);
    E->PowerOut--;
    KeReleaseSpinLock(&E->Lock, lk);
}

static NTSTATUS Power(EXT *E, PIRP Irp)
{
    KIRQL irql = KeGetCurrentIrql(), lk;
    PWORK *w = NULL;
    NTSTATUS st;
    int fwd = 0;
    if (irql != PASSIVE_LEVEL) w = (PWORK *)ExAllocatePool(NonPagedPool, sizeof(PWORK));
    if (E) {
        KeAcquireSpinLock(&E->Lock, &lk);
        fwd = !E->PowerClosed;
        if (fwd && (irql == PASSIVE_LEVEL || w)) E->PowerOut++;
        KeReleaseSpinLock(&E->Lock, lk);
    }
    Log("power", IoGetCurrentIrpStackLocation(Irp)->MinorFunction, irql, (ULONG)fwd);
    if (irql == PASSIVE_LEVEL) {
        PoStartNextPowerIrp(Irp);
        if (!fwd) return Done(Irp, STATUS_DELETE_PENDING, 0);
        IoSkipCurrentIrpStackLocation(Irp);
        st = PoCallDriver(E->Lower, Irp);
        PowerDone(E);
        return st;
    }
    if (!w) {
        /* No memory for the deferral: fail it here rather than forward it
           above PASSIVE_LEVEL (logged; never seen is the expectation). */
        Log("power-above-passive-failed", irql, 0, 0);
        PoStartNextPowerIrp(Irp);
        return Done(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    Log("power-above-passive-deferred", irql, (ULONG)fwd, 0);
    w->E = fwd ? E : NULL;
    w->Irp = Irp;
    IoMarkIrpPending(Irp);
    WorkQueue(&w->Item, PowerWorker, w);
    return STATUS_PENDING;
}

static VOID PowerWorker(PVOID Ctx)
{
    PWORK *w = (PWORK *)Ctx;
    EXT *E = w->E;
    PIRP irp = w->Irp;
    ExFreePool(w);
    PoStartNextPowerIrp(irp);
    if (!E) {
        Done(irp, STATUS_DELETE_PENDING, 0);
    } else {
        IoCopyCurrentIrpStackLocationToNext(irp);   /* marked pending: no skip */
        PoCallDriver(E->Lower, irp);
        PowerDone(E);                               /* REMOVE waits for 0 */
    }
    InterlockedDecrement(&g_WorkOut);               /* the last act */
}

/* REMOVE: close the power gate and wait (PASSIVE_LEVEL, no limit, logged)
   for every admitted power IRP to have been passed down. */
static void PowerDrain(EXT *E)
{
    LARGE_INTEGER step;
    KIRQL lk;
    ULONG t0 = Ms(), last = t0, now;
    LONG out;
    step.QuadPart = -100000;        /* 10 ms */
    KeAcquireSpinLock(&E->Lock, &lk);
    E->PowerClosed = 1;
    KeReleaseSpinLock(&E->Lock, lk);
    for (;;) {
        KeAcquireSpinLock(&E->Lock, &lk);
        out = E->PowerOut;
        KeReleaseSpinLock(&E->Lock, lk);
        now = Ms();
        if (!out) break;
        if (now - last >= 5000) {
            last = now;
            Log("power-drain-waiting-ms", now - t0, (ULONG)out, 0);
        }
        KeDelayExecutionThread(KernelMode, FALSE, &step);
    }
    Log("power-drained-ms", now - t0, 0, 0);
}

/* The one deleter: called by the party whose SlotDetach (REMOVE) or
   SlotClose (a close) claimed the deletion under the lock (mtp.md 5.4). The drains come first; IoDeleteDevice is the
   last act (98 SE calls DriverUnload inside it when it is the last device
   object), and nothing of E or of the driver runs after it but the return. */
static void DeleteDev(PDEVICE_OBJECT Dev, int WasZombie)
{
    ULONG inst = ((EXT *)Dev->DeviceExtension)->Instance;
    WorkDrain("delete-work-drained-ms");
    if (WasZombie) InterlockedDecrement(&g_Zombies);
    InterlockedIncrement(&g_Deletes);
    Log("deleting", inst, (ULONG)g_Deletes, (ULONG)WasZombie);
    g_Deleted = Dev;
    IoDeleteDevice(Dev);
}

/* Every dispatch routine's first step. Returns 1, with the IRP completed
   and its status in *St, for:
   - an IRP for a deleted device (g_Deleted): logged and counted; cleanup,
     close and the counters succeed, power is completed at PASSIVE_LEVEL,
     the rest fail; touches no device extension;
   - an IRP on a handle opened on another device (its FsContext is not
     this device's number): logged and counted in g_Foreign; cleanup and
     close succeed, the rest fail STATUS_INVALID_HANDLE;
   - an IRP for a zombie (removed, kept for its open handles): logged and
     counted; cleanup, close and the counters succeed, power is completed
     STATUS_DELETE_PENDING, the rest fail STATUS_DELETE_PENDING; the last
     close deletes the device.
   Returns 0 otherwise. */
static void NoteReady(const SLOT_ACTIONS *Out);

static int Late(PDEVICE_OBJECT Dev, PIRP Irp, NTSTATUS *St)
{
    SLOT_ACTIONS out;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    UCHAR mj = s->MajorFunction;
    PFILE_OBJECT fo = s->FileObject;
    EXT *E;
    KIRQL irql;
    ULONG inst;
    int zombie, del = 0;
    if (Dev == g_Deleted) {
        InterlockedIncrement(&g_Late);
        if (mj == IRP_MJ_CLEANUP) InterlockedIncrement(&g_Cleanups);
        if (mj == IRP_MJ_CLOSE) InterlockedIncrement(&g_Closes);
        Log(mj == IRP_MJ_CLEANUP ? "cleanup-after-delete" : mj == IRP_MJ_CLOSE ? "close-after-delete" : "irp-after-delete",
            mj, (ULONG)(ULONG_PTR)fo, (ULONG)g_Late);
        if (mj == IRP_MJ_POWER) {
            *St = Power(NULL, Irp);
            return 1;
        }
        if (mj == IRP_MJ_DEVICE_CONTROL && s->Parameters.DeviceIoControl.IoControlCode == IOCTL_PRB_COUNTERS &&
            s->Parameters.DeviceIoControl.OutputBufferLength >= PRB_COUNTERS_BYTES) {
            *St = STATUS_SUCCESS;
            Done(Irp, *St, Counters(Irp->AssociatedIrp.SystemBuffer));
            return 1;
        }
        *St = (mj == IRP_MJ_CLEANUP || mj == IRP_MJ_CLOSE) ? STATUS_SUCCESS : STATUS_DELETE_PENDING;
        Done(Irp, *St, 0);
        return 1;
    }
    E = (EXT *)Dev->DeviceExtension;
    if (fo && mj != IRP_MJ_CREATE && fo->FsContext != (PVOID)(ULONG_PTR)E->Instance) {
        InterlockedIncrement(&g_Foreign);
        Log("foreign-handle", mj, (ULONG)(ULONG_PTR)fo->FsContext, E->Instance);
        *St = (mj == IRP_MJ_CLEANUP || mj == IRP_MJ_CLOSE) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
        Done(Irp, *St, 0);
        return 1;
    }
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    zombie = E->D.Detached;
    inst = E->Instance;
    if (zombie && mj == IRP_MJ_CLOSE) del = SlotClose(&E->D, &out);   /* claims the deletion, or not */
    KeReleaseSpinLock(&E->Lock, irql);
    if (!zombie) return 0;
    /* SlotClose's actions are only the two ready notes, which need no
       performing; a close that did not claim the deletion touches nothing
       of the device from here (another close may be deleting it). */
    NoteReady(&out);
    InterlockedIncrement(&g_Late);
    if (mj == IRP_MJ_CLEANUP) InterlockedIncrement(&g_Cleanups);
    if (mj == IRP_MJ_CLOSE) InterlockedIncrement(&g_Closes);
    Log(mj == IRP_MJ_CLEANUP ? "zombie-cleanup" : mj == IRP_MJ_CLOSE ? "zombie-close" : "zombie-irp",
        mj, (ULONG)(ULONG_PTR)fo, inst);
    if (mj == IRP_MJ_CLOSE) {
        *St = STATUS_SUCCESS;
        Done(Irp, STATUS_SUCCESS, 0);
        if (del) DeleteDev(Dev, 1);         /* the zombie's last close */
        return 1;
    }
    if (mj == IRP_MJ_POWER) {
        *St = Power(E, Irp);                /* PowerClosed: completed, not forwarded */
        return 1;
    }
    if (mj == IRP_MJ_DEVICE_CONTROL && s->Parameters.DeviceIoControl.IoControlCode == IOCTL_PRB_COUNTERS &&
        s->Parameters.DeviceIoControl.OutputBufferLength >= PRB_COUNTERS_BYTES) {
        *St = STATUS_SUCCESS;
        Done(Irp, *St, Counters(Irp->AssociatedIrp.SystemBuffer));
        return 1;
    }
    *St = mj == IRP_MJ_CLEANUP ? STATUS_SUCCESS : STATUS_DELETE_PENDING;
    Done(Irp, *St, 0);
    return 1;
}

/* The ready notes SlotClose and SlotDetach return: logged, nothing else. */
static void NoteReady(const SLOT_ACTIONS *Out)
{
    int i;
    for (i = 0; i < Out->Count; i++) {
        if (Out->A[i].Kind == SA_REMOVE_READY) Log("remove-ready", 0, 0, 0);
        else if (Out->A[i].Kind == SA_FREE_READY) Log("free-ready", 0, 0, 0);
        else Log("close-action-unexpected", (ULONG)Out->A[i].Kind, 0, 0);
    }
}

static NTSTATUS DispCreateClose(PDEVICE_OBJECT Dev, PIRP Irp)
{
    UCHAR mj;
    NTSTATUS st;
    PFILE_OBJECT fo;
    EXT *E;
    KIRQL irql;
    LONG opens;
    ULONG inst;
    int ok = 1, del = 0;
    SLOT_ACTIONS out;
    if (Late(Dev, Irp, &st)) return st;
    E = (EXT *)Dev->DeviceExtension;
    fo = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    mj = IoGetCurrentIrpStackLocation(Irp)->MajorFunction;
    out.Count = 0;
    KeAcquireSpinLock(&E->Lock, &irql);
    if (mj == IRP_MJ_CREATE) {
        ok = SlotOpen(&E->D);       /* refused once removal has begun */
        if (ok && fo) fo->FsContext = (PVOID)(ULONG_PTR)E->Instance;  /* the handle's device */
    }
    /* Every close decides the deletion by SlotClose, in this hold: REMOVE
       may have detached since Late looked (mtp.md 5.4). */
    if (mj == IRP_MJ_CLOSE) del = SlotClose(&E->D, &out);
    opens = E->D.Opens;
    inst = E->Instance;
    KeReleaseSpinLock(&E->Lock, irql);
    /* From here, unless this close claimed the deletion, nothing of the
       device is touched: REMOVE may be deleting it. */
    NoteReady(&out);
    if (!ok) {
        Log("create-refused-removing", (ULONG)(ULONG_PTR)fo, inst, 0);
        return Done(Irp, STATUS_DELETE_PENDING, 0);
    }
    if (mj == IRP_MJ_CREATE) InterlockedIncrement(&g_Creates);
    if (mj == IRP_MJ_CLEANUP) InterlockedIncrement(&g_Cleanups);
    if (mj == IRP_MJ_CLOSE) InterlockedIncrement(&g_Closes);
    Log(mj == IRP_MJ_CREATE ? "create" : mj == IRP_MJ_CLEANUP ? "cleanup" : "close",
        (ULONG)(ULONG_PTR)fo, inst, (ULONG)opens);
    Done(Irp, STATUS_SUCCESS, 0);
    if (del) DeleteDev(Dev, 1);             /* the last act */
    return STATUS_SUCCESS;
}

static NTSTATUS DispRead(PDEVICE_OBJECT Dev, PIRP Irp)
{
    ULONG len;
    NTSTATUS st;
    if (Late(Dev, Irp, &st)) return st;
    len = IoGetCurrentIrpStackLocation(Irp)->Parameters.Read.Length;
    if (!len || len > MAXBUF) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
    return Admit((EXT *)Dev->DeviceExtension, Irp, SLOT_BIN, len, -1, 0, 0, NULL);
}

/* A bulk OUT write of up to 64 bytes, on SLOT_BOUT (stopped as PRB_INT). */
static NTSTATUS DispWrite(PDEVICE_OBJECT Dev, PIRP Irp)
{
    ULONG len;
    NTSTATUS st;
    PVOID src;
    if (Late(Dev, Irp, &st)) return st;
    len = IoGetCurrentIrpStackLocation(Irp)->Parameters.Write.Length;
    if (!len || len > 64 || !Irp->MdlAddress) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
    if (!((EXT *)Dev->DeviceExtension)->PipeOut) return Done(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    src = MmGetSystemAddressForMdl(Irp->MdlAddress);
    return Admit((EXT *)Dev->DeviceExtension, Irp, SLOT_BOUT, len, -1, KIND_BULKOUT, 0, src);
}

static const NTSTATUS g_Statuses[] = {
    STATUS_SUCCESS, STATUS_CANCELLED, STATUS_IO_TIMEOUT, STATUS_DELETE_PENDING, STATUS_SHARING_VIOLATION,
    STATUS_ACCESS_DENIED, STATUS_INVALID_PARAMETER, STATUS_DEVICE_BUSY, STATUS_DEVICE_NOT_CONNECTED,
    STATUS_UNSUCCESSFUL
};

static int SlotOf(ULONG Which)
{
    return Which == PRB_BIN ? SLOT_BIN : Which == PRB_INT ? SLOT_BOUT : Which == PRB_WAIT ? SLOT_WAIT : SLOT_CTL;
}

static NTSTATUS DispIoctl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    EXT *E;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG code, in, out, which = 0, req, i, v[PRB_STATE_ULONGS];
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;
    KIRQL irql;
    NTSTATUS st;
    if (Late(Dev, Irp, &st)) return st;
    E = (EXT *)Dev->DeviceExtension;
    code = s->Parameters.DeviceIoControl.IoControlCode;
    in = s->Parameters.DeviceIoControl.InputBufferLength;
    out = s->Parameters.DeviceIoControl.OutputBufferLength;
    if (in >= sizeof(ULONG)) which = *(ULONG *)buf;
    switch (code) {
    case IOCTL_PRB_CANCEL:
    case IOCTL_PRB_ABORT:
        if (in != sizeof(ULONG) || which > (ULONG)(code == IOCTL_PRB_ABORT ? PRB_INT : PRB_WAIT))
            return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        req = Stop(E, SlotOf(which), code == IOCTL_PRB_ABORT);
        if (out >= sizeof(ULONG)) {
            *(ULONG *)buf = req;
            return Done(Irp, STATUS_SUCCESS, sizeof(ULONG));
        }
        return Done(Irp, STATUS_SUCCESS, 0);
    case IOCTL_PRB_RESET:
        if (in != sizeof(ULONG) || which > PRB_INT) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        return Admit(E, Irp, SLOT_CTL, 0, SlotOf(which), CTL_RESETPIPE, (int)which, NULL);
    case IOCTL_PRB_STATUS:
        if (in != sizeof(ULONG) || which >= sizeof(g_Statuses) / sizeof(g_Statuses[0]))
            return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        Log("status-test", which, (ULONG)g_Statuses[which], 0);
        return Done(Irp, g_Statuses[which], 3);
    case IOCTL_PRB_INT:
        if (out < 4 || out > 64) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        return Admit(E, Irp, SLOT_BOUT, out, -1, 0, 0, NULL);
    case IOCTL_PRB_DEVSTATUS:
        if (out < 4 || out > 64) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        return Admit(E, Irp, SLOT_CTL, out, -1, CTL_DEVSTATUS, 0, NULL);
    case IOCTL_PRB_WAIT:
        /* The design's software wait: a request on SLOT_WAIT with no lower
           IRP, pending until it is stopped, cancelled or removed. */
        return Admit(E, Irp, SLOT_WAIT, 0, -1, 0, 0, NULL);
    case IOCTL_PRB_COUNTERS:
        if (out < PRB_COUNTERS_BYTES) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        return Done(Irp, STATUS_SUCCESS, Counters(buf));
    case IOCTL_PRB_STATE:
        if (out < sizeof(v)) return Done(Irp, STATUS_INVALID_PARAMETER, 0);
        KeAcquireSpinLock(&E->Lock, &irql);
        v[0] = (ULONG)E->Started;
        for (i = 0; i < 4; i++) {
            SLOT *S = &E->D.S[i < 3 ? i : SLOT_WAIT];
            v[1 + i * 3] = S->State;
            v[2 + i * 3] = S->Request;
            v[3 + i * 3] = (ULONG)S->Lower | ((ULONG)S->Cancel << 8) | ((ULONG)S->Pins << 16);
        }
        KeReleaseSpinLock(&E->Lock, irql);
        RtlCopyMemory(buf, v, sizeof(v));
        return Done(Irp, STATUS_SUCCESS, sizeof(v));
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
    return st;
}

/* Select the configuration with every interface at alternate setting 0
   (the list has bNumInterfaces + 1 entries, the last zero: the contract of
   USBD_CreateConfigurationRequestEx) and take the pipes of the still image
   interface (class 6/1/1, else the first). Called with the gate closed. */
static NTSTATUS Start(EXT *E)
{
    URB u;
    PUSB_CONFIGURATION_DESCRIPTOR cd;
    PUSB_INTERFACE_DESCRIPTOR id, d;
    PUSBD_INTERFACE_LIST_ENTRY list;
    PUSBD_INTERFACE_INFORMATION ii = NULL;
    PURB sel;
    PUCHAR at;
    NTSTATUS st;
    ULONG i, k, count, n = 0;
    UCHAR head[9];
    USHORT total;
    E->Pipe[PRB_BIN] = E->Pipe[PRB_INT] = E->PipeOut = NULL;
    UsbBuildGetDescriptorRequest(&u, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
                                 USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, head, NULL, 9, NULL);
    st = Sync(E, &u);
    if (!NT_SUCCESS(st)) return st;
    total = ((PUSB_CONFIGURATION_DESCRIPTOR)head)->wTotalLength;
    count = ((PUSB_CONFIGURATION_DESCRIPTOR)head)->bNumInterfaces;
    if (total < 9 || total > 4096 || count < 1 || count > 32) return STATUS_DEVICE_CONFIGURATION_ERROR;
    cd = (PUSB_CONFIGURATION_DESCRIPTOR)ExAllocatePool(NonPagedPool, total);
    if (!cd) return STATUS_INSUFFICIENT_RESOURCES;
    list = (PUSBD_INTERFACE_LIST_ENTRY)ExAllocatePool(NonPagedPool, (count + 1) * sizeof(USBD_INTERFACE_LIST_ENTRY));
    if (!list) { ExFreePool(cd); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(list, (count + 1) * sizeof(USBD_INTERFACE_LIST_ENTRY));
    UsbBuildGetDescriptorRequest(&u, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
                                 USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, cd, NULL, total, NULL);
    st = Sync(E, &u);
    if (NT_SUCCESS(st) && (cd->wTotalLength != total || cd->bNumInterfaces != count)) st = STATUS_DEVICE_CONFIGURATION_ERROR;
    /* Every interface at alternate setting 0, in descriptor order. */
    for (at = (PUCHAR)cd; NT_SUCCESS(st) && n < count; at = (PUCHAR)d + d->bLength) {
        d = USBD_ParseConfigurationDescriptorEx(cd, at, -1, 0, -1, -1, -1);
        if (!d) break;
        list[n++].InterfaceDescriptor = d;
    }
    id = NULL;
    for (k = 0; k < n && !id; k++) {
        d = list[k].InterfaceDescriptor;
        if (d->bInterfaceClass == 6 && d->bInterfaceSubClass == 1 && d->bInterfaceProtocol == 1) id = d;
    }
    if (!id && n) id = list[0].InterfaceDescriptor;
    Log("interfaces", n, count, id ? id->bInterfaceNumber : 0xFF);
    if (!NT_SUCCESS(st) || n != count || !id) {
        ExFreePool(list);
        ExFreePool(cd);
        return NT_SUCCESS(st) ? STATUS_DEVICE_CONFIGURATION_ERROR : st;
    }
    Log("interface", id->bInterfaceNumber, (id->bInterfaceClass << 16) | (id->bInterfaceSubClass << 8) |
        id->bInterfaceProtocol, id->bNumEndpoints);
    sel = USBD_CreateConfigurationRequestEx(cd, list);
    for (k = 0; sel && k < n; k++) {
        if (!list[k].Interface) { ExFreePool(sel); sel = NULL; break; }
        for (i = 0; i < list[k].Interface->NumberOfPipes; i++) list[k].Interface->Pipes[i].MaximumTransferSize = MAXBUF;
        if (list[k].InterfaceDescriptor == id) ii = list[k].Interface;
    }
    if (!sel || !ii) {
        if (sel) ExFreePool(sel);
        ExFreePool(list);
        ExFreePool(cd);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    st = Sync(E, sel);
    Log("select-config", (ULONG)st, (ULONG)sel->UrbHeader.Status, n);
    if (NT_SUCCESS(st)) {
        E->Interface = id->bInterfaceNumber;
        for (i = 0; i < ii->NumberOfPipes; i++) {
            PUSBD_PIPE_INFORMATION p = &ii->Pipes[i];
            if (p->PipeType == UsbdPipeTypeBulk && USB_ENDPOINT_DIRECTION_IN(p->EndpointAddress)) E->Pipe[PRB_BIN] = p->PipeHandle;
            if (p->PipeType == UsbdPipeTypeInterrupt) E->Pipe[PRB_INT] = p->PipeHandle;
            if (p->PipeType == UsbdPipeTypeBulk && !USB_ENDPOINT_DIRECTION_IN(p->EndpointAddress)) E->PipeOut = p->PipeHandle;
            Log("pipe", p->EndpointAddress, p->PipeType, p->MaximumPacketSize);
        }
        if (!E->Pipe[PRB_BIN] || !E->Pipe[PRB_INT]) {
            Log("refused-no-endpoints", 0, 0, 0);
            st = STATUS_DEVICE_CONFIGURATION_ERROR;
        }
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

/* Wait at PASSIVE_LEVEL (polling every 10 ms, the lock released between
   looks) until every slot is idle and every tail done; LimitMs 0: no
   limit. The time is measured in elapsed ticks (Ms), not counted in
   requested delays. Returns 1 if it got there. */
static int WaitIdle(EXT *E, const char *What, ULONG LimitMs)
{
    LARGE_INTEGER step;
    KIRQL irql;
    ULONG t0 = Ms(), last = t0, now;
    int idle;
    step.QuadPart = -100000;        /* 10 ms */
    for (;;) {
        KeAcquireSpinLock(&E->Lock, &irql);
        idle = SlotAllIdle(&E->D) && !E->D.Tails;
        KeReleaseSpinLock(&E->Lock, irql);
        now = Ms();
        if (idle || (LimitMs && now - t0 >= LimitMs)) break;
        if (now - last >= 5000) {
            last = now;
            Log("waiting-ms", now - t0, 0, 0);
        }
        KeDelayExecutionThread(KernelMode, FALSE, &step);
    }
    Log(What, now - t0, (ULONG)idle, 0);
    return idle;
}

static NTSTATUS DispPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    EXT *E;
    UCHAR mn;
    NTSTATUS st;
    SLOT_ACTIONS out;
    KIRQL irql;
    ULONG inst;
    int i, idle;
    if (Late(Dev, Irp, &st)) return st;
    E = (EXT *)Dev->DeviceExtension;
    mn = IoGetCurrentIrpStackLocation(Irp)->MinorFunction;
    Log("pnp", mn, 0, 0);
    switch (mn) {
    case IRP_MN_START_DEVICE:
        /* The gate is closed (never opened, or closed by STOP), and no
           request is out (STOP waited for its rundown), so the pipes may
           change. */
        st = ForwardWait(E, Irp);
        if (NT_SUCCESS(st)) st = Start(E);
        KeAcquireSpinLock(&E->Lock, &irql);
        E->Configured = NT_SUCCESS(st);
        E->Started = E->Configured;
        E->StopQueried = 0;
        KeReleaseSpinLock(&E->Lock, irql);
        Log("started", (ULONG)st, 0, 0);
        return Done(Irp, st, 0);
    case IRP_MN_QUERY_STOP_DEVICE:
        /* Refused while a request is out; else the gate closes until STOP
           or CANCEL_STOP. */
        KeAcquireSpinLock(&E->Lock, &irql);
        idle = SlotAllIdle(&E->D) && !E->D.Tails;
        if (idle) {
            E->Started = 0;
            E->StopQueried = 1;
        }
        KeReleaseSpinLock(&E->Lock, irql);
        Log("query-stop", (ULONG)idle, 0, 0);
        if (!idle) return Done(Irp, STATUS_DEVICE_BUSY, 0);
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
        /* STOP is never failed and does not go down before the rundown: it
           stays here, at PASSIVE_LEVEL, until every slot is idle. */
        out.Count = 0;
        KeAcquireSpinLock(&E->Lock, &irql);
        Gate(E, &out);
        E->Configured = 0;
        E->StopQueried = 0;
        KeReleaseSpinLock(&E->Lock, irql);
        Run(E, &out);
        WaitIdle(E, "stop-rundown-ms", 0);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(E->Lower, Irp);
    case IRP_MN_REMOVE_DEVICE:
        InterlockedIncrement(&g_Removes);
        out.Count = 0;
        KeAcquireSpinLock(&E->Lock, &irql);
        E->Started = 0;
        E->Configured = 0;
        for (i = 0; i < SLOT_COUNT; i++) MarkStop(E, i);
        SlotRemove(&E->D, &E->Ops, &out);
        KeReleaseSpinLock(&E->Lock, irql);
        Run(E, &out);
        /* No power IRP may reach the lower stack after its REMOVE: close
           the power gate and drain the forwarding first. */
        PowerDrain(E);
        IoSkipCurrentIrpStackLocation(Irp);
        st = IoCallDriver(E->Lower, Irp);
        /* SA_REMOVE_READY (every slot idle, every tail done), within 60 s
           of elapsed time. */
        idle = WaitIdle(E, "remove-rundown-ms", 60000);
        KeAcquireSpinLock(&E->Lock, &irql);
        i = E->D.RemoveReady;
        KeReleaseSpinLock(&E->Lock, irql);
        if (!idle || !i) {
            /* A lower request never came back: keep everything (a deliberate leak). */
            InterlockedIncrement(&g_Leaks);
            Log("remove-leaked", (ULONG)i, 0, 0);
            return st;
        }
        /* The device object stays while a handle is open (a zombie that
           refuses all but cleanup, close and the counters); the last close
           deletes it. 98 SE gives a deleted object's memory to the next
           device, and IRPs for open handles follow the old pointer. */
        IoDeleteSymbolicLink(&E->Link);
        IoDetachDevice(E->Lower);
        inst = E->Instance;
        KeAcquireSpinLock(&E->Lock, &irql);
        idle = (int)E->D.Opens;             /* read before the detach is published */
        i = SlotDetach(&E->D);              /* slot.c's verdict, not REMOVE's own count */
        if (!i) InterlockedIncrement(&g_Zombies);   /* before a close can delete and count it down */
        KeReleaseSpinLock(&E->Lock, irql);
        if (i) {
            DeleteDev(Dev, 0);              /* the last act */
            return st;
        }
        /* Only locals and the driver's counters from here: the last close
           may already be deleting the device. */
        Log("zombie-kept", inst, (ULONG)idle, (ULONG)g_Zombies);
        WorkDrain("remove-work-drained-ms");
        return st;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(E->Lower, Irp);
}

/* Power (98 SE and ME): DO_POWER_PAGABLE is set (AddDevice), and Power()
   forwards at PASSIVE_LEVEL only, PoStartNextPowerIrp first. */
static NTSTATUS DispPower(PDEVICE_OBJECT Dev, PIRP Irp)
{
    NTSTATUS st;
    EXT *E;
    if (Late(Dev, Irp, &st)) return st;
    E = (EXT *)Dev->DeviceExtension;
    return Power(E, Irp);
}

static NTSTATUS DispOther(PDEVICE_OBJECT Dev, PIRP Irp)
{
    NTSTATUS st;
    if (Late(Dev, Irp, &st)) return st;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(((EXT *)Dev->DeviceExtension)->Lower, Irp);
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Drv, PDEVICE_OBJECT Pdo)
{
    UNICODE_STRING name;
    PDEVICE_OBJECT dev;
    EXT *E;
    NTSTATUS st;
    int i;
    static WCHAR link[] = L"\\DosDevices\\MTPPRB0";     /* always the newest device */
    static WCHAR nbuf[] = L"\\Device\\MtpPrb00000";     /* one name per device: a zombie keeps its own */
    ULONG inst = (ULONG)InterlockedIncrement(&g_Instance), k = inst;
    for (i = 4; i >= 0; i--, k /= 10) nbuf[14 + i] = (WCHAR)(L'0' + k % 10);
    RtlInitUnicodeString(&name, nbuf);
    st = IoCreateDevice(Drv, sizeof(EXT), &name, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
    if (!NT_SUCCESS(st)) return st;
    if (dev == g_Deleted) {
        /* 98 SE gave the new device the deleted one's memory (read on
           2026-11-16): it is no longer the deleted device. An IRP for a
           handle of the old device would now reach this one; logged. */
        Log("devobj-reused", (ULONG)(ULONG_PTR)dev, (ULONG)g_Late, 0);
        g_Deleted = NULL;
    }
    E = (EXT *)dev->DeviceExtension;
    RtlZeroMemory(E, sizeof(*E));       /* Started = 0: the gate is closed until START */
    E->Instance = inst;
    E->Self = dev;
    KeInitializeSpinLock(&E->Lock);
    SlotInit(&E->D);
    E->Ops.Ctx = E;
    E->Ops.ArmTimer = OpArm;
    E->Ops.CancelTimer = OpCancelTimer;
    E->Ops.SetCancel = OpSetCancel;
    E->Ops.IrpCancelled = OpIrpCancelled;
    E->Ops.ClearCancel = OpClearCancel;
    E->Lower = IoAttachDeviceToDeviceStack(dev, Pdo);
    if (!E->Lower) {
        IoDeleteDevice(dev);
        return STATUS_NO_SUCH_DEVICE;
    }
    for (i = 0; i < SLOT_COUNT; i++) {
        XSLOT *x = &E->X[i];
        g_Wctx[i].E = E;
        g_Wctx[i].Slot = i;
        KeInitializeEvent(&x->AbortEvent, NotificationEvent, FALSE);
        x->Irp = IoAllocateIrp(E->Lower->StackSize, FALSE);
        x->AbortIrp = IoAllocateIrp(E->Lower->StackSize, FALSE);
        x->Urb = (PURB)ExAllocatePool(NonPagedPool, sizeof(URB));
        x->AbortUrb = (PURB)ExAllocatePool(NonPagedPool, sizeof(URB));
        x->Buf = (PUCHAR)ExAllocatePool(NonPagedPool, i == SLOT_BIN ? MAXBUF : 64);
        if (!x->Irp || !x->AbortIrp || !x->Urb || !x->AbortUrb || !x->Buf) {
            Log("adddevice-no-memory", i, 0, 0);
            IoDetachDevice(E->Lower);
            IoDeleteDevice(dev);    /* (what was allocated is leaked: a probe) */
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    RtlInitUnicodeString(&E->Link, link);
    st = IoCreateSymbolicLink(&E->Link, &name);
    if (!NT_SUCCESS(st)) Log("link-failed", (ULONG)st, inst, 0);
    dev->Flags |= DO_DIRECT_IO;
    dev->Flags |= DO_POWER_PAGABLE;     /* 98 SE and ME: power IRPs at PASSIVE_LEVEL */
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    Log("adddevice", (ULONG)(ULONG_PTR)dev, inst, 0);
    return STATUS_SUCCESS;
}

static VOID Unload(PDRIVER_OBJECT Drv)
{
    (void)Drv;
    Log("unload", 0, 0, 0);
    WorkDrain("unload-work-drained-ms");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING Path)
{
    int i;
    (void)Path;
    KeInitializeSpinLock(&g_LogLock);
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) Drv->MajorFunction[i] = DispOther;
    Drv->MajorFunction[IRP_MJ_CREATE] = DispCreateClose;
    Drv->MajorFunction[IRP_MJ_CLEANUP] = DispCreateClose;
    Drv->MajorFunction[IRP_MJ_CLOSE] = DispCreateClose;
    Drv->MajorFunction[IRP_MJ_READ] = DispRead;
    Drv->MajorFunction[IRP_MJ_WRITE] = DispWrite;
    Drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispIoctl;
    Drv->MajorFunction[IRP_MJ_PNP] = DispPnp;
    Drv->MajorFunction[IRP_MJ_POWER] = DispPower;
    Drv->DriverExtension->AddDevice = AddDevice;
    Drv->DriverUnload = Unload;
    Log("driverentry", 0, 0, 0);
    return STATUS_SUCCESS;
}
