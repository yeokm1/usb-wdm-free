/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * slotm0.h - MTPPRB.SYS's request slots, USBMTP.SYS's sys/slot.h as it
 * stood before its fallback abort was removed (slotm0.c says why). The original text:
 * USBMTP.SYS's request slots (docs/design/mtp.md 5.4): the pure
 * transition code, with no kernel calls. The driver calls each Slot*
 * function with the device's spin lock held and then, with the lock
 * released, performs the actions it returns. slotmodel.c drives the same
 * functions through every interleaving of their events.
 *
 * Ownership, from admission to the last device access:
 * - A slot owns its request from SlotAdmit until Finish, which runs only
 *   when the outcome is fixed and nothing of the request is out: no lower
 *   IRP with the stack, no timer or DPC, no cancel routine set or running,
 *   no pin (an outside call: IoCallDriver and its re-check, IoCancelIrp),
 *   no abort IRP and no abort worker.
 * - Finish hands the caller's IRP (its pointer, outcome and byte count, by
 *   value in SA_COMPLETE) to a completion tail and frees the slot at once.
 *   The tail calls IoCompleteRequest on that IRP, touching no slot, then
 *   SlotTailDone under the lock: its last device access. Removal waits for
 *   every tail (Tails), so the device outlives them.
 * - A cancel routine's or timer DPC's last device access is SlotCancelEnd /
 *   SlotTimerEnd; after it the routine only returns. A lower completion
 *   routine's last is SlotLowerDone / SlotPollDone; it then returns
 *   STATUS_MORE_PROCESSING_REQUIRED, after which the I/O manager no longer
 *   touches the lower IRP.
 * - IRP reuse on 98 SE and ME (Microsoft, "Reusing IRPs"): a slot's lower
 *   IRP (and its abort IRP) is allocated with IoAllocateIrp(n, FALSE) and
 *   rebuilt with IoInitializeIrp, its stack location, URB and completion
 *   routine set again, only for an SA_SUBMIT (lower IRP) or SA_QUEUE_ABORT
 *   (abort IRP) action. slot.c returns those only when the IRP concerned
 *   has come back from the stack (Lower or AbortIrp clear). The driver
 *   asserts it.
 * - The abort IRP's completion routine calls SlotAbortIrpDone and then
 *   signals the worker's event, its last access to the slot and the IRP;
 *   after that it only returns. The worker waits for that event before
 *   SlotAbortWorkerDone, and only SlotAbortWorkerDone releases the abort
 *   IRP for a rebuild (a coalesced repeat included). So the abort IRP is
 *   never rebuilt or resubmitted before its completion routine's last
 *   access, nor before the worker has returned.
 * - No recursion: a completion that finds a next segment or a poll re-arm
 *   to send while the submitter is still inside its IoCallDriver records
 *   it, and the submitter sends it after its call returns (SlotSubmitReturned
 *   returns SA_SUBMIT again). So at most one IoCallDriver per slot is ever
 *   on a stack, and pins stay within SLOT_MAX_PINS.
 * - Completion tails are bounded: while the device has TailLimit tails out,
 *   admission is refused SO_BUSY.
 * - Timers are one-shot (KeSetTimer, no period).
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#ifndef USBMTP_SLOT_H
#define USBMTP_SLOT_H

/* Slots: one request each (5.4). */
#define SLOT_BIN   0    /* ReadFile: bulk IN */
#define SLOT_BOUT  1    /* WriteFile, WRITE_ZLP: bulk OUT */
#define SLOT_CTL   2    /* CLASS_REQUEST, RESET_PIPE, GET_STRING: EP0 */
#define SLOT_INT   3    /* the driver's interrupt poll (no caller IRP) */
#define SLOT_WAIT  4    /* WAIT_EVENT (no lower request) */
#define SLOT_COUNT 5

#define SLOT_MAX_SEGS 16    /* 1 MiB in 64 KiB segments; more is refused */
#define SLOT_MAX_PINS 8     /* enforced: a cancel past it is coalesced (5.4) */
#define SLOT_MAX_TAILS 32   /* the default TailLimit */

