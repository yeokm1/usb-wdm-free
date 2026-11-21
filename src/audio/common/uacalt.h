/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * The kept alternate setting of one streaming interface as a pure state
 * machine (docs/design/uac2.md section 29.4): the linger, the hold, the
 * failed and unknown states, adoption, the deadlines, the alt thread's
 * scan, and the normalisation of the four settings (29.8). No Windows
 * types and no kernel calls: the caller supplies the time, the gate's
 * state and the outcome of each request, and carries out the actions
 * returned. Nothing in a driver uses this yet.
 *
 * The adapter's contracts (29.12):
 *
 * - Time. UAC_ALT_TIME is a non-wrapping 64-bit millisecond clock: the
 *   adapter derives it from the full 64-bit interrupt time (100 ns since
 *   boot) and never truncates it to 32 bits (a 32-bit millisecond clock
 *   wraps after 49.7 days). 2^64 ms is about 584 million years, so the
 *   module does not handle wrap: times must stay below UAC_ALT_TIME_LIMIT.
 *   Deadline arithmetic saturates at UAC_ALT_NEVER, which means "never";
 *   a time near the limit therefore yields a deadline that never comes.
 *   That saturation is the chosen, tested limitation.
 * - Failure. The gate (UAC_ALT_GATE.Failed) is the authority for the
 *   device's permanent failure. A record's UAC_ALT_FAILED is kept against
 *   late completions too, but every action that sends, and the start's
 *   commit, refuse on the gate alone: a successful completion arriving
 *   after the failure never authorises another request or a start.
 * - Ownership. UAC_ALT_REC.Gen is bookkeeping (it counts changes of state,
 *   owner and retry time); no action is validated by it. Correctness rests
 *   on the caller owning the record (UacAltClaim) from the decision,
 *   across its wait and its request, to the completion it applies, all
 *   under the adapter's lock where the record is read or written.
 * - Geometry. On a successful selection the adapter passes what the pipes
 *   actually retain: the MaximumTransferSize asked and the
 *   MaximumPacketSize returned, per pipe; adoption compares a new stream's
 *   needs against those, not against the first stream's needs.
 * - The start's commit. UacAltCommit is only the predicate; clearing the
 *   stream's stop mark under the gate lock and then the stream lock, the
 *   check after the worker is ready and the one in Run (29.5.7) are the
 *   adapter's.
 * - Conflicts. UacAltConflict classifies a mask the walk supplies, of at
 *   most UAC_ALT_MAX_RECORDS records; discovering the conflict, keeping
 *   the candidate, the eviction round and the second walk (29.4.7) are the
 *   adapter's.
 */
#ifndef UACALT_H
#define UACALT_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
typedef unsigned __int64 UAC_ALT_TIME;      /* milliseconds, monotonic, 64-bit */
#else
typedef unsigned long long UAC_ALT_TIME;
#endif
#define UAC_ALT_NEVER (~(UAC_ALT_TIME)0)
#define UAC_ALT_TIME_LIMIT (UAC_ALT_NEVER >> 1)  /* times stay below this */

#define UAC_ALT_MAX_RECORDS 32U    /* the conflict mask's width */

/* ---- The settings (29.8). 0xFFFFFFFF is what the registry reader returns
 * for a value missing or unusable: the default, never the top. ---- */
#define UAC_ALT_RAW_NONE        0xFFFFFFFFUL
#define UAC_LINGER_DEFAULT_MS   2000UL
#define UAC_LINGER_MAX_MS       10000UL
#define UAC_HOLD_DEFAULT_MS     200UL
#define UAC_HOLD_MAX_MS         1000UL
#define UAC_PROBE_DEFAULT_MS    0UL      /* provisional, O8: off */
#define UAC_PROBE_MIN_MS        2000UL
#define UAC_PROBE_MAX_MS        60000UL
#define UAC_FAIL_DEFAULT        1U

unsigned long UacAltLingerMs(unsigned long raw);  /* 0..10000, clamped */
unsigned long UacAltHoldMs(unsigned long raw);    /* 0..1000, clamped */
unsigned long UacAltProbeMs(unsigned long raw);   /* 0, or 2000..60000; 1..1999 -> 2000 */
unsigned int UacAltFailOn(unsigned long raw);     /* 0 off, 1 on, anything else the default */

/* The hold's direction is not decided (O2): a parameter. */
#define UAC_HOLD_TO_ZERO 0U   /* only SET_INTERFACE(0) waits for the hold */
#define UAC_HOLD_BOTH    1U   /* SET_INTERFACE(N) from Idle or Unknown waits too */

/* One RUN's snapshot of the linger and the hold (29.8). */
typedef struct UAC_ALT_SNAP {
    unsigned long LingerMs, HoldMs;
    unsigned int HoldDir;
} UAC_ALT_SNAP;

void UacAltSnap(UAC_ALT_SNAP *snap, unsigned long lingerRaw, unsigned long holdRaw,
                unsigned int holdDir);

