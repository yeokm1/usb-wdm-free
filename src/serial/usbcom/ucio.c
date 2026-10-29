/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ucio.c - USBCOM.SYS port I/O: the URB pump, the direct-call table the
 * VxD uses (ucif.h), pipe resets and teardown (design 6.2, 7.9, 7.10).
 *
 * Every USB submission happens from the port's DPC, a URB completion
 * routine or the pipe-reset work item, never directly from a VxD call:
 * the table entries only record work and queue the DPC.
 *
 * The control requests of a port, CDC-ACM's or a chip module's (design
 * 6.3), come from chipcore.c's step generator, one control transfer at a
 * time, in the order the operations were queued.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include "usbcom.h"

static NTSTATUS UcComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx);
static void     UcPump(UC_PORT *p);
static VOID     UcPollDpc(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2);
static void     UcQueueReset(UC_PORT *p);

/* ---- helpers ------------------------------------------------------------- */

static void UcPrepare(UC_PORT *p, UC_XFER *x)
{
    CCHAR stack = p->Fdo->Lower->StackSize;
    PIO_STACK_LOCATION s;

    IoInitializeIrp(x->Irp, IoSizeOfIrp(stack), stack);
    s = IoGetNextIrpStackLocation(x->Irp);
    s->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    s->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    s->Parameters.Others.Argument1 = &x->Urb;
    IoSetCompletionRoutine(x->Irp, UcComplete, x, TRUE, TRUE, TRUE);
}

static void UcBuildBulk(UC_XFER *x, USBD_PIPE_HANDLE pipe, ULONG len, BOOLEAN in)
{
    RtlZeroMemory(&x->Urb, sizeof(x->Urb));
    UsbBuildInterruptOrBulkTransferRequest((PURB)&x->Urb,
        sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER), pipe, x->Buffer, NULL,
        len, in ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK)
                : USBD_TRANSFER_DIRECTION_OUT, NULL);
}

/* A control request from a step of chipcore.c's generator. */
void UcBuildControlUrb(PURB urb, const AC_SETUP *s, PUCHAR buf)
{
    USHORT fn;
    ULONG flags = (s->Type & 0x80) ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK)
                                   : USBD_TRANSFER_DIRECTION_OUT;
    switch (s->Type & 0x7F) {
    case 0x20: fn = URB_FUNCTION_CLASS_DEVICE; break;
    case 0x21: fn = URB_FUNCTION_CLASS_INTERFACE; break;
    case 0x41: fn = URB_FUNCTION_VENDOR_INTERFACE; break;
    default:   fn = URB_FUNCTION_VENDOR_DEVICE; break;
    }
    RtlZeroMemory(urb, sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST));
    UsbBuildVendorRequest(urb, fn, sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST), flags, 0,
        s->Request, s->Value, s->Index, s->Length ? buf : NULL, NULL, s->Length, NULL);
}

/* Appends an operation of the SYS's own (AC_OP_*). Lock held. */
static void UcAddOp(UC_PORT *p, ULONG op)
{
    UCIF_OP *o;
    if (p->OpCount >= UC_OPS)
        return;
    o = &p->Ops[(p->OpHead + p->OpCount) % UC_OPS];
    RtlZeroMemory(o, sizeof(*o));
    o->Op = op;
    p->OpCount++;
}

/* ---- the CP210x status poll and the FTDI status wait (design 6.3) ---------- */

/* Sets the poll timer; its Outstanding count is released when it fires
   (UcPollDpc) or is cancelled (UcCancelPoll). The CP210x polls with it;
   the FTDI uses it only while it waits for a status report. Lock held. */
static void UcArmPoll(UC_PORT *p)
{
    LARGE_INTEGER t;
    if (p->PollArmed || !(AcChipPolls(&p->Chip) || p->AwaitStatus) || p->Stopping ||
        p->Suspended || p->Gone)
        return;
    t.QuadPart = -10 * 1000 * (LONGLONG)UC_POLL_MS;
    p->PollArmed = TRUE;
    InterlockedIncrement(&p->Outstanding);
    (void)KeSetTimer(&p->PollTimer, t, &p->PollDpc);
}

/* Lock held. A timer that has already fired releases its count itself. */
static void UcCancelPoll(UC_PORT *p)
{
    p->PollDue = FALSE;
    if (p->PollArmed && KeCancelTimer(&p->PollTimer)) {
        p->PollArmed = FALSE;
        if (InterlockedDecrement(&p->Outstanding) == 0)
            KeSetEvent(&p->Drained, IO_NO_INCREMENT, FALSE);
    }
}

/*
 * A chip's bulk OUT waits behind the configuration queued before it (design
 * 6.3): any operation but a status poll or a close. Lock held.
 */
static BOOLEAN UcConfigPending(UC_PORT *p)
{
    ULONG i;
    if (p->Chip.Family == AC_FAM_CDC)
        return FALSE;
    if (p->RestoreMask || p->CurRestore || p->AwaitStatus)
        return TRUE;
    for (i = 0; i < p->OpCount; i++) {
        ULONG op = p->Ops[(p->OpHead + i) % UC_OPS].Op;
        if (op != AC_OP_POLL && op != AC_OP_CLOSE)
            return TRUE;
    }
    return FALSE;
}

static BOOLEAN UcDataBusy(UC_PORT *p)
{
    ULONG i;
    for (i = 0; i < UC_IN_URBS; i++)
        if (p->In[i].Busy) return TRUE;
    for (i = 0; i < UC_OUT_URBS; i++)
        if (p->Out[i].Busy) return TRUE;
    return p->Notify.Busy;
}