/* Slot states. */
#define SS_IDLE      0  /* free for a request */
#define SS_ACTIVE    1  /* a request admitted, its outcome open */
#define SS_STOPPING  2  /* cancel, time-out, release or removal decided; no next segment */
#define SS_DONE      3  /* outcome fixed (status, bytes); waiting for every activity to end */
#define SS_RESERVED  4  /* held by a RESET_PIPE on the control slot */

/* Outcome codes (the driver maps them to NTSTATUS). */
#define SO_NONE      0
#define SO_SUCCESS   1
#define SO_ERROR     2  /* the URB failed */
#define SO_CANCELLED 3
#define SO_TIMEOUT   4
#define SO_REMOVED   5
#define SO_BUSY      6  /* refused: the slot (or a reset's target) is in use */
#define SO_DENIED    7  /* refused: no claim, a stale epoch, or the device is going */
#define SO_INVALID   8  /* refused: more than SLOT_MAX_SEGS segments */

/* Claim states (5.1). */
#define SC_FREE      0
#define SC_ACTIVE    1
#define SC_RELEASING 2

/* Actions the caller performs with the lock released. */
#define SA_SUBMIT        1  /* rebuild and IoCallDriver the slot's lower IRP; a pin is held */
#define SA_CANCEL_LOWER  2  /* IoCancelIrp of the slot's lower IRP; a pin is held */
#define SA_QUEUE_ABORT   3  /* queue the abort worker for the slot's abort IRP */
#define SA_COMPLETE      4  /* the completion tail: IoCompleteRequest(Irp), then SlotTailDone */
#define SA_REMOVE_READY  5  /* removal's rundown is done: the slots may be freed, the device detached */
#define SA_FREE_READY    6  /* the rundown is done and no handle is open (FreeReady set): noted only;
                               the deletion is claimed under the lock by REMOVE after its detach or
                               by the close that ended the count, whichever sees both first (mtp.md 5.4) */

typedef struct SLOT_ACTION {
    int Kind;
    int Slot;
    unsigned long Request;  /* the request the action is for */
    void *Irp;              /* SA_COMPLETE: the caller's IRP, owned by the tail */
    int Outcome;            /* SA_COMPLETE */
    unsigned long Bytes;    /* SA_COMPLETE */
} SLOT_ACTION;

/* A Slot* call emits at most three actions per slot, one SA_REMOVE_READY
   and one SA_FREE_READY, so 3 * SLOT_COUNT + 2 always fit (asserted in
   slot.c). A caller that gathers several calls into one list (admission's
   two parts, a STOP of every slot) stays within that bound too. */
#define SLOT_MAX_ACTIONS 17

typedef struct SLOT_ACTIONS {
    int Count;
    SLOT_ACTION A[SLOT_MAX_ACTIONS];
} SLOT_ACTIONS;

/*
 * Kernel primitives the transitions use under the lock. None of them calls
 * down or completes an IRP. In the driver: KeSetTimer (one-shot),
 * KeCancelTimer, IoSetCancelRoutine and Irp->Cancel; in the model: model
 * state.
 */
typedef struct SLOT_OPS {
    void *Ctx;
    void (*ArmTimer)(void *Ctx, int Slot);
    /* KeCancelTimer: nonzero if the timer was still queued (now retired). */
    int (*CancelTimer)(void *Ctx, int Slot);
    /* IoSetCancelRoutine(Irp, routine). */
    void (*SetCancel)(void *Ctx, int Slot);
    /* Irp->Cancel. */
    int (*IrpCancelled)(void *Ctx, int Slot);
    /* IoSetCancelRoutine(Irp, NULL) != NULL: nonzero if the routine was
       taken back before the I/O manager took it (it will never run). */
    int (*ClearCancel)(void *Ctx, int Slot);
} SLOT_OPS;

