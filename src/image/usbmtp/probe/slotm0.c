/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * slotm0.c - MTPPRB.SYS's request slots: USBMTP.SYS's sys/slot.c as it
 * stood before its fallback abort was removed (the probe's last build),
 * with the fallback abort the probe's ABORT_PIPE readings use (M0,
 * docs/testing/2026-11-16-mtp-m0.md). Kept for the probe alone, so those readings can be repeated; the driver's own slot.c has
 * no fallback abort since M0 found IoCancelIrp honoured (mtp.md 5.4).
 *
 * Pure transition code: no kernel calls, no allocation. Every function is
 * called with the device's spin lock held; the kernel primitives it needs
 * under the lock come through SLOT_OPS, and everything that calls down or
 * completes an IRP comes back as an action for the caller to perform after
 * releasing the lock. The ownership rules are in slot.h. Built into the
 * driver and into slotmodel.c.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include "slotm0.h"

/* The tail count's limit stays below its 16-bit range, with room for the
   tails of requests already admitted when the limit is reached. */
typedef char SlotTailLimitFits[(SLOT_MAX_TAILS + SLOT_COUNT < 65535) ? 1 : -1];

/* Every action one call can emit fits in a list (slot.h). */
typedef char SlotActionsFit[(SLOT_MAX_ACTIONS >= 3 * SLOT_COUNT + 2) ? 1 : -1];

unsigned long SlotDropped;

static void Emit(SLOT_ACTIONS *Out, int Kind, int Slot, unsigned long Request, void *Irp,
                 int Outcome, unsigned long Bytes)
{
    SLOT_ACTION *A;

    if (Out->Count >= SLOT_MAX_ACTIONS) {
        SlotDropped++;  /* cannot happen (slot.h); counted, never silent */
        return;
    }
    A = &Out->A[Out->Count++];
    A->Kind = Kind;
    A->Slot = Slot;
    A->Request = Request;
    A->Irp = Irp;
    A->Outcome = Outcome;
    A->Bytes = Bytes;
}

static void Clear(SLOT *S)
{
    S->Irp = 0;
    S->Request = 0;
    S->Bytes = 0;
    S->State = SS_IDLE;
    S->HasIrp = 0;
    S->Lower = 0;
    S->Timer = 0;
    S->Cancel = 0;
    S->Pins = 0;
    S->SegsLeft = 0;
    S->Outcome = SO_NONE;
    S->Reason = SO_NONE;
    S->Reserve = -1;
    S->AbortIrp = 0;
    S->AbortWorker = 0;
    S->AbortAgain = 0;
    S->AbortBack = 0;
    S->Submitting = 0;
    S->Resubmit = 0;
    S->CancelAgain = 0;
    S->Pad[0] = S->Pad[1] = 0;
}

void SlotInit(SLOT_DEVICE *D)
{
    int i;

    for (i = 0; i < SLOT_COUNT; i++) {
        Clear(&D->S[i]);
        D->S[i].Fallback = 0;
    }
    D->NextRequest = 0;
    D->Epoch = 0;
    D->Claim = SC_FREE;
    D->Removing = 0;
    D->RemoveReady = 0;
    D->FreeReady = 0;
    D->Tails = 0;
    D->TailLimit = SLOT_MAX_TAILS;
    D->Opens = 0;
    D->Detached = 0;
    D->Deleting = 0;
    D->Entered = 0;
    D->Gone = 0;
    D->Pad = 0;
}

static int Quiet(const SLOT *S)
{
    return !S->Lower && !S->Timer && !S->Cancel && !S->Pins && !S->AbortIrp && !S->AbortWorker;
}

int SlotAllIdle(const SLOT_DEVICE *D)
{
    int i;

    for (i = 0; i < SLOT_COUNT; i++) {
        if (D->S[i].State != SS_IDLE || !Quiet(&D->S[i])) {
            return 0;
        }
    }
    return 1;
}