/* Collects the busy data transfers (and the control one if asked). */
static ULONG UcCollectBusy(UC_PORT *p, PIRP *list, BOOLEAN control)
{
    ULONG i, n = 0;
    for (i = 0; i < UC_IN_URBS; i++)
        if (p->In[i].Busy) list[n++] = p->In[i].Irp;
    for (i = 0; i < UC_OUT_URBS; i++)
        if (p->Out[i].Busy) list[n++] = p->Out[i].Irp;
    if (p->Notify.Busy) list[n++] = p->Notify.Irp;
    if (control && p->Ctl.Busy) list[n++] = p->Ctl.Irp;
    return n;
}

#define UC_ALL_XFERS (UC_IN_URBS + UC_OUT_URBS + 2)

static void UcCancelList(PIRP *list, ULONG n)
{
    ULONG i;
    for (i = 0; i < n; i++)
        IoCancelIrp(list[i]);
}

static void UcDone(UC_PORT *p)
{
    if (InterlockedDecrement(&p->Outstanding) == 0)
        KeSetEvent(&p->Drained, IO_NO_INCREMENT, FALSE);
}

static void UcQueueDpc(UC_PORT *p)
{
    InterlockedIncrement(&p->Outstanding);
    if (!KeInsertQueueDpc(&p->Dpc, NULL, NULL))
        UcDone(p);
}

static VOID UcDpc(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
    UC_PORT *p = (UC_PORT *)ctx;
    UNREFERENCED_PARAMETER(dpc);
    UNREFERENCED_PARAMETER(a1);
    UNREFERENCED_PARAMETER(a2);
    UcPump(p);
    UcDone(p);
}

static VOID UcPollDpc(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
    UC_PORT *p = (UC_PORT *)ctx;
    KIRQL irql;
    UNREFERENCED_PARAMETER(dpc);
    UNREFERENCED_PARAMETER(a1);
    UNREFERENCED_PARAMETER(a2);
    KeAcquireSpinLock(&p->Lock, &irql);
    p->PollArmed = FALSE;
    if (p->Attached && !p->Stopping && !p->Gone && !p->Suspended) {
        if (AcChipPolls(&p->Chip)) {
            p->PollDue = TRUE;
        } else if (p->AwaitStatus && !p->OpenPending &&
                   p->StatusTicks++ >= UC_STATUS_TICKS) {
            /* No status report since the opens ran (a chip sends one
               within its latency timer): bulk OUT goes on the CTS last
               known rather than never (design 6.3). */
            p->AwaitStatus = FALSE;
            UCT1("COM%lu FTDI: no status report after the open, transmitting",
                 p->PortNumber);
        }
        UcArmPoll(p);
    }
    KeReleaseSpinLock(&p->Lock, irql);
    UcPump(p);
    UcDone(p);
}

/* ---- pipe reset (PASSIVE_LEVEL work item) ------------------------------ */

static NTSTATUS UcPipeRequest(UC_PORT *p, USBD_PIPE_HANDLE pipe, USHORT function)
{
    struct _URB_PIPE_REQUEST urb;

    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = function;
    urb.PipeHandle = pipe;
    return UcSubmitUrbSync(p->Fdo, (PURB)&urb);
}

/* Any transfer of this kind still submitted? Lock held. */
static BOOLEAN UcKindBusy(UC_PORT *p, UCHAR kind)
{
    ULONG i;
    if (kind == UC_X_IN) {
        for (i = 0; i < UC_IN_URBS; i++)
            if (p->In[i].Busy) return TRUE;
    } else {
        for (i = 0; i < UC_OUT_URBS; i++)
            if (p->Out[i].Busy) return TRUE;
    }
    return FALSE;
}

/*
 * Aborts what is still pending on the pipe, waits (up to a second) until
 * none of our transfers on it is submitted, then resets it: a reset is
 * only issued on an idle pipe. Returns TRUE if the reset succeeded.
 */
static BOOLEAN UcResetPipe(UC_PORT *p, USBD_PIPE_HANDLE pipe, UCHAR kind)
{
    LARGE_INTEGER t;
    ULONG waited;
    KIRQL irql;
    BOOLEAN busy = TRUE;

    (void)UcPipeRequest(p, pipe, URB_FUNCTION_ABORT_PIPE);
    t.QuadPart = -10 * 1000 * 20;               /* 20 ms */
    for (waited = 0; waited < 1000; waited += 20) {
        KeAcquireSpinLock(&p->Lock, &irql);
        busy = UcKindBusy(p, kind);
        if (busy)
            KeClearEvent(&p->Drained);
        KeReleaseSpinLock(&p->Lock, irql);
        if (!busy)
            break;
        KeWaitForSingleObject(&p->Drained, Executive, KernelMode, FALSE, &t);
    }
    if (busy)
        return FALSE;
    return (BOOLEAN)NT_SUCCESS(UcPipeRequest(p, pipe, URB_FUNCTION_RESET_PIPE));
}

static VOID UcResetWorker(PVOID ctx)
{
    UC_PORT *p = (UC_PORT *)ctx;
    KIRQL irql;
    BOOLEAN rx, tx, rxOk = FALSE, txOk = FALSE;

    KeAcquireSpinLock(&p->Lock, &irql);
    rx = (BOOLEAN)(p->RxHalted && p->RxResets < UC_RESET_TRIES && !p->Stopping);
    tx = (BOOLEAN)(p->TxHalted && p->TxResets < UC_RESET_TRIES && !p->Stopping);
    KeReleaseSpinLock(&p->Lock, irql);

    if (rx) rxOk = UcResetPipe(p, p->InPipe, UC_X_IN);
    if (tx) txOk = UcResetPipe(p, p->OutPipe, UC_X_OUT);

    /* A failed reset counts as a try and leaves the pipe halted. A reset
       asked for while this one ran (an open's, the PL2303 H) goes next. */
    KeAcquireSpinLock(&p->Lock, &irql);
    if (rx) { p->RxResets++; if (rxOk) p->RxHalted = FALSE; }
    if (tx) { p->TxResets++; if (txOk) p->TxHalted = FALSE; }
    p->ResetQueued = FALSE;
    if ((p->RxHalted && p->RxResets < UC_RESET_TRIES) || (p->TxHalted && p->TxResets < UC_RESET_TRIES))
        UcQueueReset(p);
    KeReleaseSpinLock(&p->Lock, irql);
    UcQueueDpc(p);
    UcDone(p);
}

