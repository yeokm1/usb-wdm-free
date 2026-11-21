/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * slotmodel.c - exhaustive model check of USBMTP.SYS's slot transitions
 * (sys/slot.c; ownership rules in sys/slot.h; docs/design/mtp.md 5.4).
 * Built and run on the build host by slotmodel.ps1 with the driver's own
 * compiler.
 *
 * Every party around the driver is an actor; their steps interleave in
 * every order. slot.c's functions run atomically (under the lock); each
 * action they return is a separate later step, during which anything else
 * may happen:
 * - hosts: successive requests on a slot (bulk IN or OUT, WAIT_EVENT,
 *   RESET_PIPE), each admission in its two parts under one hold of the
 *   lock; the I/O manager may cancel the IRP, any number of times, before
 *   or during admission or later; a host whose claim was released keeps
 *   its stale epoch;
 * - submits: rebuild the lower IRP (its Cancel bit cleared) and pass it
 *   down, then re-check; IoCancelIrp sets the lower IRP's Cancel bit and,
 *   while the stack holds it, takes the stack's cancel routine;
 * - the stack: completes a lower IRP it holds at any moment: full, short,
 *   zero-length, failed, or failed part-way (where enabled). In "needs
 *   cancel" scenarios it never ends a stopped request unless its cancel
 *   routine was taken while it held the IRP, so a lost cancel shows as a
 *   run that cannot end;
 * - each request's one-shot timer and DPC, the cancel routine; each with
 *   its last device access and then its return as separate steps;
 * - completion tails: IoCompleteRequest of the captured IRP, then
 *   SlotTailDone, as separate steps; the host may admit its next request
 *   between them;
 * - the interrupt poll delivering events to the wait slot, also during
 *   removal; release, a new claim (turnover), removal, its detach and the
 *   free after it; ABORT, also by a handle of an earlier claim; the
 *   admission counter wrapping;
 * - IRPs without a handle (power): the dispatch pin under the driver-global
 *   lock, the admission (slot.c's SlotActiveEnter) taken under it, the pin
 *   left, then either the IRP forwarded inline under its admission or
 *   queued for the power worker carrying it (SlotPowerQueue, the work item
 *   queued and counted in the driver-global count DriverUnload waits for),
 *   the worker forwarding each IRP and giving its admission up in the hold
 *   that takes the next (SlotPowerNext); an IRP not admitted is completed
 *   from the driver's globals: at PASSIVE_LEVEL in its dispatch, or, when
 *   it came above it, by the late hand-off (the driver-global list and
 *   worker, entered and counted in the very step that refused it, or
 *   while the pin is held; its own actor, which DriverUnload waits for
 *   unless DriverUnload runs inside one of its completions). Each
 *   completion may close the last handle in its own call chain, and that
 *   close may claim the deletion;
 * - REMOVE closing the admission (SlotActiveClose), waiting for it to
 *   drain before it forwards REMOVE, then its rundown and detach; the
 *   deleter's unpublish, its hold that finds no pin and publishes the
 *   object as deleted, IoDeleteDevice and DriverUnload inside it;
 * - the system's power IRPs, one at a time, as the power manager sends
 *   them (the standby rework of 2026-11-17, mtp.md 5.4, "Standby"): a
 *   standby query (Query): admitted, the gate closed for QUERY and every
 *   slot stopped (SlotQueryBegin), the cancels sent, then the decision
 *   (SlotQueryDecide) at quiet or at the bound, which the model, having no
 *   time, may take at any moment: accepted and passed down (the lower
 *   drivers may refuse it, LowerVeto: SlotQueryEnd), or refused; after an
 *   acceptance its set, or (OtherVeto) an S0 from another device's refusal,
 *   or no set at all and the preparation's lapse (SlotQueryExpire); after a
 *   refusal an S0 (Reaffirm); a second query. A set without preparation
 *   (Sleep): the gate closed for the system and every slot stopped
 *   (SlotPowerDown, SLOT_PD_SYS), the IRP down once, at quiet or at the
 *   bound. The driver requests no device power IRP (mtp.md 5.4, "Power").
 *   The wake: S0 forwarded, the system's and the query's reasons ended.
 *   Another sender's D3 and D0 (OtherD) enter like any IRP without a
 *   handle, admitted (D3: the gate closed for the device, the IRP down at
 *   quiet or at the bound; D0: on success the device's reason ends, and a
 *   failed D0 (FailWake) leaves the gate closed) or refused. DRAIN ends
 *   only in slot.c, when the last stopped request has ended. Since the
 *   review of 8557bdb: every SA_SUBMIT is performed as a commit under the
 *   lock (SlotSubmitCommit; withdrawn if a stop came) and then the call; a
 *   set (system, or D3) goes down only when no submitter is inside a
 *   committed call (SlotCalling), in the hold of SlotSetForward; a query is
 *   repeated from its acceptance or after a refusal; after a refusal a set
 *   may come (LateSet), and after the lapse a set with whatever was
 *   admitted since; an S0 may fail (FailS0: QUERY ends, SYS stays); a
 *   query may come above PASSIVE_LEVEL (QueryQueued): prepared, decided and
 *   completed on the power worker, one refused admission completed by the
 *   late hand-off; another sender's device query (DQuery) before its D3,
 *   accepted or refused (SlotQueryBegin and the rest with SLOT_QD), then its
 *   D3, a D0 (its rollback) or nothing and the lapse, its D3 anyway after a
 *   refusal; a power IRP refused admission is completed with a failure; one
 *   clock, moved only by a lapse step, starting just below the tick count's
 *   wrap, and the admission's own hold calls SlotQueryExpire first. Since
 *   the third review of the rework: an accepted query takes an admission
 *   of its own in its decision's hold, and its lower answer is an actor of
 *   its own (the completion routine's hold, giving that admission up, then
 *   the IRP's completion on up), which runs at any moment after the
 *   PoCallDriver, before or after the dispatch's or the power worker's own
 *   last device access under the forwarding IRP's admission;
 * - (Retain) a stack that keeps a request the gate stopped for good: at
 *   any moment while it holds such a lower IRP it may decide never to
 *   return it; otherwise it returns it late, at any time (before or after
 *   the bound, during the sleep, after the wake);
 * - the stack below: a conservative compatibility constraint inferred from
 *   the standby runs under NUSB's EHCI on 98 SE (2026-11-16), not a proven
 *   property of any stack: after a D3 passed down during a system sleep, a
 *   later REMOVE is taken never to return; the model fails a REMOVE in
 *   that state (it does not play the REMOVE held below). The runs read it
 *   only for the driver's own D3. MUT_SLEEP_REQUESTS_D3 is the design of
 *   3044286 (D3 requested at a system sleep, D0 after S0) and must be
 *   caught by it. (ExpectWedge) another sender's D3 during a sleep, then
 *   REMOVE: the current driver forwards that D3, so the run must meet the
 *   constraint; with the constraint left off for another sender's D3
 *   (OtherNoWedge: on ME under xHCI98 a REMOVE after another component's
 *   D3 and D0 around a standby returned, read once) the same scenario must
 *   pass;
 * - the sleep reached synchronously inside a completion tail, which 5.4
 *   excludes by assumption: before the standby rework the run had to end
 *   stuck; with the bounded wait it passes (the set goes down with that
 *   tail out). (ExpectHang) a stack that keeps a stopped read for good,
 *   then removal: REMOVE's rundown keeps its requirement, so the run must
 *   end stuck.
 *
 * Checked in every state, and at the end of every run:
 * - each admitted IRP is completed exactly once, by its tail, and only
 *   when no actor holds anything of its request (an independent scan of
 *   every actor's pending steps and the stack);
 * - its outcome is the first stop decided while it was open, or else the
 *   result of its final lower completion (precedence), and never changes;
 * - the completed byte count equals the lower completions' sum;
 * - an IoCancelIrp reaches only the request it was made for; a lower IRP
 *   is passed down again only after it came back;
 * - a slot is reserved exactly while a RESET_PIPE holding it is out;
 * - pins and segments stay within slot.h's limits; no admission number 0;
 *   no action is ever dropped from a full list (SlotDropped stays 0);
 * - a stale epoch is refused, at admission and at an ABORT, which then
 *   changes nothing; nothing touches the device after its free;
 *   the free comes only when the rundown is done, the device detached and
 *   no handle open, and no IRP of an open handle reaches the device after
 *   it (the device object outlives its handles: mtp.md 5.4); exactly one
 *   party claims the free (REMOVE at its detach, or a close), whatever
 *   the order of the closes and the detach;
 * - no power IRP is passed down after REMOVE; no admission is out at the
 *   free; no work routine runs after DriverUnload, and the work item is
 *   never queued again before its routine has started; no URB is passed
 *   down while the device sleeps, and none is with the stack then but one
 *   the gate stopped (the quarantine of a set gone down at its bound);
 * - the standby rework: a query is accepted only when nothing is out (an
 *   independent scan: no lower IRP with the stack, no admitted IRP or tail
 *   unfinished, no actor inside a slot's call); nothing is admitted
 *   between an accepted query and its set; nothing is admitted while a
 *   request the gate stopped is still out (no S0, D0, refusal or lapse
 *   reopens over it); a lower IRP with the stack always belongs to its
 *   slot (nothing freed or forgotten at the bound); a caller's IRP is never
 *   completed while its lower IRP is with the stack; a request the stack
 *   keeps is never completed, stays its slot's, and keeps the gate closed
 *   to the end; with nothing kept and no sleep, failed wake or removal
 *   left, the gate is open at the end (no refusal or lapse leaves it
 *   closed);
 * - since the review of 8557bdb: no lower IRP is passed down without its
 *   commit; every power IRP has an identity (MPIRP) and is passed down at
 *   most once, completed exactly once, never completed by the driver as not
 *   passed down after it was passed down, never passed down after its
 *   completion or after a refused admission; a query succeeds only when it
 *   was passed down and the lower drivers answered success; every set not
 *   refused admission is passed down; a query is passed down only when
 *   nothing is out (an independent scan); nothing is admitted between an
 *   accepted device query and its set; no system IRP changes the device
 *   query's state and no device IRP the system query's; each query's gate
 *   reason agrees with its state; GET_INFO's mask of what is out
 *   (SlotInfoHeld) is nonzero exactly while DRAIN stands and within its
 *   documented bits, and the refusal and forced-set counts equal the
 *   model's own; a set reports something out exactly when the independent
 *   scan finds something out; a poll's data with the gate closed is never
 *   delivered and always flagged lost (SlotPollAccept, SlotTakeLost); a
 *   self-test runs slot.c's lapse arithmetic at its boundary and across the
 *   wrap, the counters past FFFFh, the mask's tails bit and the reasons
 *   kept apart;
 * - every run ends: nothing stuck (a deleter waiting on something its own
 *   call chain holds shows as a stuck actor), a released claim ends FREE
 *   (or is claimed again), a removal reaches its free. An expected outcome
 *   (ExpectWedge, ExpectHang) is counted at each terminal state where it is
 *   met, and every other branch is explored and must pass.
 *
 * Mutants of the model's own driver steps (compile switches):
 * HL_PIN_ACROSS_COMPLETION (the dispatch pin held across the completion),
 * MUT_LATE_WORKER (an IRP not admitted is completed by a driver-global
 * worker whose count the deleter waits for: the design before
 * 2026-11-16's fifth review), MUT_UNLOAD_NO_WAIT (DriverUnload does not
 * wait for the worker's count), MUT_PUBLISH_LATE (the deleter's
 * last pin check and its publication of g_Deleted in two holds, the order
 * before 2026-11-16's sixth review) and MUT_ENTER_IGNORES_GONE (SlotEnter
 * ignoring SlotGone); those two are caught together, and each alone is
 * covered by the other layer (mtp.md 5.4). The power rework of the sixth
 * review: MUT_UNLOAD_WAITS_SELF (DriverUnload waits for the late worker
 * on its own thread), MUT_UNLOAD_NO_LATE_WAIT (it never waits for it),
 * MUT_LATE_INLINE (a refused power IRP above PASSIVE_LEVEL completed in
 * its dispatch), MUT_LATE_ADD_AFTER (its late-list entry a later step than
 * the refusal) and MUT_ONE_REASON (the wake's S0 opening the gate for the
 * device too, the single flag of before). After the standby readings of
 * 2026-11-16: MUT_SLEEP_REQUESTS_D3 (above). The standby rework of
 * 2026-11-17, its seeded faults: MUT_UNBOUNDED_WAIT (the wait for quiet
 * without a bound, the design before it), MUT_QUERY_NO_QUIET (a query
 * accepted without quiet), MUT_VETO_KEEPS_GATE (a refusal that leaves the
 * query's reason standing), MUT_DEADLINE_COMPLETES (the caller's IRP
 * completed at the bound, a double completion when the lower IRP comes
 * back), MUT_DEADLINE_FREES (the slot taken back at the bound),
 * MUT_RESUBMIT_HELD (the stopped request sent again after the wake) and
 * MUT_WAKE_RELEASES (S0 or D0 reopening over what is out). The earlier
 * MUT_SLEEP_NO_QUIET (the sleep IRP down without waiting) is withdrawn:
 * with a bound, going down at once is the bound taken at zero, which a
 * model without time cannot tell from a longer one. The review of 8557bdb:
 * MUT_SUBMIT_NO_COMMIT (an SA_SUBMIT performed as emitted, no commit),
 * MUT_SET_IGNORES_CALLS (a set down with a committed call not returned),
 * MUT_REMOVAL_QUERY_SUCCESS (a query refused admission completed with
 * success, the rule of 8557bdb), MUT_DQUERY_PASSTHROUGH (a device query
 * passed down untouched), MUT_S0_ENDS_DQUERY (an S0 ending the device
 * query), MUT_VETO_ALSO_FORWARDS (a refused query passed down too) and
 * MUT_LOWER_ANSWER_IGNORED (an accepted query completed with success
 * whatever the lower drivers answered). The third review of the rework:
 * MUT_WORKER_RIDES_QUERY (the accepted query's one admission handed to its
 * completion, the dispatch or worker giving none up after it: the stage-2
 * plan before that review) and MUT_QUERY_UNPROTECTED (the dispatch or
 * worker keeping its own, the completion holding none). slotmut.ps1 builds
 * and runs each, with broken copies of slot.c, and checks each fails for
 * its reason.
 *
 * States are stored whole and compared byte for byte on a hash match.
 * Interleavings are counted on the state graph in 64-bit integers; a
 * count that overflows is reported as such.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sys/slot.h"

#define MAXREQ   9          /* model requests 1..9: host h's k-th is h*3+k+1 */
#define MAXACT   14
#define MAXOPS   5
#define MAXHOST  3
#define MAXPIRP  7          /* power IRPs of one run, each with its own identity */
#define PIRP_ID  0x80       /* a power IRP's steps carry PIRP_ID + its index as their Req */
#define HOLD_MS  30000UL    /* QUERY_HOLD_MS, the lapse (mtp.md 5.4) */
#define CLOCK0   0xFFFF9000UL   /* the tick count at the start: a lapse crosses its wrap */

enum {
    OP_NONE, OP_ADMITCHK, OP_SUBMIT_CALL, OP_SUBMIT_RET, OP_CANCEL_CALL, OP_PIN_RET,
    OP_CR_BEGIN, OP_CR_END, OP_CR_RET, OP_TM_BEGIN, OP_TM_END, OP_TM_RET,
    OP_TAIL_COMPLETE, OP_TAIL_DONE, OP_LRET, OP_FREE,
    OP_DETACH, OP_GONE, OP_HL_ENTER, OP_HL_LEAVE, OP_HL_CLOSE,
    OP_HL_ADMIT, OP_HL_CALL, OP_HL_DONE, OP_HL_REFUSED, OP_HL_QUEUE, OP_HL_QITEM,
    OP_PW_NEXT, OP_PW_CALL, OP_PW_END, OP_RM_FWD, OP_UNLOAD, OP_SLEEP_FWD, OP_WAKE_FWD,
    OP_PUBLISH, OP_PINCHK,
    OP_LW_NEXT, OP_LW_COMPLETE, OP_LW_END, OP_D_REQ, OP_D_ENTER, OP_D_ADMIT, OP_D_FWD, OP_D_SIG, OP_D_WAIT,
    OP_Q_DECIDE, OP_Q_CALL, OP_Q_END,
    OP_SUBMIT_COMMIT, OP_Q_BEGIN, OP_Q_DONE, OP_Q_VETO, OP_SUBMIT_REBUILD, OP_SUBMIT_BUILT
};

/* A power IRP's identity: what happened to it, each counted. */
typedef struct MPIRP {
    unsigned short Kind : 3;    /* PK_* */
    unsigned short Fwd : 1;     /* passed down (PoCallDriver): at most once */
    unsigned short Done : 1;    /* completed (by the driver, or by the lower drivers for a skipped set): exactly once */
    unsigned short Local : 1;   /* completed by this driver without passing it down */
    unsigned short Status : 2;  /* 1 success, 2 failure */
    unsigned short LowerOk : 2; /* a forwarded query: the lower drivers' answer (1 success, 2 failure) */
    unsigned short Refused : 1; /* refused admission (removal begun, the device detached or deleted) */
    unsigned short Own : 1;     /* requested by this driver (MUT_SLEEP_REQUESTS_D3 only) */
    unsigned short Pad : 4;
} MPIRP;
#define PK_QUERY  1     /* a system query below working */
#define PK_SET    2     /* a system set below working */
#define PK_S0     3
#define PK_D3     4
#define PK_D0     5
#define PK_DQUERY 6     /* a device query below D0 */
#define PK_HL     7     /* an IRP without a handle passed through (the zombie scenarios) */

typedef struct OPQ { unsigned char Op, Slot, Req; } OPQ;
typedef struct ACTOR { unsigned char Used, N, Late, Pad; OPQ Q[MAXOPS]; } ACTOR;   /* Late: 1 the late worker, 2 it ran DriverUnload */

typedef struct MREQ {
    unsigned char Made, Routine, Cancelled, Cancels, Timer, Completed, Outcome, Admitted;
    unsigned char Refused, LowerSum, Slot, FirstStop, Final, Decided, TailOut, Pad;
} MREQ;

typedef struct HOST {
    unsigned char Slot, Next, Count, Segs, Reserve, Timeout, Waiting, Pad;
    unsigned long Epoch;
} HOST;

typedef struct MSTATE {
    SLOT_DEVICE D;
    MREQ R[MAXREQ + 1];
    HOST H[MAXHOST];
    ACTOR A[MAXACT];
    unsigned char Map[16];               /* driver request number (low 4 bits) -> model request */
    unsigned char InStack[SLOT_COUNT];   /* the stack holds the slot's lower IRP */
    unsigned char Seen[SLOT_COUNT];      /* its cancel routine was taken while the stack held it */
    unsigned char Bit[SLOT_COUNT];       /* the lower IRP's Cancel bit */
    unsigned char StackReq[SLOT_COUNT];
    unsigned char Lock, Freed, ReleaseUsed, RemoveUsed, StopUsed, ReclaimUsed, RemoveReady, EventsLeft;
    unsigned char FreeReady;             /* SA_FREE_READY returned: the rundown done, no handle open */
    unsigned char Closed[MAXHOST];       /* the host's handle is closed */
    unsigned char RemoveFwd;             /* REMOVE has gone down: no power IRP may follow */
    unsigned char Unloaded;              /* DriverUnload has run (inside the last IoDeleteDevice) */
    unsigned char Running;               /* the driver-global count of work routines DriverUnload waits for */
    unsigned char ItemQueued;            /* the power work item is queued and its routine not yet started */
    unsigned char Asleep, SleepUsed, WakeUsed;
    unsigned char Published;             /* the deleter has published the object as deleted (g_Deleted) */
    unsigned char LateQ, LateQueued, LateRunning;  /* the late hand-off: refused power IRPs, its worker, its count */
    unsigned char DReq;                  /* the policy owner's D-IRP: 1 requested and not yet completed */
    unsigned char SysAsleep;             /* the system sleep IRP has gone down */
    unsigned char D0Failed;              /* the wake's D0 was admitted and failed */
    unsigned char Wedged;                /* a D3 went down during a system sleep: the stack below
                                            (as read under NUSB's EHCI) never returns a later REMOVE */
    unsigned char DSeq;                  /* the other sender's device IRPs (DS_*), one at a time */
    unsigned char Sys;                   /* the system's power IRPs (SYS_*), one at a time */
    unsigned char QueriesLeft;           /* standby queries the system may still send */
    unsigned char QAccepted;             /* the model's own record: a system query accepted, no set, S0 or lapse since */
    unsigned char DQAccepted;            /* the same for the other sender's device query */
    unsigned char Held;                  /* slots the gate stopped whose request has not ended (the model's own) */
    unsigned char Retained;              /* slots whose lower IRP the stack keeps for good */
    unsigned char S0Failed;              /* an S0 failed: the system's reason stands */
    unsigned char Halt;                  /* an expected outcome reached: this run ends here */
    unsigned char Lapsed;                /* the hold passed with no set: 1 the system's query, 2 the device's */
    unsigned char Rebuild;               /* slots whose lower IRP is being rebuilt (commit to SlotSubmitBuilt) */
    unsigned char NP;                    /* power IRPs so far */
    unsigned char PwL[3], NPw;           /* the power worker's list (identities), in order */
    unsigned char LateL[3], NLate;       /* the late hand-off's list (identities), in order */
    unsigned short Vetoes, Forced;       /* the model's own counts of refusals and forced sets */
    unsigned long Clock;                 /* the tick count: moved only by a lapse */
    unsigned long AtS, AtD;              /* the ticks at which the system and the device query were accepted */
    MPIRP P[MAXPIRP];
} MSTATE;

/* The other sender's device IRPs, one at a time: its query, then its D3
   and D0 (or, after an acceptance, a D0 instead or nothing and the lapse;
   after a refusal, the D3 anyway or nothing). */
#define DS_IDLE   0
#define DS_QUERY  1
#define DS_QACC   2
#define DS_QREF   3
#define DS_D3     4
#define DS_D3DONE 5
#define DS_D0     6
#define DS_END    7

/* The system's power IRPs, as the power manager sends them: one at a time. */
#define SYS_IDLE     0   /* working, no system IRP in flight */
#define SYS_QUERY    1   /* a standby query in flight */
#define SYS_ACCEPTED 2   /* the query accepted: the set, an S0 (another device refused) or nothing follows */
#define SYS_REFUSED  3   /* the query refused (ours or a lower driver's): an S0 follows (Reaffirm) */
#define SYS_SET      4   /* the set into sleep in flight */
#define SYS_ASLEEP   5   /* the set has gone down */
#define SYS_S0       6   /* S0 in flight (the wake, or the reaffirmation) */

typedef struct SCEN {
    const char *Name;
    int Hosts;
    HOST H[MAXHOST];
    int NeedsCancel, Release, Remove, Stop, StopSlot, Poll, UserCancels, Timers,
        Failures, Reclaim, Returns, Events, TailLimit;
    unsigned long Start;    /* the admission counter's first value */
    int Zombie;             /* handles stay open across the removal and close in any order after it */
    int StopStale;          /* the ABORT comes from a handle of the first claim, at any time */
    int Handleless;         /* this many IRPs without a handle (power) enter a dispatch routine at any time */
    int HlClose;            /* their completion closes host 1's handle, the last one perhaps (in its own call chain) */
    int PowerQueued;        /* they came above PASSIVE_LEVEL: forwarded by the power worker */
    int Sleep;              /* a system sleep and a wake, at any time */
    int FailWake;           /* the wake's D0 fails */
    int TailSleep;          /* the sleep comes synchronously inside the first completion tail (the assumption of 5.4) */
    int ExpectHang;         /* the scenario shows that assumption's failure mode: it must end stuck */
    int OtherD;             /* another sender's D3, then its D0, at any time (FailWake: that D0 fails) */
    int ExpectWedge;        /* the run must meet the stack constraint (a REMOVE after a D3 during a sleep) and nothing else */
    int OtherNoWedge;       /* the constraint not imposed on another sender's D3 (ME under xHCI98, read once) */
    int Query;              /* this many standbys begin with a query (the standby rework of mtp.md 5.4) */
    int Retain;             /* the stack may keep a request the gate stopped for good (or return it late, at any time) */
    int OtherVeto;          /* after our acceptance: an S0 (another device refused) or no set at all (the lapse) may come instead */
    int LowerVeto;          /* the lower drivers may refuse our forwarded query */
    int Reaffirm;           /* after a refusal the system sends S0 */
    int QueryQueued;        /* the system's queries come above PASSIVE_LEVEL: the power worker handles them,
                               and one refused admission goes to the late hand-off */
    int DQuery;             /* the other sender queries before its D3 (OtherD implied) */
    int LateSet;            /* a set may still come after a refused query, or after the lapse */
    int FailS0;             /* an S0 may fail */
} SCEN;

static const SCEN *g_Sc;
static unsigned long g_States, g_Steps, g_Fail, g_Expected;
static int g_Overflow;
static char g_Why[256];

/* ---------------------------------------------------------------- ops */

static int ReqOf(MSTATE *M, int Slot) { return M->Map[M->D.S[Slot].Request & 15]; }
static void OpArm(void *C, int Slot) { MSTATE *M = (MSTATE *)C; M->R[ReqOf(M, Slot)].Timer = 1; }
static int OpCancelTimer(void *C, int Slot)
{
    MSTATE *M = (MSTATE *)C;
    MREQ *R = &M->R[ReqOf(M, Slot)];
    if (R->Timer == 1) { R->Timer = 3; return 1; }
    return 0;
}
static void OpSetCancel(void *C, int Slot) { MSTATE *M = (MSTATE *)C; M->R[ReqOf(M, Slot)].Routine = 1; }
static int OpIrpCancelled(void *C, int Slot) { MSTATE *M = (MSTATE *)C; return M->R[ReqOf(M, Slot)].Cancelled; }
static int OpClearCancel(void *C, int Slot)
{
    MSTATE *M = (MSTATE *)C;
    MREQ *R = &M->R[ReqOf(M, Slot)];
    if (R->Routine) { R->Routine = 0; return 1; }
    return 0;
}
static SLOT_OPS g_Ops = { 0, OpArm, OpCancelTimer, OpSetCancel, OpIrpCancelled, OpClearCancel };

/* ---------------------------------------------------------------- helpers */

static void Fail(const char *Why)
{
    if (!g_Fail) { strncpy(g_Why, Why, sizeof(g_Why) - 1); g_Why[sizeof(g_Why) - 1] = 0; }
    g_Fail++;
}

static void Push(ACTOR *A, int Front, int Op, int Slot, int Req)
{
    int i;
    if (A->N >= MAXOPS) { Fail("op queue full"); return; }
    if (Front) { for (i = A->N; i > 0; i--) A->Q[i] = A->Q[i - 1]; i = 0; } else i = A->N;
    A->Q[i].Op = (unsigned char)Op; A->Q[i].Slot = (unsigned char)Slot; A->Q[i].Req = (unsigned char)Req;
    A->N++;
}

static ACTOR *Spawn(MSTATE *M)
{
    int i;
    for (i = MAXHOST; i < MAXACT; i++) if (!M->A[i].Used) { M->A[i].Used = 1; M->A[i].N = 0; M->A[i].Late = 0; return &M->A[i]; }
    Fail("no free actor");
    return &M->A[MAXACT - 1];
}

static void Retire(MSTATE *M, ACTOR *A) { if (A >= &M->A[MAXHOST] && !A->N) A->Used = 0; }

/* Power IRP identities: one record per IRP, its forwarding and completion
   counted where they happen. */
static int NewP(MSTATE *M, int Kind)
{
    MPIRP *P;
    if (M->NP >= MAXPIRP) { Fail("too many power IRPs for the model"); return PIRP_ID; }
    P = &M->P[M->NP];
    memset(P, 0, sizeof(*P));
    P->Kind = (unsigned char)Kind;
    return PIRP_ID + M->NP++;
}
static MPIRP *PRec(MSTATE *M, int Id)
{
    if (Id < PIRP_ID || Id - PIRP_ID >= M->NP) { Fail("a step for an unknown power IRP"); return &M->P[0]; }
    return &M->P[Id - PIRP_ID];
}
static void PFwd(MSTATE *M, int Id)
{
    MPIRP *P = PRec(M, Id);
    if (P->Done) Fail("a power IRP passed down after its completion");
    if (P->Fwd) Fail("a power IRP passed down twice");
    if (P->Refused) Fail("a power IRP refused admission was passed down");
    P->Fwd = 1;
}
static void PDone(MSTATE *M, int Id, int Ok, int Local)
{
    MPIRP *P = PRec(M, Id);
    if (P->Done) { Fail("a power IRP completed twice"); return; }
    if (Local && P->Fwd) Fail("a power IRP completed by the driver as not passed down, after it was passed down");
    /* a query succeeds only with the lower drivers' success (a refusal,
       ours or theirs, and a refused admission are failures) */
    if (Ok && (P->Kind == PK_QUERY || P->Kind == PK_DQUERY)) {
        if (P->Refused) Fail("a query refused admission (removal) completed with success");
        else if (!P->Fwd || P->LowerOk != 1) Fail("a query completed with success without the lower drivers' success");
    }
    P->Done = 1;
    P->Local = (unsigned char)Local;
    P->Status = (unsigned char)(Ok ? 1 : 2);
}

/* An independent count: does any actor still hold a step of request r? */
static int Holds(MSTATE *M, int r)
{
    int i, k;
    for (i = 0; i < MAXACT; i++) {
        if (!M->A[i].Used) continue;
        for (k = 0; k < M->A[i].N; k++) {
            int op = M->A[i].Q[k].Op;
            if (M->A[i].Q[k].Req == r && op != OP_ADMITCHK && op != OP_LRET && op != OP_CR_RET &&
                op != OP_TM_RET && op != OP_TAIL_COMPLETE && op != OP_TAIL_DONE)
                return 1;
        }
    }
    return 0;
}

static void CheckLimits(MSTATE *M)
{
    int s;
    for (s = 0; s < SLOT_COUNT; s++) {
        if (SlotDropped) Fail("an action dropped from a full list");
        if (M->D.S[s].Pins > SLOT_MAX_PINS) Fail("pins above SLOT_MAX_PINS");
        if (M->D.S[s].SegsLeft >= SLOT_MAX_SEGS) Fail("segments above SLOT_MAX_SEGS");
        if (M->D.S[s].State != SS_IDLE && M->D.S[s].State != SS_RESERVED && !M->D.S[s].Request)
            Fail("a slot holds admission number 0");
    }
    for (s = 0; s < SLOT_COUNT; s++) {
        int i, k, inside = 0;
        for (i = 0; i < MAXACT; i++)
            if (M->A[i].Used)
                for (k = 0; k < M->A[i].N; k++)
                    if (M->A[i].Q[k].Op == OP_SUBMIT_RET && M->A[i].Q[k].Slot == s) inside++;
        if (inside > 1) Fail("two IoCallDriver of one slot on stacks at once (recursion)");
    }
    if (M->D.Tails > M->D.TailLimit + SLOT_COUNT) Fail("completion tails beyond their bound");
    if ((unsigned long)M->D.TailLimit + SLOT_COUNT >= 65535UL) Fail("TailLimit not below the counter range");
    /* The reservation's owner is exact. */
    if ((M->D.S[SLOT_BIN].State == SS_RESERVED) !=
        (M->D.S[SLOT_CTL].State != SS_IDLE && M->D.S[SLOT_CTL].Reserve == SLOT_BIN))
        Fail("reservation without its owner, or an owner without its reservation");
    if (M->Freed && !SlotAllIdle(&M->D)) Fail("a slot became busy after the free");
    if (M->Freed && SlotActiveOut(&M->D)) Fail("an admission out after the free");
    for (s = 0; s < SLOT_COUNT; s++) {
        /* a request the gate stopped ends when the slot is idle again */
        if ((M->Held & (1 << s)) && M->D.S[s].State == SS_IDLE && !M->InStack[s]) M->Held = (unsigned char)(M->Held & ~(1 << s));
        /* the quarantine: only a request the gate stopped may be with the
           stack while the device sleeps (a set gone down at its bound) */
        if ((M->Asleep || M->SysAsleep) && M->InStack[s] && !(M->Held & (1 << s)))
            Fail("a URB with the stack while the device sleeps");
        /* a lower IRP with the stack always belongs to the slot (nothing
           freed or forgotten on a time-out) */
        if (M->InStack[s] && M->D.S[s].State == SS_IDLE) Fail("a lower IRP with the stack while its slot is free (freed or forgotten)");
        if ((M->Retained & (1 << s)) && !M->InStack[s]) Fail("a kept lower IRP left the stack");
    }
    /* what GET_INFO reports (interface 1.1): the slots still out name
       something exactly while DRAIN stands, within the documented bits;
       the counters are the model's own */
    {
        int held = SlotInfoHeld(&M->D);
        if ((held != 0) != ((M->D.PowerDown & SLOT_PD_DRAIN) != 0))
            Fail("GET_INFO's slots still out disagree with draining (DRAIN with nothing named, or the reverse)");
        if (held & ~0x3F) Fail("GET_INFO's slots still out has an undocumented bit");
        if (M->D.HeldAtVeto & ~0x3F) Fail("GET_INFO's mask at the last refusal has an undocumented bit");
        if (M->D.Vetoes != M->Vetoes) Fail("the refusal count differs from the refusals made");
        if (M->D.Forced != M->Forced) Fail("the forced-set count differs from the sets passed down with requests out");
    }
    if (M->D.PowerDown & ~(SLOT_PD_SYS | SLOT_PD_DEV | SLOT_PD_QUERY | SLOT_PD_DRAIN | SLOT_PD_DQUERY))
        Fail("an undocumented gate reason");
    if (((M->D.PowerDown & SLOT_PD_QUERY) != 0) != (M->D.Query != SLOT_Q_NONE))
        Fail("the system query's reason and its state disagree");
    if (((M->D.PowerDown & SLOT_PD_DQUERY) != 0) != (M->D.DQuery != SLOT_Q_NONE))
        Fail("the device query's reason and its state disagree");
}

/* The gate has just closed: note the slots it stopped that are still out. */
static void NoteHeld(MSTATE *M)
{
    int s;
    for (s = 0; s < SLOT_COUNT; s++)
        if (M->D.S[s].State != SS_IDLE || M->InStack[s]) M->Held |= (unsigned char)(1 << s);
}

/* An independent reading of "something is out": a lower IRP with the
   stack, an admitted IRP not yet completed or its tail not done, or an
   actor inside a slot's outside call or callback. */
static int Outstanding(MSTATE *M)
{
    int i, k;
    for (i = 0; i < SLOT_COUNT; i++) if (M->InStack[i]) return 1;
    for (i = 1; i <= MAXREQ; i++)
        if ((M->R[i].Admitted && !M->R[i].Completed) || M->R[i].TailOut) return 1;
    for (i = 0; i < MAXACT; i++) {
        if (!M->A[i].Used) continue;
        for (k = 0; k < M->A[i].N; k++) {
            int op = M->A[i].Q[k].Op;
            if (op == OP_SUBMIT_COMMIT || op == OP_SUBMIT_REBUILD || op == OP_SUBMIT_BUILT || op == OP_SUBMIT_CALL || op == OP_SUBMIT_RET || op == OP_CANCEL_CALL || op == OP_PIN_RET ||
                op == OP_CR_BEGIN || op == OP_CR_END || op == OP_TM_BEGIN || op == OP_TM_END ||
                op == OP_TAIL_COMPLETE || op == OP_TAIL_DONE)
                return 1;
        }
    }
    return 0;
}

/* The bounded wait of the standby rework: a wait for quiet may end at
   quiet or at its bound, which the model, having no time, may take at any
   moment. MUT_UNBOUNDED_WAIT restores the wait without a bound. */
static int WaitEnds(MSTATE *M)
{
#ifdef MUT_UNBOUNDED_WAIT
    return SlotAllIdle(&M->D) && !M->D.Tails;
#else
    (void)M;
    return 1;
#endif
}

/* A set into sleep (system, or a D3) goes down after its bounded wait, and
   only once no submitter is between its commit and its IoCallDriver's
   return (the submission protocol of mtp.md 5.4): an SA_SUBMIT committed
   before the gate closed has reached the stack, one not committed is
   withdrawn by its commit. MUT_SET_IGNORES_CALLS drops that condition. */
static int SetMayForward(MSTATE *M)
{
#ifdef MUT_SET_IGNORES_CALLS
    return WaitEnds(M);
#else
    return WaitEnds(M) && !SlotCalling(&M->D);
#endif
}

/* The system's state after its query was completed (refused, here or
   below, or refused admission): an S0 or a set may follow, or another
   query, as the scenario says. */
static void SysAfterRefusal(MSTATE *M)
{
    M->Sys = (unsigned char)((g_Sc->Reaffirm || g_Sc->LateSet) ? SYS_REFUSED : SYS_IDLE);
}

/* Only IRPs without a handle and the sleep observe REMOVE's forward and
   DriverUnload (the power IRPs passed down, the work routine). Where a
   scenario has neither, both are folded into the step before them, which
   changes no outcome and keeps the larger scenarios within the store. */
static int PowerActors(void) { return g_Sc->Handleless || g_Sc->Sleep || g_Sc->OtherD || g_Sc->Query || g_Sc->DQuery; }

/* The claimant's deletion as steps at the front of A: unpublish (SlotGone),
   the hold that finds no pin and publishes the object as deleted (g_Deleted)
   in one step, IoDeleteDevice as a later step, then DriverUnload inside it.
   MUT_PUBLISH_LATE is the order before 2026-11-16's sixth review: the hold
   that finds no pin, and the publication in a later hold. */
static void Deleter(ACTOR *A)
{
    if (PowerActors()) Push(A, 1, OP_UNLOAD, 0, 0);
    Push(A, 1, OP_FREE, 0, 0);
#ifdef MUT_PUBLISH_LATE
    Push(A, 1, OP_PINCHK, 0, 0);        /* and OP_FREE publishes, in its later hold */
#else
    Push(A, 1, OP_PUBLISH, 0, 0);
#endif
    Push(A, 1, OP_GONE, 0, 0);
}

/* Turn slot.c's actions into steps of actor A (ahead of its later steps). */
static void Take(MSTATE *M, ACTOR *A, SLOT_ACTIONS *Out)
{
    OPQ save[MAXOPS];
    int nsave = A->N, i;
    memcpy(save, A->Q, sizeof(save));
    A->N = 0;
    for (i = 0; i < Out->Count; i++) {
        SLOT_ACTION *X = &Out->A[i];
        int req = X->Request ? M->Map[X->Request & 15] : 0;
        switch (X->Kind) {
        case SA_SUBMIT:
#ifdef MUT_SUBMIT_NO_COMMIT
            /* the action performed as it was emitted: no commit under the lock */
            Push(A, 0, OP_SUBMIT_CALL, X->Slot, req);
#else
            Push(A, 0, OP_SUBMIT_COMMIT, X->Slot, req);
#endif
            break;
        case SA_CANCEL_LOWER: Push(A, 0, OP_CANCEL_CALL, X->Slot, req); break;
        case SA_COMPLETE: {
            MREQ *R;
            ACTOR *T;
            if (!X->Request) break;     /* a refusal: the admitter records it */
            R = &M->R[req];
            if ((unsigned long)(size_t)X->Irp != (unsigned long)req) Fail("completion for a different IRP");
            if (R->Completed || R->TailOut) { Fail("IRP handed to a second completion"); break; }
            if (R->Routine) Fail("completed with its cancel routine still set");
            if (R->Timer == 1 || R->Timer == 2) Fail("completed with its timer or DPC alive");
            if (M->InStack[X->Slot] && M->StackReq[X->Slot] == req) Fail("completed with its lower IRP in the stack");
            if (Holds(M, req)) Fail("completed while an actor still holds a step of the request");
            if ((unsigned long)R->LowerSum != X->Bytes) Fail("byte count differs from the lower completions");
            {
                int expect = R->FirstStop ? R->FirstStop : R->Final;
                if (expect && X->Outcome != expect) Fail("outcome is not the first decided one (precedence)");
            }
            R->TailOut = 1;
            R->Outcome = (unsigned char)X->Outcome;
            T = Spawn(M);
            Push(T, 0, OP_TAIL_COMPLETE, X->Slot, req);
            Push(T, 0, OP_TAIL_DONE, X->Slot, req);
            break;
        }
        case SA_REMOVE_READY: M->RemoveReady = 1; break;
        case SA_FREE_READY:
            if (!M->RemoveReady) Fail("free ready before the rundown");
            M->FreeReady = 1;
            break;
        }
    }
    for (i = 0; i < nsave; i++) Push(A, 0, save[i].Op, save[i].Slot, save[i].Req);
}

static void StartSleep(MSTATE *M, ACTOR *A);

/* A refused power IRP above PASSIVE_LEVEL onto the late list, in the same
   step (the driver's hold of g_Global) as its refusal: the late count is
   raised there, before the deleter can delete and DriverUnload read it.
   MUT_LATE_ADD_AFTER makes it a later step of the dispatch (the order this
   model found unsafe). */
static void LateAdd(MSTATE *M, int Id)
{
    PRec(M, Id)->Refused = 1;
    if (M->NLate >= sizeof(M->LateL)) Fail("late list too long for the model");
    else M->LateL[M->NLate++] = (unsigned char)Id;
    M->LateQ++;
    if (!M->LateQueued) {
        ACTOR *W = Spawn(M);
        M->LateQueued = 1;
        M->LateRunning++;
        W->Late = 1;
        Push(W, 0, OP_LW_NEXT, 0, 0);
    }
}
/* The IRP came above PASSIVE_LEVEL: the scenario's IRPs without a handle
   (PowerQueued), or a system query in a QueryQueued scenario (Slot 1). */
static int Elevated(int Slot) { return g_Sc->PowerQueued || (Slot == 1 && g_Sc->QueryQueued); }
static int LateNow(int Slot)
{
#if defined(MUT_LATE_INLINE) || defined(MUT_LATE_ADD_AFTER)
    (void)Slot;
    return 0;
#else
    return Elevated(Slot);
#endif
}

static int Touch(MSTATE *M) { if (M->Freed) { Fail("device touched after its free"); return 0; } return 1; }

/* Record the first stop decided for the request a slot holds, if open. */
static void NoteStop(MSTATE *M, int Slot, int WasActive, unsigned long WasReq, int Outcome)
{
    int r;
    if (!WasActive || !WasReq) return;
    r = M->Map[WasReq & 15];
    if (!M->R[r].FirstStop && !M->R[r].Final) M->R[r].FirstStop = (unsigned char)Outcome;
    (void)Slot;
}

/* ---------------------------------------------------------------- steps */

enum { ST_ACTOR, ST_HOST, ST_DONE, ST_POLLDONE, ST_USERCANCEL, ST_TIMERFIRE,
       ST_RELEASE, ST_RECLAIM, ST_REMOVE, ST_STOP, ST_CLOSE, ST_SLEEP, ST_WAKE, ST_OTHER_D,
       ST_QUERY, ST_S0, ST_LAPSE, ST_RETAIN, ST_DLAPSE, ST_DEND };

typedef struct STEP { int Kind, Who, Var; } STEP;

static int LockedOp(int Op)
{
    return Op == OP_SUBMIT_RET || Op == OP_PIN_RET || Op == OP_CR_BEGIN || Op == OP_CR_END ||
           Op == OP_TM_BEGIN || Op == OP_TM_END || Op == OP_TAIL_DONE || Op == OP_HL_ADMIT ||
           Op == OP_HL_DONE || Op == OP_HL_QUEUE || Op == OP_PW_NEXT || Op == OP_RM_FWD ||
           Op == OP_WAKE_FWD || Op == OP_D_ADMIT || Op == OP_D_FWD || Op == OP_Q_DECIDE || Op == OP_Q_END ||
           Op == OP_SUBMIT_COMMIT || Op == OP_Q_BEGIN || Op == OP_SUBMIT_BUILT;
}

static int HostReq(int h, int k) { return h * 3 + k + 1; }

static int Add(STEP *S, int n, int Kind, int Who, int Var) { S[n].Kind = Kind; S[n].Who = Who; S[n].Var = Var; return n + 1; }

static int Enabled(MSTATE *M, STEP *S)
{
    int n = 0, i, h, s, v;
    if (M->Halt) return 0;      /* an expected outcome: nothing further is explored from here */
    for (i = 0; i < MAXACT; i++) {
        ACTOR *A = &M->A[i];
        int op;
        if (!A->Used || !A->N) continue;
        op = A->Q[0].Op;
        if (op == OP_ADMITCHK) { n = Add(S, n, ST_ACTOR, i, 0); continue; }
        if (LockedOp(op) && M->Lock) continue;
        /* the deleter's wait: the rundown done, no handle open, no pin; the
           hold that finds no pin publishes in the same step (MUT_PUBLISH_LATE:
           it only checks, and OP_PUBLISH follows as a step of its own) */
#ifdef MUT_PUBLISH_LATE
        if (op == OP_PINCHK && (!M->FreeReady || SlotPinned(&M->D))) continue;
#else
        if (op == OP_PUBLISH && (!M->FreeReady || SlotPinned(&M->D))) continue;
#endif
#ifdef MUT_LATE_WORKER
        if (op == OP_FREE && M->Running) continue;        /* the deleter drains the work items */
#endif
        if (op == OP_RM_FWD && SlotActiveOut(&M->D)) continue;  /* REMOVE waits for the admissions */
        if (op == OP_DETACH && (!M->RemoveReady || !M->RemoveFwd)) continue; /* REMOVE waits for its rundown */
#ifndef MUT_UNLOAD_NO_WAIT
        if (op == OP_UNLOAD && M->Running) continue;      /* DriverUnload waits for the work routines */
#endif
        /* and for the late worker, unless on the late worker's own thread */
#if defined(MUT_UNLOAD_WAITS_SELF)
        if (op == OP_UNLOAD && M->LateRunning) continue;
#elif !defined(MUT_UNLOAD_NO_LATE_WAIT)
        if (op == OP_UNLOAD && M->LateRunning && !A->Late) continue;
#endif
        if ((op == OP_D_WAIT || op == OP_SLEEP_FWD) && M->DReq == 1) continue;   /* the D-IRP's completion first */
        /* a set's wait (system or D3): at quiet or at the bound, and no
           committed submit still inside its call; a query's: at quiet or at
           the bound */
        if (op == OP_D_FWD && A->Q[0].Slot == 3 && !SetMayForward(M)) continue;
        if (op == OP_SLEEP_FWD && !SetMayForward(M)) continue;
        if (op == OP_Q_DECIDE && !WaitEnds(M)) continue;
        if (op == OP_Q_END && g_Sc->LowerVeto && !M->Lock) {
            /* the lower drivers' answer: success, or their refusal */
            n = Add(S, n, ST_ACTOR, i, 0);
            n = Add(S, n, ST_ACTOR, i, 1);
            continue;
        }
        if (op == OP_WAKE_FWD && g_Sc->FailS0 && !M->Lock) {
            /* the S0's own result: success, or failure */
            n = Add(S, n, ST_ACTOR, i, 0);
            n = Add(S, n, ST_ACTOR, i, 1);
            continue;
        }
        if (op == OP_HL_CLOSE) {
            /* the completion closes host 1's handle: after the removal, with no call outstanding */
            HOST *H1 = &M->H[1];
            int idle = !H1->Waiting || M->R[HostReq(1, H1->Next)].Completed || M->R[HostReq(1, H1->Next)].Refused;
            if (!M->RemoveUsed || !idle || M->Lock) continue;
        }
        n = Add(S, n, ST_ACTOR, i, 0);
    }
    for (h = 0; h < g_Sc->Hosts; h++) {
        HOST *H = &M->H[h];
        if (M->A[h].N || M->Lock || M->Closed[h]) continue;
        if (g_Sc->Remove) {
            /* The handle's close, when it has no call outstanding: in a
               zombie scenario any time after the removal, else after its
               last request. */
            int idle = !H->Waiting || M->R[HostReq(h, H->Next)].Completed || M->R[HostReq(h, H->Next)].Refused;
            int last = H->Waiting && H->Next + 1 >= H->Count;
            if (idle && (g_Sc->Zombie ? M->RemoveUsed : last) && !(g_Sc->HlClose && h == 1))   /* host 1's: the IRP's */
                n = Add(S, n, ST_CLOSE, h, 0);
        }
        if (H->Waiting) {
            MREQ *R = &M->R[HostReq(h, H->Next)];
            if (!R->Completed && !R->Refused) continue;
            if (H->Next + 1 >= H->Count) continue;
        } else if (H->Next >= H->Count) continue;
        n = Add(S, n, ST_HOST, h, 0);
    }
    if (!M->Lock) {
        for (s = 0; s < SLOT_COUNT; s++) {
            if (M->InStack[s] && !(M->Retained & (1 << s))) {
                int stopped = M->D.S[s].State != SS_ACTIVE;
                /* a stack that may keep a request the gate stopped */
                if (g_Sc->Retain && (M->Held & (1 << s))) n = Add(S, n, ST_RETAIN, s, 0);
                if (!(g_Sc->NeedsCancel && stopped && !M->Seen[s])) {
                    if (s == SLOT_INT) {
                        /* an event (bounded, so runs end), or a failed poll */
                        if (M->EventsLeft) n = Add(S, n, ST_POLLDONE, s, 0);
                        n = Add(S, n, ST_POLLDONE, s, 1);
                    } else {
                        int kinds = g_Sc->Failures ? 5 : 2;
                        for (v = 0; v < kinds; v++) n = Add(S, n, ST_DONE, s, v);
                    }
                }
            }
        }
    }
    for (i = 1; i <= MAXREQ; i++) {
        MREQ *R = &M->R[i];
        if (!R->Made || R->Completed || R->Refused) continue;
        if (R->Cancels < g_Sc->UserCancels) n = Add(S, n, ST_USERCANCEL, i, 0);
        if (g_Sc->Timers && R->Timer == 1) n = Add(S, n, ST_TIMERFIRE, i, 0);
    }
    if (!M->Lock) {
        if (g_Sc->Release && !M->ReleaseUsed) n = Add(S, n, ST_RELEASE, 0, 0);
        if (g_Sc->Reclaim && M->ReleaseUsed && !M->ReclaimUsed && M->D.Claim == SC_FREE) n = Add(S, n, ST_RECLAIM, 0, 0);
        if (g_Sc->Remove && !M->RemoveUsed) n = Add(S, n, ST_REMOVE, 0, 0);
        if (g_Sc->Stop && !M->StopUsed) n = Add(S, n, ST_STOP, 0, 0);
        /* the system's IRPs, one at a time: a set without preparation; a
           standby query; after an accepted one, its set, or (OtherVeto) an
           S0 or nothing (the lapse); after a refusal, an S0 (Reaffirm);
           the wake */
        if (g_Sc->Sleep && !M->SleepUsed && M->Sys == SYS_IDLE) n = Add(S, n, ST_SLEEP, 0, 0);
        /* a query from working, after an acceptance (repeated) or after a refusal */
        if (M->QueriesLeft && (M->Sys == SYS_IDLE || M->Sys == SYS_ACCEPTED || M->Sys == SYS_REFUSED))
            n = Add(S, n, ST_QUERY, 0, 0);
        if (M->Sys == SYS_ACCEPTED) n = Add(S, n, ST_SLEEP, 1, 0);
        if (M->Sys == SYS_ACCEPTED && g_Sc->OtherVeto) { n = Add(S, n, ST_S0, 0, 0); n = Add(S, n, ST_LAPSE, 0, 0); }
        if (M->Sys == SYS_REFUSED && g_Sc->Reaffirm) n = Add(S, n, ST_S0, 0, 0);
        /* a committed set after a refused query, or after the lapse, with
           whatever was admitted since */
        if (g_Sc->LateSet && !M->SleepUsed && (M->Sys == SYS_REFUSED || (M->Sys == SYS_IDLE && (M->Lapsed & 1))))
            n = Add(S, n, ST_SLEEP, 2, 0);
        if (M->Sys == SYS_ASLEEP) n = Add(S, n, ST_WAKE, 0, 0);
        /* the other sender, one IRP at a time, each after the last completed */
        if (g_Sc->OtherD || g_Sc->DQuery) {
            switch (M->DSeq) {
            case DS_IDLE: n = Add(S, n, ST_OTHER_D, g_Sc->DQuery ? 4 : 3, 0); break;
            case DS_QACC:
                n = Add(S, n, ST_OTHER_D, 3, 0);
                n = Add(S, n, ST_OTHER_D, 0, 0);    /* its rollback */
                n = Add(S, n, ST_DLAPSE, 0, 0);     /* nothing more, and the lapse */
                break;
            case DS_QREF:
                n = Add(S, n, ST_OTHER_D, 3, 0);    /* its D3 anyway */
                n = Add(S, n, ST_DEND, 0, 0);       /* nothing more */
                break;
            case DS_D3DONE: n = Add(S, n, ST_OTHER_D, 0, 0); break;
            }
        }
    }
    return n;
}

/* A locked call on slot Slot, noting a stop decided on an open request. */
#define STOPCALL(M, Slot, Outc, CALL) do { int wa_ = (M)->D.S[Slot].State == SS_ACTIVE; \
    unsigned long wr_ = (M)->D.S[Slot].Request; CALL; NoteStop(M, Slot, wa_, wr_, Outc); } while (0)

/* A device IRP has been completed: its sender's next IRP may come. */
static void DAfter(MSTATE *M, int Id)
{
    MPIRP *P = PRec(M, Id);
    if (P->Own) { M->DReq = 2; return; }
    switch (P->Kind) {
    case PK_DQUERY: M->DSeq = (unsigned char)(P->Status == 1 ? DS_QACC : DS_QREF); break;
    case PK_D3: M->DSeq = DS_D3DONE; break;
    case PK_D0: M->DSeq = DS_END; break;
    }
}

/* A power IRP not admitted (removal begun, the device detached or
   deleted): completed from the driver's globals with a failure status
   (STATUS_DELETE_PENDING), never passed down, the device untouched. A
   system query among them is no exception (mtp.md 5.4, "Removal during
   preparation"); MUT_REMOVAL_QUERY_SUCCESS completes it with success, the
   rule this model's earlier version took as an assumption. */
static void NotAdmitted(MSTATE *M, int Id)
{
    MPIRP *P = PRec(M, Id);
    int ok = 0;
    P->Refused = 1;
#ifdef MUT_REMOVAL_QUERY_SUCCESS
    if (P->Kind == PK_QUERY) ok = 1;
#endif
    PDone(M, Id, ok, 1);
    switch (P->Kind) {
    case PK_QUERY: if (ok) M->Sys = SYS_ACCEPTED; else SysAfterRefusal(M); break;
    case PK_SET: case PK_S0: M->Sys = SYS_IDLE; break;
    case PK_D3: case PK_D0: case PK_DQUERY: DAfter(M, Id); break;
    }
}

/* A query's preparation, in one hold: the gate closed for its reason and
   every slot stopped (SlotQueryBegin), the cancels as A's next steps. */
static void QueryPrepare(MSTATE *M, ACTOR *A, int Which)
{
    SLOT_ACTIONS Out;
    int s, wa[SLOT_COUNT];
    unsigned long wr[SLOT_COUNT];
    Out.Count = 0;
    if (Which == SLOT_QS) M->QAccepted = 0; else M->DQAccepted = 0;
    for (s = 0; s < SLOT_COUNT; s++) { wa[s] = M->D.S[s].State == SS_ACTIVE; wr[s] = M->D.S[s].Request; }
    SlotQueryBegin(&M->D, &g_Ops, Which, &Out);
    for (s = 0; s < SLOT_COUNT; s++) NoteStop(M, s, wa[s], wr[s], SO_CANCELLED);
    NoteHeld(M);
    Take(M, A, &Out);
}

/* A set going down: what slot.c reports still out must be what is out. */
static void SetForward(MSTATE *M)
{
    int held = SlotSetForward(&M->D);
    if ((held != 0) != (Outstanding(M) != 0))
        Fail("a set went down reporting other than what is out (GET_INFO's forced count)");
    if (held) M->Forced++;
}

/* A clock moved by a lapse only, monotonic across the wrap. Time is one:
   every accepted query whose hold has passed at the new tick has lapsed
   (the model's own record; slot.c learns it at its next SlotQueryExpire). */
static void ClockTo(MSTATE *M, unsigned long At)
{
    if ((unsigned long)(M->Clock - At) < HOLD_MS) M->Clock = At + HOLD_MS;
    if (M->QAccepted && (unsigned long)(M->Clock - M->AtS) >= HOLD_MS) { M->QAccepted = 0; M->Lapsed |= 1; }
    if (M->DQAccepted && (unsigned long)(M->Clock - M->AtD) >= HOLD_MS) { M->DQAccepted = 0; M->Lapsed |= 2; }
}

static void RunOp(MSTATE *M, int Who, int Var)
{
    ACTOR *A = &M->A[Who];
    OPQ q = A->Q[0];
    SLOT_ACTIONS Out;
    int i;
    for (i = 1; i < A->N; i++) A->Q[i - 1] = A->Q[i];
    A->N--;
    Out.Count = 0;
    /* A work routine is counted until its last act; nothing else of the
       driver's that DriverUnload is meant to outwait may run after it. */
    if (M->Unloaded && (q.Op == OP_PW_NEXT || q.Op == OP_PW_CALL || q.Op == OP_PW_END))
        Fail("a work routine ran after DriverUnload");
    if (M->Unloaded && A->Late == 1 && (q.Op == OP_LW_NEXT || q.Op == OP_LW_COMPLETE || q.Op == OP_LW_END))
        Fail("the late worker ran after DriverUnload on another thread");
    switch (q.Op) {
    case OP_ADMITCHK:
        if (Touch(M)) {
            int wa = M->D.S[q.Slot].State == SS_ACTIVE;
            unsigned long wr = M->D.S[q.Slot].Request;
            SlotAdmitCheck(&M->D, &g_Ops, q.Slot, &Out);
            if (M->R[q.Req].Cancelled) NoteStop(M, q.Slot, wa, wr, SO_CANCELLED);
        }
        M->Lock = 0;
        Take(M, A, &Out);
        break;
    case OP_SUBMIT_COMMIT:
        /* the submitter's commit, under the lock, before it rebuilds the
           lower IRP: committed, the call follows; withdrawn, never sent */
        if (!Touch(M)) break;
        if (SlotSubmitCommit(&M->D, &g_Ops, q.Slot, &Out)) {
            M->Rebuild = (unsigned char)(M->Rebuild | (1 << q.Slot));
            Push(A, 1, OP_SUBMIT_REBUILD, q.Slot, q.Req);
        } else Take(M, A, &Out);
        break;
    case OP_SUBMIT_REBUILD: {
        /* IoInitializeIrp and the rebuild, outside the lock: no IoCancelIrp
           on this IRP may be in progress (one called and not returned) */
        int i2, k2;
        if (!Touch(M)) break;
        for (i2 = 0; i2 < MAXACT; i2++)
            if (M->A[i2].Used)
                for (k2 = 0; k2 < M->A[i2].N; k2++)
                    if (M->A[i2].Q[k2].Op == OP_PIN_RET && M->A[i2].Q[k2].Slot == q.Slot)
                        Fail("the lower IRP rebuilt while an IoCancelIrp on it was in progress");
        M->Bit[q.Slot] = 0;             /* IoInitializeIrp: rebuilt, Cancel cleared */
        Push(A, 1, OP_SUBMIT_BUILT, q.Slot, q.Req);
        break;
    }
    case OP_SUBMIT_BUILT: {
        /* under the lock: the rebuild has finished; hand off, or withdraw
           if a stop came during it (its cancel was deferred) */
        int k2;
        if (!Touch(M)) break;
        M->Rebuild = (unsigned char)(M->Rebuild & ~(1 << q.Slot));
        if (SlotSubmitBuilt(&M->D, &g_Ops, q.Slot, &Out)) { Push(A, 1, OP_SUBMIT_CALL, q.Slot, q.Req); break; }
        for (k2 = 0; k2 < Out.Count; k2++)
            if (Out.A[k2].Kind == SA_CANCEL_LOWER && Out.A[k2].Slot == q.Slot)
                Fail("a deferred cancel delivered twice (the IRP withdrawn and cancelled)");
        Take(M, A, &Out);
        break;
    }
    case OP_SUBMIT_CALL:
        if (!Touch(M)) break;
        if (M->InStack[q.Slot]) { Fail("lower IRP passed down while the stack still had it"); break; }
        if (M->D.S[q.Slot].State == SS_IDLE || ReqOf(M, q.Slot) != q.Req) { Fail("submit for a request the slot no longer holds"); break; }
        if (M->Asleep || M->SysAsleep) { Fail("a URB passed down while the device sleeps"); break; }
#if !defined(MUT_SUBMIT_NO_COMMIT) && !defined(MUT_RESUBMIT_HELD)
        if (!M->D.S[q.Slot].Calling) { Fail("a lower IRP passed down without its commit"); break; }
#endif
#if defined(MUT_SUBMIT_NO_COMMIT) || defined(MUT_RESUBMIT_HELD)
        M->Bit[q.Slot] = 0;             /* the mutants' submissions skip the rebuild step */
#endif
        M->InStack[q.Slot] = 1;
        M->Seen[q.Slot] = 0;
        M->StackReq[q.Slot] = q.Req;
        Push(A, 1, OP_SUBMIT_RET, q.Slot, q.Req);
        break;
    case OP_SUBMIT_RET:
        if (Touch(M)) SlotSubmitReturned(&M->D, &g_Ops, q.Slot, &Out);
        Take(M, A, &Out);
        break;
    case OP_CANCEL_CALL:
        if (!Touch(M)) break;
        if (M->D.S[q.Slot].State == SS_IDLE || ReqOf(M, q.Slot) != q.Req) { Fail("IoCancelIrp reached a later request"); break; }
        if (M->Rebuild & (1 << q.Slot)) Fail("an IoCancelIrp overlapped the lower IRP's rebuild");
        M->Bit[q.Slot] = 1;
        if (M->InStack[q.Slot] && M->StackReq[q.Slot] == q.Req) M->Seen[q.Slot] = 1;
        Push(A, 1, OP_PIN_RET, q.Slot, q.Req);
        break;
    case OP_PIN_RET:
        if (Touch(M)) SlotPinReturned(&M->D, &g_Ops, q.Slot, &Out);
        Take(M, A, &Out);
        break;
    case OP_CR_BEGIN:
        if (Touch(M)) STOPCALL(M, q.Slot, SO_CANCELLED, SlotCancelBegin(&M->D, &g_Ops, q.Slot, &Out));
        Take(M, A, &Out);
        break;
    case OP_CR_END:
        if (Touch(M)) SlotCancelEnd(&M->D, &g_Ops, q.Slot, &Out);
        Take(M, A, &Out);
        break;
    case OP_TM_BEGIN:
        if (Touch(M)) STOPCALL(M, q.Slot, SO_TIMEOUT, SlotTimerBegin(&M->D, &g_Ops, q.Slot, &Out));
        Take(M, A, &Out);
        break;
    case OP_TM_END:
        M->R[q.Req].Timer = 3;
        if (Touch(M)) SlotTimerEnd(&M->D, &g_Ops, q.Slot, &Out);
        Take(M, A, &Out);
        break;
    case OP_TAIL_COMPLETE:
        /* IoCompleteRequest on the captured IRP: no device access. In a
           TailSleep scenario the first one reaches the sleep dispatch
           synchronously, before its SlotTailDone. */
        if (M->R[q.Req].Completed) Fail("IRP completed twice");
        if (M->InStack[q.Slot] && M->StackReq[q.Slot] == q.Req) Fail("completed with its lower IRP in the stack");
        M->R[q.Req].Completed = 1;
        if (g_Sc->TailSleep && !M->SleepUsed && M->Sys == SYS_IDLE) StartSleep(M, A);
        break;
    case OP_TAIL_DONE:
        if (Touch(M)) SlotTailDone(&M->D, &Out);
        M->R[q.Req].TailOut = 0;
        Take(M, A, &Out);
        break;
    case OP_LRET: case OP_CR_RET: case OP_TM_RET:
        break;      /* a callback's return: no device access */
    case OP_DETACH:
        /* IoDetachDevice, then the claim under the lock (step 5). */
        if (!Touch(M)) break;
        if (SlotDetach(&M->D)) Deleter(A);  /* first: unpublish, then wait for the pins */
        break;
    case OP_RM_FWD:
        /* the admissions drained: REMOVE goes down */
        if (Touch(M)) M->RemoveFwd = 1;
        break;
    case OP_UNLOAD:
        /* DriverUnload, inside the deleter's IoDeleteDevice */
        M->Unloaded = 1;
        if (A->Late) A->Late = 2;           /* on the late worker's own thread */
        break;
    case OP_GONE:
        /* the deleter, under the driver-global lock */
        if (Touch(M)) SlotGone(&M->D);
        break;
    case OP_HL_ENTER:
        /* an IRP without a handle, under the driver-global lock: a deleted
           device object is recognised first (the driver's g_Deleted), then
           SlotEnter pins the device or finds it unpublished. Slot 1: a
           system query (QueryQueued). */
        if (M->Unloaded) {
            /* no device object left to send it to: it never reaches this driver */
            MPIRP *P = PRec(M, q.Req);
            P->Done = 1; P->Status = 2; P->Refused = 1; P->Local = 1;
            if (P->Kind == PK_QUERY) SysAfterRefusal(M);
            break;
        }
        {
            int pinned;
            if (M->Published) pinned = 0;   /* Dev == g_Deleted: SlotEnter is not called */
            else {
                if (M->Freed) Fail("a dispatch read a deleted device that was not published as deleted");
#ifdef MUT_ENTER_IGNORES_GONE
                {
                    /* slot.c's SlotEnter ignoring the deleter's SlotGone */
                    unsigned char g = M->D.Gone;
                    M->D.Gone = 0;
                    pinned = SlotEnter(&M->D);
                    M->D.Gone = g;
                }
#else
                pinned = SlotEnter(&M->D);
#endif
            }
            if (!pinned) {
                if (LateNow(q.Slot)) LateAdd(M, q.Req);
                else Push(A, 1, OP_HL_REFUSED, q.Slot, q.Req);
                break;
            }
        }
        Push(A, 1, OP_HL_ADMIT, q.Slot, q.Req);
        break;
    case OP_HL_ADMIT: {
        /* under the pin and the device lock: the admission; then the pin is
           left before any call (the rule of mtp.md 5.4).
           HL_PIN_ACROSS_COMPLETION is the mutant that leaves it after. */
        int adm;
        if (!Touch(M)) break;
        adm = SlotActiveEnter(&M->D);
#ifndef HL_PIN_ACROSS_COMPLETION
        Push(A, 0, OP_HL_LEAVE, 0, 0);
#endif
        if (!adm && LateNow(q.Slot)) LateAdd(M, q.Req);     /* the pin still held: OP_HL_LEAVE follows */
        else if (!adm) Push(A, 0, OP_HL_REFUSED, q.Slot, q.Req);
        else if (Elevated(q.Slot)) Push(A, 0, OP_HL_QUEUE, q.Slot, q.Req);
        else { Push(A, 0, OP_HL_CALL, 0, q.Req); Push(A, 0, OP_HL_DONE, 0, 0); }
#ifdef HL_PIN_ACROSS_COMPLETION
        Push(A, 0, OP_HL_LEAVE, 0, 0);
#endif
        break;
    }
    case OP_HL_CALL:
    case OP_PW_CALL:
        /* PoCallDriver under the IRP's admission (passed through: the lower
           drivers complete it); the IRP may complete in this call, and its
           completion may close host 1's handle */
        if (!Touch(M)) break;
        if (M->RemoveFwd) Fail("a power IRP passed down after REMOVE");
        PFwd(M, q.Req);
        PDone(M, q.Req, 1, 0);
        if (g_Sc->HlClose && !M->Closed[1]) Push(A, 1, OP_HL_CLOSE, 0, 0);
        break;
    case OP_HL_DONE:
        if (Touch(M)) SlotActiveLeave(&M->D);   /* the dispatch's last access */
        break;
    case OP_HL_REFUSED:
        /* completed from the driver's globals, touching nothing of the
           device; its completion may close host 1's handle. MUT_LATE_WORKER
           completes it from a driver-global worker whose count the deleter
           drains (the deadlock of the fifth review). */
#ifdef MUT_LATE_WORKER
        M->Running++;
        Push(A, 1, OP_PW_END, 0, 0);
#endif
#if defined(MUT_LATE_ADD_AFTER)
        if (Elevated(q.Slot)) { LateAdd(M, q.Req); break; }   /* the list entry as a later step */
#endif
        /* inline: only at PASSIVE_LEVEL (the late list takes the others) */
        if (Elevated(q.Slot)) Fail("a power IRP completed above PASSIVE_LEVEL");
        NotAdmitted(M, q.Req);
        if (g_Sc->HlClose && !M->Closed[1]) Push(A, 1, OP_HL_CLOSE, 0, 0);
        break;
    case OP_LW_NEXT:
        /* under the driver-global lock: take the next refused IRP, or end */
        if (M->LateQ) {
            int id = M->LateL[0], k;
            M->LateQ--;
            for (k = 1; k < M->NLate; k++) M->LateL[k - 1] = M->LateL[k];
            M->LateL[--M->NLate] = 0;
            Push(A, 0, OP_LW_COMPLETE, 0, id);
            Push(A, 0, OP_LW_NEXT, 0, 0);
        } else {
            M->LateQueued = 0;
            Push(A, 0, OP_LW_END, 0, 0);
        }
        break;
    case OP_LW_COMPLETE:
        /* PoStartNextPowerIrp, IoCompleteRequest at PASSIVE_LEVEL (failure);
           the completion may close host 1's handle and delete, in this chain */
        NotAdmitted(M, q.Req);
        if (g_Sc->HlClose && !M->Closed[1]) Push(A, 1, OP_HL_CLOSE, 0, 0);
        break;
    case OP_LW_END:
        /* the late worker's last act */
        if (!M->LateRunning) Fail("the late count went below 0");
        else M->LateRunning--;
        break;
    case OP_D_REQ: {
        /* PoRequestPowerIrp: the D-IRP comes back through the dispatch
           (Slot 3: D3, 0: D0); the requester waits for its completion */
        ACTOR *X = Spawn(M);
        int id = NewP(M, q.Slot ? PK_D3 : PK_D0);
        PRec(M, id)->Own = 1;
        M->DReq = 1;
        Push(X, 0, OP_D_ENTER, q.Slot, id);
        break;
    }
    case OP_D_ENTER:
        /* a device IRP at PASSIVE_LEVEL (Slot 3: D3, 0: D0, 4: a device
           query below D0): the pin, or refused and completed at once */
        if (M->Unloaded || M->Published || !SlotEnter(&M->D)) { NotAdmitted(M, q.Req); break; }
        Push(A, 1, OP_D_ADMIT, q.Slot, q.Req);
        break;
    case OP_D_ADMIT: {
        int adm;
        if (!Touch(M)) break;
        adm = SlotActiveEnter(&M->D);
        SlotLeave(&M->D);
        if (!adm) { NotAdmitted(M, q.Req); break; }
        if (q.Slot == 4) {
#ifdef MUT_DQUERY_PASSTHROUGH
            /* the device query passed down untouched (the design before):
               no completion routine of ours, no admission of its own */
            Push(A, 0, OP_Q_CALL, SLOT_QD | 4, q.Req);
#else
            QueryPrepare(M, A, SLOT_QD);
            Push(A, 0, OP_Q_DECIDE, SLOT_QD, q.Req);
#endif
            Push(A, 0, OP_HL_DONE, 0, 0);
            break;
        }
        if (q.Slot == 3) {
            int wa[SLOT_COUNT], k;
            unsigned long wr[SLOT_COUNT];
            M->DQAccepted = 0;      /* the set replaces the device query's preparation */
            for (k = 0; k < SLOT_COUNT; k++) { wa[k] = M->D.S[k].State == SS_ACTIVE; wr[k] = M->D.S[k].Request; }
            {
                unsigned char sq = M->D.Query;
                SlotPowerDown(&M->D, &g_Ops, SLOT_PD_DEV, &Out);
                if (M->D.Query != sq) Fail("a device set ended the system query");
                if (M->D.DQuery != SLOT_Q_NONE) Fail("a device set left the device query standing");
            }
            for (k = 0; k < SLOT_COUNT; k++) NoteStop(M, k, wa[k], wr[k], SO_CANCELLED);
            NoteHeld(M);
            Take(M, A, &Out);
        }
        Push(A, 0, OP_D_FWD, q.Slot, q.Req);
        Push(A, 0, OP_HL_DONE, 0, 0);
        Push(A, 0, OP_D_SIG, 0, q.Req);
        break;
    }
    case OP_D_FWD:
        /* D3: after its bounded wait and with no committed submit still in
           its call, down once; the device sleeps. D0: forwarded and waited
           for; on success the device's reason ends; the device query's
           reason ends whatever its result */
        if (!Touch(M)) break;
        if (M->RemoveFwd) Fail("a power IRP passed down after REMOVE");
        if (q.Slot) {
            SetForward(M);
            PFwd(M, q.Req);
            PDone(M, q.Req, 1, 0);
            M->Asleep = 1;
            /* the constraint as inferred from the NUSB runs; OtherNoWedge
               leaves another sender's D3 out of it (as read on ME) */
            if ((M->Sys == SYS_SET || M->Sys == SYS_ASLEEP) && !(!PRec(M, q.Req)->Own && g_Sc->OtherNoWedge)) M->Wedged = 1;
            break;
        }
        PFwd(M, q.Req);
        M->DQAccepted = 0;
        if (g_Sc->FailWake) {
            M->D0Failed = 1;
            SlotPowerUp(&M->D, SLOT_PD_DQUERY);
            PDone(M, q.Req, 0, 0);
            break;
        }
        M->Asleep = 0;
        {
            unsigned char sq = M->D.Query;
            SlotPowerUp(&M->D, SLOT_PD_DEV | SLOT_PD_DQUERY);
            if (M->D.Query != sq) Fail("a device D0 ended the system query");
        }
#ifdef MUT_WAKE_RELEASES
        M->D.PowerDown = (unsigned char)(M->D.PowerDown & ~SLOT_PD_DRAIN);   /* the D0 reopening over what is out */
#endif
        if (M->D.PowerDown & SLOT_PD_DEV) Fail("the device's reason stayed after a successful D0");
        PDone(M, q.Req, 1, 0);
        break;
    case OP_D_SIG:
        /* the sender's completion function: its next IRP may come */
        DAfter(M, q.Req);
        break;
    case OP_D_WAIT:
        break;
    case OP_HL_QUEUE:
        /* above PASSIVE_LEVEL: on the list, carrying its admission */
        if (!Touch(M)) break;
        if (M->NPw >= sizeof(M->PwL)) Fail("power worker list too long for the model");
        else M->PwL[M->NPw++] = q.Req;
        if (SlotPowerQueue(&M->D)) {
            M->Running++;
            Push(A, 1, OP_HL_QITEM, 0, 0);
        }
        break;
    case OP_HL_QITEM: {
        /* ExInitializeWorkItem, ExQueueWorkItem of the device's one item */
        ACTOR *W;
        if (!Touch(M)) break;
        if (M->ItemQueued) Fail("the work item queued again before its routine started");
        M->ItemQueued = 1;
        W = Spawn(M);
        Push(W, 0, OP_PW_NEXT, 0, 0);
        break;
    }
    case OP_PW_NEXT:
        /* the worker, under the device lock: give up the forwarded IRP's
           admission (Slot 1; an accepted query's too: its completion gives
           up only its own), take the next IRP or end. Slot 0: the routine's
           first step; 2: MUT_WORKER_RIDES_QUERY's step after a query */
        if (!Touch(M)) break;
        if (!q.Slot) M->ItemQueued = 0;
        if (SlotPowerNext(&M->D, q.Slot == 1)) {
            int id = M->PwL[0], k;
            for (k = 1; k < M->NPw; k++) M->PwL[k - 1] = M->PwL[k];
            M->PwL[--M->NPw] = 0;
            if (PRec(M, id)->Kind == PK_QUERY) {
                /* the query's preparation, decision and completion, all on
                   the worker under the IRP's admission */
                Push(A, 0, OP_Q_BEGIN, SLOT_QS | 2, id);
                Push(A, 0, OP_Q_DECIDE, SLOT_QS | 2, id);
            } else {
                Push(A, 0, OP_PW_CALL, 0, id);
            }
            Push(A, 0, OP_PW_NEXT, 1, 0);
        } else {
            Push(A, 0, OP_PW_END, 0, 0);
        }
        break;
    case OP_PW_END:
        /* the driver-global count, the routine's last act */
        if (!M->Running) Fail("the work count went below 0");
        else M->Running--;
        break;
    case OP_SLEEP_FWD:
        /* after the bounded wait, with no committed submit in its call: the
           system sleep IRP goes down, once */
        if (!Touch(M)) break;
        if (M->RemoveFwd) Fail("a power IRP passed down after REMOVE");
#if defined(MUT_DEADLINE_COMPLETES) || defined(MUT_DEADLINE_FREES)
        for (i = 0; i < SLOT_COUNT; i++) {
            SLOT *T = &M->D.S[i];
            if (T->State == SS_IDLE || !T->Lower) continue;
#ifdef MUT_DEADLINE_COMPLETES
            /* the caller's IRP completed at the bound (cancelled), its
               lower IRP still with the stack */
            if (T->HasIrp && !M->R[ReqOf(M, i)].Completed) {
                ACTOR *X = Spawn(M);
                Push(X, 0, OP_TAIL_COMPLETE, i, ReqOf(M, i));
            }
#else
            /* the slot taken back at the bound (freed or forgotten) */
            T->State = SS_IDLE; T->Lower = 0; T->Pins = 0; T->Timer = 0; T->Cancel = 0; T->Request = 0;
            T->Irp = 0; T->HasIrp = 0; T->SegsLeft = 0; T->Submitting = 0; T->Resubmit = 0; T->CancelAgain = 0;
            T->Calling = 0; T->Rebuilding = 0;
#endif
        }
#endif
#if !defined(MUT_DEADLINE_FREES)
        SetForward(M);
#endif
        PFwd(M, q.Req);
        PDone(M, q.Req, 1, 0);
        M->SysAsleep = 1;
        M->Sys = SYS_ASLEEP;
        Push(A, 1, OP_HL_DONE, 0, 0);
        break;
    case OP_WAKE_FWD: {
        int dq;
        /* S0 forwarded and waited for (Var 1: it failed): the query's
           reason ends whatever the result, the system's on success only
           (all reasons in MUT_ONE_REASON, the single flag of before the
           sixth review); DRAIN stays while anything is out */
        if (!Touch(M)) break;
        if (M->RemoveFwd) Fail("a power IRP passed down after REMOVE");
        PFwd(M, q.Req);
        M->Sys = SYS_IDLE;
        if (Var) {
            dq = M->D.DQuery;
            SlotPowerUp(&M->D, SLOT_PD_QUERY);
            if (M->D.DQuery != dq) Fail("a system S0 ended the device query");
            if (M->D.PowerDown & SLOT_PD_SYS) M->S0Failed = 1;
            PDone(M, q.Req, 0, 0);
            break;
        }
        M->SysAsleep = 0;
        dq = M->D.DQuery;
#ifdef MUT_ONE_REASON
        SlotPowerUp(&M->D, SLOT_PD_SYS | SLOT_PD_DEV | SLOT_PD_QUERY);
#elif defined(MUT_S0_ENDS_DQUERY)
        SlotPowerUp(&M->D, SLOT_PD_SYS | SLOT_PD_QUERY | SLOT_PD_DQUERY);
#else
        SlotPowerUp(&M->D, SLOT_PD_SYS | SLOT_PD_QUERY);
#endif
        if (M->D.DQuery != dq) Fail("a system S0 ended the device query");
#ifdef MUT_WAKE_RELEASES
        M->D.PowerDown = (unsigned char)(M->D.PowerDown & ~SLOT_PD_DRAIN);   /* S0 reopening over what is out */
#endif
#ifdef MUT_RESUBMIT_HELD
        /* the request the gate stopped sent again after the wake */
        for (i = 0; i < SLOT_COUNT; i++)
            if ((M->Held & (1 << i)) && i != SLOT_WAIT && M->D.S[i].State != SS_IDLE)
                Push(A, 1, OP_SUBMIT_CALL, i, ReqOf(M, i));
#endif
#ifdef MUT_SLEEP_REQUESTS_D3
        Push(A, 1, OP_D_WAIT, 0, 0);
        Push(A, 1, OP_D_REQ, 0, 0);
#endif
        PDone(M, q.Req, 1, 0);
        break;
    }
    case OP_HL_CLOSE:
        /* IoCompleteRequest of the IRP; its completion closes host 1's
           handle synchronously, in this same call chain */
        if (M->Closed[1]) break;            /* another completion closed it: nothing to do */
        if (!Touch(M)) break;
        M->Closed[1] = 1;
        {
            int del = SlotClose(&M->D, &Out);
            Take(M, A, &Out);
            if (del) Deleter(A);            /* this close claimed the deletion: in this chain */
        }
        break;
    case OP_HL_LEAVE:
        if (Touch(M)) SlotLeave(&M->D);
        break;
    case OP_Q_BEGIN:
        /* the worker's hold that prepares the query (Slot: Which | 2) */
        if (!Touch(M)) break;
        QueryPrepare(M, A, q.Slot & 1);
        break;
    case OP_Q_DECIDE: {
        /* the query's decision, by the state in this hold, at quiet or at
           the bound (Slot: Which, | 2 on the power worker). Accepted: the
           query's own admission is taken in this hold (SlotQueryDecide),
           and PoCallDriver comes next, ahead of the dispatch's or worker's
           last step; the lower answer is an actor of its own (OP_Q_CALL). */
        int acc, which = q.Slot & 1;
        unsigned short act = M->D.Active;
        if (!Touch(M)) break;
#ifdef MUT_QUERY_NO_QUIET
        /* accepted without looking: passed down whatever is out (the
           query's admission taken as SlotQueryDecide takes it) */
        acc = 1;
        if (which == SLOT_QD) M->D.DQuery = SLOT_Q_FORWARDED; else M->D.Query = SLOT_Q_FORWARDED;
        M->D.Active++;
#else
        acc = SlotQueryDecide(&M->D, which);
#endif
        if (acc && M->D.Active != act + 1) Fail("an accepted query took no admission of its own");
        if (!acc && M->D.Active != act) Fail("a refused query changed the admissions");
#if defined(MUT_WORKER_RIDES_QUERY) || defined(MUT_QUERY_UNPROTECTED)
        /* no second admission: the forwarding IRP's one only */
        if (acc) M->D.Active--;
#endif
#ifdef MUT_WORKER_RIDES_QUERY
        /* the stage-2 plan before its third review: the accepted query's
           one admission handed to its completion, and the dispatch or
           worker giving none up after it (SlotPowerNext(..., !kept)) */
        if (acc && A->N && A->Q[0].Op == OP_HL_DONE) {
            int k;
            for (k = 1; k < A->N; k++) A->Q[k - 1] = A->Q[k];
            A->N--;
        } else if (acc && A->N && A->Q[0].Op == OP_PW_NEXT) A->Q[0].Slot = 2;
#endif
#ifdef MUT_VETO_KEEPS_GATE
        if (!acc) {
            /* the refusal leaves the query's reason standing */
            M->D.PowerDown = (unsigned char)(M->D.PowerDown | (which == SLOT_QD ? SLOT_PD_DQUERY : SLOT_PD_QUERY));
            if (which == SLOT_QD) M->D.DQuery = SLOT_Q_PREPARING; else M->D.Query = SLOT_Q_PREPARING;
        }
#endif
        if (acc) {
            Push(A, 1, OP_Q_CALL, q.Slot, q.Req);
        } else {
            M->Vetoes++;
            Push(A, 1, OP_Q_VETO, q.Slot, q.Req);
        }
        break;
    }
    case OP_Q_CALL: {
        /* PoCallDriver of the query, under the forwarding IRP's admission:
           only on quiet (an independent scan). The lower drivers' answer is
           an actor of its own: the completion routine's hold (OP_Q_END, its
           last device access), then the IRP's completion on up (OP_Q_DONE).
           It may run inside this call or at any later moment, before or
           after the dispatch's or the worker's own last device access
           (Slot | 4: passed down untouched, no completion routine of ours) */
        ACTOR *X;
        if (!Touch(M)) break;
        if (M->RemoveFwd) Fail("a power IRP passed down after REMOVE");
        if (Outstanding(M)) Fail("a query passed down with a request outstanding (accepted without quiet)");
        PFwd(M, q.Req);
        X = Spawn(M);
        Push(X, 0, OP_Q_END, q.Slot, q.Req);
        Push(X, 0, OP_Q_DONE, q.Slot, q.Req);
        break;
    }
    case OP_Q_END: {
        /* the lower drivers' final answer (Var 1: their refusal, LowerVeto),
           taken by the completion routine in one hold, which gives up the
           query's own admission: its last device access */
        MPIRP *P = PRec(M, q.Req);
        if (!(q.Slot & 4) && !Touch(M)) break;
        if (!P->Fwd) Fail("a lower answer for a query that was not passed down");
        P->LowerOk = (unsigned char)(Var ? 2 : 1);
        if (!(q.Slot & 4)) {
#ifdef MUT_QUERY_UNPROTECTED
            /* the completion holding no admission: it gives none up */
            unsigned short act = M->D.Active;
            SlotQueryEnd(&M->D, q.Slot & 1, !Var, M->Clock);
            M->D.Active = act;
#else
            SlotQueryEnd(&M->D, q.Slot & 1, !Var, M->Clock);
#endif
        }
        if (!Var) {
            if ((q.Slot & 1) == SLOT_QD) { M->DQAccepted = 1; M->AtD = M->Clock; }
            else { M->QAccepted = 1; M->AtS = M->Clock; }
        }
        break;
    }
    case OP_Q_DONE: {
        /* the IRP completes on up with the lower drivers' status once the
           completion routine has returned: no device access */
        MPIRP *P = PRec(M, q.Req);
        int ok = P->LowerOk == 1;
#ifdef MUT_LOWER_ANSWER_IGNORED
        ok = 1;
#endif
        PDone(M, q.Req, ok, 0);
        if ((q.Slot & 1) == SLOT_QD) DAfter(M, q.Req);
        else if (ok) M->Sys = SYS_ACCEPTED;
        else SysAfterRefusal(M);
        break;
    }
    case OP_Q_VETO:
        /* the refusal: completed STATUS_DEVICE_BUSY, not passed down */
        PDone(M, q.Req, 0, 1);
#ifdef MUT_VETO_ALSO_FORWARDS
        PFwd(M, q.Req);
#endif
        if ((q.Slot & 1) == SLOT_QD) DAfter(M, q.Req);
        else SysAfterRefusal(M);
        break;
    case OP_PINCHK:
        /* MUT_PUBLISH_LATE: the hold that found no pin, without publishing */
        break;
    case OP_PUBLISH:
        /* under the driver-global lock: g_Deleted = Dev, and n freed */
        if (M->Published) { Fail("the object published as deleted twice"); break; }
        M->Published = 1;
        break;
    case OP_FREE:
        /* IoDeleteDevice, a step after the publishing hold */
        if (M->Freed) { Fail("the device deleted twice"); break; }
#ifdef MUT_PUBLISH_LATE
        M->Published = 1;                   /* the publication in this later hold */
#endif
        if (!M->Published) { Fail("deleted before it was published as deleted"); break; }
        if (!M->D.Gone) { Fail("free while the device is still published"); break; }
        if (M->D.Entered) { Fail("free with a dispatch pin held"); break; }
        if (!M->D.Detached) { Fail("free before the detach"); break; }
        if (!SlotAllIdle(&M->D)) { Fail("free with a slot in use"); break; }
        if (M->D.Tails) { Fail("free with a completion tail still out"); break; }
        if (M->D.Opens) { Fail("free with a handle open"); break; }
        if (SlotActiveOut(&M->D)) { Fail("free with an admission out"); break; }
        for (i = 0; i < SLOT_COUNT; i++) if (M->InStack[i]) Fail("free with an IRP in the stack");
        M->Freed = 1;
        if (!PowerActors()) M->Unloaded = 1;    /* folded: nothing can observe it */
        break;
    }
    Retire(M, A);
}

/* A system sleep IRP at PASSIVE_LEVEL, as steps of actor A ahead of its
   later ones (a completion tail's, when it comes inside one): admitted, the
   gate closed for the system and every slot stopped in one hold; then the
   bounded wait, and the IRP goes down. */
static void StartSleep(MSTATE *M, ACTOR *A)
{
    SLOT_ACTIONS Out;
    OPQ save[MAXOPS];
    int nsave = A->N, i, s, wa[SLOT_COUNT], id = NewP(M, PK_SET);
    unsigned long wr[SLOT_COUNT];
    M->SleepUsed = 1;
    /* after an accepted query nothing may have been admitted since */
    if (M->QAccepted && Outstanding(M)) Fail("a request admitted between an accepted query and its set");
    M->QAccepted = 0;
    if (M->Published || !SlotActiveEnter(&M->D)) { NotAdmitted(M, id); return; }   /* completed from the driver's globals */
    M->Sys = SYS_SET;
    memcpy(save, A->Q, sizeof(save));
    A->N = 0;
    Out.Count = 0;
    for (s = 0; s < SLOT_COUNT; s++) { wa[s] = M->D.S[s].State == SS_ACTIVE; wr[s] = M->D.S[s].Request; }
    {
        unsigned char dq = M->D.DQuery;
        SlotPowerDown(&M->D, &g_Ops, SLOT_PD_SYS, &Out);
        if (M->D.DQuery != dq) Fail("a system set ended the device query");
        if (M->D.Query != SLOT_Q_NONE) Fail("a system set left the system query standing");
    }
    for (s = 0; s < SLOT_COUNT; s++) NoteStop(M, s, wa[s], wr[s], SO_CANCELLED);
    NoteHeld(M);
    Take(M, A, &Out);
#ifdef MUT_SLEEP_REQUESTS_D3
    Push(A, 0, OP_D_REQ, 3, 0);
#endif
    Push(A, 0, OP_SLEEP_FWD, 0, id);
    for (i = 0; i < nsave; i++) Push(A, 0, save[i].Op, save[i].Slot, save[i].Req);
}

static void Apply(MSTATE *M, const STEP *S)
{
    SLOT_ACTIONS Out;
    ACTOR *A;
    g_Ops.Ctx = M;
    Out.Count = 0;
    switch (S->Kind) {
    case ST_ACTOR:
        RunOp(M, S->Who, S->Var);
        break;
    case ST_QUERY: {
        /* a standby query: at PASSIVE_LEVEL, admitted, prepared in one hold
           (the gate closed for QUERY, every slot stopped), the cancels
           sent, then the decision at quiet or at the bound; or, in a
           QueryQueued scenario, above it, through the dispatch pin, the
           admission and the power worker. One not admitted (removal begun)
           is completed with a failure status from the globals. */
        int id = NewP(M, PK_QUERY);
        M->QueriesLeft--;
        M->Sys = SYS_QUERY;
        M->Lapsed = (unsigned char)(M->Lapsed & ~1);
        if (g_Sc->QueryQueued) {
            A = Spawn(M);
            Push(A, 0, OP_HL_ENTER, 1, id);
            break;
        }
        if (M->Published || !SlotActiveEnter(&M->D)) { NotAdmitted(M, id); break; }
        A = Spawn(M);
        QueryPrepare(M, A, SLOT_QS);
        Push(A, 0, OP_Q_DECIDE, SLOT_QS, id);
        Push(A, 0, OP_HL_DONE, 0, 0);
        break;
    }
    case ST_S0: {
        /* another device refused the standby after ours accepted, or ours
           (or a lower driver) refused it: the system sets working */
        int id = NewP(M, PK_S0);
        M->QAccepted = 0;
        M->Sys = SYS_S0;
        if (M->Published || !SlotActiveEnter(&M->D)) { NotAdmitted(M, id); break; }
        A = Spawn(M);
        Push(A, 0, OP_WAKE_FWD, 0, id);
        Push(A, 0, OP_HL_DONE, 0, 0);
        break;
    }
    case ST_LAPSE:
        /* no set, S0 or query within QUERY_HOLD_MS of the acceptance: time
           passes; the next admission (or CLAIM, EVENTS start, GET_INFO)
           ends the preparation in its own hold */
        M->Sys = SYS_IDLE;
        ClockTo(M, M->AtS);
        break;
    case ST_DLAPSE:
        /* the other sender sends nothing more after its accepted query */
        M->DSeq = DS_END;
        ClockTo(M, M->AtD);
        break;
    case ST_DEND:
        M->DSeq = DS_END;
        break;
    case ST_RETAIN:
        /* the stack keeps this lower IRP for good */
        M->Retained = (unsigned char)(M->Retained | (1 << S->Who));
        break;
    case ST_HOST: {
        HOST *H = &M->H[S->Who];
        int r;
        unsigned short tails;
        unsigned long dr;
        if (H->Waiting) {
            H->Next++;
            H->Waiting = 0;
            M->R[HostReq(S->Who, H->Next)].Made = 1;
            M->R[HostReq(S->Who, H->Next)].Slot = H->Slot;
            break;
        }
        r = HostReq(S->Who, H->Next);
        if (M->Freed) { Fail("an IRP on an open handle after the free"); break; }
        if (H->Pad && M->ReclaimUsed && M->D.Claim == SC_ACTIVE) H->Epoch = M->D.Epoch;   /* a new handle */
        {
            unsigned long nr = M->D.NextRequest + 1;
            if (!nr) nr = 1;
            M->Map[nr & 15] = (unsigned char)r;
        }
        tails = M->D.Tails;
        /* the lapse is checked in the admission's own hold */
        SlotQueryExpire(&M->D, M->Clock, HOLD_MS);
        dr = SlotAdmit(&M->D, &g_Ops, H->Slot, (void *)(size_t)r, H->Segs, H->Timeout,
                       H->Reserve ? SLOT_BIN : -1, 1, H->Epoch, &Out);
        H->Waiting = 1;
        if (!dr) {
            M->R[r].Refused = 1;
            break;
        }
        if (H->Epoch != M->D.Epoch) Fail("a stale epoch was admitted");
        if (tails >= M->D.TailLimit) Fail("admitted with the completion tails at their bound");
        if (M->Freed) Fail("admitted after the free");
        if (M->Held) Fail("admitted while a request the gate stopped is still out (the quarantine reopened)");
        if (M->QAccepted) Fail("a request admitted between an accepted query and its set");
        if (M->DQAccepted) Fail("a request admitted between an accepted device query and its set");
        M->R[r].Admitted = 1;
        M->Lock = (unsigned char)(1 + S->Who);
        Push(&M->A[S->Who], 0, OP_ADMITCHK, H->Slot, r);
        break;
    }
    case ST_DONE: {
        int s = S->Who, req = M->StackReq[s];
        /* variants: 0 full ok, 1 short ok (1 unit), 2 zero-length ok, 3 failed, 4 failed after 1 unit */
        int ok = S->Var <= 2, shrt = S->Var != 0, units = S->Var == 0 ? 2 : (S->Var == 1 || S->Var == 4) ? 1 : 0;
        int final;
        if (!Touch(M)) break;
        M->InStack[s] = 0;
        M->R[req].LowerSum = (unsigned char)(M->R[req].LowerSum + units);
        final = !(M->D.S[s].State == SS_ACTIVE && ok && !shrt && M->D.S[s].SegsLeft > 0);
        if (final && M->D.S[s].State == SS_ACTIVE && !M->R[req].FirstStop) M->R[req].Final = (unsigned char)(ok ? SO_SUCCESS : SO_ERROR);
        SlotLowerDone(&M->D, &g_Ops, s, ok, shrt, (unsigned long)units, &Out);
        A = Spawn(M);
        Take(M, A, &Out);
        if (g_Sc->Returns) Push(A, 0, OP_LRET, s, req);
        Retire(M, A);
        break;
    }
    case ST_POLLDONE: {
        int s = SLOT_INT, ok = S->Var == 0, w;
        unsigned long wr = M->D.S[SLOT_WAIT].Request;
        int wa = M->D.S[SLOT_WAIT].State == SS_ACTIVE;
        int closed = M->D.PowerDown != 0;
        if (!Touch(M)) break;
        M->InStack[s] = 0;
        if (ok) M->EventsLeft--;
        /* the event's data is queued only with the gate open */
        if (SlotPollAccept(&M->D) != !closed) Fail("a poll's data accepted with the gate closed, or refused with it open");
        SlotPollDone(&M->D, &g_Ops, ok, &Out);
        if (ok && closed) {
            if (wa) Fail("a wait was active with the gate closed");
            /* the driver takes the flag into its queue in a later hold */
            if (!SlotTakeLost(&M->D)) Fail("a poll's data with the gate closed was neither queued nor flagged lost");
        } else if (M->D.Lost) Fail("the lost flag set with the gate open");
        if (ok && wa && wr && !closed) { w = M->Map[wr & 15]; if (!M->R[w].FirstStop && !M->R[w].Final) M->R[w].Final = SO_SUCCESS; }
        A = Spawn(M);
        Take(M, A, &Out);
        if (g_Sc->Returns) Push(A, 0, OP_LRET, s, 0);
        Retire(M, A);
        break;
    }
    case ST_USERCANCEL: {
        MREQ *R = &M->R[S->Who];
        R->Cancels++;
        R->Cancelled = 1;
        if (R->Routine) {
            R->Routine = 0;
            A = Spawn(M);
            Push(A, 0, OP_CR_BEGIN, R->Slot, S->Who);
            Push(A, 0, OP_CR_END, R->Slot, S->Who);
            if (g_Sc->Returns) Push(A, 0, OP_CR_RET, R->Slot, S->Who);
        }
        break;
    }
    case ST_TIMERFIRE: {
        MREQ *R = &M->R[S->Who];
        R->Timer = 2;
        A = Spawn(M);
        Push(A, 0, OP_TM_BEGIN, R->Slot, S->Who);
        Push(A, 0, OP_TM_END, R->Slot, S->Who);
        if (g_Sc->Returns) Push(A, 0, OP_TM_RET, R->Slot, S->Who);
        break;
    }
    case ST_RELEASE: {
        int s;
        int wa[SLOT_COUNT];
        unsigned long wr[SLOT_COUNT];
        M->ReleaseUsed = 1;
        for (s = 0; s < SLOT_COUNT; s++) { wa[s] = M->D.S[s].State == SS_ACTIVE; wr[s] = M->D.S[s].Request; }
        SlotRelease(&M->D, &g_Ops, &Out);
        for (s = 0; s < SLOT_COUNT; s++) NoteStop(M, s, wa[s], wr[s], SO_CANCELLED);
        A = Spawn(M);
        Take(M, A, &Out);
        Retire(M, A);
        break;
    }
    case ST_RECLAIM:
        M->ReclaimUsed = 1;
        if (!SlotClaim(&M->D)) Fail("a free claim could not be taken again");
        break;
    case ST_REMOVE: {
        int s;
        int wa[SLOT_COUNT];
        unsigned long wr[SLOT_COUNT];
        M->RemoveUsed = 1;
        if (!Touch(M)) break;
        if (M->Wedged) {
            if (g_Sc->ExpectWedge) { g_Expected++; M->Halt = 1; break; }   /* the expected outcome: this run ends here */
            Fail("REMOVE after a D3 went down during a system sleep: the stack below does not return it (read under NUSB's EHCI)");
            break;
        }
        for (s = 0; s < SLOT_COUNT; s++) { wa[s] = M->D.S[s].State == SS_ACTIVE; wr[s] = M->D.S[s].Request; }
        SlotRemove(&M->D, &g_Ops, &Out);
        SlotActiveClose(&M->D);             /* and REMOVE's own admission given up in that hold */
        for (s = 0; s < SLOT_COUNT; s++) NoteStop(M, s, wa[s], wr[s], SO_REMOVED);
        A = Spawn(M);
        Take(M, A, &Out);
        if (PowerActors()) Push(A, 0, OP_RM_FWD, 0, 0);
        else M->RemoveFwd = 1;              /* folded: no admission can be out */
        Push(A, 0, OP_DETACH, 0, 0);
        break;
    }
    case ST_SLEEP:
        StartSleep(M, Spawn(M));
        break;
    case ST_OTHER_D: {
        /* another sender's device IRP (S->Who 3: D3, 0: D0, 4: a device
           query below D0), at PASSIVE_LEVEL */
        int id = NewP(M, S->Who == 4 ? PK_DQUERY : S->Who == 3 ? PK_D3 : PK_D0);
        M->DSeq = (unsigned char)(S->Who == 4 ? DS_QUERY : S->Who == 3 ? DS_D3 : DS_D0);
        if (S->Who == 4) M->Lapsed = (unsigned char)(M->Lapsed & ~2);
        A = Spawn(M);
        Push(A, 0, OP_D_ENTER, S->Who, id);
        break;
    }
    case ST_WAKE: {
        int id = NewP(M, PK_S0);
        M->WakeUsed = 1;
        M->Sys = SYS_S0;
        if (M->Published || !SlotActiveEnter(&M->D)) { NotAdmitted(M, id); break; }
        A = Spawn(M);
        Push(A, 0, OP_WAKE_FWD, 0, id);
        Push(A, 0, OP_HL_DONE, 0, 0);
        break;
    }
    case ST_CLOSE:
        if (!Touch(M)) break;
        M->Closed[S->Who] = 1;
        {
            int del = SlotClose(&M->D, &Out);
            A = Spawn(M);
            Take(M, A, &Out);
            if (del) Deleter(A);                    /* this close claimed the deletion */
        }
        Retire(M, A);
        break;
    case ST_STOP: {
        /* An ABORT from a handle of the current claim, or (StopStale) of
           the first claim: a stale one must change nothing at all. */
        unsigned long ep = g_Sc->StopStale ? 1 : M->D.Epoch;
        int cur = M->D.Claim == SC_ACTIVE && ep == M->D.Epoch && !M->D.Removing, done;
        SLOT_DEVICE before = M->D;
        M->StopUsed = 1;
        if (cur) {
            STOPCALL(M, g_Sc->StopSlot, SO_CANCELLED,
                     done = SlotStopEpoch(&M->D, &g_Ops, g_Sc->StopSlot, SO_CANCELLED, ep, &Out));
        } else {
            done = SlotStopEpoch(&M->D, &g_Ops, g_Sc->StopSlot, SO_CANCELLED, ep, &Out);
            if (done || Out.Count || memcmp(&before, &M->D, sizeof(before)))
                Fail("an ABORT of an earlier claim reached the device");
        }
        if (cur && !done) Fail("an ABORT of the current claim was refused");
        A = Spawn(M);
        Take(M, A, &Out);
        Retire(M, A);
        break;
    }
    }
    CheckLimits(M);
}

/* Request r is the one a lower IRP the stack keeps for good belongs to. */
static int Kept(MSTATE *M, int r)
{
    int s;
    for (s = 0; s < SLOT_COUNT; s++)
        if ((M->Retained & (1 << s)) && M->StackReq[s] == r && s != SLOT_INT) return 1;
    return 0;
}

/* The expected hang: every actor that cannot move is REMOVE waiting for
   its rundown, held by a request the stack keeps. */
static int RundownHeld(MSTATE *M)
{
    int i, any = 0;
    if (!M->Retained || M->RemoveReady) return 0;
    for (i = 0; i < MAXACT; i++) {
        if (!M->A[i].Used || !M->A[i].N) continue;
        if (M->A[i].Q[0].Op != OP_DETACH) return 0;
        any = 1;
    }
    return any;
}

static void Final(MSTATE *M)
{
    int i, h;
    if (M->Halt) return;    /* the expected constraint met: counted, not a terminal state of the driver */
    if (g_Sc->ExpectHang && RundownHeld(M)) { g_Expected++; return; }
    for (i = 0; i < MAXACT; i++) if (M->A[i].Used && M->A[i].N) { Fail("an actor is stuck: a lost cancel or a hang"); return; }
    for (h = 0; h < g_Sc->Hosts; h++) {
        HOST *H = &M->H[h];
        int r = HostReq(h, H->Next);
        MREQ *R = &M->R[r];
        if (M->Closed[h] && (!H->Waiting || R->Completed || R->Refused)) continue;
        if (H->Waiting && Kept(M, r)) continue;     /* its call stays pending while the stack keeps it */
        if (!H->Waiting || H->Next + 1 != H->Count || (!R->Completed && !R->Refused)) { Fail("a host never finished"); return; }
    }
    for (i = 1; i <= MAXREQ; i++) {
        if (M->R[i].Admitted && !M->R[i].Completed && !Kept(M, i)) { Fail("an admitted IRP was never completed"); return; }
        if (Kept(M, i) && M->R[i].Completed) { Fail("a request the stack keeps was completed"); return; }
    }
    for (i = 0; i < SLOT_COUNT; i++) {
        if (M->Retained & (1 << i)) {
            /* the quarantine at the end: still the slot's, the gate closed over it */
            if (!M->InStack[i] || M->D.S[i].State == SS_IDLE) { Fail("a kept request dropped from its slot"); return; }
            if (!(M->D.PowerDown & SLOT_PD_DRAIN)) { Fail("the gate open over a request the stack keeps"); return; }
            continue;
        }
        if (M->InStack[i] && !(i == SLOT_INT && M->D.S[i].State == SS_ACTIVE)) { Fail("an IRP left in the stack"); return; }
    }
    /* every power IRP completed exactly once; a set always passed down
       unless it was refused admission; a query's success only from the
       lower drivers (PDone checks that as it happens) */
    for (i = 0; i < M->NP; i++) {
        MPIRP *P = &M->P[i];
        if (!P->Done) { Fail("a power IRP was never completed"); return; }
        if (P->Refused && (P->Fwd || P->Status != 2)) { Fail("a power IRP refused admission was passed down or succeeded"); return; }
        if ((P->Kind == PK_SET || P->Kind == PK_S0 || P->Kind == PK_D3 || P->Kind == PK_D0) && !P->Refused && !P->Fwd)
            { Fail("a set was not passed down"); return; }
    }
    /* nothing kept, no sleep, no failed wake, no removal: the gate must be
       open again (a refusal, a lapse or a drain that left it closed); an
       accepted query whose hold has passed is ended by the next call, so
       it is ended here as that call would */
    {
        SLOT_DEVICE d = M->D;
        SlotQueryExpire(&d, M->Clock, HOLD_MS);
        if (!M->Retained && !M->RemoveUsed && (M->Sys == SYS_IDLE || M->Sys == SYS_REFUSED) && !M->Asleep &&
            !M->D0Failed && !M->S0Failed && d.PowerDown) {
            Fail("the gate stayed closed with no reason standing");
            return;
        }
    }
    if (M->Running || M->D.PowerWorker || M->D.PowerQ) { Fail("the power worker never finished"); return; }
    if (M->LateRunning || M->LateQ || M->LateQueued) { Fail("the late worker never finished"); return; }
    if (SlotActiveOut(&M->D)) { Fail("an admission was never given up"); return; }
    if (M->Freed && !M->Unloaded) { Fail("DriverUnload never ran after the free"); return; }
    if (M->RemoveUsed && !M->Freed) { Fail("a removal never reached its free"); return; }
    if (M->D.Tails) { Fail("a completion tail never finished"); return; }
    if (M->ReleaseUsed && !M->RemoveUsed && !M->ReclaimUsed && !M->Retained && M->D.Claim != SC_FREE) { Fail("a released claim did not end FREE"); return; }
}

/* ---------------------------------------------------------------- the search */

typedef unsigned __int64 U64;
typedef struct ENTRY { unsigned long Hash; unsigned long Index; U64 Paths; } ENTRY;
#define TABSIZE (1UL << 23)
/* The state store, in blocks allocated as needed (the program is linked
   large-address-aware, and no single block need be contiguous with
   another). */
#define BLOCK_BITS 17
#define BLOCK_STATES (1UL << BLOCK_BITS)
#define ARENA_BLOCKS 30
#define ARENA_STATES (ARENA_BLOCKS * BLOCK_STATES)
static ENTRY *g_Tab;
static MSTATE *g_Block[ARENA_BLOCKS];
static MSTATE *Stored(unsigned long i) { return &g_Block[i >> BLOCK_BITS][i & (BLOCK_STATES - 1)]; }
static unsigned long g_Used;

static unsigned long Hash(const void *P, size_t N)
{
    const unsigned char *b = (const unsigned char *)P;
    unsigned long a = 2166136261UL;
    size_t i;
    for (i = 0; i < N; i++) a = (a ^ b[i]) * 16777619UL;
    return a;
}

static ENTRY *Lookup(const MSTATE *M, unsigned long H, int *Found)
{
    unsigned long i = H & (TABSIZE - 1);
    for (;;) {
        ENTRY *E = &g_Tab[i];
        if (!E->Index) { *Found = 0; return E; }
        if (E->Hash == H && !memcmp(Stored(E->Index - 1), M, sizeof(*M))) { *Found = 1; return E; }
        i = (i + 1) & (TABSIZE - 1);
    }
}

static int ActorCmp(const void *a, const void *b) { return memcmp(b, a, sizeof(ACTOR)); }

static void Canonical(MSTATE *M)
{
    int i;
    for (i = MAXHOST; i < MAXACT; i++) if (!M->A[i].Used) memset(&M->A[i], 0, sizeof(ACTOR));
    qsort(&M->A[MAXHOST], MAXACT - MAXHOST, sizeof(ACTOR), ActorCmp);
}

static const char *StepName(int Kind)
{
    static const char *n[] = { "actor step", "host", "stack completes",
        "poll completes", "I/O manager cancel", "timer fires", "release", "new claim", "removal", "ABORT",
        "handle closed", "system sleep", "wake", "another sender's device IRP",
        "standby query", "S0", "the query's lapse", "the stack keeps a stopped request",
        "the device query's lapse", "the other sender sends nothing more" };
    /* every step kind has a name (a failure's trace looks them up) */
    typedef char NamesFit[(sizeof(n) / sizeof(n[0]) == ST_DEND + 1) ? 1 : -1];
    return n[Kind];
}

static U64 Explore(MSTATE *M, int Depth)
{
    STEP S[96];
    int n, i, found;
    unsigned long h;
    U64 paths = 0;
    ENTRY *E;

    Canonical(M);
    h = Hash(M, sizeof(*M));
    E = Lookup(M, h, &found);
    if (found) return E->Paths;
    if (g_Used >= ARENA_STATES) { Fail("state store full: the scenario is too large"); return 0; }
    if (!g_Block[g_Used >> BLOCK_BITS]) {
        g_Block[g_Used >> BLOCK_BITS] = (MSTATE *)malloc(BLOCK_STATES * sizeof(MSTATE));
        if (!g_Block[g_Used >> BLOCK_BITS]) { Fail("state store full: no memory for another block"); return 0; }
    }
    memcpy(Stored(g_Used), M, sizeof(*M));
    E->Hash = h;
    E->Index = ++g_Used;
    g_States++;
    n = Enabled(M, S);
    if (!n) {
        Final(M);
        paths = 1;
    } else {
        for (i = 0; i < n && !g_Fail; i++) {
            MSTATE Next;
            U64 p;
            memcpy(&Next, M, sizeof(Next));
            g_Steps++;
            Apply(&Next, &S[i]);
            /* an actor step names its op (the enum's number) */
            if (g_Fail) { printf("  at depth %d: %s (%d, %d) op %d\n", Depth, StepName(S[i].Kind), S[i].Who, S[i].Var,
                                 S[i].Kind == ST_ACTOR ? M->A[S[i].Who].Q[0].Op : -1); break; }
            p = Explore(&Next, Depth + 1);
            if (g_Fail) printf("  from depth %d: %s (%d, %d) op %d\n", Depth, StepName(S[i].Kind), S[i].Who, S[i].Var,
                               S[i].Kind == ST_ACTOR ? M->A[S[i].Who].Q[0].Op : -1);
            if (paths + p < paths) g_Overflow = 1;
            paths += p;
        }
    }
    E = Lookup(M, h, &found);
    E->Paths = paths;
    return paths;
}

static void Print64(U64 v)
{
    char buf[32];
    int i = 31;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    printf("%s", &buf[i]);
}

#define BIN(n, segs, to)  { SLOT_BIN, 0, n, segs, 0, to, 0, 0, 0 }
#define BOUT(n, segs, to) { SLOT_BOUT, 0, n, segs, 0, to, 0, 0, 0 }
#define WAITH(n, to)      { SLOT_WAIT, 0, n, 1, 0, to, 0, 0, 0 }
#define RESETH(to)        { SLOT_CTL, 0, 1, 1, 1, to, 0, 0, 0 }

#define BINA(n)           { SLOT_BIN, 0, n, 1, 0, 0, 0, 1, 0 }
#define CTLH(n, to)       { SLOT_CTL, 0, n, 1, 0, to, 0, 0, 0 }

static const SCEN g_Scen[] = {
    /* name, hosts, host specs, needscancel, release, remove, stop, stopslot, poll,
       usercancels, timers, failures, reclaim, returns, events, taillimit, start,
       zombie, stopstale, handleless, hlclose, powerqueued, sleep */
    { "bulk IN: one read of 2 segments, three cancels, timer, release, callback returns", 1, { BIN(1, 2, 1) },
      0, 1, 0, 0, 0, 0, 3, 1, 0, 0, 1, 0, 0, 0 },
    { "bulk IN: two reads, every completion kind, cancel, timers, needs-cancel stack, release", 1, { BIN(2, 2, 1) },
      1, 1, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 },
    { "bulk IN: two reads, ABORT, removal and its free, callback returns, needs-cancel stack", 1, { BIN(2, 1, 1) },
      1, 0, 1, 1, SLOT_BIN, 0, 1, 1, 0, 0, 1, 0, 0, 0 },
    { "bulk IN and OUT together, cancel, release, failures", 2, { BIN(1, 1, 0), BOUT(1, 1, 0) },
      1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0 },
    { "bulk IN: one read of 16 segments (the maximum), any completing inline, cancel", 1, { BIN(1, 16, 0) },
      1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0 },
    { "bulk IN: two reads of 2 segments, ABORT, release, needs-cancel stack", 1, { BIN(2, 2, 0) },
      1, 1, 0, 1, SLOT_BIN, 0, 1, 0, 0, 0, 0, 0, 0, 0 },
    { "wait: two waits fed by the interrupt poll, cancel, timers, release", 1, { WAITH(2, 1) },
      1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0 },
    { "wait: one wait, the poll delivering during removal and its free, returns", 1, { WAITH(1, 1) },
      1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0 },
    { "poll: twelve events completing inline, one wait, release", 1, { WAITH(1, 0) },
      1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 12, 0, 0 },
    { "RESET_PIPE reserving bulk IN against a read, cancel, timers, removal, the handles' closes", 2, { RESETH(1), BIN(1, 1, 0) },
      1, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { "RESET_PIPE reserving bulk IN against two reads (reuse across the reservation's release), cancel, removal", 2,
      { RESETH(1), BIN(2, 1, 0) }, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0 },
    { "RESET_PIPE reserving bulk IN against two reads (reuse across the reservation's release), timers, removal", 2,
      { RESETH(1), BIN(2, 1, 0) }, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
    { "claim turnover: release, a new claim; stale reads refused, a new handle's reads served", 2,
      { BIN(1, 1, 0), BINA(2) }, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0 },
    { "completion tails at their bound (limit 1): re-entrant admission refused", 1, { BIN(3, 1, 0) },
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0 },
    { "admission counter wrapping past FFFFFFFFh", 1, { BIN(3, 1, 0) },
      0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xFFFFFFFEUL },
    { "zombie: two handles kept open across the removal, closing before or after its detach, one deleter", 2,
      { BIN(1, 1, 0), BOUT(1, 1, 0) }, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1 },
    { "claim turnover with an ABORT from the first claim's handle at any time: it reaches nothing later", 2,
      { BIN(1, 1, 0), BINA(2) }, 1, 1, 0, 1, SLOT_BIN, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1 },
    { "zombie: a power IRP racing the removal, the detach and the last close: admitted and forwarded inline, or refused", 2,
      { BIN(1, 1, 0), BOUT(1, 1, 0) }, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1 },
    { "zombie: a power IRP, admitted or refused, whose completion closes the last handle and deletes in its own call chain", 2,
      { BIN(1, 1, 0), BOUT(1, 1, 0) }, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1 },
    { "zombie: two power IRPs above PASSIVE_LEVEL: admitted ones to the power worker, refused ones to the late worker, one completion closing the last handle (a read and an idle handle)", 2,
      { BIN(1, 1, 0), BOUT(0, 1, 0) }, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 1, 1 },
    { "system sleep and wake against two reads of 2 segments, cancel, needs-cancel stack, release", 1, { BIN(2, 2, 0) },
      1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },
    { "system sleep and wake against the poll and two waits, removal and its free", 1, { WAITH(2, 0) },
      1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },
    { "system sleep and wake against bulk IN and OUT, removal, the handles' closes", 2, { BIN(1, 1, 0), BOUT(1, 1, 0) },
      1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },
    { "zombie: two power IRPs above PASSIVE_LEVEL, refused ones to the late worker, the last close a host's (DriverUnload on another thread; a read and an idle handle)", 2,
      { BIN(1, 1, 0), BOUT(0, 1, 0) }, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0, 1 },
    { "another sender's D3, then its D0 failing: the gate stays closed for the device, two reads", 1, { BIN(2, 1, 0) },
      1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1 },
    { "system sleep and wake with another sender's D3 and D0 at any time, two reads", 1, { BIN(2, 1, 0) },
      1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1 },
    { "the sleep reached synchronously inside a completion tail (the assumption of 5.4): the set's bound ends the wait, the IRP goes down with that tail out, the tail finishes after", 1, { BIN(1, 1, 0) },
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0 },
    { "EXPECTED STACK CONSTRAINT: a system sleep, another sender's D3 and D0 at any time, removal: the driver forwards that D3, so a REMOVE after it meets the constraint inferred from the NUSB runs", 1,
      { BIN(1, 1, 0) }, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0 },
    { "a system sleep, another sender's D3 and D0 at any time, removal and its free, the constraint left off for that D3 (ME under xHCI98: a REMOVE after it returned, read once, the removal after the wake)", 1,
      { BIN(1, 1, 0) }, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1 },
    /* The standby rework (2026-11-17). The fields after the hosts, in
       groups of five: needscancel, release, remove, stop, stopslot; poll,
       usercancels, timers, failures, reclaim; returns, events, taillimit,
       start, zombie; stopstale, handleless, hlclose, powerqueued, sleep;
       failwake, tailsleep, expecthang, otherd, expectwedge; othernowedge;
       query, retain, otherveto, lowerveto, reaffirm. */
    { "standby query: two reads of 2 segments, cancel, needs-cancel stack; the query drains and is accepted only on quiet, nothing is admitted before its set, the wake", 1,
      { BIN(2, 2, 0) }, 1, 0, 0, 0, 0,  0, 1, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  1, 0, 0, 0, 0 },
    { "standby query with a stack that keeps a stopped request for good or returns it at any time, a read and the poll with a timed wait: refused unless quiet, S0 after a refusal, the gate closed until the last returns, the poll's late data flagged lost", 2,
      { BIN(1, 1, 0), WAITH(1, 1) }, 1, 0, 0, 0, 0,  1, 0, 1, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  1, 1, 0, 0, 1 },
    { "set without preparation (no query, or a critical sleep) with a stack that keeps a stopped request or returns it late, bulk IN and a bulk OUT of 2 segments: the bounded drain, the set down once, the quarantine across the wake", 2,
      { BIN(1, 1, 0), BOUT(1, 2, 0) }, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 1,  0, 0, 0, 0, 0,  0,  0, 1, 0, 0, 0 },
    { "query rollback: two queries, each accepted then another device's refusal (S0) or no set (the lapse), or refused by the lower drivers (S0 after), two reads", 1,
      { BIN(2, 1, 0) }, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  2, 0, 1, 1, 1 },
    { "removal during a query's preparation, a read, needs-cancel stack: REMOVE waits for the query's admission, which decides within its bound; then the free", 1,
      { BIN(1, 1, 0) }, 1, 0, 1, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  1, 0, 0, 0, 0 },
    { "EXPECTED HANG: a query, a stack that keeps a stopped read for good, removal: REMOVE's rundown keeps its requirement (nothing freed on a time-out)", 1,
      { BIN(1, 1, 0) }, 1, 0, 1, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 1, 0, 0,  0,  1, 1, 0, 0, 0 },
    { "another sender's D3 and D0 with a stack that keeps a stopped request or returns it late, two reads: the bounded drain, D3 down once, its D0 does not release what is out", 1,
      { BIN(2, 1, 0) }, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 1, 0,  0,  0, 1, 0, 0, 0 },
    { "bulk OUT: two writes of 2 segments across a standby (query, set, wake), the stack keeping a stopped write or returning it late: never sent again", 1,
      { BOUT(2, 2, 0) }, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  1, 1, 0, 0, 0 },
    /* The review of the standby rework (2026-11-17): the fields after the
       hosts as above, then queryqueued, dquery, lateset, fails0. */
    { "submission: a set without preparation against a read of 2 segments and the poll with a timed wait, a stack that keeps or returns late: an SA_SUBMIT not committed when the gate closes is never sent, one committed reaches the stack before the set", 2,
      { BIN(1, 2, 0), WAITH(1, 1) }, 1, 0, 0, 0, 0,  1, 0, 1, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 1,  0, 0, 0, 0, 0,  0,  0, 1, 0, 0, 0,  0, 0, 0, 0 },
    { "query repeated from its acceptance, then another device's refusal (S0) or no set and the lapse, new traffic admitted after the lapse, then a delayed set: two reads", 1,
      { BIN(2, 1, 0) }, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  2, 0, 1, 0, 0,  0, 0, 1, 0 },
    { "query refused, then a committed set and no S0, with a stack that keeps a stopped request or returns it late, a read of 2 segments: the set's bounded drain, down once, the quarantine across the wake", 1,
      { BIN(1, 2, 0) }, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  1, 1, 0, 0, 0,  0, 0, 1, 0 },
    { "failed S0: after a standby the wake's S0 may fail (QUERY ends, SYS stands), and so may the S0 after a refusal or another device's refusal, two reads", 1,
      { BIN(2, 1, 0) }, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  1, 0, 1, 1, 1,  0, 0, 0, 1 },
    { "query above PASSIVE_LEVEL, on the power worker: prepared, refused or accepted (the lower drivers may refuse) and completed there; with removal, one refused admission completed by the late hand-off with a failure; a read", 1,
      { BIN(1, 1, 0) }, 1, 0, 1, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  1, 0, 0, 1, 0,  1, 0, 0, 0 },
    { "query on the power worker with a stack that keeps a stopped read or returns it late, S0 after a refusal: the worker's wait is bounded and its refusal leaves DRAIN", 1,
      { BIN(1, 1, 0) }, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  1, 1, 0, 0, 1,  1, 0, 0, 0 },
    { "standby query against a RESET_PIPE reserving bulk IN, and a read, the stack keeping a stopped request or returning it late, S0 after a refusal", 2,
      { RESETH(0), BIN(1, 1, 0) }, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  1, 1, 0, 0, 1,  0, 0, 0, 0 },
    { "standby query against a class request and a read, the stack keeping a stopped request or returning it late, then a committed set after a refusal", 2,
      { CTLH(1, 0), BIN(1, 1, 0) }, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  1, 1, 0, 0, 0,  0, 0, 1, 0 },
    { "device query from another sender, a stack that keeps a stopped read or returns it late, the lower drivers may refuse: drained and accepted only on quiet, else refused; then its D3, its D0 (rollback) or nothing and the lapse; after a refusal its D3 anyway or nothing", 1,
      { BIN(1, 1, 0) }, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  0, 1, 0, 1, 0,  0, 1, 0, 0 },
    { "device query from another sender alongside a system standby query, another device's refusal and the S0 after a refusal: no system IRP ends the device query, no device IRP the system's, a read", 1,
      { BIN(1, 1, 0) }, 1, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  1, 0, 1, 0, 1,  0, 1, 0, 0 },
    { "device query with removal at any time, a read: an admitted query keeps its admission through its answer, REMOVE waits; one refused admission completed with a failure", 1,
      { BIN(1, 1, 0) }, 1, 0, 1, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  0, 0, 0, 0, 0,  0, 1, 0, 0 },
};


/* Direct checks of slot.c's arithmetic, which the exploration reaches only
   at the values its scenarios pass: the lapse's elapsed time across the
   tick count's wrap and at its boundary, the counters' wrap (modulo 65536,
   a reader comparing with its own baseline), and GET_INFO's mask while
   only completion tails are out. */
static int SelfTest(void)
{
    static const struct { unsigned long At, Now; int Lapsed; } t[] = {
        { 1000UL, 1000UL + HOLD_MS - 1, 0 },
        { 1000UL, 1000UL + HOLD_MS, 1 },
        { 0xFFFFA000UL, 0xFFFFA000UL + 1, 0 },                 /* At + HOLD_MS wraps; Now does not */
        { 0xFFFFA000UL, 0x0000152FUL, 0 },       /* At + HOLD_MS - 1, wrapped */
        { 0xFFFFA000UL, 0x00001530UL, 1 },       /* At + HOLD_MS, wrapped */
        { 0xFFFFFFFFUL, 0x0000752FUL, 1 },       /* At + HOLD_MS, wrapped */
        { 0x00000010UL, 0x00000010UL + 0x80000000UL, 1 },      /* long after */
    };
    SLOT_DEVICE D;
    SLOT_ACTIONS sa;
    int i, bad = 0, which;
    for (i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
        for (which = 0; which < 2; which++) {
            int got;
            SlotInit(&D);
            if (which == SLOT_QD) { D.DQuery = SLOT_Q_ACCEPTED; D.DQueryAt = t[i].At; D.PowerDown = SLOT_PD_DQUERY; }
            else { D.Query = SLOT_Q_ACCEPTED; D.QueryAt = t[i].At; D.PowerDown = SLOT_PD_QUERY; }
            got = SlotQueryExpire(&D, t[i].Now, HOLD_MS) != 0;
            if (got != t[i].Lapsed || (got && D.PowerDown)) {
                printf("SELFTEST FAIL: lapse at %08lXh from %08lXh (%s query): %d\n", t[i].Now, t[i].At,
                       which ? "device" : "system", got);
                bad++;
            }
        }
    }
    /* a refusal counted modulo 65536, with what was out */
    SlotInit(&D);
    D.Vetoes = 0xFFFF;
    D.Query = SLOT_Q_PREPARING;
    D.PowerDown = SLOT_PD_QUERY | SLOT_PD_DRAIN;
    D.Tails = 1;
    if (SlotQueryDecide(&D, SLOT_QS) || D.Vetoes != 0 || D.HeldAtVeto != SLOT_HELD_TAILS || D.Query != SLOT_Q_NONE ||
        D.PowerDown != SLOT_PD_DRAIN) {
        printf("SELFTEST FAIL: the refusal's count past FFFFh, or its mask with only a tail out\n");
        bad++;
    }
    /* a forced set counted modulo 65536; with nothing out, not counted */
    SlotInit(&D);
    D.Forced = 0xFFFF;
    D.PowerDown = SLOT_PD_SYS | SLOT_PD_DRAIN;
    D.Tails = 1;
    if (SlotSetForward(&D) != SLOT_HELD_TAILS || D.Forced != 0) { printf("SELFTEST FAIL: the forced count past FFFFh\n"); bad++; }
    D.Tails = 0;
    D.PowerDown = SLOT_PD_SYS;
    if (SlotSetForward(&D) != 0 || D.Forced != 0) { printf("SELFTEST FAIL: a set with nothing out counted as forced\n"); bad++; }
    /* the reasons: a system S0 does not end a device query, a D0 not a system one */
    SlotInit(&D);
    D.Query = SLOT_Q_ACCEPTED; D.DQuery = SLOT_Q_ACCEPTED; D.PowerDown = SLOT_PD_QUERY | SLOT_PD_DQUERY;
    SlotPowerUp(&D, SLOT_PD_SYS | SLOT_PD_QUERY);
    if (D.DQuery != SLOT_Q_ACCEPTED || D.PowerDown != SLOT_PD_DQUERY) { printf("SELFTEST FAIL: S0 ended the device query\n"); bad++; }
    SlotPowerUp(&D, SLOT_PD_DEV | SLOT_PD_DQUERY);
    if (D.PowerDown) { printf("SELFTEST FAIL: D0 left the device query\n"); bad++; }
    /* an accepted query's own admission: taken at the decision, given up
       by its answer whatever the query's state by then (a set that came
       first, which the power manager's order keeps out of the exploration) */
    SlotInit(&D);
    D.Active = 1;
    D.Query = SLOT_Q_PREPARING; D.PowerDown = SLOT_PD_QUERY;
    if (!SlotQueryDecide(&D, SLOT_QS) || D.Active != 2) { printf("SELFTEST FAIL: an accepted query took no admission\n"); bad++; }
    sa.Count = 0;
    SlotPowerDown(&D, &g_Ops, SLOT_PD_SYS, &sa);
    SlotQueryEnd(&D, SLOT_QS, 1, 0);
    if (D.Active != 1 || D.Query != SLOT_Q_NONE) { printf("SELFTEST FAIL: a late answer kept the query's admission or revived it\n"); bad++; }
    D.Active = 0xFFFF;
    D.Query = SLOT_Q_PREPARING; D.PowerDown = SLOT_PD_QUERY;
    if (SlotQueryDecide(&D, SLOT_QS) || D.Active != 0xFFFF) { printf("SELFTEST FAIL: a query accepted past the admission count's limit\n"); bad++; }
    printf("%s self-test: the lapse across the tick count's wrap, the counters' wrap, GET_INFO's tails bit, the reasons apart, the query's own admission\n",
           bad ? "FAIL" : "PASS");
    return bad;
}

int main(int argc, char **argv)
{
    int i, h, failed = 0, anyOverflow = 0, ran = 0;
    U64 total = 0;
    unsigned long states = 0, steps = 0;
    const char *only = argc > 1 ? argv[1] : 0;     /* run only the scenarios whose name holds this */

    g_Tab = (ENTRY *)calloc(TABSIZE, sizeof(ENTRY));
    if (!g_Tab) { printf("no memory\n"); return 2; }
    printf("slotmodel: every interleaving of sys/slot.c's events (docs/design/mtp.md 5.4)\n");
    printf("state size %u bytes; states stored whole, compared byte for byte\n", (unsigned)sizeof(MSTATE));
    if (SelfTest()) failed++;
    for (i = 0; i < (int)(sizeof(g_Scen) / sizeof(g_Scen[0])); i++) {
        MSTATE M;
        U64 paths;
        const SCEN *Sc = &g_Scen[i];
        unsigned long epoch;
        SLOT_ACTIONS Out;
        if (only && !strstr(Sc->Name, only)) continue;
        ran++;
        memset(&M, 0, sizeof(M));
        SlotInit(&M.D);
        M.D.NextRequest = Sc->Start;
        M.Clock = CLOCK0;
        g_Sc = Sc;
        g_Fail = 0;
        epoch = SlotClaim(&M.D);
        if (Sc->TailLimit) M.D.TailLimit = (unsigned short)Sc->TailLimit;
        for (h = 0; h < Sc->Hosts; h++) {
            M.H[h] = Sc->H[h];
            M.H[h].Epoch = epoch;
            M.A[h].Used = 1;
            M.R[HostReq(h, 0)].Made = 1;
            M.R[HostReq(h, 0)].Slot = Sc->H[h].Slot;
            if (!SlotOpen(&M.D)) Fail("a handle could not be opened");
        }
        g_Ops.Ctx = &M;
        M.QueriesLeft = (unsigned char)Sc->Query;
        for (h = 0; h < Sc->Handleless; h++) {
            ACTOR *X = Spawn(&M);
            Push(X, 0, OP_HL_ENTER, 0, NewP(&M, PK_HL));
        }
        if (Sc->Poll) {
            /* The poll is started at claim and its first IRP is with the stack. */
            Out.Count = 0;
            M.Map[(M.D.NextRequest + 1) & 15] = 0;
            SlotPollStart(&M.D, &g_Ops, epoch, &Out);
            M.InStack[SLOT_INT] = 1;
            M.D.S[SLOT_INT].Pins = 0;
            M.D.S[SLOT_INT].Submitting = 0;
            M.EventsLeft = (unsigned char)(Sc->Events ? Sc->Events : 2);
        }
        g_States = g_Steps = g_Fail = g_Expected = 0;
        g_Overflow = 0;
        g_Why[0] = 0;
        memset(g_Tab, 0, TABSIZE * sizeof(ENTRY));
        g_Used = 0;
        paths = Explore(&M, 0);
        if (Sc->ExpectHang || Sc->ExpectWedge) {
            /* the expected outcome must be reached, and every other branch
               must pass: the exploration does not stop at it */
            int met = !g_Fail && g_Expected;
            printf("%s %s\n     states %lu, steps %lu, complete interleavings ", met ? "PASS" : "FAIL", Sc->Name, g_States, g_Steps);
            Print64(paths);
            printf("%s; the expected outcome in %lu of its terminal states, every other run passing\n",
                   g_Overflow ? " (count overflowed 64 bits: a lower bound)" : "", g_Expected);
            if (!met) { printf("     first failure: %s\n", g_Fail ? g_Why : "the expected outcome was never reached"); failed++; }
        } else {
            printf("%s %s\n     states %lu, steps %lu, complete interleavings ", g_Fail ? "FAIL" : "PASS",
                   Sc->Name, g_States, g_Steps);
            Print64(paths);
            printf("%s\n", g_Overflow ? " (count overflowed 64 bits: a lower bound)" : "");
            if (g_Fail) { printf("     first failure: %s\n", g_Why); failed++; }
        }
        if (total + paths < total) g_Overflow = 1;
        if (g_Overflow) anyOverflow = 1;
        total += paths;
        states += g_States;
        steps += g_Steps;
    }
    printf("total: %d scenarios, %lu states, %lu steps, ", ran, states, steps);
    if (anyOverflow) printf("more than ");
    Print64(total);
    printf(" complete interleavings%s: %s\n", anyOverflow ? " (a count overflowed 64 bits)" : "",
           failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
