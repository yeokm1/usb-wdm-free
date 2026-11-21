/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * slot.h - USBMTP.SYS's request slots (docs/design/mtp.md 5.4): the pure
 * transition code, with no kernel calls. The driver calls each Slot*
 * function with the device's spin lock held and then, with the lock
 * released, performs the actions it returns. slotmodel.c drives the same
 * functions through every interleaving of their events.
 *
 * Ownership, from admission to the last device access:
 * - A slot owns its request from SlotAdmit until Finish, which runs only
 *   when the outcome is fixed and nothing of the request is out: no lower
 *   IRP with the stack, no timer or DPC, no cancel routine set or running,
 *   no pin (an outside call: IoCallDriver and its re-check, IoCancelIrp).
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
 *   IRP is allocated with IoAllocateIrp(n, FALSE) and rebuilt with
 *   IoInitializeIrp, its stack location, URB and completion routine set
 *   again, only for an SA_SUBMIT action that SlotSubmitCommit committed.
 *   slot.c returns one only when the IRP has come back from the stack
 *   (Lower clear).
 * - A stop cancels the lower IRP with IoCancelIrp only, so there is no
 *   abort worker. M0 read IoCancelIrp retiring a pending bulk IN, bulk OUT,
 *   interrupt IN and class request on both of 98 SE's stacks and on ME
 *   (docs/testing/2026-11-16-mtp-m0.md); a pending pipe reset's cancel was
 *   not run, and a reset no stack retires holds its stop's wait (mtp.md
 *   5.4, "Waits and bounds").
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
#define SO_ASLEEP    9  /* refused: the power gate is closed (SlotPowerDown) */

/* Claim states (5.1). */
#define SC_FREE      0
#define SC_ACTIVE    1
#define SC_RELEASING 2

/* Actions the caller performs with the lock released. */
#define SA_SUBMIT        1  /* SlotSubmitCommit under the lock; if committed, rebuild the slot's lower
                               IRP, SlotSubmitBuilt under the lock; if handed off, IoCallDriver, then
                               SlotSubmitReturned; a pin is held */
#define SA_CANCEL_LOWER  2  /* IoCancelIrp of the slot's lower IRP; a pin is held */
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
    signed char Reserve;    /* control slot: the slot a RESET_PIPE reserved, or -1 */
    unsigned char Submitting;   /* a submitter is inside its IoCallDriver */
    unsigned char Resubmit;     /* a next segment or re-arm waits for that submitter */
    unsigned char CancelAgain;  /* a cancel was asked for at the pin limit */
    unsigned char Calling;      /* the submitter committed its IoCallDriver (SlotSubmitCommit)
                                   and has not yet called SlotSubmitReturned */
    unsigned char Rebuilding;   /* from SlotSubmitCommit to SlotSubmitBuilt: the lower IRP is being
                                   reinitialised; no IoCancelIrp may be made on it (a stop defers) */
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
    unsigned char ActiveClosed; /* REMOVE has closed the admission (SlotActiveClose) */
    unsigned short Active;      /* admitted dispatches of IRPs without a handle (SlotActiveEnter) */
    unsigned short PowerQ;      /* admitted power IRPs queued for the power worker */
    unsigned char PowerWorker;  /* the power worker is queued or running */
    unsigned char PowerDown;    /* the power gate's reasons (SLOT_PD_*): closed while not 0 */
    unsigned char Query;        /* a system standby's query (SLOT_Q_*) */
    unsigned char DQuery;       /* another component's device query below D0 (SLOT_Q_*) */
    unsigned char Lost;         /* a poll's data came back while the gate was closed (SlotTakeLost) */
    unsigned char HeldAtVeto;   /* SlotInfoHeld at the last refused query */
    unsigned char Pad;
    unsigned short Vetoes;      /* refused queries, modulo 65536 (a reader compares with its own baseline) */
    unsigned short Forced;      /* sets passed down with requests out, modulo 65536 */
    unsigned long QueryAt;      /* the tick (ms) at which the system query was accepted */
    unsigned long DQueryAt;     /* the same for the device query */
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