/* Called with the lock held. */
static void UcQueueReset(UC_PORT *p)
{
    if (p->ResetQueued || p->Stopping)
        return;
    p->ResetQueued = TRUE;
    InterlockedIncrement(&p->Outstanding);
    ExInitializeWorkItem(&p->ResetItem, UcResetWorker, p);
    ExQueueWorkItem(&p->ResetItem, DelayedWorkQueue);
}

static BOOLEAN UcDeviceGone(NTSTATUS st)
{
    return st == STATUS_DEVICE_NOT_CONNECTED || st == STATUS_NO_SUCH_DEVICE ||
           st == STATUS_DELETE_PENDING;
}

/* ---- the pump ----------------------------------------------------------- */

/* Hands held IN data to the VxD in arrival order. Lock held. */
static void UcDeliverRx(UC_PORT *p)
{
    while (p->RxCount > 0 && p->Attached && p->Cb.RxData) {
        UC_XFER *x = &p->In[p->RxOrder[p->RxHead]];
        ULONG take = p->Cb.RxData(p->Cb.Context, x->Buffer + x->Offset,
                                  x->Length - x->Offset);
        if (take > x->Length - x->Offset)
            take = x->Length - x->Offset;
        x->Offset += take;
        if (x->Offset < x->Length)
            break;                       /* the queue is full: wait for UCK_RX */
        x->Held = FALSE;
        p->RxHead = (p->RxHead + 1) % UC_IN_URBS;
        p->RxCount--;
    }
}