/* The device-wide consequences of a slot becoming idle or a tail ending. */
static void DeviceCheck(SLOT_DEVICE *D, SLOT_ACTIONS *Out)
{
    if (!SlotAllIdle(D)) {
        return;
    }
    if (D->Claim == SC_RELEASING) {
        D->Claim = SC_FREE;
    }
    if (D->Removing && !D->RemoveReady && !D->Tails) {
        D->RemoveReady = 1;
        Emit(Out, SA_REMOVE_READY, -1, 0, 0, SO_NONE, 0);
    }
    if (D->RemoveReady && !D->FreeReady && !D->Opens) {
        D->FreeReady = 1;
        Emit(Out, SA_FREE_READY, -1, 0, 0, SO_NONE, 0);
    }
}

/* The one completion point: hand the IRP to a tail, free the slot. */
static void Finish(SLOT_DEVICE *D, int Slot, SLOT_ACTIONS *Out)
{
    SLOT *S = &D->S[Slot];

    if (S->State != SS_DONE || !Quiet(S)) {
        return;
    }
    if (S->HasIrp) {
        Emit(Out, SA_COMPLETE, Slot, S->Request, S->Irp, S->Outcome, S->Bytes);
        D->Tails++;
    }
    if (S->Reserve >= 0 && D->S[S->Reserve].State == SS_RESERVED) {
        D->S[S->Reserve].State = SS_IDLE;
    }
    Clear(S);
    DeviceCheck(D, Out);
}

/* Retire the timer and the cancel routine, at the final outcome only. */
static void Retire(const SLOT_OPS *Ops, int Slot, SLOT *S)
{
    if (S->Timer && Ops->CancelTimer(Ops->Ctx, Slot)) {
        S->Timer = 0;
    }
    if (S->Cancel && Ops->ClearCancel(Ops->Ctx, Slot)) {
        S->Cancel = 0;
    }
}

/* Fix the outcome, once, and retire. */
static void Decide(const SLOT_OPS *Ops, int Slot, SLOT *S, int Outcome)
{
    if (S->State == SS_DONE) {
        return;
    }
    S->Outcome = (unsigned char)Outcome;
    S->State = SS_DONE;
    Retire(Ops, Slot, S);
}

static void QueueAbort(int Slot, SLOT *S, SLOT_ACTIONS *Out)
{
    S->AbortIrp = 1;
    S->AbortWorker = 1;
    Emit(Out, SA_QUEUE_ABORT, Slot, S->Request, 0, SO_NONE, 0);
}

/* Ask for the lower IRP to be cancelled. IoCancelIrp: under a pin. The
   abort: one abort IRP per slot; a second request while it is out is
   coalesced and repeated once both the IRP and the worker are back. */
static void CancelLower(int Slot, SLOT *S, SLOT_ACTIONS *Out)
{
    if (S->Fallback) {
        if (S->AbortIrp || S->AbortWorker) {
            S->AbortAgain = 1;
        } else {
            QueueAbort(Slot, S, Out);
        }
        return;
    }
    if (S->Pins >= SLOT_MAX_PINS) {
        S->CancelAgain = 1;     /* repeated when a pin comes back */
        return;
    }
    S->Pins++;
    Emit(Out, SA_CANCEL_LOWER, Slot, S->Request, 0, SO_NONE, 0);
}

/* Send the lower IRP: now, or by the submitter already inside its call. */
static void SendNext(int Slot, SLOT *S, SLOT_ACTIONS *Out)
{
    S->Lower = 1;
    if (S->Submitting) {
        S->Resubmit = 1;
        return;
    }
    S->Submitting = 1;
    S->Pins++;
    Emit(Out, SA_SUBMIT, Slot, S->Request, 0, SO_NONE, 0);
}

/* Stop a request (cancel, time-out, release, removal, ABORT, EVENTS). */
static void StopRequest(const SLOT_OPS *Ops, int Slot, SLOT *S, int Outcome, SLOT_ACTIONS *Out)
{
    if (S->State != SS_ACTIVE) {
        return;
    }
    if (!S->Lower) {
        Decide(Ops, Slot, S, Outcome);      /* the wait slot */
        return;
    }
    S->State = SS_STOPPING;
    S->Reason = (unsigned char)Outcome;
    CancelLower(Slot, S, Out);
}