/* Small fields, so the model can store many states; all set by SlotInit. */
typedef struct SLOT {
    void *Irp;              /* the caller's IRP while held */
    unsigned long Request;  /* admission number of the request held (never 0) */
    unsigned long Bytes;
    unsigned char State;
    unsigned char HasIrp;   /* a caller IRP is held (not for the poll) */
    unsigned char Lower;    /* the lower IRP is with the stack */
    unsigned char Timer;    /* armed, or its DPC not yet finished */
    unsigned char Cancel;   /* the cancel routine set, or running */
    unsigned char Pins;     /* outside calls for this request in progress */
    unsigned char SegsLeft; /* segments still to send after the current */
    unsigned char Outcome;
    unsigned char Reason;   /* the stop reason, while STOPPING */
    unsigned char Fallback; /* cancel by the abort worker, not IoCancelIrp */
    signed char Reserve;    /* control slot: the slot a RESET_PIPE reserved, or -1 */
    unsigned char AbortIrp;     /* the abort IRP is out (queued or with the stack) */
    unsigned char AbortWorker;  /* the abort worker has not returned */
    unsigned char AbortAgain;   /* an abort was asked for while one was out */
    unsigned char AbortBack;    /* the abort IRP's completion routine has run */
    unsigned char Submitting;   /* a submitter is inside its IoCallDriver */
    unsigned char Resubmit;     /* a next segment or re-arm waits for that submitter */
    unsigned char CancelAgain;  /* a cancel was asked for at the pin limit */
    unsigned char Pad[2];
} SLOT;

typedef struct SLOT_DEVICE {
    SLOT S[SLOT_COUNT];
    unsigned long NextRequest;
    unsigned long Epoch;        /* the current claim's, never 0, never reused */
    unsigned char Claim;
    unsigned char Removing;
    unsigned char RemoveReady;  /* SA_REMOVE_READY has been returned */
    unsigned char FreeReady;    /* SA_FREE_READY has been returned */
    unsigned short Tails;       /* completion tails not yet done */
    unsigned short TailLimit;   /* admission refused SO_BUSY at this many */
    unsigned short Opens;       /* handles open (SlotOpen to SlotClose) */
    unsigned char Detached;     /* REMOVE has detached the device (SlotDetach) */
    unsigned char Deleting;     /* the one deleter has claimed IoDeleteDevice */
    unsigned short Entered;     /* IRPs without a handle inside a dispatch routine (SlotEnter) */
    unsigned char Gone;         /* the deleter has unpublished the device (SlotGone) */
    unsigned char Pad;
} SLOT_DEVICE;

void SlotInit(SLOT_DEVICE *D);

/* Actions dropped because a list was full: never, by the sizing above;
   counted so the driver can log it and the model can fail on it. */
extern unsigned long SlotDropped;

/*
 * Admission, in two calls under one hold of the lock: SlotAdmit stores the
 * request and sets its cancel routine; SlotAdmitCheck then reads the IRP's
 * Cancel flag (the I/O manager may cancel in between) and returns either
 * SA_SUBMIT or the completion. SlotAdmit returns the admission number, or 0
 * for a refusal, which it returns as SA_COMPLETE with SO_BUSY, SO_DENIED or
 * SO_INVALID (the caller completes the IRP itself; SlotAdmitCheck and
 * SlotTailDone are not called). Segs: 1 to SLOT_MAX_SEGS. Timeout:
 * nonzero to arm the timer. Reserve: the slot a RESET_PIPE reserves (with
 * Slot == SLOT_CTL only), or -1. Epoch: the handle's claim epoch, checked
 * when NeedClaim.
 */
unsigned long SlotAdmit(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, void *Irp, int Segs,
                        int Timeout, int Reserve, int NeedClaim, unsigned long Epoch,
                        SLOT_ACTIONS *Out);
void SlotAdmitCheck(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);

/* The submitter's IoCallDriver has returned (inline completion included). */
void SlotSubmitReturned(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);

/* A lower IRP's completion routine (not the poll's). Ok: the URB
   succeeded. Short: it ended on a short packet. */
void SlotLowerDone(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, int Ok, int Short,
                   unsigned long Bytes, SLOT_ACTIONS *Out);

/* An SA_CANCEL_LOWER call has returned. */
void SlotPinReturned(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);

/* The abort IRP's completion routine; the abort worker's return. */
void SlotAbortIrpDone(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);
void SlotAbortWorkerDone(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);