static void UcPump(UC_PORT *p)
{
    PIRP cancel[UC_ALL_XFERS];
    UC_XFER *go[UC_ALL_XFERS];
    ULONG ngo = 0, ncancel = 0, i;
    LONG flags;
    KIRQL irql;
    BOOLEAN live;

    KeAcquireSpinLock(&p->Lock, &irql);
    flags = p->KickFlags;
    p->KickFlags = 0;

    if (flags & UCK_PURGE_RX) {
        for (i = 0; i < UC_IN_URBS; i++)
            p->In[i].Held = FALSE;
        p->RxHead = 0;
        p->RxCount = 0;
    }
    if (flags & UCK_PURGE_TX) {
        for (i = 0; i < UC_OUT_URBS; i++)
            if (p->Out[i].Busy) cancel[ncancel++] = p->Out[i].Irp;
    }

    /* Control operations also run after Detach (the DTR drop and the
       close on close), one control transfer at a time (design 6.3). */
    if (!p->Stopping && !p->Suspended && !p->Gone && !p->Ctl.Busy) {
        if (p->ResendPending) {
            /* Resume (7.10): the line state again, before the FIFO, so
               that operations queued while suspended are newer and win.
               An operation half done when the device slept restarts. */
            p->ResendPending = FALSE;
            if (p->Attached) {
                p->RestoreGen = p->OpenGen;
                p->RestoreMask |= UC_RS_OPEN | (p->HaveLine ? UC_RS_LINE : 0) |
                                  (p->HaveFlow ? UC_RS_FLOW : 0) | (p->HaveControl ? UC_RS_CONTROL : 0);
                if (p->CurRestore)
                    p->RestoreMask |= p->CurRestore;
                p->CurRestore = 0;
                p->OpStep = 0;
                p->CtlOk = TRUE;
                p->CtlLen = 0;
            }
        }
        if (p->PollDue && p->OpCount == 0 && p->Attached) {
            p->PollDue = FALSE;
            UcAddOp(p, AC_OP_POLL);
            p->Ops[(p->OpHead + p->OpCount - 1) % UC_OPS].Value = p->OpenGen;
        }
    }
    while (!p->Stopping && !p->Suspended && !p->Gone && !p->Ctl.Busy &&
           (p->OpCount || p->RestoreMask || p->CurRestore)) {
        UCIF_OP *op, ro;
        UC_XFER *x = &p->Ctl;
        AC_SETUP s;
        AC_U16 state = 0;
        ULONG step = p->OpStep;
        int r;

        if (p->CurRestore == 0 && p->OpStep == 0 && p->RestoreMask) {
            p->CurRestore = p->RestoreMask & (~p->RestoreMask + 1);   /* the lowest */
            p->RestoreMask &= ~p->CurRestore;
        }
        if (p->CurRestore) {
            RtlZeroMemory(&ro, sizeof(ro));
            if (p->CurRestore == UC_RS_OPEN) { ro.Op = AC_OP_OPEN; ro.Value = p->RestoreGen; }
            else if (p->CurRestore == UC_RS_LINE) ro = p->SentLine;
            else if (p->CurRestore == UC_RS_FLOW) ro = p->SentFlow;
            else ro = p->SentControl;
            op = &ro;
        } else if (p->OpCount) {
            op = &p->Ops[p->OpHead];
        } else {
            break;
        }

        if (op->Op == AC_OP_POLL && (op->Value != p->OpenGen || (p->OpStep == 0 && !p->Attached)))
            r = AC_STEP_DONE;                /* a previous attachment's poll: no status */
        else
            r = AcChipStep(&p->Chip, op->Op, op->Value, op->Data, &step, p->CtlOk,
                           p->CtlData, p->CtlLen, &s, &state);
        if ((r & AC_STEP_STATE) && p->Attached && p->Cb.SerialState) {
            UCT2("COM%lu chip state %04lx", p->PortNumber, state);
            p->Cb.SerialState(p->Cb.Context, state);
        }
        if (r & AC_STEP_SEND) {
            p->OpStep = step;
            if (s.Length > AC_SETUP_MAX)
                s.Length = AC_SETUP_MAX;
            RtlCopyMemory(p->CtlData, s.Data, AC_SETUP_MAX);
            p->CtlLen = 0;
            UcBuildControlUrb((PURB)&x->Urb, &s, p->CtlData);
            UCT4("COM%lu op %lx step %lu: request %08lx", p->PortNumber, op->Op, step,
                 ((ULONG)s.Type << 24) | ((ULONG)s.Request << 16) | s.Value);
            go[ngo++] = x;
            break;
        }
        /* A chip's bulk transfers start once this attachment's opens have
           all run - its own and a resume's - not an older one's still in
           the FIFO (6.3). */
        if (op->Op == AC_OP_OPEN && op->Value == p->OpenGen && p->OpensLeft) {
            if (p->CurRestore == UC_RS_OPEN)
                p->RestoreOpenOwed = FALSE;
            if (--p->OpensLeft == 0 && p->OpenPending) {
                p->OpenPending = FALSE;
                UCT1("COM%lu opened: bulk transfers may start", p->PortNumber);
            }
        }
        if (p->CurRestore) {
            p->CurRestore = 0;
        } else {
            /* Finished: remembered for resume if its last transfer worked. */
            if (p->CtlOk || p->OpStep == 0) {
                if (op->Op == UCOP_LINE_CODING) { p->SentLine = *op; p->HaveLine = TRUE; }
                if (op->Op == UCOP_CONTROL) { p->SentControl = *op; p->HaveControl = TRUE; }
                if (op->Op == UCOP_FLOW) { p->SentFlow = *op; p->HaveFlow = TRUE; }
            }
            p->OpHead = (p->OpHead + 1) % UC_OPS;
            p->OpCount--;
        }
        p->OpStep = 0;
        p->CtlOk = TRUE;
        p->CtlLen = 0;
    }

    /* Data after the control operations: a finished one may end a hold. */
    live = p->Attached && !p->Stopping && !p->Suspended && !p->Gone;
    if (live) {
        UcDeliverRx(p);
        if (!p->RxHalted && !p->OpenPending) {
            ULONG limit = (p->Fdo->Quirks & AC_Q_SINGLE_RX_URB) ? 1 : UC_IN_URBS;
            for (i = 0; i < limit; i++) {
                UC_XFER *x = &p->In[i];
                if (x->Busy || x->Held)
                    continue;
#if defined(UC_TEST_SPLIT_CANCEL)
                x->Seq = ++p->InSeq;
#endif
                x->Gen = p->RxGen;
                UcBuildBulk(x, p->InPipe, UC_IN_SIZE, TRUE);
                go[ngo++] = x;
            }
        }
        if (!p->TxHalted && !p->OpenPending && !UcConfigPending(p) && p->Cb.TxPull) {
            /* With SEND_ZERO_PACKET only one OUT transfer is in flight, so
               its ZLP cannot fall behind the next transfer. */
            ULONG outLimit = (p->Fdo->Quirks & AC_Q_SEND_ZERO_PACKET) ? 1 : UC_OUT_URBS;
            if (outLimit == 1 && UcKindBusy(p, UC_X_OUT))
                outLimit = 0;
            for (i = 0; i < outLimit; i++) {
                UC_XFER *x = &p->Out[i];
                ULONG n;
                if (x->Busy)
                    continue;
                n = p->Cb.TxPull(p->Cb.Context, x->Buffer, UC_OUT_SIZE);
                if (n == 0)
                    break;
                if (n > UC_OUT_SIZE)
                    n = UC_OUT_SIZE;
                x->Length = n;
                /* SEND_ZERO_PACKET (design 6.2): a ZLP follows a full packet. */
                x->Held = (UCHAR)((p->Fdo->Quirks & AC_Q_SEND_ZERO_PACKET) &&
                                  p->Func.OutMax && n % p->Func.OutMax == 0);
                UcBuildBulk(x, p->OutPipe, n, FALSE);
                go[ngo++] = x;
            }
        }
        if (p->NotifyPipe && !p->Notify.Busy && !p->NotifyHalted) {
            UC_XFER *x = &p->Notify;
            /* A PL2303's status is one packet of wMaxPacketSize (10): a
               longer request would wait for a short packet that never
               comes (design 6.3). CDC keeps 64, which a notification
               split over 8-byte packets still ends short within. */
            ULONG nlen = (p->Chip.Family == AC_FAM_PL2303 && p->Func.NotifyMax &&
                          p->Func.NotifyMax <= UC_NOTIFY_SIZE) ? p->Func.NotifyMax : UC_NOTIFY_SIZE;
            RtlZeroMemory(&x->Urb, sizeof(x->Urb));
            UsbBuildInterruptOrBulkTransferRequest((PURB)&x->Urb,
                sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER), p->NotifyPipe,
                x->Buffer, NULL, nlen,
                USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, NULL);
            go[ngo++] = x;
        }
    }

    for (i = 0; i < ngo; i++) {
        go[i]->Busy = TRUE;
        InterlockedIncrement(&p->Outstanding);
    }
    KeReleaseSpinLock(&p->Lock, irql);

    for (i = 0; i < ngo; i++) {
        UcPrepare(p, go[i]);
        IoCallDriver(p->Fdo->Lower, go[i]->Irp);
    }
    UcCancelList(cancel, ncancel);
}