/*
 * The submission protocol (mtp.md 5.4, "The submission"). An SA_SUBMIT is
 * an intention, not a call: between the hold that emitted it and the
 * submitter's IoCallDriver anything may happen (a stop, the power gate, a
 * set going down). So the submitter, with the lock held and before it
 * rebuilds the lower IRP, calls SlotSubmitCommit:
 * - 1 (committed): the slot's request is still ACTIVE; Calling and
 *   Rebuilding are set, and the submitter releases the lock and rebuilds
 *   the IRP (IoInitializeIrp, the URB, the stack location, the completion
 *   routine). While Rebuilding is set no IoCancelIrp is made on the IRP:
 *   a stop then marks the slot STOPPING and defers its cancel. With the
 *   lock held again the submitter calls SlotSubmitBuilt:
 *   - 1 (hand-off): still ACTIVE; Rebuilding ends, the submitter releases
 *     the lock, calls IoCallDriver, then SlotSubmitReturned, which clears
 *     Calling (and cancels again if a stop came meanwhile: the post-call
 *     handshake). A stop after this hold cancels as usual: the IRP is
 *     whole, and IoCancelIrp racing IoCallDriver is the I/O manager's
 *     ordinary case;
 *   - 0 (withdrawn): a stop came during the rebuild; the deferred cancel
 *     is honoured by never passing the IRP down; the pin is given back and
 *     the actions returned end the request (its outcome the stop's).
 * - 0 (withdrawn): a stop was decided in between; the IRP is never passed
 *   down, the submitter's pin is given back, and the actions returned end
 *   the request (its outcome the stop's).
 * Because every stop, the gate's included, is decided under the lock, and
 * the gate stops every slot in the hold that closes it, no commit can
 * succeed after that hold until the gate reopens: an SA_SUBMIT not yet
 * committed when the gate closes is never sent. One committed before is
 * sent; a set into sleep goes down only when SlotCalling is 0, so that
 * call has reached the stack (and is quarantined there if it does not come
 * back) before the sleep IRP does. That last wait is on the submitter's
 * own IoCallDriver returning (a lower dispatch), not on a request's
 * completion.
 */
int SlotSubmitCommit(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);
int SlotSubmitBuilt(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);

/* The submitter's IoCallDriver has returned (inline completion included). */
void SlotSubmitReturned(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);

/* Bit s set for each slot whose submitter committed and has not returned. */
int SlotCalling(const SLOT_DEVICE *D);

/* A lower IRP's completion routine (not the poll's). Ok: the URB
   succeeded. Short: it ended on a short packet. */
void SlotLowerDone(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, int Ok, int Short,
                   unsigned long Bytes, SLOT_ACTIONS *Out);

/* An SA_CANCEL_LOWER call has returned. */
void SlotPinReturned(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Slot, SLOT_ACTIONS *Out);

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

/* A successful poll's data, in the hold of its lower completion before
   SlotPollDone: SlotPollAccept is nonzero when it may be queued as an
   event (the power gate open). While the gate is closed it is not queued
   and not delivered; SlotPollDone sets Lost instead, which the driver takes
   with SlotTakeLost (in a later hold) into its queue's lost flag, so the
   library refreshes the device rather than take an event of the
   interrupted session as current (mtp.md 5.4, "Late and absent
   completions"). */
int SlotPollAccept(const SLOT_DEVICE *D);
int SlotTakeLost(SLOT_DEVICE *D);

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

/*
 * The admission of an IRP without a handle (the driver's remove lock),
 * under the device lock. The pin above covers only the dispatch's first
 * reads; the admission keeps the device for the rest of the dispatch,
 * calls down and completions included. Only REMOVE waits for it, before it
 * forwards REMOVE and detaches; the deleter never does, and no device is
 * detached, so none can be deleted, while an admission is out.
 * - SlotActiveEnter: 0 once the device is detached or REMOVE has closed
 *   the admission (the IRP is then completed from the driver's globals),
 *   else the admission is taken.
 * - SlotActiveLeave: the dispatch's last access to the device.
 * - SlotActiveClose: REMOVE, before it gives its own admission up; then
 *   it waits until SlotActiveOut is 0.
 * An accepted query (SlotQueryDecide's 1) carries a second admission of
 * its own, taken in the hold of that decision and given up by its lower
 * completion (SlotQueryEnd). The dispatch or power worker that forwarded
 * it keeps its own admission to its own last device access, whenever the
 * query's answer comes: neither lifetime rides on the other.
 */