unsigned long SlotAdmit(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, void *Irp, int Segs,
                        int Timeout, int Reserve, int NeedClaim, unsigned long Epoch,
                        SLOT_ACTIONS *Out)
{
    SLOT *S = &D->S[Slot];

    if (Segs < 1 || Segs > SLOT_MAX_SEGS) {
        Emit(Out, SA_COMPLETE, Slot, 0, Irp, SO_INVALID, 0);
        return 0;
    }
    if (D->Removing || (NeedClaim && (D->Claim != SC_ACTIVE || Epoch != D->Epoch))) {
        Emit(Out, SA_COMPLETE, Slot, 0, Irp, SO_DENIED, 0);
        return 0;
    }
    if (D->Tails >= D->TailLimit || S->State != SS_IDLE || !Quiet(S) ||
        (Reserve >= 0 && (D->S[Reserve].State != SS_IDLE || !Quiet(&D->S[Reserve])))) {
        Emit(Out, SA_COMPLETE, Slot, 0, Irp, SO_BUSY, 0);
        return 0;
    }
    if (Reserve >= 0) {
        D->S[Reserve].State = SS_RESERVED;
    }
    D->NextRequest++;
    if (!D->NextRequest) {
        D->NextRequest = 1;     /* 0 is the refusal: never an admission number */
    }
    S->Request = D->NextRequest;
    S->Irp = Irp;
    S->State = SS_ACTIVE;
    S->HasIrp = (unsigned char)(Slot != SLOT_INT);
    S->Pins = 1;            /* the admitter's, held until its re-check */
    S->SegsLeft = (unsigned char)(Segs - 1);
    S->Reserve = (signed char)Reserve;
    if (Timeout) {
        Ops->ArmTimer(Ops->Ctx, Slot);
        S->Timer = 1;
    }
    if (S->HasIrp) {
        Ops->SetCancel(Ops->Ctx, Slot);
        S->Cancel = 1;
    }
    return S->Request;
}

void SlotAdmitCheck(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out)
{
    SLOT *S = &D->S[Slot];

    if (S->HasIrp && Ops->IrpCancelled(Ops->Ctx, Slot)) {
        /* Cancelled before or while it was admitted. If the routine was
           taken back, it will never run; if not, it is running and clears
           Cancel at its end, and the IRP waits for it (a handshake). */
        S->Pins--;
        Decide(Ops, Slot, S, SO_CANCELLED);
        Finish(D, Slot, Out);
        return;
    }
    if (Slot == SLOT_WAIT) {
        S->Pins--;
        return;
    }
    S->Lower = 1;           /* the admitter's pin covers IoCallDriver and its re-check */
    S->Submitting = 1;
    Emit(Out, SA_SUBMIT, Slot, S->Request, 0, SO_NONE, 0);
}

void SlotSubmitReturned(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out)
{
    SLOT *S = &D->S[Slot];

    if (S->Resubmit) {
        /* A completion found the next segment (or re-arm) while this
           submitter was inside its call: send it from here, keeping the
           pin, unless a stop came meanwhile, in which case it never goes. */
        S->Resubmit = 0;
        if (S->State == SS_ACTIVE) {
            Emit(Out, SA_SUBMIT, Slot, S->Request, 0, SO_NONE, 0);
            return;
        }
        S->Lower = 0;
        S->Submitting = 0;
        S->Pins--;
        if (S->State == SS_STOPPING) {
            Decide(Ops, Slot, S, S->Reason);
        }
        Finish(D, Slot, Out);
        return;
    }
    S->Submitting = 0;
    S->Pins--;
    /* A stop decided while the IRP was being passed down may have cancelled
       it before the stack had it: cancel again now that it has. */
    if (S->State == SS_STOPPING && S->Lower) {
        CancelLower(Slot, S, Out);
    }
    Finish(D, Slot, Out);
}

void SlotLowerDone(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, int Ok, int Short,
                   unsigned long Bytes, SLOT_ACTIONS *Out)
{
    SLOT *S = &D->S[Slot];

    S->Lower = 0;
    S->Bytes += Bytes;
    if (S->State == SS_ACTIVE && Ok && !Short && S->SegsLeft > 0) {
        /* The next segment: decided here, under the lock, and sent by this
           completion path under a pin, which also covers its re-check. */
        S->SegsLeft--;
        SendNext(Slot, S, Out);
        return;
    }
    /* Precedence: a stop decided first wins over the URB's own result. */
    if (S->State == SS_STOPPING) {
        Decide(Ops, Slot, S, S->Reason);
    } else if (S->State == SS_ACTIVE) {
        Decide(Ops, Slot, S, Ok ? SO_SUCCESS : SO_ERROR);
    }
    Finish(D, Slot, Out);
}