static NTSTATUS UcComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UC_XFER *x = (UC_XFER *)ctx;
    UC_PORT *p = x->Port;
    NTSTATUS st = irp->IoStatus.Status;
    BOOLEAN ok = NT_SUCCESS(st);
    BOOLEAN quiet = (st == STATUS_CANCELLED) || p->Stopping || p->Suspended;
    BOOLEAN zlp = FALSE;
    KIRQL irql;

    UNREFERENCED_PARAMETER(dev);
    KeAcquireSpinLock(&p->Lock, &irql);
    switch (x->Kind) {
    case UC_X_IN:
        x->Busy = FALSE;
        if (ok) {
            x->Length = x->Urb.Bulk.TransferBufferLength;
            if (x->Length > UC_IN_SIZE)
                x->Length = UC_IN_SIZE;
            if (p->Chip.Family == AC_FAM_FTDI) {
                /* The two status bytes of every packet (design 6.3). */
                AC_U16 states[UC_IN_SIZE / 64];
                int ns, k;
                if (x->Gen != p->RxGen) {
                    /* From before a suspend or an open: its status is stale
                       and must not reach the chip's state either. */
                    x->Length = AcChipStrip(&p->Chip, x->Buffer, x->Length);
                    ns = 0;
                } else {
                    x->Length = AcChipRx(&p->Chip, x->Buffer, x->Length, states, &ns,
                                         UC_IN_SIZE / 64);
                }
                if (ns)
                    p->AwaitStatus = FALSE;
                for (k = 0; k < ns && p->Attached && p->Cb.SerialState; k++) {
                    UCT2("COM%lu FTDI status %04lx", p->PortNumber, states[k]);
                    p->Cb.SerialState(p->Cb.Context, states[k]);
                }
            }
            if (x->Length) {
                x->Offset = 0;
                x->Held = TRUE;
                p->RxOrder[(p->RxHead + p->RxCount) % UC_IN_URBS] = x->Index;
                p->RxCount++;
            }
            p->RxResets = 0;
        } else if (!quiet) {
            UCT2("COM%lu bulk IN failed %08lx", p->PortNumber, st);
            p->RxHalted = TRUE;
            if (!UcDeviceGone(st))
                UcQueueReset(p);
        }
        break;
    case UC_X_OUT: {
        ULONG sent = ok ? x->Urb.Bulk.TransferBufferLength : 0, i;
        BOOLEAN idle = TRUE;
        if (ok && x->Held && !quiet) {
            /* Send the zero-length packet before releasing this transfer. */
            UcBuildBulk(x, p->OutPipe, 0, FALSE);
            zlp = TRUE;
        } else {
            x->Busy = FALSE;
        }
        x->Held = FALSE;
        if (ok)
            p->TxResets = 0;
        else if (!quiet) {
            UCT2("COM%lu bulk OUT failed %08lx", p->PortNumber, st);
            p->TxHalted = TRUE;
            if (!UcDeviceGone(st))
                UcQueueReset(p);
        }
        for (i = 0; i < UC_OUT_URBS; i++)
            if (p->Out[i].Busy) idle = FALSE;
        if (p->Attached && p->Cb.TxDone)
            p->Cb.TxDone(p->Cb.Context, sent, idle);
        break;
    }
    case UC_X_NOTIFY:
        x->Busy = FALSE;
        if (ok && p->Attached && p->Cb.SerialState) {
            ULONG off = 0, len = x->Urb.Bulk.TransferBufferLength;
            USHORT state;
            if (len > UC_NOTIFY_SIZE)
                len = UC_NOTIFY_SIZE;
            while (AcChipNotify(&p->Chip, x->Buffer, len, &off, &state)) {
                if (state != p->LastState)
                    UCT3("COM%lu SERIAL_STATE %04lx (was %04lx)", p->PortNumber, state, p->LastState);
                p->LastState = state;
                p->Cb.SerialState(p->Cb.Context, state);
            }
        } else if (ok) {
            UCT2("COM%lu notification %lu bytes while not attached",
                 p->PortNumber, x->Urb.Bulk.TransferBufferLength);
        } else if (!ok && !quiet) {
            /* A failing notification pipe is left idle; data still flows. */
            p->NotifyHalted = TRUE;
            UCT1("notify failed %08lx", st);
        }
        break;
    default:                                    /* control */
        x->Busy = FALSE;
        if (p->OpCount || p->CurRestore) {
            UCT4("COM%lu control step %lu (restore %lx): %08lx", p->PortNumber, p->OpStep,
                 p->CurRestore, st);
            /* The next step of the same operation sees this result. */
            p->CtlOk = ok;
            p->CtlLen = (ok && (x->Urb.Class.TransferFlags & USBD_TRANSFER_DIRECTION_IN))
                        ? x->Urb.Class.TransferBufferLength : 0;
            if (p->CtlLen > AC_SETUP_MAX)
                p->CtlLen = AC_SETUP_MAX;
            p->OpStep++;
        }
        break;
    }
    KeReleaseSpinLock(&p->Lock, irql);

    if (zlp) {
        UcPrepare(p, x);
        IoCallDriver(p->Fdo->Lower, x->Irp);   /* still counted outstanding */
        return STATUS_MORE_PROCESSING_REQUIRED;
    }
    if (!p->Stopping)
        UcPump(p);
    UcDone(p);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* ---- the table (called by USBCOMV.VXD, see ucif.h) ------------------------- */

static void UcWaitData(UC_PORT *p, ULONG ms)
{
    LARGE_INTEGER t;
    ULONG waited = 0;
    t.QuadPart = -10 * 1000 * 50;               /* 50 ms */
    while (waited < ms) {
        KIRQL irql;
        BOOLEAN busy;
        KeAcquireSpinLock(&p->Lock, &irql);
        busy = UcDataBusy(p);
        if (busy)
            KeClearEvent(&p->Drained);
        KeReleaseSpinLock(&p->Lock, irql);
        if (!busy)
            return;
        KeWaitForSingleObject(&p->Drained, Executive, KernelMode, FALSE, &t);
        waited += 50;
    }
}