/* ---- The gate (29.5.5), as the machine reads it. ---- */
#define UAC_ADMIT_OPEN       0U
#define UAC_ADMIT_QUIESCING  1U   /* PnP stop: only the PnP caller ends records */
#define UAC_ADMIT_CLOSED     2U   /* surprise removal or remove: nothing sent */
#define UAC_ADMIT_BELOW_D0   3U   /* power change: only the power caller ends records */

typedef struct UAC_ALT_GATE {
    unsigned int Admit;
    unsigned int Failed;      /* permanent: the authority (see above) */
    unsigned long Epoch;
} UAC_ALT_GATE;

/* ---- The record (29.4.1, 29.4.2). ---- */
#define UAC_ALT_IDLE      0U
#define UAC_ALT_ACTIVE    1U
#define UAC_ALT_LINGERING 2U
#define UAC_ALT_UNKNOWN   3U
#define UAC_ALT_FAILED    4U   /* the device failed: no transition sends */

#define UAC_OWN_NONE   0U
#define UAC_OWN_STREAM 1U   /* the stream the record is Active for */
#define UAC_OWN_THREAD 2U
#define UAC_OWN_START  3U   /* a starting caller: its own record or an evicted one */
#define UAC_OWN_POWER  4U
#define UAC_OWN_PNP    5U

/* A pipe: a transfer size and a packet size. As a stream's need, what it
 * will submit; as what a selection kept, the MaximumTransferSize asked and
 * the MaximumPacketSize the stack returned. */
typedef struct UAC_ALT_PIPE {
    unsigned long Transfer, Packet;
} UAC_ALT_PIPE;

typedef struct UAC_ALT_GEOM {
    unsigned int HasFeedback;         /* a feedback or synch pipe */
    UAC_ALT_PIPE Data, Feedback;
} UAC_ALT_GEOM;

/* What a start wants. */
typedef struct UAC_ALT_WANT {
    unsigned int Alt, Terminal;
    unsigned long Rate;
    UAC_ALT_GEOM Need;
    UAC_ALT_SNAP Snap;
} UAC_ALT_WANT;

/* The retry after a SET_INTERFACE(0) that could not be submitted (29.4.6):
 * 100 ms, doubling to 1 s while it keeps failing. */
#define UAC_ALT_RETRY_MIN_MS 100UL
#define UAC_ALT_RETRY_MAX_MS 1000UL

typedef struct UAC_ALT_REC {
    unsigned int State, Owner, Adoptable;
    unsigned int Alt, Terminal;
    unsigned long Rate;
    UAC_ALT_GEOM Kept;                /* what the selection's pipes retain */
    UAC_ALT_SNAP Snap;
    unsigned long Epoch;              /* the gate's epoch at the selection */
    unsigned long Gen;                /* bookkeeping only (see above) */
    UAC_ALT_TIME AttemptAt;           /* last submitted SELECT_INTERFACE completed */
    UAC_ALT_TIME StopAt, Deadline;    /* set once at a hand-back */
    UAC_ALT_TIME SavedDeadline;       /* the deadline before an adoption */
    UAC_ALT_TIME RetryAt;             /* not runnable before this (0: none) */
    unsigned long RetryMs;            /* the last retry interval (0: none) */
    unsigned int Adopted;             /* Active by adoption (no transfer yet) */
} UAC_ALT_REC;

/* SELECT_CONFIGURATION completed at `now`: every interface at alternate 0. */
void UacAltInit(UAC_ALT_REC *r, UAC_ALT_TIME now);

/* ---- Actions returned. ---- */
#define UAC_ACT_NONE       0   /* nothing to do, nothing sent */
#define UAC_ACT_REFUSE     1   /* STATUS_DEVICE_NOT_READY: the gate or the failure */
#define UAC_ACT_BUSY       2   /* another actor owns the record: wait or skip */
#define UAC_ACT_ADOPT      3   /* start inside the linger: nothing sent */
#define UAC_ACT_SELECT     4   /* wait until *at, then SET_INTERFACE(N) */
#define UAC_ACT_ZERO       5   /* wait until *at, then SET_INTERFACE(0) */
#define UAC_ACT_LINGER     6   /* the record lingers until its deadline */
#define UAC_ACT_DISCARD    7   /* removal: nothing sent, the record goes */
#define UAC_ACT_RUNNING    8   /* a start while running: nothing (as before) */

/* The outcome of a SET_INTERFACE (29.4.6). */
#define UAC_SEL_OK          0  /* succeeded, pipes fit */
#define UAC_SEL_BAD_PIPES   1  /* succeeded, but the pipes do not fit */
#define UAC_SEL_REFUSED     2  /* submitted, refused by the stack or the device */
#define UAC_SEL_UNSUBMITTED 3  /* never submitted: allocation or the gate */
#define UAC_SEL_EXPIRED     4  /* its 5 s deadline expired: the device failed */

/* The outcome of an expired request: the failed state with the failure
 * setting on, that one request's failure with it off (29.8). */
int UacAltExpiry(unsigned int failOn);