void SlotPinReturned(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out)
{
    SLOT *S = &D->S[Slot];

    (void)Ops;
    S->Pins--;
    if (S->CancelAgain && S->Lower && S->State == SS_STOPPING) {
        S->CancelAgain = 0;
        CancelLower(Slot, S, Out);
    }
    Finish(D, Slot, Out);
}

/* Once both the abort IRP and its worker are back: repeat a coalesced
   abort if the lower IRP is still out, else let the slot finish. */
static void AbortSettle(SLOT_DEVICE *D, int Slot, SLOT_ACTIONS *Out)
{
    SLOT *S = &D->S[Slot];

    if (!S->AbortIrp && !S->AbortWorker && S->AbortAgain) {
        S->AbortAgain = 0;
        if (S->Lower && S->State == SS_STOPPING) {
            QueueAbort(Slot, S, Out);
        }
    }
    Finish(D, Slot, Out);
}

void SlotAbortIrpDone(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out)
{
    (void)Ops;
    (void)Out;
    /* The routine's last steps follow: this, then the worker's event. The
       abort IRP stays out until the worker, having seen the event, returns. */
    D->S[Slot].AbortBack = 1;
}

void SlotAbortWorkerDone(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out)
{
    SLOT *S = &D->S[Slot];

    (void)Ops;
    S->AbortWorker = 0;
    S->AbortIrp = 0;    /* the worker saw the routine's event: the IRP is free */
    S->AbortBack = 0;
    AbortSettle(D, Slot, Out);
}

void SlotTailDone(SLOT_DEVICE *D, SLOT_ACTIONS *Out)
{
    if (D->Tails) {
        D->Tails--;
    }
    DeviceCheck(D, Out);
}

void SlotCancelBegin(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out)
{
    StopRequest(Ops, Slot, &D->S[Slot], SO_CANCELLED, Out);
}

void SlotCancelEnd(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out)
{
    (void)Ops;
    D->S[Slot].Cancel = 0;
    Finish(D, Slot, Out);
}

void SlotTimerBegin(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out)
{
    StopRequest(Ops, Slot, &D->S[Slot], SO_TIMEOUT, Out);
}

void SlotTimerEnd(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out)
{
    (void)Ops;
    D->S[Slot].Timer = 0;
    Finish(D, Slot, Out);
}

void SlotStop(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, int Outcome, SLOT_ACTIONS *Out)
{
    StopRequest(Ops, Slot, &D->S[Slot], Outcome, Out);
    Finish(D, Slot, Out);
}

/* The claim Epoch names is the active one. */
static int Current(const SLOT_DEVICE *D, unsigned long Epoch)
{
    return Epoch && D->Claim == SC_ACTIVE && Epoch == D->Epoch && !D->Removing;
}

int SlotStopEpoch(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, int Outcome, unsigned long Epoch,
                  SLOT_ACTIONS *Out)
{
    if (!Current(D, Epoch)) {
        return 0;   /* an old claim's handle: it reaches nothing of a later one */
    }
    SlotStop(D, Ops, Slot, Outcome, Out);
    return 1;
}

/* An event reaches the wait slot (from the poll's completion). */
static void Deliver(SLOT_DEVICE *D, const SLOT_OPS *Ops, SLOT_ACTIONS *Out)
{
    SLOT *S = &D->S[SLOT_WAIT];

    if (S->State != SS_ACTIVE) {
        return;     /* the driver queues it for a later WAIT_EVENT */
    }
    Decide(Ops, SLOT_WAIT, S, SO_SUCCESS);
    Finish(D, SLOT_WAIT, Out);
}