static long __stdcall UcAttach(void *port, const UCIF_CALLBACKS *cb)
{
    UC_PORT *p = (UC_PORT *)port;
    long rc = 1;
    KIRQL irql;
    ULONG i;

    if (cb == NULL || cb->Size < sizeof(UCIF_CALLBACKS) || !cb->RxData ||
        !cb->TxPull || !cb->TxDone || !cb->SerialState || !cb->Gone)
        return 2;
    InterlockedIncrement(&p->Busy);
    UcWaitData(p, 2000);
    KeAcquireSpinLock(&p->Lock, &irql);
    /* Room for this open and its whole end: the open, then what
       UC_OPS_RESERVED keeps (a close's flush, the close, a poll). */
    if (p->Started && !p->Gone && !p->Stopping && !p->Attached && !UcDataBusy(p) &&
        p->OpCount + 1 + UC_OPS_RESERVED <= UC_OPS) {
        p->Cb = *cb;
        p->Attached = TRUE;
        p->RxHead = p->RxCount = 0;
        for (i = 0; i < UC_IN_URBS; i++)
            p->In[i].Held = FALSE;
        p->RxHalted = p->TxHalted = p->NotifyHalted = FALSE;
        p->RxResets = p->TxResets = 0;
        p->LastState = 0;
        p->HaveLine = p->HaveControl = p->HaveFlow = FALSE;
        p->ResendPending = FALSE;
        p->KickFlags = 0;
        /* After what the previous open left (its DTR drop and close), and
           before this open's line coding (design 6.3). A chip's bulk
           transfers wait for both. */
        p->OpenGen++;
        UcAddOp(p, AC_OP_OPEN);
        p->Ops[(p->OpHead + p->OpCount - 1) % UC_OPS].Value = p->OpenGen;
        p->OpensLeft = 1;
        p->RestoreOpenOwed = FALSE;
        p->RxGen++;
        p->OpenPending = (BOOLEAN)(p->Chip.Family != AC_FAM_CDC);
        p->AwaitStatus = (BOOLEAN)(p->Chip.Family == AC_FAM_FTDI);
        p->StatusTicks = 0;
        p->RestoreMask = 0;                /* the previous open's: superseded */
        if (AcChipResetsPipes(&p->Chip)) {
            /* The PL2303 H: both bulk pipes reset first (design 6.3). */
            p->RxHalted = p->TxHalted = TRUE;
            UcQueueReset(p);
        }
        UcArmPoll(p);
        rc = 0;
    }
    KeReleaseSpinLock(&p->Lock, irql);
    UCT2("COM%lu attach: %ld", p->PortNumber, rc);
    if (rc == 0)
        UcQueueDpc(p);
    InterlockedDecrement(&p->Busy);
    return rc;
}

#if defined(UC_TEST_SPLIT_CANCEL)
/*
 * Test flavour "split" only, never released. At a close, before the rest:
 *   1. cancel the third-oldest busy bulk IN alone. The oldest INs are the
 *      TDs on xHCI98's ring (it queues newer ones in software), so xHCI98
 *      rewrites that TD in place as a No Op behind live ones;
 *   2. cancel the oldest alone. It is the ring's head, so xHCI98 moves the
 *      dequeue pointer to the next live TD, which lies before the No Op.
 * QEMU's qemu-xhci then fetches that live TD and the No Op after it, and
 * passes the No Op to the device as a zero-length bulk IN
 * (2026-10-22-usbcom98-legs.md): this makes that path happen at a close
 * instead of now and then. Each step waits for the cancel and 50 ms more,
 * so the endpoint restarts in between. Returns the list without them.
 */
static void UcSplitWait(UC_PORT *p, UC_XFER *x, const char *what)
{
    KEVENT never;
    LARGE_INTEGER t;
    ULONG i, waited = 0;

    UCT4("COM%lu split cancel %s: IN %lu (seq %lu) alone", p->PortNumber, what, x->Index, x->Seq);
    IoCancelIrp(x->Irp);
    KeInitializeEvent(&never, NotificationEvent, FALSE);
    t.QuadPart = -10 * 1000 * 10;           /* 10 ms */
    while (x->Busy && waited < 250) {
        KeWaitForSingleObject(&never, Executive, KernelMode, FALSE, &t);
        waited += 10;
    }
    for (i = 0; i < 5; i++)
        KeWaitForSingleObject(&never, Executive, KernelMode, FALSE, &t);
    UCT3("COM%lu split cancel %s: done after %lu ms", p->PortNumber, what, waited);
}

static ULONG UcSplitDrop(PIRP *list, ULONG n, PIRP irp)
{
    ULONG i;
    for (i = 0; i < n; i++) {
        if (list[i] == irp) {
            list[i] = list[--n];
            break;
        }
    }
    return n;
}

static ULONG UcSplitCancel(UC_PORT *p, PIRP *list, ULONG n)
{
    UC_XFER *by[UC_IN_URBS], *x;
    ULONG i, j, k = 0;

    /* The busy INs, oldest first (Seq wraps: compare differences). */
    for (i = 0; i < UC_IN_URBS; i++) {
        x = &p->In[i];
        if (!x->Busy)
            continue;
        for (j = k; j > 0 && x->Seq - by[j - 1]->Seq >= 0x80000000UL; j--)
            by[j] = by[j - 1];
        by[j] = x;
        k++;
    }
    UCT2("COM%lu split cancel: %lu busy INs", p->PortNumber, k);
    if (k < 3)
        return n;
    UcSplitWait(p, by[2], "third-oldest");
    n = UcSplitDrop(list, n, by[2]->Irp);
    UcSplitWait(p, by[0], "oldest");
    n = UcSplitDrop(list, n, by[0]->Irp);
    for (i = 0; i < UC_IN_URBS; i++)
        UCT4("COM%lu split cancel: IN %lu seq %lu busy %lu", p->PortNumber, i, p->In[i].Seq,
             p->In[i].Busy);
    return n;
}
#endif