/* A start (29.4.4, 29.4.5). The caller must own the record (UAC_OWN_START)
 * from UacAltClaim. Returns UAC_ACT_REFUSE, UAC_ACT_RUNNING, UAC_ACT_ADOPT,
 * UAC_ACT_SELECT (from Idle or Unknown; *at is when, the symmetric hold
 * included) or UAC_ACT_ZERO (a lingering record that cannot be adopted is
 * ended first; call UacAltZeroDone, then UacAltStart again). */
int UacAltStart(UAC_ALT_REC *r, const UAC_ALT_GATE *g, const UAC_ALT_WANT *w,
                UAC_ALT_TIME now, UAC_ALT_TIME *at);

/* The SET_INTERFACE(N) of a start completed (or was never submitted).
 * `kept` is what the pipes retain (NULL: the want's needs). A record
 * already failed stays failed. Returns the new state. */
unsigned int UacAltSelectDone(UAC_ALT_REC *r, const UAC_ALT_WANT *w, const UAC_ALT_GEOM *kept,
                              int outcome, UAC_ALT_TIME now);

/* A start failed after its selection or adoption, before any transfer:
 * an adopted record lingers again with its earlier deadline; a fresh one
 * lingers not adoptable until AttemptAt + H. */
void UacAltStartFailed(UAC_ALT_REC *r, UAC_ALT_TIME now);

/* The start's commit predicate (29.5.7): 1 if the gate still admits this
 * record's start (not failed, open, the same epoch); 0: the start fails. */
int UacAltCommit(const UAC_ALT_REC *r, const UAC_ALT_GATE *g);

/* A stream stops, closes, or is drained (29.4.5): only an Active record's
 * first hand-back acts. `clean`: the stream ended clean (29.4.4). Returns
 * UAC_ACT_NONE (no Active record: repeated stops), UAC_ACT_ZERO (the
 * deadline has passed: the stopping caller selects alternate 0 now; *at
 * is now), UAC_ACT_LINGER (*at the deadline; the record stays Lingering,
 * also when a power or PnP caller will end it), UAC_ACT_DISCARD (the gate
 * is closed: nothing sent) or UAC_ACT_REFUSE (failed: nothing sent). */
int UacAltHandBack(UAC_ALT_REC *r, const UAC_ALT_GATE *g, int clean, UAC_ALT_TIME now,
                   UAC_ALT_TIME *at);

/* A forced end of a lingering record by `owner` (the alt thread at its
 * deadline, a start's eviction, the power or the PnP path): UAC_ACT_ZERO
 * with *at = AttemptAt + H, UAC_ACT_REFUSE when the gate does not admit
 * that owner, UAC_ACT_NONE when there is nothing to end (yet). */
int UacAltEnd(UAC_ALT_REC *r, const UAC_ALT_GATE *g, unsigned int owner, UAC_ALT_TIME now,
              UAC_ALT_TIME *at);

/* SET_INTERFACE(0) completed (or was never submitted: the record and
 * AttemptAt stay as they are, and RetryAt holds it back, 29.4.6). A
 * record already failed stays failed. Returns the state. */
unsigned int UacAltZeroDone(UAC_ALT_REC *r, int outcome, UAC_ALT_TIME now);

/* The device failed (the expiry hook): no transition sends from now on. */
void UacAltFail(UAC_ALT_REC *r);

/* Surprise removal or remove: nothing sent, the record is discarded. */
int UacAltRemove(UAC_ALT_REC *r);

/* Ownership (29.5.3), under the gate lock in a driver. UacAltClaim takes
 * the record for `owner` if no one owns it; for the alt thread it also
 * checks, in the same step, that the record is runnable (29.5.4). Returns
 * 1 if taken, 0 if not. UacAltRelease gives it back. */
int UacAltClaim(UAC_ALT_REC *r, const UAC_ALT_GATE *g, unsigned int owner, UAC_ALT_TIME now);
void UacAltRelease(UAC_ALT_REC *r);

/* The alt thread's scan (29.5.4): 1 if the record is runnable now; else 0,
 * and *next (if not NULL) lowered to the time it becomes runnable when it
 * is pending, left alone when it is owned or held back by the gate. */
int UacAltRunnable(const UAC_ALT_REC *r, const UAC_ALT_GATE *g, UAC_ALT_TIME now,
                   UAC_ALT_TIME *next);
/* Over n records: the index of the first runnable one, or -1; *next the
 * earliest pending time, UAC_ALT_NEVER when none (wait untimed). */
int UacAltScan(const UAC_ALT_REC *r, unsigned int n, const UAC_ALT_GATE *g, UAC_ALT_TIME now,
               UAC_ALT_TIME *next);

/* A clock path in conflict with claims of the records whose indices are
 * set in `mask` (29.4.7; n at most UAC_ALT_MAX_RECORDS): UAC_CLAIM_FREE
 * (mask 0), UAC_CLAIM_LINGERING (all of them Lingering: a candidate for
 * eviction) or UAC_CLAIM_ACTIVE (any other holder, or a bit past n). */
#define UAC_CLAIM_FREE      0
#define UAC_CLAIM_LINGERING 1
#define UAC_CLAIM_ACTIVE    2
int UacAltConflict(const UAC_ALT_REC *r, unsigned int n, unsigned long mask);

#ifdef __cplusplus
}
#endif
#endif