int SlotPollStart(SLOT_DEVICE *D, const SLOT_OPS *Ops, unsigned long Epoch, SLOT_ACTIONS *Out)
{
    SLOT *S = &D->S[SLOT_INT];

    (void)Ops;
    if (!Current(D, Epoch)) {
        return 0;
    }
    if (S->State != SS_IDLE || !Quiet(S)) {
        return 1;   /* running, or still ending: EVENTS start again once it is idle */
    }
    D->NextRequest++;
    if (!D->NextRequest) {
        D->NextRequest = 1;
    }
    S->Request = D->NextRequest;
    S->State = SS_ACTIVE;
    S->HasIrp = 0;
    S->Lower = 1;
    S->Pins = 1;
    S->Submitting = 1;
    S->Reserve = -1;
    Emit(Out, SA_SUBMIT, SLOT_INT, S->Request, 0, SO_NONE, 0);
    return 1;
}

void SlotPollDone(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Ok, SLOT_ACTIONS *Out)
{
    SLOT *S = &D->S[SLOT_INT];

    S->Lower = 0;
    if (Ok) {
        Deliver(D, Ops, Out);
    }
    if (S->State == SS_ACTIVE && Ok && D->Claim == SC_ACTIVE && !D->Removing) {
        SendNext(SLOT_INT, S, Out);     /* re-armed like a next segment */
        return;
    }
    if (S->State == SS_STOPPING) {
        Decide(Ops, SLOT_INT, S, S->Reason);
    } else if (S->State == SS_ACTIVE) {
        Decide(Ops, SLOT_INT, S, Ok ? SO_SUCCESS : SO_ERROR);
    }
    Finish(D, SLOT_INT, Out);
}

unsigned long SlotClaim(SLOT_DEVICE *D)
{
    if (D->Claim != SC_FREE || D->Removing || D->Epoch == 0xFFFFFFFFUL) {
        return 0;   /* an epoch is never reused: no claim past the last */
    }
    D->Epoch++;
    D->Claim = SC_ACTIVE;
    return D->Epoch;
}

void SlotRelease(SLOT_DEVICE *D, const SLOT_OPS *Ops, SLOT_ACTIONS *Out)
{
    int i;

    if (D->Claim != SC_ACTIVE) {
        return;     /* idempotent: cleanup after close, close after cleanup */
    }
    D->Claim = SC_RELEASING;
    for (i = 0; i < SLOT_COUNT; i++) {
        SlotStop(D, Ops, i, SO_CANCELLED, Out);
    }
    DeviceCheck(D, Out);
}

int SlotOpen(SLOT_DEVICE *D)
{
    if (D->Removing || D->Opens == 0xFFFF) {
        return 0;
    }
    D->Opens++;
    return 1;
}

/* The deletion's one claim: detached, the rundown done, no handle open. */
static int ClaimDelete(SLOT_DEVICE *D)
{
    if (!D->Detached || !D->FreeReady || D->Deleting) {
        return 0;
    }
    D->Deleting = 1;
    return 1;
}

int SlotClose(SLOT_DEVICE *D, SLOT_ACTIONS *Out)
{
    if (D->Opens) {
        D->Opens--;
    }
    DeviceCheck(D, Out);
    return ClaimDelete(D);
}

int SlotDetach(SLOT_DEVICE *D)
{
    D->Detached = 1;
    return ClaimDelete(D);
}

int SlotEnter(SLOT_DEVICE *D)
{
    if (D->Gone || D->Entered == 0xFFFF) {
        return 0;
    }
    D->Entered++;
    return 1;
}

void SlotLeave(SLOT_DEVICE *D)
{
    if (D->Entered) {
        D->Entered--;
    }
}

void SlotGone(SLOT_DEVICE *D)
{
    D->Gone = 1;
}

int SlotPinned(const SLOT_DEVICE *D)
{
    return D->Entered != 0;
}

void SlotRemove(SLOT_DEVICE *D, const SLOT_OPS *Ops, SLOT_ACTIONS *Out)
{
    int i;

    if (D->Removing) {
        return;
    }
    D->Removing = 1;
    if (D->Claim == SC_ACTIVE) {
        D->Claim = SC_RELEASING;
    }
    for (i = 0; i < SLOT_COUNT; i++) {
        SlotStop(D, Ops, i, SO_REMOVED, Out);
    }
    DeviceCheck(D, Out);
}