static void __stdcall UcDetach(void *port)
{
    UC_PORT *p = (UC_PORT *)port;
    PIRP list[UC_ALL_XFERS];
    ULONG n;
    KIRQL irql;

    InterlockedIncrement(&p->Busy);
    KeAcquireSpinLock(&p->Lock, &irql);
    p->Attached = FALSE;
    RtlZeroMemory(&p->Cb, sizeof(p->Cb));
    UcCancelPoll(p);
    p->RestoreMask = 0;                    /* a closed port needs no restore */
    if (!p->Gone)
        UcAddOp(p, AC_OP_CLOSE);           /* after the VxD's DTR drop; drops it again */
    n = UcCollectBusy(p, list, FALSE);
    KeReleaseSpinLock(&p->Lock, irql);
    UCT2("COM%lu detach: cancelling %lu transfers", p->PortNumber, n);
#if defined(UC_TEST_SPLIT_CANCEL)
    n = UcSplitCancel(p, list, n);
#endif
    UcCancelList(list, n);
    UcQueueDpc(p);                         /* runs the DTR drop if queued */
    InterlockedDecrement(&p->Busy);
}

static void __stdcall UcKick(void *port, unsigned long flags)
{
    UC_PORT *p = (UC_PORT *)port;
    KIRQL irql;

    InterlockedIncrement(&p->Busy);
    KeAcquireSpinLock(&p->Lock, &irql);
    p->KickFlags |= (LONG)flags;
    KeReleaseSpinLock(&p->Lock, irql);
    UcQueueDpc(p);
    InterlockedDecrement(&p->Busy);
}

static long __stdcall UcControl(void *port, const UCIF_OP *op)
{
    UC_PORT *p = (UC_PORT *)port;
    long rc = 1;
    KIRQL irql;
    ULONG code, limit;

    if (op == NULL)
        return 2;
    code = op->Op & ~UCOP_F_CLOSING;
    if (code < UCOP_LINE_CODING || code > UCOP_FLOW)
        return 2;
    /* A close's flush may use the places kept for it, less one for the
       close itself (design 6.3). */
    limit = (op->Op & UCOP_F_CLOSING) ? UC_OPS - 1 : UC_OPS - UC_OPS_RESERVED;
    InterlockedIncrement(&p->Busy);
    KeAcquireSpinLock(&p->Lock, &irql);
    if (code == UCOP_BREAK && !(p->Func.Caps & AC_CAP_BREAK)) {
        rc = 0;                          /* not supported: accepted, not sent */
    } else if (!p->Gone && p->OpCount < limit) {
        p->Ops[(p->OpHead + p->OpCount) % UC_OPS] = *op;
        p->Ops[(p->OpHead + p->OpCount) % UC_OPS].Op = code;
        p->OpCount++;
        rc = 0;
    }
    KeReleaseSpinLock(&p->Lock, irql);
    if (rc == 0)
        UcQueueDpc(p);
    InterlockedDecrement(&p->Busy);
    return rc;
}

/* ---- lifetime ------------------------------------------------------------ */