int SlotActiveEnter(SLOT_DEVICE *D);
void SlotActiveLeave(SLOT_DEVICE *D);
void SlotActiveClose(SLOT_DEVICE *D);
int SlotActiveOut(const SLOT_DEVICE *D);

/*
 * The power worker: a power IRP that came above PASSIVE_LEVEL is forwarded
 * from the device's one preallocated work item. Each queued IRP carries
 * its own admission. Under the device lock:
 * - SlotPowerQueue: the IRP goes on the driver's list in the same hold;
 *   nonzero when the caller must queue the work item (none is queued or
 *   running).
 * - SlotPowerNext: the worker's step. Forwarded nonzero: the IRP it has
 *   just forwarded gives its admission up here. Nonzero: take the next IRP
 *   from the list in this same hold. 0: the list is empty and the worker
 *   is no longer running. That hold is the worker's last device access. It
 *   is the admission of the IRP being forwarded that keeps the device, so
 *   no count of the worker itself is drained by the deleter. An accepted
 *   query is no exception: the worker passes Forwarded nonzero after it
 *   too, since the query's completion gives up only the query's own second
 *   admission (SlotQueryDecide, SlotQueryEnd), which may already be gone,
 *   inline or on another processor, when the worker comes back here.
 */
int SlotPowerQueue(SLOT_DEVICE *D);
int SlotPowerNext(SLOT_DEVICE *D, int Forwarded);

/*
 * The power gate, under the device lock (mtp.md 5.4, "The power gate" and
 * "Standby"). Five reasons, kept apart; PowerDown is their mask and the
 * gate is closed while it is not 0 (SlotAdmit refuses SO_ASLEEP,
 * SlotPollStart refuses and the poll does not re-arm):
 * - SLOT_PD_SYS: a system state below working has been set; ends with
 *   SlotPowerUp after an S0 completed successfully.
 * - SLOT_PD_DEV: a device state below D0; ends after a successful D0.
 * - SLOT_PD_QUERY: a system standby's query is being prepared or was
 *   accepted; ends at our refusal (SlotQueryDecide), the lower drivers'
 *   refusal (SlotQueryEnd), a system set below working (SlotPowerDown with
 *   SLOT_PD_SYS), an S0 whatever its result (SlotPowerUp with
 *   SLOT_PD_QUERY), or the local lapse (SlotQueryExpire).
 * - SLOT_PD_DQUERY: another component's device query below D0, the same
 *   way, with its own events: a device set below D0 (SlotPowerDown with
 *   SLOT_PD_DEV), a D0 whatever its result (SlotPowerUp with
 *   SLOT_PD_DQUERY), its own lapse. No system IRP ends it, and no device
 *   IRP ends the system's.
 * - SLOT_PD_DRAIN: a request the gate stopped has not yet ended. Set with
 *   each of the others; ended only here, in the hold in which every slot
 *   is idle with no completion tail out (at once if nothing was out). No
 *   power IRP ends it, so nothing reopens the gate over a request the
 *   stack still holds (the quarantine).
 * SlotPowerDown (SYS or DEV) and SlotQueryBegin add their reason and DRAIN
 * and stop every slot (SO_CANCELLED). The driver then waits, with a bound,
 * for quiet (DRAIN ended); a set goes down at quiet or at its bound, once
 * SlotCalling is 0, in the hold of SlotSetForward.
 */
#define SLOT_PD_SYS    1
#define SLOT_PD_DEV    2
#define SLOT_PD_QUERY  4
#define SLOT_PD_DRAIN  8
#define SLOT_PD_DQUERY 16
void SlotPowerDown(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Why, SLOT_ACTIONS *Out);
void SlotPowerUp(SLOT_DEVICE *D, int Why);

