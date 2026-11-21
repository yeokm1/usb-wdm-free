/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * docs/design/uac2.md 29.5.7 and 29.9: the failed state's decisions that
 * need no kernel call, shared by uacusb.c and uacwdm.cpp and run by the
 * host test (uac2\failtest.c). Independent of Windows types.
 */
#ifndef UACGATE_H
#define UACGATE_H

/* ---- Gate-aware acquisition (29.5.7). One try is a timed wait for the
 * mutex (100 ms in the driver) that answers nonzero only when it acquired
 * it: STATUS_TIMEOUT is also a success status, so only STATUS_SUCCESS
 * counts. The gate is read before every try and once more after the
 * acquisition, so a failure published while the caller waited never lets
 * it proceed; a refusal after the acquisition releases what it took.
 * Returns 1 acquired (the gate not failed), 0 refused (nothing held). ---- */
typedef int (*UAC_GATE_FAILED_FN)(void *context);
typedef int (*UAC_GATE_TRY_FN)(void *context);
typedef void (*UAC_GATE_RELEASE_FN)(void *context);
static int UacGateAcquire(void *context, UAC_GATE_FAILED_FN failed, UAC_GATE_TRY_FN tryWait,
                          UAC_GATE_RELEASE_FN release)
{
    for (;;) {
        if (failed(context)) return 0;
        if (tryWait(context)) break;
    }
    if (failed(context)) {
        release(context);
        return 0;
    }
    return 1;
}

/* ---- The expired request's final status and lateness (29.9). ---- */
#ifdef _MSC_VER
typedef unsigned __int64 UAC_GATE_TIME;   /* interrupt time, 100 ns */
#else
typedef unsigned long long UAC_GATE_TIME;
#endif
#define UAC_GATE_STATUS_CANCELLED  ((long)0xC0000120L)
#define UAC_GATE_STATUS_IO_TIMEOUT ((long)0xC00000B5L)
/* What the caller of an expired request is given: STATUS_CANCELLED (the
 * cancellation took it) becomes STATUS_IO_TIMEOUT, as before; any other
 * final status is returned as it came. The record keeps the raw one. */
static long UacGateCallerStatus(long raw)
{
    return raw == UAC_GATE_STATUS_CANCELLED ? UAC_GATE_STATUS_IO_TIMEOUT : raw;
}
/* LateMs, expiry to completion: the full-width interval divided first,
 * then saturated at 0xFFFFFFFE, which is not the record's "not completed"
 * (UAC_LATE_NONE). A completion read before the expiry's time is 0. */
#define UAC_LATE_NONE 0xFFFFFFFFul
static unsigned long UacGateLateMs(UAC_GATE_TIME now, UAC_GATE_TIME expiredAt)
{
    UAC_GATE_TIME ms;
    if (now <= expiredAt) return 0;
    ms = (now - expiredAt) / 10000u;
    return ms >= UAC_LATE_NONE ? UAC_LATE_NONE - 1 : (unsigned long)ms;
}

/* ---- The startup latch (29.9). A step's status as the PnP start takes
 * it: once a request of the start has expired (latched), every outcome,
 * a success, a tolerated failure or another failure, is
 * STATUS_IO_TIMEOUT. ---- */
#define UAC_GATE_STATUS_NOT_SUPPORTED ((long)0xC00000BBL)
static long UacStartVerdict(long status, int latched)
{
    return latched ? UAC_GATE_STATUS_IO_TIMEOUT : status;
}
/* uac1's null function after Prepare (no format either direction can
 * run): only a STATUS_NOT_SUPPORTED the latch's verdict left standing. */
static int UacStartNullFunction(long verdict)
{
    return verdict == UAC_GATE_STATUS_NOT_SUPPORTED;
}

/* ---- The notifier's steps (29.9). Life, as the gate keeps it. ---- */
#define UAC_LIFE_STARTING 0
#define UAC_LIFE_STARTED  1
#define UAC_LIFE_TEARDOWN 2
#define UAC_LIFE_SURPRISE 3

typedef struct UAC_NOTIFY {
    int Recorded;       /* Uac2Failed written for this failure */
    int FinalRecorded;  /* written again with the expired request's final status */
    int Invalidated;    /* the device-state call made, or never to be made */
} UAC_NOTIFY;
#define UAC_NOTIFY_WRITE 1u   /* write Uac2Failed now */
#define UAC_NOTIFY_FIRST 2u   /* with WRITE: the first write (the count goes up) */
#define UAC_NOTIFY_ADMIT 4u   /* read Life under the gate lock (UacNotifyLife) */
#define UAC_NOTIFY_EXIT  8u   /* then leave the loop */
/* One wake of the notifier. exitAsked is read before failed and completed:
 * a teardown asks the exit only after the expired request has drained
 * (the PnP caller's own requests are synchronous), so the snapshot read
 * after the exit flag already holds the final status. A pending record,
 * first or final, is written before the exit is honoured: a join never
 * discards it. No device-state call is admitted once the exit is asked. */
static unsigned int UacNotifyStep(UAC_NOTIFY *n, int exitAsked, int failed, int completed)
{
    unsigned int a = 0;
    if (failed) {
        if (!n->Recorded || (completed && !n->FinalRecorded)) {
            a |= UAC_NOTIFY_WRITE;
            if (!n->Recorded) a |= UAC_NOTIFY_FIRST;
            n->Recorded = 1;
            if (completed) n->FinalRecorded = 1;
        }
        if (!exitAsked && !n->Invalidated) a |= UAC_NOTIFY_ADMIT;
    }
    if (exitAsked) a |= UAC_NOTIFY_EXIT;
    return a;
}
/* Life as read under the gate lock: 1 when the call is admitted now (the
 * caller marks it in progress under the same lock); a start still in
 * progress admits nothing and is asked again at the start's wake; a
 * teardown never admits it. */
static int UacNotifyLife(UAC_NOTIFY *n, int life)
{
    if (n->Invalidated || life == UAC_LIFE_STARTING) return 0;
    n->Invalidated = 1;
    return life == UAC_LIFE_STARTED;
}

#endif