/* The completion tail's IoCompleteRequest has returned. */
void SlotTailDone(SLOT_DEVICE *D, SLOT_ACTIONS *Out);

/* The cancel routine (the I/O manager took it). Begin, the outside call, End. */
void SlotCancelBegin(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);
void SlotCancelEnd(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);

/* The timer DPC. Begin, the outside call, End. */
void SlotTimerBegin(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);
void SlotTimerEnd(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);

/* A stop of one slot by the driver itself (release, removal, STOP,
   standby, a bound handle's cleanup of its own wait). */
void SlotStop(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, int Outcome, SLOT_ACTIONS *Out);

/* ABORT or EVENTS stop of one slot by a handle: only under the claim the
   handle holds or is bound to (Epoch), checked in the same hold of the
   lock as the stop; returns 0, with nothing changed, if that claim is no
   longer the active one. */
int SlotStopEpoch(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, int Outcome, unsigned long Epoch,
                  SLOT_ACTIONS *Out);

/* The interrupt poll: start it (at claim, or EVENTS start) under the claim
   Epoch, checked in the same hold of the lock; returns 0 if that claim is
   not the active one (a poll already running counts as started). The
   driver's started gate is checked by the caller in the same hold. Its
   lower completion delivers an event to the wait slot and re-arms. */
int SlotPollStart(SLOT_DEVICE *D, const SLOT_OPS *Ops, unsigned long Epoch, SLOT_ACTIONS *Out);
void SlotPollDone(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Ok, SLOT_ACTIONS *Out);

/* Claims (5.1): SlotClaim returns the new epoch, or 0 if refused. */
unsigned long SlotClaim(SLOT_DEVICE *D);
void SlotRelease(SLOT_DEVICE *D, const SLOT_OPS *Ops, SLOT_ACTIONS *Out);

/* IRP_MN_REMOVE_DEVICE: stop everything; SA_REMOVE_READY once all idle and
   every completion tail done; SA_FREE_READY once, besides, no handle is
   open (the device object outlives its handles: mtp.md 5.4). FreeReady is
   set in the same hold of the lock as the action is emitted, so the
   deleter decides by the field, under the lock, never by the action's
   arrival; exactly one party claims the deletion (mtp.md 5.4). */
void SlotRemove(SLOT_DEVICE *D, const SLOT_OPS *Ops, SLOT_ACTIONS *Out);

/* IRP_MJ_CREATE: count a handle; 0 (refused) once removal has begun.
   IRP_MJ_CLOSE: the handle is gone; may return SA_FREE_READY. Every close
   calls it, whatever it saw of the removal before: it returns 1 when this
   close claims the deletion (detached, FreeReady, no one claimed it yet).
   SlotDetach: REMOVE has detached; returns 1 when REMOVE claims the
   deletion. Exactly one of all these calls returns 1 (mtp.md 5.4). */
int SlotOpen(SLOT_DEVICE *D);
int SlotClose(SLOT_DEVICE *D, SLOT_ACTIONS *Out);
int SlotDetach(SLOT_DEVICE *D);

/*
 * IRPs that carry no handle (PnP, power, create, every other major): an
 * open handle does not keep the device alive for them, so each pins it for
 * its dispatch. These three are called under the driver-global lock, not
 * the device's, and the driver checks first, under that lock, that the
 * device object is not the one already deleted.
 * - SlotEnter: at dispatch entry; 0 if the deleter has unpublished the
 *   device (the IRP is then completed from the driver's globals, touching
 *   nothing of the device), else the pin is taken.
 * - SlotLeave: the dispatch's last access to the device.
 * - SlotGone: the deleter, after its claim and before IoDeleteDevice;
 *   it then waits, under that lock, until Entered is 0.
 */
int SlotEnter(SLOT_DEVICE *D);
void SlotLeave(SLOT_DEVICE *D);
void SlotGone(SLOT_DEVICE *D);
/* The deleter's wait: nonzero while a pin is held. */
int SlotPinned(const SLOT_DEVICE *D);

/* Nonzero if every slot is idle with nothing outstanding. */
int SlotAllIdle(const SLOT_DEVICE *D);

#endif