/* A set into sleep (system or device) is passed down: in the hold that
   decides it, after the bounded wait and once SlotCalling is 0. Returns
   SlotInfoHeld (what goes down still out, quarantined) and counts the set
   in Forced (modulo 65536) when that is not 0. */
int SlotSetForward(SLOT_DEVICE *D);

/*
 * A query below working (IRP_MN_QUERY_POWER): Which is SLOT_QS for a system
 * state (Query, SLOT_PD_QUERY), SLOT_QD for another component's device
 * state below D0 (DQuery, SLOT_PD_DQUERY). A query for S0 or for D0 is
 * passed down untouched and calls none of these.
 * - SlotQueryBegin: close the gate for that reason, stop every slot; the
 *   query becomes PREPARING. A repeated query (already ACCEPTED) does the
 *   same; nothing is out, so it finds quiet at once.
 * - SlotQueryDecide: at quiet or at the driver's bound, by the state in
 *   this hold, called under the query IRP's own admission (its dispatch's
 *   or the power worker's): 1 (FORWARDED: the driver passes the query down
 *   with its completion routine) only when preparing and DRAIN has ended;
 *   the query's second admission, its completion's, is taken in this same
 *   hold (safe without SlotActiveEnter's checks: the caller's admission
 *   keeps REMOVE's drain from having ended). Else 0, the refusal: the
 *   reason ends here, the refusal is counted (Vetoes, modulo 65536) with
 *   what was out (HeldAtVeto), and the driver completes the query failed,
 *   not passed down. DRAIN stays until the last stopped request has ended.
 *   (An admission count at its limit, never met, also refuses.)
 * - SlotQueryEnd: the lower drivers' final answer to a forwarded query,
 *   in its completion routine: success makes it ACCEPTED at tick Now (the
 *   gate stays closed by its reason until a set or a rollback); failure
 *   ends the reason (the driver's own rollback). Whatever the query's state
 *   by then, it gives up the second admission SlotQueryDecide took: this
 *   hold is the completion routine's last device access.
 * - SlotQueryExpire: the local lapse, a recovery heuristic (mtp.md 5.4):
 *   an ACCEPTED query for which no set, S0/D0 or other query has come
 *   within HoldMs of its acceptance ends its reason. The elapsed time is
 *   (unsigned long)(Now - At), right across the tick count's wrap. The
 *   driver calls it in the same hold of the lock as the admission, CLAIM,
 *   EVENTS start or GET_INFO that it precedes. Returns the reasons it
 *   ended (SLOT_PD_QUERY, SLOT_PD_DQUERY), 0 if none.
 */
#define SLOT_QS 0
#define SLOT_QD 1
#define SLOT_Q_NONE      0
#define SLOT_Q_PREPARING 1
#define SLOT_Q_FORWARDED 2
#define SLOT_Q_ACCEPTED  3
void SlotQueryBegin(SLOT_DEVICE *D, const SLOT_OPS *Ops, int Which, SLOT_ACTIONS *Out);
int SlotQueryDecide(SLOT_DEVICE *D, int Which);
void SlotQueryEnd(SLOT_DEVICE *D, int Which, int LowerOk, unsigned long Now);
int SlotQueryExpire(SLOT_DEVICE *D, unsigned long Now, unsigned long HoldMs);

/* Nonzero if every slot is idle with nothing outstanding. */
int SlotAllIdle(const SLOT_DEVICE *D);

/* Bit s set for each slot s not idle with nothing outstanding (what the
   gate is still waiting for, for the trace). */
int SlotBusyMask(const SLOT_DEVICE *D);

/*
 * What GET_INFO reports as the slots still out while draining (mtp.md 5.3,
 * interface 1.1): 0 unless DRAIN stands; then bit 0 bulk IN, 1 bulk OUT,
 * 2 control, 3 interrupt, 4 WAIT_EVENT (SlotBusyMask), and bit 5 when
 * completion tails are still returning requests to their callers (DRAIN
 * can stand on those alone, with every slot idle). DRAIN stands exactly
 * when this is not 0 (the model checks it in every state).
 */
#define SLOT_HELD_TAILS 0x20
int SlotInfoHeld(const SLOT_DEVICE *D);

#endif