static NTSTATUS UcXferInit(UC_PORT *p, UC_XFER *x, UCHAR kind, UCHAR index, ULONG size)
{
    CCHAR stack = p->Fdo->Lower->StackSize;

    x->Port = p;
    x->Kind = kind;
    x->Index = index;
    x->Irp = (PIRP)ExAllocatePool(NonPagedPool, IoSizeOfIrp(stack));
    if (x->Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (size) {
        x->Buffer = (PUCHAR)ExAllocatePool(NonPagedPool, size);
        if (x->Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

NTSTATUS UcPortCreate(UC_FDO *fdo, ULONG index, const AC_FUNC *f, UC_PORT **out)
{
    UC_PORT *p;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG i;

    *out = NULL;
    p = (UC_PORT *)ExAllocatePool(NonPagedPool, sizeof(UC_PORT));
    if (p == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(p, sizeof(*p));
    KeInitializeSpinLock(&p->Lock);
    KeInitializeDpc(&p->Dpc, UcDpc, p);
    KeInitializeDpc(&p->PollDpc, UcPollDpc, p);
    KeInitializeTimer(&p->PollTimer);
    KeInitializeEvent(&p->Drained, NotificationEvent, TRUE);
    p->CtlOk = TRUE;
    p->Fdo = fdo;
    p->Index = index;
    p->Func = *f;

    for (i = 0; i < UC_IN_URBS && NT_SUCCESS(st); i++)
        st = UcXferInit(p, &p->In[i], UC_X_IN, (UCHAR)i, UC_IN_SIZE);
    for (i = 0; i < UC_OUT_URBS && NT_SUCCESS(st); i++)
        st = UcXferInit(p, &p->Out[i], UC_X_OUT, (UCHAR)i, UC_OUT_SIZE);
    if (NT_SUCCESS(st))
        st = UcXferInit(p, &p->Notify, UC_X_NOTIFY, 0, UC_NOTIFY_SIZE);
    if (NT_SUCCESS(st))
        st = UcXferInit(p, &p->Ctl, UC_X_CONTROL, 0, 0);
    if (!NT_SUCCESS(st)) {
        UcPortFree(p);
        return st;
    }

    p->Table.Version = UCIF_VERSION;
    p->Table.Size = sizeof(UCIF_TABLE);
    p->Table.Port = p;
    p->Table.Caps = ((f->Caps & AC_CAP_BREAK) ? UCIF_CAP_BREAK : 0) |
                    ((f->Caps & AC_CAP_LINE) ? UCIF_CAP_LINE : 0);
    p->Table.Attach = UcAttach;
    p->Table.Detach = UcDetach;
    p->Table.Kick = UcKick;
    p->Table.Control = UcControl;
    p->Stopping = TRUE;                 /* until the PDO starts */
    *out = p;
    return STATUS_SUCCESS;
}

static void UcXferFree(UC_XFER *x)
{
    if (x->Irp) ExFreePool(x->Irp);
    if (x->Buffer) ExFreePool(x->Buffer);
    x->Irp = NULL;
    x->Buffer = NULL;
}

void UcPortFree(UC_PORT *p)
{
    ULONG i;
    for (i = 0; i < UC_IN_URBS; i++)
        UcXferFree(&p->In[i]);
    for (i = 0; i < UC_OUT_URBS; i++)
        UcXferFree(&p->Out[i]);
    UcXferFree(&p->Notify);
    UcXferFree(&p->Ctl);
    ExFreePool(p);
}

/*
 * Stops all I/O and, if the VxD is attached, tells it the port is gone
 * (design 7.9). PASSIVE_LEVEL. On return nothing of the port is in use.
 */
void UcPortStop(UC_PORT *p)
{
    PIRP list[UC_ALL_XFERS];
    UCIF_CALLBACKS cb;
    ULONG n, rounds;
    KIRQL irql;
    LARGE_INTEGER t;

    RtlZeroMemory(&cb, sizeof(cb));
    KeAcquireSpinLock(&p->Lock, &irql);
    p->Stopping = TRUE;
    if (p->Attached) {
        cb = p->Cb;
        p->Attached = FALSE;
        p->Gone = TRUE;
        RtlZeroMemory(&p->Cb, sizeof(p->Cb));
        /* Under the lock, like the other callbacks, so a Detach from
           PortClose cannot interleave with it. UvGone makes only
           asynchronous VMM calls and runs client notifications. */
        cb.Gone(cb.Context);
    }
    p->OpCount = 0;
    p->OpStep = 0;
    p->CtlOk = TRUE;
    p->ResendPending = FALSE;
    p->RestoreMask = 0;
    p->CurRestore = 0;
    p->RestoreOpenOwed = FALSE;
    p->OpensLeft = 0;
    UcCancelPoll(p);
    UCT2("COM%lu stop (VxD told: %lu)", p->PortNumber, cb.Gone != 0);
    KeReleaseSpinLock(&p->Lock, irql);

    if (KeRemoveQueueDpc(&p->Dpc))
        UcDone(p);

    t.QuadPart = -10 * 1000 * 50;
    for (rounds = 0; ; rounds++) {
        KeAcquireSpinLock(&p->Lock, &irql);
        n = UcCollectBusy(p, list, TRUE);
        if (p->Outstanding || p->Busy)
            KeClearEvent(&p->Drained);
        KeReleaseSpinLock(&p->Lock, irql);
        if (p->Outstanding == 0 && p->Busy == 0)
            break;
        UcCancelList(list, n);
        KeWaitForSingleObject(&p->Drained, Executive, KernelMode, FALSE, &t);
        if (rounds == 200 || rounds == 2000)
            UCT3("COM%lu still draining: %ld outstanding, %ld busy", p->PortNumber,
                 p->Outstanding, p->Busy);
    }

    KeAcquireSpinLock(&p->Lock, &irql);
    p->RxHead = p->RxCount = 0;
    for (n = 0; n < UC_IN_URBS; n++)
        p->In[n].Held = FALSE;
    KeReleaseSpinLock(&p->Lock, irql);
    UCT2("COM%lu stopped after %lu rounds", p->PortNumber, rounds);
}

/* The PDO started (again): a later open may attach. */
void UcPortStart(UC_PORT *p)
{
    KIRQL irql;
    KeAcquireSpinLock(&p->Lock, &irql);
    p->Stopping = FALSE;
    p->Gone = FALSE;
    p->Suspended = FALSE;
    p->Started = TRUE;
    KeReleaseSpinLock(&p->Lock, irql);
}

/* Device leaving D0: cancel the data transfers, keep the port attached. */
void UcPortSuspend(UC_PORT *p)
{
    PIRP list[UC_ALL_XFERS];
    ULONG n;
    KIRQL irql;

    KeAcquireSpinLock(&p->Lock, &irql);
    p->Suspended = TRUE;
    p->RxGen++;
    UcCancelPoll(p);
    n = UcCollectBusy(p, list, FALSE);
    KeReleaseSpinLock(&p->Lock, irql);
    UcCancelList(list, n);
}

/* Back in D0: send the line state again and restart (design 7.10). */
void UcPortResume(UC_PORT *p)
{
    KIRQL irql;

    KeAcquireSpinLock(&p->Lock, &irql);
    if (p->Stopping) {                     /* being removed: do nothing */
        KeReleaseSpinLock(&p->Lock, irql);
        return;
    }
    p->Suspended = FALSE;
    p->RxHalted = p->TxHalted = p->NotifyHalted = FALSE;
    /* The pump puts the line state back at the head once no control
       transfer is in flight (design 7.10, 6.3). */
    if (p->Attached) {
        p->ResendPending = TRUE;
        /* The restore's open: counted once however many resumes come
           before it has run (they coalesce). */
        if (!p->RestoreOpenOwed) {
            p->RestoreOpenOwed = TRUE;
            p->OpensLeft++;
        }
        if (p->Chip.Family != AC_FAM_CDC)
            p->OpenPending = TRUE;           /* no bulk transfer before the opens again */
        if (p->Chip.Family == AC_FAM_FTDI) {
            p->AwaitStatus = TRUE;           /* nor bulk OUT before a fresh CTS */
            p->StatusTicks = 0;
        }
        UcArmPoll(p);
    }
    /* Queued under the lock so a concurrent stop sees it counted. */
    InterlockedIncrement(&p->Outstanding);
    if (!KeInsertQueueDpc(&p->Dpc, NULL, NULL))
        InterlockedDecrement(&p->Outstanding);
    KeReleaseSpinLock(&p->Lock, irql);
}
